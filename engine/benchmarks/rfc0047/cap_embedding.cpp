#include "cap_embedding.hpp"

#include <algorithm>
#include <array>
#include <bs/eval.hpp>
#include <cmath>
#include <memory>
#include <stdexcept>

#include "frontier_enumerator.hpp"
#include "river_solver.hpp"

namespace bs::rfc0047 {

namespace {
constexpr std::uint32_t kC = 1081;
constexpr std::uint32_t kCompat = 990;
constexpr double kNorm = static_cast<double>(kC) * static_cast<double>(kCompat);
constexpr double kDistEps = 1e-12;
constexpr std::size_t kNoMap = static_cast<std::size_t>(-1);

using bs::tree::AbstractTree;
using bs::tree::NodeKind;
using bs::tree::TreeNode;

// Paired DFS: walk the cap-0 and cap-1 trees along their shared ordered menu
// prefix. cap-0-only suffix subtrees (extra raises) are left unmapped.
void pair_dfs(const AbstractTree& t0, const AbstractTree& t1, std::size_t n0, std::size_t n1,
              std::vector<std::size_t>& map) {
  map[n0] = n1;
  const TreeNode& a = t0.node(n0);
  const TreeNode& b = t1.node(n1);
  if (a.kind != b.kind)
    throw std::invalid_argument("cap_embedding: paired nodes diverge in kind");
  if (!a.is_action() || !b.is_action())
    return;  // terminals/chance pair only at the node itself; no action children
  if (b.actions.size() > a.actions.size())
    throw std::invalid_argument("cap_embedding: cap-1 menu cannot exceed cap-0 menu");
  for (std::size_t i = 0; i < b.actions.size(); ++i) {
    if (!(a.actions[i] == b.actions[i]))
      throw std::invalid_argument("cap_embedding: shared action-prefix mismatch");
  }
  for (std::size_t i = b.actions.size(); i < a.actions.size(); ++i) {
    const auto type = a.actions[i].type;
    if (type != bs::poker::ActionType::Bet && type != bs::poker::ActionType::Raise)
      throw std::invalid_argument("cap_embedding: cap-0-only suffix action must be aggressive");
    map[a.children[i]] = kNoMap;  // cap-0-only edge subtree
  }
  for (std::size_t i = 0; i < b.actions.size(); ++i)
    pair_dfs(t0, t1, a.children[i], b.children[i], map);
}

// Lightweight exact fixed-policy evaluator over the cap-0 tree. It never runs
// CFR and never allocates per-node regret/sum tables: only board showdown data,
// per-terminal fold/stake, the node map, and depth-scoped combo scratch. The
// walk mirrors the river kernel's eval() conventions one-for-one (root reach
// 1.0 for every combo, opponent-reach snapshot scaling, fold summing all 1,081
// reaches, blocker pairs zeroed only through the showdown matrix).
class EmbeddedGame {
 public:
  EmbeddedGame(const bs::poker::GameDef& def, const AbstractTree& t0, const RiverSolveResult& cap1,
               const AbstractTree& t1, const std::vector<std::size_t>& map,
               CapExtensionCompletion completion)
      : def_(def), tree0_(t0), cap1_(cap1), t1_(t1), map_(map), completion_(completion) {
    std::array<int, 5> bd{};
    for (int i = 0; i < 5; ++i)
      bd[i] = def.board[i];
    for (std::uint32_t x = 0; x < kC; ++x)
      holdings_.push_back(holding_for_dense(std::span<const int>(bd.data(), 5), x));
    precompute(bd);
    for (int k = 0; k < 64; ++k) {
      pool_.emplace_back(std::vector<double>(kC, 0.0));
      free_.push_back(pool_.size() - 1);
    }
  }

  std::size_t action_nodes() const {
    std::size_t n = 0;
    for (std::size_t i = 0; i < tree0_.size(); ++i)
      if (tree0_.node(i).is_action())
        ++n;
    return n;
  }

  std::size_t mapped_action_nodes() const {
    std::size_t n = 0;
    for (std::size_t i = 0; i < tree0_.size(); ++i)
      if (map_[i] != kNoMap && tree0_.node(i).is_action())
        ++n;
    return n;
  }

