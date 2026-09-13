// Independent brute-force BR/equilibrium evaluator for the FIXED-RUN game.
// Walks its own public tree, reads trained behavior via PolicyAt, and computes
// showdown directly. Fixed turn/river => no chance, so it's an exact finite game.
#include <algorithm>
#include <bs/eval.hpp>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "gto/multistreet_cfr.h"
using namespace bs;
static int C(const char* s) {
  return cardId(std::string(s));
}

struct V {
  const gto::MultiStreetSolver& S;
  std::vector<int> turn{0, 0}, river{0, 0};
  double a, B;
  // global combo ids
  std::vector<int> ipC, ooC;
  std::vector<double> ipW, ooW;
  int T, R;

  bool share(int i, int j) {
    auto a = comboTable()[ipC[i]], b = comboTable()[ooC[j]];
    return a[0] == b[0] || a[0] == b[1] || a[1] == b[0] || a[1] == b[1];
  }

  // showdown payoff to IP given extra bet commits (0..3)
  double sd(int ii, int oi, int cc) {
    auto p = comboTable()[ipC[ii]], o = comboTable()[ooC[oi]];
    int h[7] = {p[0], p[1], S.Flop()[0], S.Flop()[1], S.Flop()[2], T, R};
    int g[7] = {o[0], o[1], S.Flop()[0], S.Flop()[1], S.Flop()[2], T, R};
    unsigned s1 = evaluate(h, 7).score, s2 = evaluate(g, 7).score;
    double c = a + cc * B;
    return s1 > s2 ? c : s1 < s2 ? -c : 0;
  }

  // resp perspective, own index fixed; walk public tree.
  // h: 0 IP c/b ;1 OOP c/lead ;2 OOP f/c vs bet ;3 IP f/c vs lead ; neg closed
  double rec(int resp, int own, int oi, bool bound, int st, int h, int cc, bool best) {
    if (h < 0) {
      if (st == 2) {
        double u = sd(resp == 0 ? own : oi, resp == 0 ? oi : own, cc);
        return resp == 0 ? u : -u;
      }
      // fixed run -> advance street with no branching
      return rec(resp, own, oi, bound, st + 1, 0, cc, best);
    }
    int actor = (h == 0 || h == 3) ? 0 : 1;
    int polPlayer = actor;
    int combo = actor == 0 ? ipC[(resp == 0 ? own : oi)] : ooC[(resp == 1 ? own : oi)];
    bool isOwn = actor == resp;
    auto val = [&](int act) -> double {
      int nh, nc = cc;
      if (h == 0)
        nh = act ? 2 : 1;
      else if (h == 1)
        nh = act ? 3 : -1;
      else if (act == 0)
        nh = 99;
      else {
        nh = (h == 2 ? -2 : -3);
        nc = cc + 1;
      }
      if (nh == 99) {
        double pay = a + cc * B;
        bool ipFolds = (h == 3);
        double uip = ipFolds ? -pay : pay;
        return resp == 0 ? uip : -uip;
      }
      return rec(resp, own, oi, bound, st, nh, nc, best);
    };
    if (isOwn) {
      if (best)
        return std::max(val(0), val(1));
      double pown = S.PolicyAt(resp, combo, st, T, R, h, 0);
      return pown * val(0) + (1.0 - pown) * val(1);  // own equilibrium policy
    } else {
      double p0 = S.PolicyAt(polPlayer, combo, st, T, R, h, 0);
      return p0 * val(0) + (1.0 - p0) * val(1);  // opponent avg policy
    }
  }

  // distribution-aggregated: own combo fixed, average over opponent combos
  double agg(int resp, int own, bool best) {
    double num = 0, den = 0;
    int no = resp == 0 ? (int)ooC.size() : (int)ipC.size();
    for (int q = 0; q < no; ++q) {
      int ii = resp == 0 ? own : q, oi = resp == 0 ? q : own;
      if (share(ii, oi))
        continue;
      double w = resp == 0 ? ooW[q] : ipW[q];
      num += w * rec(resp, own, q, false, 0, 0, 0, best);
      den += w;
    }
    return den ? num / den : 0;
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
  S.Solve();

  V v{S, {0, 0}, {0, 0}, 50.0, 75.0, {}, {}, {}, {}, o.fixed_turn, o.fixed_river};
  for (int k = 0; k < 2; ++k) {
    v.ipC.push_back(S.ComboId(0, k));
    v.ipW.push_back(S.ComboWeight(0, k));
    v.ooC.push_back(S.ComboId(1, k));
    v.ooW.push_back(S.ComboWeight(1, k));
  }

  bool ok = true;
  for (int rep = 0; rep < 2; ++rep)
    for (int own = 0; own < 2; ++own) {
      double ref = v.agg(rep, own, true), slv = S.BrValue(rep, own, 1, 1);
      double refe = v.agg(rep, own, false), slve = S.EqValue(rep, own, 1, 1);
      bool m = std::fabs(ref - slv) < 0.5 && std::fabs(refe - slve) < 0.5;
      printf("%s own%d refBR=%+8.2f slvBR=%+8.2f refEQ=%+8.2f slvEQ=%+8.2f %s\n",
             rep == 0 ? "IP" : "OO", own, ref, slv, refe, slve, m ? "MATCH" : "*** MISMATCH ***");
      ok = ok && m;
    }
  printf(ok ? "multistreet BR oracle: MATCH independent reference\n"
            : "multistreet BR oracle: MISMATCH\n");
  return ok ? 0 : 1;
}
