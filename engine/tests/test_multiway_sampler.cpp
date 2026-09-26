// Independent regressions for RFC 0006 joint-deal sampling.
//
// The property that matters is stated in the RFC as a prohibition: do NOT
// sample each seat independently and renormalize. The decisive test is
// therefore a distributional one - the empirical draw frequencies must match
// the enumerated joint distribution, on a fixture where independent-per-seat
// sampling would give a measurably different answer.
#include <algorithm>
#include <array>
#include <bs/eval.hpp>
#include <bs/multiway_sampler.hpp>
#include <bs/prng.hpp>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <exception>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

using bs::SplitMix64;
using namespace bs::solver;

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

using Range = std::vector<MultiwayWeightedHand>;

// A three-seat fixture whose seats overlap heavily, so card conflicts are
// common and the conditional joint distribution differs sharply from the
// product of the marginals.
std::vector<Range> overlapping_fixture() {
  // Conflicts are arranged so the SUPPORT itself is not a product set:
  //   seat 0: X=AsKs, Y=QhJh
  //   seat 1: Z=AsAd, W=QhQd      (Z conflicts X; W conflicts Y)
  //   seat 2: U=AdKd, V=JhJd      (V conflicts Y)
  // Compatible joint deals are exactly (X,W,U), (X,W,V), (Y,Z,U).
  // Each has joint weight 1/3, while the product of the per-seat marginals
  // would give (X,W,U) only 8/27 - so independent per-seat sampling is
  // measurably different, which is what the RFC forbids.
  return {
      {{{card("As"), card("Ks")}, 1.0}, {{card("Qh"), card("Jh")}, 1.0}},
      {{{card("As"), card("Ad")}, 1.0}, {{card("Qh"), card("Qd")}, 1.0}},
      {{{card("Ad"), card("Kd")}, 1.0}, {{card("Jh"), card("Jd")}, 1.0}},
  };
}

std::string deal_key(const MultiwayDeal& deal) {
  std::string key;
  for (const auto& hand : deal.hands) {
    key += std::to_string(hand[0]) + "-" + std::to_string(hand[1]) + "|";
  }
  return key;
}

// ------------------------------------------------------------- enumeration

int test_enumeration_matches_hand_derived_support() {
  const std::vector<Range> ranges = {
      {{{card("As"), card("Ks")}, 2.0}, {{card("Qh"), card("Jh")}, 1.0}},
      {{{card("Ah"), card("Kh")}, 1.0}},
  };
  const JointDealTable table = enumerate_joint_deals(ranges);
  // Seat 0 As/Ad conflicts with seat 1's Ah? No: As, Ks vs Ah, Kh share no
  // card, so both seat-0 combos are compatible with the single seat-1 combo.
  CHECK(table.deals.size() == 2);
  // Weights are the products 2.0 and 1.0, normalized to 2/3 and 1/3.
  double total = 0;
  for (const MultiwayDeal& deal : table.deals)
    total += deal.weight;
  CHECK(std::abs(total - 1.0) < 1e-12);
  CHECK(std::abs(table.unnormalized_mass - 3.0) < 1e-12);
  // The heavier combo is the first seat's first hand.
  const std::array<int, 2> expected_first{card("As"), card("Ks")};
  CHECK(table.deals[0].hands[0] == expected_first);
  CHECK(std::abs(table.deals[0].weight - 2.0 / 3.0) < 1e-12);
  CHECK(std::abs(table.deals[1].weight - 1.0 / 3.0) < 1e-12);
  return 0;
}

int test_enumeration_rejects_conflicts() {
  // Every seat holds the same single combo: no compatible deal exists.
  const std::vector<Range> ranges = {
      {{{card("As"), card("Ks")}, 1.0}},
      {{{card("As"), card("Ks")}, 1.0}},
      {{{card("As"), card("Ks")}, 1.0}},
  };
  bool threw = false;
  try {
    const JointDealTable table = enumerate_joint_deals(ranges);
  } catch (const std::invalid_argument&) {
    threw = true;
  }
  CHECK(threw);

  // A board card blocks a hand: with As on the board, the seat whose only combo
  // uses As drops out, leaving no compatible joint deal at all.
  const std::vector<Range> blocked = {
      {{{card("As"), card("Ks")}, 1.0}},
      {{{card("As"), card("Kh")}, 1.0}},
  };
  bool blocked_threw = false;
  try {
    enumerate_joint_deals(blocked, {card("As")});
  } catch (const std::invalid_argument&) {
    blocked_threw = true;
  }
  CHECK(blocked_threw);

  // With the board removed the same ranges DO have a joint deal only if the two
  // seats can avoid sharing a card; As/Ks vs As/Kh share As, so no deal exists
  // either. Adding an independent combo to the second seat makes one appear.
  const std::vector<Range> unblocked = {
      {{{card("As"), card("Ks")}, 1.0}},
      {{{card("As"), card("Kh")}, 1.0}, {{card("2c"), card("3c")}, 1.0}},
  };
  const JointDealTable table = enumerate_joint_deals(unblocked);
  CHECK(table.deals.size() == 1);  // only As/Ks against 2c/3c
  return 0;
}

