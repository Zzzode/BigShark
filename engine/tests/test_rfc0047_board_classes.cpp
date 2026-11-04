// RFC 0047 Stage-1 native test: canonical suit-isomorphism board classes.
// Pins flop/turn/river class counts and cross-checks the generic enumerator
// against the production 1,755 flop classes.
#include <array>
#include <bs/abstraction.hpp>
#include <cstdio>
#include <set>
#include <span>
#include <vector>

#include "../benchmarks/rfc0047/board_classes.hpp"

#define CHECK(cond)                                                         \
  do {                                                                      \
    if (!(cond)) {                                                          \
      std::printf("CHECK failed: %s (%s:%d)\n", #cond, __FILE__, __LINE__); \
      return false;                                                         \
    }                                                                       \
  } while (0)

using namespace bs::rfc0047;

bool test_pinned_counts() {
  const auto flop = enumerate_canonical_class_keys(3);
  const auto turn = enumerate_canonical_class_keys(4);
  CHECK(flop.size() == kCanonicalFlopClasses);
  CHECK(turn.size() == kCanonicalTurnClasses);
  CHECK(flop.size() == 1755);
  CHECK(turn.size() == 16432);
  // River enumeration is exercised in the census binary; pin it here too so
  // the guarded constant cannot drift unnoticed (a few seconds).
  const auto river = enumerate_canonical_class_keys(5);
  CHECK(river.size() == kCanonicalRiverClasses);
  CHECK(river.size() == 134459);
  // Keys are sorted and unique.
  for (std::size_t i = 1; i < river.size(); ++i)
    CHECK(river[i - 1] < river[i]);
  return true;
}

bool test_concrete_identities() {
  const auto choose = [](unsigned long long n, unsigned k) {
    unsigned long long num = 1, den = 1;
    for (unsigned i = 0; i < k; ++i) {
      num *= n - i;
      den *= i + 1;
    }
    return num / den;
  };
  CHECK(choose(52, 3) == 22100);
  CHECK(choose(52, 4) == 270725);
  CHECK(choose(52, 5) == 2598960);
  CHECK(choose(52, 4) * 48 == 5 * choose(52, 5));
  CHECK(choose(52, 4) * 48 == 12994800ull);
  return true;
}

bool test_matches_production_flop() {
  const std::vector<std::array<int, 3>> prod = bs::abstraction::enumerate_canonical_flop_classes();
  std::set<std::uint32_t> prod_keys;
  for (const auto& b : prod)
    prod_keys.insert(pack_sorted(std::span<const int>(b.data(), b.size())));
  const auto ours = enumerate_canonical_class_keys(3);
  CHECK(prod_keys.size() == ours.size());
  for (std::uint32_t k : ours)
    CHECK(prod_keys.count(k) == 1);
  return true;
}

bool test_canonical_invariants() {
  // Idempotence and invariance under a concrete suit permutation.
  const std::vector<int> board{0, 5, 10, 13, 17};  // arbitrary 5 distinct
  const std::vector<int> rep1 = canonical_rep(board);
  const std::vector<int> rep2 = canonical_rep(rep1);
  CHECK(rep1 == rep2);
  // Apply suit permutation swap(0,1) to every card, then canonicalize: the
  // class representative must be unchanged.
  std::vector<int> swapped(board.size());
  for (std::size_t i = 0; i < board.size(); ++i) {
    const int rank = board[i] / 4;
    int suit = board[i] % 4;
    if (suit == 0)
      suit = 1;
    else if (suit == 1)
      suit = 0;
    swapped[i] = rank * 4 + suit;
  }
  CHECK(canonical_rep(swapped) == rep1);
  return true;
}

int main() {
  if (!test_pinned_counts() || !test_concrete_identities() || !test_matches_production_flop() ||
      !test_canonical_invariants())
    return 1;
  std::printf("rfc0047_board_classes: all checks passed\n");
  return 0;
}
