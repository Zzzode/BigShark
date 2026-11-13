#include "reach_bound.hpp"

#include <algorithm>
#include <array>
#include <bs/eval.hpp>
#include <cmath>
#include <limits>
#include <memory>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <unordered_map>
#include <utility>

namespace bs::rfc0047 {

namespace {
constexpr std::uint32_t kC = 1081;
constexpr std::uint32_t kCompat = 990;
constexpr double kNorm = static_cast<double>(kC) * static_cast<double>(kCompat);
constexpr double kDistEps = 1e-12;

using bs::tree::AbstractTree;
using bs::tree::NodeKind;
using bs::tree::TreeNode;

// Instrumented fixed-policy evaluator. The walk conventions are copied from
// the Stage-2b UnionGame (same pool, showdown matrix, fold/stake tables,
// snapshot reach scaling, completion arms); the only addition is exact
// node-level accounting at mapped own-seat action nodes during BR walks.
class ReachInstrument {
 public:
  ReachInstrument(const bs::poker::GameDef& def, const AbstractTree& reference,
                  const RiverSolveResult& solved, const AbstractTree& bounded_tree,
                  const SubsetMap& subset, CapExtensionCompletion completion)
      : def_(def),
        ref_(reference),
        solved_(solved),
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
    for (std::size_t s = 0; s < 2; ++s) {
      node_records_[s].clear();
      node_records_[s].resize(ref_.size());
      payoff_lo_[s].assign(ref_.size(), std::numeric_limits<double>::infinity());
      payoff_hi_[s].assign(ref_.size(), -std::numeric_limits<double>::infinity());
    }
    build_payoff_intervals();
  }

  // Full forward reach of one seat over the reference tree: the product of
  // the seat's OWN action masses on each path (opponent masses are not part
  // of a seat's reach; they enter the node gap through the continuation
  // values). Mapped-edge lookup only; extension subtrees hold no records.
  std::vector<std::vector<double>> forward_reach(std::size_t seat) const {
    std::vector<std::vector<double>> reach(ref_.size());
    reach[ref_.root_index()].assign(kC, 1.0);
    for (std::size_t n = 0; n < ref_.size(); ++n) {
      if (reach[n].empty())
        continue;
      const TreeNode& node = ref_.node(n);
      if (!node.is_action() || subset_.node_map[n] == SubsetMap::kNoNode)
        continue;
      const std::size_t nb = subset_.node_map[n];
      const std::size_t menu_b = bounded_tree_.node(nb).actions.size();
      for (std::size_t ar = 0; ar < node.actions.size(); ++ar) {
        const int edge = subset_.edge_map[n][ar];
        std::vector<double> child_reach = reach[n];
        if (node.actor == seat && edge != -1)
          for (std::uint32_t i = 0; i < kC; ++i)
            child_reach[i] = reach[n][i] * solved_.avg_sigma[nb][i * menu_b + edge];
        reach[node.children[ar]] = std::move(child_reach);
      }
    }
    return reach;
  }

  // D198: per-combo root EV and BR vectors over the reference tree for the
  // supplied snapshot, so an externally produced policy (e.g. the uniform
  // average) can be validated against the solver's own root CFV without
  // re-solving.
  UnionRootValues root_values(CapExtensionCompletion completion) {
    UnionRootValues values;
    values.ev_seat0.resize(kC);
    values.ev_seat1.resize(kC);
    values.br_seat0.resize(kC);
    values.br_seat1.resize(kC);
    const auto collect = [&](std::size_t viewer, bool br, std::vector<double>& dst) {
      std::array<std::vector<double>, 2> ro{std::vector<double>(kC, 1.0),
                                            std::vector<double>(kC, 1.0)};
      const std::size_t id = eval(viewer, br, ref_.root_index(), ro);
      dst = vec(id);
      release(id);
    };
    // The completion arm is fixed for the instrument; verify the request.
    if (completion != completion_)
      throw std::invalid_argument("reach_bound: root_values completion arm mismatch");
    collect(0, false, values.ev_seat0);
    collect(0, true, values.br_seat0);
    collect(1, false, values.ev_seat1);
    collect(1, true, values.br_seat1);
    return values;
  }

