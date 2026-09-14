#include <algorithm>
#include <bit>
#include <bs/icm.hpp>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace bs::poker {
namespace {

void require(bool condition, const char* message) {
  if (!condition)
    throw std::invalid_argument(message);
}

std::uint64_t bounded_add(std::uint64_t total, std::uint64_t value) {
  require(value <= kMaxIcmAmount - total, "ICM amount exceeds exact numeric profile");
  return total + value;
}

std::uint64_t validate_players(const std::vector<IcmPlayer>& players, bool positive) {
  std::uint64_t total = 0;
  for (std::size_t i = 0; i < players.size(); ++i) {
    require(!players[i].id.empty(), "ICM requires nonempty stable player IDs");
    for (std::size_t j = 0; j < i; ++j)
      require(players[j].id != players[i].id, "duplicate ICM player ID");
    require(!positive || players[i].stack > 0, "ICM requires positive remaining stacks");
    total = bounded_add(total, players[i].stack);
  }
  return total;
}

struct Totals {
  std::uint64_t chips;
  std::uint64_t prizes;
};

Totals validate_field(const IcmField& field) {
  require(field.complete, "ICM requires the complete remaining field");
  require(field.players.size() >= 2 && field.players.size() <= kMaxIcmPlayers,
          "ICM supports 2..10 remaining players");
  require(field.payouts.size() == field.players.size(),
          "ICM requires one explicit payout per remaining place");
  require(!field.payout_unit.empty(), "ICM requires a separate payout atomic unit");
  Totals totals{validate_players(field.players, true), 0};
  for (std::size_t i = 0; i < field.payouts.size(); ++i) {
    if (i != 0)
      require(field.payouts[i - 1] >= field.payouts[i], "ICM payouts must be nonincreasing");
    totals.prizes = bounded_add(totals.prizes, field.payouts[i]);
  }
  return totals;
}

void check_sum(long double sum, long double expected, std::uint64_t prize_pool) {
  const long double tolerance = 1e-10L * std::max(1.0L, static_cast<long double>(prize_pool));
  if (!std::isfinite(sum) || std::abs(sum - expected) > tolerance)
    throw std::logic_error("ICM prize conservation failed");
}

// Each subset holds the probability that exactly its players occupied the
// first popcount(subset) places, aggregating every ordering of those finishers.
std::vector<double> expected_equities(const std::vector<IcmPlayer>& players,
                                      const std::vector<std::uint64_t>& payouts,
                                      std::uint64_t total_chips) {
  const std::size_t count = players.size();
  const std::size_t states = std::size_t{1} << count;
  std::vector<std::uint64_t> subset_chips(states, 0);
  std::vector<long double> probability(states, 0);
  std::vector<long double> equity(count, 0);
  probability[0] = 1;
  for (std::size_t mask = 1; mask < states; ++mask) {
    const std::size_t player = static_cast<std::size_t>(std::countr_zero(mask));
    subset_chips[mask] = subset_chips[mask & (mask - 1)] + players[player].stack;
  }
  for (std::size_t mask = 0; mask + 1 < states; ++mask) {
    const std::size_t place = static_cast<std::size_t>(std::popcount(mask));
    // Subtract exact integers before conversion, including extremely short stacks.
    const auto remaining = total_chips - subset_chips[mask];
    for (std::size_t player = 0; player < count; ++player) {
      const std::size_t bit = std::size_t{1} << player;
      if ((mask & bit) != 0)
        continue;
      const long double next = probability[mask] * static_cast<long double>(players[player].stack) /
                               static_cast<long double>(remaining);
      probability[mask | bit] += next;
      equity[player] += next * static_cast<long double>(payouts[place]);
    }
  }
  std::vector<double> result;
  result.reserve(count);
  for (long double value : equity) {
    const double converted = static_cast<double>(value);
    if (!std::isfinite(converted) || converted < 0)
      throw std::logic_error("ICM produced invalid expected equity");
    result.push_back(converted);
  }
  return result;
}

void check_equities(const std::vector<double>& equities, std::uint64_t prize_pool) {
  long double sum = 0;
  for (double equity : equities)
    sum += equity;
  check_sum(sum, static_cast<long double>(prize_pool), prize_pool);
}

bool id_bytes_less(const std::string& left, const std::string& right) {
  return std::lexicographical_compare(
      left.begin(), left.end(), right.begin(), right.end(),
      [](char a, char b) { return static_cast<unsigned char>(a) < static_cast<unsigned char>(b); });
}

}  // namespace

