// Independent brute-force BR/equilibrium evaluator for the FIXED-RUN game.
// Walks its own public tree, reads trained behavior via PolicyAt, and computes
// showdown directly. Fixed turn/river => no chance, so it's an exact finite game.
#include <algorithm>
#include <bs/eval.hpp>
#include <bs/range.hpp>
#include <cmath>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include "gto/multistreet_cfr.h"
using namespace bs;
static int C(const char* s) {
  return cardId(std::string(s));
}

struct V {
  const gto::MultiStreetSolver& S;
  double a, B;
  std::vector<int> ipC, ooC;
  std::vector<double> ipW, ooW;
  int T, R;

  bool share(int i, int j) {
    auto a = comboTable()[ipC[i]], b = comboTable()[ooC[j]];
    return a[0] == b[0] || a[0] == b[1] || a[1] == b[0] || a[1] == b[1];
  }

  double sd(int resp, int own, int opponent, int commits) {
    int ii = resp == 0 ? own : opponent;
    int oi = resp == 0 ? opponent : own;
    auto p = comboTable()[ipC[ii]], o = comboTable()[ooC[oi]];
    int h[7] = {p[0], p[1], S.Flop()[0], S.Flop()[1], S.Flop()[2], T, R};
    int g[7] = {o[0], o[1], S.Flop()[0], S.Flop()[1], S.Flop()[2], T, R};
    unsigned s1 = evaluate(h, 7).score, s2 = evaluate(g, 7).score;
    double payoff = a + commits * B;
    double ipValue = s1 > s2 ? payoff : s1 < s2 ? -payoff : 0;
    return resp == 0 ? ipValue : -ipValue;
  }

  double rec(int resp, int own, const std::vector<double>& reach, int st, int h, int commits,
             int history, bool best) {
    if (h < 0) {
      if (st == 2) {
        double value = 0;
        for (int opponent = 0; opponent < (int)reach.size(); ++opponent)
          value += reach[opponent] * sd(resp, own, opponent, commits);
        return value;
      }
      return rec(resp, own, reach, st + 1, 0, commits, history * 3 + (-h - 1), best);
    }
    int actor = (h == 0 || h == 3) ? 0 : 1;
    bool isOwn = actor == resp;
    auto transition = [&](int act) {
      int nh, nc = commits;
      if (h == 0)
        nh = act ? 2 : 1;
      else if (h == 1)
        nh = act ? 3 : -1;
      else if (act == 0)
        nh = 99;
      else {
        nh = (h == 2 ? -2 : -3);
        nc = commits + 1;
      }
      return std::pair{nh, nc};
    };
    auto val = [&](int act, const std::vector<double>& childReach) -> double {
      auto [nh, nc] = transition(act);
      if (nh == 99) {
        double pay = a + commits * B;
        bool ipFolds = (h == 3);
        double uip = ipFolds ? -pay : pay;
        double total = 0;
        for (double probability : childReach)
          total += probability;
        return total * (resp == 0 ? uip : -uip);
      }
      return rec(resp, own, childReach, st, nh, nc, history, best);
    };
    if (isOwn) {
      if (best)
        return std::max(val(0, reach), val(1, reach));
      int combo = resp == 0 ? ipC[own] : ooC[own];
      int turn = st >= 1 ? T : -1;
      int river = st >= 2 ? R : -1;
      double p0 = S.PolicyAt(resp, combo, st, turn, river, h, 0, history);
      return p0 * val(0, reach) + (1.0 - p0) * val(1, reach);
    }

    double value = 0;
    for (int act = 0; act < 2; ++act) {
      std::vector<double> childReach(reach.size(), 0.0);
      for (int opponent = 0; opponent < (int)reach.size(); ++opponent) {
        int combo = actor == 0 ? ipC[opponent] : ooC[opponent];
        int turn = st >= 1 ? T : -1;
        int river = st >= 2 ? R : -1;
        childReach[opponent] =
            reach[opponent] * S.PolicyAt(actor, combo, st, turn, river, h, act, history);
      }
      value += val(act, childReach);
    }
    return value;
  }

  double agg(int resp, int own, bool best) {
    std::vector<double> reach(resp == 0 ? ooC.size() : ipC.size(), 0.0);
    double total = 0;
    int no = resp == 0 ? (int)ooC.size() : (int)ipC.size();
    for (int q = 0; q < no; ++q) {
      int ii = resp == 0 ? own : q, oi = resp == 0 ? q : own;
      if (share(ii, oi))
        continue;
      double w = resp == 0 ? ooW[q] : ipW[q];
      reach[q] = w;
      total += w;
    }
    if (total <= 0)
      return 0;
    for (double& probability : reach)
      probability /= total;
    return rec(resp, own, reach, 0, 0, 0, 0, best);
  }
};

