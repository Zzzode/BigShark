// Independent contribution-ledger and declared-rake regressions for RFC 0006.
#include <algorithm>
#include <bs/settlement.hpp>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <limits>
#include <optional>
#include <stdexcept>
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

template <typename Exception, typename F>
bool throws_as(F&& operation) {
  try {
    operation();
  } catch (const Exception&) {
    return true;
  } catch (...) {
    return false;
  }
  return false;
}

SettlementInput input_for(const std::vector<Chips>& contributions) {
  SettlementInput input;
  input.seat_count = contributions.size();
  input.odd_chip_rule = OddChipRule::ClockwiseLeftOfButton;
  input.rake = {RakeRule::PotPercentageFloor, 0, 0, false};
  input.flop_dealt = true;
  for (std::size_t p = 0; p < contributions.size(); ++p) {
    SettlementPlayer player;
    player.seat = p;
    player.chips.contributed = contributions[p];
    player.showdown_score = static_cast<std::uint32_t>(contributions.size() - p);
    input.players.push_back(player);
  }
  return input;
}

int check_accounting(const SettlementInput& input, const ContributionSettlement& result) {
  const auto count = input.players.size();
  CHECK(result.awards.size() == count);
  CHECK(result.refunds.size() == count);
  CHECK(result.net_contributions.size() == count);
  CHECK(result.final_stacks.size() == count);
  CHECK(result.chip_utility.size() == count);
  Chips gross = 0;
  Chips refunds = 0;
  Chips awards = 0;
  Chips initial = 0;
  Chips final = 0;
  std::int64_t utility = 0;
  for (std::size_t p = 0; p < count; ++p) {
    const auto& chips = input.players[p].chips;
    CHECK(result.net_contributions[p] == chips.contributed - chips.refunded);
    CHECK(result.refunds[p] == chips.refunded);
    CHECK(result.final_stacks[p] == chips.stack + result.awards[p]);
    CHECK(result.chip_utility[p] == static_cast<std::int64_t>(result.awards[p]) +
                                        static_cast<std::int64_t>(chips.refunded) -
                                        static_cast<std::int64_t>(chips.contributed));
    if (chips.folded)
      CHECK(result.awards[p] == 0);
    gross += chips.contributed;
    refunds += chips.refunded;
    awards += result.awards[p];
    initial += chips.stack + chips.contributed - chips.refunded;
    final += result.final_stacks[p];
    utility += result.chip_utility[p];
  }
  CHECK(result.gross_contributions == gross);
  CHECK(result.total_refunds == refunds);
  CHECK(result.pot == gross - refunds);
  CHECK(awards + result.rake + refunds == gross);
  CHECK(utility == -static_cast<std::int64_t>(result.rake));
  CHECK(final + result.rake == initial);
  return 0;
}

