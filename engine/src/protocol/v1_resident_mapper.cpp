#include "v1_resident_mapper.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace bs::v1 {
namespace {

namespace pv = ::bigshark::engine::v1;

using poker::Action;
using poker::ActionType;
using poker::Chips;
using poker::HeadsUpRoot;
using poker::HeadsUpState;

// proto card: (Rank-1)*4 + (Suit-1) into the poker id space rank*4+suit with
// rank 0..12 (2..A) and suit 0..3 (s,h,d,c).
int pokerCard(const pv::Card& card) {
  return (static_cast<int>(card.rank()) - 1) * 4 + (static_cast<int>(card.suit()) - 1);
}

pv::Street wireStreet(poker::Street street) {
  switch (street) {
    case poker::Street::Flop:
      return pv::STREET_FLOP;
    case poker::Street::Turn:
      return pv::STREET_TURN;
    case poker::Street::River:
      return pv::STREET_RIVER;
  }
  return pv::STREET_UNSPECIFIED;
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

bool reconstructPostflop(const pv::DecisionRequest& request, ReconstructedPostflop& out,
                         V1BlueprintMiss& miss) {
  const pv::HandState& state = request.state();
  auto fail = [&](V1BlueprintMiss reason) {
    miss = reason;
    out.state.reset();
    return false;
  };

  // ---- Profile gates: postflop, heads-up, no ante/rake/straddle profile. ---
  if (state.street() != pv::STREET_FLOP && state.street() != pv::STREET_TURN &&
      state.street() != pv::STREET_RIVER)
    return fail(V1BlueprintMiss::RootNotSupported);
  if (state.players_size() != 2)
    return fail(V1BlueprintMiss::RootNotSupported);
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
  // artifact convention: ranges and information keys index the same 0/1
  // players, and the poker engine opens postflop on 1-button.
  std::vector<const pv::PlayerState*> bySeat;
  for (const pv::PlayerState& player : state.players())
    bySeat.push_back(&player);
  std::sort(bySeat.begin(), bySeat.end(), [](const pv::PlayerState* a, const pv::PlayerState* b) {
    return a->seat() < b->seat();
  });
  if (bySeat[0]->seat() == bySeat[1]->seat())
    return fail(V1BlueprintMiss::RootNotSupported);
  std::array<const pv::PlayerState*, 2> actorPlayer{bySeat[0], bySeat[1]};
  std::size_t rootButton = 0;
  if (bySeat[0]->seat() == state.button_seat())
    rootButton = 0;
  else if (bySeat[1]->seat() == state.button_seat())
    rootButton = 1;
  else
    return fail(V1BlueprintMiss::RootNotSupported);
  std::unordered_map<std::string, std::size_t> actorOf;
  actorOf.emplace(actorPlayer[0]->player_id(), 0);
  actorOf.emplace(actorPlayer[1]->player_id(), 1);
  {
    const auto found = actorOf.find(state.hero_player_id());
    if (found == actorOf.end())
      return fail(V1BlueprintMiss::RootNotSupported);
    out.hero_actor = found->second;
  }
  if (actorPlayer[0]->status() == pv::PLAYER_STATUS_FOLDED ||
      actorPlayer[1]->status() == pv::PLAYER_STATUS_FOLDED)
    return fail(V1BlueprintMiss::RootNotSupported);
  // A postflop decision with any player all-in is outside the resident
  // profile: unmatched/all-in pots and runout-only nodes do not fit the
  // equal-matched postflop root the blueprints are trained on.
  if (actorPlayer[0]->status() == pv::PLAYER_STATUS_ALL_IN ||
      actorPlayer[1]->status() == pv::PLAYER_STATUS_ALL_IN)
    return fail(V1BlueprintMiss::RootNotSupported);

  // ---- Preflop matched-contribution accounting. The root requires EQUAL
  // matched contributions; an even pot alone does not prove them equal (a
  // 15+25 short-stack all-in makes pot 40 from 15/25). Sum the forced
  // blinds plus the exact voluntary preflop payments and require equality and
  // pot agreement whenever the full preflop ledger is available.
  std::array<Chips, 2> preflopContrib{};
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
    std::array<Chips, 2> preflopStreetPaid = preflopContrib;
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
  std::array<Chips, 2> paidPostflop{};
  std::array<Chips, 2> streetPaid{};
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
      streetPaid = {0, 0};
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
        // A bet opens voluntary commitment on the street: neither player may
        // have posted postflop chips on this street yet.
        if (!event.has_target_total() || streetPaid[0] != 0 || streetPaid[1] != 0)
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
    const Chips paidTotal = paidPostflop[0] + paidPostflop[1];
    if (paidTotal > state.pot().pot_total())
      return fail(V1BlueprintMiss::OffTree);
    flopPot = state.pot().pot_total() - paidTotal;
  }

  const Chips bigBlind = state.game().big_blind();
  if (flopPot == 0 || flopPot % 2 != 0)
    return fail(V1BlueprintMiss::RootNotSupported);
  const Chips matched = flopPot / 2;
  if (matched < bigBlind)
    return fail(V1BlueprintMiss::RootNotSupported);

  // Explicit matched contributions. When the complete voluntary preflop
  // ledger is attributed (every chip move carries its amount), the two
  // reconstructed totals must be equal AND reconcile exactly with the flop
  // pot: this rejects even-pot asymmetric shapes such as 15+25=40 that the
  // flopPot%2 parity check alone cannot catch. Blinds alone with no
  // attributed voluntary ledger leave the standard even-pot matched
  // assumption (an unmatched SB completion cannot be distinguished without
  // the event, and both players are known non-all-in via the status gate).
  if (preflopVoluntaryComplete && hasPreflopVoluntaryChip) {
    if (preflopContrib[0] != preflopContrib[1] || preflopContrib[0] + preflopContrib[1] != flopPot)
      return fail(V1BlueprintMiss::RootNotSupported);
  }

  std::array<Chips, 2> flopStacks{};
  for (std::size_t actor = 0; actor < 2; ++actor) {
    flopStacks[actor] = actorPlayer[actor]->stack() + paidPostflop[actor];
    if (flopStacks[actor] == 0)
      return fail(V1BlueprintMiss::RootNotSupported);
  }

  HeadsUpRoot root;
  root.flop = flop;
  root.stacks = flopStacks;
  root.contributions = {matched, matched};
  root.pot = flopPot;
  root.big_blind = bigBlind;
  root.button = rootButton;

  // ---- Exact engine replay. Cards are inserted at each dealing boundary and
  // voluntary events are applied by kind and exact target total. Optional
  // pot_before/stack_after ledger fields are cross-checked when present.
  HeadsUpState cursor(root);
  try {
    pv::Street replayedStreet = pv::STREET_FLOP;
    for (const PlannedAction& planned_action : planned) {
      if (static_cast<int>(planned_action.street) < static_cast<int>(replayedStreet))
        return fail(V1BlueprintMiss::OffTree);
      while (static_cast<int>(replayedStreet) < static_cast<int>(planned_action.street)) {
        if (cursor.phase() != bs::poker::Phase::Deal)
          return fail(V1BlueprintMiss::OffTree);
        const std::size_t nextBoard = cursor.board().size();
        if (nextBoard >= state.board_size())
          return fail(V1BlueprintMiss::OffTree);
        cursor = cursor.after_card(pokerCard(state.board(static_cast<int>(nextBoard))));
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
      if (planned_action.stackAfter != 0 &&
          cursor.players()[planned_action.actor].stack != planned_action.stackAfter)
        return fail(V1BlueprintMiss::OffTree);
    }

    // Advance to the current decision street.
    while (static_cast<int>(wireStreet(cursor.street())) < static_cast<int>(state.street())) {
      if (cursor.phase() != bs::poker::Phase::Deal)
        return fail(V1BlueprintMiss::OffTree);
      const std::size_t nextBoard = cursor.board().size();
      if (nextBoard >= state.board_size())
        return fail(V1BlueprintMiss::OffTree);
      cursor = cursor.after_card(pokerCard(state.board(static_cast<int>(nextBoard))));
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
  for (std::size_t actor = 0; actor < 2; ++actor) {
    const auto& chips = cursor.players()[actor];
    const pv::PlayerState& player = *actorPlayer[actor];
    if (chips.stack != player.stack() || chips.street_committed != player.street_committed())
      return fail(V1BlueprintMiss::OffTree);
  }
  const Chips due = [&] {
    const auto& chips = cursor.players();
    const Chips high = std::max(chips[0].street_committed, chips[1].street_committed);
    return high - chips[*cursor.actor()].street_committed;
  }();
  if (due != state.to_call())
    return fail(V1BlueprintMiss::OffTree);

  out.root = root;
  out.state = std::move(cursor);
  out.hero_cards = heroCards;
  miss = V1BlueprintMiss::None;
  return true;
}

}  // namespace bs::v1
