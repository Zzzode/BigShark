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

MultiwayDeal sample_scalable_joint_deal(
    const std::vector<std::vector<MultiwayWeightedHand>>& ranges, const std::vector<int>& board,
    SplitMix64& rng, std::size_t max_attempts, std::size_t* attempts_out) {
  require(!ranges.empty(), "at least one seat range is required");
  require(max_attempts > 0, "max_attempts must be positive");
  std::array<bool, 52> board_used{};
  for (int card : board) {
    require(card >= 0 && card < 52, "board card outside the deck");
    require(!board_used[card], "duplicate board card");
    board_used[card] = true;
  }
  const std::size_t seats = ranges.size();

  // Per-seat board-filtered marginal tables. A combo blocked by the board
  // never appears in the enumerated support, so it is excluded here; the
  // enumeration's validation rules are applied to every retained combo.
  struct Marginal {
    std::vector<std::array<int, 2>> combos;
    std::vector<double> weights;
    std::vector<double> cumulative;
  };
  std::vector<Marginal> marginals(seats);
  for (std::size_t seat = 0; seat < seats; ++seat) {
    require(!ranges[seat].empty(), "seat range must not be empty");
    Marginal& m = marginals[seat];
    m.combos.reserve(ranges[seat].size());
    m.cumulative.reserve(ranges[seat].size());
    double running = 0.0;
    for (const MultiwayWeightedHand& hand : ranges[seat]) {
      const double weight = checked_weight(hand);
      if (weight == 0)
        continue;
      if (board_used[hand.cards[0]] || board_used[hand.cards[1]])
        continue;
      m.combos.push_back(hand.cards);
      running += weight;
      require(std::isfinite(running) && running > 0.0, "seat marginal has no positive weight");
      m.weights.push_back(weight);
      m.cumulative.push_back(running);
    }
    require(!m.combos.empty(), "seat range has no combo compatible with the board");
  }

  // One unbiased unit double per draw (top 53 bits, matching the pinned
  // sampler convention) and an ascending cumulative weighted pick.
  const auto unit = [&rng] {
    return static_cast<double>(rng.next_u64() >> 11) * (1.0 / 9007199254740992.0);
  };
  const auto pick = [&unit](const Marginal& m) {
    const double target = unit() * m.cumulative.back();
    auto it = std::upper_bound(m.cumulative.begin(), m.cumulative.end(), target);
    return static_cast<std::size_t>(std::distance(m.cumulative.begin(), it));
  };

  for (std::size_t attempt = 0; attempt < max_attempts; ++attempt) {
    std::vector<std::array<int, 2>> proposal;
    std::vector<double> proposal_weights;
    proposal.reserve(seats);
    proposal_weights.reserve(seats);
    bool conflicted = false;
    for (std::size_t seat = 0; seat < seats && !conflicted; ++seat) {
      const std::size_t choice = pick(marginals[seat]);
      const std::array<int, 2> candidate = marginals[seat].combos[choice];
      for (const auto& chosen : proposal)
        for (int card : chosen)
          if (card == candidate[0] || card == candidate[1]) {
            conflicted = true;
            break;
          }
      if (conflicted)
        break;
      proposal.push_back(candidate);
      proposal_weights.push_back(marginals[seat].weights[choice]);
    }
    if (conflicted)
      continue;
    if (attempts_out)
      *attempts_out = attempt + 1;
    MultiwayDeal deal;
    deal.hands = std::move(proposal);
    deal.weight = 1.0;
    for (double w : proposal_weights)
      deal.weight *= w;  // unnormalized joint weight, on the enumerated scale
    return deal;
  }
  throw std::runtime_error("scalable joint dealing exceeded its rejection budget");
}

}  // namespace bs::solver
