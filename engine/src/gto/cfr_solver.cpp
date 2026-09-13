#include "cfr_solver.h"

#include <algorithm>
#include <bs/eval.hpp>
#include <chrono>
#include <cmath>
#include <string>
#include <string_view>
#include <unordered_map>

namespace bs::gto {
namespace {

// Full one-bet/one-raise river, full-width DCFR+ with frozen policy snapshots.
// Public histories (c=check, b=bet/lead, r=raise, f=fold, ca=call):
//   ""  IP c/b;  "c" OOP c/b;  "b" OOP f/ca/r;  "cb" IP f/ca/r;
//   "br" IP f/ca;  "cbr" OOP f/ca.
struct ISet {
  int na = 2;
  std::array<double, 3> r{}, s{}, cur{};
  void policy(double* o) const {
    double z = 0;
    for (int a = 0; a < na; ++a)
      z += std::max(0.0, r[a]);
    if (z <= 1e-12) {
      for (int a = 0; a < na; ++a)
        o[a] = 1.0 / na;
      return;
    }
    for (int a = 0; a < na; ++a)
      o[a] = std::max(0.0, r[a]) / z;
  }
  void freeze() { policy(cur.data()); }
  double avg(int a) const {
    double z = 0;
    for (int i = 0; i < na; ++i)
      z += s[i];
    return z > 1e-12 ? s[a] / z : 1.0 / na;
  }
};

struct Solver {
  std::vector<int> ipC, oopC;
  std::vector<uint32_t> ipS, oopS;
  std::vector<double> ipW, oopW;
  std::unordered_map<int, ISet> N[6];  // RiverNode order
  int Ni = 0, No = 0;
  double P = 0, B = 0, R = 0, Wi = 0, Wo = 0;

  explicit Solver(const std::vector<int>& board, int pot, float bf, float rf, const Range& wr_ip,
                  const Range& wr_oop) {
    P = (double)pot;
    B = bf * P;
    R = rf * (P + 2 * B);
    bool bl[52] = {};
    for (int x : board)
      bl[x] = true;
    for (int c = 0; c < N_COMBOS; ++c) {
      auto [a, b2] = comboTable()[c];
      if (bl[a] || bl[b2])
        continue;
      int cd[7] = {a, b2};
      int n = 2;
      for (int x : board)
        cd[n++] = x;
      uint32_t sc = evaluate(cd, n).score;
      if (wr_ip[c] > 0) {
        ipC.push_back(c);
        ipS.push_back(sc);
        ipW.push_back(wr_ip[c]);
      }
      if (wr_oop[c] > 0) {
        oopC.push_back(c);
        oopS.push_back(sc);
        oopW.push_back(wr_oop[c]);
      }
    }
    Ni = (int)ipC.size();
    No = (int)oopC.size();
    for (int c : ipC) {
      N[0][c].na = 2;
      N[3][c].na = 3;
      N[4][c].na = 2;
    }
    for (int c : oopC) {
      N[1][c].na = 2;
      N[2][c].na = 3;
      N[5][c].na = 2;
    }
    for (double w : ipW)
      Wi += w;
    for (double w : oopW)
      Wo += w;
  }

  bool share(int i, int j) const {
    auto [a, b] = comboTable()[ipC[i]];
    auto [d, e] = comboTable()[oopC[j]];
    return a == d || a == e || b == d || b == e;
  }
  int win(int i, int j) const { return ipS[i] > oopS[j] ? 1 : ipS[i] < oopS[j] ? -1 : 0; }
  static int nodeIdx(std::string_view h) {
    if (h.empty())
      return 0;
    if (h == "c")
      return 1;
    if (h == "b")
      return 2;
    if (h == "cb")
      return 3;
    if (h == "br")
      return 4;
    return 5;  // "cbr"
  }
  static std::string_view childHist(std::string_view h, int a) {
    if (h.empty())
      return a == 0 ? "c" : "b";
    if (h == "c")
      return a == 0 ? "cc" : "cb";
    if (h == "b")
      return a == 0 ? "bf" : a == 1 ? "bc" : "br";
    if (h == "cb")
      return a == 0 ? "cbf" : a == 1 ? "cbc" : "cbr";
    if (h == "br")
      return a == 0 ? "brf" : "brc";
    return a == 0 ? "cbrf" : "cbrc";
  }
  static bool isLeaf(std::string_view h) {
    static constexpr std::string_view L[] = {"cc",  "bf",  "bc",   "cbf", "cbc",
                                             "brf", "brc", "cbrf", "cbrc"};
    for (auto x : L)
      if (h == x)
        return true;
    return false;
  }
  static int owner(std::string_view h) { return (h.empty() || h == "cb" || h == "br") ? 0 : 1; }
  static int nAct(std::string_view h) { return (h == "b" || h == "cb") ? 3 : 2; }
  // strict zero-sum net chip payoff to IP (each anted a=P/2)
  double leaf(std::string_view h, int i, int j) const {
    const int r = win(i, j);
    const double a = P / 2;
    auto sd = [&](double s) { return r == 1 ? s : r == 0 ? 0.0 : -s; };
    if (h == "cc")
      return sd(a);
    if (h == "bf")
      return a;
    if (h == "bc")
      return sd(a + B);
    if (h == "cbf")
      return -a;
    if (h == "cbc")
      return sd(a + B);
    if (h == "brf")
      return -(a + B);
    if (h == "brc")
      return sd(a + R);
    if (h == "cbrf")
      return a + B;
    return sd(a + R);  // cbrc
  }