  struct RootWalks {
    std::array<std::vector<double>, 2> ev;
    std::array<std::vector<double>, 2> br;
  };

  // One EV and one exact BR walk per seat; returns the raw per-combo root
  // chip sums (opponent-reach convention: sum over all 1,081, blockers zeroed
  // only through the showdown matrix).
  RootWalks root_walks() {
    RootWalks w;
    for (std::size_t viewer = 0; viewer < 2; ++viewer) {
      std::array<std::vector<double>, 2> ro{std::vector<double>(kC, 1.0),
                                            std::vector<double>(kC, 1.0)};
      const std::size_t ev_id = eval(viewer, false, tree0_.root_index(), ro);
      std::array<std::vector<double>, 2> ro2{std::vector<double>(kC, 1.0),
                                             std::vector<double>(kC, 1.0)};
      const std::size_t br_id = eval(viewer, true, tree0_.root_index(), ro2);
      w.ev[viewer] = vec(ev_id);
      w.br[viewer] = vec(br_id);
      release(ev_id);
      release(br_id);
    }
    return w;
  }

  // Summed-seat exploitability in BB of the embedded cap-1 policy.
  double exploitability_bb() {
    const RootWalks w = root_walks();
    double raw = 0.0;
    for (std::size_t viewer = 0; viewer < 2; ++viewer)
      for (std::uint32_t i = 0; i < kC; ++i)
        raw += w.br[viewer][i] - w.ev[viewer][i];
    const double bb = def_.big_blind > 0 ? static_cast<double>(def_.big_blind) : 1.0;
    return raw / kNorm / bb;
  }

 private:
  using Reaches = std::array<std::vector<double>, 2>;

  void precompute(const std::array<int, 5>& bd) {
    a0_.assign(static_cast<std::size_t>(kC) * kC, 0);
    for (std::uint32_t i = 0; i < kC; ++i) {
      std::array<int, 7> h0{holdings_[i][0], holdings_[i][1], bd[0], bd[1], bd[2], bd[3], bd[4]};
      const auto s0 = bs::evaluate(h0.data(), 7).score;
      std::array<bool, 52> mine{};
      mine[holdings_[i][0]] = true;
      mine[holdings_[i][1]] = true;
      for (std::uint32_t j = 0; j < kC; ++j) {
        if (mine[holdings_[j][0]] || mine[holdings_[j][1]])
          continue;
        std::array<int, 7> h1{holdings_[j][0], holdings_[j][1], bd[0], bd[1], bd[2], bd[3], bd[4]};
        const auto s1 = bs::evaluate(h1.data(), 7).score;
        a0_[static_cast<std::size_t>(i) * kC + j] = s0 > s1 ? 1 : (s0 < s1 ? -1 : 0);
      }
    }
    fold0_.assign(tree0_.size(), 0.0);
    stake_.assign(tree0_.size(), 0.0);
    for (std::size_t n = 0; n < tree0_.size(); ++n) {
      const TreeNode& node = tree0_.node(n);
      if (!node.is_terminal())
        continue;
      const auto& pl = tree0_.terminal(node.terminal);
      if (node.kind == NodeKind::TerminalFold)
        fold0_[n] = static_cast<double>(pl.chip_utility[0]);
      else
        stake_[n] = static_cast<double>(pl.seats[pl.live_order[0]].contributed);
    }
  }

  std::size_t acquire() {
    if (free_.empty()) {
      pool_.emplace_back(std::vector<double>(kC, 0.0));
      return pool_.size() - 1;
    }
    std::size_t id = free_.back();
    free_.pop_back();
    return id;
  }
  void release(std::size_t id) { free_.push_back(id); }
  std::vector<double>& vec(std::size_t id) { return pool_[id]; }

  // True when the embedded policy puts exactly zero mass on this cap-0 edge
  // for EVERY combo: a cap-0-only suffix edge on a mapped node.
  bool zero_mass_edge(std::size_t n0, std::size_t a) const {
    const std::size_t n1 = map_[n0];
    if (n1 == kNoMap)
      return false;
    return a >= t1_.node(n1).actions.size();
  }

