// Independent arithmetic regressions for the bounded RFC 0006 ICM milestone.
#include <algorithm>
#include <bs/icm.hpp>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <functional>
#include <limits>
#include <numeric>
#include <random>
#include <stdexcept>
#include <string>
#include <type_traits>
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

// Floating-point inputs, including NaN and infinity, are not domain amounts.
// Check list initialization rather than invoking an undefined float-to-int cast.
template <typename T>
concept StackAmount = requires(T value) { IcmPlayer{"player", value}; };

template <typename T>
concept PayoutAmount = requires(T value) { std::vector<std::uint64_t>{value}; };

static_assert(StackAmount<std::uint64_t>);
static_assert(PayoutAmount<std::uint64_t>);
static_assert(!StackAmount<double>);
static_assert(!PayoutAmount<double>);
static_assert(!StackAmount<std::int64_t>);
static_assert(!PayoutAmount<std::int64_t>);
static_assert(std::is_same_v<decltype(IcmPlayer{}.stack), std::uint64_t>);
static_assert(std::is_same_v<decltype(IcmPlayerUtility{}.awarded_prize), std::uint64_t>);
static_assert(std::is_same_v<decltype(IcmPlayerUtility{}.utility), double>);

bool near(long double actual, long double expected, long double scale = 1) {
  return std::isfinite(actual) && std::isfinite(expected) &&
         std::abs(actual - expected) <= 1e-10L * std::max(1.0L, std::abs(scale));
}

template <typename F>
bool rejects(F&& operation) {
  try {
    operation();
  } catch (const std::invalid_argument&) {
    return true;
  } catch (...) {
    return false;
  }
  return false;
}

IcmField field(const std::vector<std::uint64_t>& stacks,
               const std::vector<std::uint64_t>& payouts) {
  IcmField result{{}, payouts, "USD-cent", true};
  for (std::size_t i = 0; i < stacks.size(); ++i)
    result.players.push_back({"player-" + std::to_string(i), stacks[i]});
  return result;
}

std::uint64_t pool(const IcmField& input) {
  return std::accumulate(input.payouts.begin(), input.payouts.end(), std::uint64_t{0});
}

struct Oracle {
  std::vector<long double> equities;
  long double probability = 0;
  std::size_t permutations = 0;
};

// Enumerate complete finishing permutations, with no subsets, DP, or native
// validation/equity helpers. Each permutation contributes to every placement.
Oracle enumerate(const std::vector<IcmPlayer>& players, const std::vector<std::uint64_t>& payouts) {
  if (players.empty() || players.size() > 6 || payouts.size() != players.size())
    throw std::invalid_argument("oracle supports 1..6 complete players");
  std::vector<std::size_t> order(players.size());
  std::iota(order.begin(), order.end(), std::size_t{0});
  Oracle result{std::vector<long double>(players.size(), 0), 0, 0};
  do {
    long double probability = 1;
    for (std::size_t place = 0; place < order.size(); ++place) {
      std::uint64_t remaining = 0;
      for (std::size_t later = place; later < order.size(); ++later)
        remaining += players[order[later]].stack;
      probability *= static_cast<long double>(players[order[place]].stack) /
                     static_cast<long double>(remaining);
    }
    for (std::size_t place = 0; place < order.size(); ++place)
      result.equities[order[place]] += probability * static_cast<long double>(payouts[place]);
    result.probability += probability;
    ++result.permutations;
  } while (std::next_permutation(order.begin(), order.end()));
  return result;
}