int test_enumeration_validates_input() {
  // Empty seat range.
  {
    const std::vector<Range> ranges = {{{}}, {{{card("As"), card("Ks")}, 1.0}}};
    bool threw = false;
    try {
      enumerate_joint_deals(ranges);
    } catch (const std::invalid_argument&) {
      threw = true;
    }
    CHECK(threw);
  }
  // Negative weight.
  {
    const std::vector<Range> ranges = {{{{{card("As"), card("Ks")}, -1.0}}}};
    bool threw = false;
    try {
      enumerate_joint_deals(ranges);
    } catch (const std::invalid_argument&) {
      threw = true;
    }
    CHECK(threw);
  }
  // Card outside the deck.
  {
    const std::vector<Range> ranges = {{{{{99, 3}, 1.0}}}};
    bool threw = false;
    try {
      enumerate_joint_deals(ranges);
    } catch (const std::invalid_argument&) {
      threw = true;
    }
    CHECK(threw);
  }
  return 0;
}

// ------------------------------------------------------------ distribution

// The decisive property: the empirical draw frequencies match the ENUMERATED
// joint distribution. The fixture uses UNEQUAL weights so the joint is
// non-uniform, which makes the test discriminating: a sampler that drew
// uniformly over the support, or that sampled seats independently and
// renormalized, would produce a visibly different histogram.
int test_sampling_matches_the_joint_distribution() {
  // Seat 0 offers a heavy and a light combo; the heavy one conflicts with the
  // heavier of seat 1's choices, so the surviving joint weights are skewed.
  const std::vector<Range> ranges = {
      {{{card("As"), card("Ks")}, 3.0}, {{card("Qh"), card("Jh")}, 1.0}},
      {{{card("As"), card("Ad")}, 1.0}, {{card("2c"), card("2d")}, 1.0}},
      {{{card("Ad"), card("Kd")}, 2.0}, {{card("Jh"), card("Jd")}, 1.0}},
  };
  const JointDealTable table = enumerate_joint_deals(ranges);
  CHECK(table.deals.size() >= 2);

  // The enumerated weights must be skewed, not uniform.
  double min_weight = 1.0;
  double max_weight = 0.0;
  double total = 0;
  for (const MultiwayDeal& deal : table.deals) {
    min_weight = std::min(min_weight, deal.weight);
    max_weight = std::max(max_weight, deal.weight);
    total += deal.weight;
  }
  CHECK(std::abs(total - 1.0) < 1e-12);
  CHECK(max_weight > min_weight * 1.5);  // genuinely non-uniform

  constexpr std::size_t kDraws = 300000;
  SplitMix64 rng(0x5eed1234ULL);
  std::map<std::string, std::size_t> counts;
  for (std::size_t i = 0; i < kDraws; ++i) {
    const std::size_t index = sample_joint_deal(table, ranges, rng);
    CHECK(index < table.deals.size());
    ++counts[deal_key(table.deals[index])];
  }

  // Every enumerated deal is reachable and nothing outside the support is drawn.
  CHECK(counts.size() == table.deals.size());
  double max_deviation = 0;
  for (const MultiwayDeal& deal : table.deals) {
    const auto it = counts.find(deal_key(deal));
    CHECK(it != counts.end());
    const double observed = static_cast<double>(it->second) / static_cast<double>(kDraws);
    max_deviation = std::max(max_deviation, std::abs(observed - deal.weight));
  }
  // With 300k draws the standard error on a probability near 1/3 is about
  // 0.0009, so an 0.01 bound is generous yet still catches the wrong
  // distributions (uniform-over-support would be off by the weight spread).
  CHECK(max_deviation < 0.01);
  return 0;
}