int test_fixed_payouts() {
  auto input = input_for({50, 100, 100});
  auto result = settle_contributions(input);
  CHECK((result.awards == std::vector<Chips>{150, 100, 0}));
  CHECK(result.layers.size() == 2);
  CHECK(result.layers[0].amount == 150);
  CHECK(result.layers[1].amount == 100);
  CHECK((result.layers[1].eligible == std::vector<std::size_t>{1, 2}));
  CHECK(check_accounting(input, result) == 0);
  input.players[1].chips.folded = true;
  input.players[1].showdown_score.reset();
  result = settle_contributions(input);
  CHECK((result.awards == std::vector<Chips>{150, 0, 100}));
  CHECK(result.layers[1].contributors.size() == 2);
  CHECK((result.layers[1].eligible == std::vector<std::size_t>{2}));
  CHECK(check_accounting(input, result) == 0);

  input = input_for({1, 1, 1});
  input.players[0].showdown_score = 9;
  input.players[1].showdown_score = 9;
  input.players[2].chips.folded = true;
  input.players[2].showdown_score = std::numeric_limits<std::uint32_t>::max();
  result = settle_contributions(input);
  CHECK((result.awards == std::vector<Chips>{1, 2, 0}));
  input.seat_count = 6;
  input.button = 2;
  input.players[0].seat = 5;
  input.players[1].seat = 1;
  input.players[2].seat = 3;
  result = settle_contributions(input);
  CHECK((result.awards == std::vector<Chips>{2, 1, 0}));
  CHECK((result.layers[0].winners == std::vector<std::size_t>{0, 1}));
  input.button = 5;
  result = settle_contributions(input);
  CHECK((result.awards == std::vector<Chips>{1, 2, 0}));
  CHECK((result.layers[0].winners == std::vector<std::size_t>{1, 0}));
  std::swap(input.players[0], input.players[1]);
  result = settle_contributions(input);
  CHECK((result.awards == std::vector<Chips>{2, 1, 0}));
  CHECK(check_accounting(input, result) == 0);
  input.seat_count = std::numeric_limits<std::size_t>::max();
  input.button = input.seat_count - 2;
  input.players[0].seat = input.seat_count - 1;
  input.players[1].seat = 0;
  result = settle_contributions(input);
  CHECK((result.awards == std::vector<Chips>{2, 1, 0}));

  input = input_for({1, 1, 1, 1, 1, 1});
  for (auto& player : input.players)
    player.showdown_score = 0;
  input.players[2].chips.folded = true;
  input.players[5].chips.folded = true;
  input.button = 0;
  result = settle_contributions(input);
  CHECK((result.awards == std::vector<Chips>{1, 2, 0, 2, 1, 0}));
  CHECK(check_accounting(input, result) == 0);
  return 0;
}

int test_refunds_and_score_scope() {
  auto input = input_for({100, 60});
  input.players[0].chips.refunded = 40;
  input.players[0].chips.stack = 45;
  input.players[1].chips.stack = 7;
  auto result = settle_contributions(input);
  CHECK((result.net_contributions == std::vector<Chips>{60, 60}));
  CHECK((result.awards == std::vector<Chips>{120, 0}));
  CHECK((result.final_stacks == std::vector<Chips>{165, 7}));
  CHECK((result.chip_utility == std::vector<std::int64_t>{60, -60}));
  CHECK(check_accounting(input, result) == 0);
  input.rake = {RakeRule::PotPercentageFloor, 500, 3, false};
  result = settle_contributions(input);
  CHECK(result.rake == 3);
  CHECK((result.awards == std::vector<Chips>{117, 0}));
  CHECK((result.final_stacks == std::vector<Chips>{162, 7}));
  CHECK((result.chip_utility == std::vector<std::int64_t>{57, -60}));
  input.players[1].showdown_score = 100;
  result = settle_contributions(input);
  CHECK((result.final_stacks == std::vector<Chips>{45, 124}));
  CHECK(check_accounting(input, result) == 0);
  CHECK(input.players[0].chips.refunded == 40);
  CHECK(input.players[0].chips.stack == 45);
  CHECK(input.players[0].chips.contributed == 100);

  // Cumulative refunds may have been wagered again; F need not fit current stack.
  input = input_for({10, 4});
  input.players[0].chips.refunded = 6;
  result = settle_contributions(input);
  CHECK((result.final_stacks == std::vector<Chips>{8, 0}));
  CHECK((result.chip_utility == std::vector<std::int64_t>{4, -4}));
  CHECK(check_accounting(input, result) == 0);

  // Fold settlement needs neither cards nor scores, even for the sole winner.
  input = input_for({3, 3, 1, 0});
  for (auto& player : input.players)
    player.showdown_score.reset();
  input.players[1].chips.folded = true;
  input.players[2].chips.folded = true;
  result = settle_contributions(input);
  CHECK((result.awards == std::vector<Chips>{7, 0, 0, 0}));
  CHECK(result.layers.size() == 2);
  CHECK(check_accounting(input, result) == 0);
  input.players[1].chips.folded = false;
  CHECK(throws_as<std::invalid_argument>([&] { settle_contributions(input); }));
  input.players[0].showdown_score = 0;
  input.players[1].showdown_score = 0;
  result = settle_contributions(input);
  CHECK((result.awards == std::vector<Chips>{3, 4, 0, 0}));
  // Zero-contribution non-folded players still need no score.
  CHECK(!input.players[3].showdown_score);
  input.rake = {RakeRule::PotPercentageFloor, 10000, 7, false};
  input.players[0].showdown_score.reset();
  CHECK(throws_as<std::invalid_argument>([&] { settle_contributions(input); }));

  input = input_for({4, 0});
  input.players[0].chips.refunded = 4;
  input.players[0].chips.stack = 4;
  for (auto& player : input.players) {
    player.chips.folded = true;
    player.showdown_score.reset();
  }
  result = settle_contributions(input);
  CHECK(result.layers.empty());
  CHECK(result.rake == 0);
  CHECK((result.final_stacks == std::vector<Chips>{4, 0}));
  CHECK(check_accounting(input, result) == 0);
  return 0;
}

