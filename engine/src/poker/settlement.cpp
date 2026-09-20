#include <algorithm>
#include <bs/settlement.hpp>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numeric>
#include <stdexcept>
#include <vector>

namespace bs::poker {
namespace {

void require(bool condition, const char* message) {
  if (!condition)
    throw std::invalid_argument(message);
}

Chips add(Chips left, Chips right) {
  if (right > std::numeric_limits<Chips>::max() - left)
    throw std::overflow_error("settlement chip sum overflow");
  return left + right;
}

Chips multiply(Chips left, Chips right) {
  if (right != 0 && left > std::numeric_limits<Chips>::max() / right)
    throw std::overflow_error("settlement chip product overflow");
  return left * right;
}

Chips profile(Chips value) {
  if (value > kMaxHeadsUpChips)
    throw std::overflow_error("settlement exceeds exact numeric profile");
  return value;
}

struct Quotient {
  Chips whole = 0;
  Chips remainder = 0;
};

Quotient multiply_divide(Chips left, Chips right, Chips divisor) {
  // Long division of left * right without constructing the product. Each
  // iteration extends the processed binary prefix of right by one bit.
  // Operands are at most 2^53 - 1; 2 * remainder + left fits uint64_t.
  require(divisor != 0, "zero settlement divisor");
  Quotient result;
  for (unsigned bit = 53; bit-- > 0;) {
    result.whole = add(result.whole, result.whole);
    result.remainder = add(result.remainder, result.remainder);
    if (((right >> bit) & Chips{1}) != 0)
      result.remainder = add(result.remainder, left);
    result.whole = add(result.whole, result.remainder / divisor);
    result.remainder %= divisor;
  }
  return result;
}

}  // namespace

ContributionSettlement settle_contributions(const SettlementInput& input) {
  const std::size_t count = input.players.size();
  require(count >= 2 && count <= kMaxContributionSeats,
          "settlement requires 2..10 players");
  require(input.seat_count >= count, "invalid settlement seat count");
  require(input.button < input.seat_count, "button outside settlement seats");
  require(input.odd_chip_rule == OddChipRule::ClockwiseLeftOfButton, "unsupported odd-chip rule");
  require(input.rake.rule == RakeRule::PotPercentageFloor, "unsupported rake rule");
  require(input.rake.basis_points <= 10000, "rake basis points outside 0..10000");
  profile(input.rake.cap);

  ContributionSettlement result;
  result.net_contributions.resize(count);
  result.awards.resize(count);
  result.refunds.resize(count);
  result.final_stacks.resize(count);
  result.chip_utility.resize(count);
  std::vector<Chips> levels;
  Chips hand_start_total = 0;
  for (std::size_t p = 0; p < count; ++p) {
    const auto& player = input.players[p];
    require(player.seat < input.seat_count, "player outside settlement seats");
    for (std::size_t other = 0; other < p; ++other)
      require(input.players[other].seat != player.seat, "duplicate settlement seat");
    const auto& chips = player.chips;
    profile(chips.stack);
    profile(chips.contributed);
    profile(chips.refunded);
    profile(chips.street_committed);
    require(chips.refunded <= chips.contributed, "refund exceeds gross contribution");
    const Chips net = chips.net_contributed();
    result.net_contributions[p] = net;
    result.refunds[p] = chips.refunded;
    result.gross_contributions = profile(add(result.gross_contributions, chips.contributed));
    result.total_refunds = profile(add(result.total_refunds, chips.refunded));
    result.pot = profile(add(result.pot, net));
    hand_start_total = profile(add(hand_start_total, add(chips.stack, net)));
    if (net != 0)
      levels.push_back(net);
  }
  std::sort(levels.begin(), levels.end());
  levels.erase(std::unique(levels.begin(), levels.end()), levels.end());

  const auto clockwise = [&](std::size_t left, std::size_t right) {
    const auto a = input.players[left].seat;
    const auto b = input.players[right].seat;
    if ((a > input.button) != (b > input.button))
      return a > input.button;
    return a < b;
  };
  Chips previous = 0;
  Chips layered = 0;
  for (Chips level : levels) {
    PotLayer layer;
    layer.contribution_level = level;
    for (std::size_t p = 0; p < count; ++p) {
      if (result.net_contributions[p] >= level) {
        layer.contributors.push_back(p);
        if (!input.players[p].chips.folded)
          layer.eligible.push_back(p);
      }
    }
    require(layer.contributors.size() >= 2, "unmarked unmatched contribution");
    require(!layer.eligible.empty(), "positive pot layer has no eligible winner");
    layer.amount = profile(multiply(level - previous, layer.contributors.size()));
    layered = profile(add(layered, layer.amount));
    if (layer.eligible.size() == 1) {
      layer.winners = layer.eligible;
    } else {
      std::uint32_t best = 0;
      for (std::size_t p : layer.eligible) {
        const auto score = input.players[p].showdown_score;
        require(score.has_value(), "contested pot requires showdown scores");
        if (layer.winners.empty() || *score > best) {
          best = *score;
          layer.winners = {p};
        } else if (*score == best) {
          layer.winners.push_back(p);
        }
      }
    }
    std::sort(layer.winners.begin(), layer.winners.end(), clockwise);
    result.layers.push_back(layer);
    previous = level;
  }
  if (layered != result.pot)
    throw std::logic_error("settlement layer conservation failed");

  if (result.pot != 0 && !(input.rake.no_flop_no_drop && !input.flop_dealt)) {
    result.rake =
        std::min(input.rake.cap, multiply_divide(result.pot, input.rake.basis_points, 10000).whole);
  }
  if (result.rake != 0) {
    Chips allocated = 0;
    std::vector<Chips> remainders;
    for (auto& layer : result.layers) {
      const auto share = multiply_divide(result.rake, layer.amount, result.pot);
      layer.rake = share.whole;
      allocated = add(allocated, share.whole);
      remainders.push_back(share.remainder);
    }
    std::vector<std::size_t> order(result.layers.size());
    std::iota(order.begin(), order.end(), std::size_t{0});
    std::sort(order.begin(), order.end(), [&](std::size_t left, std::size_t right) {
      if (remainders[left] != remainders[right])
        return remainders[left] > remainders[right];
      return left < right;
    });
    const Chips remaining = result.rake - allocated;
    if (remaining > order.size())
      throw std::logic_error("settlement rake remainder exceeds layer count");
    for (std::size_t i = 0; i < remaining; ++i)
      result.layers[order[i]].rake = add(result.layers[order[i]].rake, 1);
  }

  Chips total_awards = 0;
  Chips total_layer_rake = 0;
  for (const auto& layer : result.layers) {
    if (layer.rake > layer.amount)
      throw std::logic_error("settlement rake exceeds pot layer");
    const Chips available = layer.amount - layer.rake;
    const Chips share = available / layer.winners.size();
    const Chips odd = available % layer.winners.size();
    for (std::size_t i = 0; i < layer.winners.size(); ++i) {
      const auto winner = layer.winners[i];
      const Chips award = add(share, i < odd ? 1 : 0);
      result.awards[winner] = profile(add(result.awards[winner], award));
      total_awards = profile(add(total_awards, award));
    }
    total_layer_rake = add(total_layer_rake, layer.rake);
  }
  std::int64_t utility_sum = 0;
  Chips final_total = 0;
  for (std::size_t p = 0; p < count; ++p) {
    result.final_stacks[p] = profile(add(input.players[p].chips.stack, result.awards[p]));
    // Net contributions avoid unsigned subtraction and crediting refunds twice.
    result.chip_utility[p] = static_cast<std::int64_t>(result.awards[p]) -
                             static_cast<std::int64_t>(result.net_contributions[p]);
    utility_sum += result.chip_utility[p];
    final_total = profile(add(final_total, result.final_stacks[p]));
  }
  if (total_layer_rake != result.rake ||
      add(add(total_awards, result.rake), result.total_refunds) != result.gross_contributions ||
      utility_sum != -static_cast<std::int64_t>(result.rake) ||
      add(final_total, result.rake) != hand_start_total)
    throw std::logic_error("settlement chip conservation failed");
  return result;
}

}  // namespace bs::poker