// The sampler is reproducible from its seed.
int test_sampling_is_reproducible() {
  const std::vector<Range> ranges = overlapping_fixture();
  const JointDealTable table = enumerate_joint_deals(ranges);
  SplitMix64 a(42);
  SplitMix64 b(42);
  for (int i = 0; i < 200; ++i)
    CHECK(sample_joint_deal(table, ranges, a) == sample_joint_deal(table, ranges, b));
  // A different seed must produce a different sequence.
  SplitMix64 c(43);
  SplitMix64 d(42);
  bool differed = false;
  for (int i = 0; i < 200; ++i)
    if (sample_joint_deal(table, ranges, c) != sample_joint_deal(table, ranges, d))
      differed = true;
  CHECK(differed);
  return 0;
}

// Weighted seats are respected: a heavier combo is drawn more often, in
// proportion.
int test_sampling_respects_weights() {
  const std::vector<Range> ranges = {
      {{{card("As"), card("Ks")}, 3.0}, {{card("2h"), card("3h")}, 1.0}},
      {{{card("Ah"), card("Kh")}, 1.0}},
  };
  const JointDealTable table = enumerate_joint_deals(ranges);
  CHECK(table.deals.size() == 2);
  constexpr std::size_t kDraws = 100000;
  SplitMix64 rng(7);
  std::size_t heavy = 0;
  for (std::size_t i = 0; i < kDraws; ++i)
    if (sample_joint_deal(table, ranges, rng) == 0)
      ++heavy;
  const double observed = static_cast<double>(heavy) / static_cast<double>(kDraws);
  CHECK(std::abs(observed - 0.75) < 0.01);  // 3/(3+1)
  return 0;
}

// A zero weight means "not in the range at all", which the header states. The
// distinction matters at the head of a seat's range: a zero first weight puts a
// repeated value in the cumulative table, so the draw has to be able to land
// past it rather than stall on index 0.
//
// Only the first two checks below are non-tautological; the third is recorded
// as such. `sample_joint_deal` returns an index into `table.deals`, and
// `enumerate_joint_deals` only ever admits positive-weight deals, so "the
// sampler never returns a zero-weight combo" is true by construction and would
// pass under almost any mutation. It is not asserted here, because a check that
// cannot fail is not evidence.
int test_zero_weight_combos_are_absent() {
  // 1. Enumeration excludes them, and the surviving weights stay normalized.
  {
    const std::vector<Range> ranges = {
        {{{card("As"), card("Ks")}, 0.0}, {{card("2h"), card("3h")}, 2.0}},
        {{{card("Ah"), card("Kh")}, 1.0}},
    };
    const JointDealTable table = enumerate_joint_deals(ranges);
    CHECK(table.deals.size() == 1);  // only the positive-weight combo
    CHECK(table.unnormalized_mass == 2.0);
    CHECK(std::abs(table.deals[0].weight - 1.0) < 1e-12);
    const std::array<int, 2> expected = {card("2h"), card("3h")};
    CHECK(table.deals[0].hands[0] == expected);
  }

  // 2. A seat whose entire range is zero-weight has no support and must be
  //    rejected rather than silently producing a degenerate distribution.
  {
    const std::vector<Range> ranges = {
        {{{card("As"), card("Ks")}, 0.0}, {{card("2h"), card("3h")}, 0.0}},
        {{{card("Ah"), card("Kh")}, 1.0}},
    };
    bool threw = false;
    try {
      enumerate_joint_deals(ranges);
    } catch (const std::invalid_argument&) {
      threw = true;
    }
    CHECK(threw);
  }

  // 3. With a LEADING zero weight, a long draw still matches the declared
  //    distribution over the surviving support. This is the case a boundary bug
  //    in the cumulative lookup would move, so the assertion is on the shape of
  //    the distribution, not on any single draw.
  {
    const std::vector<Range> ranges = {
        {{{card("As"), card("Ks")}, 0.0},
         {{card("2h"), card("3h")}, 1.0},
         {{card("4h"), card("5h")}, 3.0}},
        {{{card("Ah"), card("Kh")}, 1.0}},
    };
    const JointDealTable table = enumerate_joint_deals(ranges);
    CHECK(table.deals.size() == 2);
    constexpr std::size_t kDraws = 100000;
    SplitMix64 rng(11);
    std::size_t heavy = 0;
    for (std::size_t i = 0; i < kDraws; ++i)
      if (sample_joint_deal(table, ranges, rng) == 1)
        ++heavy;
    const double observed = static_cast<double>(heavy) / static_cast<double>(kDraws);
    CHECK(std::abs(observed - 0.75) < 0.01);  // 3/(1+3)
  }
  return 0;
}

