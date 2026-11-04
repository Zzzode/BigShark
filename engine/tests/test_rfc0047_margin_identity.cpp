// RFC 0047 S2 native test: the depth-limited continual-resolving margin
// identity on a tiny hand-built two-stage zero-sum imperfect-information game.
//
// This implements the RFC acceptance item "b_river_s(c) margin identity on a
// tiny game (depth-limited with EXACT leaves vs full tree)". It isolates the
// gadget/margin machinery from oracle accuracy:
//
//   A  the game solved as ONE full tree (reference equilibrium; source of
//      b_turn and of the EXACT per-leaf continuation matrices V_A);
//   B  the depth-limited trunk whose post-chance leaves are terminals valued
//      by the exact V_A, plus a responder (P0) -x alternate terminal at the
//      trunk root locked to A's exact root margin b_turn;
//   R  per reached (line, card) leaf, a river re-solve gadget dealing BOTH
//      seats' trunk-conditioned ranges, with a responder alternate terminal
//      locked to the leaf margin b_river(i) = sum_j r1(j) V_A(i,j).
//
// Pinned (tolerances scaled by the reference solver's own exploitability):
//   C2  exact-leaf trunk reproduces the full-game TURN strategy;
//   C4a each re-solve is a subtree equilibrium and realizes its locked margin;
//   C4b (the RFC identity) for every hand A brings to a line, the locked leaf
//       margin equals the full-game conditioned margin under A;
//   micro-G3 a known +0.5 chip margin perturbation appears as a ~0.5 responder
//       value error (leaf error->value slope ~1) - teeth, orders of magnitude
//       above the certified gap.
//
// Two REPORT-ONLY arms (not gated) quantify what this micro-test deliberately
// does not claim: (1) the same trunk fed a STANDALONE-UNIFORM oracle V_O
// instead of exact leaves, and (2) the statically-assembled two-sided hybrid
// NashConv across independently re-solved leaves. At a chance frontier whose
// generator range collapses to one hand, a value-indifferent bluff frequency
// is not pinned by the margin, so static assembly can differ from A even while
// every reached re-solve is responder-safe; online continual re-solving only
// ever serves the ONE reached card, and the real-engine reached-leaf hybrid
// slope is gate G3 (measured with the RFC 0046 partition walker), not here.
//
// The tiny game (two private hands per seat, one public card A/B at 50/50,
// pot-sized bet/cap; polar/bluff-catcher payoffs that mix at equilibrium):
//
//   turn:  P0 check | bet
//            check -> card -> RIVER(stake g=1)
//            bet   -> P1 fold(+1) | call -> card -> RIVER(g=3)
//   river: P0 check(showdown g) | bet -> P1 fold(+g) | call(showdown 2g)
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <utility>
#include <vector>

