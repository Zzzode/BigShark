// RFC 0047 Stage-1 suit-isomorphism canonical board-class enumeration.
//
// Generic n-card (n=3,4,5) version of the production flop canonicalization
// (engine abstraction `canonicalize`, RFC 0009 D5.1): a concrete board folds
// to its isomorphism class by taking the component-wise minimum, over the 24
// suit permutations, of its rank-major card-id tuple after sorting; cards are
// rank-major (id = rank*4 + suit), so sorting ids is the (rank,suit) order.
// This guarded TU does NOT modify the shared abstraction; a native test
// cross-checks that n=3 reproduces the production 1,755 classes exactly.
//
// Canonical universe sizes (pinned by native tests):
//   flop  C(52,3)=22,100   ->  1,755 classes
//   turn  C(52,4)=270,725  -> 16,432 classes
//   river C(52,5)=2,598,960->134,459 classes
// Identity 270,725 x 48 = 12,994,800 = 5 x 2,598,960.
#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace bs::rfc0047 {

inline constexpr std::uint32_t kCanonicalFlopClasses = 1'755;
inline constexpr std::uint32_t kCanonicalTurnClasses = 16'432;
inline constexpr std::uint32_t kCanonicalRiverClasses = 134'459;

// Pack a sorted (ascending) n-card tuple into a 6n-bit key (n<=5 -> <=30 bits).
std::uint32_t pack_sorted(std::span<const int> sorted_cards);
void unpack_key(std::uint32_t key, int n, std::array<int, 5>& out);

// Canonical representative (sorted card ids) of an n-card distinct board.
std::vector<int> canonical_rep(std::span<const int> board);

// All distinct canonical class keys for n-card boards, sorted ascending.
std::vector<std::uint32_t> enumerate_canonical_class_keys(int n);

inline std::uint32_t count_canonical_classes(int n) {
  return static_cast<std::uint32_t>(enumerate_canonical_class_keys(n).size());
}

}  // namespace bs::rfc0047