int main() {
  std::vector<int> f = {C("Kh"), C("7s"), C("2d")};
  Range ri = zeroRange(), ro = zeroRange();
  int ipg[2][2] = {{C("As"), C("Ad")}, {C("2c"), C("3c")}};
  int oog[2][2] = {{C("Ah"), C("Ac")}, {C("9c"), C("8c")}};
  for (auto& h : ipg)
    ri[comboIndex(h[0], h[1])] = 1;
  for (auto& h : oog)
    ro[comboIndex(h[0], h[1])] = 1;
  gto::MultiStreetOptions o;
  o.iterations = 100000;
  o.bet_frac = .75f;
  o.buckets = 0;
  o.fixed_turn = C("Qc");
  o.fixed_river = C("Jd");
  gto::MultiStreetSolver S(f, 100, ri, ro, o);
  const auto result = S.Solve();

  V v{S, 50.0, 75.0, {}, {}, {}, {}, o.fixed_turn, o.fixed_river};
  for (int k = 0; k < 2; ++k) {
    v.ipC.push_back(S.ComboId(0, k));
    v.ipW.push_back(S.ComboWeight(0, k));
    v.ooC.push_back(S.ComboId(1, k));
    v.ooW.push_back(S.ComboWeight(1, k));
  }

  bool ok = true;
  for (int rep = 0; rep < 2; ++rep)
    for (int own = 0; own < 2; ++own) {
      double ref = v.agg(rep, own, true), slv = S.BrValue(rep, own);
      double refe = v.agg(rep, own, false), slve = S.EqValue(rep, own);
      bool m = std::fabs(ref - slv) < 0.5 && std::fabs(refe - slve) < 0.5;
      printf("%s own%d refBR=%+8.2f slvBR=%+8.2f refEQ=%+8.2f slvEQ=%+8.2f %s\n",
             rep == 0 ? "IP" : "OO", own, ref, slv, refe, slve, m ? "MATCH" : "*** MISMATCH ***");
      ok = ok && m;
    }
  bool converged = result.exploitability_pot < 0.002;
  printf("multi-combo fixed-run: value(IP)=%.4f exploitability=%.5f%% pot %s\n", result.value_to_ip,
         100 * result.exploitability_pot, converged ? "CONVERGED" : "*** NOT CONVERGED ***");
  ok = ok && converged;

  Range weightedIp = zeroRange(), weightedOop = zeroRange();
  weightedIp[comboIndex(C("As"), C("Ad"))] = 3.0f;
  weightedIp[comboIndex(C("Ac"), C("3c"))] = 1.0f;
  weightedOop[comboIndex(C("Ah"), C("Ac"))] = 5.0f;
  weightedOop[comboIndex(C("9c"), C("8c"))] = 2.0f;
  gto::MultiStreetSolver weighted(f, 100, weightedIp, weightedOop, o);
  const auto weightedResult = weighted.Solve();
  V weightedReference{weighted, 50.0, 75.0, {}, {}, {}, {}, o.fixed_turn, o.fixed_river};
  for (int k = 0; k < weighted.NumCombos(0); ++k) {
    weightedReference.ipC.push_back(weighted.ComboId(0, k));
    weightedReference.ipW.push_back(weighted.ComboWeight(0, k));
  }
  for (int k = 0; k < weighted.NumCombos(1); ++k) {
    weightedReference.ooC.push_back(weighted.ComboId(1, k));
    weightedReference.ooW.push_back(weighted.ComboWeight(1, k));
  }
  bool weightedMatch = weightedResult.ok && std::isfinite(weightedResult.exploitability_pot);
  for (int responder = 0; responder < 2; ++responder) {
    for (int own = 0; own < weighted.NumCombos(responder); ++own) {
      double referenceBr = weightedReference.agg(responder, own, true);
      double solverBr = weighted.BrValue(responder, own);
      double referenceEq = weightedReference.agg(responder, own, false);
      double solverEq = weighted.EqValue(responder, own);
      weightedMatch = weightedMatch && std::fabs(referenceBr - solverBr) < 0.5 &&
                      std::fabs(referenceEq - solverEq) < 0.5;
    }
  }
  bool weightedConverged = weightedResult.exploitability_pot < 0.002;
  printf("weighted overlap fixed-run: value(IP)=%.4f exploitability=%.5f%% pot %s\n",
         weightedResult.value_to_ip, 100 * weightedResult.exploitability_pot,
         weightedMatch && weightedConverged ? "PASS" : "*** FAILED ***");
  ok = ok && weightedMatch && weightedConverged;

  printf(ok ? "multistreet BR oracle: MATCH independent reference\n"
            : "multistreet BR oracle: MISMATCH\n");
  return ok ? 0 : 1;
}