int test_rake() {
  auto input = input_for({2, 5, 5});
  input.rake = {RakeRule::PotPercentageFloor, 1000, 100, false};
  auto result = settle_contributions(input);
  CHECK(result.rake == 1);
  CHECK(result.layers[0].rake == 1);
  CHECK(result.layers[1].rake == 0);
  CHECK((result.awards == std::vector<Chips>{5, 6, 0}));
  CHECK(check_accounting(input, result) == 0);
  input = input_for({1, 3, 3});
  input.rake = {RakeRule::PotPercentageFloor, 2000, 100, false};
  result = settle_contributions(input);
  CHECK(result.layers[0].rake == 0);
  CHECK(result.layers[1].rake == 1);
  CHECK((result.awards == std::vector<Chips>{3, 3, 0}));
  input.players[1].chips.folded = true;
  input.players[1].showdown_score.reset();
  result = settle_contributions(input);
  CHECK(result.rake == 1);
  CHECK((result.awards == std::vector<Chips>{3, 0, 3}));

  input = input_for({2, 5, 5});
  for (std::uint32_t bps = 0; bps <= 10000; ++bps) {
    input.rake = {RakeRule::PotPercentageFloor, bps, 100, false};
    result = settle_contributions(input);
    const Chips expected = 12 * bps / 10000;
    CHECK(result.rake == expected);
    CHECK(result.layers[0].rake == (expected + 1) / 2);
    CHECK(result.layers[1].rake == expected / 2);
    CHECK(check_accounting(input, result) == 0);
  }
  for (bool no_drop : {false, true})
    for (bool flop : {false, true})
      for (Chips cap : {Chips{0}, Chips{3}, Chips{12}, Chips{100}}) {
        input.rake = {RakeRule::PotPercentageFloor, 10000, cap, no_drop};
        input.flop_dealt = flop;
        result = settle_contributions(input);
        CHECK(result.rake == (no_drop && !flop ? 0 : std::min(cap, Chips{12})));
        CHECK(check_accounting(input, result) == 0);
      }
  return 0;
}

