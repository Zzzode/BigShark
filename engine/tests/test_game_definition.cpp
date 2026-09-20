// Independent equivalence oracle for the unified game definition (RFC 0008
// stage 1).
//
// This suite drives `bs::poker::GameState` against a reference ledger written
// here, from the rules of no-limit hold'em and from `GameDef`'s structure. It
// deliberately does NOT include `bs/heads_up.hpp`: comparing the unified state
// to `HeadsUpState` would only prove that two implementations agree, never that
// either is right. A reviewer can check that claim by reading this include list.
//
// Non-vacuity is part of the contract. Every branch this file checks is counted,
// and the counts are asserted positive and printed, because a walk that never
// reaches a branch passes no matter what the implementation does. This
// repository has already shipped two tests that could not fail.
//
// Mutation verification is machine-run, not hand-run:
//   npm run mutation
// or directly, `node dist/tools/mutation/verify.js --config <config>`. The runner
// is TypeScript; there is no .mjs copy, because handwritten JavaScript is
// forbidden under tools/.
// It applies each mutation, rebuilds this target, classifies the result, and
// restores the file. Every mutation listed there must be RED except the ones
// recorded as equivalent below; a new GREEN is a coverage gap to close, a
// surprising RED means the suite caught something other than what was intended.
//
// The battery catches FOURTEEN semantic mutations of the stage-1 code, including
// three this file had missed (and two that were added after the fact to hold the
// mistakes an independent reviewer found): a rooted flop with fewer than two actionable seats
// (reachable only from an empty stack, which the settlement sweep's range never
// produced), a settlement that awards the pot by seat order rather than by hand
// score (invisible while both fixture hands tied on a board straight flush), and
// after_card opening action with a single actionable seat. The same config
// carries a second target, `test_heads_up`, whose counts the stage evidence
// cites; that target's own two mutations are what turn a citation into a check.
//
// FOUR mutants are EQUIVALENT at two seats: they change no reachable state. They
// are recorded in tools/mutation/game-definition.json with their rationale, and
// the two that could plausibly become reachable are pinned by standing
// assertions here rather than left as an argument that expires silently.
//   - `refund_unmatched` stops skipping folded seats when finding the high
//     commitment. The guard can only matter when a FOLDED seat holds the strict
//     maximum, which cannot happen at two seats: a seat folds facing a bet, so
//     its commitment is strictly below the bettor's, and the only other chip
//     movement lowers a commitment rather than raising one.
//   - `legal()` ignores `raise_rights`. The states where the two versions differ
//     are those where the actor holds no raise rights AND every other guard in
//     `legal()` would still allow a raise. Zero such states are reachable.
//   - The constructor omits deriving `all_in` after an ANTE. Unreachable here:
//     the two-seat profile rejects a nonzero ante outright, which
//     `ante_is_rejected_in_this_profile` pins.
//   - The rooted `pending` set keys off the folded flag instead of the stack.
//     Unobservable here: `GameDef` declares no folded seat, so at a root every
//     seat is unfolded and the two predicates denote the same set.
// The non-vacuity pins for the first two are asserted at the end of the flop
// sweep, so a fixture change that reaches one of those states turns this claim
// red instead of silently expiring.
#include <algorithm>
#include <array>
#include <bs/eval.hpp>
#include <bs/game_definition.hpp>
#include <bs/settlement.hpp>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

using namespace bs::poker;

