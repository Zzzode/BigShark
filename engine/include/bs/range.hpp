#pragma once
#include <algorithm>
#include <array>
#include <bs/charts.hpp>
#include <bs/eval.hpp>
#include <string>
#include <vector>

namespace bs {

inline constexpr int N_COMBOS = 1326;

struct ComboCards {
  std::array<int, 2> c;
};

// combo index for two distinct card ids a<b
inline int comboIndex(int a, int b) {
  if (a > b)
    std::swap(a, b);
  return a * 52 - a * (a + 1) / 2 + (b - 1 - a);
}

// lazily built tables
inline const std::array<std::array<int, 2>, N_COMBOS>& comboTable() {
  static std::array<std::array<int, 2>, N_COMBOS> t{};
  static bool done = false;
  if (!done) {
    int k = 0;
    for (int a = 0; a < 52; a++)
      for (int b = a + 1; b < 52; b++)
        t[k++] = {a, b};
    done = true;
  }
  return t;
}

inline std::array<int, 2> comboCards(int idx) {
  return comboTable()[idx];
}

using Range = std::array<float, N_COMBOS>;  // weight per combo, 0 = absent
using RangeC = Range;                       // explicit alias for combo range

inline Range zeroRange() {
  Range r{};
  r.fill(0.0f);
  return r;
}

// build a combo-weighted range from a 169 spec ("AA AKs QQ-99 ..."),
// removing combos blocked by board card ids.
inline RangeC rangeFromSpec(const std::string& spec, const std::vector<int>& board = {}) {
  RangeC r = zeroRange();
  bool blocked[52] = {};
  for (int c : board)
    blocked[c] = true;
  const Range169& keys = parseRange(spec);
  const auto& ct = comboTable();
  for (int i = 0; i < N_COMBOS; i++) {
    auto [a, b] = ct[i];
    if (blocked[a] || blocked[b])
      continue;
    if (keys.count(key169(a, b)))
      r[i] = 1.0f;
  }
  return r;
}

// weighted count
inline float rangeWeight(const Range& r) {
  float w = 0;
  for (float x : r)
    w += x;
  return w;
}

// strength-ordered combo list on a fixed board (only combos that don't block it)
struct ComboStrength {
  int combo;
  int c0, c1;
  uint32_t score;
};
inline std::vector<ComboStrength> boardComboStrengths(const std::vector<int>& board) {
  bool blocked[52] = {};
  for (int c : board)
    blocked[c] = true;
  std::vector<ComboStrength> out;
  out.reserve(1225);
  const auto& ct = comboTable();
  int cards[7];
  int n = 0;
  for (int c : board)
    cards[n++] = c;
  for (int i = 0; i < N_COMBOS; i++) {
    auto [a, b] = ct[i];
    if (blocked[a] || blocked[b])
      continue;
    cards[n] = a;
    cards[n + 1] = b;
    out.push_back({i, a, b, evaluate(cards, n + 2).score});
  }
  std::sort(out.begin(), out.end(), [](auto& x, auto& y) { return x.score < y.score; });
  return out;
}

}  // namespace bs
