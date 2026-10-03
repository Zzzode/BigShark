// RFC 0007: exact equity frontier evaluator for heads-up flop-terminal games.
//
// Computes each seat's exact equity by enumerating all C(45,2) = 990
// turn/river combos, then returns equity * pot - contributed as the expected
// net chip delta. This is the "all-in at flop" approximation: it assumes no
// postflop betting, so the frontier value is the showdown equity weighted by
// the pot. Equity results are cached per (flop, hands) pair; the pot is
// recomputed from the ledger each call because different preflop betting
// lines produce different pot sizes for the same board and deal.
#pragma once

#include <array>
#include <bs/frontier.hpp>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace bs::gto {

class EquityFrontierEvaluator : public FrontierEvaluator {
 public:
  std::vector<double> evaluate(std::span<const int> flop, std::span<const std::array<int, 2>> hands,
                               const bs::tree::TerminalPayload& ledger) const override;

 private:
  static std::uint64_t make_key(std::span<const int> flop,
                                std::span<const std::array<int, 2>> hands);
  mutable std::unordered_map<std::uint64_t, std::array<double, 2>> equity_cache_;
};

}  // namespace bs::gto
