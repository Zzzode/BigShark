#include "range_builder.h"

#include <algorithm>
#include <array>
#include <bs/eval.hpp>
#include <cstddef>
#include <utility>
#include <vector>

namespace bs::gto {
namespace {
int rankOf(char c) {
  return (int)std::string(RANKS).find(c);
}
bool onBoard(int card, const std::vector<int>& board) {
  for (int b : board)
    if (b == card)
      return true;
  return false;
}
}  // namespace

void Expand169(const std::string& k, const std::vector<int>& board, float w, Range& out) {
  const int hi = rankOf(k[0]), lo = rankOf(k[1]);
  auto add = [&](int r1, int s1, int r2, int s2) {
    int c1 = r1 * 4 + s1, c2 = r2 * 4 + s2;
    if (onBoard(c1, board) || onBoard(c2, board))
      return;
    int idx = comboIndex(c1, c2);
    out[idx] = std::max(out[idx], w);
  };
  if (hi == lo) {  // pocket pair: 6 combos
    for (int s1 = 0; s1 < 4; ++s1)
      for (int s2 = s1 + 1; s2 < 4; ++s2)
        add(hi, s1, lo, s2);
  } else if (k.size() > 2 && k[2] == 's') {  // suited: 4 combos
    for (int s = 0; s < 4; ++s)
      add(hi, s, lo, s);
  } else {  // offsuit: 12 combos
    for (int s1 = 0; s1 < 4; ++s1)
      for (int s2 = 0; s2 < 4; ++s2)
        if (s1 != s2)
          add(hi, s1, lo, s2);
  }
}

Range CapRange(const std::vector<int>& board, const Range& in, int cap) {
  std::vector<std::pair<unsigned, int>> cand;  // (board score, combo)
  for (int c = 0; c < N_COMBOS; ++c) {
    if (in[c] <= 0.0f)
      continue;
    auto [a, b] = comboTable()[c];
    if (onBoard(a, board) || onBoard(b, board))
      continue;
    int h7[7] = {a, b};
    int n = 2;
    for (int x : board)
      h7[n++] = x;
    cand.push_back({evaluate(h7, n).score, c});
  }
  Range out = zeroRange();
  if (cand.empty())
    return out;
  std::sort(cand.begin(), cand.end());
  int take = std::min(cap, (int)cand.size());
  for (int k = 0; k < take; ++k) {
    int idx = (int)((long long)k * (cand.size() - 1) / (take > 1 ? take - 1 : 1));
    out[cand[idx].second] = 1.0f;
  }
  return out;
}

Range BuildRiverRange(const std::vector<int>& board, const std::vector<std::string>* preflop_set,
                      int cap) {
  Range raw = zeroRange();
  if (preflop_set == nullptr || preflop_set->empty()) {
    for (int c = 0; c < N_COMBOS; ++c)
      raw[c] = 1.0f;
  } else {
    for (const std::string& k : *preflop_set)
      Expand169(k, board, 1.0f, raw);
  }
  return cap <= 0 ? raw : CapRange(board, raw, cap);
}

}  // namespace bs::gto