  double cfr(int i, int j, std::string_view h, double p0, double p1, int tr) {
    if (isLeaf(h)) {
      double u = leaf(h, i, j);
      return tr == 0 ? u : -u;
    }
    const int pl = owner(h), ni = nodeIdx(h);
    const int combo = pl == 0 ? ipC[i] : oopC[j];
    ISet& is = N[ni][combo];
    const double* stp = is.cur.data();
    const int na = is.na;
    double cu[3], node = 0;
    for (int a = 0; a < na; ++a) {
      double np0 = p0, np1 = p1;
      if (pl == 0)
        np0 *= stp[a];
      else
        np1 *= stp[a];
      cu[a] = cfr(i, j, childHist(h, a), np0, np1, tr);
      node += stp[a] * cu[a];
    }
    if (pl == tr) {
      const double opp = pl == 0 ? p1 : p0;
      const double self = pl == 0 ? p0 : p1;
      for (int a = 0; a < na; ++a) {
        is.r[a] += opp * (cu[a] - node);
        is.s[a] += self * stp[a];
      }
    }
    return node;
  }

  mutable std::vector<double> effO, effI;
  void effW() const {
    if (!effO.empty())
      return;
    effO.assign(Ni, 0);
    effI.assign(No, 0);
    for (int a = 0; a < Ni; ++a)
      for (int b = 0; b < No; ++b)
        if (!share(a, b)) {
          effO[a] += oopW[b];
          effI[b] += ipW[a];
        }
  }

  void iterate() {
    effW();
    for (auto& m : N)
      for (auto& [k, is] : m)
        is.freeze();
    for (int tr = 0; tr < 2; ++tr)
      for (int i = 0; i < Ni; ++i)
        for (int j = 0; j < No; ++j) {
          if (share(i, j))
            continue;
          double chance =
              tr == 0 ? (ipW[i] / Wi) * (oopW[j] / effO[i]) : (oopW[j] / Wo) * (ipW[i] / effI[j]);
          cfr(i, j, "", chance, chance, tr);
        }
  }
  void discount(long long t) {
    double f = (double)t / (t + 1);
    for (auto& m : N)
      for (auto& [k, is] : m)
        for (int a = 0; a < is.na; ++a) {
          is.r[a] = is.r[a] > 0 ? is.r[a] * f : 0;
          is.s[a] *= f;
        }
  }

  const ISet& iset(std::string_view h, int combo) const { return N[nodeIdx(h)].at(combo); }

  // ---- correct info-set best response: aggregate opponent distribution
  // (deal weight * opponent avg reach) BEFORE choosing at every own node ----
  mutable std::unordered_map<long, int> baCache;
  static int hidx(std::string_view h) {
    static constexpr std::string_view A[] = {"",   "c",   "b",   "cb",  "br",  "cbr",  "cc",  "bf",
                                             "bc", "cbf", "cbc", "brf", "brc", "cbrf", "cbrc"};
    for (int k = 0; k < 15; ++k)
      if (h == A[k])
        return k;
    return -1;
  }
  void ijOf(int resp, int own, int q, int& i, int& j) const {
    if (resp == 0) {
      i = own;
      j = q;
    } else {
      i = q;
      j = own;
    }
  }
  bool validPair(int resp, int own, int q) const {
    int i, j;
    ijOf(resp, own, q, i, j);
    return !share(i, j);
  }
  double oppAvg(int resp, int q, std::string_view h, int a) const {
    int g = resp == 0 ? oopC[q] : ipC[q];
    return iset(h, g).avg(a);
  }
  double oppReach(int resp, int q, std::string_view h) const {
    double r = 1;
    std::string s;
    for (size_t z = 0; z < h.size(); ++z) {
      if (owner(s) == 1 - resp) {
        int a = -1;
        for (int k = 0; k < nAct(s); ++k)
          if (childHist(s, k) == s + h[z])
            a = k;
        r *= oppAvg(resp, q, s, a);
      }
      s += h[z];
    }
    return r;
  }
  double dealW(int resp, int q) const { return resp == 0 ? oopW[q] : ipW[q]; }
  int nOpp(int resp) const { return resp == 0 ? No : Ni; }