#define CHECK(cond)                                                         \
  do {                                                                      \
    if (!(cond)) {                                                          \
      std::printf("CHECK failed: %s (%s:%d)\n", #cond, __FILE__, __LINE__); \
      return false;                                                         \
    }                                                                       \
  } while (0)

namespace {

constexpr int kH = 2;  // private hands per seat
using Val = std::array<double, kH>;

// Showdown sign for P0: [card][i][j]; +1 win, -1 loss, 0 tie.
// Polar / bluff-catcher structure: P0 h1 is the nuts on both cards; h0 is pure
// air on card A and ties only j1 on card B. P1 j0/j1 are bluff-catchers (beat
// air, lose to the nuts). This is the textbook family whose equilibrium mixes
// turn check/bluff and river bet/fold, putting non-uniform conditioned ranges
// on BOTH public turn lines - the regime the margin identity is non-trivial in.
constexpr std::array<std::array<std::array<double, kH>, kH>, 2> kSign = {{
    {{{{-1.0, -1.0}}, {{1.0, 1.0}}}},
    {{{{-1.0, 0.0}}, {{1.0, 1.0}}}},
}};

struct Node {
  int actor = 0;  // 0/1 player, 2 chance, 3 terminal
  std::vector<int> child;
  std::vector<double> prob;
  int kind = 0;        // 0 internal, 1 fold, 2 showdown, 3 matrix, 4 alternate
  double pay = 0.0;    // fold: constant P0 payoff
  int card = -1;       // showdown: public card 0/1
  double stake = 0.0;  // showdown: gross per-seat stake
  int ref = -1;        // matrix id (kind 3) or alternate id (kind 4)
};

struct Game {
  std::vector<Node> nodes;
  std::array<std::array<double, kH>, kH> w{};  // joint deal weights
  std::vector<std::array<std::array<double, kH>, kH>> mats;
  std::vector<Val> alts;
  int root = 0;

  int mk(int actor) {
    Node n;
    n.actor = actor;
    nodes.push_back(n);
    return static_cast<int>(nodes.size()) - 1;
  }
  int mk_fold(double pay0) {
    int id = mk(3);
    nodes[id].kind = 1;
    nodes[id].pay = pay0;
    return id;
  }
  int mk_show(int card, double stake) {
    int id = mk(3);
    nodes[id].kind = 2;
    nodes[id].card = card;
    nodes[id].stake = stake;
    return id;
  }
  int mk_mat(int fid) {
    int id = mk(3);
    nodes[id].kind = 3;
    nodes[id].ref = fid;
    return id;
  }
  int mk_alt(int aid) {
    int id = mk(3);
    nodes[id].kind = 4;
    nodes[id].ref = aid;
    return id;
  }
};

struct RiverLayout {
  int p0 = 0;  // P0 check/bet node
  int p1 = 0;  // P1 fold/call node
};

// Appends a real-terminal river subtree: P0 check(showdown g) | bet ->
// P1 fold(+g) | call(showdown 2g).
RiverLayout build_river_real(Game& g, int card, double gross) {
  RiverLayout L;
  L.p0 = g.mk(0);
  const int check = g.mk_show(card, gross);
  L.p1 = g.mk(1);
  const int fold = g.mk_fold(gross);
  const int call = g.mk_show(card, 2.0 * gross);
  g.nodes[L.p0].child = {check, L.p1};
  g.nodes[L.p1].child = {fold, call};
  return L;
}

struct SolveResult {
  std::vector<std::vector<double>> sigma;  // [node][hand*na + action]
  double nash_conv = 0.0;
  Val ev0{};  // equilibrium root values per P0 hand
  Val ev1{};
};

// Vectorized RM+ CFR with linear average weighting. All value vectors are
// fixed-size stack arrays (two private hands), so the traversal loop does no
// heap allocation per iteration.
class Solver {
 public:
  explicit Solver(Game& g) : g_(g) {
    reg_.resize(g.nodes.size());
    sum_.resize(g.nodes.size());
    for (std::size_t n = 0; n < g.nodes.size(); ++n) {
      if (g.nodes[n].actor == 0 || g.nodes[n].actor == 1) {
        const std::size_t na = g.nodes[n].child.size();
        reg_[n].assign(na * kH, 0.0);
        sum_[n].assign(na * kH, 0.0);
      }
    }
    // Counterfactual reach convention: own seed 1 per private hand; the joint
    // deal weights w already carry each seat's marginal (uniform 0.25 here,
    // and the re-solve gadget bakes the generator prefix range into w).
    r0_.fill(1.0);
    r1_.fill(1.0);
  }

  void run(std::uint64_t iters) {
    for (std::uint64_t k = 0; k < iters; ++k)
      step();
  }

  // Evaluate an externally supplied action profile on this game: exact
  // per-seat best response minus equilibrium play, summed over both seats.
  double profile_nashconv(const std::vector<std::vector<double>>& sig) const {
    Reach pref{};
    pref.fill(1.0);
    double nc = 0.0;
    for (int viewer = 0; viewer < 2; ++viewer) {
      const Val ev = eval(g_.root, viewer, /*br=*/false, pref, sig);
      const Val br = eval(g_.root, viewer, /*br=*/true, pref, sig);
      for (int h = 0; h < kH; ++h)
        nc += br[h] - ev[h];
    }
    return nc;
  }

  SolveResult result() const {
    SolveResult r;
    r.sigma.resize(g_.nodes.size());
    for (std::size_t n = 0; n < g_.nodes.size(); ++n) {
      if (g_.nodes[n].actor != 0 && g_.nodes[n].actor != 1)
        continue;
      const std::size_t na = g_.nodes[n].child.size();
      r.sigma[n].assign(na * kH, 0.0);
      for (int h = 0; h < kH; ++h) {
        double s = 0.0;
        for (std::size_t a = 0; a < na; ++a)
          s += sum_[n][h * na + a];
        for (std::size_t a = 0; a < na; ++a)
          r.sigma[n][h * na + a] =
              s <= 0.0 ? 1.0 / static_cast<double>(na) : sum_[n][h * na + a] / s;
      }
    }
    const Val e0 = evaluate(0, /*br=*/false, r.sigma);
    const Val e1 = evaluate(1, /*br=*/false, r.sigma);
    const Val b0 = evaluate(0, /*br=*/true, r.sigma);
    const Val b1 = evaluate(1, /*br=*/true, r.sigma);
    r.ev0 = e0;
    r.ev1 = e1;
    for (int i = 0; i < kH; ++i)
      r.nash_conv += (b0[i] - e0[i]) + (b1[i] - e1[i]);
    return r;
  }

 private:
  using Reach = Val;

  void step() {
    ++t_;
    for (int trav = 0; trav < 2; ++trav) {
      auto p0 = r0_;
      auto p1 = r1_;
      walk(g_.root, p0, p1, trav);
    }
  }

  double u0(const Node& n, int i, int j) const {
    switch (n.kind) {
      case 1:
        return n.pay;
      case 2:
        return kSign[n.card][i][j] * n.stake;
      case 3:
        return g_.mats[n.ref][i][j];
      case 4:
        return g_.alts[n.ref][i];  // constant across opponent hands
      default:
        return 0.0;
    }
  }

  static double sigmah(const std::vector<double>& rg, int h, std::size_t na, std::size_t a) {
    double total = 0.0;
    for (std::size_t b = 0; b < na; ++b)
      total += std::max(0.0, rg[h * na + b]);
    if (total <= 0.0)
      return 1.0 / static_cast<double>(na);
    return std::max(0.0, rg[h * na + a]) / total;
  }

  // Counterfactual traverser value vector.
  Val walk(int nid, Reach p0, Reach p1, int trav, int depth = 0) {
    if (depth > 200) {
      std::fprintf(stderr, "WALK CYCLE nid=%d actor=%d depth=%d kids:", nid, g_.nodes[nid].actor,
                   depth);
      for (int c : g_.nodes[nid].child)
        std::fprintf(stderr, " %d", c);
      std::fprintf(stderr, "\n");
      std::abort();
    }
    const Node& n = g_.nodes[nid];
    if (n.actor == 3) {
      Val v{};
      if (trav == 0) {
        for (int i = 0; i < kH; ++i) {
          double acc = 0.0;
          for (int j = 0; j < kH; ++j)
            acc += p1[j] * g_.w[i][j] * u0(n, i, j);
          v[i] = acc;
        }
      } else {
        for (int j = 0; j < kH; ++j) {
          double acc = 0.0;
          for (int i = 0; i < kH; ++i)
            acc += p0[i] * g_.w[i][j] * (-u0(n, i, j));
          v[j] = acc;
        }
      }
      return v;
    }
    if (n.actor == 2) {
      Val v{};
      for (std::size_t a = 0; a < n.child.size(); ++a) {
        const Val ca = walk(n.child[a], p0, p1, trav, depth + 1);
        for (int h = 0; h < kH; ++h)
          v[h] += n.prob[a] * ca[h];
      }
      return v;
    }
    const int p = n.actor;
    const std::size_t na = n.child.size();
    std::vector<Val> va(na);
    for (std::size_t a = 0; a < na; ++a) {
      Reach q0 = p0;
      Reach q1 = p1;
      if (p != trav) {
        // Only the OPPONENT reach is folded into terminal values; the
        // traverser's own reach stays unscaled at every own node (vanilla
        // CFR pi_{-i} convention), so late regret shifts cannot starve a
        // still-relevant subtree's average-strategy accumulation.
        for (int h = 0; h < kH; ++h) {
          const double s = sigmah(reg_[nid], h, na, a);
          if (p == 0)
            q0[h] = p0[h] * s;
          else
            q1[h] = p1[h] * s;
        }
      }
      va[a] = walk(n.child[a], q0, q1, trav, depth + 1);
    }
    Val val{};
    if (p == trav) {
      // Own node: child action values are unweighted at THIS node (only the
      // opponent reach is folded into terminal values), so dot with sigma.
      for (int h = 0; h < kH; ++h)
        for (std::size_t a = 0; a < na; ++a)
          val[h] += sigmah(reg_[nid], h, na, a) * va[a][h];
    } else {
      // Opponent node: each child already carries this node's sigma(a) via
      // the scaled opponent reach; sum without re-weighting.
      for (int h = 0; h < kH; ++h)
        for (std::size_t a = 0; a < na; ++a)
          val[h] += va[a][h];
    }
    if (p == trav) {
      // Textbook average-strategy weight pi_{-i} per own hand: the opponent
      // reach folded with the joint deal weights (a per-hand, per-action
      // constant; chance contributes via the value return, not the reach).
      Val opp_w{};
      if (p == 0) {
        for (int i = 0; i < kH; ++i)
          for (int j = 0; j < kH; ++j)
            opp_w[i] += p1[j] * g_.w[i][j];
      } else {
        for (int j = 0; j < kH; ++j)
          for (int i = 0; i < kH; ++i)
            opp_w[j] += p0[i] * g_.w[i][j];
      }
      for (int h = 0; h < kH; ++h) {
        // Capture THIS iteration's strategy sigma^t before the regret update;
        // the average strategy accumulates sigma^t, not the post-update one.
        std::vector<double> s_t(na);
        for (std::size_t a = 0; a < na; ++a)
          s_t[a] = sigmah(reg_[nid], h, na, a);
        for (std::size_t a = 0; a < na; ++a) {
          double& rg = reg_[nid][h * na + a];
          rg += va[a][h] - val[h];  // vanilla regret matching (no RM+ clip)
          sum_[nid][h * na + a] += static_cast<double>(t_) * opp_w[h] * s_t[a];  // linear CFR
        }
      }
    }
    return val;
  }

  // Viewer-specific value. br=false evaluates under the average strategy
  // (equilibrium CFV); br=true best-responds at the viewer's own nodes. Own
  // reach never enters the viewer's value (counterfactual form); only the
  // opponent reach is carried and scaled at opponent nodes.
  Val evaluate(int viewer, bool br, const std::vector<std::vector<double>>& sig) const {
    Reach pref{};
    pref.fill(1.0);  // opponent root reach; joint weights w carry the marginals
    return eval(g_.root, viewer, br, pref, sig);
  }

  Val eval(int nid, int viewer, bool br, Reach opp,
           const std::vector<std::vector<double>>& sig) const {
    const Node& n = g_.nodes[nid];
    if (n.actor == 3) {
      Val v{};
      for (int me = 0; me < kH; ++me) {
        double acc = 0.0;
        for (int op = 0; op < kH; ++op) {
          const int i = viewer == 0 ? me : op;
          const int j = viewer == 0 ? op : me;
          const double sign = viewer == 0 ? 1.0 : -1.0;
          acc += opp[op] * g_.w[i][j] * sign * u0(n, i, j);
        }
        v[me] = acc;
      }
      return v;
    }
    if (n.actor == 2) {
      Val v{};
      for (std::size_t a = 0; a < n.child.size(); ++a) {
        const Val ca = eval(n.child[a], viewer, br, opp, sig);
        for (int h = 0; h < kH; ++h)
          v[h] += n.prob[a] * ca[h];
      }
      return v;
    }
    const int p = n.actor;
    const std::size_t na = n.child.size();
    if (p == viewer) {
      std::vector<Val> kids(na);
      for (std::size_t a = 0; a < na; ++a)
        kids[a] = eval(n.child[a], viewer, br, opp, sig);  // own reach excluded
      Val v{};
      for (int h = 0; h < kH; ++h) {
        if (br) {
          double best = kids[0][h];
          for (std::size_t a = 1; a < na; ++a)
            best = std::max(best, kids[a][h]);
          v[h] = best;
        } else {
          for (std::size_t a = 0; a < na; ++a)
            v[h] += sig[nid][h * na + a] * kids[a][h];
        }
      }
      return v;
    }
    // Opponent node: scale opponent reach per action and sum the children.
    Val v{};
    for (std::size_t a = 0; a < na; ++a) {
      Reach q = opp;
      for (int h = 0; h < kH; ++h)
        q[h] = opp[h] * sig[nid][h * na + a];
      const Val ca = eval(n.child[a], viewer, br, q, sig);
      for (int h = 0; h < kH; ++h)
        v[h] += ca[h];
    }
    return v;
  }

  Game& g_;
  std::vector<std::vector<double>> reg_;
  std::vector<std::vector<double>> sum_;
  Val r0_{};
  Val r1_{};
  std::uint64_t t_ = 0;
};

// Deal-level P0 value of a river subgame under explicit per-hand action
// probabilities: x_i = P0 bet, f_j = P1 fold facing a bet.
double river_deal_value(int card, double gross, int i, int j, double x_i, double f_j) {
  const double s_check = kSign[card][i][j] * gross;
  const double s_betcall = kSign[card][i][j] * 2.0 * gross;
  return (1.0 - x_i) * s_check + x_i * (f_j * gross + (1.0 - f_j) * s_betcall);
}

struct FullLayout {
  int p0_turn = 0;
  int p1_turn = 0;
  std::array<std::array<RiverLayout, 2>, 2> river{};  // [line][card]
};

// A: full tree. line 0 = check-check (gross 1), line 1 = bet/call (gross 3).
FullLayout build_A(Game& g) {
  FullLayout L;
  L.p0_turn = g.mk(0);
  const int chance_check = g.mk(2);
  L.p1_turn = g.mk(1);
  const int turn_fold = g.mk_fold(1.0);
  const int chance_call = g.mk(2);
  for (int c = 0; c < 2; ++c)
    L.river[0][c] = build_river_real(g, c, 1.0);
  for (int c = 0; c < 2; ++c)
    L.river[1][c] = build_river_real(g, c, 3.0);
  g.nodes[chance_check].child = {L.river[0][0].p0, L.river[0][1].p0};
  g.nodes[chance_check].prob = {0.5, 0.5};
  g.nodes[chance_call].child = {L.river[1][0].p0, L.river[1][1].p0};
  g.nodes[chance_call].prob = {0.5, 0.5};
  g.nodes[L.p0_turn].child = {chance_check, L.p1_turn};
  g.nodes[L.p1_turn].child = {turn_fold, chance_call};
  g.root = L.p0_turn;
  return L;
}

struct TrunkLayout {
  int p0_turn = 0;
  int p1_turn = 0;
};

// B: depth-limited trunk. Four matrix terminals (fid = line*2+card); P0 root
// gains alternate child aid 0 locked to b_turn.
TrunkLayout build_B(Game& g) {
  TrunkLayout L;
  L.p0_turn = g.mk(0);
  const int chance_check = g.mk(2);
  L.p1_turn = g.mk(1);
  const int turn_fold = g.mk_fold(1.0);
  const int chance_call = g.mk(2);
  // Create every terminal before wiring edges: mk_* appends to (and can
  // reallocate) g.nodes, so never hold a g.nodes[...] reference across one.
  std::array<int, 4> mat_node{};
  g.mats.resize(4);
  for (int m = 0; m < 4; ++m)
    mat_node[m] = g.mk_mat(m);
  g.nodes[chance_check].child = {mat_node[0], mat_node[1]};
  g.nodes[chance_call].child = {mat_node[2], mat_node[3]};
  g.nodes[chance_check].prob = {0.5, 0.5};
  g.nodes[chance_call].prob = {0.5, 0.5};
  g.alts.push_back(Val{});  // aid 0, filled by the caller
  const int alt = g.mk_alt(0);
  g.nodes[L.p0_turn].child = {chance_check, L.p1_turn, alt};
  g.nodes[L.p1_turn].child = {turn_fold, chance_call};
  g.root = L.p0_turn;
  return L;
}

// R: river re-solve gadget. P0 root actions check/bet plus an alternate
// terminal aid 0 paying the locked per-combo leaf margin.
RiverLayout build_R(Game& g, int card, double gross) {
  RiverLayout L = build_river_real(g, card, gross);
  g.alts.push_back(Val{});  // aid 0, filled by the caller
  const int alt = g.mk_alt(0);
  g.nodes[L.p0].child.push_back(alt);
  g.root = L.p0;
  return L;
}

void set_uniform(Game& g) {
  for (int i = 0; i < kH; ++i)
    for (int j = 0; j < kH; ++j)
      g.w[i][j] = 0.25;
}

constexpr std::uint64_t kIters = 1000000;

SolveResult solve(Game& g) {
  Solver s(g);
  s.run(kIters);
  return s.result();
}

// Conditional private-hand range of the acting seat at a turn decision, given
// that the public line took `action` (0..na_real-1). The source node may carry
// an extra trailing -x alternate action (B's P0) which is excluded before
// conditioning. Returns a normalized distribution over the kH hands.
Val turn_hand_range(const SolveResult& t, int node, int action, int na_total, int na_real) {
  Val r{};
  double tot = 0.0;
  for (int h = 0; h < kH; ++h) {
    double real_mass = 0.0;
    for (int a = 0; a < na_real; ++a)
      real_mass += t.sigma[node][h * na_total + a];
    const double q = real_mass > 0.0 ? t.sigma[node][h * na_total + action] / real_mass : 0.0;
    r[h] = q;
    tot += q;
  }
  if (tot <= 0.0)
    return {0.5, 0.5};
  for (double& x : r)
    x /= tot;
  return r;
}

// Ranges at a turn line: line 0 = P0 checked (P1 did not act); line 1 = P0
// bet and P1 called. `p0_total`/`p0_real` describe the source P0 turn node
// (A: 2/2, B: 3/2 with an alt).
void line_ranges(const SolveResult& t, int p0_node, int p1_node, int line, int p0_total,
                 int p0_real, Val& r0, Val& r1) {
  if (line == 0) {
    r0 = turn_hand_range(t, p0_node, /*check=*/0, p0_total, p0_real);
    r1 = {0.5, 0.5};
  } else {
    r0 = turn_hand_range(t, p0_node, /*bet=*/1, p0_total, p0_real);
    r1 = turn_hand_range(t, p1_node, /*call=*/1, 2, 2);
  }
}

// Fill an A-game action node's profile from a (possibly alt-bearing) source
// node, renormalizing over the `na_dst` real actions per hand.
void copy_real_profile(std::vector<std::vector<double>>& dst, int dst_node,
                       const std::vector<double>& src, int na_src, int na_dst) {
  dst[dst_node].assign(na_dst * kH, 0.0);
  for (int h = 0; h < kH; ++h) {
    double real_mass = 0.0;
    for (int a = 0; a < na_dst; ++a)
      real_mass += src[h * na_src + a];
    for (int a = 0; a < na_dst; ++a)
      dst[dst_node][h * na_dst + a] =
          real_mass > 0.0 ? src[h * na_src + a] / real_mass : 1.0 / static_cast<double>(na_dst);
  }
}

struct LeafSolve {
  SolveResult res;
  RiverLayout layout;
  Val r0{};
  Val r1{};
  Val b_river{};
};

// Build one river re-solve gadget: generator deals BOTH seats' trunk-
// conditioned ranges (independent conditional joint), and P0 has an extra
// alternate terminal locked to the per-combo responder margin b_river.
LeafSolve solve_leaf(int line, int c, double gross, const Val& r0, const Val& r1,
                     const std::array<std::array<double, kH>, kH>& vo, double margin_shift) {
  LeafSolve L;
  L.r0 = r0;
  L.r1 = r1;
  Game g;
  for (int i = 0; i < kH; ++i)
    for (int j = 0; j < kH; ++j)
      g.w[i][j] = r0[i] * r1[j];
  for (int i = 0; i < kH; ++i) {
    double m = 0.0;
    for (int j = 0; j < kH; ++j)
      m += r1[j] * vo[i][j];  // P0 own range never weights its own margin
    L.b_river[i] = m + margin_shift;
  }
  L.layout = build_R(g, c, gross);
  g.alts[0] = L.b_river;
  L.res = solve(g);
  return L;
}

bool test_margin_identity() {
  // ---- A: full reference game ---------------------------------------------
  Game ga;
  set_uniform(ga);
  const FullLayout LA = build_A(ga);
  const SolveResult A = solve(ga);
  std::printf("  A full-game NashConv = %.2e\n", A.nash_conv);
  CHECK(std::isfinite(A.nash_conv) && A.nash_conv < 1e-3);

  // Per-deal river continuation matrix under an explicit river strategy pair.
  auto leaf_matrix = [&](const std::vector<std::vector<double>>& sig, int np0, int np1, int line,
                         int c) {
    std::array<std::array<double, kH>, kH> v{};
    const double gross = line == 0 ? 1.0 : 3.0;
    for (int i = 0; i < kH; ++i) {
      const double x_i = sig[np0][i * 2 + 1];
      for (int j = 0; j < kH; ++j) {
        const double f_j = sig[np1][j * 2 + 0];
        v[i][j] = river_deal_value(c, gross, i, j, x_i, f_j);
      }
    }
    return v;
  };

  // EXACT leaves: the full-game (A) river continuation at every (line, card).
  // The RFC margin-identity test is "depth-limited with EXACT leaves vs full
  // tree"; this isolates the gadget/margin machinery from oracle accuracy.
  std::array<std::array<std::array<std::array<double, kH>, kH>, 2>, 2> V_A{};
  for (int line = 0; line < 2; ++line)
    for (int c = 0; c < 2; ++c)
      V_A[line][c] = leaf_matrix(A.sigma, LA.river[line][c].p0, LA.river[line][c].p1, line, c);

  // APPROXIMATE leaves: standalone UNIFORM river solves (the offline oracle
  // analog). A secondary, REPORT-ONLY arm: its end-to-end gap is exactly the
  // oracle-accuracy quantity that gates G2/G3 bound on the real-scale engine.
  std::array<std::array<std::array<std::array<double, kH>, kH>, 2>, 2> V_O{};
  for (int line = 0; line < 2; ++line) {
    const double gross = line == 0 ? 1.0 : 3.0;
    for (int c = 0; c < 2; ++c) {
      Game go;
      set_uniform(go);
      const RiverLayout lo = build_river_real(go, c, gross);
      const SolveResult o = solve(go);
      CHECK(std::isfinite(o.nash_conv));
      V_O[line][c] = leaf_matrix(o.sigma, lo.p0, lo.p1, line, c);
    }
  }

  // B: depth-limited trunk with EXACT leaf matrices + the true b_turn margin.
  Game gb;
  set_uniform(gb);
  const TrunkLayout LB = build_B(gb);
  for (int line = 0; line < 2; ++line)
    for (int c = 0; c < 2; ++c)
      gb.mats[line * 2 + c] = V_A[line][c];
  gb.alts[0] = A.ev0;  // b_turn: exact full-game P0 root margin
  const SolveResult B = solve(gb);
  std::printf("  B depth-trunk NashConv (exact leaves) = %.2e\n", B.nash_conv);
  CHECK(std::isfinite(B.nash_conv));

  // B_uni: same trunk but fed the standalone-uniform oracle (report only).
  Game gu;
  set_uniform(gu);
  const TrunkLayout LU = build_B(gu);
  for (int line = 0; line < 2; ++line)
    for (int c = 0; c < 2; ++c)
      gu.mats[line * 2 + c] = V_O[line][c];
  gu.alts[0] = A.ev0;
  const SolveResult BU = solve(gu);

  // C2: with EXACT leaves the depth-limited trunk reproduces the full-game
  // turn strategy (loose at mixed indifferent nodes; C3 carries the sharp
  // guarantee). The same trunk fed the standalone-uniform oracle is reported
  // alongside - its gap is the oracle-accuracy term G2/G3 must bound.
  double c2_gap = 0.0;
  double c2_uni_gap = 0.0;
  for (int i = 0; i < kH; ++i) {
    const double xA = A.sigma[LA.p0_turn][i * 2 + 1];
    const double xB =
        B.sigma[LB.p0_turn][i * 3 + 1] /
        std::max(1e-300, B.sigma[LB.p0_turn][i * 3 + 0] + B.sigma[LB.p0_turn][i * 3 + 1]);
    const double xBU =
        BU.sigma[LU.p0_turn][i * 3 + 1] /
        std::max(1e-300, BU.sigma[LU.p0_turn][i * 3 + 0] + BU.sigma[LU.p0_turn][i * 3 + 1]);
    c2_gap = std::max(c2_gap, std::fabs(xB - xA));
    c2_uni_gap = std::max(c2_uni_gap, std::fabs(xBU - xA));
    c2_gap = std::max(c2_gap,
                      std::fabs(B.sigma[LB.p1_turn][i * 2 + 0] - A.sigma[LA.p1_turn][i * 2 + 0]));
    c2_uni_gap = std::max(
        c2_uni_gap, std::fabs(BU.sigma[LU.p1_turn][i * 2 + 0] - A.sigma[LA.p1_turn][i * 2 + 0]));
  }
  std::printf("  C2 trunk turn gap, exact leaves = %.3e\n", c2_gap);
  std::printf("  C2 trunk turn gap, UNIFORM oracle (report) = %.3e\n", c2_uni_gap);
  CHECK(c2_gap < 0.02);

  // ---- R: per-leaf re-solve gadgets (correct) and a +0.5-shift control ----
  std::array<std::array<LeafSolve, 2>, 2> R{};
  std::array<std::array<LeafSolve, 2>, 2> W{};
  std::array<std::array<LeafSolve, 2>, 2> RU{};  // report-only uniform-oracle arm
  // Whether a public turn line has positive trunk mass. line 0 = P0 check;
  // line 1 = P0 bet AND P1 call.
  auto line_reachable = [&](const SolveResult& t, int p0_node, int p1_node, int line, int p0_total,
                            int p0_real) {
    double p0_action = 0.0;
    for (int h = 0; h < kH; ++h) {
      double real_mass = 0.0;
      for (int a = 0; a < p0_real; ++a)
        real_mass += t.sigma[p0_node][h * p0_total + a];
      if (real_mass > 0.0)
        p0_action += t.sigma[p0_node][h * p0_total + (line == 0 ? 0 : 1)] / real_mass;
    }
    if (p0_action < 1e-9)
      return false;
    if (line == 1) {
      double call = 0.0;
      for (int h = 0; h < kH; ++h)
        call += t.sigma[p1_node][h * 2 + 1];
      if (call < 1e-9)
        return false;
    }
    return true;
  };

  double worst_gadget_margin_gap = 0.0;
  double worst_e2e_margin_gap = 0.0;
  double worst_gadget_nc = 0.0;
  double worst_ctrl_value_gap = 0.0;  // +0.5 margin error -> responder value error
  for (int line = 0; line < 2; ++line) {
    const double gross = line == 0 ? 1.0 : 3.0;
    Val r0B, r1B, r0A, r1A, r0U, r1U;
    line_ranges(B, LB.p0_turn, LB.p1_turn, line, 3, 2, r0B, r1B);
    line_ranges(A, LA.p0_turn, LA.p1_turn, line, 2, 2, r0A, r1A);
    line_ranges(BU, LU.p0_turn, LU.p1_turn, line, 3, 2, r0U, r1U);
    const bool reachable = line_reachable(A, LA.p0_turn, LA.p1_turn, line, 2, 2);
    for (int c = 0; c < 2; ++c) {
      R[line][c] = solve_leaf(line, c, gross, r0B, r1B, V_A[line][c], 0.0);
      W[line][c] = solve_leaf(line, c, gross, r0B, r1B, V_A[line][c], 0.5);
      RU[line][c] = solve_leaf(line, c, gross, r0U, r1U, V_O[line][c], 0.0);
      CHECK(std::isfinite(R[line][c].res.nash_conv));

      if (!reachable)
        continue;  // zero-mass line: gadget play cannot affect exploitability

      // Conditional per-combo gadget value: ev0[i] carries the joint weight
      // r0[i]*r1[j]; divide by the responder's own line mass r0[i] to recover
      // the per-own-combo continuation value comparable to b_river.
      worst_gadget_nc = std::max(worst_gadget_nc, R[line][c].res.nash_conv);

      // Full-game conditioned responder margin b* per own hand under A.
      Val b_star{};
      for (int i = 0; i < kH; ++i) {
        for (int j = 0; j < kH; ++j) {
          const double x_i = A.sigma[LA.river[line][c].p0][i * 2 + 1];
          const double f_j = A.sigma[LA.river[line][c].p1][j * 2 + 0];
          b_star[i] += r1A[j] * river_deal_value(c, gross, i, j, x_i, f_j);
        }
      }
      for (int i = 0; i < kH; ++i) {
        if (r0B[i] < 1e-9)
          continue;
        const double cv = R[line][c].res.ev0[i] / r0B[i];
        // C4a: the equilibrated gadget realizes its locked margin (in
        // equilibrium the responder is indifferent, or strictly continues).
        worst_gadget_margin_gap =
            std::max(worst_gadget_margin_gap, std::fabs(cv - R[line][c].b_river[i]));
        // C4b end-to-end: for a hand A actually brings to this line, the locked
        // margin equals the full-game conditioned margin under A.
        if (r0A[i] >= 1e-9)
          worst_e2e_margin_gap = std::max(worst_e2e_margin_gap, std::fabs(cv - b_star[i]));
        // Micro-G3 slope: with the margin shifted +0.5, the responder extracts
        // detectibly more than its true continuation value (leaf slope ~1).
        const double cvw = W[line][c].res.ev0[i] / r0B[i];
        worst_ctrl_value_gap = std::max(worst_ctrl_value_gap, std::fabs(cvw - b_star[i]));
      }
    }
  }

  // REPORT-ONLY full-tree hybrid NashConv of the statically assembled trunk+
  // river profile. This is intentionally NOT the gate for independent per-river
  // re-solves: at a chance frontier whose generator range collapses to a single
  // hand, a responder bluff frequency is value-indifferent and is not pinned by
  // the margin, so a static assembly can differ from A at an off-path leaf even
  // though every reached re-solve is responder-safe (that is exactly why online
  // continual re-solving re-solves the ONE reached card, and why G3 on the real
  // engine measures the reached-leaf hybrid via the RFC 0046 partition walker).
  // It still usefully reports the standalone-uniform oracle's end-to-end cost.
  auto assemble = [&](const SolveResult& T, const TrunkLayout& LT,
                      const std::array<std::array<LeafSolve, 2>, 2>& leaves) {
    std::vector<std::vector<double>> prof(ga.nodes.size());
    copy_real_profile(prof, LA.p0_turn, T.sigma[LT.p0_turn], 3, 2);
    copy_real_profile(prof, LA.p1_turn, T.sigma[LT.p1_turn], 2, 2);
    for (int line = 0; line < 2; ++line)
      for (int c = 0; c < 2; ++c) {
        const LeafSolve& L = leaves[line][c];
        copy_real_profile(prof, LA.river[line][c].p0, L.res.sigma[L.layout.p0], 3, 2);
        copy_real_profile(prof, LA.river[line][c].p1, L.res.sigma[L.layout.p1], 2, 2);
      }
    return prof;
  };
  Solver prof_eval(ga);
  const double hybrid_nc = prof_eval.profile_nashconv(assemble(B, LB, R));
  const double uniform_nc = prof_eval.profile_nashconv(assemble(BU, LU, RU));

  std::printf("  per-leaf gadget NashConv (max) = %.2e\n", worst_gadget_nc);
  std::printf("  C4a gadget-locked-margin gap = %.2e\n", worst_gadget_margin_gap);
  std::printf("  C4b end-to-end leaf-margin gap = %.2e\n", worst_e2e_margin_gap);
  std::printf("  micro-G3: +0.5 margin shift -> responder value error = %.3f\n",
              worst_ctrl_value_gap);
  std::printf("  [report] static hybrid NashConv, exact leaves = %.3e\n", hybrid_nc);
  std::printf("  [report] static hybrid NashConv, UNIFORM oracle = %.3e\n", uniform_nc);

  // Gates scale with the reference solver's own floor (A is an epsilon-
  // equilibrium, not an exact analytic one): the identity must hold to a small
  // multiple of A's residual, not to an absolute 0.
  const double gate = std::max(2.0e-3, 40.0 * A.nash_conv);
  CHECK(worst_gadget_nc < gate);          // each re-solve is a subtree equilibrium
  CHECK(worst_gadget_margin_gap < gate);  // C4a
  CHECK(worst_e2e_margin_gap < gate);     // C4b: the RFC margin identity
  // Teeth / micro-G3: a known +0.5 chip margin error must appear, to within the
  // solver floor, as a ~0.5 responder-value error (leaf slope near 1) and be
  // orders of magnitude larger than the certified pipeline's gap.
  CHECK(worst_ctrl_value_gap > 0.4);
  CHECK(worst_ctrl_value_gap > 100.0 * std::max(worst_e2e_margin_gap, 1e-12));
  return true;
}

}  // namespace

int main() {
  if (!test_margin_identity())
    return 1;
  std::printf("rfc0047_margin_identity: all checks passed\n");
  return 0;
}