int test_sampler_validates_input() {
  const std::vector<Range> ranges = {{{{{card("As"), card("Ks")}, 1.0}}}};
  const JointDealTable table = enumerate_joint_deals(ranges);
  // Seat count mismatch.
  {
    SplitMix64 rng(1);
    const std::vector<Range> other = {{{{card("As"), card("Ks")}, 1.0}},
                                      {{{card("2c"), card("3c")}, 1.0}}};
    bool threw = false;
    try {
      sample_joint_deal(table, other, rng);
    } catch (const std::invalid_argument&) {
      threw = true;
    }
    CHECK(threw);
  }
  // Empty table.
  {
    SplitMix64 rng(1);
    JointDealTable empty;
    bool threw = false;
    try {
      sample_joint_deal(empty, ranges, rng);
    } catch (const std::invalid_argument&) {
      threw = true;
    }
    CHECK(threw);
  }
  return 0;
}

// Four to six seats enumerate compatibly: the support counts follow the card
// arithmetic rather than a seat cap.
int test_wider_tables_enumerate() {
  std::vector<Range> ranges;
  for (int seat = 0; seat < 4; ++seat) {
    // Four disjoint combos, so every joint deal is compatible.
    ranges.push_back({{{seat * 4 + 0, seat * 4 + 1}, 1.0}, {{seat * 4 + 2, seat * 4 + 3}, 1.0}});
  }
  const JointDealTable table = enumerate_joint_deals(ranges);
  CHECK(table.deals.size() == 16);  // 2^4
  double total = 0;
  for (const MultiwayDeal& deal : table.deals) {
    total += deal.weight;
    CHECK(deal.hands.size() == 4);
  }
  CHECK(std::abs(total - 1.0) < 1e-12);
  // Sampling over a wide table yields only compatible deals.
  SplitMix64 rng(11);
  for (int i = 0; i < 500; ++i) {
    const std::size_t index = sample_joint_deal(table, ranges, rng);
    std::array<bool, 52> used{};
    for (const auto& hand : table.deals[index].hands)
      for (int c : hand) {
        CHECK(!used[c]);
        used[c] = true;
      }
  }
  return 0;
}

// ------------------------------------------------- scalable restart dealer

// Primary distribution gate (R5): the scalable restart dealer must match the
// enumerated product-conditional joint distribution on the same skewed
// nonuniform fixture the enumeration sampler is tested on, where conflicts are
// common and the weights genuinely skew the support.
int test_scalable_matches_joint_distribution() {
  const std::vector<Range> ranges = {
      {{{card("As"), card("Ks")}, 3.0}, {{card("Qh"), card("Jh")}, 1.0}},
      {{{card("As"), card("Ad")}, 1.0}, {{card("2c"), card("2d")}, 1.0}},
      {{{card("Ad"), card("Kd")}, 2.0}, {{card("Jh"), card("Jd")}, 1.0}},
  };
  const JointDealTable table = enumerate_joint_deals(ranges);
  constexpr std::size_t kDraws = 400000;
  SplitMix64 rng(0x5ca1ab1eULL);
  std::map<std::string, std::size_t> counts;
  std::size_t total_attempts = 0;
  for (std::size_t i = 0; i < kDraws; ++i) {
    std::size_t attempts = 0;
    const MultiwayDeal deal = sample_scalable_joint_deal(ranges, {}, rng, 100000, &attempts);
    CHECK(deal.hands.size() == ranges.size());
    CHECK(attempts >= 1);
    total_attempts += attempts;
    ++counts[deal_key(deal)];
    // Zero card duplication across the deal.
    bool used[52] = {};
    for (const auto& hand : deal.hands)
      for (int c : hand) {
        CHECK(!used[c]);
        used[c] = true;
      }
  }
  CHECK(counts.size() == table.deals.size());
  double max_deviation = 0;
  for (const MultiwayDeal& deal : table.deals) {
    const auto it = counts.find(deal_key(deal));
    CHECK(it != counts.end());
    const double observed = static_cast<double>(it->second) / static_cast<double>(kDraws);
    max_deviation = std::max(max_deviation, std::abs(observed - deal.weight));
  }
  // ~400k draws; 0.01 bound mirrors the enumeration-sampler gate.
  CHECK(max_deviation < 0.01);
  // Conflicts occur on this fixture, so the restart counter must have fired;
  // the measured acceptance rate stays comfortably above zero.
  CHECK(total_attempts > kDraws);
  CHECK(static_cast<double>(kDraws) / static_cast<double>(total_attempts) > 0.05);
  return 0;
}

