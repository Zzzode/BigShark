// resident_mapper.cpp — RFC 0022 wire HandState -> native GameState.
//
// Reconstructs a native GameState from the wire HandState by replaying the
// TransitionEvent path from the true hand-start root. Does not
// rebase mid-street: the replay runs continuously from preflop to the
// actionable node, retaining refunds and folded seats. The G/F contribution
// ledger is validated against the native replay at the actionable node.
//
// Differences from the heads-up protocol:
// - Seat-based identity (uint32 seat) instead of player_id strings
// - Explicit BoardDeal events in the transition path
// - G/F ledger validation (gross_contributed, refunded)
// - Starting stack validation
// - Pot layer projection and validation
// - Observation provenance gate (FULL coverage required)
#include <algorithm>
#include <array>
#include <bs/game_definition.hpp>
#include <bs/protocol.hpp>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "mappers.hpp"

namespace bs {
namespace {

namespace pe = ::bigshark::engine;

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
int pokerCard(const pe::Card& card) {
  return (static_cast<int>(card.rank()) - 1) * 4 + (static_cast<int>(card.suit()) - 1);
}

Street pokerStreet(pe::Street street) {
  switch (street) {
    case pe::STREET_FLOP:
      return Street::Flop;
    case pe::STREET_TURN:
      return Street::Turn;
    case pe::STREET_RIVER:
      return Street::River;
    default:
      return Street::Preflop;
  }
}

// Provenance gate: only FULL coverage with OFFICIAL_RESPONSE or SUPPLIED
// source allows hand-start replay. JOURNAL_RECOVERY, MID_HAND_ATTACH, and GAP
// are rejected because a stateless host cannot verify their coverage.
bool coverageAllowsReplay(const pe::ObservationProvenance& provenance) {
  if (provenance.coverage() != pe::OBSERVATION_COVERAGE_FULL)
    return false;
  if (provenance.source() == pe::OBSERVATION_SOURCE_JOURNAL_RECOVERY)
    return false;
  return true;
}

// One validated action in actor space.
struct PlannedAction {
  pe::Street street;
  std::size_t actor;
  pe::ActionType kind;
  Chips target;
  Chips paid;
  Chips potBefore;
  Chips stackAfter;
};

}  // namespace

bool reconstructHandState(const pe::DecisionRequest& request, ReconstructedState& out,
                          ReplayMiss& miss) {
  const pe::HandState& state = request.state();
  auto fail = [&](ReplayMiss reason) {
    miss = reason;
    out.state.reset();
    out.history.clear();
    return false;
  };

  // ---- Provenance gate -----------------------------------------------------
  if (!state.has_provenance() || !coverageAllowsReplay(state.provenance()))
    return fail(ReplayMiss::HistoryIncomplete);

  // ---- Profile gates: 2..10 seats, no extended utility ---------------------
  if (state.players_size() < static_cast<int>(poker::kMinUnifiedSeats) ||
      state.players_size() > static_cast<int>(poker::kMaxUnifiedSeats))
    return fail(ReplayMiss::IncompatibleRoot);
  const std::size_t seat_count = static_cast<std::size_t>(state.players_size());

  // Reject antes, rake, straddles for now (native GameDef supports ante but
  // the heuristic path does not model it strategically).
  if (state.has_game() && (state.game().ante() != 0 || state.game().button_ante() != 0 ||
                           (state.game().has_straddle() && state.game().straddle().enabled()) ||
                           (state.game().has_rake() && (state.game().rake().basis_points() > 0 ||
                                                        state.game().rake().cap() > 0))))
    return fail(ReplayMiss::UnsupportedFeature);

  // Only SB and BB forced contributions.
  for (const pe::ForcedContribution& forced : state.forced_contributions())
    if (forced.type() != pe::FORCED_CONTRIBUTION_TYPE_SMALL_BLIND &&
        forced.type() != pe::FORCED_CONTRIBUTION_TYPE_BIG_BLIND)
      return fail(ReplayMiss::UnsupportedFeature);

  // ---- Player mapping: sort by seat, rotate so button is actor 0 -----------
  std::vector<const pe::PlayerState*> bySeat;
  for (const pe::PlayerState& player : state.players())
    bySeat.push_back(&player);
  std::sort(bySeat.begin(), bySeat.end(), [](const pe::PlayerState* a, const pe::PlayerState* b) {
    return a->seat() < b->seat();
  });
  for (std::size_t i = 1; i < seat_count; ++i)
    if (bySeat[i - 1]->seat() == bySeat[i]->seat())
      return fail(ReplayMiss::ReplayMismatch);

  // Resolve button to actor index in seat-sorted order.
  std::optional<std::size_t> rootButton;
  for (std::size_t i = 0; i < seat_count; ++i)
    if (bySeat[i]->seat() == state.button_seat())
      rootButton = i;
  if (!rootButton)
    return fail(ReplayMiss::ReplayMismatch);

  // Rotate actor indices so the button is always at index 0. Resident
  // blueprints are trained with button=0; the game is rotationally symmetric
  // with respect to the button, so this preserves strategy identity while
  // allowing any physical button position to match the blueprint.
  std::rotate(bySeat.begin(), bySeat.begin() + static_cast<std::ptrdiff_t>(*rootButton),
              bySeat.end());

  // actorPlayer[actor_index] = wire PlayerState pointer (button-first order)
  const std::vector<const pe::PlayerState*> actorPlayer = bySeat;

  // seatToActor[wire_seat] = actor_index (button-first order)
  std::unordered_map<std::uint32_t, std::size_t> seatToActor;
  for (std::size_t i = 0; i < seat_count; ++i)
    seatToActor[actorPlayer[i]->seat()] = i;

  rootButton = 0;

  // Resolve hero to actor index.
  {
    const auto found = seatToActor.find(state.hero_seat());
    if (found == seatToActor.end())
      return fail(ReplayMiss::ReplayMismatch);
    out.hero_actor = found->second;
  }

  // Hero must be ACTIVE (validator already checks, but double-check).
  if (actorPlayer[out.hero_actor]->status() != pe::PLAYER_STATUS_ACTIVE)
    return fail(ReplayMiss::IncompatibleRoot);

  // ---- Cards: hero hole cards and board ------------------------------------
  if (state.hero_hole_cards_size() != 2)
    return fail(ReplayMiss::ReplayMismatch);
  std::array<int, 2> heroCards{pokerCard(state.hero_hole_cards(0)),
                               pokerCard(state.hero_hole_cards(1))};
  std::sort(heroCards.begin(), heroCards.end());
  if (heroCards[0] < 0 || heroCards[1] >= 52 || heroCards[0] == heroCards[1])
    return fail(ReplayMiss::ReplayMismatch);

  // Board from transition path BoardDeal events.
  std::array<bool, 52> used{};
  used[heroCards[0]] = used[heroCards[1]] = true;
  std::vector<int> boardCards;
  for (const pe::TransitionEvent& event : state.transition_path()) {
    if (event.transition_case() != pe::TransitionEvent::kBoardDeal)
      continue;
    for (const pe::Card& card : event.board_deal().cards()) {
      const int id = pokerCard(card);
      if (id < 0 || id >= 52 || used[id])
        return fail(ReplayMiss::ReplayMismatch);
      used[id] = true;
      boardCards.push_back(id);
    }
  }
  // Board cards from transition path must match the declared board.
  if (boardCards.size() != static_cast<std::size_t>(state.board_size()))
    return fail(ReplayMiss::ReplayMismatch);

  // ---- Blinds from forced contributions ------------------------------------
  const Chips bigBlind = state.game().big_blind();
  std::array<Chips, poker::kMaxUnifiedSeats> blindsPosted{};
  for (const pe::ForcedContribution& forced : state.forced_contributions()) {
    const auto it = seatToActor.find(forced.seat());
    if (it == seatToActor.end())
      return fail(ReplayMiss::ReplayMismatch);
    blindsPosted[it->second] = forced.amount();
  }
  // Exactly one SB and one BB.
  int sb_count = 0, bb_count = 0;
  for (const pe::ForcedContribution& forced : state.forced_contributions()) {
    if (forced.type() == pe::FORCED_CONTRIBUTION_TYPE_SMALL_BLIND)
      ++sb_count;
    else if (forced.type() == pe::FORCED_CONTRIBUTION_TYPE_BIG_BLIND)
      ++bb_count;
  }
  if (sb_count != 1 || bb_count != 1)
    return fail(ReplayMiss::IncompatibleRoot);

  Chips blindsPot = 0;
  for (std::size_t actor = 0; actor < seat_count; ++actor)
    blindsPot += blindsPosted[actor];

  // ---- Starting stacks from wire starting_stack -----------------------------
  std::array<Chips, poker::kMaxUnifiedSeats> startingStacks{};
  for (std::size_t actor = 0; actor < seat_count; ++actor) {
    startingStacks[actor] = actorPlayer[actor]->starting_stack();
    if (startingStacks[actor] == 0)
      return fail(ReplayMiss::IncompatibleRoot);
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
    for (const pe::ForcedContribution& forced : state.forced_contributions()) {
      const std::size_t expected = forced.type() == pe::FORCED_CONTRIBUTION_TYPE_SMALL_BLIND
                                       ? poker::small_blind_seat(rootDef)
                                       : poker::big_blind_seat(rootDef);
      if (seatToActor.at(forced.seat()) != expected)
        return fail(ReplayMiss::IncompatibleRoot);
    }
  }

  // ---- Parse transition path into planned actions and board deals ----------
  std::vector<PlannedAction> planned;
  struct PlannedBoardDeal {
    pe::Street street;
    std::vector<int> cards;
  };
  std::vector<PlannedBoardDeal> boardDeals;

  // Track street commitments for bet/raise disambiguation.
  std::array<Chips, poker::kMaxUnifiedSeats> streetPaid{};
  streetPaid = blindsPosted;
  pe::Street ledgerStreet = pe::STREET_PREFLOP;

  for (const pe::TransitionEvent& event : state.transition_path()) {
    switch (event.transition_case()) {
      case pe::TransitionEvent::kBoardDeal: {
        const pe::BoardDeal& deal = event.board_deal();
        if (deal.street() < ledgerStreet)
          return fail(ReplayMiss::ReplayMismatch);
        if (deal.street() != ledgerStreet) {
          streetPaid.fill(0);
          ledgerStreet = deal.street();
        }
        PlannedBoardDeal plannedDeal;
        plannedDeal.street = deal.street();
        for (const pe::Card& card : deal.cards())
          plannedDeal.cards.push_back(pokerCard(card));
        boardDeals.push_back(std::move(plannedDeal));
        break;
      }
      case pe::TransitionEvent::kAction: {
        const pe::ActionEvent& action = event.action();
        if (action.street() < ledgerStreet)
          return fail(ReplayMiss::ReplayMismatch);
        if (action.street() != ledgerStreet) {
          streetPaid.fill(0);
          ledgerStreet = action.street();
        }
        const auto actorIt = seatToActor.find(action.actor_seat());
        if (actorIt == seatToActor.end())
          return fail(ReplayMiss::ReplayMismatch);
        const std::size_t actor = actorIt->second;

        Chips paid = 0;
        Chips target = 0;
        switch (action.action()) {
          case pe::ACTION_TYPE_FOLD:
          case pe::ACTION_TYPE_CHECK:
            if (action.has_target_total() ||
                (action.has_incremental_amount() && action.incremental_amount() != 0))
              return fail(ReplayMiss::ReplayMismatch);
            break;
          case pe::ACTION_TYPE_CALL:
            if (action.has_target_total() || !action.has_incremental_amount() ||
                action.incremental_amount() == 0)
              return fail(ReplayMiss::ReplayMismatch);
            paid = action.incremental_amount();
            break;
          case pe::ACTION_TYPE_BET:
            if (!action.has_target_total() || streetPaid[actor] != 0)
              return fail(ReplayMiss::ReplayMismatch);
            target = action.target_total();
            paid = target;
            break;
          case pe::ACTION_TYPE_RAISE:
            if (!action.has_target_total() || action.target_total() <= streetPaid[actor])
              return fail(ReplayMiss::ReplayMismatch);
            target = action.target_total();
            paid = target - streetPaid[actor];
            break;
          default:
            return fail(ReplayMiss::ReplayMismatch);
        }
        planned.push_back({action.street(), actor, action.action(), target, paid,
                           action.pot_before(), action.stack_after()});
        streetPaid[actor] += paid;
        break;
      }
      case pe::TransitionEvent::TRANSITION_NOT_SET:
        return fail(ReplayMiss::ReplayMismatch);
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
    for (const pe::TransitionEvent& event : state.transition_path()) {
      if (event.transition_case() == pe::TransitionEvent::kBoardDeal) {
        // Deal board cards.
        const PlannedBoardDeal& deal = boardDeals[boardDealIdx++];
        for (int card : deal.cards) {
          if (cursor.phase() != Phase::Deal)
            return fail(ReplayMiss::ReplayMismatch);
          cursor = cursor.after_card(card);
        }
      } else if (event.transition_case() == pe::TransitionEvent::kAction) {
        const PlannedAction& planned_action = planned[actionIdx++];
        if (cursor.phase() != Phase::Action || !cursor.actor().has_value() ||
            *cursor.actor() != planned_action.actor)
          return fail(ReplayMiss::ReplayMismatch);
        if (planned_action.potBefore != 0 && cursor.pot() != planned_action.potBefore)
          return fail(ReplayMiss::ReplayMismatch);

        Action nativeAction{};
        switch (planned_action.kind) {
          case pe::ACTION_TYPE_FOLD:
            nativeAction = {ActionType::Fold};
            break;
          case pe::ACTION_TYPE_CHECK:
            nativeAction = {ActionType::Check};
            break;
          case pe::ACTION_TYPE_CALL:
            nativeAction = {ActionType::Call};
            break;
          case pe::ACTION_TYPE_BET:
          case pe::ACTION_TYPE_RAISE: {
            // The engine distinguishes Bet (opening the street, due == 0) from
            // Raise (facing an existing bet).
            Chips high = 0;
            for (std::size_t a = 0; a < seat_count; ++a)
              high = std::max(high, cursor.players()[a].street_committed);
            const Chips due = high - cursor.players()[planned_action.actor].street_committed;
            // Postflop BET and RAISE are distinct observations.
            if (planned_action.street != pe::STREET_PREFLOP &&
                ((planned_action.kind == pe::ACTION_TYPE_BET && due != 0) ||
                 (planned_action.kind == pe::ACTION_TYPE_RAISE && due == 0)))
              return fail(ReplayMiss::ReplayMismatch);
            nativeAction = {due == 0 ? ActionType::Bet : ActionType::Raise, planned_action.target};
            break;
          }
          default:
            return fail(ReplayMiss::ReplayMismatch);
        }
        const Chips contributed_before = cursor.players()[planned_action.actor].contributed;
        cursor = cursor.after_action(planned_action.actor, nativeAction);
        // Refunds are accepted (never rejected).
        if (seat_count > 2 &&
            cursor.players()[planned_action.actor].contributed - contributed_before !=
                planned_action.paid)
          return fail(ReplayMiss::ReplayMismatch);
        if (planned_action.stackAfter != 0 &&
            cursor.players()[planned_action.actor].stack != planned_action.stackAfter)
          return fail(ReplayMiss::ReplayMismatch);
        out.history.push_back(
            PublicAction{pokerStreet(planned_action.street), planned_action.actor, nativeAction});
      }
    }

    // ---- Decision-node invariants against the structured snapshot ----------
    if (cursor.street() != pokerStreet(state.street()))
      return fail(ReplayMiss::ReplayMismatch);
    if (cursor.board().size() != static_cast<std::size_t>(state.board_size()))
      return fail(ReplayMiss::ReplayMismatch);
    if (!cursor.actor().has_value() || *cursor.actor() != out.hero_actor)
      return fail(ReplayMiss::ReplayMismatch);
    if (cursor.pot() != state.pot().pot_total())
      return fail(ReplayMiss::ReplayMismatch);

    // G/F ledger validation: native contributed == wire gross_contributed (G),
    // native refunded == wire refunded (F), native stack == wire stack.
    for (std::size_t actor = 0; actor < seat_count; ++actor) {
      const auto& chips = cursor.players()[actor];
      const pe::PlayerState& player = *actorPlayer[actor];
      if (chips.stack != player.stack())
        return fail(ReplayMiss::ReplayMismatch);
      if (chips.street_committed != player.street_committed())
        return fail(ReplayMiss::ReplayMismatch);
      if (chips.contributed != player.gross_contributed())
        return fail(ReplayMiss::ReplayMismatch);
      if (chips.refunded != player.refunded())
        return fail(ReplayMiss::ReplayMismatch);
      // Folded/all-in status.
      const pe::PlayerStatus expectedStatus = chips.folded   ? pe::PLAYER_STATUS_FOLDED
                                              : chips.all_in ? pe::PLAYER_STATUS_ALL_IN
                                                             : pe::PLAYER_STATUS_ACTIVE;
      if (player.status() != expectedStatus)
        return fail(ReplayMiss::ReplayMismatch);
    }

    // to_call validation.
    const Chips due = cursor.legal().call_amount;
    if (due != state.to_call())
      return fail(ReplayMiss::ReplayMismatch);

    // Legal action set validation: wire legal_actions must match native legal.
    const poker::LegalActions& nativeLegal = cursor.legal();
    bool wireHasFold = false, wireHasCheck = false, wireHasCall = false;
    bool wireHasBet = false, wireHasRaise = false;
    Chips wireMinTarget = 0, wireMaxTarget = 0;
    for (const pe::LegalAction& legal : state.legal_actions()) {
      switch (legal.type()) {
        case pe::ACTION_TYPE_FOLD:
          wireHasFold = true;
          break;
        case pe::ACTION_TYPE_CHECK:
          wireHasCheck = true;
          break;
        case pe::ACTION_TYPE_CALL:
          wireHasCall = true;
          break;
        case pe::ACTION_TYPE_BET:
          wireHasBet = true;
          if (legal.has_min_target_total())
            wireMinTarget = legal.min_target_total();
          if (legal.has_max_target_total())
            wireMaxTarget = legal.max_target_total();
          break;
        case pe::ACTION_TYPE_RAISE:
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
      return fail(ReplayMiss::ReplayMismatch);
    if (wireHasCheck != nativeLegal.check)
      return fail(ReplayMiss::ReplayMismatch);
    if (wireHasCall != nativeLegal.call)
      return fail(ReplayMiss::ReplayMismatch);
    if (nativeLegal.aggressive.has_value()) {
      const bool nativeIsBet = nativeLegal.aggressive->type == ActionType::Bet;
      if (nativeIsBet != wireHasBet || !nativeIsBet != wireHasRaise)
        return fail(ReplayMiss::ReplayMismatch);
      if (wireMinTarget != nativeLegal.aggressive->minimum ||
          wireMaxTarget != nativeLegal.aggressive->maximum)
        return fail(ReplayMiss::ReplayMismatch);
    } else {
      if (wireHasBet || wireHasRaise)
        return fail(ReplayMiss::ReplayMismatch);
    }

    if (state.street() == pe::STREET_PREFLOP) {
      // ---- Preflop decision: hand-start root for blueprint identity --------
      // The preflop blueprint's root is the hand-start state (starting stacks,
      // zero contributions, blinds-only pot, no board). Build the GameState
      // from rootDef so same_root() matches the blueprint identity. The
      // history carries the observed preflop actions from the root to the
      // decision point; replay_public_path replays them against the blueprint
      // tree to reach the actionable node.
      GameState rootState(rootDef);
      out.state = std::move(rootState);
    } else {
      // ---- Postflop decision: flop-rooted GameState ------------------------
      // The flop library's root is the flop state. Preflop actions are already
      // reflected in the flop-rooted stacks/contributions/folded flags, so
      // they are excluded from the history. The Street enum orders Flop=0,
      // Turn=1, River=2, Preflop=3, so preflop is the highest value.
      out.history.erase(std::remove_if(out.history.begin(), out.history.end(),
                                       [](const poker::PublicAction& a) {
                                         return a.street == poker::Street::Preflop;
                                       }),
                        out.history.end());

      // RFC 0032: carry the cursor's RUNTIME and attach a decision-node
      // identity, instead of rebuilding both from a flop-shaped def.
      //
      // The rebuild this replaces copied gross `contributed` against a net
      // `pot()`, so poker::validate rejected it for a heads-up node facing a
      // bet and for any refunded hand; it also re-derived `pending` and the
      // actor from three fields, which cannot express "this seat already acted"
      // and so answered a checked-to turn for the wrong seat. The cursor already
      // holds the real board, the true pending set, the correct actor, and the
      // net ledger; only its `def()` is stale, because `after_action` and
      // `after_card` never touch `def_`.
      GameDef nodeDef{};
      nodeDef.player_count = seat_count;
      nodeDef.button = *rootButton;
      nodeDef.big_blind = bigBlind;
      nodeDef.preflop = false;
      nodeDef.terminal = rootDef.terminal;
      nodeDef.board_size = static_cast<std::uint8_t>(cursor.board().size());
      for (std::size_t i = 0; i < cursor.board().size(); ++i)
        nodeDef.board[i] = cursor.board()[i];
      for (std::size_t a = 0; a < seat_count; ++a) {
        const auto& seat = cursor.players()[a];
        nodeDef.stacks[a] = seat.stack;
        // NET, so the def reconciles with pot() (net_contributed() is
        // contributed - refunded) and passes validate for a refunded hand.
        nodeDef.contributions[a] = seat.net_contributed();
        nodeDef.folded[a] = seat.folded;
        // The mid-street fields are what make an unequal-contribution node
        // valid: validate relaxes the two-seat equality rule only when
        // mid_street, which it derives from street_committed > 0. Same rule
        // build_subgame_def_from_node already uses.
        nodeDef.street_committed[a] = seat.street_committed;
        nodeDef.raise_rights[a] = seat.raise_rights;
      }
      nodeDef.pot = cursor.pot();
      // GameDef's sentinel for "street start" is 0 (the constructor substitutes
      // big_blind); only a MID-STREET node carries a real increment, where
      // validate requires >= big_blind. `last_full_raise() > 0` is not the test:
      // a street-start node also reports big_blind there, so a refunded
      // street-start node (unequal net contributions, no live wager) would carry
      // a nonzero increment and `validate` would reject it. The rule is the same
      // one build_subgame_def_from_node applies to the identical field set.
      bool mid_street = false;
      for (std::size_t a = 0; a < seat_count; ++a)
        if (cursor.players()[a].street_committed > 0)
          mid_street = true;
      nodeDef.last_full_raise = mid_street ? cursor.last_full_raise() : 0;
      out.state = GameState::with_identity(cursor, nodeDef);
    }
    out.hero_cards = heroCards;
    miss = ReplayMiss::None;
    return true;
  } catch (const std::exception&) {
    // Preserve constructor failure behavior for heads-up; other failures are
    // deterministic replay misses.
    if (!root_constructed && seat_count == 2)
      throw;
    return fail(ReplayMiss::ReplayMismatch);
  }
}

}  // namespace bs
