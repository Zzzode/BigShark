#include "union_embedding.hpp"

#include <algorithm>
#include <array>
#include <bs/eval.hpp>
#include <cmath>
#include <memory>
#include <stdexcept>

namespace bs::rfc0047 {

namespace {
constexpr std::uint32_t kC = 1081;
constexpr std::uint32_t kCompat = 990;
constexpr double kNorm = static_cast<double>(kC) * static_cast<double>(kCompat);
constexpr double kDistEps = 1e-12;

using bs::tree::AbstractTree;
using bs::tree::NodeKind;
using bs::tree::tree_shape_digest;
using bs::tree::TreeNode;

// Edge-map-driven fixed-policy evaluator over a reference tree that is a
// menu superset of the solved bounded tree. Structure mirrors the frozen
// cap_embedding EmbeddedGame one-for-one (pool, showdown matrix, fold/stake
// tables, snapshot reach scaling, completion arms); only the map lookups
// change: edge_map replaces every prefix-index assumption.
class UnionGame {
 public:
  UnionGame(const bs::poker::GameDef& def, const AbstractTree& reference,
            const RiverSolveResult& bounded_result, const AbstractTree& bounded_tree,
            const SubsetMap& subset, CapExtensionCompletion completion)
      : def_(def),
        ref_(reference),
        online_(bounded_result),
        bounded_tree_(bounded_tree),
        subset_(subset),
        completion_(completion) {
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
    for (std::size_t i = 0; i < ref_.size(); ++i)
      if (ref_.node(i).is_action())
        ++n;
    return n;
  }

  std::size_t mapped_action_nodes() const {
    std::size_t n = 0;
    for (std::size_t i = 0; i < ref_.size(); ++i)
      if (subset_.node_map[i] != SubsetMap::kNoNode && ref_.node(i).is_action())
        ++n;
    return n;
  }

  struct RootWalks {
    std::array<std::vector<double>, 2> ev;
    std::array<std::vector<double>, 2> br;
  };