int test_rejections_and_limits() {
  for (const auto& contributions : {std::vector<Chips>{}, std::vector<Chips>{1},
                                    std::vector<Chips>(kMaxContributionSeats + 1, 1)}) {
    const auto input = input_for(contributions);
    CHECK(throws_as<std::invalid_argument>([&] { settle_contributions(input); }));
  }
  for (int variant = 0; variant < 12; ++variant) {
    auto input = input_for({2, 2});
    switch (variant) {
      case 0:
        input.button = 2;
        break;
      case 1:
        input.players[0].seat = 2;
        break;
      case 2:
        input.players[1].seat = 0;
        break;
      case 3:
        input.seat_count = 1;
        break;
      case 4:
        input.seat_count = 0;
        break;
      case 5:
        input.odd_chip_rule = OddChipRule::Unspecified;
        break;
      case 6:
        input.odd_chip_rule = static_cast<OddChipRule>(99);
        break;
      case 7:
        input.rake.rule = RakeRule::Unspecified;
        break;
      case 8:
        input.rake.rule = static_cast<RakeRule>(99);
        break;
      case 9:
        input.rake.basis_points = 10001;
        break;
      case 10:
        input.rake.basis_points = std::numeric_limits<std::uint32_t>::max();
        break;
      case 11:
        input.players[0].chips.refunded = 3;
        break;
    }
    CHECK(throws_as<std::invalid_argument>([&] { settle_contributions(input); }));
  }
  for (bool folded : {false, true}) {
    auto input = input_for({5, 3, 3});
    input.players[0].chips.folded = folded;
    CHECK(throws_as<std::invalid_argument>([&] { settle_contributions(input); }));
    CHECK(input.players[0].chips.contributed == 5);
    CHECK(input.players[0].chips.refunded == 0);
  }
  auto input = input_for({1, 3, 3});
  input.players[1].chips.folded = true;
  input.players[2].chips.folded = true;
  CHECK(throws_as<std::invalid_argument>([&] { settle_contributions(input); }));
  input = input_for({2, 2});
  input.players[0].chips.folded = true;
  input.players[1].chips.folded = true;
  CHECK(throws_as<std::invalid_argument>([&] { settle_contributions(input); }));

  for (Chips bad : {kMaxHeadsUpChips + 1, std::numeric_limits<Chips>::max()})
    for (int field = 0; field < 5; ++field) {
      input = input_for({2, 2});
      if (field == 0)
        input.players[0].chips.stack = bad;
      else if (field == 1)
        input.players[0].chips.contributed = bad;
      else if (field == 2)
        input.players[0].chips.refunded = bad;
      else if (field == 3)
        input.players[0].chips.street_committed = bad;
      else
        input.rake.cap = bad;
      CHECK(throws_as<std::overflow_error>([&] { settle_contributions(input); }));
    }
  input = input_for({kMaxHeadsUpChips / 2 + 1, kMaxHeadsUpChips / 2 + 1});
  CHECK(throws_as<std::overflow_error>([&] { settle_contributions(input); }));
  input = input_for({1, 1});
  input.players[0].chips.stack = kMaxHeadsUpChips - 1;
  CHECK(throws_as<std::overflow_error>([&] { settle_contributions(input); }));
  input.players[0].chips.stack = kMaxHeadsUpChips - 2;
  CHECK(settle_contributions(input).final_stacks[0] == kMaxHeadsUpChips);

  const Chips half = kMaxHeadsUpChips / 2;
  input = input_for({1, half, half});
  input.rake = {RakeRule::PotPercentageFloor, 5000, kMaxHeadsUpChips, false};
  auto result = settle_contributions(input);
  CHECK(result.pot == kMaxHeadsUpChips);
  CHECK(result.rake == half);
  CHECK(result.layers[0].rake == 1);
  CHECK(result.layers[1].rake == half - 1);
  CHECK((result.awards == std::vector<Chips>{2, half - 1, 0}));
  CHECK(check_accounting(input, result) == 0);
  for (std::uint32_t bps : {0U, 1U, 3333U, 9999U, 10000U}) {
    input.rake.basis_points = bps;
    result = settle_contributions(input);
    const Chips expected =
        (kMaxHeadsUpChips / 10000) * bps + ((kMaxHeadsUpChips % 10000) * bps) / 10000;
    CHECK(result.rake == expected);
    CHECK(check_accounting(input, result) == 0);
  }
  // Gross/refund totals are bounded even when the net pot is tiny.
  input = input_for({kMaxHeadsUpChips, 1});
  input.players[0].chips.refunded = kMaxHeadsUpChips - 1;
  CHECK(throws_as<std::overflow_error>([&] { settle_contributions(input); }));
  return 0;
}

struct ReferenceLayer {
  Chips level = 0;
  Chips amount = 0;
  Chips rake = 0;
  unsigned contributors = 0;
  unsigned eligible = 0;
  std::vector<std::size_t> winners;
};

struct Reference {
  Chips rake = 0;
  std::vector<Chips> awards;
  std::vector<ReferenceLayer> layers;
};