int check_equities(const IcmField& input, bool use_oracle = true) {
  const auto actual = icm_equities(input);
  CHECK(actual.payout_unit == input.payout_unit);
  CHECK(actual.equities.size() == input.players.size());
  long double sum = 0;
  for (double value : actual.equities) {
    CHECK(std::isfinite(value));
    CHECK(value >= 0);
    CHECK(value <= static_cast<double>(pool(input)));
    sum += value;
  }
  CHECK(near(sum, pool(input), pool(input)));
  CHECK(icm_equities(input).equities == actual.equities);
  if (use_oracle) {
    const auto oracle = enumerate(input.players, input.payouts);
    CHECK(near(oracle.probability, 1));
    std::size_t factorial = 1;
    for (std::size_t n = 2; n <= input.players.size(); ++n)
      factorial *= n;
    CHECK(oracle.permutations == factorial);
    for (std::size_t i = 0; i < actual.equities.size(); ++i)
      CHECK(near(actual.equities[i], oracle.equities[i], oracle.equities[i]));
  }
  return 0;
}

int test_two_player_analytic() {
  for (std::uint64_t first = 1; first <= 12; ++first) {
    for (std::uint64_t second = 1; second <= 12; ++second) {
      for (const auto& payouts :
           std::vector<std::vector<std::uint64_t>>{{100, 0}, {100, 30}, {71, 71}, {0, 0}, {1, 0}}) {
        const auto input = field({first, second}, payouts);
        CHECK(check_equities(input) == 0);
        const auto actual = icm_equities(input);
        const long double denominator = static_cast<long double>(first + second);
        const long double expected = (static_cast<long double>(first) * payouts[0] +
                                      static_cast<long double>(second) * payouts[1]) /
                                     denominator;
        CHECK(near(actual.equities[0], expected, expected));
        CHECK(near(actual.equities[1], pool(input) - expected, pool(input) - expected));
      }
    }
  }
  const auto fractional = icm_equities(field({1, 2}, {1, 0}));
  CHECK(near(fractional.equities[0], 1.0L / 3));
  CHECK(near(fractional.equities[1], 2.0L / 3));
  return 0;
}

int test_symmetry_monotonicity_and_ten_players() {
  for (std::size_t count = 2; count <= kMaxIcmPlayers; ++count) {
    std::vector<std::uint64_t> payouts;
    for (std::size_t i = 0; i < count; ++i)
      payouts.push_back(static_cast<std::uint64_t>((count - i) * (count - i)));
    auto input = field(std::vector<std::uint64_t>(count, 13), payouts);
    CHECK(check_equities(input, count <= 6) == 0);
    auto actual = icm_equities(input);
    const long double equal = static_cast<long double>(pool(input)) / count;
    for (double value : actual.equities)
      CHECK(near(value, equal, equal));

    for (std::size_t i = 0; i < count; ++i)
      input.players[i].stack = static_cast<std::uint64_t>(i * i + 1);
    input.payouts.assign(count, 17);
    CHECK(check_equities(input, count <= 6) == 0);
    for (double value : icm_equities(input).equities)
      CHECK(near(value, 17));

    input.payouts = payouts;
    actual = icm_equities(input);
    for (std::size_t i = 1; i < count; ++i)
      CHECK(actual.equities[i] > actual.equities[i - 1]);
    auto changed = input;
    ++changed.payouts[0];
    const auto increased = icm_equities(changed);
    for (std::size_t i = 0; i < count; ++i)
      CHECK(increased.equities[i] > actual.equities[i]);

    changed = input;
    ++changed.players[0].stack;
    CHECK(icm_equities(changed).equities[0] > actual.equities[0]);
    changed = input;
    for (auto& player : changed.players)
      player.stack *= 19;
    const auto scaled = icm_equities(changed);
    for (std::size_t i = 0; i < count; ++i)
      CHECK(near(scaled.equities[i], actual.equities[i], actual.equities[i]));
    changed = input;
    for (auto& payout : changed.payouts)
      payout *= 19;
    const auto scaled_prizes = icm_equities(changed);
    for (std::size_t i = 0; i < count; ++i)
      CHECK(near(scaled_prizes.equities[i], 19 * actual.equities[i], scaled_prizes.equities[i]));

    changed = input;
    std::reverse(changed.players.begin(), changed.players.end());
    const auto reversed = icm_equities(changed);
    for (std::size_t i = 0; i < count; ++i)
      CHECK(near(reversed.equities[count - 1 - i], actual.equities[i], actual.equities[i]));
    changed = input;
    changed.payout_unit = "EUR-micro";
    CHECK(icm_equities(changed).payout_unit == "EUR-micro");
    CHECK(icm_equities(changed).equities == actual.equities);

    input.payouts.assign(count, 0);
    CHECK(check_equities(input, count <= 6) == 0);
    for (double value : icm_equities(input).equities)
      CHECK(value == 0);
    input.payouts[0] = 113;
    const auto winner_only = icm_equities(input);
    std::uint64_t chips = 0;
    for (const auto& player : input.players)
      chips += player.stack;
    for (std::size_t i = 0; i < count; ++i)
      CHECK(near(winner_only.equities[i],
                 113.0L * input.players[i].stack / static_cast<long double>(chips), 113));
  }
  return 0;
}