  ReachBoundResult run() {
    ReachBoundResult out;
    out.bounded_abstraction_digest = bounded_tree_.action_id().digest;
    out.reference_abstraction_digest = ref_.action_id().digest;
    out.bounded_shape_digest = bs::tree::tree_shape_digest(bounded_tree_);
    out.reference_shape_digest = bs::tree::tree_shape_digest(ref_);
    out.bounded_nash_conv_bb = solved_.nash_conv_bb;

    for (std::size_t viewer = 0; viewer < 2; ++viewer) {
      std::array<std::vector<double>, 2> ro_ev{std::vector<double>(kC, 1.0),
                                               std::vector<double>(kC, 1.0)};
      const std::size_t ev_id = eval(viewer, false, ref_.root_index(), ro_ev);
      std::array<std::vector<double>, 2> ro_br{std::vector<double>(kC, 1.0),
                                               std::vector<double>(kC, 1.0)};
      const std::size_t br_id = eval(viewer, true, ref_.root_index(), ro_br);
      double raw = 0.0;
      for (std::uint32_t i = 0; i < kC; ++i)
        raw += vec(br_id)[i] - vec(ev_id)[i];
      out.raw_exact_chips[viewer] = raw;
      release(ev_id);
      release(br_id);
    }

    const auto forward0 = forward_reach(0);
    const auto forward1 = forward_reach(1);
    const std::array<const std::vector<std::vector<double>>*, 2> forward{&forward0, &forward1};

    std::size_t mapped = 0;
    std::size_t widened = 0;
    double raw_exact = 0.0;
    double raw_certificate = 0.0;
    for (std::size_t s = 0; s < 2; ++s) {
      for (std::size_t n = 0; n < ref_.size(); ++n) {
        const NodeGapRecord* r = node_records_[s][n].get();
        if (r == nullptr)
          continue;
        ++mapped;
        if (r->widened)
          ++widened;
        // The telescope identity weights each node gap by the seat's FULL
        // forward reach (own action masses included); the walk-time
        // counterfactual reach in pi is only correct inside the continuation
        // values, not as this outer weight.
        NodeGapRecord copy = *r;
        copy.own_reach = (*forward[s])[n];
        double exact_term = 0.0;
        double bound_term = 0.0;
        for (std::uint32_t i = 0; i < kC; ++i) {
          exact_term += copy.own_reach[i] * copy.delta[i];
          bound_term += copy.own_reach[i] * copy.structural_bound_b;
        }
        raw_exact += exact_term;
        raw_certificate += copy.widened ? exact_term : bound_term;
        out.records[s].push_back(std::move(copy));
      }
    }
    const double bb = def_.big_blind > 0 ? static_cast<double>(def_.big_blind) : 1.0;
    out.mapped_action_nodes = mapped;
    out.widened_nodes = widened;
    out.bb_normalization = kNorm * bb;
    out.exact_exploitability_bb = raw_exact / out.bb_normalization;
    out.structural_certificate_bb = raw_certificate / out.bb_normalization;
    if (out.structural_certificate_bb + 1e-12 < out.exact_exploitability_bb)
      throw std::logic_error("reach_bound: structural certificate undercuts exact exploitability");
    return out;
  }