// Enumerate individual chip heights, coalescing identical funder sets. The
// oracle never queries native layers, winner selection, or acceptance rules.
std::optional<Reference> reference_small(const SettlementInput& input) {
  Reference result;
  const auto count = input.players.size();
  result.awards.resize(count);
  for (Chips chip = 1; chip <= 4; ++chip) {
    unsigned contributors = 0;
    unsigned eligible = 0;
    unsigned funders = 0;
    for (std::size_t p = 0; p < count; ++p) {
      const auto& chips = input.players[p].chips;
      if (chips.contributed - chips.refunded >= chip) {
        contributors |= 1U << p;
        ++funders;
        if (!chips.folded)
          eligible |= 1U << p;
      }
    }
    if (contributors == 0)
      break;
    // The library's layered invariants reject two shapes:
    //  - a layer reached by fewer than two gross contributors (the
    //    "unmarked unmatched contribution" precondition);
    //  - a layer with contributors but no eligible (unfolded) winner.
    // A third precondition is a contested layer (two or more eligible seats)
    // missing a showdown score; a SOLE eligible winner takes the layer
    // without one, however many folded seats funded it.
    if (funders < 2 || eligible == 0)
      return std::nullopt;
    if (__builtin_popcount(eligible) > 1) {
      bool contested_without_score = false;
      for (unsigned mask = eligible; mask != 0; mask &= mask - 1)
        if (!input.players[__builtin_ctz(mask)].showdown_score)
          contested_without_score = true;
      if (contested_without_score)
        return std::nullopt;
    }
    if (result.layers.empty() || result.layers.back().contributors != contributors)
      result.layers.push_back({0, 0, 0, contributors, eligible, {}});
    result.layers.back().level = chip;
    result.layers.back().amount += funders;
  }
  Chips pot = 0;
  for (const auto& layer : result.layers)
    pot += layer.amount;
  result.rake = input.rake.no_flop_no_drop && !input.flop_dealt
                    ? 0
                    : std::min(input.rake.cap, pot * input.rake.basis_points / 10000);
  Chips allocated = 0;
  for (auto& layer : result.layers) {
    layer.rake = result.rake * layer.amount / pot;
    allocated += layer.rake;
  }
  // Repeatedly pay the greatest remaining proportional debt; signed debt
  // becomes negative once a layer receives its extra unit.
  while (allocated < result.rake) {
    std::size_t best = 0;
    std::int64_t debt = -1;
    for (std::size_t i = 0; i < result.layers.size(); ++i) {
      const auto& layer = result.layers[i];
      const auto candidate = static_cast<std::int64_t>(result.rake * layer.amount) -
                             static_cast<std::int64_t>(layer.rake * pot);
      if (candidate > debt) {
        best = i;
        debt = candidate;
      }
    }
    ++result.layers[best].rake;
    ++allocated;
  }
  for (auto& layer : result.layers) {
    // A sole eligible winner takes the layer without a score. The winner loop
    // below still has to emit that one seat in clockwise order while never
    // dereferencing the absent optional; only a contested layer compares.
    if (__builtin_popcount(layer.eligible) == 1) {
      layer.winners = {static_cast<std::size_t>(__builtin_ctz(layer.eligible))};
    } else {
      for (std::size_t offset = 1; offset <= input.seat_count; ++offset) {
        const auto seat = (input.button + offset) % input.seat_count;
        for (std::size_t p = 0; p < count; ++p) {
          if (input.players[p].seat != seat || (layer.eligible & (1U << p)) == 0)
            continue;
          bool beaten = false;
          for (std::size_t other = 0; other < count; ++other)
            if ((layer.eligible & (1U << other)) != 0 &&
                *input.players[other].showdown_score > *input.players[p].showdown_score)
              beaten = true;
          if (!beaten)
            layer.winners.push_back(p);
        }
      }
    }
    const Chips available = layer.amount - layer.rake;
    for (Chips chip = 0; chip < available; ++chip)
      ++result.awards[layer.winners[chip % layer.winners.size()]];
  }
  return result;
}

