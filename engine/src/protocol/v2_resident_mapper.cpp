// v2_resident_mapper.cpp — RFC 0022 v2 wire HandState -> native GameState.
//
// Reconstructs a native GameState from the v2 HandState by replaying the
// TransitionEvent path from the true hand-start root. Unlike v1, v2 does not
// rebase mid-street: the replay runs continuously from preflop to the
// actionable node, retaining refunds and folded seats. The G/F contribution
// ledger is validated against the native replay at the actionable node.
//
// v2-specific differences from v1:
// - Seat-based identity (uint32 seat) instead of player_id strings
// - Explicit BoardDeal events in the transition path
// - G/F ledger validation (gross_contributed, refunded)
// - Starting stack validation
// - Pot layer projection and validation
// - Observation provenance gate (FULL coverage required)
#include <algorithm>
#include <array>
#include <bs/game_definition.hpp>
#include <bs/v2_protocol.hpp>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "v2_mappers.hpp"

namespace bs::v2 {
namespace {

namespace pv2 = ::bigshark::engine::v2;

using poker::Action;
using poker::ActionType;
using poker::Chips;
using poker::GameDef;
using poker::GameState;
using poker::Phase;
using poker::PublicAction;
using poker::Street;

// proto card: (Rank-1)*4 + (Suit-1) into the poker id space rank*4+suit with
// rank 0..12 (2..A) and suit 0..3 (s,h,d,c).
int pokerCard(const pv2::Card& card) {
  return (static_cast<int>(card.rank()) - 1) * 4 + (static_cast<int>(card.suit()) - 1);
}

Street pokerStreet(pv2::Street street) {
  switch (street) {
    case pv2::STREET_FLOP:
      return Street::Flop;
    case pv2::STREET_TURN:
      return Street::Turn;
    case pv2::STREET_RIVER:
      return Street::River;
    default:
      return Street::Preflop;
  }
}

// Provenance gate: only FULL coverage with OFFICIAL_RESPONSE or SUPPLIED
// source allows hand-start replay. JOURNAL_RECOVERY, MID_HAND_ATTACH, and GAP
// are rejected because a stateless host cannot verify their coverage.
bool coverageAllowsReplay(const pv2::ObservationProvenance& provenance) {
  if (provenance.coverage() != pv2::OBSERVATION_COVERAGE_FULL)
    return false;
  if (provenance.source() == pv2::OBSERVATION_SOURCE_JOURNAL_RECOVERY)
    return false;
  return true;
}

// One validated action in actor space.
struct PlannedAction {
  pv2::Street street;
  std::size_t actor;
  pv2::ActionType kind;
  Chips target;
  Chips paid;
  Chips potBefore;
  Chips stackAfter;
};

}  // namespace

bool reconstructHandState(const pv2::DecisionRequest& request, ReconstructedState& out,
                          V2ReplayMiss& miss) {
  const pv2::HandState& state = request.state();
  auto fail = [&](V2ReplayMiss reason) {
    miss = reason;
    out.state.reset();
    out.history.clear();
    return false;
  };

  // ---- Provenance gate -----------------------------------------------------
  if (!state.has_provenance() || !coverageAllowsReplay(state.provenance()))
    return fail(V2ReplayMiss::HistoryIncomplete);

  // ---- Profile gates: 2..10 seats, no extended utility ---------------------
  if (state.players_size() < static_cast<int>(poker::kMinUnifiedSeats) ||
      state.players_size() > static_cast<int>(poker::kMaxUnifiedSeats))
    return fail(V2ReplayMiss::IncompatibleRoot);
  const std::size_t seat_count = static_cast<std::size_t>(state.players_size());

  // Reject antes, rake, straddles for now (native GameDef supports ante but
  // the heuristic path does not model it strategically).
  if (state.has_game() && (state.game().ante() != 0 || state.game().button_ante() != 0 ||
                           (state.game().has_straddle() && state.game().straddle().enabled()) ||
                           (state.game().has_rake() && (state.game().rake().basis_points() > 0 ||
                                                        state.game().rake().cap() > 0))))
    return fail(V2ReplayMiss::UnsupportedFeature);

  // Only SB and BB forced contributions.
  for (const pv2::ForcedContribution& forced : state.forced_contributions())
    if (forced.type() != pv2::FORCED_CONTRIBUTION_TYPE_SMALL_BLIND &&
        forced.type() != pv2::FORCED_CONTRIBUTION_TYPE_BIG_BLIND)
      return fail(V2ReplayMiss::UnsupportedFeature);

  // ---- Player mapping: sort by seat, map seat -> actor index ---------------
  std::vector<const pv2::PlayerState*> bySeat;
  for (const pv2::PlayerState& player : state.players())
    bySeat.push_back(&player);
  std::sort(bySeat.begin(), bySeat.end(), [](const pv2::PlayerState* a, const pv2::PlayerState* b) {
    return a->seat() < b->seat();
  });
  for (std::size_t i = 1; i < seat_count; ++i)
    if (bySeat[i - 1]->seat() == bySeat[i]->seat())
      return fail(V2ReplayMiss::ReplayMismatch);

  // actorPlayer[actor_index] = wire PlayerState pointer
  const std::vector<const pv2::PlayerState*> actorPlayer = bySeat;

  // seatToActor[wire_seat] = actor_index
  std::unordered_map<std::uint32_t, std::size_t> seatToActor;
  for (std::size_t i = 0; i < seat_count; ++i)
    seatToActor[actorPlayer[i]->seat()] = i;

  // Resolve button to actor index.
  std::optional<std::size_t> rootButton;
  for (std::size_t i = 0; i < seat_count; ++i)
    if (actorPlayer[i]->seat() == state.button_seat())
      rootButton = i;
  if (!rootButton)
    return fail(V2ReplayMiss::ReplayMismatch);

  // Resolve hero to actor index.
  {
    const auto found = seatToActor.find(state.hero_seat());
    if (found == seatToActor.end())
      return fail(V2ReplayMiss::ReplayMismatch);
    out.hero_actor = found->second;
  }

  // Hero must be ACTIVE (validator already checks, but double-check).
  if (actorPlayer[out.hero_actor]->status() != pv2::PLAYER_STATUS_ACTIVE)
    return fail(V2ReplayMiss::IncompatibleRoot);

  // ---- Cards: hero hole cards and board ------------------------------------
  if (state.hero_hole_cards_size() != 2)
    return fail(V2ReplayMiss::ReplayMismatch);
  std::array<int, 2> heroCards{pokerCard(state.hero_hole_cards(0)),
                               pokerCard(state.hero_hole_cards(1))};
  std::sort(heroCards.begin(), heroCards.end());
  if (heroCards[0] < 0 || heroCards[1] >= 52 || heroCards[0] == heroCards[1])
    return fail(V2ReplayMiss::ReplayMismatch);

  // Board from transition path BoardDeal events.
  std::array<bool, 52> used{};
  used[heroCards[0]] = used[heroCards[1]] = true;
  std::vector<int> boardCards;
  for (const pv2::TransitionEvent& event : state.transition_path()) {
    if (event.transition_case() != pv2::TransitionEvent::kBoardDeal)
      continue;
    for (const pv2::Card& card : event.board_deal().cards()) {
      const int id = pokerCard(card);
      if (id < 0 || id >= 52 || used[id])
        return fail(V2ReplayMiss::ReplayMismatch);
      used[id] = true;
      boardCards.push_back(id);
    }
  }
  // Board cards from transition path must match the declared board.
  if (boardCards.size() != static_cast<std::size_t>(state.board_size()))
    return fail(V2ReplayMiss::ReplayMismatch);

  // ---- Blinds from forced contributions ------------------------------------
  const Chips bigBlind = state.game().big_blind();
  std::array<Chips, poker::kMaxUnifiedSeats> blindsPosted{};
  for (const pv2::ForcedContribution& forced : state.forced_contributions()) {
    const auto it = seatToActor.find(forced.seat());
    if (it == seatToActor.end())
      return fail(V2ReplayMiss::ReplayMismatch);
    blindsPosted[it->second] = forced.amount();
  }
  // Exactly one SB and one BB.
  int sb_count = 0, bb_count = 0;
  for (const pv2::ForcedContribution& forced : state.forced_contributions()) {
    if (forced.type() == pv2::FORCED_CONTRIBUTION_TYPE_SMALL_BLIND)
      ++sb_count;
    else if (forced.type() == pv2::FORCED_CONTRIBUTION_TYPE_BIG_BLIND)
      ++bb_count;
  }
  if (sb_count != 1 || bb_count != 1)
    return fail(V2ReplayMiss::IncompatibleRoot);

  Chips blindsPot = 0;
  for (std::size_t actor = 0; actor < seat_count; ++actor)
    blindsPot += blindsPosted[actor];

  // ---- Starting stacks from wire starting_stack -----------------------------
  std::array<Chips, poker::kMaxUnifiedSeats> startingStacks{};
  for (std::size_t actor = 0; actor < seat_count; ++actor) {
    startingStacks[actor] = actorPlayer[actor]->starting_stack();
    if (startingStacks[actor] == 0)
      return fail(V2ReplayMiss::IncompatibleRoot);
  }

  // ---- Build the preflop GameDef -------------------------------------------
  GameDef rootDef{};
  rootDef.player_count = seat_count;
  rootDef.button = static_cast<int>(*rootButton);
  rootDef.big_blind = bigBlind;
  rootDef.stacks = startingStacks;
  rootDef.contributions = {};
  rootDef.pot = blindsPot;
  rootDef.board = {};
  rootDef.board_size = 0;
  rootDef.blinds_posted = blindsPosted;
  rootDef.preflop = true;
  rootDef.terminal = poker::TerminalDepth::River;

  // Verify blind positions for 3+ seats (heads-up uses the button=SB convention).
  if (seat_count > 2) {
    for (const pv2::ForcedContribution& forced : state.forced_contributions()) {
      const std::size_t expected = forced.type() == pv2::FORCED_CONTRIBUTION_TYPE_SMALL_BLIND
                                       ? poker::small_blind_seat(rootDef)
                                       : poker::big_blind_seat(rootDef);
      if (seatToActor.at(forced.seat()) != expected)
        return fail(V2ReplayMiss::IncompatibleRoot);
    }
  }

  // ---- Parse transition path into planned actions and board deals ----------
  std::vector<PlannedAction> planned;
  struct PlannedBoardDeal {
    pv2::Street street;
    std::vector<int> cards;
  };
  std::vector<PlannedBoardDeal> boardDeals;

  // Track street commitments for bet/raise disambiguation.
  std::array<Chips, poker::kMaxUnifiedSeats> streetPaid{};
  streetPaid = blindsPosted;
  pv2::Street ledgerStreet = pv2::STREET_PREFLOP;

  for (const pv2::TransitionEvent& event : state.transition_path()) {
    switch (event.transition_case()) {
      case pv2::TransitionEvent::kBoardDeal: {
        const pv2::BoardDeal& deal = event.board_deal();
        if (deal.street() < ledgerStreet)
          return fail(V2ReplayMiss::ReplayMismatch);
        if (deal.street() != ledgerStreet) {
          streetPaid.fill(0);
          ledgerStreet = deal.street();
        }
        PlannedBoardDeal plannedDeal;
        plannedDeal.street = deal.street();
        for (const pv2::Card& card : deal.cards())
          plannedDeal.cards.push_back(pokerCard(card));
        boardDeals.push_back(std::move(plannedDeal));
        break;
      }
      case pv2::TransitionEvent::kAction: {
        const pv2::ActionEvent& action = event.action();
        if (action.street() < ledgerStreet)
          return fail(V2ReplayMiss::ReplayMismatch);
        if (action.street() != ledgerStreet) {
          streetPaid.fill(0);
          ledgerStreet = action.street();
        }
        const auto actorIt = seatToActor.find(action.actor_seat());
        if (actorIt == seatToActor.end())
          return fail(V2ReplayMiss::ReplayMismatch);
        const std::size_t actor = actorIt->second;

        Chips paid = 0;
        Chips target = 0;
        switch (action.action()) {
          case pv2::ACTION_TYPE_FOLD:
          case pv2::ACTION_TYPE_CHECK:
            if (action.has_target_total() ||
                (action.has_incremental_amount() && action.incremental_amount() != 0))
              return fail(V2ReplayMiss::ReplayMismatch);
            break;
          case pv2::ACTION_TYPE_CALL:
            if (action.has_target_total() || !action.has_incremental_amount() ||
                action.incremental_amount() == 0)
              return fail(V2ReplayMiss::ReplayMismatch);
            paid = action.incremental_amount();
            break;
          case pv2::ACTION_TYPE_BET:
            if (!action.has_target_total() || streetPaid[actor] != 0)
              return fail(V2ReplayMiss::ReplayMismatch);
            target = action.target_total();
            paid = target;
            break;
          case pv2::ACTION_TYPE_RAISE:
            if (!action.has_target_total() || action.target_total() <= streetPaid[actor])
              return fail(V2ReplayMiss::ReplayMismatch);
            target = action.target_total();
            paid = target - streetPaid[actor];
            break;
          default:
            return fail(V2ReplayMiss::ReplayMismatch);
        }
        planned.push_back({action.street(), actor, action.action(), target, paid,
                           action.pot_before(), action.stack_after()});
        streetPaid[actor] += paid;
        break;
      }
      case pv2::TransitionEvent::TRANSITION_NOT_SET:
        return fail(V2ReplayMiss::ReplayMismatch);
    }
  }

  // ---- Exact engine replay --------------------------------------------------
  bool root_constructed = false;
  try {
    GameState cursor(rootDef);
    root_constructed = true;

    std::size_t boardDealIdx = 0;
    std::size_t actionIdx = 0;

    // Replay events in transition_path order.
    for (const pv2::TransitionEvent& event : state.transition_path()) {
      if (event.transition_case() == pv2::TransitionEvent::kBoardDeal) {
        // Deal board cards.
        const PlannedBoardDeal& deal = boardDeals[boardDealIdx++];
        for (int card : deal.cards) {
          if (cursor.phase() != Phase::Deal)
            return fail(V2ReplayMiss::ReplayMismatch);
          cursor = cursor.after_card(card);
        }
      } else if (event.transition_case() == pv2::TransitionEvent::kAction) {
        const PlannedAction& planned_action = planned[actionIdx++];
        if (cursor.phase() != Phase::Action || !cursor.actor().has_value() ||
            *cursor.actor() != planned_action.actor)
          return fail(V2ReplayMiss::ReplayMismatch);
        if (planned_action.potBefore != 0 && cursor.pot() != planned_action.potBefore)
          return fail(V2ReplayMiss::ReplayMismatch);

        Action nativeAction{};
        switch (planned_action.kind) {
          case pv2::ACTION_TYPE_FOLD:
            nativeAction = {ActionType::Fold};
            break;
          case pv2::ACTION_TYPE_CHECK:
            nativeAction = {ActionType::Check};
            break;
          case pv2::ACTION_TYPE_CALL:
            nativeAction = {ActionType::Call};
            break;
          case pv2::ACTION_TYPE_BET:
          case pv2::ACTION_TYPE_RAISE: {
            // The engine distinguishes Bet (opening the street, due == 0) from
            // Raise (facing an existing bet).
            Chips high = 0;
            for (std::size_t a = 0; a < seat_count; ++a)
              high = std::max(high, cursor.players()[a].street_committed);
            const Chips due = high - cursor.players()[planned_action.actor].street_committed;
            // Postflop BET and RAISE are distinct observations.
            if (planned_action.street != pv2::STREET_PREFLOP &&
                ((planned_action.kind == pv2::ACTION_TYPE_BET && due != 0) ||
                 (planned_action.kind == pv2::ACTION_TYPE_RAISE && due == 0)))
              return fail(V2ReplayMiss::ReplayMismatch);
            nativeAction = {due == 0 ? ActionType::Bet : ActionType::Raise, planned_action.target};
            break;
          }
          default:
            return fail(V2ReplayMiss::ReplayMismatch);
        }
        const Chips contributed_before = cursor.players()[planned_action.actor].contributed;
        cursor = cursor.after_action(planned_action.actor, nativeAction);
        // Refunds are accepted in v2 (never rejected, unlike v1).
        if (seat_count > 2 &&
            cursor.players()[planned_action.actor].contributed - contributed_before !=
                planned_action.paid)
          return fail(V2ReplayMiss::ReplayMismatch);
        if (planned_action.stackAfter != 0 &&
            cursor.players()[planned_action.actor].stack != planned_action.stackAfter)
          return fail(V2ReplayMiss::ReplayMismatch);
        out.history.push_back(
            PublicAction{pokerStreet(planned_action.street), planned_action.actor, nativeAction});
      }
    }

    // ---- Decision-node invariants against the structured snapshot ----------
    if (cursor.street() != pokerStreet(state.street()))
      return fail(V2ReplayMiss::ReplayMismatch);
    if (cursor.board().size() != static_cast<std::size_t>(state.board_size()))
      return fail(V2ReplayMiss::ReplayMismatch);
    if (!cursor.actor().has_value() || *cursor.actor() != out.hero_actor)
      return fail(V2ReplayMiss::ReplayMismatch);
    if (cursor.pot() != state.pot().pot_total())
      return fail(V2ReplayMiss::ReplayMismatch);

    // G/F ledger validation: native contributed == wire gross_contributed (G),
    // native refunded == wire refunded (F), native stack == wire stack.
    for (std::size_t actor = 0; actor < seat_count; ++actor) {
      const auto& chips = cursor.players()[actor];
      const pv2::PlayerState& player = *actorPlayer[actor];
      if (chips.stack != player.stack())
        return fail(V2ReplayMiss::ReplayMismatch);
      if (chips.street_committed != player.street_committed())
        return fail(V2ReplayMiss::ReplayMismatch);
      if (chips.contributed != player.gross_contributed())
        return fail(V2ReplayMiss::ReplayMismatch);
      if (chips.refunded != player.refunded())
        return fail(V2ReplayMiss::ReplayMismatch);
      // Folded/all-in status.
      const pv2::PlayerStatus expectedStatus = chips.folded   ? pv2::PLAYER_STATUS_FOLDED
                                               : chips.all_in ? pv2::PLAYER_STATUS_ALL_IN
                                                              : pv2::PLAYER_STATUS_ACTIVE;
      if (player.status() != expectedStatus)
        return fail(V2ReplayMiss::ReplayMismatch);
    }

    // to_call validation.
    const Chips due = cursor.legal().call_amount;
    if (due != state.to_call())
      return fail(V2ReplayMiss::ReplayMismatch);

    // Legal action set validation: wire legal_actions must match native legal.
    const poker::LegalActions& nativeLegal = cursor.legal();
    bool wireHasFold = false, wireHasCheck = false, wireHasCall = false;
    bool wireHasBet = false, wireHasRaise = false;
    Chips wireMinTarget = 0, wireMaxTarget = 0;
    for (const pv2::LegalAction& legal : state.legal_actions()) {
      switch (legal.type()) {
        case pv2::ACTION_TYPE_FOLD:
          wireHasFold = true;
          break;
        case pv2::ACTION_TYPE_CHECK:
          wireHasCheck = true;
          break;
        case pv2::ACTION_TYPE_CALL:
          wireHasCall = true;
          break;
        case pv2::ACTION_TYPE_BET:
          wireHasBet = true;
          if (legal.has_min_target_total())
            wireMinTarget = legal.min_target_total();
          if (legal.has_max_target_total())
            wireMaxTarget = legal.max_target_total();
          break;
        case pv2::ACTION_TYPE_RAISE:
          wireHasRaise = true;
          if (legal.has_min_target_total())
            wireMinTarget = legal.min_target_total();
          if (legal.has_max_target_total())
            wireMaxTarget = legal.max_target_total();
          break;
        default:
          break;
      }
    }
    if (wireHasFold != nativeLegal.fold)
      return fail(V2ReplayMiss::ReplayMismatch);
    if (wireHasCheck != nativeLegal.check)
      return fail(V2ReplayMiss::ReplayMismatch);
    if (wireHasCall != nativeLegal.call)
      return fail(V2ReplayMiss::ReplayMismatch);
    if (nativeLegal.aggressive.has_value()) {
      const bool nativeIsBet = nativeLegal.aggressive->type == ActionType::Bet;
      if (nativeIsBet != wireHasBet || !nativeIsBet != wireHasRaise)
        return fail(V2ReplayMiss::ReplayMismatch);
      if (wireMinTarget != nativeLegal.aggressive->minimum ||
          wireMaxTarget != nativeLegal.aggressive->maximum)
        return fail(V2ReplayMiss::ReplayMismatch);
    } else {
      if (wireHasBet || wireHasRaise)
        return fail(V2ReplayMiss::ReplayMismatch);
    }

    out.state = std::move(cursor);
    out.hero_cards = heroCards;
    miss = V2ReplayMiss::None;
    return true;
  } catch (const std::exception&) {
    // Preserve constructor failure behavior for heads-up; other failures are
    // deterministic replay misses.
    if (!root_constructed && seat_count == 2)
      throw;
    return fail(V2ReplayMiss::ReplayMismatch);
  }
}

}  // namespace bs::v2