  RootWalks root_walks() {
    RootWalks w;
    for (std::size_t viewer = 0; viewer < 2; ++viewer) {
      std::array<std::vector<double>, 2> ro{std::vector<double>(kC, 1.0),
                                            std::vector<double>(kC, 1.0)};
      const std::size_t ev_id = eval(viewer, false, ref_.root_index(), ro);
      std::array<std::vector<double>, 2> ro2{std::vector<double>(kC, 1.0),
                                             std::vector<double>(kC, 1.0)};
      const std::size_t br_id = eval(viewer, true, ref_.root_index(), ro2);
      w.ev[viewer] = vec(ev_id);
      w.br[viewer] = vec(br_id);
      release(ev_id);
      release(br_id);
    }
    return w;
  }

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
    fold0_.assign(ref_.size(), 0.0);
    stake_.assign(ref_.size(), 0.0);
    for (std::size_t n = 0; n < ref_.size(); ++n) {
      const TreeNode& node = ref_.node(n);
      if (!node.is_terminal())
        continue;
      const auto& pl = ref_.terminal(node.terminal);
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

  // True when the embedded policy puts exactly zero mass on this reference
  // edge for every combo: a reference-only edge (-1) on a paired node.
  bool zero_mass_edge(std::size_t n_ref, std::size_t a_ref) const {
    const std::size_t nb = subset_.node_map[n_ref];
    if (nb == SubsetMap::kNoNode)
      return false;
    return subset_.edge_map[n_ref][a_ref] == -1;
  }

  // Embedded bounded probability for (reference node n_ref, own combo i,
  // reference action a_ref). Paired nodes route through edge_map; extension
  // nodes exist only under the BR walk's completion rule, which intercepts
  // before this lookup except for Uniform.
  double sigma(std::size_t n_ref, std::uint32_t i, std::size_t a_ref) const {
    const std::size_t nb = subset_.node_map[n_ref];
    if (nb != SubsetMap::kNoNode) {
      const int edge = subset_.edge_map[n_ref][a_ref];
      if (edge == -1)
        return 0.0;  // reference-only edge, zero embedded mass
      const std::size_t menu_b = bounded_tree_.node(nb).actions.size();
      return online_.avg_sigma[nb][i * menu_b + static_cast<std::size_t>(edge)];
    }
    const TreeNode& node = ref_.node(n_ref);
    if (completion_ == CapExtensionCompletion::Uniform)
      return 1.0 / static_cast<double>(node.actions.size());
    throw std::logic_error(
        "union_embedding: BrFavorable completion must intercept extension nodes");
  }

  std::size_t terminal_value(std::size_t n, std::size_t viewer, const Reaches& pi) {
    const std::size_t opp = viewer ^ 1;
    const std::size_t id = acquire();
    std::vector<double>& out = vec(id);
    const TreeNode& node = ref_.node(n);
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
    const TreeNode& node = ref_.node(n);
    if (node.is_terminal())
      return terminal_value(n, viewer, pi);
    if (node.is_chance())
      throw std::runtime_error("union_embedding: chance node in river-root tree");
    // Fail-closed structural invariant: the EV walk follows only positive
    // embedded-mass mapped edges, so it can never enter an extension subtree.
    if (!br && subset_.node_map[n] == SubsetMap::kNoNode)
      throw std::logic_error("union_embedding: EV walk reached an unmapped extension node");
    const std::size_t actor = node.actor;
    const std::size_t A = node.actions.size();
    const std::size_t out = acquire();
    std::fill(vec(out).begin(), vec(out).end(), 0.0);

    if (actor == viewer && br) {
      // Exact best response over the FULL reference menu, including
      // extension edges; the superset max is the source of conservatism.
      std::vector<std::size_t> kids(A);
      for (std::size_t a = 0; a < A; ++a)
        kids[a] = eval(viewer, br, node.children[a], pi);
      for (std::uint32_t i = 0; i < kC; ++i) {
        double best = vec(kids[0])[i];
        for (std::size_t a = 1; a < A; ++a)
          best = std::max(best, vec(kids[a])[i]);  // tie -> lowest index
        vec(out)[i] = best;
      }
      for (std::size_t a = 0; a < A; ++a)
        release(kids[a]);
      return out;
    }

    const bool br_favorable_extension = br && subset_.node_map[n] == SubsetMap::kNoNode &&
                                        completion_ == CapExtensionCompletion::BrFavorable;
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
      // counterfactual and stays unchanged; zero-mass edges are skipped.
      std::vector<std::size_t> kids(A, SubsetMap::kNoNode);
      for (std::size_t a = 0; a < A; ++a) {
        if (zero_mass_edge(n, a))
          continue;
        kids[a] = eval(viewer, br, node.children[a], pi);
      }
      for (std::uint32_t i = 0; i < kC; ++i) {
        double v = 0.0;
        for (std::size_t a = 0; a < A; ++a)
          if (kids[a] != SubsetMap::kNoNode)
            v += sigma(n, i, a) * vec(kids[a])[i];
        vec(out)[i] = v;
      }
      for (std::size_t a = 0; a < A; ++a)
        if (kids[a] != SubsetMap::kNoNode)
          release(kids[a]);
      return out;
    }

    // Opponent node: kernel-identical snapshot reach scaling per action.
    const std::vector<double> saved = pi[actor];
    for (std::size_t a = 0; a < A; ++a) {
      if (zero_mass_edge(n, a))
        continue;
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
  const AbstractTree& ref_;
  const RiverSolveResult& online_;
  const AbstractTree& bounded_tree_;
  const SubsetMap& subset_;
  CapExtensionCompletion completion_;
  std::vector<std::array<int, 2>> holdings_;
  std::vector<std::int8_t> a0_;
  std::vector<double> fold0_;
  std::vector<double> stake_;
  std::vector<std::vector<double>> pool_;
  std::vector<std::size_t> free_;
};

struct PreparedSubset {
  RiverSolveResult online;
  std::unique_ptr<AbstractTree> ref_tree;
  std::unique_ptr<AbstractTree> bounded_tree;
  SubsetMap subset;
};

PreparedSubset prepare_subset(const bs::poker::GameDef& river_def,
                              const bs::abstraction::ActionAbstraction& bounded_action,
                              const bs::abstraction::ActionAbstraction& reference_action,
                              std::uint64_t iterations, std::size_t threads) {
  if (river_def.player_count != 2 || river_def.board_size != 5 ||
      river_def.terminal != bs::poker::TerminalDepth::River)
    throw std::invalid_argument("union_embedding: needs a 2-seat five-card river-root def");
  if (bounded_action.cover_seeds() != bs::abstraction::CoverSeeds::MinAndCap ||
      reference_action.cover_seeds() != bs::abstraction::CoverSeeds::MinAndCap)
    throw std::invalid_argument("union_embedding: both abstractions must use MinAndCap");

  RiverSolveConfig cfg;
  cfg.def = river_def;
  cfg.iterations = iterations;
  cfg.linear_averaging = true;
  cfg.threads = threads;
  // The SAME abstraction object drives the solver and the bounded tree.
  cfg.action_abstraction = bounded_action;
  PreparedSubset p;
  p.online = solve_river_subgame(cfg, {});
  p.ref_tree = std::make_unique<AbstractTree>(river_def, reference_action);
  p.bounded_tree = std::make_unique<AbstractTree>(river_def, bounded_action);
  p.subset = map_bounded_subset(*p.ref_tree, *p.bounded_tree);

  // Distribution invariants on paired nodes (fail-closed). The snapshot
  // vectors are indexed by the bounded tree's deterministic DFS node order.
  for (std::size_t nr = 0; nr < p.ref_tree->size(); ++nr) {
    const std::size_t nb = p.subset.node_map[nr];
    if (nb == SubsetMap::kNoNode || !p.ref_tree->node(nr).is_action())
      continue;
    const auto& dist = p.online.avg_sigma[nb];
    const std::size_t menu_b = p.bounded_tree->node(nb).actions.size();
    if (dist.empty())
      throw std::invalid_argument("union_embedding: missing bounded policy on paired node");
    for (std::uint32_t i = 0; i < kC; ++i) {
      double sum = 0.0;
      for (std::size_t a = 0; a < menu_b; ++a) {
        const double pr = dist[i * menu_b + a];
        if (pr < 0.0)
          throw std::invalid_argument("union_embedding: negative embedded probability");
        sum += pr;
      }
      if (std::fabs(sum - 1.0) > kDistEps)
        throw std::invalid_argument("union_embedding: embedded distribution does not sum to 1");
    }
  }
  return p;
}

UnionMateriality run_measurement(const bs::poker::GameDef& river_def,
                                 const bs::abstraction::ActionAbstraction& bounded_action,
                                 const bs::abstraction::ActionAbstraction& reference_action,
                                 std::uint64_t iterations, std::size_t threads,
                                 CapExtensionCompletion completion) {
  PreparedSubset p =
      prepare_subset(river_def, bounded_action, reference_action, iterations, threads);
  UnionGame game(river_def, *p.ref_tree, p.online, *p.bounded_tree, p.subset, completion);
  UnionMateriality out;
  out.bounded_abstraction_digest = bounded_action.id().digest;
  out.reference_abstraction_digest = reference_action.id().digest;
  out.bounded_shape_digest = tree_shape_digest(*p.bounded_tree);
  out.reference_shape_digest = tree_shape_digest(*p.ref_tree);
  out.reference_action_nodes = game.action_nodes();
  out.bounded_action_nodes = p.online.action_nodes;
  out.mapped_nodes = game.mapped_action_nodes();
  out.unmapped_ev_reachable_nodes = 0;
  out.bounded_nash_conv_bb = p.online.nash_conv_bb;
  out.embedded_exploitability_bb = game.exploitability_bb();
  out.pure_menu_term_bb = out.embedded_exploitability_bb - out.bounded_nash_conv_bb;
  out.candidate_gate_pass = out.embedded_exploitability_bb <= kCapMaterialityGateBb;
  if (out.pure_menu_term_bb < -kDistEps)
    throw std::logic_error("union_embedding: negative pure menu term violates the BR proof");
  return out;
}

}  // namespace

UnionMateriality measure_subset_materiality(
    const bs::poker::GameDef& river_def, const bs::abstraction::ActionAbstraction& bounded_action,
    const bs::abstraction::ActionAbstraction& reference_action, std::uint64_t iterations,
    std::size_t threads, CapExtensionCompletion completion) {
  return run_measurement(river_def, bounded_action, reference_action, iterations, threads,
                         completion);
}

UnionMateriality measure_union_materiality(const bs::poker::GameDef& river_def,
                                           const bs::abstraction::ActionAbstraction& bounded_action,
                                           std::uint64_t iterations, std::size_t threads,
                                           CapExtensionCompletion completion) {
  const bs::abstraction::ActionAbstraction reference =
      make_union_abstraction(bs::abstraction::ActionAbstraction::identity(0), bounded_action);
  return run_measurement(river_def, bounded_action, reference, iterations, threads, completion);
}

UnionRootValues subset_root_values(const bs::poker::GameDef& river_def,
                                   const bs::abstraction::ActionAbstraction& bounded_action,
                                   const bs::abstraction::ActionAbstraction& reference_action,
                                   std::uint64_t iterations, std::size_t threads,
                                   CapExtensionCompletion completion) {
  PreparedSubset p =
      prepare_subset(river_def, bounded_action, reference_action, iterations, threads);
  UnionGame game(river_def, *p.ref_tree, p.online, *p.bounded_tree, p.subset, completion);
  const UnionGame::RootWalks w = game.root_walks();
  return {w.ev[0], w.br[0], w.ev[1], w.br[1]};
}

}  // namespace bs::rfc0047