int test_factorial_oracle() {
  std::size_t fixtures = 0;
  std::size_t permutations = 0;
  for (std::size_t count = 2; count <= 6; ++count) {
    std::size_t combinations = 1;
    for (std::size_t i = 0; i < count; ++i)
      combinations *= 3;
    for (std::size_t code = 0; code < combinations; ++code) {
      std::vector<std::uint64_t> stacks(count);
      std::vector<std::uint64_t> payouts(count);
      auto digits = code;
      for (std::size_t i = 0; i < count; ++i) {
        stacks[i] = 1 + digits % 3;
        digits /= 3;
        payouts[i] = static_cast<std::uint64_t>((count - i) * (count - i) - 1);
      }
      const auto input = field(stacks, payouts);
      CHECK(check_equities(input) == 0);
      permutations += enumerate(input.players, input.payouts).permutations;
      ++fixtures;
    }
  }
  CHECK(fixtures == 1089);
  CHECK(permutations == 556164);
  for (std::uint64_t seed : {1ULL, 17ULL, 43ULL}) {
    std::mt19937_64 random(seed);
    for (std::size_t count = 2; count <= 6; ++count) {
      for (std::size_t trial = 0; trial < 8; ++trial) {
        std::vector<std::uint64_t> stacks(count);
        std::vector<std::uint64_t> payouts(count);
        for (std::size_t i = 0; i < count; ++i) {
          stacks[i] = 1 + random() % 1000000;
          payouts[i] = random() % 100000;
        }
        std::sort(payouts.begin(), payouts.end(), std::greater<std::uint64_t>());
        CHECK(check_equities(field(stacks, payouts)) == 0);
      }
    }
  }
  std::printf("ICM oracle: 1089 grid fixtures, 556164 permutations, 120 seeded fixtures\n");
  return 0;
}

