#include "v1_resident_mapper.hpp"

#include <algorithm>
#include <array>
#include <bs/game_definition.hpp>
#include <bs/heads_up.hpp>
#include <string>
#include <unordered_map>
#include <vector>

namespace bs::v1 {
namespace {

namespace pv = ::bigshark::engine::v1;

using poker::Action;
using poker::ActionType;
using poker::Chips;
using poker::GameDef;
using poker::GameState;
using poker::HeadsUpRoot;
using poker::HeadsUpState;
using poker::PublicAction;
using poker::Street;

// proto card: (Rank-1)*4 + (Suit-1) into the poker id space rank*4+suit with
// rank 0..12 (2..A) and suit 0..3 (s,h,d,c).
int pokerCard(const pv::Card& card) {
  return (static_cast<int>(card.rank()) - 1) * 4 + (static_cast<int>(card.suit()) - 1);
}

pv::Street wireStreet(Street street) {
  switch (street) {
    case Street::Preflop:
      return pv::STREET_PREFLOP;
    case Street::Flop:
      return pv::STREET_FLOP;
    case Street::Turn:
      return pv::STREET_TURN;
    case Street::River:
      return pv::STREET_RIVER;
  }
  return pv::STREET_UNSPECIFIED;
}

// Inverse of wireStreet for observed actions. The legacy postflop path skips
// preflop events; the shared hand-start replay additionally consumes them.
Street pokerStreet(pv::Street street) {
  switch (street) {
    case pv::STREET_FLOP:
      return Street::Flop;
    case pv::STREET_TURN:
      return Street::Turn;
    case pv::STREET_RIVER:
      return Street::River;
    default:
      return Street::Preflop;
  }
}

// One validated postflop action in actor space.
struct PlannedAction {
  pv::Street street;
  std::size_t actor;
  pv::ActionType kind;
  Chips target;
  Chips paid;
  Chips potBefore;
  Chips stackAfter;
};

}  // namespace

bool reconstructPostflopImpl(const pv::DecisionRequest& request, ReconstructedPostflop& out,
                             V1BlueprintMiss& miss, bool allow_facing_all_in) {
  const pv::HandState& state = request.state();
  auto fail = [&](V1BlueprintMiss reason) {
    miss = reason;
    out.state.reset();
    out.history.clear();
    out.oracle_state.reset();
    return false;
  };

  // ---- Profile gates: postflop, 2..10 seats, no ante/rake/straddle. --------
  if (state.street() != pv::STREET_FLOP && state.street() != pv::STREET_TURN &&
      state.street() != pv::STREET_RIVER)
    return fail(V1BlueprintMiss::RootNotSupported);
  if (state.players_size() < static_cast<int>(bs::poker::kMinUnifiedSeats) ||
      state.players_size() > static_cast<int>(bs::poker::kMaxUnifiedSeats))
    return fail(V1BlueprintMiss::RootNotSupported);
  const std::size_t seat_count = static_cast<std::size_t>(state.players_size());
  if (state.has_game() && (state.game().ante() != 0 || state.game().button_ante() != 0 ||
                           (state.game().has_straddle() && state.game().straddle().enabled()) ||
                           state.game().has_rake()))
    return fail(V1BlueprintMiss::RootNotSupported);
  for (const pv::ForcedContribution& forced : state.forced_contributions())
    if (forced.type() != pv::FORCED_CONTRIBUTION_TYPE_SMALL_BLIND &&
        forced.type() != pv::FORCED_CONTRIBUTION_TYPE_BIG_BLIND)
      return fail(V1BlueprintMiss::RootNotSupported);
  if (state.pot().side_pots_size() != 0)
    return fail(V1BlueprintMiss::RootNotSupported);

  // ---- Player mapping: poker player index equals occupied seat order (lower
  // seat first) and root.button is the button player's index. This matches the
  // artifact convention: ranges and information keys index the same seat
  // order, and the poker engine opens postflop on the first live seat
  // clockwise of the button.
  std::vector<const pv::PlayerState*> bySeat;
  for (const pv::PlayerState& player : state.players())
    bySeat.push_back(&player);
  std::sort(bySeat.begin(), bySeat.end(), [](const pv::PlayerState* a, const pv::PlayerState* b) {
    return a->seat() < b->seat();
  });
  for (std::size_t i = 1; i < seat_count; ++i)
    if (bySeat[i - 1]->seat() == bySeat[i]->seat())
      return fail(V1BlueprintMiss::RootNotSupported);
  const std::vector<const pv::PlayerState*> actorPlayer = bySeat;
  std::optional<std::size_t> rootButton;
  for (std::size_t i = 0; i < seat_count; ++i)
    if (actorPlayer[i]->seat() == state.button_seat())
      rootButton = i;
  if (!rootButton)
    return fail(V1BlueprintMiss::RootNotSupported);
  std::unordered_map<std::string, std::size_t> actorOf;
  for (std::size_t i = 0; i < seat_count; ++i)
    actorOf.emplace(actorPlayer[i]->player_id(), i);
  {
    const auto found = actorOf.find(state.hero_player_id());
    if (found == actorOf.end())
      return fail(V1BlueprintMiss::RootNotSupported);
    out.hero_actor = found->second;
  }
  // A folded seat carries dead money into a pot the equal-matched root cannot
  // express, so the BLUEPRINT path admits live seats only. The resolver path
  // (allow_facing_all_in=true) admits folded seats: the terminal-only resolver
  // replays the fold through its cursor, and the subgame resolver carries the
  // folded bitmap through the GameDef so the existing fold-aware machinery
  // (next_actor, settlement, trainer) handles them.
  for (std::size_t i = 0; i < seat_count; ++i)
    if (actorPlayer[i]->status() == pv::PLAYER_STATUS_FOLDED && !allow_facing_all_in)
      return fail(V1BlueprintMiss::RootNotSupported);
  // A postflop decision with any player all-in is outside the resident
  // blueprint profile. The resolver path additionally admits the canonical
  // terminal-only case in which a NON-hero seat is all-in and the acting hero
  // still has a fold/call decision; the hero itself may never be all-in at an
  // action node. The BLUEPRINT gate keeps rejecting every all-in shape.
  const bool hero_all_in = actorPlayer[out.hero_actor]->status() == pv::PLAYER_STATUS_ALL_IN;
  bool other_all_in = false;
  for (std::size_t i = 0; i < seat_count; ++i)
    if (i != out.hero_actor && actorPlayer[i]->status() == pv::PLAYER_STATUS_ALL_IN)
      other_all_in = true;
  if (hero_all_in || (other_all_in && !allow_facing_all_in))
    return fail(V1BlueprintMiss::RootNotSupported);

  // ---- Preflop matched-contribution accounting. The root requires EQUAL
  // matched contributions at every seat; an even pot alone does not prove
  // them equal (a 15+25 short-stack all-in makes pot 40 from 15/25). Sum the
  // forced blinds plus the exact voluntary preflop payments and require
  // equality and pot agreement whenever the full preflop ledger is available.
  std::vector<Chips> preflopContrib(seat_count, 0);
  for (const pv::ForcedContribution& forced : state.forced_contributions()) {
    const auto it = actorOf.find(forced.player_id());
    if (it != actorOf.end())
      preflopContrib[it->second] += forced.amount();
  }
  bool preflopVoluntaryComplete = false;
  bool hasPreflopVoluntaryChip = false;
  {
    // Street-paid starts at the posted blinds because a raise-to target counts
    // the blind already in front of the actor.
    std::vector<Chips> preflopStreetPaid = preflopContrib;
    preflopVoluntaryComplete = true;
    for (const pv::ActionEvent& event : state.action_history()) {
      if (event.street() != pv::STREET_PREFLOP)
        continue;
      const auto actorIt = actorOf.find(event.actor_player_id());
      if (actorIt == actorOf.end()) {
        preflopVoluntaryComplete = false;  // unattributed opener
        continue;
      }
      const std::size_t actor = actorIt->second;
      Chips payment = 0;
      bool known = false;
      if (event.action() == pv::ACTION_TYPE_CALL && event.has_incremental_amount()) {
        payment = event.incremental_amount();
        known = true;
        hasPreflopVoluntaryChip = true;
      } else if ((event.action() == pv::ACTION_TYPE_BET ||
                  event.action() == pv::ACTION_TYPE_RAISE) &&
                 event.has_target_total() && event.target_total() >= preflopStreetPaid[actor]) {
        payment = event.target_total() - preflopStreetPaid[actor];
        known = true;
        hasPreflopVoluntaryChip = true;
      }
      if (!known)
        preflopVoluntaryComplete = false;
      preflopStreetPaid[actor] += payment;
      preflopContrib[actor] += payment;
    }
  }
  // ---- Cards. --------------------------------------------------------------
  if (state.board_size() != 3 && state.board_size() != 4 && state.board_size() != 5)
    return fail(V1BlueprintMiss::OffTree);
  std::array<int, 3> flop{};
  for (std::size_t i = 0; i < 3; ++i)
    flop[i] = pokerCard(state.board(static_cast<int>(i)));
  std::array<int, 2> heroCards{pokerCard(state.hero_hole_cards(0)),
                               pokerCard(state.hero_hole_cards(1))};
  std::sort(heroCards.begin(), heroCards.end());
  if (heroCards[0] < 0 || heroCards[1] >= 52 || heroCards[0] == heroCards[1])
    return fail(V1BlueprintMiss::OffTree);
  for (const pv::Card& board_card : state.board()) {
    const int id = pokerCard(board_card);
    if (id < 0 || id >= 52 || id == heroCards[0] || id == heroCards[1])
      return fail(V1BlueprintMiss::OffTree);
  }

  // ---- Ledger pass: derive the exact payment of every voluntary postflop
  // event from its absolute target total. Preflop events only establish the
  // root pot/contributions and are never replayed. incremental_amount is
  // advisory (platforms report either the total payment or the raise-only
  // increment); the authoritative replay quantity is the exact target total.
  std::vector<PlannedAction> planned;
  std::vector<Chips> paidPostflop(seat_count, 0);
  std::vector<Chips> streetPaid(seat_count, 0);
  pv::Street ledgerStreet = pv::STREET_UNSPECIFIED;
  Chips firstEventPotBefore = 0;

  for (const pv::ActionEvent& event : state.action_history()) {
    if (event.street() == pv::STREET_PREFLOP)
      continue;
    if (ledgerStreet == pv::STREET_UNSPECIFIED) {
      ledgerStreet = event.street();
      firstEventPotBefore = event.pot_before();
    } else if (event.street() != ledgerStreet) {
      if (static_cast<int>(event.street()) < static_cast<int>(ledgerStreet))
        return fail(V1BlueprintMiss::OffTree);
      std::fill(streetPaid.begin(), streetPaid.end(), Chips{0});
      ledgerStreet = event.street();
    }

    const auto actorIt = actorOf.find(event.actor_player_id());
    if (actorIt == actorOf.end())
      return fail(V1BlueprintMiss::OffTree);
    const std::size_t actor = actorIt->second;

    Chips paid = 0;
    Chips target = 0;
    switch (event.action()) {
      case pv::ACTION_TYPE_FOLD:
      case pv::ACTION_TYPE_CHECK:
        if (event.has_target_total() || event.has_incremental_amount())
          return fail(V1BlueprintMiss::OffTree);
        break;
      case pv::ACTION_TYPE_CALL:
        if (event.has_target_total() || !event.has_incremental_amount() ||
            event.incremental_amount() == 0)
          return fail(V1BlueprintMiss::OffTree);
        paid = event.incremental_amount();
        break;
      case pv::ACTION_TYPE_BET:
        // A bet opens voluntary commitment on the street: no seat may have
        // posted postflop chips on this street yet.
        if (!event.has_target_total() || std::any_of(streetPaid.begin(), streetPaid.end(),
                                                     [](Chips posted) { return posted != 0; }))
          return fail(V1BlueprintMiss::OffTree);
        target = event.target_total();
        paid = target;
        break;
      case pv::ACTION_TYPE_RAISE: {
        if (!event.has_target_total() || event.target_total() <= streetPaid[actor])
          return fail(V1BlueprintMiss::OffTree);
        target = event.target_total();
        paid = target - streetPaid[actor];
        break;
      }
      default:
        return fail(V1BlueprintMiss::OffTree);
    }
    planned.push_back({event.street(), actor, event.action(), target, paid, event.pot_before(),
                       event.stack_after()});
    paidPostflop[actor] += paid;
    streetPaid[actor] += paid;
  }

  // Flop-start pot: the first postflop event's pot_before when the platform
  // supplies it. Production River snapshots omit it (serialized zero), so it
  // is reconstructed as the current pot minus every postflop payment.
  Chips flopPot = 0;
  if (firstEventPotBefore != 0) {
    flopPot = firstEventPotBefore;
  } else {
    Chips paidTotal = 0;
    for (Chips paid : paidPostflop)
      paidTotal += paid;
    if (paidTotal > state.pot().pot_total())
      return fail(V1BlueprintMiss::OffTree);
    flopPot = state.pot().pot_total() - paidTotal;
  }

  const Chips bigBlind = state.game().big_blind();
  if (flopPot == 0 || flopPot % static_cast<Chips>(seat_count) != 0)
    return fail(V1BlueprintMiss::RootNotSupported);
  const Chips matched = flopPot / static_cast<Chips>(seat_count);
  if (matched < bigBlind)
    return fail(V1BlueprintMiss::RootNotSupported);

  // Explicit matched contributions. When the complete voluntary preflop
  // ledger is attributed (every chip move carries its amount), every seat's
  // reconstructed total must be equal AND the totals must reconcile exactly
  // with the flop pot: this rejects even-pot asymmetric shapes such as
  // 15+25=40 that the flopPot%n parity check alone cannot catch. Blinds alone
  // with no attributed voluntary ledger leave the standard even-pot matched
  // assumption (an unmatched completion cannot be distinguished without the
  // event, and every seat is known non-all-in via the status gate).
  if (preflopVoluntaryComplete && hasPreflopVoluntaryChip) {
    for (std::size_t i = 1; i < seat_count; ++i)
      if (preflopContrib[i] != preflopContrib[0])
        return fail(V1BlueprintMiss::RootNotSupported);
    Chips contribTotal = 0;
    for (Chips contrib : preflopContrib)
      contribTotal += contrib;
    if (contribTotal != flopPot)
      return fail(V1BlueprintMiss::RootNotSupported);
  }

  std::vector<Chips> flopStacks(seat_count, 0);
  for (std::size_t actor = 0; actor < seat_count; ++actor) {
    flopStacks[actor] = actorPlayer[actor]->stack() + paidPostflop[actor];
    if (flopStacks[actor] == 0)
      return fail(V1BlueprintMiss::RootNotSupported);
  }

  GameDef rootDef{};
  rootDef.player_count = seat_count;
  rootDef.button = *rootButton;
  rootDef.big_blind = bigBlind;
  for (std::size_t actor = 0; actor < seat_count; ++actor) {
    rootDef.stacks[actor] = flopStacks[actor];
    rootDef.contributions[actor] = matched;
  }
  rootDef.pot = flopPot;
  rootDef.board = {flop[0], flop[1], flop[2], 0, 0};
  rootDef.board_size = 3;

  // ---- Exact engine replay. Cards are inserted at each dealing boundary and
  // voluntary events are applied by kind and exact target total. Optional
  // pot_before/stack_after ledger fields are cross-checked when present. At
  // two seats the shipped HeadsUpState cursor is replayed in lockstep as the
  // differential oracle (W2c-ii-a gate 1).
  GameState cursor(rootDef);
  std::optional<HeadsUpState> oracle;
  if (seat_count == 2) {
    HeadsUpRoot oracleRoot;
    oracleRoot.flop = flop;
    oracleRoot.stacks = {flopStacks[0], flopStacks[1]};
    oracleRoot.contributions = {matched, matched};
    oracleRoot.pot = flopPot;
    oracleRoot.big_blind = bigBlind;
    oracleRoot.button = *rootButton;
    oracle.emplace(oracleRoot);
  }
  try {
    pv::Street replayedStreet = pv::STREET_FLOP;
    for (const PlannedAction& planned_action : planned) {
      if (static_cast<int>(planned_action.street) < static_cast<int>(replayedStreet))
        return fail(V1BlueprintMiss::OffTree);
      while (static_cast<int>(replayedStreet) < static_cast<int>(planned_action.street)) {
        if (cursor.phase() != bs::poker::Phase::Deal)
          return fail(V1BlueprintMiss::OffTree);
        const std::size_t nextBoard = cursor.board().size();
        if (nextBoard >= static_cast<std::size_t>(state.board_size()))
          return fail(V1BlueprintMiss::OffTree);
        const int card = pokerCard(state.board(static_cast<int>(nextBoard)));
        cursor = cursor.after_card(card);
        if (oracle)
          *oracle = oracle->after_card(card);
        replayedStreet = wireStreet(cursor.street());
      }
      if (cursor.phase() != bs::poker::Phase::Action || !cursor.actor().has_value() ||
          *cursor.actor() != planned_action.actor)
        return fail(V1BlueprintMiss::OffTree);
      if (planned_action.potBefore != 0 && cursor.pot() != planned_action.potBefore)
        return fail(V1BlueprintMiss::OffTree);

      Action action{};
      switch (planned_action.kind) {
        case pv::ACTION_TYPE_FOLD:
          action = {ActionType::Fold};
          break;
        case pv::ACTION_TYPE_CHECK:
          action = {ActionType::Check};
          break;
        case pv::ACTION_TYPE_CALL:
          action = {ActionType::Call};
          break;
        case pv::ACTION_TYPE_BET:
          action = {ActionType::Bet, planned_action.target};
          break;
        case pv::ACTION_TYPE_RAISE:
          action = {ActionType::Raise, planned_action.target};
          break;
        default:
          return fail(V1BlueprintMiss::OffTree);
      }
      cursor = cursor.after_action(planned_action.actor, action);
      if (oracle)
        *oracle = oracle->after_action(planned_action.actor, action);
      if (planned_action.stackAfter != 0 &&
          cursor.players()[planned_action.actor].stack != planned_action.stackAfter)
        return fail(V1BlueprintMiss::OffTree);
      out.history.push_back(
          PublicAction{pokerStreet(planned_action.street), planned_action.actor, action});
    }

    // Advance to the current decision street.
    while (static_cast<int>(wireStreet(cursor.street())) < static_cast<int>(state.street())) {
      if (cursor.phase() != bs::poker::Phase::Deal)
        return fail(V1BlueprintMiss::OffTree);
      const std::size_t nextBoard = cursor.board().size();
      if (nextBoard >= static_cast<std::size_t>(state.board_size()))
        return fail(V1BlueprintMiss::OffTree);
      const int card = pokerCard(state.board(static_cast<int>(nextBoard)));
      cursor = cursor.after_card(card);
      if (oracle)
        *oracle = oracle->after_card(card);
    }
  } catch (const std::exception&) {
    // An illegal transition or card collision against the exact poker rules is
    // a deterministic coverage miss: forced BLUEPRINT becomes
    // UNSUPPORTED_FEATURE, AUTOMATIC falls back to the heuristic.
    return fail(V1BlueprintMiss::OffTree);
  }

  // ---- Decision-node invariants against the structured snapshot. ----------
  if (wireStreet(cursor.street()) != state.street())
    return fail(V1BlueprintMiss::OffTree);
  if (cursor.board().size() != static_cast<std::size_t>(state.board_size()))
    return fail(V1BlueprintMiss::OffTree);
  if (!cursor.actor().has_value() || *cursor.actor() != out.hero_actor)
    return fail(V1BlueprintMiss::OffTree);
  if (cursor.pot() != state.pot().pot_total())
    return fail(V1BlueprintMiss::OffTree);
  for (std::size_t actor = 0; actor < seat_count; ++actor) {
    const auto& chips = cursor.players()[actor];
    const pv::PlayerState& player = *actorPlayer[actor];
    if (chips.stack != player.stack() || chips.street_committed != player.street_committed())
      return fail(V1BlueprintMiss::OffTree);
  }
  const Chips due = [&] {
    Chips high = 0;
    for (std::size_t actor = 0; actor < seat_count; ++actor)
      high = std::max(high, cursor.players()[actor].street_committed);
    return high - cursor.players()[*cursor.actor()].street_committed;
  }();
  if (due != state.to_call())
    return fail(V1BlueprintMiss::OffTree);

  out.state = std::move(cursor);
  out.oracle_state = std::move(oracle);
  out.hero_cards = heroCards;
  miss = V1BlueprintMiss::None;
  return true;
}

// ---- RFC 0007 / 0019 / 0021 hand-start reconstruction ---------------------
//
// Minor 1 retains the heads-up profile. Minor 2 reconstructs every dealt
// preflop seat, and supplied multiway hand history can also reach a true flop
// root and subsequent streets through the same exact replay.
// Reconstruction alone does not establish policy coverage or GTO quality.

namespace {

bool reconstructHandStartImpl(const pv::DecisionRequest& request, ReconstructedPostflop& out,
                              V1BlueprintMiss& miss, std::uint32_t negotiated_minor, bool postflop,
                              bool allow_facing_all_in) {
  const pv::HandState& state = request.state();
  auto fail = [&](V1BlueprintMiss reason) {
    miss = reason;
    out.state.reset();
    out.history.clear();
    out.oracle_state.reset();
    return false;
  };

  // ---- Profile gates: legacy HU or minor-2 N-seat, no extended utility. ---
  if ((!postflop && state.street() != pv::STREET_PREFLOP) ||
      (postflop && state.street() != pv::STREET_FLOP && state.street() != pv::STREET_TURN &&
       state.street() != pv::STREET_RIVER))
    return fail(V1BlueprintMiss::RootNotSupported);
  if (state.players_size() < static_cast<int>(poker::kMinUnifiedSeats) ||
      state.players_size() > static_cast<int>(poker::kMaxUnifiedSeats) ||
      (negotiated_minor < 2 && state.players_size() != 2))
    return fail(V1BlueprintMiss::RootNotSupported);
  const std::size_t seat_count = static_cast<std::size_t>(state.players_size());
  if (state.has_game() && (state.game().ante() != 0 || state.game().button_ante() != 0 ||
                           (state.game().has_straddle() && state.game().straddle().enabled()) ||
                           state.game().has_rake()))
    return fail(V1BlueprintMiss::RootNotSupported);
  for (const pv::ForcedContribution& forced : state.forced_contributions())
    if (forced.type() != pv::FORCED_CONTRIBUTION_TYPE_SMALL_BLIND &&
        forced.type() != pv::FORCED_CONTRIBUTION_TYPE_BIG_BLIND)
      return fail(V1BlueprintMiss::RootNotSupported);
  if (state.pot().side_pots_size() != 0)
    return fail(V1BlueprintMiss::RootNotSupported);

  // ---- Player mapping (identical to the postflop path). -------------------
  std::vector<const pv::PlayerState*> bySeat;
  for (const pv::PlayerState& player : state.players())
    bySeat.push_back(&player);
  std::sort(bySeat.begin(), bySeat.end(), [](const pv::PlayerState* a, const pv::PlayerState* b) {
    return a->seat() < b->seat();
  });
  for (std::size_t i = 1; i < seat_count; ++i)
    if (bySeat[i - 1]->seat() == bySeat[i]->seat())
      return fail(V1BlueprintMiss::RootNotSupported);
  const std::vector<const pv::PlayerState*> actorPlayer = bySeat;
  std::optional<std::size_t> rootButton;
  for (std::size_t i = 0; i < seat_count; ++i)
    if (actorPlayer[i]->seat() == state.button_seat())
      rootButton = i;
  if (!rootButton)
    return fail(V1BlueprintMiss::RootNotSupported);
  std::unordered_map<std::string, std::size_t> actorOf;
  for (std::size_t i = 0; i < seat_count; ++i)
    if (!actorOf.emplace(actorPlayer[i]->player_id(), i).second)
      return fail(V1BlueprintMiss::RootNotSupported);
  {
    const auto found = actorOf.find(state.hero_player_id());
    if (found == actorOf.end())
      return fail(V1BlueprintMiss::RootNotSupported);
    out.hero_actor = found->second;
  }
  for (std::size_t i = 0; i < seat_count; ++i)
    if (actorPlayer[i]->status() == pv::PLAYER_STATUS_FOLDED && seat_count == 2)
      return fail(V1BlueprintMiss::RootNotSupported);
  const bool hero_all_in = actorPlayer[out.hero_actor]->status() == pv::PLAYER_STATUS_ALL_IN;
  if (hero_all_in || actorPlayer[out.hero_actor]->status() == pv::PLAYER_STATUS_FOLDED)
    return fail(V1BlueprintMiss::RootNotSupported);
  if (postflop && !allow_facing_all_in)
    for (const auto* player : actorPlayer)
      if (player->status() == pv::PLAYER_STATUS_ALL_IN)
        return fail(V1BlueprintMiss::RootNotSupported);

  // ---- Cards: hero hole cards and the observed public board, if present. ---
  if (!postflop && state.board_size() != 0)
    return fail(V1BlueprintMiss::OffTree);
  if (seat_count > 2) {
    if (state.hero_hole_cards_size() != 2)
      return fail(V1BlueprintMiss::OffTree);
    for (const pv::Card& card : state.hero_hole_cards())
      if (static_cast<int>(card.rank()) < 1 || static_cast<int>(card.rank()) > 13 ||
          static_cast<int>(card.suit()) < 1 || static_cast<int>(card.suit()) > 4)
        return fail(V1BlueprintMiss::OffTree);
  }
  std::array<int, 2> heroCards{pokerCard(state.hero_hole_cards(0)),
                               pokerCard(state.hero_hole_cards(1))};
  std::sort(heroCards.begin(), heroCards.end());
  if (heroCards[0] < 0 || heroCards[1] >= 52 || heroCards[0] == heroCards[1])
    return fail(V1BlueprintMiss::OffTree);
  if (postflop) {
    const int expected_board = state.street() == pv::STREET_FLOP   ? 3
                               : state.street() == pv::STREET_TURN ? 4
                                                                   : 5;
    if (state.board_size() != expected_board)
      return fail(V1BlueprintMiss::OffTree);
    std::array<bool, 52> used{};
    used[heroCards[0]] = used[heroCards[1]] = true;
    for (const pv::Card& card : state.board()) {
      if (static_cast<int>(card.rank()) < 1 || static_cast<int>(card.rank()) > 13 ||
          static_cast<int>(card.suit()) < 1 || static_cast<int>(card.suit()) > 4)
        return fail(V1BlueprintMiss::OffTree);
      const int id = pokerCard(card);
      if (used[id])
        return fail(V1BlueprintMiss::OffTree);
      used[id] = true;
    }
  }

  const auto add_payment = [&](Chips& total, Chips payment) {
    if (payment > poker::kMaxHeadsUpChips || total > poker::kMaxHeadsUpChips - payment)
      return false;
    total += payment;
    return true;
  };

  // ---- Blinds from forced contributions. -----------------------------------
  const Chips bigBlind = state.game().big_blind();
  if (seat_count > 2 && state.game().small_blind() != bigBlind / 2)
    return fail(V1BlueprintMiss::RootNotSupported);
  std::array<Chips, poker::kMaxUnifiedSeats> blindsPosted{};
  for (const pv::ForcedContribution& forced : state.forced_contributions()) {
    const auto it = actorOf.find(forced.player_id());
    if (it == actorOf.end())
      return fail(V1BlueprintMiss::RootNotSupported);
    if (!add_payment(blindsPosted[it->second], forced.amount()))
      return fail(V1BlueprintMiss::RootNotSupported);
  }
  // Exactly one SB and one BB. Their native positions are checked below for
  // the newly admitted multiway profile, including the declared post type.
  int sb_count = 0, bb_count = 0;
  for (const pv::ForcedContribution& forced : state.forced_contributions()) {
    if (forced.type() == pv::FORCED_CONTRIBUTION_TYPE_SMALL_BLIND)
      ++sb_count;
    else if (forced.type() == pv::FORCED_CONTRIBUTION_TYPE_BIG_BLIND)
      ++bb_count;
  }
  if (sb_count != 1 || bb_count != 1)
    return fail(V1BlueprintMiss::RootNotSupported);
  Chips blindsPot = 0;
  for (std::size_t actor = 0; actor < seat_count; ++actor)
    if (!add_payment(blindsPot, blindsPosted[actor]))
      return fail(V1BlueprintMiss::RootNotSupported);

  // ---- Supplied payments: gross per seat and current-street targets. -------
  auto grossPaid = blindsPosted;
  auto streetPaid = blindsPosted;
  std::vector<PlannedAction> planned;
  pv::Street ledger_street = pv::STREET_PREFLOP;
  for (const pv::ActionEvent& event : state.action_history()) {
    if ((!postflop && event.street() != pv::STREET_PREFLOP) || event.street() < ledger_street ||
        event.street() > state.street())
      return fail(V1BlueprintMiss::OffTree);
    if (event.street() != ledger_street) {
      streetPaid.fill(0);
      ledger_street = event.street();
    }
    const auto actorIt = actorOf.find(event.actor_player_id());
    if (actorIt == actorOf.end())
      return fail(V1BlueprintMiss::OffTree);
    const std::size_t actor = actorIt->second;

    Chips paid = 0;
    Chips target = 0;
    switch (event.action()) {
      case pv::ACTION_TYPE_FOLD:
      case pv::ACTION_TYPE_CHECK:
        if (event.has_target_total() || event.has_incremental_amount())
          return fail(V1BlueprintMiss::OffTree);
        break;
      case pv::ACTION_TYPE_CALL:
        if (event.has_target_total() || !event.has_incremental_amount() ||
            event.incremental_amount() == 0)
          return fail(V1BlueprintMiss::OffTree);
        paid = event.incremental_amount();
        break;
      case pv::ACTION_TYPE_BET:
        if (!event.has_target_total() || streetPaid[actor] != 0)
          return fail(V1BlueprintMiss::OffTree);
        target = event.target_total();
        paid = target;
        break;
      case pv::ACTION_TYPE_RAISE:
        if (!event.has_target_total() || event.target_total() <= streetPaid[actor])
          return fail(V1BlueprintMiss::OffTree);
        target = event.target_total();
        paid = target - streetPaid[actor];
        break;
      default:
        return fail(V1BlueprintMiss::OffTree);
    }
    planned.push_back({event.street(), actor, event.action(), target, paid, event.pot_before(),
                       event.stack_after()});
    if (!add_payment(grossPaid[actor], paid) || !add_payment(streetPaid[actor], paid))
      return fail(V1BlueprintMiss::OffTree);
  }

  // ---- Starting stacks: current + gross, valid only without refunds. -------
  std::array<Chips, poker::kMaxUnifiedSeats> startingStacks{};
  for (std::size_t actor = 0; actor < seat_count; ++actor) {
    startingStacks[actor] = actorPlayer[actor]->stack();
    if (!add_payment(startingStacks[actor], grossPaid[actor]))
      return fail(V1BlueprintMiss::RootNotSupported);
    if (startingStacks[actor] == 0)
      return fail(V1BlueprintMiss::RootNotSupported);
  }

  // ---- Build the preflop GameDef. -------------------------------------------
  GameDef rootDef{};
  rootDef.player_count = seat_count;
  rootDef.button = static_cast<int>(*rootButton);
  rootDef.big_blind = bigBlind;
  rootDef.stacks = startingStacks;
  rootDef.contributions = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
  rootDef.pot = blindsPot;
  rootDef.board = {-1, -1, -1, 0, 0};
  rootDef.board_size = 0;
  rootDef.preflop = true;
  rootDef.blinds_posted = blindsPosted;
  rootDef.terminal = postflop ? poker::TerminalDepth::River : poker::TerminalDepth::Flop;

  if (seat_count > 2) {
    for (const pv::ForcedContribution& forced : state.forced_contributions()) {
      const std::size_t expected = forced.type() == pv::FORCED_CONTRIBUTION_TYPE_SMALL_BLIND
                                       ? poker::small_blind_seat(rootDef)
                                       : poker::big_blind_seat(rootDef);
      if (actorOf.at(forced.player_id()) != expected)
        return fail(V1BlueprintMiss::RootNotSupported);
    }
  }

  // ---- Exact engine replay. --------------------------------------------------
  bool root_constructed = false;
  try {
    GameState cursor(rootDef);
    root_constructed = true;
    bool rebased = false;
    const auto no_refunds = [&] {
      for (std::size_t actor = 0; actor < seat_count; ++actor)
        if (cursor.players()[actor].refunded != 0)
          return false;
      return true;
    };
    const auto advance_to = [&](pv::Street target) {
      const std::size_t target_board = target == pv::STREET_PREFLOP ? 0
                                       : target == pv::STREET_FLOP  ? 3
                                       : target == pv::STREET_TURN  ? 4
                                                                    : 5;
      while (cursor.board().size() < target_board) {
        if (!postflop || cursor.phase() != poker::Phase::Deal ||
            cursor.board().size() >= static_cast<std::size_t>(state.board_size()))
          return false;
        cursor = cursor.after_card(pokerCard(state.board(static_cast<int>(cursor.board().size()))));
        if (!no_refunds())
          return false;
        // Native street changes after the FIRST flop card. Rebase only after
        // all three cards, before any flop action, preserving the real button.
        if (!rebased && cursor.board().size() == 3) {
          GameDef flop{};
          flop.player_count = seat_count;
          flop.button = rootDef.button;
          flop.big_blind = bigBlind;
          flop.pot = cursor.pot();
          flop.board_size = 3;
          std::copy(cursor.board().begin(), cursor.board().end(), flop.board.begin());
          for (std::size_t actor = 0; actor < seat_count; ++actor) {
            flop.stacks[actor] = cursor.players()[actor].stack;
            flop.contributions[actor] = cursor.players()[actor].net_contributed();
            flop.folded[actor] = cursor.players()[actor].folded;
          }
          cursor = GameState(flop);
          out.history.clear();
          rebased = true;
        }
      }
      return wireStreet(cursor.street()) == target;
    };
    for (const PlannedAction& planned_action : planned) {
      if (postflop && !advance_to(planned_action.street))
        return fail(V1BlueprintMiss::OffTree);
      if (cursor.phase() != bs::poker::Phase::Action || !cursor.actor().has_value() ||
          *cursor.actor() != planned_action.actor)
        return fail(V1BlueprintMiss::OffTree);
      if (planned_action.potBefore != 0 && cursor.pot() != planned_action.potBefore)
        return fail(V1BlueprintMiss::OffTree);

      Action action{};
      switch (planned_action.kind) {
        case pv::ACTION_TYPE_FOLD:
          action = {ActionType::Fold};
          break;
        case pv::ACTION_TYPE_CHECK:
          action = {ActionType::Check};
          break;
        case pv::ACTION_TYPE_CALL:
          action = {ActionType::Call};
          break;
        case pv::ACTION_TYPE_BET:
        case pv::ACTION_TYPE_RAISE: {
          // The engine distinguishes Bet (opening the street, due == 0) from
          // Raise (facing an existing bet). The v1 protocol uses RAISE for
          // both (e.g. the BB's "raise" after a limp is a Bet in engine
          // terms because the SB's call brought the street commitment even
          // with the BB's blind).
          Chips high = 0;
          for (std::size_t a = 0; a < seat_count; ++a)
            high = std::max(high, cursor.players()[a].street_committed);
          const Chips due = high - cursor.players()[planned_action.actor].street_committed;
          // Preflop RAISE also names the BB option. Postflop BET and RAISE
          // are distinct observations and must not be silently reinterpreted.
          if (postflop && planned_action.street != pv::STREET_PREFLOP &&
              ((planned_action.kind == pv::ACTION_TYPE_BET && due != 0) ||
               (planned_action.kind == pv::ACTION_TYPE_RAISE && due == 0)))
            return fail(V1BlueprintMiss::OffTree);
          action = {due == 0 ? ActionType::Bet : ActionType::Raise, planned_action.target};
          break;
        }
        default:
          return fail(V1BlueprintMiss::OffTree);
      }
      const Chips contributed_before = cursor.players()[planned_action.actor].contributed;
      cursor = cursor.after_action(planned_action.actor, action);
      if (postflop && !no_refunds())
        return fail(V1BlueprintMiss::RootNotSupported);
      if (seat_count > 2 &&
          cursor.players()[planned_action.actor].contributed - contributed_before !=
              planned_action.paid)
        return fail(V1BlueprintMiss::OffTree);
      if (planned_action.stackAfter != 0 &&
          cursor.players()[planned_action.actor].stack != planned_action.stackAfter)
        return fail(V1BlueprintMiss::OffTree);
      out.history.push_back(
          PublicAction{pokerStreet(planned_action.street), planned_action.actor, action});
    }
    if (postflop && (!advance_to(state.street()) || !rebased || !no_refunds()))
      return fail(V1BlueprintMiss::OffTree);
    // ---- Decision-node invariants against the structured snapshot. -----------
    if (wireStreet(cursor.street()) != state.street())
      return fail(V1BlueprintMiss::OffTree);
    if (!cursor.actor().has_value() || *cursor.actor() != out.hero_actor)
      return fail(V1BlueprintMiss::OffTree);
    if (cursor.pot() != state.pot().pot_total())
      return fail(V1BlueprintMiss::OffTree);
    for (std::size_t actor = 0; actor < seat_count; ++actor) {
      const auto& chips = cursor.players()[actor];
      const pv::PlayerState& player = *actorPlayer[actor];
      if (chips.stack != player.stack() || chips.street_committed != player.street_committed())
        return fail(V1BlueprintMiss::OffTree);
      if (seat_count > 2) {
        const auto expected = chips.folded   ? pv::PLAYER_STATUS_FOLDED
                              : chips.all_in ? pv::PLAYER_STATUS_ALL_IN
                                             : pv::PLAYER_STATUS_ACTIVE;
        if (player.status() != expected)
          return fail(V1BlueprintMiss::OffTree);
      }
    }
    const Chips due = postflop ? cursor.legal().call_amount : [&] {
      Chips high = 0;
      for (std::size_t actor = 0; actor < seat_count; ++actor)
        high = std::max(high, cursor.players()[actor].street_committed);
      return high - cursor.players()[*cursor.actor()].street_committed;
    }();
    if (due != state.to_call())
      return fail(V1BlueprintMiss::OffTree);

    out.state = std::move(cursor);
    if (postflop)
      out.oracle_state.reset();
    out.hero_cards = heroCards;
    miss = V1BlueprintMiss::None;
    return true;
  } catch (const std::exception&) {
    // Preserve legacy HU constructor failure behavior; newly admitted roots
    // must fail closed before any lookup, including constructor validation.
    if (!root_constructed && seat_count == 2)
      throw;
    return fail(root_constructed ? V1BlueprintMiss::OffTree : V1BlueprintMiss::RootNotSupported);
  }
}

bool hasPreflopHistory(const pv::DecisionRequest& request, std::uint32_t negotiated_minor) {
  return negotiated_minor >= 2 && request.state().players_size() > 2 &&
         std::any_of(
             request.state().action_history().begin(), request.state().action_history().end(),
             [](const pv::ActionEvent& event) { return event.street() == pv::STREET_PREFLOP; });
}

}  // namespace

bool reconstructPreflop(const pv::DecisionRequest& request, ReconstructedPostflop& out,
                        V1BlueprintMiss& miss, std::uint32_t negotiated_minor) {
  return reconstructHandStartImpl(request, out, miss, negotiated_minor, false, false);
}

bool reconstructPostflop(const pv::DecisionRequest& request, ReconstructedPostflop& out,
                         V1BlueprintMiss& miss, std::uint32_t negotiated_minor) {
  if (hasPreflopHistory(request, negotiated_minor))
    return reconstructHandStartImpl(request, out, miss, negotiated_minor, true, false);
  return reconstructPostflopImpl(request, out, miss, /*allow_facing_all_in=*/false);
}

bool reconstructPostflopForResolve(const pv::DecisionRequest& request, ReconstructedPostflop& out,
                                   V1BlueprintMiss& miss, std::uint32_t negotiated_minor) {
  if (hasPreflopHistory(request, negotiated_minor))
    return reconstructHandStartImpl(request, out, miss, negotiated_minor, true, true);
  return reconstructPostflopImpl(request, out, miss, /*allow_facing_all_in=*/true);
}

}  // namespace bs::v1
