// equity.hpp — deterministic Monte Carlo range-vs-range equity.
// Opponent hands are gated by preflop percentile and postflop plausibility
// (pair+ or draw). Returns P(hero beats all) + 0.5*P(chop).
#pragma once
#include <algorithm>
#include <array>
#include <bs/charts.hpp>
#include <bs/eval.hpp>
#include <cstdint>
#include <string>
#include <vector>

namespace bs {

struct XorShift64 {
  uint64_t s;
  explicit XorShift64(uint64_t seed) : s(seed ? seed : 0x9E3779B97F4A7C15ULL) {}
  uint64_t next() {
    s ^= s << 13;
    s ^= s >> 7;
    s ^= s << 17;
    return s;
  }
  double unit() { return (next() >> 11) * (1.0 / 9007199254740992.0); }
  int below(int n) { return (int)(unit() * n); }
};

struct EquityResult {
  double equity = 0;
  int iterations = 0;
};

struct EquityOpts {
  std::vector<double> minPct{0.0};  // one gate per opponent
  int iterations = 4000;
  bool postflopContinue = true;
  uint64_t seed = 0;
};

inline bool isPotentialDraw(int* ids, int n) {
  int suitCount[4] = {};
  uint16_t ranks = 0;
  for (int i = 0; i < n; i++) {
    suitCount[ids[i] & 3]++;
    ranks |= (1 << (ids[i] >> 2));
  }
  for (int c : suitCount)
    if (c >= 4)
      return true;
  // gutshot / OESD: exists a missing rank completing five consecutive
  for (int r = 0; r < 13; r++) {
    if (ranks & (1 << r))
      continue;
    uint16_t m = ranks | (1 << r);
    for (int hi = 12; hi >= 4; hi--) {
      bool ok = true;
      for (int k = 0; k < 5; k++)
        if (!(m & (1 << (hi - k)))) {
          ok = false;
          break;
        }
      if (ok)
        return true;
    }
    if ((m & (1 << 12)) && (m & 0b1111) == 0b1111)
      return true;
  }
  return false;
}

inline EquityResult equityVsAll(const std::array<int, 2>& holeIds, const std::vector<int>& boardIds,
                                const EquityOpts& opts) {
  XorShift64 rng(opts.seed);
  bool blocked[52] = {};
  blocked[holeIds[0]] = blocked[holeIds[1]] = true;
  for (int c : boardIds)
    blocked[c] = true;

  std::vector<int> pool;
  for (int c = 0; c < 52; c++)
    if (!blocked[c])
      pool.push_back(c);

  const int nOpp = std::max(1, (int)opts.minPct.size());
  const int need = 5 - (int)boardIds.size();

  int wins = 0, chops = 0, valid = 0;

  auto draw = [&](bool used[52]) -> int {
    for (int t = 0; t < 30; t++) {
      int c = pool[rng.below((int)pool.size())];
      if (!used[c])
        return c;
    }
    return -1;
  };

  for (int it = 0; it < opts.iterations; it++) {
    bool used[52] = {};
    used[holeIds[0]] = used[holeIds[1]] = true;
    for (int c : boardIds)
      used[c] = true;

    std::array<std::array<int, 2>, 6> opp{};
    int nDrawn = 0;
    bool feasible = true;

    for (int o = 0; o < nOpp; o++) {
      double gate = opts.minPct[o];
      int a = -1, b = -1;
      for (int tries = 0; tries < 30; tries++) {
        int ca = draw(used);
        int cb = draw(used);
        if (ca < 0 || cb < 0) {
          feasible = false;
          break;
        }
        std::string k = key169(ca, cb);
        bool keep = preflopPct(k) >= gate;
        if (keep && boardIds.size() >= 3 && opts.postflopContinue) {
          int seven[9];
          int n2 = 0;
          for (int x : boardIds)
            seven[n2++] = x;
          seven[n2++] = ca;
          seven[n2++] = cb;
          // on the river a missed draw has no equity and normally folds to a
          // bet — only count made hands as continuing opponents there
          keep =
              evaluate(seven, n2).cat >= 2 || (boardIds.size() < 5 && isPotentialDraw(seven, n2));
        }
        if (keep) {
          a = ca;
          b = cb;
          used[a] = used[b] = true;
          break;
        }
      }
      if (a < 0) {
        feasible = false;
        break;
      }
      opp[nDrawn++] = {a, b};
    }
    if (!feasible)
      continue;

    std::vector<int> runCards;
    for (int k = 0; k < need; k++) {
      int c = draw(used);
      if (c < 0) {
        feasible = false;
        break;
      }
      used[c] = true;
      runCards.push_back(c);
    }
    if (!feasible)
      continue;

    int hero[7];
    int hn = 0;
    hero[hn++] = holeIds[0];
    hero[hn++] = holeIds[1];
    for (int c : boardIds)
      hero[hn++] = c;
    for (int c : runCards)
      hero[hn++] = c;
    uint32_t hs = evaluate(hero, hn).score;

    int beat = 0, tie = 0;
    for (int o = 0; o < nDrawn; o++) {
      int os[7];
      int on = 0;
      os[on++] = opp[o][0];
      os[on++] = opp[o][1];
      for (int c : boardIds)
        os[on++] = c;
      for (int c : runCards)
        os[on++] = c;
      uint32_t sc = evaluate(os, on).score;
      if (hs > sc)
        beat++;
      else if (hs == sc)
        tie++;
    }
    valid++;
    if (beat == nDrawn)
      wins++;
    else if (beat + tie == nDrawn)
      chops++;
  }
  EquityResult r;
  r.iterations = valid;
  if (valid)
    r.equity = (wins + 0.5 * chops) / (double)valid;
  return r;
}

}  // namespace bs