int check_terminal(const IcmField& before, const std::vector<IcmPlayer>& after, IcmTieRule rule,
                   const std::vector<std::uint64_t>& expected_awards) {
  const auto actual = icm_terminal_utility(before, after, rule);
  CHECK(actual.payout_unit == before.payout_unit);
  CHECK(actual.players.size() == before.players.size());
  CHECK(expected_awards.size() == before.players.size());
  const auto pre_equities = enumerate(before.players, before.payouts);
  std::vector<IcmPlayer> survivors;
  for (const auto& player : after)
    if (player.stack != 0)
      survivors.push_back(player);
  const auto survivor_count = static_cast<std::ptrdiff_t>(survivors.size());
  const std::vector<std::uint64_t> payouts(before.payouts.begin(),
                                           before.payouts.begin() + survivor_count);
  const auto survivor_equities = enumerate(survivors, payouts);
  long double terminal_sum = 0;
  long double utility_sum = 0;
  std::uint64_t awarded = 0;
  for (std::size_t i = 0; i < actual.players.size(); ++i) {
    const auto& player = actual.players[i];
    CHECK(player.id == before.players[i].id);
    CHECK(player.awarded_prize == expected_awards[i]);
    CHECK(near(player.prehand_equity, pre_equities.equities[i], pool(before)));
    long double expected_survivor = 0;
    for (std::size_t j = 0; j < survivors.size(); ++j)
      if (survivors[j].id == player.id)
        expected_survivor = survivor_equities.equities[j];
    CHECK(near(player.survivor_equity, expected_survivor, pool(before)));
    const long double expected_terminal = expected_awards[i] + expected_survivor;
    CHECK(near(player.terminal_equity, expected_terminal, pool(before)));
    CHECK(near(player.utility, expected_terminal - pre_equities.equities[i], pool(before)));
    CHECK(player.utility == player.terminal_equity - player.prehand_equity);
    CHECK(std::isfinite(player.utility));
    terminal_sum += player.terminal_equity;
    utility_sum += player.utility;
    awarded += player.awarded_prize;
  }
  CHECK(near(terminal_sum, pool(before), pool(before)));
  CHECK(near(utility_sum, 0, pool(before)));
  CHECK(awarded + std::accumulate(payouts.begin(), payouts.end(), std::uint64_t{0}) ==
        pool(before));
  auto reordered = after;
  std::reverse(reordered.begin(), reordered.end());
  const auto repeated = icm_terminal_utility(before, reordered, rule);
  for (std::size_t i = 0; i < actual.players.size(); ++i) {
    CHECK(repeated.players[i].id == actual.players[i].id);
    CHECK(repeated.players[i].awarded_prize == actual.players[i].awarded_prize);
    CHECK(repeated.players[i].survivor_equity == actual.players[i].survivor_equity);
    CHECK(repeated.players[i].utility == actual.players[i].utility);
  }
  auto reordered_before = before;
  std::reverse(reordered_before.players.begin(), reordered_before.players.end());
  const auto reversed = icm_terminal_utility(reordered_before, after, rule);
  for (std::size_t i = 0; i < actual.players.size(); ++i) {
    const auto& player = reversed.players[actual.players.size() - 1 - i];
    CHECK(player.id == actual.players[i].id);
    CHECK(player.awarded_prize == actual.players[i].awarded_prize);
    CHECK(near(player.survivor_equity, actual.players[i].survivor_equity, pool(before)));
    CHECK(near(player.utility, actual.players[i].utility, pool(before)));
  }
  return 0;
}

int test_terminal_busts() {
  auto input = field({20, 30, 50}, {100, 60, 10});
  CHECK(check_terminal(input, input.players, IcmTieRule::RejectTies, {0, 0, 0}) == 0);
  for (const auto& player : icm_terminal_utility(input, input.players).players)
    CHECK(player.utility == 0);
  auto after = input.players;
  after[0].stack = 25;
  after[1].stack = 25;
  CHECK(check_terminal(input, after, IcmTieRule::RejectTies, {0, 0, 0}) == 0);
  after = input.players;
  after[0].stack = 0;
  after[1].stack = 50;
  CHECK(check_terminal(input, after, IcmTieRule::RejectTies, {10, 0, 0}) == 0);
  after[1].stack = 0;
  after[2].stack = 100;
  CHECK(check_terminal(input, after, IcmTieRule::RejectTies, {10, 60, 0}) == 0);
  const auto winner = icm_terminal_utility(input, after);
  CHECK(winner.players[2].survivor_equity == 100);
  CHECK(winner.players[2].awarded_prize == 0);

  input = field({30, 10, 20, 40}, {100, 60, 30, 10});
  after = input.players;
  after[0].stack = 0;
  after[1].stack = 0;
  after[2].stack = 40;
  after[3].stack = 60;
  CHECK(check_terminal(input, after, IcmTieRule::RejectTies, {30, 10, 0, 0}) == 0);
  after[2].stack = 0;
  after[3].stack = 100;
  CHECK(check_terminal(input, after, IcmTieRule::RejectTies, {60, 10, 30, 0}) == 0);

  input = field({1, 2}, {1, 0});
  after = input.players;
  after[0].stack = 3;
  after[1].stack = 0;
  CHECK(check_terminal(input, after, IcmTieRule::RejectTies, {0, 0}) == 0);
  CHECK(icm_terminal_utility(input, after).players[0].terminal_equity == 1);
  input.payouts = {0, 0};
  CHECK(check_terminal(input, after, IcmTieRule::RejectTies, {0, 0}) == 0);
  for (const auto& player : icm_terminal_utility(input, after).players) {
    CHECK(player.prehand_equity == 0);
    CHECK(player.terminal_equity == 0);
    CHECK(player.utility == 0);
  }
  return 0;
}

