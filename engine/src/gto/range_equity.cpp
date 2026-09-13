#include "range_equity.h"

#include <bs/equity.hpp>
#include <bs/eval.hpp>
#include <vector>

namespace bs::gto {

double RangeVsRangeEquity(const std::array<int, 2>& hole, const std::vector<int>& board_prefix,
                          const Range& opp, int iterations, uint64_t seed) {
  bs::XorShift64 rng(seed ? seed : 0x9E3779B97F4A7C15ULL);

  // build the weighted opponent combo pool (excluding board/hole conflicts)
  bool blocked[52] = {};
  blocked[hole[0]] = blocked[hole[1]] = true;
  for (int c : board_prefix)
    blocked[c] = true;
  std::vector<int> oc;
  std::vector<double> cw;
  double tot = 0;
  for (int c = 0; c < N_COMBOS; ++c) {
    if (opp[c] <= 0.0f)
      continue;
    auto [a, b] = comboTable()[c];
    if (blocked[a] || blocked[b])
      continue;
    oc.push_back(c);
    cw.push_back(opp[c]);
    tot += opp[c];
  }
  if (oc.empty() || tot <= 0)
    return 0.0;
  // cumulative weights
  std::vector<double> cum(oc.size());
  double acc = 0;
  for (size_t i = 0; i < oc.size(); ++i) {
    acc += cw[i];
    cum[i] = acc;
  }

  std::vector<int> deck;
  for (int c = 0; c < 52; ++c)
    if (!blocked[c])
      deck.push_back(c);

  const int need = 5 - (int)board_prefix.size();
  int wins = 0, chops = 0, valid = 0;
  for (int it = 0; it < iterations; ++it) {
    // draw an opponent combo by weight
    const double pick = rng.unit() * tot;
    size_t k = 0;
    while (k + 1 < cum.size() && cum[k] < pick)
      ++k;
    auto [oa, ob] = comboTable()[oc[k]];
    if (blocked[oa] || blocked[ob])
      continue;  // impossible vs our hole/board

    bool used[52] = {};
    used[hole[0]] = used[hole[1]] = used[oa] = used[ob] = true;
    for (int c : board_prefix)
      used[c] = true;

    bool feasible = true;
    int board[5];
    int bn = 0;
    for (int c : board_prefix)
      board[bn++] = c;
    for (int r = 0; r < need; ++r) {
      int card = -1;
      for (int tries = 0; tries < 24; ++tries) {
        int x = deck[rng.below((int)deck.size())];
        if (!used[x]) {
          card = x;
          break;
        }
      }
      if (card < 0) {
        feasible = false;
        break;
      }
      used[card] = true;
      board[bn++] = card;
    }
    if (!feasible)
      continue;

    int hh[7] = {hole[0], hole[1], board[0], board[1], board[2], board[3], board[4]};
    int oh[7] = {oa, ob, board[0], board[1], board[2], board[3], board[4]};
    unsigned hs = evaluate(hh, 7).score, os = evaluate(oh, 7).score;
    ++valid;
    if (hs > os)
      ++wins;
    else if (hs == os)
      ++chops;
  }
  return valid ? (wins + 0.5 * chops) / (double)valid : 0.0;
}

}  // namespace bs::gto