IcmEquities icm_equities(const IcmField& field) {
  const auto totals = validate_field(field);
  auto equities = expected_equities(field.players, field.payouts, totals.chips);
  check_equities(equities, totals.prizes);
  return {field.payout_unit, std::move(equities)};
}

IcmTerminalUtility icm_terminal_utility(const IcmField& prehand,
                                        const std::vector<IcmPlayer>& posthand,
                                        IcmTieRule tie_rule) {
  const auto totals = validate_field(prehand);
  require(tie_rule == IcmTieRule::RejectTies || tie_rule == IcmTieRule::StableIdByteOrder,
          "unsupported ICM elimination tie rule");
  require(posthand.size() == prehand.players.size(), "ICM terminal field size changed");
  const auto post_chips = validate_players(posthand, false);
  require(post_chips == totals.chips, "ICM terminal stacks must conserve chips");

  const std::size_t count = prehand.players.size();
  std::vector<std::size_t> busted;
  std::vector<std::size_t> survivor_indices;
  std::vector<IcmPlayer> survivors;
  for (std::size_t i = 0; i < count; ++i) {
    const auto found = std::find_if(posthand.begin(), posthand.end(), [&](const IcmPlayer& p) {
      return p.id == prehand.players[i].id;
    });
    require(found != posthand.end(), "ICM terminal player IDs differ from pre-hand IDs");
    if (found->stack == 0) {
      busted.push_back(i);
    } else {
      survivor_indices.push_back(i);
      survivors.push_back(*found);
    }
  }
  require(!survivors.empty(), "ICM terminal field has no survivor");
  std::sort(busted.begin(), busted.end(), [&](std::size_t a, std::size_t b) {
    if (prehand.players[a].stack != prehand.players[b].stack)
      return prehand.players[a].stack < prehand.players[b].stack;
    return id_bytes_less(prehand.players[a].id, prehand.players[b].id);
  });

  IcmTerminalUtility result{prehand.payout_unit, {}};
  const auto before = expected_equities(prehand.players, prehand.payouts, totals.chips);
  check_equities(before, totals.prizes);
  for (std::size_t i = 0; i < count; ++i)
    result.players.push_back({prehand.players[i].id, before[i]});

  // Traverse the worst finish first. A tied group shares exactly its occupied
  // places, with integer remainders going to ascending unsigned ID-byte order.
  std::uint64_t awarded = 0;
  for (std::size_t begin = 0; begin < busted.size();) {
    std::size_t end = begin + 1;
    while (end < busted.size() &&
           prehand.players[busted[end]].stack == prehand.players[busted[begin]].stack)
      ++end;
    const std::size_t group_size = end - begin;
    require(group_size == 1 || tie_rule == IcmTieRule::StableIdByteOrder,
            "tied ICM eliminations require the declared stable-ID byte-order rule");
    std::uint64_t group_prizes = 0;
    for (std::size_t i = begin; i < end; ++i)
      group_prizes = bounded_add(group_prizes, prehand.payouts[count - 1 - i]);
    const auto divisor = static_cast<std::uint64_t>(group_size);
    for (std::size_t i = begin; i < end; ++i) {
      const std::uint64_t prize =
          group_prizes / divisor + (i - begin < group_prizes % divisor ? 1 : 0);
      result.players[busted[i]].awarded_prize = prize;
      awarded = bounded_add(awarded, prize);
    }
    begin = end;
  }

  const std::vector<std::uint64_t> remaining_payouts(
      prehand.payouts.begin(),
      prehand.payouts.begin() + static_cast<std::ptrdiff_t>(survivors.size()));
  const auto after = expected_equities(survivors, remaining_payouts, totals.chips);
  check_equities(after, totals.prizes - awarded);
  for (std::size_t i = 0; i < survivors.size(); ++i)
    result.players[survivor_indices[i]].survivor_equity = after[i];

  long double terminal_sum = 0;
  long double utility_sum = 0;
  for (auto& player : result.players) {
    player.terminal_equity = static_cast<double>(player.awarded_prize) + player.survivor_equity;
    player.utility = player.terminal_equity - player.prehand_equity;
    if (!std::isfinite(player.terminal_equity) || !std::isfinite(player.utility))
      throw std::logic_error("ICM produced invalid prize utility");
    terminal_sum += player.terminal_equity;
    utility_sum += player.utility;
  }
  check_sum(terminal_sum, static_cast<long double>(totals.prizes), totals.prizes);
  check_sum(utility_sum, 0, totals.prizes);
  return result;
}

}  // namespace bs::poker
