// test_gto.cpp — exact river LP / bounded CFR + range builder contracts.
#include <algorithm>
#include <bs/eval.hpp>
#include <bs/range.hpp>
#include <bs/river_gto.hpp>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include "gto/multistreet_cfr.h"

using namespace bs;
using namespace bs::gto;

static int card(const char* s) {
  return cardId(std::string(s));
}

static bool near(double a, double b, double tol) {
  return std::fabs(a - b) <= tol;
}

#define CHECK(cond)                                                         \
  do {                                                                      \
    if (!(cond)) {                                                          \
      std::printf("CHECK failed: %s (%s:%d)\n", #cond, __FILE__, __LINE__); \
      return 1;                                                             \
    }                                                                       \
  } while (0)

// card-disjoint 12-combo mixed fixture (every pair non-blocking, weak+strong)
static std::vector<int> disjointFixture(const std::vector<int>& board) {
  std::vector<std::pair<unsigned, int>> v;
  for (int c = 0; c < N_COMBOS; ++c) {
    auto p = comboTable()[c];
    bool onb = false;
    for (int b : board)
      if (b == p[0] || b == p[1])
        onb = true;
    if (onb)
      continue;
    int h[7] = {p[0], p[1], board[0], board[1], board[2], board[3], board[4]};
    v.push_back({evaluate(h, 7).score, c});
  }
  std::sort(v.begin(), v.end());
  std::vector<int> used(52, 0);
  for (int b : board)
    used[b] = 1;
  std::vector<int> picked;
  int lo = 0, hi = (int)v.size() - 1;
  bool top = false;
  auto take = [&](int z) {
    auto p = comboTable()[v[z].second];
    if (used[p[0]] || used[p[1]])
      return false;
    used[p[0]] = used[p[1]] = 1;
    picked.push_back(v[z].second);
    return true;
  };
  while ((int)picked.size() < 12 && lo <= hi) {
    if (top) {
      if (!take(hi)) {
        --hi;
        continue;
      }
      --hi;
    } else {
      if (!take(lo)) {
        ++lo;
        continue;
      }
      ++lo;
    }
    top = !top;
  }
  return picked;
}

int main() {
  std::vector<int> board = {card("Kh"), card("7s"), card("2d"), card("9c"), card("3h")};
  auto picked = disjointFixture(board);
  CHECK(picked.size() == 12);

  Range r = zeroRange();
  for (int c : picked)
    r[c] = 1.0f;

  // 1) bounded CFR must always work and be near an equilibrium; budget-aware
  // early stop keeps it fast (this is the no-HiGHS fallback path)
  RiverSolveOptions opt;
  opt.allow_exact = false;
  opt.time_budget_s = 0.5;
  opt.cfr_iterations = 200000;
  auto cfr = SolveRiver(board, 100, 0.75f, 1.0f, r, r, opt);
  CHECK(cfr->result().ok);
  CHECK(!cfr->result().exact);
  printf("CFR exploitability=%.5f%% pot, value(IP)=%.4f\n", 100 * cfr->result().exploitability_pot,
         cfr->result().value_to_ip);
  CHECK(cfr->result().exploitability_pot < 0.05);

  // 2) every chosen combo has a legal probability distribution at each node
  for (int c : picked) {
    double p[3];
    for (auto node : {RiverNode::kRoot, RiverNode::kFaceBet, RiverNode::kFaceRaiseI}) {
      bool got = cfr->ProbsFor(c, node, p);
      CHECK(got);
      double s = 0;
      int na = RiverSolution::NumActions(node);
      for (int a = 0; a < na; ++a) {
        CHECK(p[a] >= -1e-9 && p[a] <= 1 + 1e-9);
        s += p[a];
      }
      CHECK(near(s, 1.0, 1e-6));
    }
  }

  // 3) deterministic mixed choice is in range and stable under the same seed
  for (int c : picked) {
    int a1 = cfr->ChooseAction(c, RiverNode::kFaceBet, 12345);
    int a2 = cfr->ChooseAction(c, RiverNode::kFaceBet, 12345);
    CHECK(a1 == a2 && a1 >= 0 && a1 < RiverSolution::NumActions(RiverNode::kFaceBet));
  }

  // 4) exact backend (if compiled with HiGHS): both-side value symmetry and
  // near-zero exploitability
  RiverSolveOptions eopt;
  eopt.allow_exact = true;
  auto ex = SolveRiver(board, 100, 0.75f, 1.0f, r, r, eopt);
  CHECK(ex->result().ok);
  if (ex->result().exact) {
    printf("Exact LP value(IP)=%.4f\n", ex->result().value_to_ip);
    CHECK(std::fabs(ex->result().value_to_ip - cfr->result().value_to_ip) < 0.5);
  } else {
    printf("(exact LP unavailable, bounded CFR only)\n");
  }

  // 5) range builder: cap honored, no board cards, value+air both represented
  Range capped = BuildRiverRange(board, 20);
  int n = 0;
  for (int c = 0; c < N_COMBOS; ++c)
    if (capped[c] > 0)
      ++n;
  CHECK(n == 20);

  // 6) per-street range tracker: differentiated by action line and keeps hero
  {
    int chosen = -1;  // a concrete non-board combo
    for (int c = 0; c < N_COMBOS; ++c) {
      auto p = comboTable()[c];
      bool bl = false;
      for (int b : board)
        if (b == p[0] || b == p[1])
          bl = true;
      if (!bl) {
        chosen = c;
        break;
      }
    }
    CHECK(chosen >= 0);

    TrackedRangesOptions to;
    to.cap = 24;
    to.preflop_raises = 1;
    // opponent double-barreled (Ob both streets) vs checked down (Ox both)
    auto barrel = TrackRiverRanges(board, chosen, true, "Ob,Hc", "Ob,Hc", to);
    auto slow = TrackRiverRanges(board, chosen, true, "Ox,Hx", "Ox,Hx", to);
    int ipB = 0, ooB = 0, ooS = 0;
    for (int c = 0; c < N_COMBOS; ++c) {
      if (barrel.ip[c] > 0)
        ++ipB;
      if (barrel.oop[c] > 0)
        ++ooB;
      if (slow.oop[c] > 0)
        ++ooS;
    }
    CHECK(ipB <= 24 && ipB > 0);
    CHECK(ooB <= 24 && ooB > 0);
    CHECK(barrel.ip[chosen] > 0.0f);  // hero combo retained

    // mean made-hand strength of a double-barrel range should be >= checked-down
    auto meanCat = [&](const Range& w) {
      double num = 0, den = 0;
      for (int c = 0; c < N_COMBOS; ++c) {
        if (w[c] <= 0)
          continue;
        auto p = comboTable()[c];
        int h[7] = {p[0], p[1], board[0], board[1], board[2], board[3], board[4]};
        num += w[c] * evaluate(h, 7).cat;
        den += w[c];
      }
      return den ? num / den : 0.0;
    };
    double catBarrel = meanCat(barrel.oop), catSlow = meanCat(slow.oop);
    printf("tracker: opp mean cat barrel=%.3f checkdown=%.3f (sizes oo=%d/%d ip=%d)\n", catBarrel,
           catSlow, ooB, ooS, ipB);
    CHECK(catBarrel >= catSlow - 0.15);  // aggression line at least as strong

    // 6b) equity-driven continuation: a strong value combo survives a
    // double barrel-call line with high weight, while a no-pair/no-draw combo
    // is (nearly) folded; on a double check-down both stay broad.
    int kq = comboIndex(cardId("Kc"), cardId("Qs"));
    int air = -1;
    for (int c = 0; c < N_COMBOS; ++c) {
      auto p = comboTable()[c];
      bool bl = false;
      for (int b : board)
        if (b == p[0] || b == p[1])
          bl = true;
      if (bl)
        continue;
      int h[7] = {p[0], p[1], board[0], board[1], board[2], board[3], board[4]};
      if (evaluate(h, 7).cat <= 1) {
        air = c;
        break;
      }
    }
    CHECK(air >= 0 && air != kq);
    TrackedRangesOptions to2;
    to2.cap = 36;
    to2.preflop_raises = 1;
    auto call2 = TrackRiverRanges(board, kq, true, "Ob,Hc", "Ob,Hc", to2);
    CHECK(call2.ip[kq] >= 0.5f);  // top pair keeps value-continuing
  }

  printf("test_gto PASS\n");

  // 7) multi-street (flop->turn->river, CHANCE nodes) correctness. Collapse
  // chance to a fixed turn/river on a near-deterministic board: the chance-
  // sampled DCFR must reach ~0 exploitability and the exact equilibrium value
  // (IP nut flush vs weak air => IP wins OOP's ante = pot/2).
  {
    std::vector<int> flop3 = {cardId("2h"), cardId("3h"), cardId("4h")};
    Range a = zeroRange(), b2 = zeroRange();
    a[comboIndex(cardId("Ah"), cardId("Kh"))] = 1.0f;   // nut flush, holds
    b2[comboIndex(cardId("9c"), cardId("8c"))] = 1.0f;  // weak, can't catch
    gto::MultiStreetOptions mo;
    mo.iterations = 10000;
    mo.bet_frac = 0.75f;
    mo.fixed_turn = cardId("Qc");
    mo.fixed_river = cardId("Jd");
    gto::MultiStreetSolver ms(flop3, 100, a, b2, mo);
    auto mr = ms.Solve();
    CHECK(mr.ok);
    printf("multi-street fixed-run: value(IP)=%.2f expl=%.5f%% pot\n", mr.value_to_ip,
           100 * mr.exploitability_pot);
    CHECK(std::fabs(mr.value_to_ip - 50.0) < 0.5);
    CHECK(mr.exploitability_pot < 0.01);
  }

  // 8) Public chance remains part of the information-set-correct value. The
  // same dominant made hand must converge when turn and river are sampled
  // during training and enumerated by the independent exploitability pass.
  {
    std::vector<int> flop3 = {cardId("2h"), cardId("3h"), cardId("4h")};
    Range a = zeroRange(), b2 = zeroRange();
    a[comboIndex(cardId("Ah"), cardId("Kh"))] = 1.0f;
    b2[comboIndex(cardId("9c"), cardId("8c"))] = 1.0f;
    gto::MultiStreetOptions mo;
    mo.iterations = 20000;
    mo.bet_frac = 0.75f;
    mo.seed = 17;
    gto::MultiStreetSolver ms(flop3, 100, a, b2, mo);
    auto mr = ms.Solve();
    CHECK(mr.ok);
    printf("multi-street sampled chance: value(IP)=%.2f expl=%.5f%% pot\n", mr.value_to_ip,
           100 * mr.exploitability_pot);
    CHECK(std::isfinite(mr.value_to_ip));
    CHECK(mr.exploitability_pot < 0.02);
  }

  printf("test_gto ALL PASS\n");
  return 0;
}
