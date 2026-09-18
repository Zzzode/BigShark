#include <algorithm>
#include <bs/multiway_sampler.hpp>
#include <cmath>
#include <cstddef>
#include <limits>
#include <stdexcept>
#include <vector>

namespace bs::solver {
namespace {

void require(bool condition, const char* message) {
  if (!condition)
    throw std::invalid_argument(message);
}

double checked_weight(const MultiwayWeightedHand& hand) {
  require(hand.cards[0] >= 0 && hand.cards[0] < 52 && hand.cards[1] >= 0 && hand.cards[1] < 52,
          "range combo card outside the deck");
  require(hand.cards[0] != hand.cards[1], "range combo repeats a card");
  require(std::isfinite(hand.weight) && hand.weight >= 0, "range weight must be finite and >= 0");
  return hand.weight;
}

bool conflicts(const std::vector<std::array<int, 2>>& chosen, const std::array<int, 2>& candidate) {
  for (const auto& hand : chosen)
    for (int card : hand)
      if (card == candidate[0] || card == candidate[1])
        return true;
  return false;
}

}  // namespace

JointDealTable enumerate_joint_deals(const std::vector<std::vector<MultiwayWeightedHand>>& ranges,
                                     const std::vector<int>& board) {
  require(!ranges.empty(), "at least one seat range is required");
  std::array<bool, 52> board_used{};
  for (int card : board) {
    require(card >= 0 && card < 52, "board card outside the deck");
    require(!board_used[card], "duplicate board card");
    board_used[card] = true;
  }

  JointDealTable table;
  const std::size_t seats = ranges.size();
  std::vector<double> weights(seats, 1.0);

  // Depth-first over seats so a conflict is detected as early as possible; the
  // enumeration order is the seat order, which keeps the result deterministic
  // for a given input.
  std::vector<std::array<int, 2>> chosen;
  chosen.reserve(seats);

  const auto recurse = [&](auto&& self, std::size_t seat) -> void {
    if (seat == seats) {
      MultiwayDeal deal;
      deal.hands = chosen;
      deal.weight = 1.0;
      for (double w : weights)
        deal.weight *= w;
      require(std::isfinite(deal.weight) && deal.weight > 0,
              "joint deal weight underflow or non-finite");
      table.unnormalized_mass += deal.weight;
      require(std::isfinite(table.unnormalized_mass), "joint mass overflow");
      table.deals.push_back(std::move(deal));
      return;
    }
    require(!ranges[seat].empty(), "seat range must not be empty");
    for (const MultiwayWeightedHand& hand : ranges[seat]) {
      const double weight = checked_weight(hand);
      if (weight == 0)
        continue;  // not in the range at all
      if (board_used[hand.cards[0]] || board_used[hand.cards[1]])
        continue;  // blocked by the board
      if (conflicts(chosen, hand.cards)) {
        ++table.rejected_conflicts;
        continue;
      }
      weights[seat] = weight;
      chosen.push_back(hand.cards);
      self(self, seat + 1);
      chosen.pop_back();
    }
  };
  recurse(recurse, 0);

  require(table.unnormalized_mass > 0 && std::isfinite(table.unnormalized_mass),
          "no compatible joint deal with positive weight");
  for (MultiwayDeal& deal : table.deals)
    deal.weight /= table.unnormalized_mass;
  return table;
}

std::size_t sample_joint_deal(const JointDealTable& table,
                              const std::vector<std::vector<MultiwayWeightedHand>>& ranges,
                              SplitMix64& rng, std::size_t max_attempts) {
  require(!table.deals.empty(), "cannot sample from an empty joint table");
  require(table.deals.front().hands.size() == ranges.size(),
          "range seat count does not match the joint table");
  require(max_attempts > 0, "max_attempts must be positive");

  // Per-seat cumulative weight tables, built here so they cannot drift from the
  // ranges the table was enumerated from.
  const std::size_t seats = ranges.size();
  std::vector<std::vector<double>> cumulative(seats);
  for (std::size_t seat = 0; seat < seats; ++seat) {
    require(!ranges[seat].empty(), "seat range must not be empty");
    auto& table_seat = cumulative[seat];
    table_seat.reserve(ranges[seat].size());
    double running = 0;
    for (const MultiwayWeightedHand& hand : ranges[seat]) {
      running += checked_weight(hand);
      table_seat.push_back(running);
    }
    require(running > 0 && std::isfinite(running), "seat range has no positive weight");
  }

  // Draw a unit double in [0, 1) from the top 53 bits, matching the pinned
  // SplitMix64 convention used elsewhere in the solver.
  const auto unit = [&rng] {
    return static_cast<double>(rng.next_u64() >> 11) * (1.0 / 9007199254740992.0);
  };
  // Cumulative weights [w0, w0+w1, ...] partition [0, total) into intervals.
  // `upper_bound` returns the first cumulative value STRICTLY ABOVE the target,
  // and that position IS the selected index: a target in [0, w0) finds index 0,
  // a target in [w0, w0+w1) finds index 1, and so on. (Subtracting one would
  // bias every draw toward index 0.)
  const auto pick = [&unit](const std::vector<double>& table_seat) {
    const double target = unit() * table_seat.back();
    const auto it = std::upper_bound(table_seat.begin(), table_seat.end(), target);
    return static_cast<std::size_t>(std::distance(table_seat.begin(), it));
  };

  for (std::size_t attempt = 0; attempt < max_attempts; ++attempt) {
    std::vector<std::array<int, 2>> proposal;
    proposal.reserve(seats);
    bool conflicted = false;
    for (std::size_t seat = 0; seat < seats && !conflicted; ++seat) {
      const std::array<int, 2> candidate = ranges[seat][pick(cumulative[seat])].cards;
      // A zero-weight or conflicting pick invalidates the whole proposal; the
      // rejection is what turns the product of marginals into the conditional
      // joint distribution RFC 0006 requires.
      if (conflicts(proposal, candidate))
        conflicted = true;
      else
        proposal.push_back(candidate);
    }
    if (conflicted)
      continue;
    // Reject proposals whose joint weight is zero (a seat picked a combo the
    // declared weights exclude): the draw must be proportional to the PRODUCT.
    for (const MultiwayDeal& deal : table.deals) {
      if (deal.hands == proposal)
        return static_cast<std::size_t>(&deal - table.deals.data());
    }
  }
  throw std::runtime_error("joint deal sampling exceeded its rejection budget");
}

}  // namespace bs::solver
