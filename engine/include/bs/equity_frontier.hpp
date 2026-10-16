// RFC 0007 (scope extended by RFC 0010, RFC 0024): side-pot-safe equity
// frontier evaluator for flop-terminal games and depth-limited resolving,
// 2..10 seats.
//
// Computes each live seat's expected showdown award by enumerating all
// remaining turn/river combos and settling per-runout pot layers (side pots)
// using the same layer logic as settle_contributions, then returns
// expected_award - net_contributed as the expected net chip delta. This
// handles unequal stacks and side pots correctly: each seat competes only
// for the pot layers its net contribution is eligible for.
//
// Only live seats (ledger.seats[s].folded == false) compete; folded seats
// receive expected award 0 and their net contribution stays in the pot as
// dead money.
//
// RFC 0024 board-size behavior:
//  - 3 cards (flop): enumerate all C(49-2N, 2) turn/river combos.
//  - 4 cards (turn): enumerate all C(48-2N, 1) river combos.
//  - 5 cards (river): direct showdown settlement (no cards to enumerate).
//
// Expected awards are cached per (board, hands, live-set, net-contributions).
// The contribution suffix is required because the layer structure changes
// with the contribution pattern even for the same board and deal.
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
  // (2 each), the live-seat set (one int per seat: 0 = folded, 1 = live),
  // and per-seat net contributions (two ints each: low/high 32 bits). The
  // live-set and contribution suffix are required because the cached award
  // depends on which seats compete and how the pot is layered.
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
  mutable std::unordered_map<std::vector<int>, std::vector<double>, KeyHash> award_cache_;
};

}  // namespace bs::gto