  // Embedded cap-1 probability for (node n0, own combo i, action a). On mapped
  // nodes the shared prefix reads the cap-1 policy and cap-0-only suffix edges
  // get exactly 0.0; extension (unmapped) nodes exist only under the BR walk's
  // completion rule, which intercepts before this lookup except for Uniform.
  double sigma(std::size_t n0, std::uint32_t i, std::size_t a) const {
    const TreeNode& node = tree0_.node(n0);
    const std::size_t n1 = map_[n0];
    if (n1 != kNoMap) {
      const std::size_t menu1 = t1_.node(n1).actions.size();
      if (a < menu1)
        return cap1_.avg_sigma[n1][i * menu1 + a];
      return 0.0;
    }
    if (completion_ == CapExtensionCompletion::Uniform)
      return 1.0 / static_cast<double>(node.actions.size());
    throw std::logic_error("cap_embedding: BrFavorable completion must intercept extension nodes");
  }

  std::size_t terminal_value(std::size_t n, std::size_t viewer, const Reaches& pi) {
    const std::size_t opp = viewer ^ 1;
    const std::size_t id = acquire();
    std::vector<double>& out = vec(id);
    const TreeNode& node = tree0_.node(n);
    if (node.kind == NodeKind::TerminalFold) {
      const double c = viewer == 0 ? fold0_[n] : -fold0_[n];
      double total = 0.0;
      for (std::uint32_t j = 0; j < kC; ++j)
        total += pi[opp][j];
      for (std::uint32_t i = 0; i < kC; ++i)
        out[i] = c * total;
      return id;
    }
    const double stake = stake_[n];
    if (viewer == 0) {
      for (std::uint32_t i = 0; i < kC; ++i) {
        double acc = 0.0;
        const std::int8_t* row =
            reinterpret_cast<const std::int8_t*>(a0_.data()) + static_cast<std::size_t>(i) * kC;
        for (std::uint32_t j = 0; j < kC; ++j)
          acc += static_cast<double>(row[j]) * pi[opp][j];
        out[i] = stake * acc;
      }
    } else {
      for (std::uint32_t i = 0; i < kC; ++i) {
        double acc = 0.0;
        for (std::uint32_t j = 0; j < kC; ++j)
          acc -= static_cast<double>(a0_[static_cast<std::size_t>(j) * kC + i]) * pi[opp][j];
        out[i] = stake * acc;
      }
    }
    return id;
  }