int test_scalable_is_reproducible() {
  const std::vector<Range> ranges = overlapping_fixture();
  SplitMix64 rng_a(12345ULL);
  SplitMix64 rng_b(12345ULL);
  MultiwayDeal a = sample_scalable_joint_deal(ranges, {}, rng_a);
  MultiwayDeal b = sample_scalable_joint_deal(ranges, {}, rng_b);
  CHECK(a.hands == b.hands);
  CHECK(std::abs(a.weight - b.weight) < 1e-12);
  return 0;
}

// The balanced-marginal assertion is exact ONLY for the symmetric uniform
// control (R5): with every seat offered the same full uniform range, each
// accepted seat's marginal must be uniform over its 1326 combos. Under
// nonuniform/asymmetric weights the marginal is instead shifted by suffix
// completion mass, which the joint-frequency gate above covers.
int test_scalable_uniform_control_preserves_marginals() {
  Range all;
  for (int a = 0; a < 52; ++a)
    for (int b = a + 1; b < 52; ++b)
      all.push_back(MultiwayWeightedHand{std::array<int, 2>{a, b}, 1.0});
  CHECK(all.size() == 1326);
  const std::vector<Range> ranges = {all, all, all};
  constexpr std::size_t kDraws = 200000;
  SplitMix64 rng(0xabcdef01ULL);
  std::array<std::map<int, std::size_t>, 3> per_combo{};
  for (std::size_t i = 0; i < kDraws; ++i) {
    const MultiwayDeal deal = sample_scalable_joint_deal(ranges, {}, rng);
    for (std::size_t s = 0; s < 3; ++s) {
      const int canonical = deal.hands[s][0] < deal.hands[s][1]
                                ? deal.hands[s][0] * 64 + deal.hands[s][1]
                                : deal.hands[s][1] * 64 + deal.hands[s][0];
      ++per_combo[s][canonical];
    }
  }
  // Every one of the 1326 combos appears for every seat, and no combo's share
  // is far off 1/1326. Allow generous Monte Carlo slack on the per-combo tail
  // while still catching a systematic renormalization bias.
  for (std::size_t s = 0; s < 3; ++s) {
    CHECK(per_combo[s].size() == 1326);
    const double expected = kDraws / 1326.0;
    for (const auto& [combo, n] : per_combo[s]) {
      (void)combo;
      const double z = std::abs(static_cast<double>(n) - expected) / std::sqrt(expected);
      CHECK(z < 6.0);  // far beyond any per-combo MC fluctuation
    }
  }
  return 0;
}

