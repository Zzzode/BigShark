#include "board_classes.hpp"

#include <algorithm>
#include <stdexcept>
#include <unordered_set>

namespace bs::rfc0047 {

namespace {

// The 24 permutations of suits 0..3, in lexicographic order (identity first),
// matching the production tie-break (smallest minimizer relabel wins).
const std::array<std::array<int, 4>, 24>& suit_permutations() {
  static const auto perms = [] {
    std::array<std::array<int, 4>, 24> p{};
    std::array<int, 4> cur{0, 1, 2, 3};
    int k = 0;
    p[k++] = cur;
    while (std::next_permutation(cur.begin(), cur.end()))
      p[k++] = cur;
    return p;
  }();
  return perms;
}

}  // namespace

std::uint32_t pack_sorted(std::span<const int> sorted_cards) {
  std::uint32_t key = 0;
  for (std::size_t i = 0; i < sorted_cards.size(); ++i) {
    if (sorted_cards[i] < 0 || sorted_cards[i] > 51)
      throw std::invalid_argument("rfc0047: card id out of range");
    if (i > 0 && sorted_cards[i] <= sorted_cards[i - 1])
      throw std::invalid_argument("rfc0047: pack_sorted requires ascending distinct ids");
    key |= static_cast<std::uint32_t>(sorted_cards[i]) << (6u * i);
  }
  return key;
}

void unpack_key(std::uint32_t key, int n, std::array<int, 5>& out) {
  for (int i = 0; i < n; ++i)
    out[i] = static_cast<int>((key >> (6u * i)) & 0x3Fu);
}

std::vector<int> canonical_rep(std::span<const int> board) {
  const int n = static_cast<int>(board.size());
  if (n < 1 || n > 5)
    throw std::invalid_argument("rfc0047: canonical board size must be 1..5");
  std::array<bool, 52> seen{};
  for (int c : board) {
    if (c < 0 || c > 51)
      throw std::invalid_argument("rfc0047: card id out of range");
    if (seen[c])
      throw std::invalid_argument("rfc0047: duplicate board card");
    seen[c] = true;
  }
  const auto& perms = suit_permutations();
  std::vector<int> best(n, 52);
  for (const auto& perm : perms) {
    std::vector<int> mapped(n);
    for (int i = 0; i < n; ++i)
      mapped[i] = (board[i] / 4) * 4 + perm[board[i] % 4];
    std::sort(mapped.begin(), mapped.end());
    if (mapped < best)
      best = mapped;
  }
  return best;
}

std::vector<std::uint32_t> enumerate_canonical_class_keys(int n) {
  if (n < 1 || n > 5)
    throw std::invalid_argument("rfc0047: enumerate board size must be 1..5");
  std::unordered_set<std::uint32_t> classes;
  classes.reserve(n == 5 ? 140'000 : n == 4 ? 18'000 : 2'000);

  std::vector<int> combo(n, -1);
  // Lexicographic combinations via the standard "next combination" over
  // increasing indices in 0..51.
  for (int i = 0; i < n; ++i)
    combo[i] = i;
  while (true) {
    std::vector<int> board(combo.begin(), combo.end());
    const std::vector<int> rep = canonical_rep(board);
    classes.insert(pack_sorted(rep));
    // Next combination.
    int i = n - 1;
    while (i >= 0 && combo[i] == 52 - n + i)
      --i;
    if (i < 0)
      break;
    ++combo[i];
    for (int j = i + 1; j < n; ++j)
      combo[j] = combo[j - 1] + 1;
  }

  std::vector<std::uint32_t> keys(classes.begin(), classes.end());
  std::sort(keys.begin(), keys.end());
  return keys;
}

}  // namespace bs::rfc0047