  // Per-viewer descendant terminal payoff interval, exposed for the naive
  // bound helper and the R(n) column of the records.
  double payoff_range(std::size_t seat, std::size_t node) const {
    return payoff_hi_[seat][node] - payoff_lo_[seat][node];
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

  // Reverse DFS-index aggregation: parent indices are always smaller.
  void build_payoff_intervals() {
    for (std::size_t n = ref_.size(); n-- > 0;) {
      const TreeNode& node = ref_.node(n);
      if (node.is_terminal()) {
        for (std::size_t s = 0; s < 2; ++s) {
          if (node.kind == NodeKind::TerminalFold) {
            const double c = s == 0 ? fold0_[n] : -fold0_[n];
            payoff_lo_[s][n] = c;
            payoff_hi_[s][n] = c;
          } else {
            payoff_lo_[s][n] = -stake_[n];
            payoff_hi_[s][n] = stake_[n];
          }
        }
        continue;
      }
      for (std::size_t s = 0; s < 2; ++s) {
        double lo = std::numeric_limits<double>::infinity();
        double hi = -std::numeric_limits<double>::infinity();
        for (std::size_t child : node.children) {
          lo = std::min(lo, payoff_lo_[s][child]);
          hi = std::max(hi, payoff_hi_[s][child]);
        }
        payoff_lo_[s][n] = lo;
        payoff_hi_[s][n] = hi;
      }
    }
  }

  std::size_t acquire() {
    if (free_.empty()) {
      pool_.emplace_back(std::vector<double>(kC, 0.0));
      return pool_.size() - 1;
    }
    const std::size_t id = free_.back();
    free_.pop_back();
    return id;
  }
  void release(std::size_t id) { free_.push_back(id); }
  std::vector<double>& vec(std::size_t id) { return pool_[id]; }

  bool zero_mass_edge(std::size_t n_ref, std::size_t a_ref) const {
    const std::size_t nb = subset_.node_map[n_ref];
    if (nb == SubsetMap::kNoNode)
      return false;
    return subset_.edge_map[n_ref][a_ref] == -1;
  }

  double sigma(std::size_t n_ref, std::uint32_t i, std::size_t a_ref) const {
    const std::size_t nb = subset_.node_map[n_ref];
    if (nb != SubsetMap::kNoNode) {
      const int edge = subset_.edge_map[n_ref][a_ref];
      if (edge == -1)
        return 0.0;
      const std::size_t menu_b = bounded_tree_.node(nb).actions.size();
      return solved_.avg_sigma[nb][i * menu_b + static_cast<std::size_t>(edge)];
    }
    const TreeNode& node = ref_.node(n_ref);
    if (completion_ == CapExtensionCompletion::Uniform)
      return 1.0 / static_cast<double>(node.actions.size());
    throw std::logic_error("reach_bound: BrFavorable must intercept extension nodes");
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

  // Exact node-level accounting at a mapped own-seat node.
  void record_node(std::size_t viewer, std::size_t n, const Reaches& pi,
                   const std::vector<std::size_t>& kids) {
    const TreeNode& node = ref_.node(n);
    const std::size_t nb = subset_.node_map[n];
    auto r = std::make_unique<NodeGapRecord>();
    r->reference_node = n;
    r->bounded_node = nb;
    r->seat = viewer;
    r->own_reach = pi[viewer];
    r->opponent_reach = pi[viewer ^ 1];
    r->edge_map = subset_.edge_map[n];
    r->widened = std::ranges::none_of(r->edge_map, [](int e) { return e == -1; });
    const std::size_t A = node.actions.size();
    r->continuation.assign(static_cast<std::size_t>(kC) * A, 0.0);
    r->delta.assign(kC, 0.0);
    r->bounded_gap.assign(kC, 0.0);
    for (std::uint32_t i = 0; i < kC; ++i) {
      for (std::size_t a = 0; a < A; ++a)
        r->continuation[i * A + a] = vec(kids[a])[i];
      double best = vec(kids[0])[i];
      for (std::size_t a = 1; a < A; ++a)
        best = std::max(best, vec(kids[a])[i]);
      double policy_value = 0.0;
      double best_bounded = -std::numeric_limits<double>::infinity();
      for (std::size_t a = 0; a < A; ++a) {
        const double pr = sigma(n, i, a);
        policy_value += pr * vec(kids[a])[i];
        if (!zero_mass_edge(n, a))
          best_bounded = std::max(best_bounded, vec(kids[a])[i]);
      }
      r->delta[i] = best - policy_value;
      r->bounded_gap[i] = best_bounded - policy_value;
    }
    double reach_sum = 0.0;
    for (double x : r->opponent_reach)
      reach_sum += x;
    r->opponent_reach_sum_s = reach_sum;
    r->payoff_range_r = payoff_range(viewer, n);
    r->structural_bound_b = r->payoff_range_r * reach_sum;
    node_records_[viewer][n] = std::move(r);
  }

  std::size_t eval(std::size_t viewer, bool br, std::size_t n, Reaches& pi) {
    const TreeNode& node = ref_.node(n);
    if (node.is_terminal())
      return terminal_value(n, viewer, pi);
    if (node.is_chance())
      throw std::runtime_error("reach_bound: chance node in river-root tree");
    if (!br && subset_.node_map[n] == SubsetMap::kNoNode)
      throw std::logic_error("reach_bound: EV walk reached an unmapped extension node");
    const std::size_t actor = node.actor;
    const std::size_t A = node.actions.size();
    const std::size_t out = acquire();
    std::fill(vec(out).begin(), vec(out).end(), 0.0);

    if (actor == viewer && br) {
      std::vector<std::size_t> kids(A);
      for (std::size_t a = 0; a < A; ++a)
        kids[a] = eval(viewer, br, node.children[a], pi);
      if (subset_.node_map[n] != SubsetMap::kNoNode)
        record_node(viewer, n, pi, kids);
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
  const RiverSolveResult& solved_;
  const AbstractTree& bounded_tree_;
  const SubsetMap& subset_;
  CapExtensionCompletion completion_;
  std::vector<std::array<int, 2>> holdings_;
  std::vector<std::int8_t> a0_;
  std::vector<double> fold0_;
  std::vector<double> stake_;
  std::vector<std::vector<double>> pool_;
  std::vector<std::size_t> free_;
  std::array<std::vector<std::unique_ptr<NodeGapRecord>>, 2> node_records_;
  std::array<std::vector<double>, 2> payoff_lo_;
  std::array<std::vector<double>, 2> payoff_hi_;
};

void validate_domain(const bs::poker::GameDef& river_def,
                     const bs::abstraction::ActionAbstraction& bounded,
                     const bs::abstraction::ActionAbstraction& reference) {
  if (river_def.player_count != 2 || river_def.board_size != 5 ||
      river_def.terminal != bs::poker::TerminalDepth::River)
    throw std::invalid_argument("reach_bound: needs a 2-seat five-card river-root def");
  if (bounded.cover_seeds() != bs::abstraction::CoverSeeds::MinAndCap ||
      reference.cover_seeds() != bs::abstraction::CoverSeeds::MinAndCap)
    throw std::invalid_argument("reach_bound: both abstractions must use MinAndCap");
}

void validate_snapshot(const RiverSolveResult& solved, const AbstractTree& bounded_tree,
                       const SubsetMap& subset, const AbstractTree& ref_tree) {
  for (std::size_t nr = 0; nr < ref_tree.size(); ++nr) {
    const std::size_t nb = subset.node_map[nr];
    if (nb == SubsetMap::kNoNode || !ref_tree.node(nr).is_action())
      continue;
    const auto& dist = solved.avg_sigma[nb];
    const std::size_t menu_b = bounded_tree.node(nb).actions.size();
    if (dist.empty())
      throw std::invalid_argument("reach_bound: missing bounded policy on paired node");
    for (std::uint32_t i = 0; i < kC; ++i) {
      double sum = 0.0;
      for (std::size_t a = 0; a < menu_b; ++a) {
        const double pr = dist[i * menu_b + a];
        if (!std::isfinite(pr) || pr < 0.0)
          throw std::invalid_argument("reach_bound: invalid embedded probability");
        sum += pr;
      }
      if (std::fabs(sum - 1.0) > kDistEps)
        throw std::invalid_argument("reach_bound: embedded distribution does not sum to 1");
    }
  }
}

}  // namespace

ReachBoundResult reach_bound_from_snapshot(const bs::poker::GameDef& river_def,
                                           const RiverSolveResult& solved,
                                           const AbstractTree& reference,
                                           const AbstractTree& bounded, const SubsetMap& subset,
                                           CapExtensionCompletion completion) {
  validate_domain(river_def, bounded.action_abstraction(), reference.action_abstraction());
  validate_snapshot(solved, bounded, subset, reference);
  ReachInstrument instrument(river_def, reference, solved, bounded, subset, completion);
  return instrument.run();
}

ReachBoundResult measure_reach_bound(const bs::poker::GameDef& river_def,
                                     const bs::abstraction::ActionAbstraction& bounded_action,
                                     const bs::abstraction::ActionAbstraction& reference_action,
                                     std::uint64_t iterations, std::size_t threads,
                                     CapExtensionCompletion completion) {
  validate_domain(river_def, bounded_action, reference_action);
  RiverSolveConfig cfg;
  cfg.def = river_def;
  cfg.iterations = iterations;
  cfg.linear_averaging = true;
  cfg.threads = threads;
  cfg.action_abstraction = bounded_action;  // one immutable snapshot for both passes
  const RiverSolveResult solved = solve_river_subgame(cfg, {});
  const AbstractTree ref_tree(river_def, reference_action);
  const AbstractTree bounded_tree(river_def, bounded_action);
  const SubsetMap subset = map_bounded_subset(ref_tree, bounded_tree);
  return reach_bound_from_snapshot(river_def, solved, ref_tree, bounded_tree, subset, completion);
}

std::vector<std::vector<double>> recompute_forward_reach(const AbstractTree& reference,
                                                         const AbstractTree& bounded,
                                                         const SubsetMap& subset,
                                                         const RiverSolveResult& solved,
                                                         std::size_t seat) {
  if (seat > 1)
    throw std::invalid_argument("reach_bound: seat must be 0 or 1");
  std::vector<std::vector<double>> reach(reference.size());
  reach[reference.root_index()].assign(kC, 1.0);
  // DFS node order guarantees parent before child. Only mapped edges are
  // traversed: unmapped extension subtrees contain no mapped nodes.
  for (std::size_t n = 0; n < reference.size(); ++n) {
    if (reach[n].empty())
      continue;
    const TreeNode& node = reference.node(n);
    if (!node.is_action() || subset.node_map[n] == SubsetMap::kNoNode)
      continue;
    const std::size_t nb = subset.node_map[n];
    const std::size_t menu_b = bounded.node(nb).actions.size();
    for (std::size_t ar = 0; ar < node.actions.size(); ++ar) {
      const int edge = subset.edge_map[n][ar];
      std::vector<double> child_reach = reach[n];  // snapshot update order
      if (node.actor == seat && edge != -1)
        for (std::uint32_t i = 0; i < kC; ++i)
          child_reach[i] = reach[n][i] * solved.avg_sigma[nb][i * menu_b + edge];
      reach[node.children[ar]] = std::move(child_reach);
    }
  }
  return reach;
}

void validate_supplied_reach(const AbstractTree& reference, const AbstractTree& bounded,
                             const SubsetMap& subset, const RiverSolveResult& solved,
                             std::size_t seat, const std::vector<std::vector<double>>& supplied) {
  if (supplied.size() != reference.size())
    throw std::invalid_argument("reach_bound: supplied reach table has the wrong node count");
  for (const auto& v : supplied) {
    if (v.empty())
      continue;
    if (v.size() != kC)
      throw std::invalid_argument("reach_bound: supplied reach row has the wrong combo width");
    for (double x : v)
      if (!std::isfinite(x) || x < 0.0)
        throw std::invalid_argument("reach_bound: supplied reach contains invalid entries");
  }
  const auto expected = recompute_forward_reach(reference, bounded, subset, solved, seat);
  for (std::size_t n = 0; n < reference.size(); ++n) {
    if (expected[n].empty())
      continue;
    if (supplied[n].size() != kC)
      throw std::invalid_argument("reach_bound: supplied reach missing a mapped node");
    for (std::uint32_t i = 0; i < kC; ++i)
      if (supplied[n][i] != expected[n][i])  // exact: tolerance would admit scaling attacks
        throw std::invalid_argument("reach_bound: supplied reach is not the policy's own reach");
  }
}

namespace {

// Flatten order shared by verify_custom_bounds and naive_max_abs_payoff_bounds:
// seat 0 records then seat 1 records, each in reference-node index order.
std::vector<const NodeGapRecord*> flatten_records(const ReachBoundResult& result) {
  std::vector<const NodeGapRecord*> flat;
  for (std::size_t s = 0; s < 2; ++s)
    for (const NodeGapRecord& r : result.records[s])
      flat.push_back(&r);
  return flat;
}

}  // namespace

void verify_custom_bounds(const ReachBoundResult& result, const std::vector<double>& node_bound) {
  const auto flat = flatten_records(result);
  if (node_bound.size() != flat.size())
    throw std::invalid_argument("reach_bound: bound table size does not match the node records");
  for (std::size_t k = 0; k < flat.size(); ++k) {
    if (!std::isfinite(node_bound[k]) || node_bound[k] < 0.0)
      throw std::invalid_argument("reach_bound: bound entry is malformed");
    const NodeGapRecord& r = *flat[k];
    for (std::uint32_t i = 0; i < kC; ++i)
      if (r.own_reach[i] > 0.0 && node_bound[k] + 1e-12 < r.delta[i])
        throw std::invalid_argument("reach_bound: custom bound undercuts a measured gap");
  }
}

double selective_certificate(const ReachBoundResult& result,
                             const std::vector<std::size_t>& widened_reference_nodes) {
  // Global node validity: a claimed reference node must be a mapped action
  // node of exactly one seat and must carry no extension edge.
  std::unordered_map<std::size_t, std::pair<std::size_t, std::size_t>> location;
  for (std::size_t s = 0; s < 2; ++s)
    for (std::size_t k = 0; k < result.records[s].size(); ++k)
      location.emplace(result.records[s][k].reference_node, std::make_pair(s, k));
  std::array<std::vector<bool>, 2> is_claimed;
  for (std::size_t s = 0; s < 2; ++s)
    is_claimed[s].assign(result.records[s].size(), false);
  for (std::size_t node : widened_reference_nodes) {
    const auto it = location.find(node);
    if (it == location.end())
      throw std::invalid_argument(
          "reach_bound: widening claim names a node that is not a mapped action node");
    const auto [seat, index] = it->second;
    if (!result.records[seat][index].widened)
      throw std::invalid_argument(
          "reach_bound: cannot claim widening at a node with an extension edge");
    is_claimed[seat][index] = true;
  }
  double raw = 0.0;
  for (std::size_t s = 0; s < 2; ++s) {
    for (std::size_t k = 0; k < result.records[s].size(); ++k) {
      const NodeGapRecord& r = result.records[s][k];
      for (std::uint32_t i = 0; i < kC; ++i) {
        const double per_combo = is_claimed[s][k] ? r.delta[i] : r.structural_bound_b;
        raw += r.own_reach[i] * per_combo;
      }
    }
  }
  return raw / result.bb_normalization;
}

double exact_diagnostic_certificate(const ReachBoundResult& result) {
  double raw = 0.0;
  for (std::size_t s = 0; s < 2; ++s)
    for (const NodeGapRecord& r : result.records[s])
      for (std::uint32_t i = 0; i < kC; ++i)
        raw += r.own_reach[i] * r.delta[i];
  return raw / result.bb_normalization;
}

double aggregate_roots_max(const std::vector<double>& per_root_exploitability_bb) {
  if (per_root_exploitability_bb.empty())
    throw std::invalid_argument("reach_bound: root aggregation needs at least one root");
  double best = per_root_exploitability_bb.front();
  for (double x : per_root_exploitability_bb) {
    if (!std::isfinite(x))
      throw std::invalid_argument("reach_bound: non-finite root exploitability");
    best = std::max(best, x);
  }
  return best;
}

void refuse_root_weights(const std::vector<double>& per_root_exploitability_bb,
                         const std::vector<double>& root_weights) {
  if (per_root_exploitability_bb.empty() ||
      root_weights.size() != per_root_exploitability_bb.size())
    throw std::invalid_argument("reach_bound: malformed root weighting");
  double sum = 0.0;
  for (double w : root_weights) {
    if (!std::isfinite(w) || w < 0.0)
      throw std::invalid_argument("reach_bound: invalid root weight");
    sum += w;
  }
  if (std::fabs(sum - 1.0) > 1e-12)
    throw std::invalid_argument("reach_bound: root weights must sum to 1");
  throw std::invalid_argument(
      "reach_bound: root-level weighting is refused; the frozen contract uses an unweighted MAX");
}

std::vector<double> naive_max_abs_payoff_bounds(const AbstractTree& reference,
                                                const ReachBoundResult& result) {
  // The review-rejected draft bound: max absolute descendant terminal
  // payoff per record node. Fold leaves carry the settled net utility
  // (equal magnitude for both seats); showdown leaves span +/- the live
  // seat contribution. The bench retains this only as the RED falsifier:
  // it omits the opponent-reach mass S(n) and the range factor.
  const auto flat = flatten_records(result);
  std::vector<double> bounds;
  bounds.reserve(flat.size());
  for (const NodeGapRecord* r : flat) {
    double max_abs = 0.0;
    std::vector<std::size_t> stack{r->reference_node};
    while (!stack.empty()) {
      const std::size_t n = stack.back();
      stack.pop_back();
      const TreeNode& node = reference.node(n);
      if (node.is_terminal()) {
        const auto& pl = reference.terminal(node.terminal);
        if (node.kind == NodeKind::TerminalFold) {
          max_abs = std::max(max_abs, static_cast<double>(std::fabs(pl.chip_utility[0])));
        } else {
          max_abs = std::max(max_abs, static_cast<double>(pl.seats[pl.live_order[0]].contributed));
        }
        continue;
      }
      for (std::size_t child : node.children)
        stack.push_back(child);
    }
    bounds.push_back(max_abs);
  }
  return bounds;
}

namespace {

// D195: self-consistent BR walk over the BOUNDED tree. The bounded tree has
// no extension nodes by construction, so there is no completion-arm
// parameter; the walk is the standard fixed-policy exploitability
// recurrence with per-node gap recording.
class RestrictedInstrument {
 public:
  RestrictedInstrument(const bs::poker::GameDef& def, const AbstractTree& bounded,
                       const RiverSolveResult& solved)
      : def_(def), tree_(bounded), solved_(solved) {
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
    for (std::size_t s = 0; s < 2; ++s)
      records_[s].resize(tree_.size());
  }

  RestrictedGapResult run() {
    RestrictedGapResult out;
    out.bounded_nash_conv_bb = solved_.nash_conv_bb;
    const double bb = def_.big_blind > 0 ? static_cast<double>(def_.big_blind) : 1.0;
    out.bb_normalization = kNorm * bb;

    for (std::size_t viewer = 0; viewer < 2; ++viewer) {
      // Full forward reach for the telescope weight.
      const auto forward = forward_reach(viewer);
      std::array<std::vector<double>, 2> ro{std::vector<double>(kC, 1.0),
                                            std::vector<double>(kC, 1.0)};
      const std::size_t br_id = eval(viewer, true, tree_.root_index(), ro);
      double raw = 0.0;
      for (std::size_t n = 0; n < tree_.size(); ++n) {
        if (!records_[viewer][n].has_value())
          continue;
        RestrictedNodeGap& g = *records_[viewer][n];
        g.own_reach = forward[n];
        for (std::uint32_t i = 0; i < kC; ++i)
          raw += g.own_reach[i] * g.restricted_gap[i];
        out.records[viewer].push_back(g);
      }
      out.raw_chips[viewer] = raw;
      release(br_id);
    }
    double total_raw = out.raw_chips[0] + out.raw_chips[1];
    out.nash_conv_bb = total_raw / out.bb_normalization;
    return out;
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
    fold0_.assign(tree_.size(), 0.0);
    stake_.assign(tree_.size(), 0.0);
    for (std::size_t n = 0; n < tree_.size(); ++n) {
      const TreeNode& node = tree_.node(n);
      if (!node.is_terminal())
        continue;
      const auto& pl = tree_.terminal(node.terminal);
      if (node.kind == NodeKind::TerminalFold)
        fold0_[n] = static_cast<double>(pl.chip_utility[0]);
      else
        stake_[n] = static_cast<double>(pl.seats[pl.live_order[0]].contributed);
    }
  }

  std::vector<std::vector<double>> forward_reach(std::size_t seat) const {
    std::vector<std::vector<double>> reach(tree_.size());
    reach[tree_.root_index()].assign(kC, 1.0);
    for (std::size_t n = 0; n < tree_.size(); ++n) {
      if (reach[n].empty())
        continue;
      const TreeNode& node = tree_.node(n);
      if (!node.is_action())
        continue;
      const std::size_t menu = node.actions.size();
      for (std::size_t a = 0; a < menu; ++a) {
        std::vector<double> child = reach[n];
        if (node.actor == seat)
          for (std::uint32_t i = 0; i < kC; ++i)
            child[i] = reach[n][i] * solved_.avg_sigma[n][i * menu + a];
        reach[node.children[a]] = std::move(child);
      }
    }
    return reach;
  }

  std::size_t acquire() {
    if (free_.empty()) {
      pool_.emplace_back(std::vector<double>(kC, 0.0));
      return pool_.size() - 1;
    }
    const std::size_t id = free_.back();
    free_.pop_back();
    return id;
  }
  void release(std::size_t id) { free_.push_back(id); }
  std::vector<double>& vec(std::size_t id) { return pool_[id]; }

  double sigma(std::size_t n, std::uint32_t i, std::size_t a) const {
    const std::size_t menu = tree_.node(n).actions.size();
    return solved_.avg_sigma[n][i * menu + a];
  }

  std::size_t terminal_value(std::size_t n, std::size_t viewer, const Reaches& pi) {
    const std::size_t opp = viewer ^ 1;
    const std::size_t id = acquire();
    std::vector<double>& out = vec(id);
    const TreeNode& node = tree_.node(n);
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
    const TreeNode& node = tree_.node(n);
    if (node.is_terminal())
      return terminal_value(n, viewer, pi);
    if (node.is_chance())
      throw std::runtime_error("reach_bound: chance node in bounded river tree");
    const std::size_t actor = node.actor;
    const std::size_t A = node.actions.size();
    const std::size_t out = acquire();
    std::fill(vec(out).begin(), vec(out).end(), 0.0);

    if (actor == viewer && br) {
      std::vector<std::size_t> kids(A);
      for (std::size_t a = 0; a < A; ++a)
        kids[a] = eval(viewer, br, node.children[a], pi);
      auto g = std::make_optional<RestrictedNodeGap>();
      g->bounded_node = n;
      g->seat = viewer;
      g->restricted_gap.assign(kC, 0.0);
      for (std::uint32_t i = 0; i < kC; ++i) {
        double best = vec(kids[0])[i];
        for (std::size_t a = 1; a < A; ++a)
          best = std::max(best, vec(kids[a])[i]);
        double policy_value = 0.0;
        for (std::size_t a = 0; a < A; ++a)
          policy_value += sigma(n, i, a) * vec(kids[a])[i];
        g->restricted_gap[i] = best - policy_value;
      }
      records_[viewer][n] = std::move(g);
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
      std::vector<std::size_t> kids(A, SubsetMap::kNoNode);
      for (std::size_t a = 0; a < A; ++a)
        kids[a] = eval(viewer, br, node.children[a], pi);
      for (std::uint32_t i = 0; i < kC; ++i) {
        double v = 0.0;
        for (std::size_t a = 0; a < A; ++a)
          v += sigma(n, i, a) * vec(kids[a])[i];
        vec(out)[i] = v;
      }
      for (std::size_t a = 0; a < A; ++a)
        release(kids[a]);
      return out;
    }

    const std::vector<double> saved = pi[actor];
    for (std::size_t a = 0; a < A; ++a) {
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
  const AbstractTree& tree_;
  const RiverSolveResult& solved_;
  std::vector<std::array<int, 2>> holdings_;
  std::vector<std::int8_t> a0_;
  std::vector<double> fold0_;
  std::vector<double> stake_;
  std::vector<std::vector<double>> pool_;
  std::vector<std::size_t> free_;
  std::array<std::vector<std::optional<RestrictedNodeGap>>, 2> records_;
};

}  // namespace

RestrictedGapResult restricted_gaps_from_snapshot(const bs::poker::GameDef& river_def,
                                                  const RiverSolveResult& solved,
                                                  const AbstractTree& bounded) {
  if (river_def.player_count != 2 || river_def.board_size != 5 ||
      river_def.terminal != bs::poker::TerminalDepth::River)
    throw std::invalid_argument("reach_bound: needs a 2-seat five-card river-root def");
  RestrictedInstrument instrument(river_def, bounded, solved);
  return instrument.run();
}

UnionRootValues root_values_from_snapshot(const bs::poker::GameDef& river_def,
                                          const RiverSolveResult& solved,
                                          const AbstractTree& reference,
                                          const AbstractTree& bounded, const SubsetMap& subset,
                                          CapExtensionCompletion completion) {
  validate_domain(river_def, bounded.action_abstraction(), reference.action_abstraction());
  validate_snapshot(solved, bounded, subset, reference);
  ReachInstrument instrument(river_def, reference, solved, bounded, subset, completion);
  return instrument.root_values(completion);
}

RestrictedGapResult measure_restricted_gaps(
    const bs::poker::GameDef& river_def, const bs::abstraction::ActionAbstraction& bounded_action,
    std::uint64_t iterations, std::size_t threads) {
  RiverSolveConfig cfg;
  cfg.def = river_def;
  cfg.iterations = iterations;
  cfg.linear_averaging = true;
  cfg.threads = threads;
  cfg.action_abstraction = bounded_action;
  const RiverSolveResult solved = solve_river_subgame(cfg, {});
  const AbstractTree bounded(river_def, bounded_action);
  return restricted_gaps_from_snapshot(river_def, solved, bounded);
}

bool subtree_is_extension_free(const AbstractTree& reference, const SubsetMap& subset,
                               std::size_t reference_node, std::size_t seat) {
  // DFS over mapped edges only. A same-seat mapped action node with any -1
  // edge makes the subtree not extension-free; opponent-seat -1 edges are
  // zero-mass and irrelevant. A mapped edge reaching an unmapped child is
  // a mapper invariant violation.
  std::vector<std::size_t> stack{reference_node};
  while (!stack.empty()) {
    const std::size_t n = stack.back();
    stack.pop_back();
    const TreeNode& node = reference.node(n);
    if (!node.is_action() || subset.node_map[n] == SubsetMap::kNoNode)
      continue;
    if (node.actor == seat) {
      for (int edge : subset.edge_map[n])
        if (edge == -1)
          return false;
    }
    for (std::size_t a = 0; a < node.actions.size(); ++a) {
      if (subset.edge_map[n][a] == -1)
        continue;
      const std::size_t child = node.children[a];
      if (subset.node_map[child] == SubsetMap::kNoNode)
        throw std::invalid_argument("reach_bound: mapped edge reached an unmapped child");
      stack.push_back(child);
    }
  }
  return true;
}

}  // namespace bs::rfc0047