// Negative control (R5): the forbidden construction — independently sampling
// every earlier seat and renormalizing ONLY the last seat's pool — yields a
// distribution measurably different from the enumerated one. This verifies
// the test fixture itself can detect the RFC 0006 violation, so a passing
// scalable match cannot be a fixture artifact.
int test_scalable_detects_last_seat_renormalization() {
  // Dedicated fixture that isolates the RFC 0006 prohibition. With equal
  // weights the last-seat-renormalization construction coincides with the
  // correct distribution, so the discriminating ingredient is UNEQUAL last-
  // seat weight:
  //   seat 0: single X=AsKs ; seat 1: single A=QhQd (X,A compatible)
  //   seat 2: U=AdKd (w=3), V=2c3c (w=1) — both compatible with X,A;
  //           W=Ks2d (w=1) — shares Ks with X, so filtered out.
  // The correct joint distribution keeps U's weight: 3/4 U, 1/4 V. The
  // forbidden construction renormalizes the last seat's compatible pool to
  // UNIFORM: 1/2 U, 1/2 V. The 1/4 gap is far beyond MC noise.
  const std::vector<Range> ranges = {
      {{{card("As"), card("Ks")}, 1.0}},
      {{{card("Qh"), card("Qd")}, 1.0}},
      {{{card("Ad"), card("Kd")}, 3.0},
       {{card("2c"), card("3c")}, 1.0},
       {{card("Ks"), card("2d")}, 1.0}},
  };
  const JointDealTable table = enumerate_joint_deals(ranges);
  CHECK(table.deals.size() == 2);
  auto conflicts = [](const std::array<int, 2>& a, const std::array<int, 2>& b) {
    for (int x : a)
      for (int y : b)
        if (x == y)
          return true;
    return false;
  };
  constexpr std::size_t kDraws = 300000;
  SplitMix64 rng(0xdeadbeefULL);
  std::map<std::string, std::size_t> counts;
  for (std::size_t i = 0; i < kDraws; ++i) {
    const auto h0 = ranges[0][0].cards;  // deterministic single-combo seat
    const auto h1 = ranges[1][0].cards;
    std::vector<std::array<int, 2>> ok;
    for (const auto& h2 : ranges[2])
      if (!conflicts(h2.cards, h0) && !conflicts(h2.cards, h1))
        ok.push_back(h2.cards);
    CHECK(ok.size() == 2);  // U and V survive; W is filtered by the X conflict
    // Forbidden step: uniform over the last seat's compatible pool, dropping
    // the declared weights entirely.
    const auto h2 = ok[rng.next_u64() % ok.size()];
    MultiwayDeal deal;
    deal.hands = {h0, h1, h2};
    ++counts[deal_key(deal)];
  }
  std::size_t accepted = 0;
  for (const auto& [k, n] : counts)
    accepted += n;
  CHECK(accepted == kDraws);  // no restart possible with these fixed prefixes
  double max_deviation = 0;
  for (const MultiwayDeal& deal : table.deals) {
    const auto it = counts.find(deal_key(deal));
    const double observed = it == counts.end() ? 0.0 : static_cast<double>(it->second) / accepted;
    max_deviation = std::max(max_deviation, std::abs(observed - deal.weight));
  }
  // Correct is 3/4 vs 1/4; the renormalization yields 1/2 vs 1/2 -> gap 1/4.
  // Generous MC slack; still far above the 0.01 the correct sampler passes.
  CHECK(max_deviation > 0.20);
  return 0;
}

// Board cards are excluded from every marginal (an accepted hand never uses a
// board card), and the attempts counter reports the restarts.
int test_scalable_filters_board_and_counts_attempts() {
  // Two single-combo seats that are mutually compatible but share the board
  // card 2c: with 2c on board both marginals empty out and the dealer throws.
  const std::vector<Range> blocked = {
      {{{card("2c"), card("3c")}, 1.0}},
      {{{card("2c"), card("4c")}, 1.0}},
  };
  bool threw = false;
  try {
    SplitMix64 rng(1ULL);
    (void)sample_scalable_joint_deal(blocked, {card("2c")}, rng);
  } catch (const std::invalid_argument&) {
    threw = true;
  }
  CHECK(threw);

  // Without the board conflict the same two hands deal successfully and the
  // accepted hands contain no board card.
  const std::vector<Range> ok = {
      {{{card("2c"), card("3c")}, 1.0}},
      {{{card("2d"), card("4c")}, 1.0}},
  };
  SplitMix64 rng(2ULL);
  std::size_t attempts = 99;
  const MultiwayDeal deal = sample_scalable_joint_deal(ok, {card("As")}, rng, 1000, &attempts);
  CHECK(attempts == 1);  // single-combo seats never conflict here
  for (const auto& hand : deal.hands)
    for (int c : hand)
      CHECK(c != card("As"));
  return 0;
}

}  // namespace

int main() {
  try {
    CHECK(test_enumeration_matches_hand_derived_support() == 0);
    CHECK(test_enumeration_rejects_conflicts() == 0);
    CHECK(test_enumeration_validates_input() == 0);
    CHECK(test_sampling_matches_the_joint_distribution() == 0);
    CHECK(test_sampling_is_reproducible() == 0);
    CHECK(test_sampling_respects_weights() == 0);
    CHECK(test_zero_weight_combos_are_absent() == 0);
    CHECK(test_sampler_validates_input() == 0);
    CHECK(test_wider_tables_enumerate() == 0);
    CHECK(test_scalable_matches_joint_distribution() == 0);
    CHECK(test_scalable_is_reproducible() == 0);
    CHECK(test_scalable_uniform_control_preserves_marginals() == 0);
    CHECK(test_scalable_detects_last_seat_renormalization() == 0);
    CHECK(test_scalable_filters_board_and_counts_attempts() == 0);
  } catch (const std::exception& error) {
    std::printf("Unexpected sampler exception: %s\n", error.what());
    return 1;
  }
  std::printf("test_multiway_sampler PASS\n");
  return 0;
}