  std::size_t eval(std::size_t viewer, bool br, std::size_t n, Reaches& pi) {
    const TreeNode& node = tree0_.node(n);
    if (node.is_terminal())
      return terminal_value(n, viewer, pi);
    if (node.is_chance())
      throw std::runtime_error("cap_embedding: chance node in river-root tree");
    // Fail-closed structural invariant: the EV walk follows only positive
    // embedded-mass edges, every one of which leaves a mapped node toward
    // another mapped node, so it can never enter a cap-0-only extension. The
    // BR walk legitimately enters them and is handled by the completion rule.
    if (!br && map_[n] == kNoMap)
      throw std::logic_error("cap_embedding: EV walk reached an unmapped extension node");
    const std::size_t actor = node.actor;
    const std::size_t A = node.actions.size();
    const std::size_t out = acquire();
    std::fill(vec(out).begin(), vec(out).end(), 0.0);

    if (actor == viewer && br) {
      // Exact best response over the FULL cap-0 menu, including extensions.
      std::vector<std::size_t> kids(A);
      for (std::size_t a = 0; a < A; ++a)
        kids[a] = eval(viewer, br, node.children[a], pi);
      for (std::uint32_t i = 0; i < kC; ++i) {
        double best = vec(kids[0])[i];
        for (std::size_t a = 1; a < A; ++a)
          best = std::max(best, vec(kids[a])[i]);  // tie -> lowest index, deterministic
        vec(out)[i] = best;
      }
      for (std::size_t a = 0; a < A; ++a)
        release(kids[a]);
      return out;
    }

    // BrFavorable arm ONLY: at an unmapped extension node the opponent takes
    // the per-combo action most favorable to the best-responding viewer, which
    // bounds e_embed over EVERY possible extension completion. The Uniform arm
    // must NOT intercept here: it falls through to the generic opponent branch
    // below, where sigma() returns 1/A and the kernel-identical snapshot reach
    // scaling plays the uniform completion exactly.
    const bool br_favorable_extension =
        br && map_[n] == kNoMap && completion_ == CapExtensionCompletion::BrFavorable;
    if (br_favorable_extension) {
      std::vector<std::size_t> kids(A);
      for (std::size_t a = 0; a < A; ++a)
        kids[a] = eval(viewer, br, node.children[a], pi);
      for (std::uint32_t i = 0; i < kC; ++i) {
        double best = vec(kids[0])[i];
        for (std::size_t a = 1; a < A; ++a)
          best = std::max(best, vec(kids[a])[i]);
        vec(out)[i] = best;
      }
      for (std::size_t a = 0; a < A; ++a)
        release(kids[a]);
      return out;
    }

    if (actor == viewer) {
      // Own node under the fixed embedded policy (EV walk): own reach is
      // counterfactual and stays unchanged; zero-mass suffix edges are skipped.
      std::vector<std::size_t> kids(A, kNoMap);
      for (std::size_t a = 0; a < A; ++a) {
        if (zero_mass_edge(n, a))
          continue;
        kids[a] = eval(viewer, br, node.children[a], pi);
      }
      for (std::uint32_t i = 0; i < kC; ++i) {
        double v = 0.0;
        for (std::size_t a = 0; a < A; ++a)
          if (kids[a] != kNoMap)
            v += sigma(n, i, a) * vec(kids[a])[i];
        vec(out)[i] = v;
      }
      for (std::size_t a = 0; a < A; ++a)
        if (kids[a] != kNoMap)
          release(kids[a]);
      return out;
    }

    // Opponent node: kernel-identical snapshot reach scaling per action.
    const std::vector<double> saved = pi[actor];
    for (std::size_t a = 0; a < A; ++a) {
      if (zero_mass_edge(n, a))
        continue;  // embedded probability 0 for every combo
      for (std::uint32_t j = 0; j < kC; ++j)
        pi[actor][j] = saved[j] * sigma(n, j, a);
      const std::size_t child = eval(viewer, br, node.children[a], pi);
      for (std::uint32_t i = 0; i < kC; ++i)
        vec(out)[i] += vec(child)[i];
      release(child);
      pi[actor] = saved;
    }
    return out;
  }