int test_tied_busts_and_byte_order() {
  auto input = field({10, 10, 30, 50}, {100, 60, 11, 4});
  input.players[0].id = "z";
  input.players[1].id = "a";
  auto after = input.players;
  after[0].stack = 0;
  after[1].stack = 0;
  after[2].stack = 40;
  after[3].stack = 60;
  CHECK(rejects([&] { icm_terminal_utility(input, after); }));
  CHECK(check_terminal(input, after, IcmTieRule::StableIdByteOrder, {7, 8, 0, 0}) == 0);
  input.payouts = {100, 60, 10, 4};
  CHECK(rejects([&] { icm_terminal_utility(input, after); }));
  CHECK(check_terminal(input, after, IcmTieRule::StableIdByteOrder, {7, 7, 0, 0}) == 0);
  input.payouts = {100, 60, 0, 0};
  CHECK(rejects([&] { icm_terminal_utility(input, after); }));
  CHECK(check_terminal(input, after, IcmTieRule::StableIdByteOrder, {0, 0, 0, 0}) == 0);

  // Signed-char ordering, numeric-ID ordering, and locale ordering are incorrect.
  input = field({10, 10, 10, 10, 60}, {100, 7, 5, 3, 2});
  input.players[0].id = std::string(1, static_cast<char>(0x80));
  input.players[1].id = "2";
  input.players[2].id = "10";
  input.players[3].id = "1";
  after = input.players;
  for (std::size_t i = 0; i < 4; ++i)
    after[i].stack = 0;
  after[4].stack = 100;
  CHECK(check_terminal(input, after, IcmTieRule::StableIdByteOrder, {4, 4, 4, 5, 0}) == 0);
  input.payouts = {100, 8, 6, 3, 2};
  CHECK(check_terminal(input, after, IcmTieRule::StableIdByteOrder, {4, 5, 5, 5, 0}) == 0);

  // Distinct tied groups occupy distinct places; they do not share all bust prizes.
  input = field({10, 20, 10, 20, 40, 50}, {100, 60, 31, 20, 9, 4});
  after = input.players;
  for (std::size_t i = 0; i < 4; ++i)
    after[i].stack = 0;
  after[4].stack = 70;
  after[5].stack = 80;
  CHECK(check_terminal(input, after, IcmTieRule::StableIdByteOrder, {7, 26, 6, 25, 0, 0}) == 0);
  return 0;
}

int rejects_field(const IcmField& input) {
  CHECK(rejects([&] { icm_equities(input); }));
  CHECK(rejects([&] { icm_terminal_utility(input, input.players); }));
  return 0;
}

