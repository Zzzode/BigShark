#include <bs/equity_frontier.hpp>
#include <bs/eval.hpp>
#include <stdexcept>

namespace bs::gto {

std::uint64_t EquityFrontierEvaluator::make_key(std::span<const int> flop,
                                                std::span<const std::array<int, 2>> hands) {
  std::uint64_t key = 0;
  for (int c : flop)
    key = (key << 6) | static_cast<std::uint64_t>(c);
  for (const auto& hand : hands)
    for (int c : hand)
      key = (key << 6) | static_cast<std::uint64_t>(c);
  return key;
}

std::vector<double> EquityFrontierEvaluator::evaluate(
    std::span<const int> flop, std::span<const std::array<int, 2>> hands,
    const bs::tree::TerminalPayload& ledger) const {
  if (hands.size() != 2)
    throw std::invalid_argument("equity frontier evaluator is heads-up only");
  if (flop.size() != 3)
    throw std::invalid_argument("equity frontier evaluator requires a 3-card flop");

  const std::uint64_t key = make_key(flop, hands);
  std::array<double, 2> equity;
  if (auto it = equity_cache_.find(key); it != equity_cache_.end()) {
    equity = it->second;
  } else {
    // Build the remaining card pool (52 - 3 flop - 4 hole = 45 cards).
    bool blocked[52] = {};
    for (int c : flop)
      blocked[c] = true;
    for (const auto& hand : hands) {
      blocked[hand[0]] = true;
      blocked[hand[1]] = true;
    }
    int pool[45];
    int pool_size = 0;
    for (int c = 0; c < 52; ++c)
      if (!blocked[c])
        pool[pool_size++] = c;

    // Enumerate all C(45,2) = 990 turn/river combos.
    int wins0 = 0, wins1 = 0, ties = 0;
    for (int i = 0; i < pool_size; ++i) {
      for (int j = i + 1; j < pool_size; ++j) {
        const int hero[7] = {hands[0][0], hands[0][1], flop[0], flop[1], flop[2], pool[i], pool[j]};
        const int opp[7] = {hands[1][0], hands[1][1], flop[0], flop[1], flop[2], pool[i], pool[j]};
        const std::uint32_t hs = bs::evaluate(hero, 7).score;
        const std::uint32_t os = bs::evaluate(opp, 7).score;
        if (hs > os)
          ++wins0;
        else if (os > hs)
          ++wins1;
        else
          ++ties;
      }
    }
    const int total = wins0 + wins1 + ties;
    equity[0] = (wins0 + 0.5 * ties) / total;
    equity[1] = (wins1 + 0.5 * ties) / total;
    equity_cache_[key] = equity;
  }

  // The pot is the sum of all seats' contributed amounts.
  double pot = 0;
  for (std::size_t s = 0; s < ledger.player_count; ++s)
    pot += static_cast<double>(ledger.seats[s].contributed);

  // Net chip delta: expected pot share minus own contribution.
  std::vector<double> values(2);
  values[0] = equity[0] * pot - static_cast<double>(ledger.seats[0].contributed);
  values[1] = equity[1] * pot - static_cast<double>(ledger.seats[1].contributed);
  return values;
}

}  // namespace bs::gto