  bs::poker::GameDef def_;
  const AbstractTree& tree0_;
  const RiverSolveResult& cap1_;
  const AbstractTree& t1_;
  const std::vector<std::size_t>& map_;
  CapExtensionCompletion completion_;
  std::vector<std::array<int, 2>> holdings_;
  std::vector<std::int8_t> a0_;
  std::vector<double> fold0_;
  std::vector<double> stake_;
  std::vector<std::vector<double>> pool_;
  std::vector<std::size_t> free_;
};

}  // namespace

std::vector<std::size_t> map_cap1_to_cap0(const AbstractTree& cap0, const AbstractTree& cap1) {
  std::vector<std::size_t> map(cap0.size(), kNoMap);
  pair_dfs(cap0, cap1, cap0.root_index(), cap1.root_index(), map);
  return map;
}

namespace {
// Solve the cap-1 policy, materialize both menus, build the paired map, and
// fail-closed on any distribution invariant violation. Shared by the gate
// measurement and the diagnostic root-value oracle.
struct PreparedEmbed {
  RiverSolveResult cap1;
  std::unique_ptr<AbstractTree> t0;
  std::unique_ptr<AbstractTree> t1;
  std::vector<std::size_t> map;
};

PreparedEmbed prepare_embed(const bs::poker::GameDef& river_def, std::uint64_t cap1_iters,
                            std::size_t threads) {
  if (river_def.player_count != 2 || river_def.board_size != 5 ||
      river_def.terminal != bs::poker::TerminalDepth::River)
    throw std::invalid_argument("cap_embedding: needs a 2-seat five-card river-root def");

  RiverSolveConfig cfg;
  cfg.def = river_def;
  cfg.iterations = cap1_iters;
  cfg.linear_averaging = true;
  cfg.threads = threads;
  cfg.action_cap = 1;
  PreparedEmbed p;
  p.cap1 = solve_river_subgame(cfg, {});
  p.t0 = std::make_unique<AbstractTree>(river_def, bs::abstraction::ActionAbstraction::identity(0));
  p.t1 = std::make_unique<AbstractTree>(river_def, bs::abstraction::ActionAbstraction::identity(1));
  p.map = map_cap1_to_cap0(*p.t0, *p.t1);

  // Distribution invariants on mapped nodes (fail-closed, never weaken). The
  // cap-1 snapshot vectors are indexed by the cap-1 tree's deterministic DFS
  // node order, which is identical for the same GameDef + identity(1) menu.
  for (std::size_t n0 = 0; n0 < p.t0->size(); ++n0) {
    const std::size_t n1 = p.map[n0];
    if (n1 == kNoMap || !p.t0->node(n0).is_action())
      continue;
    const auto& s1 = p.cap1.avg_sigma[n1];
    const std::size_t menu1 = p.t1->node(n1).actions.size();
    if (s1.empty())
      throw std::invalid_argument("cap_embedding: missing cap-1 policy on mapped node");
    for (std::uint32_t i = 0; i < kC; ++i) {
      double sum = 0.0;
      for (std::size_t a = 0; a < menu1; ++a) {
        const double pr = s1[i * menu1 + a];
        if (pr < 0.0)
          throw std::invalid_argument("cap_embedding: negative embedded probability");
        sum += pr;
      }
      if (std::fabs(sum - 1.0) > kDistEps)
        throw std::invalid_argument("cap_embedding: embedded distribution does not sum to 1");
    }
  }
  return p;
}
}  // namespace

CapMateriality measure_cap_materiality(const bs::poker::GameDef& river_def,
                                       std::uint64_t cap1_iters, std::size_t threads,
                                       CapExtensionCompletion completion) {
  PreparedEmbed p = prepare_embed(river_def, cap1_iters, threads);
  EmbeddedGame game(river_def, *p.t0, p.cap1, *p.t1, p.map, completion);
  CapMateriality out;
  out.cap0_action_nodes = game.action_nodes();
  out.cap1_action_nodes = p.cap1.action_nodes;
  out.mapped_nodes = game.mapped_action_nodes();
  out.cap1_nash_conv_bb = p.cap1.nash_conv_bb;
  out.embedded_exploitability_bb = game.exploitability_bb();
  out.gate_pass = out.embedded_exploitability_bb <= kCapMaterialityGateBb;
  // Structurally zero: the EV walk follows only positive embedded-mass edges,
  // and every cap-0-only edge leaves a mapped node with exactly zero mass, so
  // no unmapped node is EV-walk reachable. The BR walk entering extensions is
  // expected and handled by the completion rule, not counted here.
  out.unmapped_ev_reachable_nodes = 0;
  return out;
}

EmbeddedRootValues embed_root_values(const bs::poker::GameDef& river_def, std::uint64_t cap1_iters,
                                     std::size_t threads, CapExtensionCompletion completion) {
  PreparedEmbed p = prepare_embed(river_def, cap1_iters, threads);
  EmbeddedGame game(river_def, *p.t0, p.cap1, *p.t1, p.map, completion);
  const EmbeddedGame::RootWalks w = game.root_walks();
  return {w.ev[0], w.br[0], w.ev[1], w.br[1]};
}

StratumCapResult measure_stratum_cap_materiality(const StratumSpec& spec, std::uint64_t cap1_iters,
                                                 std::size_t threads,
                                                 CapExtensionCompletion completion) {
  // Every enumerated (betting line, river card) leaf is a distinct reached
  // five-card subgame: lines differ in pot/contributions and cards differ in
  // board, so no dedup is needed or valid. The gate is the maximum over ALL of
  // them (seedless exact enumeration has no sampling half-width).
  const std::vector<FrontierLeaf> leaves = enumerate_frontiers(spec, 1);
  StratumCapResult out;
  double worst = 0.0;
  for (const FrontierLeaf& lf : leaves) {
    const CapMateriality m = measure_cap_materiality(lf.river_def, cap1_iters, threads, completion);
    worst = std::max(worst, m.embedded_exploitability_bb);
    ++out.roots_measured;
  }
  out.worst_embedded_exploitability_bb = worst;
  out.gate_pass = worst <= kCapMaterialityGateBb;
  return out;
}

}  // namespace bs::rfc0047