int test_malformed_fields() {
  const auto good = field({10, 20, 30}, {100, 60, 0});
  for (std::size_t count : {std::size_t{0}, std::size_t{1}, std::size_t{11}})
    CHECK(rejects_field(field(std::vector<std::uint64_t>(count, 1),
                              std::vector<std::uint64_t>(count, 1))) == 0);
  auto bad = good;
  bad.complete = false;
  CHECK(rejects_field(bad) == 0);
  bad = good;
  bad.payout_unit.clear();
  CHECK(rejects_field(bad) == 0);
  for (std::size_t i = 0; i < good.players.size(); ++i) {
    bad = good;
    bad.players[i].id.clear();
    CHECK(rejects_field(bad) == 0);
    bad = good;
    bad.players[i].stack = 0;
    CHECK(rejects_field(bad) == 0);
    for (std::size_t j = 0; j < i; ++j) {
      bad = good;
      bad.players[i].id = bad.players[j].id;
      CHECK(rejects_field(bad) == 0);
    }
  }
  for (const auto& payouts : std::vector<std::vector<std::uint64_t>>{
           {}, {100}, {100, 60}, {100, 60, 0, 0}, {60, 100, 0}, {100, 0, 60}}) {
    bad = good;
    bad.payouts = payouts;
    CHECK(rejects_field(bad) == 0);
  }
  for (std::uint64_t invalid : {kMaxIcmAmount + 1, std::numeric_limits<std::uint64_t>::max()}) {
    bad = good;
    bad.players[0].stack = invalid;
    CHECK(rejects_field(bad) == 0);
    bad = good;
    bad.payouts[0] = invalid;
    CHECK(rejects_field(bad) == 0);
  }
  CHECK(rejects_field(field({kMaxIcmAmount, 1}, {1, 0})) == 0);
  CHECK(rejects_field(field({1, 1}, {kMaxIcmAmount, 1})) == 0);
  CHECK(rejects_field(field({kMaxIcmAmount, kMaxIcmAmount}, {1, 0})) == 0);
  CHECK(rejects_field(field({1, 1}, {kMaxIcmAmount, kMaxIcmAmount})) == 0);
  CHECK(!near(std::numeric_limits<double>::quiet_NaN(), 0));
  CHECK(!near(std::numeric_limits<double>::infinity(), 0));
  CHECK(!near(0, std::numeric_limits<double>::quiet_NaN()));
  return 0;
}

int test_malformed_terminals() {
  const auto input = field({10, 20, 30}, {100, 60, 0});
  auto after = input.players;
  after.pop_back();
  CHECK(rejects([&] { icm_terminal_utility(input, after); }));
  after = input.players;
  after.push_back({"historical-player", 0});
  CHECK(rejects([&] { icm_terminal_utility(input, after); }));
  for (std::size_t i = 0; i < input.players.size(); ++i) {
    after = input.players;
    after[i].id = "unknown-remote-player";
    CHECK(rejects([&] { icm_terminal_utility(input, after); }));
    after[i].id.clear();
    CHECK(rejects([&] { icm_terminal_utility(input, after); }));
    after = input.players;
    after[i].id = after[(i + 1) % after.size()].id;
    CHECK(rejects([&] { icm_terminal_utility(input, after); }));
    after = input.players;
    ++after[i].stack;
    CHECK(rejects([&] { icm_terminal_utility(input, after); }));
    after = input.players;
    --after[i].stack;
    CHECK(rejects([&] { icm_terminal_utility(input, after); }));
    for (std::uint64_t invalid :
         {kMaxIcmAmount, kMaxIcmAmount + 1, std::numeric_limits<std::uint64_t>::max()}) {
      after = input.players;
      after[i].stack = invalid;
      CHECK(rejects([&] { icm_terminal_utility(input, after); }));
    }
  }
  after = input.players;
  for (auto& player : after)
    player.stack = 0;
  CHECK(rejects([&] { icm_terminal_utility(input, after); }));
  CHECK(rejects([&] { icm_terminal_utility(input, input.players, static_cast<IcmTieRule>(99)); }));
  return 0;
}