int test_exhaustive_small_contributions() {
  std::size_t accepted = 0;
  std::size_t rejected = 0;
  std::size_t single_eligible = 0;
  for (std::size_t count = 2; count <= 6; ++count) {
    unsigned combinations = 1;
    for (std::size_t p = 0; p < count; ++p)
      combinations *= 5;
    for (unsigned code = 0; code < combinations; ++code) {
      std::vector<Chips> contributions(count);
      unsigned remaining = code;
      for (auto& contribution : contributions) {
        contribution = remaining % 5;
        remaining /= 5;
      }
      for (unsigned folded = 0; folded < (1U << count); ++folded) {
        auto input = input_for(contributions);
        input.button = (code + folded) % count;
        const unsigned mode = (code + folded) % 4;
        input.rake = {RakeRule::PotPercentageFloor, mode * 3333U, mode == 3 ? 2U : 100U, false};
        for (std::size_t p = 0; p < count; ++p) {
          input.players[p].seat = count - 1 - p;
          input.players[p].chips.folded = (folded & (1U << p)) != 0;
          input.players[p].showdown_score =
              mode == 0 ? 0 : static_cast<std::uint32_t>((p + mode) % 3);
          // Add independently recorded refunds without changing net contributions.
          const Chips refund = (code + folded + p) % 3;
          input.players[p].chips.refunded = refund;
          input.players[p].chips.contributed += refund;
          input.players[p].chips.stack = refund + 1;
        }
        const auto expected = reference_small(input);
        if (!expected) {
          bool invalid = false;
          bool logic = false;
          try {
            settle_contributions(input);
          } catch (const std::invalid_argument&) {
            invalid = true;
          } catch (const std::logic_error&) {
            logic = true;
          }
          CHECK(invalid || logic);
          ++rejected;
          continue;
        }
        const auto actual = settle_contributions(input);
        CHECK(actual.awards == expected->awards);
        CHECK(actual.rake == expected->rake);
        CHECK(actual.layers.size() == expected->layers.size());
        CHECK(check_accounting(input, actual) == 0);
        for (std::size_t i = 0; i < actual.layers.size(); ++i) {
          const auto& layer = actual.layers[i];
          const auto& ref = expected->layers[i];
          CHECK(layer.contribution_level == ref.level);
          CHECK(layer.amount == ref.amount);
          CHECK(layer.rake == ref.rake);
          CHECK(layer.winners == ref.winners);
          unsigned contributors = 0;
          unsigned eligible = 0;
          for (auto p : layer.contributors)
            contributors |= 1U << p;
          for (auto p : layer.eligible)
            eligible |= 1U << p;
          CHECK(contributors == ref.contributors);
          CHECK(eligible == ref.eligible);
          if (layer.eligible.size() == 1)
            ++single_eligible;
        }
        ++accepted;
      }
    }
  }
  CHECK(accepted > 100000);
  CHECK(rejected > 100000);
  CHECK(single_eligible > 10000);
  std::printf("settlement exhaustive: accepted=%zu rejected=%zu single-eligible=%zu\n", accepted,
              rejected, single_eligible);
  return 0;
}

