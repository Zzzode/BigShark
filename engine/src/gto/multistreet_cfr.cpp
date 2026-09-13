#include "multistreet_cfr.h"

#include <algorithm>
#include <array>
#include <bs/equity.hpp>
#include <bs/eval.hpp>
#include <bs/range.hpp>
#include <cstddef>
#include <cstdint>
#include <utility>
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
}

bool MultiStreetSolver::share(int i, int j) const {
  auto [a, b] = comboTable()[ipC_[i]];
  auto [d, e] = comboTable()[oopC_[j]];
  return a == d || a == e || b == d || b == e;
}
uint64_t MultiStreetSolver::key(int combo, int street, int hcode, int bucket, int history) const {
  uint64_t k = (uint64_t)combo;
  k = (k << 2) | (uint64_t)street;
  k = (k << 3) | (uint64_t)(hcode & 7);
  k = (k << 8) | (uint64_t)(bucket & 0xff);
  k = (k << 4) | (uint64_t)(history & 0xf);
  return k;
}
MultiStreetSolver::ISet& MultiStreetSolver::iset(int player, int combo, int street, int hcode,
                                                 int bucket, int history) {
  ISet& s = N_[player][key(combo, street, hcode, bucket, history)];
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
                              double p0, double p1, int tr, int history) {
  const double a = pot_ / 2.0, B = bfrac_ * pot_;
  auto sd = [&](int cc) { return msShowdown(i, j, flop_, turn, river, a, B, cc, ipC_, oopC_); };

  // ---- street-closed sentinel ----
  if (hcode < 0) {
    if (street == 2) {
      double u = sd(commits);
      return tr == 0 ? u : -u;
    }
    const int nextHistory = history * 3 + (-hcode - 1);
    int cd = street == 0 ? sampledTurn_ : sampledRiver_;
    int nt = turn, nr = river;
    if (street == 0)
      nt = cd;
    else
      nr = cd;
    return cfr(i, j, nt, nr, street + 1, 0, commits, p0, p1, tr, nextHistory);
  }

  const int pl = owner(street, hcode);
  const int combo = pl == 0 ? ipC_[i] : oopC_[j];
  const int buck = bucketOf(combo, pl, turn, river);
  ISet& s = iset(pl, combo, street, hcode, buck, history);
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
      v = cfr(i, j, turn, river, street, nh, nc, np0, np1, tr, history);
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
  if (opt_.fixed_turn < 0 && opt_.fixed_river >= 0)
    used[opt_.fixed_river] = true;
  sampledTurn_ = opt_.fixed_turn >= 0 ? opt_.fixed_turn : drawCard(used, rng_);
  if (sampledTurn_ >= 0)
    used[sampledTurn_] = true;
  sampledRiver_ = opt_.fixed_river >= 0 ? opt_.fixed_river : drawCard(used, rng_);
  bool blocked[52] = {};
  if (sampledTurn_ >= 0)
    blocked[sampledTurn_] = true;
  if (sampledRiver_ >= 0)
    blocked[sampledRiver_] = true;
  bool fixedBlocked[52] = {};
  if (opt_.fixed_turn >= 0)
    fixedBlocked[opt_.fixed_turn] = true;
  if (opt_.fixed_river >= 0)
    fixedBlocked[opt_.fixed_river] = true;
  double rangeMass = 0;
  for (int i = 0; i < Ni_; ++i)
    for (int j = 0; j < No_; ++j) {
      auto pp = comboTable()[ipC_[i]];
      auto qq = comboTable()[oopC_[j]];
      if (share(i, j) || fixedBlocked[pp[0]] || fixedBlocked[pp[1]] || fixedBlocked[qq[0]] ||
          fixedBlocked[qq[1]])
        continue;
      rangeMass += ipW_[i] * oopW_[j];
    }
  if (rangeMass <= 0)
    return;
  for (int i = 0; i < Ni_; ++i)
    for (int j = 0; j < No_; ++j) {
      auto pp = comboTable()[ipC_[i]];
      auto qq = comboTable()[oopC_[j]];
      if (share(i, j) || blocked[pp[0]] || blocked[pp[1]] || blocked[qq[0]] || blocked[qq[1]])
        continue;
      const double q = ipW_[i] * oopW_[j] / rangeMass;
      for (int tr = 0; tr < 2; ++tr)
        cfr(i, j, -1, -1, 0, 0, 0, q, q, tr, 0);
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

// ---------------- information-set-correct best response ----------------
double MultiStreetSolver::PolicyAt(int player, int combo, int street, int turn, int river,
                                   int hcode, int act, int history) const {
  return polAvg(player, combo, street, hcode, bucketOf(combo, player, turn, river), act, history);
}
double MultiStreetSolver::polAvg(int player, int combo, int street, int hcode, int bucket, int act,
                                 int history) const {
  auto it = N_[player].find(key(combo, street, hcode, bucket, history));
  if (it == N_[player].end())
    return 0.5;
  return it->second.avg(act);
}

std::vector<double> MultiStreetSolver::initialOpponentReach(int resp, int own) const {
  const int nOpp = resp == 0 ? No_ : Ni_;
  const auto ownCombo = resp == 0 ? comboTable()[ipC_[own]] : comboTable()[oopC_[own]];
  std::vector<double> reach(nOpp, 0.0);
  if ((opt_.fixed_turn >= 0 &&
       (ownCombo[0] == opt_.fixed_turn || ownCombo[1] == opt_.fixed_turn)) ||
      (opt_.fixed_river >= 0 &&
       (ownCombo[0] == opt_.fixed_river || ownCombo[1] == opt_.fixed_river)))
    return reach;

  double total = 0;
  for (int q = 0; q < nOpp; ++q) {
    const int ipIndex = resp == 0 ? own : q;
    const int oopIndex = resp == 0 ? q : own;
    const auto opponentCombo = resp == 0 ? comboTable()[oopC_[q]] : comboTable()[ipC_[q]];
    if (share(ipIndex, oopIndex) ||
        (opt_.fixed_turn >= 0 &&
         (opponentCombo[0] == opt_.fixed_turn || opponentCombo[1] == opt_.fixed_turn)) ||
        (opt_.fixed_river >= 0 &&
         (opponentCombo[0] == opt_.fixed_river || opponentCombo[1] == opt_.fixed_river)))
      continue;
    reach[q] = resp == 0 ? oopW_[q] : ipW_[q];
    total += reach[q];
  }
  if (total > 0)
    for (double& probability : reach)
      probability /= total;
  return reach;
}

double MultiStreetSolver::responseValue(int resp, int own, const std::vector<double>& opponentReach,
                                        int turn, int river, int street, int hcode, int commits,
                                        int history, bool best) const {
  const double ante = pot_ / 2.0;
  const double bet = bfrac_ * pot_;
  const int nOpp = resp == 0 ? No_ : Ni_;
  const int ownGlobal = resp == 0 ? ipC_[own] : oopC_[own];
  const auto ownCombo = comboTable()[ownGlobal];
  auto opponentGlobal = [&](int q) { return resp == 0 ? oopC_[q] : ipC_[q]; };
  auto opponentCombo = [&](int q) { return comboTable()[opponentGlobal(q)]; };
  auto showdown = [&](int q, int calledStreets) {
    const int ipIndex = resp == 0 ? own : q;
    const int oopIndex = resp == 0 ? q : own;
    const double utility =
        msShowdown(ipIndex, oopIndex, flop_, turn, river, ante, bet, calledStreets, ipC_, oopC_);
    return resp == 0 ? utility : -utility;
  };
  auto foldValue = [&](const std::vector<double>& reach, int currentCommits) {
    const double payoff = ante + currentCommits * bet;
    const bool ipFolds = hcode == 3;
    const double utility = resp == 0 ? (ipFolds ? -payoff : payoff) : (ipFolds ? payoff : -payoff);
    double total = 0;
    for (double probability : reach)
      total += probability;
    return total * utility;
  };

  if (hcode < 0) {
    if (street == 2) {
      double value = 0;
      for (int q = 0; q < nOpp; ++q)
        value += opponentReach[q] * showdown(q, commits);
      return value;
    }

    const int nextHistory = history * 3 + (-hcode - 1);
    const int fixedCard = street == 0 ? opt_.fixed_turn : opt_.fixed_river;
    if (fixedCard >= 0) {
      std::vector<double> childReach = opponentReach;
      for (int q = 0; q < nOpp; ++q) {
        const auto cards = opponentCombo(q);
        if (cards[0] == fixedCard || cards[1] == fixedCard)
          childReach[q] = 0;
      }
      const int nextTurn = street == 0 ? fixedCard : turn;
      const int nextRiver = street == 1 ? fixedCard : river;
      return responseValue(resp, own, childReach, nextTurn, nextRiver, street + 1, 0, commits,
                           nextHistory, best);
    }

    bool publicUsed[52] = {};
    publicUsed[ownCombo[0]] = publicUsed[ownCombo[1]] = true;
    for (int card : flop_)
      publicUsed[card] = true;
    if (turn >= 0)
      publicUsed[turn] = true;
    if (street == 0 && opt_.fixed_river >= 0)
      publicUsed[opt_.fixed_river] = true;

    double value = 0;
    for (int card = 0; card < 52; ++card) {
      if (publicUsed[card])
        continue;
      std::vector<double> childReach(nOpp, 0.0);
      for (int q = 0; q < nOpp; ++q) {
        if (opponentReach[q] <= 0)
          continue;
        const auto cards = opponentCombo(q);
        if (cards[0] == card || cards[1] == card)
          continue;
        int availableCards = 0;
        for (int candidate = 0; candidate < 52; ++candidate) {
          if (!publicUsed[candidate] && cards[0] != candidate && cards[1] != candidate)
            ++availableCards;
        }
        if (availableCards > 0)
          childReach[q] = opponentReach[q] / availableCards;
      }
      const int nextTurn = street == 0 ? card : turn;
      const int nextRiver = street == 1 ? card : river;
      value += responseValue(resp, own, childReach, nextTurn, nextRiver, street + 1, 0, commits,
                             nextHistory, best);
    }
    return value;
  }

  auto transition = [&](int act) {
    int nextNode;
    int nextCommits = commits;
    if (hcode == 0)
      nextNode = act == 0 ? 1 : 2;
    else if (hcode == 1)
      nextNode = act == 0 ? -1 : 3;
    else if (act == 0)
      nextNode = 99;
    else {
      nextNode = hcode == 2 ? -2 : -3;
      nextCommits = commits + 1;
    }
    return std::pair{nextNode, nextCommits};
  };
  auto childValue = [&](int act, const std::vector<double>& reach) {
    auto [nextNode, nextCommits] = transition(act);
    if (nextNode == 99)
      return foldValue(reach, commits);
    return responseValue(resp, own, reach, turn, river, street, nextNode, nextCommits, history,
                         best);
  };

  const int player = owner(street, hcode);
  if (player == resp) {
    std::array<double, 2> actionValues = {
        childValue(0, opponentReach),
        childValue(1, opponentReach),
    };
    if (best)
      return std::max(actionValues[0], actionValues[1]);
    const int bucket = bucketOf(ownGlobal, resp, turn, river);
    const double actionZero = polAvg(resp, ownGlobal, street, hcode, bucket, 0, history);
    return actionZero * actionValues[0] + (1.0 - actionZero) * actionValues[1];
  }

  double value = 0;
  for (int act = 0; act < 2; ++act) {
    std::vector<double> childReach(nOpp, 0.0);
    for (int q = 0; q < nOpp; ++q) {
      if (opponentReach[q] <= 0)
        continue;
      const int combo = opponentGlobal(q);
      const int bucket = bucketOf(combo, player, turn, river);
      childReach[q] = opponentReach[q] * polAvg(player, combo, street, hcode, bucket, act, history);
    }
    value += childValue(act, childReach);
  }
  return value;
}

double MultiStreetSolver::BrValue(int resp, int own) const {
  return responseValue(resp, own, initialOpponentReach(resp, own), -1, -1, 0, 0, 0, 0, true);
}
double MultiStreetSolver::EqValue(int resp, int own) const {
  return responseValue(resp, own, initialOpponentReach(resp, own), -1, -1, 0, 0, 0, 0, false);
}

double MultiStreetSolver::exploitability() {
  double brI = 0, eqI = 0, brO = 0, eqO = 0;
  double dealMass = 0;
  std::vector<double> ipMass(Ni_, 0.0);
  std::vector<double> oopMass(No_, 0.0);
  for (int i = 0; i < Ni_; ++i)
    for (int j = 0; j < No_; ++j) {
      const auto ipCombo = comboTable()[ipC_[i]];
      const auto oopCombo = comboTable()[oopC_[j]];
      if (share(i, j) ||
          (opt_.fixed_turn >= 0 &&
           (ipCombo[0] == opt_.fixed_turn || ipCombo[1] == opt_.fixed_turn ||
            oopCombo[0] == opt_.fixed_turn || oopCombo[1] == opt_.fixed_turn)) ||
          (opt_.fixed_river >= 0 &&
           (ipCombo[0] == opt_.fixed_river || ipCombo[1] == opt_.fixed_river ||
            oopCombo[0] == opt_.fixed_river || oopCombo[1] == opt_.fixed_river)))
        continue;
      const double mass = ipW_[i] * oopW_[j];
      dealMass += mass;
      ipMass[i] += mass;
      oopMass[j] += mass;
    }
  if (dealMass <= 0)
    return 1.0;

  for (int i = 0; i < Ni_; ++i) {
    brI += ipMass[i] / dealMass * BrValue(0, i);
    eqI += ipMass[i] / dealMass * EqValue(0, i);
  }
  for (int j = 0; j < No_; ++j) {
    brO += oopMass[j] / dealMass * BrValue(1, j);
    eqO += oopMass[j] / dealMass * EqValue(1, j);
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
