// RFC 0007 (scope extended by RFC 0010, RFC 0024): exact equity frontier
// evaluator for flop-terminal games and depth-limited resolving, 2..10 seats.
//
// Computes each live seat's exact equity by enumerating all remaining
// turn/river combos, then returns equity * pot - contributed as the expected
// net chip delta. This is the "all-in" approximation: it assumes no future
// betting, so the frontier value is the showdown equity weighted by the pot.
// Only live seats (ledger.seats[s].folded == false) compete; folded seats
// receive equity 0 and their chips stay in the pot.
//
// RFC 0024 board-size behavior:
//  - 3 cards (flop): enumerate all C(49-2N, 2) turn/river combos.
//  - 4 cards (turn): enumerate all C(48-2N, 1) river combos.
//  - 5 cards (river): direct showdown settlement (no cards to enumerate).
//
// Equity results are cached per (board, hands, live-set); the pot is
// recomputed from the ledger each call because different betting lines
// produce different pot sizes for the same board and deal.
#pragma once

#include <array>
#include <bs/frontier.hpp>
#include <cstddef>
#include <mutex>
#include <span>
#include <unordered_map>
#include <vector>

namespace bs::gto {

class EquityFrontierEvaluator : public FrontierEvaluator {
 public:
  // Default cache capacity: 1,000,000 entries. The cache stops inserting new
  // entries once the cap is reached; existing entries remain valid. This
  // bounds memory for N-way preflop training where the joint deal space is
  // 1,326^N and revisits are rare.
  static constexpr std::size_t kDefaultCacheCap = 1'000'000;

  EquityFrontierEvaluator() = default;
  explicit EquityFrontierEvaluator(std::size_t cache_cap) : cache_cap_(cache_cap) {}

  std::vector<double> evaluate(std::span<const int> board,
                               std::span<const std::array<int, 2>> hands,
                               const bs::tree::TerminalPayload& ledger) const override;

 private:
  // The cache key: sorted board (3/4/5 cards), per-seat sorted hole cards
  // (2 each), then the live-seat set (one int per seat: 0 = folded, 1 = live).
  // The live-set is required because the cached equity depends on which seats
  // compete: the same (board, hands) pair can reach evaluate() with different
  // fold patterns.
  static std::vector<int> make_key(std::span<const int> board,
                                   std::span<const std::array<int, 2>> hands,
                                   const bs::tree::TerminalPayload& ledger);

  // FNV-1a hash over the key vector's bytes, for use as the unordered_map
  // hasher. Equality is exact (std::vector<int>::operator==), so a hash
  // collision degrades to a bucket collision, never a wrong value.
  struct KeyHash {
    std::size_t operator()(const std::vector<int>& key) const noexcept;
  };

  std::size_t cache_cap_ = kDefaultCacheCap;
  mutable std::mutex mutex_;
  mutable std::unordered_map<std::vector<int>, std::vector<double>, KeyHash> equity_cache_;
};

}  // namespace bs::gto
