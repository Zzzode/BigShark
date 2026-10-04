// RFC 0010: N-way equity frontier evaluator tests.
//
// Pins exact equity fractions for analytical scenarios (quads, broadway
// ties, folded-seat exclusion), verifies dead-money settlement, checks the
// cache (repeatability, live-set keying, cap behavior), validates the
// rejection contract, and cross-checks the evaluator against an independently
// coded brute-force oracle on non-trivial 3-way deals.
#include <array>
#include <bs/abstract_tree.hpp>
#include <bs/equity_frontier.hpp>
#include <bs/eval.hpp>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <span>
#include <stdexcept>
#include <vector>

#define CHECK(cond)                                                         \
  do {                                                                      \
    if (!(cond)) {                                                          \
      std::printf("CHECK failed: %s (%s:%d)\n", #cond, __FILE__, __LINE__); \
      return 1;                                                             \
    }                                                                       \
  } while (0)

#define CHECK_CLOSE(a, b, tol)                                                             \
  do {                                                                                     \
    if (std::fabs(static_cast<double>(a) - static_cast<double>(b)) > (tol)) {              \
      std::printf("CHECK_CLOSE failed: %s=%.12f vs %s=%.12f (%s:%d)\n", #a,                \
                  static_cast<double>(a), #b, static_cast<double>(b), __FILE__, __LINE__); \
      return 1;                                                                            \
    }                                                                                      \
  } while (0)

namespace {

using bs::gto::EquityFrontierEvaluator;
using bs::tree::TerminalPayload;

// Card ids: rank*4+suit, 0=2c.
constexpr int c2c = 0, c2d = 1, c2h = 2, c2s = 3;
constexpr int c3c = 4, c3d = 5, c3h = 6;
constexpr int c5h = 14;
constexpr int c7c = 20, c7h = 22;
constexpr int c8c = 24, c8h = 26;
constexpr int c9d = 29, c9s = 31;
constexpr int cTc = 32, cTh = 34;
constexpr int cJd = 37;
constexpr int cQc = 40, cQd = 41, cQh = 42;
constexpr int cKc = 44, cKd = 45, cKh = 46, cKs = 47;
constexpr int cAd = 49, cAh = 50, cAs = 51;

// Builds a ledger with per-seat folded flags and contributed amounts.
TerminalPayload make_ledger(std::size_t n, std::span<const bool> folded,
                            std::span<const std::int64_t> contributed) {
  TerminalPayload tp;
  tp.player_count = n;
  for (std::size_t s = 0; s < n; ++s) {
    tp.seats[s].folded = folded[s];
    tp.seats[s].contributed = contributed[s];
  }
  return tp;
}

// Extracts equity fractions from the evaluator's chip-delta values:
// equity = (value + contributed) / pot.
std::vector<double> equity_from_values(std::span<const double> values,
                                       std::span<const std::int64_t> contributed) {
  double pot = 0;
  for (std::int64_t c : contributed)
    pot += static_cast<double>(c);
  std::vector<double> equity(values.size());
  for (std::size_t s = 0; s < values.size(); ++s)
    equity[s] = (values[s] + static_cast<double>(contributed[s])) / pot;
  return equity;
}

// Independent brute-force oracle: enumerates all turn/river combos and
// aggregates equity with its own loop structure (separate from
// equity_frontier.cpp). Uses the same hand evaluator (bs::evaluate) but
// independently codes card blocking, runout iteration, best-hand finding,
// and tie splitting.
std::vector<double> oracle_equity(std::span<const int> flop,
                                  std::span<const std::array<int, 2>> hands,
                                  const TerminalPayload& ledger) {
  const std::size_t n = hands.size();

  // Block every dealt card (flop + all hole cards, including folded seats).
  std::array<bool, 52> dealt{};
  for (int c : flop)
    dealt[c] = true;
  for (const auto& h : hands) {
    dealt[h[0]] = true;
    dealt[h[1]] = true;
  }
  std::vector<int> pool;
  for (int c = 0; c < 52; ++c)
    if (!dealt[c])
      pool.push_back(c);

  std::vector<double> wins(n, 0.0);
  std::uint64_t runouts = 0;
  for (std::size_t i = 0; i + 1 < pool.size(); ++i) {
    for (std::size_t j = i + 1; j < pool.size(); ++j) {
      // Evaluate each live seat once, track the best score.
      std::array<std::uint32_t, 10> scores{};
      std::uint32_t best = 0;
      for (std::size_t s = 0; s < n; ++s) {
        if (ledger.seats[s].folded)
          continue;
        const int cards[7] = {hands[s][0], hands[s][1], flop[0], flop[1],
                              flop[2],     pool[i],     pool[j]};
        scores[s] = bs::evaluate(cards, 7).score;
        if (scores[s] > best)
          best = scores[s];
      }
      // Split the runout among tied live winners.
      int tied = 0;
      for (std::size_t s = 0; s < n; ++s)
        if (!ledger.seats[s].folded && scores[s] == best)
          ++tied;
      const double share = 1.0 / static_cast<double>(tied);
      for (std::size_t s = 0; s < n; ++s)
        if (!ledger.seats[s].folded && scores[s] == best)
          wins[s] += share;
      ++runouts;
    }
  }
  for (std::size_t s = 0; s < n; ++s)
    wins[s] /= static_cast<double>(runouts);
  return wins;
}

// --- Analytical pins -------------------------------------------------------

// HU: quads vs two overcards on a paired board. Seat 0 always wins.
int test_hu_quads_pin() {
  const std::array<int, 3> flop = {c2c, c2d, c2h};
  const std::array<std::array<int, 2>, 2> hands = {{{c2s, c3c}, {cAh, cKh}}};
  const std::array<bool, 2> folded = {false, false};
  const std::array<std::int64_t, 2> contributed = {2, 2};
  const auto ledger = make_ledger(2, folded, contributed);

  const EquityFrontierEvaluator eval;
  const auto values = eval.evaluate(flop, hands, ledger);
  const auto eq = equity_from_values(values, contributed);

  CHECK_CLOSE(eq[0], 1.0, 1e-12);
  CHECK_CLOSE(eq[1], 0.0, 1e-12);
  // Pot = 4; seat 0 nets +2, seat 1 nets -2.
  CHECK_CLOSE(values[0], 2.0, 1e-9);
  CHECK_CLOSE(values[1], -2.0, 1e-9);
  return 0;
}

// HU: both players hold broadway on a rainbow T-J-Q flop. Always tie.
int test_hu_broadway_tie() {
  const std::array<int, 3> flop = {cTc, cJd, cQh};
  const std::array<std::array<int, 2>, 2> hands = {{{cAs, cKh}, {cAd, cKc}}};
  const std::array<bool, 2> folded = {false, false};
  const std::array<std::int64_t, 2> contributed = {2, 2};
  const auto ledger = make_ledger(2, folded, contributed);

  const EquityFrontierEvaluator eval;
  const auto values = eval.evaluate(flop, hands, ledger);
  const auto eq = equity_from_values(values, contributed);

  CHECK_CLOSE(eq[0], 0.5, 1e-12);
  CHECK_CLOSE(eq[1], 0.5, 1e-12);
  // Both break even: each contributes 2 to a 4-pot and wins half.
  CHECK_CLOSE(values[0], 0.0, 1e-9);
  CHECK_CLOSE(values[1], 0.0, 1e-9);
  return 0;
}

// 3-way: three broadway hands on a rainbow T-J-Q flop. All three always tie.
int test_three_way_split() {
  const std::array<int, 3> flop = {cTc, cJd, cQh};
  const std::array<std::array<int, 2>, 3> hands = {{{cAs, cKh}, {cAd, cKc}, {cAh, cKd}}};
  const std::array<bool, 3> folded = {false, false, false};
  const std::array<std::int64_t, 3> contributed = {2, 2, 2};
  const auto ledger = make_ledger(3, folded, contributed);

  const EquityFrontierEvaluator eval;
  const auto values = eval.evaluate(flop, hands, ledger);
  const auto eq = equity_from_values(values, contributed);

  CHECK_CLOSE(eq[0], 1.0 / 3.0, 1e-12);
  CHECK_CLOSE(eq[1], 1.0 / 3.0, 1e-12);
  CHECK_CLOSE(eq[2], 1.0 / 3.0, 1e-12);
  // Each contributes 2 to a 6-pot and wins a third: net 0.
  CHECK_CLOSE(values[0], 0.0, 1e-9);
  CHECK_CLOSE(values[1], 0.0, 1e-9);
  CHECK_CLOSE(values[2], 0.0, 1e-9);
  return 0;
}

// 3-way: two broadway hands tie, third has a ragged hand. The third hand has
// small but non-zero equity (runner-runner A-K makes broadway to tie), so this
// is verified against the independent oracle rather than an analytical pin.
int test_two_tie_third_worse() {
  const std::array<int, 3> flop = {cTc, cJd, cQh};
  const std::array<std::array<int, 2>, 3> hands = {{{cAs, cKh}, {cAd, cKc}, {c2s, c3h}}};
  const std::array<bool, 3> folded = {false, false, false};
  const std::array<std::int64_t, 3> contributed = {2, 2, 2};
  const auto ledger = make_ledger(3, folded, contributed);

  const EquityFrontierEvaluator eval;
  const auto values = eval.evaluate(flop, hands, ledger);
  const auto eq = equity_from_values(values, contributed);
  const auto oracle = oracle_equity(flop, hands, ledger);

  // The two broadway hands each get close to 0.5 (the third hand's runner-
  // runner broadway equity is small but non-zero).
  for (std::size_t s = 0; s < 3; ++s)
    CHECK_CLOSE(eq[s], oracle[s], 1e-12);
  CHECK(eq[0] > 0.49 && eq[0] < 0.5);
  CHECK(eq[1] > 0.49 && eq[1] < 0.5);
  CHECK(eq[2] > 0.0 && eq[2] < 0.01);
  return 0;
}

// 3-way with one fold: the folded seat's chips stay as dead money. The two
// live seats compete for the full pot; the folded seat gets equity 0.
int test_folded_seat_exclusion() {
  const std::array<int, 3> flop = {c2c, c2d, c2h};
  const std::array<std::array<int, 2>, 3> hands = {{{c2s, c3c}, {cAh, cKh}, {cQc, cQd}}};
  const std::array<bool, 3> folded = {false, false, true};
  const std::array<std::int64_t, 3> contributed = {2, 2, 2};
  const auto ledger = make_ledger(3, folded, contributed);

  const EquityFrontierEvaluator eval;
  const auto values = eval.evaluate(flop, hands, ledger);
  const auto eq = equity_from_values(values, contributed);

  // Seat 0 has quads and always wins; seat 2 folded (equity 0).
  CHECK_CLOSE(eq[0], 1.0, 1e-12);
  CHECK_CLOSE(eq[1], 0.0, 1e-12);
  CHECK_CLOSE(eq[2], 0.0, 1e-12);
  // Pot = 6 (including seat 2's dead 2). Seat 0 nets +4, seats 1 and 2 net -2.
  CHECK_CLOSE(values[0], 4.0, 1e-9);
  CHECK_CLOSE(values[1], -2.0, 1e-9);
  CHECK_CLOSE(values[2], -2.0, 1e-9);
  return 0;
}

// All but one seat fold: the live seat gets equity 1.0 regardless of cards.
int test_single_live_seat() {
  const std::array<int, 3> flop = {c2c, c3d, c5h};
  const std::array<std::array<int, 2>, 3> hands = {{{c7c, c8c}, {cAh, cKh}, {cQc, cQd}}};
  const std::array<bool, 3> folded = {false, true, true};
  const std::array<std::int64_t, 3> contributed = {2, 2, 2};
  const auto ledger = make_ledger(3, folded, contributed);

  const EquityFrontierEvaluator eval;
  const auto values = eval.evaluate(flop, hands, ledger);
  const auto eq = equity_from_values(values, contributed);

  CHECK_CLOSE(eq[0], 1.0, 1e-12);
  CHECK_CLOSE(eq[1], 0.0, 1e-12);
  CHECK_CLOSE(eq[2], 0.0, 1e-12);
  // Pot = 6; seat 0 nets +4, the folded seats net -2 each.
  CHECK_CLOSE(values[0], 4.0, 1e-9);
  CHECK_CLOSE(values[1], -2.0, 1e-9);
  CHECK_CLOSE(values[2], -2.0, 1e-9);
  return 0;
}

// --- Cache tests -----------------------------------------------------------

// Repeated calls with the same key return identical values; calls with
// different hands return different values.
int test_cache_repeatability() {
  const std::array<int, 3> flop = {cTc, cJd, cQh};
  const std::array<bool, 3> folded = {false, false, false};
  const std::array<std::int64_t, 3> contributed = {2, 2, 2};
  const auto ledger = make_ledger(3, folded, contributed);

  const EquityFrontierEvaluator eval;

  const std::array<std::array<int, 2>, 3> broadway = {{{cAs, cKh}, {cAd, cKc}, {cAh, cKd}}};
  const auto v1 = eval.evaluate(flop, broadway, ledger);
  const auto v2 = eval.evaluate(flop, broadway, ledger);
  CHECK(v1 == v2);

  // Different hands (one seat has a pair instead of broadway) → different
  // equity for that seat.
  const std::array<std::array<int, 2>, 3> mixed = {{{cAs, cKh}, {cAd, cKc}, {c9s, c9d}}};
  const auto v3 = eval.evaluate(flop, mixed, ledger);
  CHECK(v3[2] != v2[2]);
  return 0;
}

// The cache key includes the live-seat set: the same (flop, hands) pair with
// two different fold patterns returns different equities. If the key omitted
// the live-set, the second call would return the cached all-live value.
int test_cache_live_set_keying() {
  const std::array<int, 3> flop = {cTc, cJd, cQh};
  const std::array<std::array<int, 2>, 3> hands = {{{cAs, cKh}, {cAd, cKc}, {cAh, cKd}}};
  const std::array<std::int64_t, 3> contributed = {2, 2, 2};

  const EquityFrontierEvaluator eval;

  // All three live: each gets 1/3.
  const std::array<bool, 3> all_live = {false, false, false};
  const auto ledger_live = make_ledger(3, all_live, contributed);
  const auto v_live = eval.evaluate(flop, hands, ledger_live);
  const auto eq_live = equity_from_values(v_live, contributed);
  CHECK_CLOSE(eq_live[0], 1.0 / 3.0, 1e-12);

  // Seat 2 folds: seats 0 and 1 now split at 0.5 each.
  const std::array<bool, 3> seat2_folded = {false, false, true};
  const auto ledger_fold = make_ledger(3, seat2_folded, contributed);
  const auto v_fold = eval.evaluate(flop, hands, ledger_fold);
  const auto eq_fold = equity_from_values(v_fold, contributed);
  CHECK_CLOSE(eq_fold[0], 0.5, 1e-12);
  CHECK_CLOSE(eq_fold[1], 0.5, 1e-12);
  CHECK_CLOSE(eq_fold[2], 0.0, 1e-12);

  // The two calls must have produced different equities (proving the cache
  // did not serve the all-live entry for the folded call).
  CHECK(eq_live[0] != eq_fold[0]);
  return 0;
}

// With a cap of 1, the cache holds at most one entry. New deals are computed
// fresh (not served from a stale entry), and the single cached entry remains
// valid on repeat. Correctness is cross-checked against the independent
// oracle because three AK hands with different suits are not equity-symmetric
// on arbitrary flops (suit interactions break the symmetry).
int test_cache_cap() {
  const EquityFrontierEvaluator eval(/*cache_cap=*/1);

  const std::array<bool, 3> folded = {false, false, false};
  const std::array<std::int64_t, 3> contributed = {2, 2, 2};
  const auto ledger = make_ledger(3, folded, contributed);

  // Five distinct deals, each with a different flop. Every call must return
  // the correct equity (computed fresh when the cache is full).
  const std::array<std::array<int, 3>, 5> flops = {{
      {cTc, cJd, cQh},
      {c2c, c2d, c2h},
      {c3c, c3d, c5h},
      {c7c, c8c, cTh},
      {cQc, cKd, cAh},
  }};
  const std::array<std::array<int, 2>, 3> hands = {{{cAs, cKh}, {cAd, cKc}, {cAh, cKd}}};

  for (const auto& flop : flops) {
    const auto values = eval.evaluate(flop, hands, ledger);
    const auto eq = equity_from_values(values, contributed);
    const auto oracle = oracle_equity(flop, hands, ledger);
    for (std::size_t s = 0; s < 3; ++s)
      CHECK_CLOSE(eq[s], oracle[s], 1e-9);
  }

  // Repeat the first deal: the cached entry (if still present) or a fresh
  // computation must return the same value.
  const auto v_repeat = eval.evaluate(flops[0], hands, ledger);
  const auto eq_repeat = equity_from_values(v_repeat, contributed);
  const auto oracle_first = oracle_equity(flops[0], hands, ledger);
  for (std::size_t s = 0; s < 3; ++s)
    CHECK_CLOSE(eq_repeat[s], oracle_first[s], 1e-12);
  return 0;
}

// --- Rejection contract ----------------------------------------------------

int test_rejections() {
  const std::array<int, 3> flop = {c2c, c3d, c5h};
  const std::array<bool, 2> folded2 = {false, false};
  const std::array<std::int64_t, 2> contributed2 = {2, 2};

  // N = 1: rejected.
  {
    const std::array<std::array<int, 2>, 1> hands = {{{cAs, cKh}}};
    const auto ledger = make_ledger(1, folded2, contributed2);
    const EquityFrontierEvaluator eval;
    bool threw = false;
    try {
      eval.evaluate(flop, hands, ledger);
    } catch (const std::invalid_argument&) {
      threw = true;
    }
    CHECK(threw);
  }

  // N = 11: rejected.
  {
    std::array<std::array<int, 2>, 11> hands{};
    for (auto& h : hands)
      h = {cAs, cKh};
    TerminalPayload ledger;
    ledger.player_count = 11;
    const EquityFrontierEvaluator eval;
    bool threw = false;
    try {
      eval.evaluate(flop, hands, ledger);
    } catch (const std::invalid_argument&) {
      threw = true;
    }
    CHECK(threw);
  }

  // Flop size != 3: rejected.
  {
    const std::array<int, 2> bad_flop = {c2c, c3d};
    const std::array<std::array<int, 2>, 2> hands = {{{cAs, cKh}, {cQc, cQd}}};
    const auto ledger = make_ledger(2, folded2, contributed2);
    const EquityFrontierEvaluator eval;
    bool threw = false;
    try {
      eval.evaluate(bad_flop, hands, ledger);
    } catch (const std::invalid_argument&) {
      threw = true;
    }
    CHECK(threw);
  }

  // Ledger player_count mismatch: rejected.
  {
    const std::array<std::array<int, 2>, 2> hands = {{{cAs, cKh}, {cQc, cQd}}};
    TerminalPayload ledger;
    ledger.player_count = 3;  // hands has 2
    const EquityFrontierEvaluator eval;
    bool threw = false;
    try {
      eval.evaluate(flop, hands, ledger);
    } catch (const std::invalid_argument&) {
      threw = true;
    }
    CHECK(threw);
  }

  // No live seats: rejected.
  {
    const std::array<std::array<int, 2>, 2> hands = {{{cAs, cKh}, {cQc, cQd}}};
    const std::array<bool, 2> all_folded = {true, true};
    const auto ledger = make_ledger(2, all_folded, contributed2);
    const EquityFrontierEvaluator eval;
    bool threw = false;
    try {
      eval.evaluate(flop, hands, ledger);
    } catch (const std::invalid_argument&) {
      threw = true;
    }
    CHECK(threw);
  }

  return 0;
}

// --- Independent oracle cross-check ----------------------------------------

// Cross-checks the evaluator against the independently coded brute-force
// oracle on three non-trivial 3-way deals, including a folded seat.
int test_oracle_agreement() {
  const EquityFrontierEvaluator eval;

  // Deal A: overcards vs flush draw vs pair, all live.
  {
    const std::array<int, 3> flop = {c2c, c3d, c5h};
    const std::array<std::array<int, 2>, 3> hands = {{{cAs, cKs}, {c7h, c8h}, {cQc, cQd}}};
    const std::array<bool, 3> folded = {false, false, false};
    const std::array<std::int64_t, 3> contributed = {2, 2, 2};
    const auto ledger = make_ledger(3, folded, contributed);

    const auto values = eval.evaluate(flop, hands, ledger);
    const auto eq = equity_from_values(values, contributed);
    const auto oracle = oracle_equity(flop, hands, ledger);

    for (std::size_t s = 0; s < 3; ++s)
      CHECK_CLOSE(eq[s], oracle[s], 1e-12);
  }

  // Deal B: two broadway tie, third worse (oracle-verified; the third hand
  // has small runner-runner broadway equity, so no analytical 0.5/0.5/0 pin).
  {
    const std::array<int, 3> flop = {cTc, cJd, cQh};
    const std::array<std::array<int, 2>, 3> hands = {{{cAs, cKh}, {cAd, cKc}, {c2s, c3h}}};
    const std::array<bool, 3> folded = {false, false, false};
    const std::array<std::int64_t, 3> contributed = {2, 2, 2};
    const auto ledger = make_ledger(3, folded, contributed);

    const auto values = eval.evaluate(flop, hands, ledger);
    const auto eq = equity_from_values(values, contributed);
    const auto oracle = oracle_equity(flop, hands, ledger);

    for (std::size_t s = 0; s < 3; ++s)
      CHECK_CLOSE(eq[s], oracle[s], 1e-12);
  }

  // Deal C: quads vs overcards, third seat folded (dead money).
  {
    const std::array<int, 3> flop = {c2c, c2d, c2h};
    const std::array<std::array<int, 2>, 3> hands = {{{c2s, c3c}, {cAh, cKh}, {cQc, cQd}}};
    const std::array<bool, 3> folded = {false, false, true};
    const std::array<std::int64_t, 3> contributed = {2, 2, 2};
    const auto ledger = make_ledger(3, folded, contributed);

    const auto values = eval.evaluate(flop, hands, ledger);
    const auto eq = equity_from_values(values, contributed);
    const auto oracle = oracle_equity(flop, hands, ledger);

    for (std::size_t s = 0; s < 3; ++s)
      CHECK_CLOSE(eq[s], oracle[s], 1e-12);
    CHECK_CLOSE(oracle[0], 1.0, 1e-12);
    CHECK_CLOSE(oracle[2], 0.0, 1e-12);
  }

  return 0;
}

}  // namespace

int main() {
  if (test_hu_quads_pin())
    return 1;
  if (test_hu_broadway_tie())
    return 1;
  if (test_three_way_split())
    return 1;
  if (test_two_tie_third_worse())
    return 1;
  if (test_folded_seat_exclusion())
    return 1;
  if (test_single_live_seat())
    return 1;
  if (test_cache_repeatability())
    return 1;
  if (test_cache_live_set_keying())
    return 1;
  if (test_cache_cap())
    return 1;
  if (test_rejections())
    return 1;
  if (test_oracle_agreement())
    return 1;
  std::printf("test_equity_frontier: all tests passed\n");
  return 0;
}