int test_seven_to_ten_seat_extension() {
  // RFC 0008 stage 2 raises the contribution ledger from 6 to 10 seats. The
  // 2..6 exhaustive grid below stays byte-identical; full enumeration at 7+
  // seats is not feasible (5^10 roots times the fold masks), so the extension
  // is a DETERMINISTIC sample (fixed LCG, fixed seed list) driven against
  // the independently written `reference_small`, plus conservation checks
  // this test computes itself. The bit-mask oracle supports up to 31 seats,
  // so nothing in it had to widen.
  struct Lcg {
    std::uint64_t state = 0;
    unsigned next(unsigned bound) {
      // Deterministic 64-bit LCG (Knuth); every case is reproducible.
      state = state * 6364136223846793005ULL + 1442695040888963407ULL;
      return static_cast<unsigned>(state >> 33) % bound;
    }
  };
  std::size_t sampled = 0;
  std::size_t logic_rejected = 0;
  const char* stage_env = std::getenv("BS_SETTLE_STAGE");
  for (std::size_t count = 7; count <= kMaxContributionSeats; ++count) {
    if (stage_env && count < 7)
      continue;
    for (std::uint64_t seed :
         {std::uint64_t{0x123456789abcdef0ULL}, std::uint64_t{0x0fedcba987654321ULL},
          std::uint64_t{0x9e3779b97f4a7c15ULL}}) {
      Lcg rng{(seed ^ (count * 0x9e3779b97f4a7c15ULL)) + 1};
      for (unsigned trial = 0; trial < 400; ++trial) {
        std::vector<Chips> contributions(count);
        for (std::size_t p = 0; p < count; ++p)
          contributions[p] = rng.next(5);  // 0..4 keeps the tree to 4 layers
        SettlementInput input = input_for(contributions);
        const unsigned mask_limit = 1U << static_cast<unsigned>(count);
        const unsigned folded = static_cast<unsigned>(rng.next(mask_limit));
        const unsigned mode = rng.next(4);
        input.button = rng.next(static_cast<unsigned>(count));
        input.rake = {RakeRule::PotPercentageFloor, mode * 3333U, mode == 3 ? 2U : 100U, false};
        for (std::size_t p = 0; p < count; ++p) {
          input.players[p].seat = count - 1 - p;
          input.players[p].chips.folded = (folded & (1U << p)) != 0;
          input.players[p].showdown_score =
              mode == 0
                  ? std::nullopt
                  : std::make_optional(static_cast<std::uint32_t>((p + mode + rng.next(3)) % 4));
          const Chips refund = rng.next(3);
          input.players[p].chips.refunded = refund;
          input.players[p].chips.contributed += refund;
          input.players[p].chips.stack = refund + 1;
        }
        const auto expected = reference_small(input);
        if (!expected) {
          // Every shape the reference rejects is invalid input to the
          // library: a sole gross contributor, a layer with no unfolded
          // winner, or a contested layer missing a score.
          CHECK(throws_as<std::invalid_argument>([&] { settle_contributions(input); }));
          ++logic_rejected;
          continue;
        }
        const ContributionSettlement actual = settle_contributions(input);
        CHECK(actual.awards == expected->awards);
        CHECK(actual.rake == expected->rake);
        CHECK(actual.layers.size() == expected->layers.size());
        CHECK(check_accounting(input, actual) == 0);
        // Independent layer guards the oracle's award vector does not cover:
        // a folded seat never occupies a winners slot and no folded seat is
        // eligible for a contested layer.
        for (const auto& layer : actual.layers) {
          for (std::size_t winner : layer.winners)
            CHECK(!input.players[winner].chips.folded);
          for (std::size_t p : layer.eligible)
            CHECK(!input.players[p].chips.folded);
        }
        ++sampled;
      }
    }
  }
  // 4,800 trials over the 7..10 range; structurally rejected input (the
  // library's invalid_argument surfaces: no eligible winner, a sole gross
  // contributor, or a contested layer missing a score) is classified and
  // counted rather than compared.
  std::printf("settlement 7..10 extension: sampled=%zu structurally_rejected=%zu\n", sampled,
              logic_rejected);
  CHECK(sampled + logic_rejected == 4800);
  CHECK(sampled > 1000);
  CHECK(logic_rejected > 0);
  return 0;
}

}  // namespace

int main() {
  try {
    CHECK(test_fixed_payouts() == 0);
    CHECK(test_refunds_and_score_scope() == 0);
    CHECK(test_rake() == 0);
    CHECK(test_rejections_and_limits() == 0);
    CHECK(test_exhaustive_small_contributions() == 0);
    CHECK(test_seven_to_ten_seat_extension() == 0);
  } catch (const std::exception& error) {
    std::printf("Unexpected settlement exception: %s\n", error.what());
    return 1;
  }
  std::printf("test_settlement PASS\n");
  return 0;
}