#define CHECK(cond)                                                         \
  do {                                                                      \
    if (!(cond)) {                                                          \
      std::printf("CHECK failed: %s (%s:%d)\n", #cond, __FILE__, __LINE__); \
      return 1;                                                             \
    }                                                                       \
  } while (0)

namespace {

int card(const char* name) {
  return bs::cardId(std::string(name));
}

// ---------------------------------------------------------------------------
// The reference ledger. Deliberately small and self-contained: only the two
// seats this stage serves, so every rule can be read at a glance.
//
// Seats are 0 and 1 with `button` naming one of them. The two-seat blind rule
// is NOT the three-handed rule turned down: the BUTTON posts the small blind
// and the other seat posts the big blind, which is why the button acts first
// preflop. Deriving it as "one clockwise from the button" would put the small
// blind on the non-button seat and the big blind back on the button.
// ---------------------------------------------------------------------------
struct Ref {
  std::array<Chips, 2> stack{}, committed{}, contributed{}, returned{};
  std::array<bool, 2> folded{}, all_in{}, raise_rights{}, pending{};
  int board_size = 0;
  std::array<int, 5> board{};
  Street street = Street::Flop;
  Phase phase = Phase::Action;
  int actor = -1;
  Chips big_blind = 0;
  Chips last_full_raise = 0;
  bool preflop_root = false;
  std::size_t button = 0;
  Chips dead_pot = 0;
  int history = 0;

  Chips high() const { return std::max(committed[0], committed[1]); }
  int opponent(int p) const { return 1 - p; }
  int live_count() const { return (folded[0] ? 0 : 1) + (folded[1] ? 0 : 1); }
  int actionable_count() const {
    return ((!folded[0] && !all_in[0]) ? 1 : 0) + ((!folded[1] && !all_in[1]) ? 1 : 0);
  }
  bool actionable(int p) const { return pending[p] && !folded[p] && !all_in[p]; }
  std::size_t small_blind_seat() const { return button; }
  std::size_t big_blind_seat() const { return (button + 1) % 2; }

  void pay(int p, Chips amount) {
    stack[p] -= amount;
    committed[p] += amount;
    contributed[p] += amount;
    all_in[p] = stack[p] == 0;
  }

  // An unmatched excess returns only down to the best level the OTHER seat
  // reached. A folded seat's chips are dead money but still establish a level.
  void refund_unmatched() {
    int high_seat = -1;
    Chips amount = 0;
    int count = 0;
    for (int p = 0; p < 2; ++p) {
      if (folded[p])
        continue;
      if (committed[p] > amount) {
        amount = committed[p];
        high_seat = p;
        count = 1;
      } else if (committed[p] == amount) {
        ++count;
      }
    }
    if (count != 1)
      return;
    const Chips other = committed[opponent(high_seat)];
    if (amount <= other)
      return;
    const Chips excess = amount - other;
    stack[high_seat] += excess;
    returned[high_seat] += excess;
    committed[high_seat] -= excess;
    // `all_in` is deliberately NOT re-derived. A refund returns an amount nobody
    // matched, and a seat that went all in had no such amount: it committed
    // every chip it had. Re-deriving here would revive it, and the shipped
    // heads-up rule does the opposite -- a capped big blind returns the part
    // nobody matched and still never acts again
    // (`engine/tests/test_heads_up_preflop.cpp:229`: "The board runs out with no
    // action at any street"). The difference is one chip of stack and a whole
    // extra betting round, so it is a rules divergence, not a rounding detail.
  }

  void close_street() {
    refund_unmatched();
    pending = {false, false};
    phase = street == Street::River ? Phase::Showdown : Phase::Deal;
  }
};

Ref ref_start(const GameDef& def) {
  Ref s;
  s.big_blind = def.big_blind;
  s.last_full_raise = def.big_blind;
  s.preflop_root = def.preflop;
  s.button = def.button;
  // Every seat may raise at the start of a hand. `std::array<bool,2>` value
  // initializes to false, so leaving this out gives the ledger a state where
  // nobody can ever raise, which reads as an implementation bug and is not.
  s.raise_rights = {true, true};
  for (int p = 0; p < 2; ++p) {
    s.stack[p] = def.stacks[static_cast<std::size_t>(p)];
    s.contributed[p] = def.contributions[static_cast<std::size_t>(p)];
  }
  if (def.preflop) {
    if (def.ante > 0) {
      for (int p = 0; p < 2; ++p) {
        const Chips posted = std::min(def.ante, s.stack[p]);
        s.stack[p] -= posted;
        s.contributed[p] += posted;
        s.all_in[p] = s.stack[p] == 0;
      }
    }
    for (int p = 0; p < 2; ++p) {
      const Chips posted = def.blinds_posted[static_cast<std::size_t>(p)];
      s.stack[p] -= posted;
      s.committed[p] += posted;
      s.contributed[p] += posted;
      s.all_in[p] = s.stack[p] == 0;
    }
    s.street = Street::Preflop;
    s.actor = static_cast<int>(s.small_blind_seat());
    for (int p = 0; p < 2; ++p)
      s.pending[p] = !s.folded[p] && !s.all_in[p];
  } else {
    s.board_size = def.board_size;
    std::copy_n(def.board.begin(), s.board_size, s.board.begin());
    s.street = Street::Flop;
    s.actor = static_cast<int>((def.button + 1) % 2);
    for (int p = 0; p < 2; ++p) {
      s.all_in[p] = s.stack[p] == 0;
      s.pending[p] = !s.folded[p] && !s.all_in[p];
    }
  }
  s.dead_pot = def.pot;
  if (s.actionable_count() < 2) {
    s.close_street();
    s.actor = -1;
  }
  return s;
}

LegalActions ref_legal(const Ref& s) {
  LegalActions result;
  if (s.phase != Phase::Action || s.actor < 0)
    return result;
  const int hero = s.actor;
  const int other = hero == 0 ? 1 : 0;
  const Chips high = s.high();
  const Chips due = high - s.committed[hero];
  result.fold = due > 0;
  result.check = due == 0;
  result.call = due > 0;
  result.call_amount = std::min(due, s.stack[hero]);
  if (!s.raise_rights[hero] || s.stack[other] == 0 || s.stack[hero] <= due)
    return result;
  const Chips maximum = s.committed[hero] + s.stack[hero];
  const Chips full_minimum = high + s.last_full_raise;
  result.aggressive =
      TargetRange{due == 0 ? ActionType::Bet : ActionType::Raise, std::min(full_minimum, maximum),
                  maximum, maximum <= full_minimum};
  return result;
}

Ref ref_act(const Ref& s, int player, Action action) {
  Ref next = s;
  const int other = next.opponent(player);
  const Chips high = next.high();
  next.pending[player] = false;
  next.raise_rights[player] = false;
  switch (action.type) {
    case ActionType::Fold:
      next.folded[player] = true;
      next.all_in[player] = false;
      next.refund_unmatched();
      next.pending = {false, false};
      next.raise_rights = {false, false};
      next.phase = Phase::Folded;
      next.actor = -1;
      return next;
    case ActionType::Check:
      break;
    case ActionType::Call:
      next.pay(player, std::min(high - next.committed[player], next.stack[player]));
      break;
    case ActionType::Bet:
    case ActionType::Raise: {
      const Chips increment = action.target_total - high;
      next.pay(player, action.target_total - next.committed[player]);
      if (!next.folded[other] && !next.all_in[other])
        next.pending[other] = true;
      if (increment >= next.last_full_raise) {
        next.last_full_raise = increment;
        if (!next.folded[other] && !next.all_in[other])
          next.raise_rights[other] = true;
      }
      break;
    }
  }
  next.history += 1;
  for (int p = 0; p < 2; ++p)
    next.all_in[p] = next.stack[p] == 0;
  if (next.phase != Phase::Action)
    return next;
  if (next.live_count() <= 1) {
    next.phase = Phase::Folded;
    next.refund_unmatched();
    next.pending = {false, false};
    next.actor = -1;
    return next;
  }
  if (!next.actionable(0) && !next.actionable(1)) {
    next.close_street();
    next.actor = -1;
    return next;
  }
  if (next.actionable(other)) {
    next.actor = other;
  } else {
    next.close_street();
    next.actor = -1;
  }
  return next;
}

Ref ref_deal(const Ref& s, int public_card) {
  Ref next = s;
  next.board[next.board_size++] = public_card;
  const int dealt = next.board_size;
  next.street = dealt <= 3 ? Street::Flop : (dealt == 4 ? Street::Turn : Street::River);
  if (dealt < 3)
    return next;
  for (int p = 0; p < 2; ++p) {
    next.committed[p] = 0;
    next.raise_rights[p] = true;
    next.pending[p] = !next.folded[p] && !next.all_in[p];
  }
  next.last_full_raise = next.big_blind;
  // ONE all-in seat already ends the betting at two seats, because there is
  // nobody left to bet against. That is the shipped heads-up rule: `after_card`
  // closes the street the moment EITHER seat is all in (engine/src/poker/
  // heads_up.cpp:271). It is NOT "neither seat is pending" -- that test is false
  // for an all-in seat, so it would open a betting round in which the sole
  // remaining seat is free to bet into an opponent who cannot respond.
  //
  // The general N-seat form of the same rule is "fewer than two seats can act",
  // which is what the unified state asks through `actionable_count()`. At two
  // seats the two statements are identical, and this ledger states the two-seat
  // one because it is the one the heads-up reference actually implements.
  if (next.all_in[0] || next.all_in[1]) {
    next.phase = next.street == Street::River ? Phase::Showdown : Phase::Deal;
    next.actor = -1;
    return next;
  }
  next.phase = Phase::Action;
  next.actor = static_cast<int>((next.button + 1) % 2);
  return next;
}

// ---------------------------------------------------------------------------
// Field-by-field comparison. Every assertion here is a place an implementation
// defect could hide, so the sweep is exhaustive rather than sampled.
// ---------------------------------------------------------------------------
struct Counts {
  long nodes = 0, folds = 0, showdowns = 0, refunds = 0, short_raises = 0, calls = 0;
  long option_held = 0, flops = 0, preflop_roots = 0;
  long refunded_all_in = 0;       // a seat holds chips AND is all in (refund on a capped blind)
  long probe_skipped = 0;         // the M4 probe refused to index an actorless ledger
  long probe_bad_actor = 0;       // the M4 probe was handed an action phase with no actor
  long folded_high = 0;           // M3: a folded seat holds the strict max commitment
  long rr_false = 0;              // M4: the actor holds no raise rights
  long rr_false_would_raise = 0;  // M4: ...and every other guard would allow a raise
  long node_hash = 0;             // fingerprint that distinguishes the M9 patch site
};

int compare(const GameState& got, const Ref& want, Counts& counts) {
  {
    // Reachability probes. A branch the walk never reaches makes every assertion
    // inside it vacuous, so a mutation that only changes that branch survives for
    // a reason that has nothing to do with implementation correctness.
    // M3 probe: the folded-skip guard can only change a refund when a FOLDED
    // seat holds the strict maximum commitment, because that is the only way the
    // guard moves which seat `refund_unmatched` picks.
    bool folded_high = false;
    for (int a = 0; a < 2; ++a)
      if (want.folded[a])
        for (int b = 0; b < 2; ++b)
          if (want.committed[a] > want.committed[b])
            folded_high = true;
    counts.folded_high += folded_high ? 1 : 0;
    // The state that separates "re-derive all_in after a refund" from "preserve
    // it": a seat with chips behind that is nonetheless all in. Reaching it
    // requires a blind capped at its poster's stack, so this counter is also the
    // proof that the preflop sweep covers the shipped short-big-blind shape.
    for (int p = 0; p < 2; ++p)
      if (!want.folded[p] && want.all_in[p] && want.stack[p] > 0)
        counts.refunded_all_in += 1;
    // M4 probe: the mutation can only show up when the actor lacks raise rights
    // AND every OTHER guard in `legal()` would still let a raise through.
    //
    // The guard below tests the LEDGER's phase and actor, not the state's, and
    // it tests both. An earlier version tested `got.phase()` and then indexed
    // `want` with `want.actor`, which is out of bounds whenever the two disagree
    // about which seat holds the action -- `committed[-1]` and `stack[2]` on a
    // two-element `std::array`. This probe runs BEFORE the phase comparison, so
    // a divergence reached it. `std::array` is a plain aggregate, so the
    // sanitizers do not annotate it and the reads went unreported; an
    // independent reviewer found it by reading. Indexing a container with a
    // value that is only valid in a state you have not yet checked is the whole
    // defect, and the fix is to check the state first.
    // Indexing `want` is only legal once `want` says a seat holds the action, so
    // that is checked FIRST and the illegal case is COUNTED rather than skipped
    // quietly. A silent skip would hide the very divergence this probe exists
    // near: `committed[-1]` and `stack[2]` on a two-element `std::array` are
    // undefined-but-unreported, because `std::array` is a plain aggregate that
    // the sanitizers do not annotate.
    if (want.phase == Phase::Action && want.actor < 0)
      ++counts.probe_bad_actor;
    if (want.phase == Phase::Action && want.actor >= 0) {
      const int h = want.actor;
      const int o = 1 - h;
      const Chips due = want.high() - want.committed[h];
      if (!want.raise_rights[h]) {
        ++counts.rr_false;
        if (want.stack[o] > 0 && want.stack[h] > due)
          ++counts.rr_false_would_raise;
      }
    }
    // A fingerprint over every field the walk can observe, so "this mutation
    // changed no reachable state" is a measurement rather than an argument.
    long long h = counts.node_hash;
    for (int p = 0; p < 2; ++p) {
      const GamePlayer& g = got.players()[static_cast<std::size_t>(p)];
      h = h * 131 + static_cast<long long>(g.stack) * 3 +
          static_cast<long long>(g.street_committed) * 5 +
          static_cast<long long>(g.contributed) * 7 + static_cast<long long>(g.refunded) * 11 +
          (g.folded ? 13 : 0) + (g.all_in ? 17 : 0) + (g.raise_rights ? 19 : 0) +
          (g.pending ? 23 : 0);
    }
    h = h * 131 + static_cast<int>(got.phase()) * 29 + static_cast<int>(got.street()) * 31 +
        static_cast<long long>(got.last_full_raise()) * 37 +
        static_cast<long long>(got.pot()) * 41 +
        (got.actor() ? static_cast<long long>(*got.actor()) + 1 : 0) * 43 +
        static_cast<long long>(got.board().size()) * 47;
    counts.node_hash = h % 1000000007;
  }
  // A zero-stack seat must never hold the action. This is the property that
  // separates a correct all-in representation from the one that let an ante
  // empty a stack and still hand out a free check.
  if (const auto actor = got.actor()) {
    if (got.players()[*actor].stack == 0 && !got.players()[*actor].folded) {
      std::printf("CHECK failed: actor %zu holds the action with stack 0 (%s:%d)\n", *actor,
                  __FILE__, __LINE__);
      return 1;
    }
  }
  if (got.phase() != want.phase || got.street() != want.street)
    goto mismatch;
  if (got.street() != Street::Preflop) {
    const auto board = got.board();
    if (static_cast<int>(board.size()) != want.board_size)
      goto mismatch;
    for (std::size_t i = 0; i < board.size(); ++i)
      if (board[i] != want.board[i])
        goto mismatch;
  }
  if (got.last_full_raise() != want.last_full_raise)
    goto mismatch;
  {
    const auto actor = got.actor();
    const int got_actor = actor ? static_cast<int>(*actor) : -1;
    if (got_actor != (want.phase == Phase::Action ? want.actor : -1))
      goto mismatch;
  }
  for (std::size_t p = 0; p < 2; ++p) {
    const GamePlayer& g = got.players()[p];
    if (g.stack != want.stack[p] || g.street_committed != want.committed[p] ||
        g.contributed != want.contributed[p] || g.refunded != want.returned[p] ||
        g.folded != want.folded[p])
      goto mismatch;
    // The all-in flag is compared against the LEDGER, not against the identity
    // `all_in == (stack == 0)`. That identity looked like the stronger check but
    // was the weaker one: it holds in every reachable state except the one that
    // matters, where a refund on a capped blind leaves the seat holding chips
    // and still all in. Comparing to the ledger is what makes this able to fail.
    if (g.all_in != want.all_in[p])
      goto mismatch;
  }
  {
    Chips total = 0;
    for (std::size_t p = 0; p < 2; ++p)
      total += got.players()[p].net_contributed();
    if (total != got.pot())
      goto mismatch;
  }
  if (got.phase() == Phase::Action) {
    const LegalActions want_legal = ref_legal(want);
    const LegalActions got_legal = got.legal();
    if (got_legal.fold != want_legal.fold || got_legal.check != want_legal.check ||
        got_legal.call != want_legal.call || got_legal.call_amount != want_legal.call_amount)
      goto mismatch;
    if (got_legal.aggressive.has_value() != want_legal.aggressive.has_value())
      goto mismatch;
    if (want_legal.aggressive) {
      if (got_legal.aggressive->type != want_legal.aggressive->type ||
          got_legal.aggressive->minimum != want_legal.aggressive->minimum ||
          got_legal.aggressive->maximum != want_legal.aggressive->maximum ||
          got_legal.aggressive->all_in_only != want_legal.aggressive->all_in_only)
        goto mismatch;
    }
    // The full legality sweep: every action type against every target, so a
    // `contains` that accepts the wrong thing is caught rather than sampled.
    const Chips ceiling = want.stack[static_cast<std::size_t>(want.actor)];
    for (int t = 0; t < 5; ++t) {
      const auto type = static_cast<ActionType>(t);
      for (Chips target = 0; target <= ceiling + 1; ++target) {
        const Action probe{type, target};
        if (got_legal.contains(probe) != ref_legal(want).contains(probe))
          goto mismatch;
      }
    }
  }
  return 0;

mismatch:
  std::printf(
      "MISMATCH (%s:%d)\n  phase got=%d want=%d | street got=%d want=%d | "
      "stack got=[%llu,%llu] want=[%llu,%llu] | committed got=[%llu,%llu] want=[%llu,%llu] | "
      "all_in got=[%d,%d] want=[%d,%d] | actor=%d want=%d\n"
      "  last_full_raise got=%llu want=%llu | pot got=%llu want=%llu | board got=%zu want=%d\n"
      "  legal got={f%d c%d k%d amt=%llu agg=%d} want={f%d c%d k%d amt=%llu agg=%d}\n",
      __FILE__, __LINE__, static_cast<int>(got.phase()), static_cast<int>(want.phase),
      static_cast<int>(got.street()), static_cast<int>(want.street),
      static_cast<unsigned long long>(got.players()[0].stack),
      static_cast<unsigned long long>(got.players()[1].stack),
      static_cast<unsigned long long>(want.stack[0]),
      static_cast<unsigned long long>(want.stack[1]),
      static_cast<unsigned long long>(got.players()[0].street_committed),
      static_cast<unsigned long long>(got.players()[1].street_committed),
      static_cast<unsigned long long>(want.committed[0]),
      static_cast<unsigned long long>(want.committed[1]), static_cast<int>(got.players()[0].all_in),
      static_cast<int>(got.players()[1].all_in), static_cast<int>(want.all_in[0]),
      static_cast<int>(want.all_in[1]), got.actor() ? static_cast<int>(*got.actor()) : -1,
      want.actor, static_cast<unsigned long long>(got.last_full_raise()),
      static_cast<unsigned long long>(want.last_full_raise),
      static_cast<unsigned long long>(got.pot()),
      static_cast<unsigned long long>(want.contributed[0] + want.contributed[1] - want.returned[0] -
                                      want.returned[1]),
      got.board().size(), want.board_size, static_cast<int>(got.legal().fold),
      static_cast<int>(got.legal().check), static_cast<int>(got.legal().call),
      static_cast<unsigned long long>(got.legal().call_amount),
      got.legal().aggressive.has_value() ? 1 : 0, static_cast<int>(ref_legal(want).fold),
      static_cast<int>(ref_legal(want).check), static_cast<int>(ref_legal(want).call),
      static_cast<unsigned long long>(ref_legal(want).call_amount),
      ref_legal(want).aggressive.has_value() ? 1 : 0);
  static_cast<void>(counts);
  return 1;
}

// ---------------------------------------------------------------------------
// The exhaustive walk. It drives GameState, never HeadsUpState.
// ---------------------------------------------------------------------------
int walk(const GameState& state, const Ref& ref, Counts& counts, int depth = 0) {
  if (depth > 40)
    return 0;  // the small-stack sweeps terminate long before this
  ++counts.nodes;
  CHECK(compare(state, ref, counts) == 0);

  if (state.phase() == Phase::Folded) {
    ++counts.folds;
    return 0;
  }
  if (state.phase() == Phase::Showdown) {
    ++counts.showdowns;
    return 0;
  }
  if (state.phase() == Phase::Deal) {
    if (ref.board_size == 0)
      ++counts.flops;
    static const char* names[] = {"5c", "6c", "7c", "8c", "9c"};
    std::array<bool, 52> used{};
    for (int i = 0; i < ref.board_size; ++i)
      used[ref.board[i]] = true;
    for (const char* name : names) {
      const int c = card(name);
      if (used[c])
        continue;
      CHECK(walk(state.after_card(c), ref_deal(ref, c), counts, depth + 1) == 0);
    }
    return 0;
  }

  const auto actor = state.actor();
  CHECK(actor.has_value());
  const int player = static_cast<int>(*actor);
  const LegalActions legal = state.legal();
  const LegalActions want_legal = ref_legal(ref);
  std::vector<Action> options;
  if (want_legal.fold)
    options.push_back({ActionType::Fold, 0});
  if (want_legal.check)
    options.push_back({ActionType::Check, 0});
  if (want_legal.call) {
    options.push_back({ActionType::Call, 0});
    ++counts.calls;
    // The preflop option: a call by the seat left of the big blind does not
    // close the street while the big blind still owes an action.
    if (ref.street == Street::Preflop && ref.preflop_root && ref.dead_pot > 0 &&
        !ref.pending[ref.opponent(player)])
      ++counts.option_held;
  }
  if (want_legal.aggressive) {
    options.push_back({want_legal.aggressive->type, want_legal.aggressive->minimum});
    const Chips bump = want_legal.aggressive->minimum;
    if (bump < want_legal.aggressive->maximum)
      options.push_back({want_legal.aggressive->type, want_legal.aggressive->maximum});
    if (bump < ref.last_full_raise + ref.high())
      ++counts.short_raises;
  }
  for (const Action& action : options) {
    CHECK(legal.contains(action));
    CHECK(walk(state.after_action(*actor, action), ref_act(ref, player, action), counts,
               depth + 1) == 0);
  }
  return 0;
}

// ---------------------------------------------------------------------------
// Settlement equivalence. The ledger computes the same contribution-layer
// result independently: layers are the distinct contribution levels, each
// contested by the seats that reached it.
// ---------------------------------------------------------------------------
struct RefSettle {
  std::array<Chips, 2> awards{}, refunds{};
  Chips pot = 0;
};

RefSettle ref_settle(const Ref& s, const std::array<std::optional<std::uint32_t>, 2>& scores) {
  RefSettle out;
  // Layered award: every distinct level, lowest first.
  std::vector<Chips> levels;
  for (int p = 0; p < 2; ++p) {
    const Chips net = s.contributed[p] - s.returned[p];
    if (net > 0)
      levels.push_back(net);
  }
  std::sort(levels.begin(), levels.end());
  levels.erase(std::unique(levels.begin(), levels.end()), levels.end());
  Chips previous = 0;
  for (Chips level : levels) {
    std::vector<int> eligible;
    int contributors = 0;
    for (int p = 0; p < 2; ++p) {
      if (s.contributed[p] - s.returned[p] >= level) {
        ++contributors;
        if (!s.folded[p])
          eligible.push_back(p);
      }
    }
    const Chips amount = (level - previous) * static_cast<Chips>(contributors);
    previous = level;
    if (eligible.empty())
      continue;
    if (eligible.size() == 1) {
      out.awards[eligible[0]] += amount;
      continue;
    }
    // Odd chips go clockwise from the button, so the seat closer to the button
    // takes the remainder.
    std::uint32_t best = 0;
    std::array<bool, 2> is_best{};
    for (int p : eligible) {
      if (scores[p] && *scores[p] > best) {
        best = *scores[p];
        is_best = {false, false};
      }
      if (scores[p] && *scores[p] == best)
        is_best[p] = true;
    }
    std::vector<int> winners;
    for (int p : eligible)
      if (is_best[p])
        winners.push_back(p);
    const Chips share = amount / static_cast<Chips>(winners.size());
    Chips remainder = amount - share * static_cast<Chips>(winners.size());
    for (int p : winners)
      out.awards[p] += share;
    // The remainder goes to the winner nearest the button, clockwise.
    if (remainder > 0) {
      int nearest = winners[0];
      Chips best_distance = 0;
      bool first = true;
      for (int p : winners) {
        const Chips distance = static_cast<Chips>((static_cast<std::size_t>(p) + 2 - s.button) % 2);
        if (first || distance < best_distance) {
          best_distance = distance;
          nearest = p;
          first = false;
        }
      }
      out.awards[nearest] += remainder;
    }
  }
  for (int p = 0; p < 2; ++p)
    out.refunds[p] = s.returned[p];
  out.pot = s.contributed[0] - s.returned[0] + s.contributed[1] - s.returned[1];
  return out;
}

int compare_settlement(const GameState& state, const Ref& ref, bool showdown, std::array<int, 2> h0,
                       std::array<int, 2> h1) {
  std::array<std::optional<std::uint32_t>, 2> scores;
  std::vector<std::array<int, 2>> holes;
  if (showdown) {
    // Score each seat from its OWN seven cards. Sharing one buffer would give
    // both seats the second seat's hand, which is exactly the defect a
    // settlement comparison exists to catch.
    for (int p = 0; p < 2; ++p) {
      std::array<int, 7> seven{};
      std::copy_n(ref.board.begin(), 5, seven.begin());
      const std::array<int, 2>& hand = p == 0 ? h0 : h1;
      std::copy(hand.begin(), hand.end(), seven.begin() + 5);
      scores[static_cast<std::size_t>(p)] = bs::evaluate(seven.data(), 7).score;
    }
    holes = {h0, h1};
  }
  const ContributionSettlement got = showdown ? state.settle_showdown(holes) : state.settle_fold();
  const RefSettle want = ref_settle(ref, scores);
  Chips total_awards = 0;
  for (std::size_t p = 0; p < 2; ++p) {
    total_awards += got.awards[p];
    if (got.awards[p] != want.awards[p]) {
      std::printf("SETTLE MISMATCH (%s:%d) award[%zu] got=%llu want=%llu\n", __FILE__, __LINE__, p,
                  static_cast<unsigned long long>(got.awards[p]),
                  static_cast<unsigned long long>(want.awards[p]));
      return 1;
    }
    if (got.refunds[p] != want.refunds[p]) {
      std::printf("SETTLE MISMATCH (%s:%d) refund[%zu] got=%llu want=%llu\n", __FILE__, __LINE__, p,
                  static_cast<unsigned long long>(got.refunds[p]),
                  static_cast<unsigned long long>(want.refunds[p]));
      return 1;
    }
  }
  // Conservation: awards plus refunds equal exactly what the seats put in.
  const Chips put_in = ref.contributed[0] + ref.contributed[1];
  if (total_awards + got.total_refunds != put_in) {
    std::printf("CONSERVATION (%s:%d) awards=%llu refunds=%llu put_in=%llu\n", __FILE__, __LINE__,
                static_cast<unsigned long long>(total_awards),
                static_cast<unsigned long long>(got.total_refunds),
                static_cast<unsigned long long>(put_in));
    return 1;
  }
  return 0;
}

// ---------------------------------------------------------------------------
// Cases
// ---------------------------------------------------------------------------

// The acceptance criterion: an exhaustive drive of every root the flop-rooted
// profile can start from, at small stacks so the tree closes.
int case_flop_rooted_sweep() {
  Counts counts;
  for (Chips blind = 1; blind <= 3; ++blind) {
    for (Chips first = 0; first <= 4; ++first) {
      for (Chips second = 0; second <= 4; ++second) {
        for (std::size_t button = 0; button < 2; ++button) {
          GameDef def{};
          def.player_count = 2;
          def.button = button;
          def.big_blind = blind;
          def.stacks = {first + blind, second + blind, 0, 0, 0, 0, 0, 0, 0, 0};
          def.contributions = {blind, blind, 0, 0, 0, 0, 0, 0, 0, 0};
          def.pot = 2 * blind;
          def.board = {card("2h"), card("3h"), card("4h"), 0, 0};
          def.board_size = 3;
          def.preflop = false;
          const GameState state(def);
          CHECK(walk(state, ref_start(def), counts) == 0);
        }
      }
    }
  }
  std::printf(
      "  flop sweep nodes=%ld folds=%ld showdowns=%ld folded_high=%ld rr_false=%ld "
      "rr_would_raise=%ld hash=%ld\n",
      counts.nodes, counts.folds, counts.showdowns, counts.folded_high, counts.rr_false,
      counts.rr_false_would_raise, counts.node_hash);
  std::printf("  flop sweep refunded_all_in=%ld probe_bad_actor=%ld\n", counts.refunded_all_in,
              counts.probe_bad_actor);
  std::printf("  flop sweep nodes=%ld folds=%ld showdowns=%ld\n", counts.nodes, counts.folds,
              counts.showdowns);
  CHECK(counts.nodes > 1000);
  CHECK(counts.folds > 0);
  CHECK(counts.showdowns > 0);

  // --- Non-vacuity pins, each a measured equivalent-mutant result ------------
  //
  // Mutation testing this suite (2026-09-19) left exactly two survivors, and both
  // are EQUIVALENT at two seats. They are pinned here rather than merely argued
  // away: if a future fixture reaches one of these states, the pin fires and the
  // mutation testing has to be redone on a suite whose coverage actually changed.
  // Silence would let the same mutations start passing for a new reason.
  //
  // (a) `refund_unmatched` skipping folded seats when it finds the high
  //     commitment. The guard can only matter when a FOLDED seat holds the
  //     strict maximum, and at two seats that is structurally impossible: a seat
  //     folds facing a bet, so its commitment is strictly below the bettor's, and
  //     the only other chip movement, a refund, returns chips by LOWERING a
  //     commitment. So the guard is unreachable and the mutant is equivalent.
  CHECK(counts.folded_high == 0);
  //
  // (b) `legal()` ignoring `raise_rights`. `rr_false_would_raise` counts nodes
  //     where the actor holds NO raise rights AND every other guard in `legal()`
  //     would still allow a raise -- the only states where the two versions
  //     differ. Zero across every reachable node, so the mutant is equivalent.
  //     The states are genuinely EXERCISED (`rr_false > 0`), which is what keeps
  //     this from being a vacuous claim about an unreached branch.
  CHECK(counts.rr_false > 0);
  CHECK(counts.rr_false_would_raise == 0);
  // The M4 probe indexes the ledger, so it must never be handed an action phase
  // with no actor. Asserted rather than assumed because the alternative is an
  // out-of-bounds read that `std::array` lets through unreported.
  CHECK(counts.probe_bad_actor == 0);
  // The fingerprint is not decoration: it is what makes "this mutation changed no
  // reachable state" a measurement rather than an argument. It hashes every field
  // the walk can observe, including `all_in` itself.
  CHECK(counts.node_hash != 0);
  return 0;
}

// The preflop profile. This is the sweep the reference does NOT have, and it is
// where the big-blind option and the blind-posting path are actually reachable.
// Without it those rules are taken on faith.
int case_preflop_sweep() {
  Counts counts;
  for (Chips blind = 2; blind <= 4; blind += 2) {
    // ASYMMETRIC stacks, both seats swept independently. Symmetric stacks are
    // not enough and never were: the shipped short-big-blind fixture is
    // `{2, 10}`, and the rule it pins -- a blind capped at its poster's stack
    // makes that seat all in for the rest of the hand -- is only reachable when
    // one seat is short and the other is not. An independent reviewer found the
    // implementation got that rule wrong while this sweep, which used one shared
    // stack value, could not see it. The cost is a few hundred thousand extra
    // nodes; the alternative is a suite that certifies a rule it never tests.
    for (Chips first = blind; first <= blind + 5; ++first) {
      for (Chips second = blind; second <= blind + 5; ++second) {
        for (std::size_t button = 0; button < 2; ++button) {
          GameDef def{};
          def.player_count = 2;
          def.button = button;
          def.big_blind = blind;
          const Chips small = blind / 2;
          def.stacks = {first, second, 0, 0, 0, 0, 0, 0, 0, 0};
          def.contributions = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
          def.pot = blind;
          def.board = {-1, -1, -1, 0, 0};
          def.board_size = 0;
          def.preflop = true;
          def.blinds_posted = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
          def.blinds_posted[button] = small;
          def.blinds_posted[1 - button] = blind;
          const GameState state(def);
          ++counts.preflop_roots;
          CHECK(walk(state, ref_start(def), counts) == 0);
        }
      }
    }
  }
  std::printf("  preflop sweep roots=%ld nodes=%ld folds=%ld option_held=%ld refunded_all_in=%ld\n",
              counts.preflop_roots, counts.nodes, counts.folds, counts.option_held,
              counts.refunded_all_in);
  CHECK(counts.preflop_roots > 0);
  CHECK(counts.nodes > 100);
  // The option is the reason this sweep exists, so its reachability is asserted
  // rather than assumed.
  CHECK(counts.option_held > 0);
  // The refunded-but-still-all-in state, which is what a capped blind creates and
  // what the implementation got wrong. Asserted here, in the sweep whose fixture
  // changes reach it, so the coverage cannot quietly lapse.
  CHECK(counts.refunded_all_in > 0);
  return 0;
}

// A rooted flop where FEWER THAN TWO seats can act, so the constructor must close
// the street instead of opening a round. Mutation testing found the settlement
// sweep never reached this: its stacks started at 4, and this branch needs an
// empty stack. Both reachable shapes are covered, because the branch tests
// `actionable_count() < 2` and satisfies that two different ways:
//   - one seat empty: it committed its whole stack on an earlier street and the
//     hand is rooted on the flop with the other seat still holding chips;
//   - both seats empty: the hand was fully committed before the flop.
// A seat with chips behind is not required, because a rooted root declares its
// contributions and pot directly rather than posting blinds.
int case_rooted_flop_with_nobody_able_to_act() {
  const std::array<int, 2> strong = {card("Ad"), card("Kd")};
  const std::array<int, 2> weak = {card("Ah"), card("Kc")};
  const Chips stack_sets[2][2] = {{0, 3}, {0, 0}};
  for (std::size_t button = 0; button < 2; ++button) {
    for (const auto& stacks : stack_sets) {
      GameDef def{};
      def.player_count = 2;
      def.button = button;
      def.big_blind = 2;
      def.stacks = {stacks[0], stacks[1], 0, 0, 0, 0, 0, 0, 0, 0};
      def.contributions = {2, 2, 0, 0, 0, 0, 0, 0, 0, 0};
      def.pot = 4;
      def.board = {card("2h"), card("7d"), card("Qs"), 0, 0};
      def.board_size = 3;
      def.preflop = false;
      const GameState state(def);
      const Ref ref = ref_start(def);
      Counts counts;
      CHECK(compare(state, ref, counts) == 0);
      CHECK(!state.actor().has_value());
      CHECK(state.phase() == Phase::Deal);
      CHECK(state.players()[0].all_in == (state.players()[0].stack == 0));
      CHECK(state.players()[1].all_in == (state.players()[1].stack == 0));
      // The whole board runs out with no further betting, and the hand settles.
      // Distinct cards, because `after_card` rejects a card already on the board.
      static const char* turn_and_river[] = {"5c", "6c"};
      GameState runout = state;
      for (const char* name : turn_and_river) {
        CHECK(runout.phase() == Phase::Deal);
        runout = runout.after_card(card(name));
      }
      CHECK(runout.board().size() == 5);
      CHECK(runout.phase() == Phase::Showdown);
      CHECK(compare_settlement(runout, ref, true, strong, weak) == 0);
    }
  }
  return 0;
}

// Settlement on real terminal positions, plus conservation.
int case_settlement_and_conservation() {
  // Hole cards chosen so the two seats' SEVEN-CARD SCORES DIFFER, which is what
  // makes the settlement comparison able to test hand evaluation at all. The
  // earlier fixture paired `Ad Kd` with `Ah Kc` on a board whose runout was
  // `5c 6c 7c 8c 9c`, so both seats played the board and always tied; mutation
  // testing showed the score comparison was therefore never exercised, because
  // awarding the pot to the first live seat won exactly as many pots as
  // evaluating the hands did. The runout also ended in a board straight flush,
  // which is not a board either seat's hidden cards can be excluded from once
  // real evaluation runs.
  const std::array<int, 2> strong = {card("Ad"), card("Kd")};
  const std::array<int, 2> weak = {card("Ac"), card("9c")};
  // A zero-stack seat is the CASE, not an edge case: a closed street that cannot
  // be acted on is exactly how a short stack reaches the flop with nothing
  // behind, and it is the state where the flop constructor must close the street
  // instead of opening a round. Mutation testing found this range started at 4,
  // so the "nobody can act at a rooted flop" path was never reached at all and a
  // defect in it was invisible here. `stack = 0` is legal: the root declares the
  // contributions and pot directly, so a seat with chips behind is not required.
  for (Chips stack = 0; stack <= 12; ++stack) {
    for (std::size_t button = 0; button < 2; ++button) {
      GameDef def{};
      def.player_count = 2;
      def.button = button;
      def.big_blind = 2;
      def.stacks = {stack, stack, 0, 0, 0, 0, 0, 0, 0, 0};
      def.contributions = {2, 2, 0, 0, 0, 0, 0, 0, 0, 0};
      def.pot = 4;
      def.board = {card("2h"), card("7d"), card("Qs"), 0, 0};
      def.board_size = 3;
      def.preflop = false;
      GameState state(def);
      Ref ref = ref_start(def);
      int guard = 0;
      while (state.phase() == Phase::Action && guard++ < 12) {
        const auto actor = state.actor();
        CHECK(actor.has_value());
        const LegalActions legal = state.legal();
        const Action choice =
            legal.check ? Action{ActionType::Check, 0} : Action{ActionType::Call, 0};
        CHECK(legal.contains(choice));
        state = state.after_action(*actor, choice);
        ref = ref_act(ref, static_cast<int>(*actor), choice);
      }
      int dealt = ref.board_size;
      while (state.phase() == Phase::Deal && dealt < 5) {
        static const char* names[] = {"5c", "6c", "7c", "Tc", "Jh"};
        const int c = card(names[dealt]);
        state = state.after_card(c);
        ref = ref_deal(ref, c);
        ++dealt;
      }
      if (state.phase() != Phase::Showdown)
        continue;
      CHECK(compare_settlement(state, ref, true, strong, weak) == 0);
    }
  }
  // A fold ends the hand and must settle with nothing uncalled left behind.
  for (Chips stack = 4; stack <= 12; ++stack) {
    GameDef def{};
    def.player_count = 2;
    def.button = 0;
    def.big_blind = 2;
    def.stacks = {stack, stack, 0, 0, 0, 0, 0, 0, 0, 0};
    def.contributions = {2, 2, 0, 0, 0, 0, 0, 0, 0, 0};
    def.pot = 4;
    def.board = {card("2h"), card("7d"), card("Qs"), 0, 0};
    def.board_size = 3;
    def.preflop = false;
    GameState state(def);
    Ref ref = ref_start(def);
    const auto actor = state.actor();
    CHECK(actor.has_value());
    const LegalActions legal = state.legal();
    if (!legal.aggressive)
      continue;
    // A large bet, then a fold: the bettor must get the uncalled part back.
    const Action bet{legal.aggressive->type, legal.aggressive->maximum};
    state = state.after_action(*actor, bet);
    ref = ref_act(ref, static_cast<int>(*actor), bet);
    const auto responder = state.actor();
    CHECK(responder.has_value());
    const Action fold{ActionType::Fold, 0};
    state = state.after_action(*responder, fold);
    ref = ref_act(ref, static_cast<int>(*responder), fold);
    CHECK(state.phase() == Phase::Folded);
    CHECK(compare_settlement(state, ref, false, strong, weak) == 0);
    // The uncalled excess went back, so the winner holds exactly what the
    // opponent reached.
    CHECK(ref.committed[static_cast<std::size_t>(*actor)] ==
          ref.committed[static_cast<std::size_t>(*responder)]);
  }
  return 0;
}

// Constructor rejections, including the seat-count gate that keeps stage 1 from
// being reached at three seats before the seat-count stage lands.
int case_construction_rejections() {
  const auto rejects = [](GameDef def) {
    try {
      const GameState state(def);
      static_cast<void>(state);
    } catch (const std::invalid_argument&) {
      return true;
    } catch (const std::overflow_error&) {
      return true;
    }
    return false;
  };

  GameDef base{};
  base.player_count = 2;
  base.button = 1;
  base.big_blind = 2;
  base.stacks = {20, 20, 0, 0, 0, 0, 0, 0, 0, 0};
  base.contributions = {2, 2, 0, 0, 0, 0, 0, 0, 0, 0};
  base.pot = 4;
  base.board = {card("2h"), card("3h"), card("4h"), 0, 0};
  base.board_size = 3;
  {
    const GameState ok(base);
    CHECK(ok.phase() != Phase::Folded);
  }
  {
    // Three seats construct under the multiway profile as of RFC 0008
    // stage 2; a rooted board carries dead money at every seat.
    GameDef def = base;
    def.player_count = 3;
    def.stacks = {20, 20, 20, 0, 0, 0, 0, 0, 0, 0};
    def.contributions = {2, 2, 2, 0, 0, 0, 0, 0, 0, 0};
    def.pot = 6;
    const GameState widened(def);
    CHECK(widened.player_count() == 3);
    CHECK(widened.actor() == 2);
  }
  {
    // Eleven seats remain past the 10-seat bound.
    GameDef def = base;
    def.player_count = 11;
    CHECK(rejects(def));
  }
  {
    GameDef def = base;
    def.player_count = 1;
    CHECK(rejects(def));
  }
  {
    GameDef def = base;
    def.board_size = 2;
    CHECK(rejects(def));
  }
  {
    GameDef def = base;
    def.board_size = 4;
    CHECK(rejects(def));
  }
  {
    GameDef def = base;
    def.board = {card("2h"), card("2h"), card("4h"), 0, 0};  // duplicate
    CHECK(rejects(def));
  }
  {
    GameDef def = base;
    def.pot = 5;  // a rooted board must reconcile exactly
    CHECK(rejects(def));
  }
  {
    GameDef def = base;
    def.button = 2;
    CHECK(rejects(def));
  }
  {
    GameDef def = base;
    def.big_blind = 0;
    CHECK(rejects(def));
  }
  {
    GameDef def = base;
    def.contributions = {1, 3, 0, 0, 0, 0, 0, 0, 0, 0};  // unmatched
    CHECK(rejects(def));
  }
  {
    GameDef def = base;
    def.terminal = TerminalDepth::Flop;
    CHECK(rejects(def));  // RFC 0007's stage, not this one
  }
  return 0;
}

// An ante that empties a stack leaves that seat unable to act. The two-seat
// profile has no ante, so this asserts the rejection instead; the general form
// is pinned by the guard suite against the multiway type.
int case_ante_is_rejected_in_this_profile() {
  GameDef def{};
  def.player_count = 2;
  def.button = 1;
  def.big_blind = 2;
  def.ante = 10;
  def.stacks = {10, 10, 0, 0, 0, 0, 0, 0, 0, 0};
  def.contributions = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
  def.pot = 2;
  def.board = {-1, -1, -1, 0, 0};
  def.board_size = 0;
  def.preflop = true;
  def.blinds_posted = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
  def.blinds_posted[1] = 1;
  def.blinds_posted[0] = 2;
  bool rejected = false;
  try {
    const GameState state(def);
    static_cast<void>(state);
  } catch (const std::invalid_argument&) {
    rejected = true;
  }
  // The two-seat profile has no ante, so `validate` refuses one whose sum would
  // not fit either stack, which this one does not.
  CHECK(rejected);
  return 0;
}

}  // namespace

int main() {
  struct Case {
    const char* name;
    int (*fn)();
  };
  const std::array<Case, 6> cases = {{
      {"flop_rooted_sweep", case_flop_rooted_sweep},
      {"preflop_sweep", case_preflop_sweep},
      {"rooted_flop_nobody_able_to_act", case_rooted_flop_with_nobody_able_to_act},
      {"settlement_and_conservation", case_settlement_and_conservation},
      {"construction_rejections", case_construction_rejections},
      {"ante_is_rejected_in_this_profile", case_ante_is_rejected_in_this_profile},
  }};
  int failures = 0;
  for (const Case& item : cases) {
    std::printf("case %s ...\n", item.name);
    std::fflush(stdout);
    const int result = item.fn();
    std::printf("  %s\n", result == 0 ? "ok" : "FAILED");
    failures += result;
  }
  if (failures != 0) {
    std::printf("test_game_definition FAILED\n");
    return 1;
  }
  std::printf("test_game_definition PASS\n");
  return 0;
}