int test_numeric_boundaries() {
  CHECK(kMaxIcmAmount == 9007199254740991ULL);
  CHECK(kMaxIcmPlayers == 10);
  auto input = field({kMaxIcmAmount - 1, 1}, {kMaxIcmAmount, 0});
  CHECK(check_equities(input) == 0);
  auto result = icm_equities(input);
  CHECK(near(result.equities[1], 1));
  CHECK(near(result.equities[0], kMaxIcmAmount - 1, kMaxIcmAmount));
  input = field({kMaxIcmAmount - 2, 1, 1}, {kMaxIcmAmount - 3, 2, 1});
  CHECK(check_equities(input) == 0);
  input = field({1, 1, 1}, {kMaxIcmAmount / 3, kMaxIcmAmount / 3, kMaxIcmAmount / 3});
  CHECK(check_equities(input) == 0);
  for (double value : icm_equities(input).equities)
    CHECK(near(value, kMaxIcmAmount / 3, kMaxIcmAmount));

  input = field({1, 1, kMaxIcmAmount - 2}, {kMaxIcmAmount - 3, 2, 1});
  auto after = input.players;
  after[0].stack = 0;
  after[1].stack = 0;
  after[2].stack = kMaxIcmAmount;
  CHECK(check_terminal(input, after, IcmTieRule::StableIdByteOrder, {2, 1, 0}) == 0);

  // Splitting a large odd prize total must never pass through floating rounding.
  input = field({1, 1, 1}, {kMaxIcmAmount / 3 + 1, kMaxIcmAmount / 3, kMaxIcmAmount / 3});
  after = input.players;
  after[0].stack = 0;
  after[1].stack = 0;
  after[2].stack = 3;
  CHECK(check_terminal(input, after, IcmTieRule::StableIdByteOrder,
                       {kMaxIcmAmount / 3, kMaxIcmAmount / 3, 0}) == 0);
  input.payouts = {kMaxIcmAmount / 3 + 1, kMaxIcmAmount / 3, kMaxIcmAmount / 3 - 1};
  CHECK(check_terminal(input, after, IcmTieRule::StableIdByteOrder,
                       {kMaxIcmAmount / 3, kMaxIcmAmount / 3 - 1, 0}) == 0);

  input = field(std::vector<std::uint64_t>(10, 1), std::vector<std::uint64_t>(10, 0));
  input.players[0].stack = kMaxIcmAmount - 9;
  input.payouts[0] = kMaxIcmAmount;
  CHECK(check_equities(input, false) == 0);
  result = icm_equities(input);
  for (std::size_t i = 1; i < 10; ++i)
    CHECK(near(result.equities[i], 1));
  after = input.players;
  after[0].stack = kMaxIcmAmount;
  for (std::size_t i = 1; i < 10; ++i)
    after[i].stack = 0;
  CHECK(rejects([&] { icm_terminal_utility(input, after); }));
  const auto terminal = icm_terminal_utility(input, after, IcmTieRule::StableIdByteOrder);
  CHECK(terminal.players[0].terminal_equity == static_cast<double>(kMaxIcmAmount));
  long double utility_sum = 0;
  for (std::size_t i = 0; i < 10; ++i) {
    CHECK(terminal.players[i].awarded_prize == 0);
    CHECK(std::isfinite(terminal.players[i].utility));
    if (i != 0)
      CHECK(terminal.players[i].terminal_equity == 0);
    utility_sum += terminal.players[i].utility;
  }
  CHECK(near(utility_sum, 0, kMaxIcmAmount));
  return 0;
}

}  // namespace

int main() {
  try {
    CHECK(test_two_player_analytic() == 0);
    CHECK(test_symmetry_monotonicity_and_ten_players() == 0);
    CHECK(test_factorial_oracle() == 0);
    CHECK(test_terminal_busts() == 0);
    CHECK(test_tied_busts_and_byte_order() == 0);
    CHECK(test_malformed_fields() == 0);
    CHECK(test_malformed_terminals() == 0);
    CHECK(test_numeric_boundaries() == 0);
  } catch (const std::exception& error) {
    std::printf("Unexpected ICM exception: %s\n", error.what());
    return 1;
  }
  std::printf("test_icm PASS\n");
  return 0;
}
