#include "range_tracker.h"

#include <algorithm>
#include <array>
#include <bs/equity.hpp>
#include <bs/eval.hpp>
#include <cmath>
#include <sstream>
#include <utility>
#include <vector>

#include "range_equity.h"

namespace bs::gto {
namespace {

int comboCategory(const std::vector<int>& board_prefix, int combo) {
  auto [a, b] = comboTable()[combo];
  int h[7] = {a, b};
  int n = 2;
  for (int c : board_prefix)
    h[n++] = c;
  return evaluate(h, n).cat;
}

std::vector<std::vector<std::pair<char, char>>> parseLine(const std::string& line) {
  // returns, per actor in encounter order, list of (who,code); we split tokens
  std::vector<std::pair<char, char>> tok;
  std::stringstream ss(line);
  std::string item;
  while (std::getline(ss, item, ',')) {
    if (item.size() >= 2)
      tok.push_back({item[0], item[1]});
  }
  return {tok};
}

// strength-stratified WEIGHTED cap. Distribute the `cap` slots across strength
// bands in proportion to surviving continuation mass, then keep the
// highest-weight combos in each band. This preserves the equity-driven shape
// (polar betting keeps strong + bluff-tail; call-downs keep middle; check-downs
// stay broad) instead of re-flattening to an even strength slice. Hero is
// always retained with weight 1.
Range weightedCap(const std::vector<int>& board, const Range& in, int cap, int hero_combo) {
  struct C {
    unsigned score;
    int combo;
    float w;
    int band;
  };
  std::vector<C> cand;
  for (int c = 0; c < N_COMBOS; ++c) {
    if (in[c] <= 0.0f)
      continue;
    auto [a, b] = comboTable()[c];
    bool blocked = false;
    for (int x : board)
      if (x == a || x == b)
        blocked = true;
    if (blocked)
      continue;
    int h7[7] = {a, b};
    int n = 2;
    for (int x : board)
      h7[n++] = x;
    unsigned sc = evaluate(h7, n).score;
    cand.push_back({sc, c, in[c], (int)(sc % 8)});  // band overwritten below
  }
  if (cand.empty())
    return zeroRange();
  std::sort(cand.begin(), cand.end(), [](const C& x, const C& y) { return x.score < y.score; });
  constexpr int kBands = 8;
  for (size_t i = 0; i < cand.size(); ++i)
    cand[i].band = (int)((long long)i * kBands / cand.size());  // rank bands
  float mass[kBands] = {};
  for (const C& x : cand)
    mass[x.band] += x.w;
  float totalMass = 0;
  for (float m : mass)
    totalMass += m;
  int slots[kBands] = {};
  int budget = std::min(cap, (int)cand.size());
  if (totalMass <= 0)
    totalMass = 1;
  int used = 0;
  for (int g = 0; g < kBands; ++g) {
    slots[g] = (int)(budget * mass[g] / totalMass);
    used += slots[g];
  }
  // hand remainder to bands that still have eligible combos (round-robin)
  for (int g = 0; used < budget; g = (g + 1) % kBands) {
    bool any = false;
    for (const C& x : cand)
      if (x.band == g)
        any = true;
    if (any) {
      ++slots[g];
      ++used;
    }
  }
  // within each band keep highest-weight combos
  Range out = zeroRange();
  for (int g = 0; g < kBands; ++g) {
    std::vector<const C*> v;
    for (const C& x : cand)
      if (x.band == g)
        v.push_back(&x);
    std::sort(v.begin(), v.end(), [](const C* a, const C* b) { return a->w > b->w; });
    for (int k = 0; k < std::min(slots[g], (int)v.size()); ++k)
      out[v[k]->combo] = v[k]->w;
  }
  // force hero in
  if (hero_combo >= 0 && out[hero_combo] <= 0.0f) {
    for (const C& x : cand)
      if (x.combo == hero_combo) {
        // drop the lowest-weight currently kept combo
        int victim = -1;
        float vw = 1e30f;
        for (int c2 = 0; c2 < N_COMBOS; ++c2)
          if (out[c2] > 0 && c2 != hero_combo && out[c2] < vw) {
            vw = out[c2];
            victim = c2;
          }
        if (victim >= 0)
          out[victim] = 0;
        out[hero_combo] = 1.0f;
        break;
      }
  }
  for (auto& x : out)
    if (x < 1e-3f)
      x = 0;
  if (hero_combo >= 0)
    out[hero_combo] = 1.0f;
  return out;
}

// crude preflop gate by raise count and preflop role (aggressor tighter/polar)
float preflopGate(int cat, bool draw, int raises, bool aggressor) {
  (void)draw;
  if (raises >= 2)
    return cat >= 2 ? 1.0f : 0.05f;  // 4bet+ pot
  if (raises == 1)
    return aggressor ? (cat >= 2 ? 1.0f : 0.18f) : (cat >= 2 ? 0.85f : 0.22f);  // raised pot
  return cat >= 2 ? 0.9f : 0.55f;                                               // limped pot
}

// keep at most `lim` positive-weight combos, strength-stratified, to bound the
// per-street equity work before the final weighted cap.
Range preLimit(const std::vector<int>& board, const Range& in, int lim) {
  std::vector<std::pair<unsigned, int>> cand;
  for (int c = 0; c < N_COMBOS; ++c) {
    if (in[c] <= 0)
      continue;
    auto [a, b] = comboTable()[c];
    bool bl = false;
    for (int x : board)
      if (x == a || x == b)
        bl = true;
    if (bl)
      continue;
    int h7[7] = {a, b};
    int n = 2;
    for (int x : board)
      h7[n++] = x;
    cand.push_back({evaluate(h7, n).score, c});
  }
  std::sort(cand.begin(), cand.end());
  Range out = zeroRange();
  int take = std::min(lim, (int)cand.size());
  for (int k = 0; k < take; ++k) {
    int idx = (int)((long long)k * (cand.size() - 1) / (take > 1 ? take - 1 : 1));
    out[cand[idx].second] = in[cand[idx].second];
  }
  return out;
}

float stepUp(float x, float edge) {
  return 1.0f / (1.0f + std::exp(-(x - edge) * 14.0f));
}

// equity-driven continuation weight for one side's actions on a street
float equityActionWeight(const std::vector<char>& acts, float eq, bool draw) {
  float mul = 1.0f;
  for (char code : acts) {
    if (code == 'b' || code == 'r') {
      // polarized: high-equity value + capped low-equity bluffs + draw semibluffs
      float value = stepUp(eq, code == 'r' ? 0.72f : 0.66f);
      float bluff = (1.0f - stepUp(eq, 0.42f)) * 0.55f * (code == 'r' ? 0.6f : 1.0f);
      float sb = draw && eq < 0.55f ? (code == 'r' ? 0.5f : 0.4f) : 0.0f;
      mul *= std::max(0.04f, std::min(1.0f, value + bluff + sb));
    } else if (code == 'c') {
      // continue vs a ~2/3..3/4 pot bet by pot equity (need ~0.27)
      mul *= stepUp(eq, 0.26f) * 0.95f + 0.03f;
    } else {  // x
      // checking is free: air keeps (bluff-catching); only the strongest
      // de-weight because they would usually bet for value.
      float strong = stepUp(eq, 0.80f);
      mul *= std::clamp(1.0f - 0.65f * strong, 0.35f, 1.0f);
    }
  }
  return mul;
}

// narrow both sides on one street using each combo's pot equity vs the OTHER
// side's range entering the street (avoids circularity).
void equityStreet(Range& wH, Range& wO, const std::vector<int>& prefix,
                  const std::vector<std::pair<char, char>>& tok, int pool, int mc_iters,
                  uint64_t salt) {
  Range pH = preLimit(prefix, wH, pool);
  Range pO = preLimit(prefix, wO, pool);
  auto combosOf = [](const Range& w) {
    std::vector<int> v;
    for (int c = 0; c < N_COMBOS; ++c)
      if (w[c] > 0)
        v.push_back(c);
    return v;
  };
  auto cH = combosOf(pH), cO = combosOf(pO);
  if (cH.empty() || cO.empty())
    return;
  auto actsOf = [&](char who) {
    std::vector<char> a;
    for (auto [actor, code] : tok)
      if (actor == who)
        a.push_back(code);
    return a;
  };
  auto actsH = actsOf('H'), actsO = actsOf('O');

  for (int c : cH) {
    auto pc = comboTable()[c];
    float eq = (float)RangeVsRangeEquity({pc[0], pc[1]}, prefix, pO, mc_iters,
                                         salt ^ (uint64_t)c * 2654435761u);
    int d[7] = {pc[0], pc[1]};
    int dn = 2;
    for (int x : prefix)
      d[dn++] = x;
    bool isdraw = prefix.size() < 5 && isPotentialDraw(d, dn);
    float mul = equityActionWeight(actsH, eq, isdraw);
    wH[c] *= mul;
  }
  for (int c : cO) {
    auto pc = comboTable()[c];
    float eq = (float)RangeVsRangeEquity({pc[0], pc[1]}, prefix, pH, mc_iters,
                                         salt ^ (uint64_t)c * 40503u);
    int d[7] = {pc[0], pc[1]};
    int dn = 2;
    for (int x : prefix)
      d[dn++] = x;
    bool isdraw = prefix.size() < 5 && isPotentialDraw(d, dn);
    float mul = equityActionWeight(actsO, eq, isdraw);
    wO[c] *= mul;
  }
}

}  // namespace

TrackedRangesImpl TrackRiverRangesImpl(const std::vector<int>& board, int hero_combo,
                                       bool hero_is_ip, const std::string& flop_line,
                                       const std::string& turn_line, const TrackOptionsImpl& opt) {
  auto seed = [&](bool aggressor) {
    Range w = zeroRange();
    for (int c = 0; c < N_COMBOS; ++c) {
      auto [a, b] = comboTable()[c];
      bool blocked = false;
      for (int x : board)
        if (x == a || x == b)
          blocked = true;
      if (blocked)
        continue;
      int cat = comboCategory(board, c);
      int h7[7] = {a, b};
      int hn = 2;
      for (int x : board)
        h7[hn++] = x;
      bool draw = isPotentialDraw(h7, hn);
      w[c] = preflopGate(cat, draw, opt.preflop_raises, aggressor);
    }
    return w;
  };
  Range wH = seed(opt.hero_was_aggressor);
  Range wO = seed(!opt.hero_was_aggressor);

  const std::vector<int> flopBoard(board.begin(), board.begin() + 3);
  const std::vector<int> turnBoard(board.begin(), board.begin() + 4);
  auto flop = parseLine(flop_line)[0];
  auto turn = parseLine(turn_line)[0];
  // equity pool is a bit wider than the final cap so the tracker sees enough
  // combos before narrowing; MC iterations kept low for sub-second tracking.
  constexpr int kPool = 48, kMc = 300;
  equityStreet(wH, wO, flopBoard, flop, kPool, kMc, 0xA11CE11ULL);
  equityStreet(wH, wO, turnBoard, turn, kPool, kMc, 0xB0B2ULL);

  wH = weightedCap(board, wH, opt.cap, hero_combo);
  wO = weightedCap(board, wO, opt.cap, -1);

  TrackedRangesImpl out;
  if (hero_is_ip) {
    out.ip = wH;
    out.oop = wO;
  } else {
    out.ip = wO;
    out.oop = wH;
  }
  return out;
}

}  // namespace bs::gto
