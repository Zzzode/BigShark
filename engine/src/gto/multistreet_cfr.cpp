#include "multistreet_cfr.h"

#include <algorithm>
#include <bs/eval.hpp>
#include <cmath>
#include <vector>

namespace bs::gto {

// h-code within a street
//   0  IP: check/bet
//   1  OOP after IP checks: check / lead
//   2  OOP faces IP bet: fold / call
//   3  IP faces OOP lead: fold / call
// terminal sentinels passed as negative hcode (street already advanced):
//   -1 check-check closed; -2 IP bet called; -3 OOP lead called

void MultiStreetSolver::ISet::policy(double* o) const {
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
void MultiStreetSolver::ISet::freeze() {
  policy(cur.data());
}
double MultiStreetSolver::ISet::avg(int a) const {
  double z = 0;
  for (int i = 0; i < na; ++i)
    z += s[i];
  return z > 1e-12 ? s[a] / z : 1.0 / na;
}

MultiStreetSolver::MultiStreetSolver(std::vector<int> flop, int pot, const Range& ipr,
                                     const Range& oopr, const MultiStreetOptions& opt)
    : flop_(std::move(flop)), pot_(pot), bfrac_(opt.bet_frac), opt_(opt) {
  bool bl[52] = {};
  for (int c : flop_)
    bl[c] = true;
  for (int c = 0; c < N_COMBOS; ++c) {
    auto [a, b] = comboTable()[c];
    if (bl[a] || bl[b])
      continue;
    if (ipr[c] > 0) {
      ipC_.push_back(c);
      ipW_.push_back(ipr[c]);
    }
    if (oopr[c] > 0) {
      oopC_.push_back(c);
      oopW_.push_back(oopr[c]);
    }
  }
  Ni_ = (int)ipC_.size();
  No_ = (int)oopC_.size();
  for (double w : ipW_)
    Wi_ += w;
  for (double w : oopW_)
    Wo_ += w;
}

bool MultiStreetSolver::share(int i, int j) const {
  auto [a, b] = comboTable()[ipC_[i]];
  auto [d, e] = comboTable()[oopC_[j]];
  return a == d || a == e || b == d || b == e;
}
uint64_t MultiStreetSolver::key(int combo, int street, int hcode, int bucket) const {
  uint64_t k = (uint64_t)combo;
  k = (k << 2) | (uint64_t)street;
  k = (k << 3) | (uint64_t)(hcode & 7);
  k = (k << 8) | (uint64_t)(bucket & 0xff);
  return k;
}
MultiStreetSolver::ISet& MultiStreetSolver::iset(int player, int combo, int street, int hcode,
                                                 int bucket) {
  ISet& s = N_[player][key(combo, street, hcode, bucket)];
  if (s.na == 0)
    s.na = 2;
  return s;
}

// OCHS-style bucket: equity of `combo` (on flop+turn+river currently dealt)
// vs a representative opponent hand sampled from the opponent range, averaged
// over the still-to-come board cards via Monte Carlo; quantized to B buckets.
int MultiStreetSolver::bucketOf(int combo, int player, int turn, int river) const {
  if (opt_.buckets <= 0)
    return combo;  // exact per-combo keying (no abstraction)
  uint64_t ck = (uint64_t)combo;
  ck = (ck << 1) | (uint64_t)player;
  ck = (ck << 7) | (uint64_t)(turn + 1);
  ck = (ck << 7) | (uint64_t)(river + 1);
  auto it = bucketCache_.find(ck);
  if (it != bucketCache_.end())
    return it->second;
  int b = bucketCompute(combo, player, turn, river);
  bucketCache_[ck] = b;
  return b;
}

int MultiStreetSolver::bucketCompute(int combo, int player, int turn, int river) const {
  auto hp = comboTable()[combo];
  bool blocked[52] = {};
  blocked[hp[0]] = blocked[hp[1]] = true;
  for (int c : flop_)
    blocked[c] = true;
  if (turn >= 0)
    blocked[turn] = true;
  // opponent pool = the OTHER player's live combos (exclude card conflicts)
  const std::vector<int>& oc = player == 0 ? oopC_ : ipC_;
  const std::vector<double>& ow = player == 0 ? oopW_ : ipW_;
  std::vector<int> pool;
  std::vector<double> pw;
  double tot = 0;
  for (size_t q = 0; q < oc.size(); ++q) {
    auto p = comboTable()[oc[q]];
    if (blocked[p[0]] || blocked[p[1]])
      continue;
    pool.push_back(oc[q]);
    pw.push_back(ow[q]);
    tot += ow[q];
  }
  if (pool.empty())
    return opt_.buckets / 2;
  std::vector<double> cum(pool.size());
  double acc = 0;
  for (size_t q = 0; q < pool.size(); ++q) {
    acc += pw[q];
    cum[q] = acc;
  }
  bs::XorShift64 rng((uint64_t)(combo * 2654435761u) ^ (uint64_t)(player + 1) ^
                     (uint64_t)(turn + 1) * 7919u ^ (uint64_t)(river + 1) * 104729u ^ 0x9e37u);
  const int nDeal = 5 - (3 + (turn >= 0 ? 1 : 0));  // turn pending ->2, river pending ->1
  int wins = 0, chops = 0, valid = 0;
  for (int it = 0; it < opt_.bucket_mc; ++it) {
    const double pick = rng.unit() * tot;
    size_t q = 0;
    while (q + 1 < cum.size() && cum[q] < pick)
      ++q;
    auto op = comboTable()[pool[q]];
    bool used[52] = {};
    used[hp[0]] = used[hp[1]] = used[op[0]] = used[op[1]] = true;
    for (int c : flop_)
      used[c] = true;
    if (turn >= 0)
      used[turn] = true;
    int board[5];
    int bn = 0;
    for (int c : flop_)
      board[bn++] = c;
    if (turn >= 0)
      board[bn++] = turn;
    bool ok = true;
    for (int r = 0; r < nDeal; ++r) {
      int cd = -1;
      for (int t = 0; t < 24; ++t) {
        int x = rng.unit() * 52;
        int xi = (int)x;
        if (xi >= 0 && xi < 52 && !used[xi]) {
          cd = xi;
          break;
        }
      }
      if (cd < 0) {
        ok = false;
        break;
      }
      used[cd] = true;
      board[bn++] = cd;
    }
    if (!ok)
      continue;
    int h1[7] = {hp[0], hp[1], board[0], board[1], board[2], board[3], board[4]};
    int h2[7] = {op[0], op[1], board[0], board[1], board[2], board[3], board[4]};
    unsigned s1 = evaluate(h1, 7).score, s2 = evaluate(h2, 7).score;
    ++valid;
    if (s1 > s2)
      ++wins;
    else if (s1 == s2)
      ++chops;
  }
  double eq = valid ? (wins + 0.5 * chops) / valid : 0.5;
  int b = (int)(eq * opt_.buckets);
  if (b >= opt_.buckets)
    b = opt_.buckets - 1;
  if (b < 0)
    b = 0;
  return b;
}

int MultiStreetSolver::owner(int /*street*/, int hcode) const {
  return (hcode == 0 || hcode == 3) ? 0 : 1;
}

// showdown payoff to IP with `commits` extra bets each (0 checks, >=1 calls)
double msShowdown(int i, int j, const std::vector<int>& flop, int turn, int river, double a,
                  double B, int commits, const std::vector<int>& ipC,
                  const std::vector<int>& oopC) {
  (void)B;
  int hi[7] = {}, ho[7] = {};
  int n = 0;
  {
    auto pp = comboTable()[ipC[i]];
    hi[n++] = pp[0];
    hi[n++] = pp[1];
  }
  for (int c : flop)
    hi[n++] = c;
  if (turn >= 0)
    hi[n++] = turn;
  if (river >= 0)
    hi[n++] = river;
  int m = 0;
  {
    auto qq = comboTable()[oopC[j]];
    ho[m++] = qq[0];
    ho[m++] = qq[1];
  }
  for (int c : flop)
    ho[m++] = c;
  if (turn >= 0)
    ho[m++] = turn;
  if (river >= 0)
    ho[m++] = river;
  unsigned s1 = evaluate(hi, n).score, s2 = evaluate(ho, m).score;
  double c = a + commits * B;
  return s1 > s2 ? c : s1 < s2 ? -c : 0.0;
}

int MultiStreetSolver::drawCard(const bool used[52], bs::XorShift64& rng) const {
  int pool = 0;
  for (int c = 0; c < 52; ++c)
    if (!used[c])
      ++pool;
  int k = rng.below(pool), seen = 0;
  for (int c = 0; c < 52; ++c) {
    if (used[c])
      continue;
    if (seen == k)
      return c;
    ++seen;
  }
  return -1;
}

double MultiStreetSolver::cfr(int i, int j, int turn, int river, int street, int hcode, int commits,
                              double p0, double p1, int tr) {
  const double a = pot_ / 2.0, B = bfrac_ * pot_;
  auto sd = [&](int cc) { return msShowdown(i, j, flop_, turn, river, a, B, cc, ipC_, oopC_); };

  // ---- street-closed sentinel ----
  if (hcode < 0) {
    if (street == 2) {
      double u = sd(commits);
      return tr == 0 ? u : -u;
    }
    int cd = street == 0 ? sampledTurn_ : sampledRiver_;
    int nt = turn, nr = river;
    if (street == 0)
      nt = cd;
    else
      nr = cd;
    return cfr(i, j, nt, nr, street + 1, 0, commits, p0, p1, tr);
  }

  const int pl = owner(street, hcode);
  const int combo = pl == 0 ? ipC_[i] : oopC_[j];
  const int buck = bucketOf(combo, pl, turn, river);
  ISet& s = iset(pl, combo, street, hcode, buck);
  const double* stp = s.cur.data();

  double cu[2], node = 0;
  for (int act = 0; act < 2; ++act) {
    double np0 = p0, np1 = p1;
    if (pl == 0)
      np0 *= stp[act];
    else
      np1 *= stp[act];
    int nh, nc = commits;
    if (hcode == 0) {
      nh = act == 0 ? 1 : 2;
    } else if (hcode == 1) {
      if (act == 0)
        nh = -1;
      else {
        nh = 3;
      }
    } else if (hcode == 2) {
      if (act == 0)
        nh = 99;
      else {
        nh = -2;
        nc = commits + 1;
      }
    } else {
      if (act == 0)
        nh = 99;
      else {
        nh = -3;
        nc = commits + 1;
      }
    }
    double v;
    if (nh == 99) {
      // folder loses only the bets already committed; winner gains opponent's
      double pay = a + commits * B;
      bool ipFolds = (hcode == 3);
      double uip = ipFolds ? -pay : pay;
      v = tr == 0 ? uip : -uip;
    } else {
      v = cfr(i, j, turn, river, street, nh, nc, np0, np1, tr);
    }
    cu[act] = v;
    node += stp[act] * v;
  }
  if (pl == tr) {
    double opp = pl == 0 ? p1 : p0, self = pl == 0 ? p0 : p1;
    for (int act = 0; act < 2; ++act) {
      s.r[act] += opp * (cu[act] - node);
      s.s[act] += self * stp[act];
    }
  }
  return node;
}

void MultiStreetSolver::iterate() {
  for (auto& m : N_[0])
    m.second.freeze();
  for (auto& m : N_[1])
    m.second.freeze();
  bool used[52] = {};
  for (int c : flop_)
    used[c] = true;
  sampledTurn_ = opt_.fixed_turn >= 0 ? opt_.fixed_turn : drawCard(used, rng_);
  if (sampledTurn_ >= 0)
    used[sampledTurn_] = true;
  sampledRiver_ = opt_.fixed_river >= 0 ? opt_.fixed_river : drawCard(used, rng_);
  bool blocked[52] = {};
  if (sampledTurn_ >= 0)
    blocked[sampledTurn_] = true;
  if (sampledRiver_ >= 0)
    blocked[sampledRiver_] = true;
  for (int i = 0; i < Ni_; ++i)
    for (int j = 0; j < No_; ++j) {
      auto pp = comboTable()[ipC_[i]];
      auto qq = comboTable()[oopC_[j]];
      if (blocked[pp[0]] || blocked[pp[1]] || blocked[qq[0]] || blocked[qq[1]])
        continue;
      // conditional private reach: normalize the opponent over combos live
      // given THIS deal, and the traverser's own type by its marginal.
      double effO = 0, effI = 0;
      for (int q = 0; q < No_; ++q) {
        auto o = comboTable()[oopC_[q]];
        if (!(blocked[o[0]] || blocked[o[1]]) &&
            !(pp[0] == o[0] || pp[0] == o[1] || pp[1] == o[0] || pp[1] == o[1]))
          effO += oopW_[q];
      }
      for (int p = 0; p < Ni_; ++p) {
        auto h = comboTable()[ipC_[p]];
        if (!(blocked[h[0]] || blocked[h[1]]) &&
            !(qq[0] == h[0] || qq[0] == h[1] || qq[1] == h[0] || qq[1] == h[1]))
          effI += ipW_[p];
      }
      for (int tr = 0; tr < 2; ++tr) {
        double q = tr == 0 ? (ipW_[i] / Wi_) * (oopW_[j] / (effO > 0 ? effO : 1.0))
                           : (oopW_[j] / Wo_) * (ipW_[i] / (effI > 0 ? effI : 1.0));
        cfr(i, j, -1, -1, 0, 0, 0, q, q, tr);
      }
    }
}
void MultiStreetSolver::discount(long long t) {
  double f = (double)t / (t + 1);
  for (int pm = 0; pm < 2; ++pm)
    for (auto& kv : N_[pm])
      for (int a = 0; a < 2; ++a) {
        kv.second.r[a] = kv.second.r[a] > 0 ? kv.second.r[a] * f : 0;
        kv.second.s[a] *= f;
      }
}

// ---------------- best response (chance averaged before max) ----------------
double MultiStreetSolver::PolicyAt(int player, int combo, int street, int turn, int river,
                                   int hcode, int act) const {
  return polAvg(player, combo, street, hcode, bucketOf(combo, player, turn, river), act);
}
double MultiStreetSolver::polAvg(int player, int combo, int street, int hcode, int bucket,
                                 int act) const {
  auto it = N_[player].find(key(combo, street, hcode, bucket));
  if (it == N_[player].end())
    return 0.5;
  return it->second.avg(act);
}

double MultiStreetSolver::aggFixed(int resp, int own, int fq, int turn, int river, int street,
                                   int hcode, int commits, bool best) const {
  const double a = pot_ / 2.0, B = bfrac_ * pot_;
  auto ownCombo = [&] { return resp == 0 ? comboTable()[ipC_[own]] : comboTable()[oopC_[own]]; };
  auto oc = [&](int q) { return resp == 0 ? comboTable()[oopC_[q]] : comboTable()[ipC_[q]]; };
  auto ownPol = [&](int act) {
    int g = resp == 0 ? ipC_[own] : oopC_[own];
    return polAvg(resp, g, street, hcode, bucketOf(g, resp, turn, river), act);
  };
  // showdown vs the bound opponent combo fq
  auto sdBound = [&](int cc) {
    int ipIdx = resp == 0 ? own : fq, opIdx = resp == 0 ? fq : own;
    auto p = comboTable()[ipC_[ipIdx]];
    auto o = comboTable()[oopC_[opIdx]];
    int hi[7] = {p[0], p[1]}, ho[7] = {o[0], o[1]};
    int n = 2, m = 2;
    for (int c : flop_)
      hi[n++] = c, ho[m++] = c;
    if (turn >= 0)
      hi[n++] = turn, ho[m++] = turn;
    if (river >= 0)
      hi[n++] = river, ho[m++] = river;
    unsigned s1 = evaluate(hi, n).score, s2 = evaluate(ho, m).score;
    double c = a + cc * B;
    return s1 > s2 ? c : s1 < s2 ? -c : 0.0;
  };

  // closed sentinel
  if (hcode < 0) {
    if (street == 2) {
      double u = sdBound(commits);
      return resp == 0 ? u : -u;
    }
    if (aggSampleMode_) {
      int cd = street == 0 ? evalTurn_ : evalRiver_;
      if (cd < 0)
        return 0;
      auto pp = ownCombo();
      auto oo = oc(fq);
      if (pp[0] == cd || pp[1] == cd || oo[0] == cd || oo[1] == cd)
        return 0;
      int nt = turn, nr = river;
      if (street == 0)
        nt = cd;
      else
        nr = cd;
      return aggFixed(resp, own, fq, nt, nr, street + 1, 0, commits, best);
    }
    bool used[52] = {};
    auto pp = ownCombo();
    auto oo = oc(fq);
    used[pp[0]] = used[pp[1]] = used[oo[0]] = used[oo[1]] = true;
    for (int c : flop_)
      used[c] = true;
    if (turn >= 0)
      used[turn] = true;
    int cards = 0;
    for (int cd = 0; cd < 52; ++cd)
      if (!used[cd])
        ++cards;
    double acc = 0;
    for (int cd = 0; cd < 52; ++cd) {
      if (used[cd])
        continue;
      int nt = turn, nr = river;
      if (street == 0)
        nt = cd;
      else
        nr = cd;
      acc += (1.0 / cards) * aggFixed(resp, own, fq, nt, nr, street + 1, 0, commits, best);
    }
    return acc;
  }

  int pl = owner(street, hcode);
  auto foldPay = [&](int cc) {
    double pay = a + cc * B;
    bool ipFolds = (hcode == 3);
    return resp == 0 ? (ipFolds ? -pay : pay) : (ipFolds ? pay : -pay);
  };
  if (pl == resp) {
    auto child = [&](int nh, int nc) {
      // the opponent's private combo is FIXED for the whole hand; only folds
      // terminate. Continued play (including across a street close nh<0) keeps
      // the bound combo — no re-averaging mid-hand.
      if (nh == 99)
        return foldPay(nc);
      return aggFixed(resp, own, fq, turn, river, street, nh, nc, best);
    };
    if (best) {
      double out = -1e30;
      for (int act = 0; act < 2; ++act) {
        int nh, nc = commits;
        if (hcode == 0)
          nh = act ? 2 : 1;
        else if (hcode == 1)
          nh = act ? 3 : -1;
        else if (act == 0)
          nh = 99;
        else {
          nh = (hcode == 2 ? -2 : -3);
          nc = commits + 1;
        }
        out = std::max(out, child(nh, nc));
      }
      return out;
    }
    double ev = 0;
    for (int act = 0; act < 2; ++act) {
      int nh, nc = commits;
      if (hcode == 0)
        nh = act ? 2 : 1;
      else if (hcode == 1)
        nh = act ? 3 : -1;
      else if (act == 0)
        nh = 99;
      else {
        nh = (hcode == 2 ? -2 : -3);
        nc = commits + 1;
      }
      ev += ownPol(act) * child(nh, nc);
    }
    return ev;
  }
  // opponent node with a BOUND combo: use its policy, no weighting/averaging
  int og = resp == 0 ? oopC_[fq] : ipC_[fq];
  double v = 0;
  for (int act = 0; act < 2; ++act) {
    double pr = polAvg(1 - resp, og, street, hcode, bucketOf(og, 1 - resp, turn, river), act);
    int nh, nc = commits;
    if (hcode == 0)
      nh = act ? 2 : 1;
    else if (hcode == 1)
      nh = act ? 3 : -1;
    else if (act == 0)
      nh = 99;
    else {
      nh = (hcode == 2 ? -2 : -3);
      nc = commits + 1;
    }
    double cv =
        (nh == 99) ? foldPay(commits) : aggFixed(resp, own, fq, turn, river, street, nh, nc, best);
    v += pr * cv;
  }
  return v;
}

double MultiStreetSolver::aggCore(int resp, int own, int turn, int river, int street, int hcode,
                                  int commits, bool best) const {
  const double a = pot_ / 2.0, B = bfrac_ * pot_;
  auto ownCombo = [&] { return resp == 0 ? comboTable()[ipC_[own]] : comboTable()[oopC_[own]]; };
  auto oc = [&](int q) { return resp == 0 ? comboTable()[oopC_[q]] : comboTable()[ipC_[q]]; };
  auto ow = [&](int q) -> double { return resp == 0 ? oopW_[q] : ipW_[q]; };
  int nOpp = resp == 0 ? No_ : Ni_;
  auto blocked = [&](int q, int turn, int river) {
    auto p = ownCombo();
    auto o = oc(q);
    if (p[0] == o[0] || p[0] == o[1] || p[1] == o[0] || p[1] == o[1])
      return true;
    for (int c : flop_)
      if (p[0] == c || p[1] == c || o[0] == c || o[1] == c)
        return true;
    if (turn >= 0 && (p[0] == turn || p[1] == turn || o[0] == turn || o[1] == turn))
      return true;
    if (river >= 0 && (p[0] == river || p[1] == river || o[0] == river || o[1] == river))
      return true;
    return false;
  };

  // closed sentinel where opponent is hidden: river showdown averaged, chance
  if (hcode < 0) {
    if (street == 2) {
      double num = 0, den = 0;
      for (int q = 0; q < nOpp; ++q) {
        if (blocked(q, turn, river))
          continue;
        double w = ow(q);
        num += w * aggFixed(resp, own, q, turn, river, street, hcode, commits, best);
        den += w;
      }
      return den ? num / den : 0;
    }
    if (aggSampleMode_) {
      int cd = street == 0 ? evalTurn_ : evalRiver_;
      if (cd < 0)
        return 0;
      auto pp = ownCombo();
      if (pp[0] == cd || pp[1] == cd)
        return 0;
      int nt = turn, nr = river;
      if (street == 0)
        nt = cd;
      else
        nr = cd;
      return aggCore(resp, own, nt, nr, street + 1, 0, commits, best);
    }
    bool used[52] = {};
    auto pp = ownCombo();
    used[pp[0]] = used[pp[1]] = true;
    for (int c : flop_)
      used[c] = true;
    if (turn >= 0)
      used[turn] = true;
    int cards = 0;
    for (int cd = 0; cd < 52; ++cd)
      if (!used[cd])
        ++cards;
    double acc = 0;
    for (int cd = 0; cd < 52; ++cd) {
      if (used[cd])
        continue;
      int nt = turn, nr = river;
      if (street == 0)
        nt = cd;
      else
        nr = cd;
      acc += (1.0 / cards) * aggCore(resp, own, nt, nr, street + 1, 0, commits, best);
    }
    return acc;
  }

  int pl = owner(street, hcode);
  if (pl != resp) {
    // opponent is hidden here: average over opponent combos, then dispatch each
    // bound combo for the action continuation (correct E[max] aggregation).
    double total = 0;
    for (int q = 0; q < nOpp; ++q)
      if (!blocked(q, turn, river))
        total += ow(q);
    double v = 0;
    for (int q = 0; q < nOpp; ++q) {
      if (blocked(q, turn, river))
        continue;
      v += ow(q) * aggFixed(resp, own, q, turn, river, street, hcode, commits, best);
    }
    return total > 0 ? v / total : 0;
  }
  // responder's own node: continuation re-enters hidden averaging (aggCore),
  // a fold terminates locally.
  auto ownPol = [&](int act) {
    int g = resp == 0 ? ipC_[own] : oopC_[own];
    return polAvg(resp, g, street, hcode, bucketOf(g, resp, turn, river), act);
  };
  auto foldPay = [&](int cc) {
    double pay = a + cc * B;
    bool ipFolds = (hcode == 3);
    return resp == 0 ? (ipFolds ? -pay : pay) : (ipFolds ? pay : -pay);
  };
  auto child = [&](int nh, int nc) {
    if (nh == 99)
      return foldPay(nc);
    return aggCore(resp, own, turn, river, street, nh, nc, best);
  };
  if (best) {
    double out = -1e30;
    for (int act = 0; act < 2; ++act) {
      int nh, nc = commits;
      if (hcode == 0)
        nh = act ? 2 : 1;
      else if (hcode == 1)
        nh = act ? 3 : -1;
      else if (act == 0)
        nh = 99;
      else {
        nh = (hcode == 2 ? -2 : -3);
        nc = commits + 1;
      }
      out = std::max(out, child(nh, nc));
    }
    return out;
  }
  double ev = 0;
  for (int act = 0; act < 2; ++act) {
    int nh, nc = commits;
    if (hcode == 0)
      nh = act ? 2 : 1;
    else if (hcode == 1)
      nh = act ? 3 : -1;
    else if (act == 0)
      nh = 99;
    else {
      nh = (hcode == 2 ? -2 : -3);
      nc = commits + 1;
    }
    ev += ownPol(act) * child(nh, nc);
  }
  return ev;
}

double MultiStreetSolver::brAgg(int r, int o, int t, int rv, int st, int h, int cc) const {
  return aggCore(r, o, t, rv, st, h, cc, true);
}
double MultiStreetSolver::eqAgg(int r, int o, int t, int rv, int st, int h, int cc) const {
  return aggCore(r, o, t, rv, st, h, cc, false);
}

double MultiStreetSolver::BrValue(int resp, int own, int runs, uint64_t seed) const {
  bs::XorShift64 rng(seed ? seed : 1);
  aggSampleMode_ = true;
  double sum = 0;
  int got = 0;
  for (int k = 0; k < runs; ++k) {
    bool used[52] = {};
    for (int c : flop_)
      used[c] = true;
    evalTurn_ = opt_.fixed_turn >= 0 ? opt_.fixed_turn : drawCard(used, rng);
    if (evalTurn_ >= 0)
      used[evalTurn_] = true;
    evalRiver_ = opt_.fixed_river >= 0 ? opt_.fixed_river : drawCard(used, rng);
    auto pp = resp == 0 ? comboTable()[ipC_[own]] : comboTable()[oopC_[own]];
    if (pp[0] == evalTurn_ || pp[1] == evalTurn_ || pp[0] == evalRiver_ || pp[1] == evalRiver_)
      continue;
    sum += aggCore(resp, own, -1, -1, 0, 0, 0, true);
    ++got;
  }
  aggSampleMode_ = false;
  return got ? sum / got : 0;
}
double MultiStreetSolver::EqValue(int resp, int own, int runs, uint64_t seed) const {
  bs::XorShift64 rng(seed ? seed : 1 + 777);
  aggSampleMode_ = true;
  double sum = 0;
  int got = 0;
  for (int k = 0; k < runs; ++k) {
    bool used[52] = {};
    for (int c : flop_)
      used[c] = true;
    evalTurn_ = opt_.fixed_turn >= 0 ? opt_.fixed_turn : drawCard(used, rng);
    if (evalTurn_ >= 0)
      used[evalTurn_] = true;
    evalRiver_ = opt_.fixed_river >= 0 ? opt_.fixed_river : drawCard(used, rng);
    auto pp = resp == 0 ? comboTable()[ipC_[own]] : comboTable()[oopC_[own]];
    if (pp[0] == evalTurn_ || pp[1] == evalTurn_ || pp[0] == evalRiver_ || pp[1] == evalRiver_)
      continue;
    sum += aggCore(resp, own, -1, -1, 0, 0, 0, false);
    ++got;
  }
  aggSampleMode_ = false;
  return got ? sum / got : 0;
}

double MultiStreetSolver::exploitability() {
  constexpr int kRuns = 400;
  double brI = 0, eqI = 0, brO = 0, eqO = 0;
  for (int i = 0; i < Ni_; ++i) {
    brI += ipW_[i] / Wi_ * BrValue(0, i, kRuns, opt_.seed + i);
    eqI += ipW_[i] / Wi_ * EqValue(0, i, kRuns, opt_.seed + i);
  }
  for (int j = 0; j < No_; ++j) {
    brO += oopW_[j] / Wo_ * BrValue(1, j, kRuns, opt_.seed + j);
    eqO += oopW_[j] / Wo_ * EqValue(1, j, kRuns, opt_.seed + j);
  }
  double gi = std::max(0.0, brI - eqI) / pot_, go = std::max(0.0, brO - eqO) / pot_;
  valueToIp_ = eqI;
  gapI_ = gi;
  gapO_ = go;
  return gi + go;
}

MultiStreetResult MultiStreetSolver::Solve() {
  MultiStreetResult out;
  if (Ni_ == 0 || No_ == 0)
    return out;
  for (int t = 1; t <= opt_.iterations; ++t) {
    iterate();
    discount(t);
  }
  double ex = exploitability();
  out.ok = true;
  out.exploitability_pot = ex;
  out.value_to_ip = valueToIp_;
  out.iterations_done = opt_.iterations;
  return out;
}

}  // namespace bs::gto
