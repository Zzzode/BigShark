// RFC 0046 Stage-1 measurement-only exact per-combo card keying.
//
// ConcreteCardKeyer maps a two-card holding over a FIXED four-card turn board
// to a dense id in 0..C(52-4,2)-1 = 0..1127. Card ids are rank*4+suit
// (rank-major); live cards are the 48 cards not on the board, enumerated in
// ascending id order. Unordered live pairs (c_i, c_j), i < j, are dense-ranked
// by (i, j):
//
//   dense(i, j) = offset(i) + (j - i - 1),   offset(i) = i*n - i*(i+1)/2
//
// On a fixed board this is a bijection between live holdings and 0..nChoose2
// (injective exact keying). The key depends on board4 ONLY: a holding keeps
// the same id at every turn/river node, so cross-node average strategy stays
// consistent and the river card acts purely through zero-reach pruning.
//
// Benchmark-only: lives in bigshark_rfc0046_bench, never installed, never
// linked into engine-host/practice/resident/pack. Deleted after the gates
// under no-legacy unless a production addendum promotes a named subset.
#pragma once

#include <array>
#include <bs/nseat_trainer.hpp>
#include <cstdint>
#include <span>
#include <vector>

namespace bs::rfc0046 {

inline constexpr std::size_t kTurnBoardCards = 4;
inline constexpr std::size_t kLiveCardsOnTurn = 48;
inline constexpr std::uint32_t kConcreteBucketCap =
    static_cast<std::uint32_t>(kLiveCardsOnTurn * (kLiveCardsOnTurn - 1) / 2);  // 1128

class ConcreteCardKeyer final : public bs::solver::NSeatCardKeyerSeam {
 public:
  // board4 must be four distinct cards in 0..51.
  explicit ConcreteCardKeyer(std::span<const int> board4);

  std::uint32_t bucket(const std::array<int, 2>& hole, std::span<const int> board) const override;
  std::uint32_t bucket_cap() const override { return kConcreteBucketCap; }

  // Inverse of the dense map: holding (two ascending live card ids) for a
  // dense id. Throws for an out-of-range id.
  std::array<int, 2> holding(std::uint32_t dense) const;

  // Dense id for an ascending live pair without the board-span consistency
  // check (used by certifier/walkers that already validated the board).
  std::uint32_t dense_for_live_pair(const std::array<int, 2>& hole) const;

  const std::array<int, kTurnBoardCards>& board4() const { return board4_; }
  const std::vector<int>& live_cards() const { return live_; }

 private:
  std::array<int, kTurnBoardCards> board4_{};
  std::array<bool, 52> blocked_{};
  std::vector<int> live_;                     // ascending non-board cards
  std::array<std::int32_t, 52> live_rank_{};  // rank in live_ or -1
};

// All unblocked two-card holdings on `board` (ascending card ids, dense id
// order), each with unit weight. Used to build full uniform prior ranges for
// the two-seat cells. `board` may be the four- or five-card public board; any
// board card blocks.
std::vector<std::array<int, 2>> live_unblocked_holdings(std::span<const int> board);

}  // namespace bs::rfc0046