  double brVal(int resp, std::string_view h, int own, int q) const {
    if (isLeaf(h)) {
      int i, j;
      ijOf(resp, own, q, i, j);
      double u = leaf(h, i, j);
      return resp == 0 ? u : -u;
    }
    if (owner(h) == resp) {
      long key = (long)resp * 100000 + hidx(h) * 1000 + own;
      int a = baCache.count(key) ? baCache.at(key) : bestAction(resp, h, own);
      return brVal(resp, childHist(h, a), own, q);
    }
    double v = 0;
    for (int a = 0; a < nAct(h); ++a)
      v += oppAvg(resp, q, h, a) * brVal(resp, childHist(h, a), own, q);
    return v;
  }
  int bestAction(int resp, std::string_view h, int own) const {
    long key = (long)resp * 100000 + hidx(h) * 1000 + own;
    auto it = baCache.find(key);
    if (it != baCache.end())
      return it->second;
    double best = -1e30;
    int ba = 0;
    for (int a = 0; a < nAct(h); ++a) {
      double score = 0;
      for (int q = 0; q < nOpp(resp); ++q) {
        if (!validPair(resp, own, q))
          continue;
        score += dealW(resp, q) * oppReach(resp, q, h) * brVal(resp, childHist(h, a), own, q);
      }
      if (score > best) {
        best = score;
        ba = a;
      }
    }
    baCache[key] = ba;
    return ba;
  }
  double eqVal(int resp, std::string_view h, int own, int q) const {
    if (isLeaf(h)) {
      int i, j;
      ijOf(resp, own, q, i, j);
      double u = leaf(h, i, j);
      return resp == 0 ? u : -u;
    }
    int i, j;
    ijOf(resp, own, q, i, j);
    int g = owner(h) == 0 ? ipC[i] : oopC[j];
    double v = 0;
    for (int a = 0; a < nAct(h); ++a)
      v += iset(h, g).avg(a) * eqVal(resp, childHist(h, a), own, q);
    return v;
  }
  double brOwn(int resp, int own) const {
    double num = 0, den = 0;
    if (resp == 0) {
      int a = bestAction(0, "", own);
      for (int q = 0; q < No; ++q) {
        if (!share(own, q))
          continue;
        double w = oopW[q];
        num += w * brVal(0, childHist("", a), own, q);
        den += w;
      }
    } else {
      for (int q = 0; q < Ni; ++q) {
        if (!share(q, own))
          continue;
        double w = ipW[q];
        num += w * brVal(1, "", own, q);
        den += w;
      }
    }
    return den ? num / den : 0;
  }
  double eqOwn(int resp, int own) const {
    double num = 0, den = 0;
    for (int q = 0; q < nOpp(resp); ++q) {
      if (!validPair(resp, own, q))
        continue;
      double w = dealW(resp, q);
      num += w * eqVal(resp, "", own, q);
      den += w;
    }
    return den ? num / den : 0;
  }
};

}  // namespace

CfrEquilibrium SolveCfr(const std::vector<int>& board, int pot, float bet_frac, float raise_frac,
                        const Range& ip_range, const Range& oop_range, const CfrOptions& opts) {
  CfrEquilibrium out;
  Solver s(board, pot, bet_frac, raise_frac, ip_range, oop_range);
  if (s.Ni == 0 || s.No == 0)
    return out;
  const bool timed = opts.time_budget_s > 0.0;
  const auto start = std::chrono::steady_clock::now();
  int done = 0;
  for (int t = 1; t <= opts.iterations; ++t) {
    s.iterate();
    s.discount(t);
    ++done;
    // check the clock periodically (iteration batches grow geometrically so the
    // check overhead stays negligible)
    if (timed && (t & 511) == 0) {
      double el = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
      if (el >= opts.time_budget_s)
        break;
    }
  }
  out.iterations_done = done;

  // per-combo average behavior at each of the six nodes
  for (int ni = 0; ni < 6; ++ni) {
    const auto& combos = (ni == 0 || ni == 3 || ni == 4) ? s.ipC : s.oopC;
    for (int c : combos) {
      const ISet& is = s.N[ni].at(c);
      std::array<double, 3> p{0, 0, 0};
      for (int a = 0; a < is.na; ++a)
        p[a] = is.avg(a);
      out.node[ni][c] = p;
    }
  }

  s.baCache.clear();
  double brI = 0, eqI = 0, brO = 0, eqO = 0;
  for (int i = 0; i < s.Ni; ++i) {
    brI += (s.ipW[i] / s.Wi) * s.brOwn(0, i);
    eqI += (s.ipW[i] / s.Wi) * s.eqOwn(0, i);
  }
  for (int j = 0; j < s.No; ++j) {
    brO += (s.oopW[j] / s.Wo) * s.brOwn(1, j);
    eqO += (s.oopW[j] / s.Wo) * s.eqOwn(1, j);
  }
  double gi = std::max(0.0, brI - eqI) / s.P;
  double go = std::max(0.0, brO - eqO) / s.P;
  out.ok = true;
  out.value_to_ip = eqI;
  out.exploitability_pot = gi + go;
  return out;
}

}  // namespace bs::gto
