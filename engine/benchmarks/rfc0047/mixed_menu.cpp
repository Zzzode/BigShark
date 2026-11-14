#include "mixed_menu.hpp"

#include <algorithm>
#include <array>
#include <bs/abstract_tree.hpp>
#include <bs/abstraction.hpp>
#include <bs/eval.hpp>
#include <cmath>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <vector>

#include "strata.hpp"
#include "thread_pool.hpp"

namespace bs::rfc0047 {

using bs::abstraction::ActionAbstraction;
using bs::poker::Action;
using bs::poker::ActionType;
using bs::poker::GameDef;
using bs::poker::TerminalDepth;
using bs::tree::AbstractTree;
using bs::tree::NodeKind;
using bs::tree::TreeNode;

namespace {

constexpr std::uint32_t kC = 1081;      // C(47,2) own combos on a river board
constexpr std::uint32_t kCompat = 990;  // C(45,2) compatible opponent combos
constexpr double kNorm = static_cast<double>(kC) * kCompat;

bool is_aggressive(ActionType type) {
  return type == ActionType::Bet || type == ActionType::Raise;
}

// Root DFS closure over allowed edges; tree DFS order guarantees parents are
// indexed before their children.
std::vector<bool> reachable_closure(const AbstractTree& tree,
                                    const std::vector<std::vector<bool>>& allowed) {
  std::vector<bool> reachable(tree.size(), false);
  reachable[tree.root_index()] = true;
  for (std::size_t n = 0; n < tree.size(); ++n) {
    if (!reachable[n] || !tree.node(n).is_action())
      continue;
    const auto& kids = tree.node(n).children;
    for (std::size_t a = 0; a < kids.size(); ++a)
      if (allowed[n][a])
        reachable[kids[a]] = true;
  }
  return reachable;
}

}  // namespace

// Deployment-observable public betting state at every node, reconstructed from
// the root GameDef and the parent-chain incoming actions. Single river street
// only (TerminalDepth::River): street commitments never reset and
// close_street/refund never occur inside a replayed river path. The root pot
// is dead money, so the invariant pot == root_pot + street_committed[0] +
// street_committed[1] holds at the root and after every replayed action.
std::vector<PublicState> replay_public_states(const AbstractTree& reference) {
  const GameDef& def = reference.def();
  // Explicit street-start contract: the replay seeds fresh zero commitments,
  // so a mid-street root whose GameDef already carries street_committed (a
  // live subgame resolver root facing a wager) is rejected up front rather
  // than failing opaquely at the first child's pot invariant.
  for (std::size_t p = 0; p < def.player_count; ++p)
    if (def.street_committed[p] != 0)
      throw std::invalid_argument(
          "mixed_menu: public-state replay requires a street-start root (zero street_committed)");
  std::vector<PublicState> states(reference.size());
  const std::size_t root = reference.root_index();
  // The river betting street starts at zero: def.pot is the root dead money and
  // def.contributions already sit inside it, so street commitments are fresh.
  states[root].pot = def.pot;
  // DFS order indexes every parent before its children.
  for (std::size_t n = 0; n < reference.size(); ++n) {
    if (n == root)
      continue;
    const TreeNode& node = reference.node(n);
    PublicState s = states[node.parent];
    // The mover of incoming_action is the PARENT node's actor (the seat that
    // acted to create this child), not node.actor, which is the next player to
    // act and is kNoNode on terminal branches. River trees have no chance
    // nodes, so every non-root node has an action-node parent with a valid seat.
    const std::size_t actor = reference.node(node.parent).actor;
    const Action& a = node.incoming_action;
    if (a.type == ActionType::Call) {
      const std::uint64_t high = std::max(s.street_committed[0], s.street_committed[1]);
      const std::uint64_t due = high - s.street_committed[actor];
      const std::uint64_t remaining = def.stacks[actor] - s.street_committed[actor];
      const std::uint64_t paid = std::min(due, remaining);  // exact for short all-in calls
      s.street_committed[actor] += paid;
      s.pot += paid;
    } else if (is_aggressive(a.type)) {
      // target_total is the street-cumulative total; payment is its increment.
      const std::uint64_t paid = a.target_total - s.street_committed[actor];
      s.street_committed[actor] = a.target_total;
      s.pot += paid;
    }  // Fold and Check change nothing.
    // NDEBUG-independent invariant: live pot is the root dead money plus every
    // fresh street payment, at action and terminal nodes alike.
    const std::uint64_t street_sum = s.street_committed[0] + s.street_committed[1];
    if (s.pot != def.pot + street_sum)
      throw std::logic_error("mixed_menu: public-state replay broke the pot/street invariant");
    states[n] = s;
  }
  return states;
}

// Pure nearest-target comparator over additional-payment values: pick the
// legal aggressive action whose additional payment is closest to the pot-sized
// ideal; an exact equidistant pair resolves to the smaller target (the lower
// sorted index). Exposed as a free function so the tie-break is unit-testable
// with synthetic inputs (review F4). Returns -1 when no aggressive edge exists.
long nearest_aggressive_choice(const TreeNode& node, std::uint64_t ideal_additional,
                               std::uint64_t street_base) {
  long chosen = -1;
  std::uint64_t best_distance = 0;
  for (std::size_t a = 0; a < node.actions.size(); ++a) {
    if (!is_aggressive(node.actions[a].type))
      continue;
    // additional payment = literal street target_total minus the actor's
    // street-base commitment; the subtraction is a node-constant across
    // candidate actions, so argmin matches the literal-target comparison.
    const std::uint64_t additional = node.actions[a].target_total - street_base;
    const std::uint64_t distance = additional >= ideal_additional ? additional - ideal_additional
                                                                  : ideal_additional - additional;
    if (chosen == -1 || distance < best_distance) {
      chosen = static_cast<long>(a);
      best_distance = distance;
    }
  }
  return chosen;
}

namespace {

// D208 chooser: retain the sorted-first aggressive edge; ignores pot/due.
// Returns the edge index, or -1 when the node has no legal aggressive action.
long first_aggressive_choice(const TreeNode& node) {
  for (std::size_t a = 0; a < node.actions.size(); ++a)
    if (is_aggressive(node.actions[a].type))
      return static_cast<long>(a);
  return -1;
}

// D217 chooser: pick the existing aggressive action nearest the canonical
// pot-sized target. The street_base offset keeps the comparison in
// additional-payment space; it is a node-constant and cannot change the
// argmin versus literal target_total.
long pot_fraction_choice(const TreeNode& node, const PublicState& state) {
  const std::size_t actor = node.actor;
  const std::uint64_t high = std::max(state.street_committed[0], state.street_committed[1]);
  const std::uint64_t due = high - state.street_committed[actor];
  const std::uint64_t ideal_additional = due == 0 ? state.pot : state.pot + 2 * due;
  // street_committed already starts at zero for the river street, so it is
  // directly the base for additional payment against target_total.
  return nearest_aggressive_choice(node, ideal_additional, state.street_committed[actor]);
}

// Shared sparse-mask builder for the D208/D217 single-factor arms. W selection
// (public depth + deterministic DFS order), full reference menu on W, coarse
// subset on other coarse-mapped nodes, and on a reference-only node all
// passive edges plus the single aggressive edge returned by the chooser. The
// chooser is the ONLY factor that differs between arms.
template <typename Chooser>
MixedMenuMask build_sparse_mask(const AbstractTree& reference, const SubsetMap& base,
                                std::size_t widened_budget, Chooser choose_aggressive) {
  if (widened_budget == 0)
    throw std::invalid_argument("mixed_menu: widened budget must be positive");
  const std::size_t root = reference.root_index();

  std::vector<std::size_t> eligible;
  for (std::size_t n = 0; n < reference.size(); ++n) {
    if (!reference.node(n).is_action() || base.node_map[n] == SubsetMap::kNoNode)
      continue;
    const bool has_suppressed = std::any_of(base.edge_map[n].begin(), base.edge_map[n].end(),
                                            [](int edge) { return edge == -1; });
    if (has_suppressed)
      eligible.push_back(n);
  }
  const bool root_eligible = std::find(eligible.begin(), eligible.end(), root) != eligible.end();
  if (!root_eligible)
    throw std::invalid_argument(
        "mixed_menu: root has no suppressed edge; the root-first widening precondition is unmet");

  auto public_depth = [&](std::size_t n) {
    std::size_t depth = 0;
    while (n != root) {
      ++depth;
      n = reference.node(n).parent;
    }
    return depth;
  };
  std::sort(eligible.begin(), eligible.end(), [&](std::size_t x, std::size_t y) {
    const std::size_t dx = public_depth(x);
    const std::size_t dy = public_depth(y);
    if (dx != dy)
      return dx < dy;
    return x < y;
  });
  const std::size_t take = std::min(widened_budget, eligible.size());
  std::vector<bool> widened(reference.size(), false);
  for (std::size_t k = 0; k < take; ++k)
    widened[eligible[k]] = true;

  const std::vector<PublicState> states = replay_public_states(reference);
  MixedMenuMask mask;
  mask.allowed.assign(reference.size(), {});
  for (std::size_t n = 0; n < reference.size(); ++n)
    if (reference.node(n).is_action())
      mask.allowed[n].assign(reference.node(n).actions.size(), false);

  std::vector<bool> reachable(reference.size(), false);
  reachable[root] = true;
  for (std::size_t n = 0; n < reference.size(); ++n) {
    if (!reachable[n] || !reference.node(n).is_action())
      continue;
    const TreeNode& node = reference.node(n);
    std::vector<bool> row(node.actions.size(), false);
    if (widened[n]) {
      std::fill(row.begin(), row.end(), true);
    } else if (base.node_map[n] != SubsetMap::kNoNode) {
      for (std::size_t a = 0; a < node.actions.size(); ++a)
        row[a] = base.edge_map[n][a] != -1;
    } else {
      // Reference-only node opened through a widened ancestor: retain every
      // passive legality edge and the one aggressive edge the chooser picks.
      const long chosen = choose_aggressive(node, states[n]);
      for (std::size_t a = 0; a < node.actions.size(); ++a) {
        if (!is_aggressive(node.actions[a].type))
          row[a] = true;
        else if (static_cast<long>(a) == chosen)
          row[a] = true;
      }
    }
    mask.allowed[n] = std::move(row);
    for (std::size_t a = 0; a < node.children.size(); ++a)
      if (mask.allowed[n][a])
        reachable[node.children[a]] = true;
  }
  validate_mask(reference, mask);
  return mask;
}

// Self-contained CFR+ solver of the restricted extensive-form game an edge
// mask defines over the immutable reference tree. Terminal/stake/showdown
// data and the scratch-pool convention are shared with the river kernel; the
// traversal is slot-based and never descends a suppressed reference edge.
class MixedMenuGame {
 public:
  MixedMenuGame(const GameDef& d, const ActionAbstraction& reference_action,
                const MixedMenuMask& mask, std::size_t threads)
      : def_(d) {
    if (def_.player_count != 2 || def_.board_size != 5 || def_.terminal != TerminalDepth::River)
      throw std::invalid_argument("mixed_menu: needs a 2-seat five-card river-root def");
    workers_ = std::make_unique<Pool>(threads);
    std::array<int, 5> bd{};
    for (int i = 0; i < 5; ++i)
      bd[i] = def_.board[i];
    tree_ = std::make_unique<AbstractTree>(d, reference_action);
    validate_mask(*tree_, mask);
    reachable_ = reachable_closure(*tree_, mask.allowed);
    layout(mask);
    holdings_.resize(kC);
    for (std::uint32_t x = 0; x < kC; ++x)
      holdings_[x] = holding_for_dense(std::span<const int>(bd.data(), 5), x);
    precompute(bd);
  }

  ~MixedMenuGame() {
    for (std::vector<double>* p : reg_)
      delete p;
    for (std::vector<double>* p : sum_)
      delete p;
    for (std::vector<double>* p : sigma_)
      delete p;
  }

  const AbstractTree& tree() const { return *tree_; }

  std::size_t reachable_action_nodes() const { return action_nodes_; }
  std::size_t compact_slot_total() const { return compact_slots_; }
  std::size_t reference_edge_total() const { return reference_edges_; }

  void iterate_weighted(double weight) {
    avg_weight_[0] = weight;
    avg_weight_[1] = weight;
    run_traversal(0);
    run_traversal(1);
    clip_regrets();
  }

  struct ComboValues {
    std::vector<double> ev;
    std::vector<double> br;
  };

  ComboValues combo_values(std::size_t seat) {
    build_sigma();
    std::array<std::vector<double>, 2> pi_ev{std::vector<double>(kC, 1.0),
                                             std::vector<double>(kC, 1.0)};
    const std::size_t ev = eval(seat, /*br=*/false, tree_->root_index(), pi_ev);
    std::array<std::vector<double>, 2> pi_br{std::vector<double>(kC, 1.0),
                                             std::vector<double>(kC, 1.0)};
    const std::size_t br = eval(seat, /*br=*/true, tree_->root_index(), pi_br);
    ComboValues out{std::vector<double>(kC), std::vector<double>(kC)};
    for (std::uint32_t i = 0; i < kC; ++i) {
      out.ev[i] = vec(ev)[i] / static_cast<double>(kCompat);
      out.br[i] = vec(br)[i] / static_cast<double>(kCompat);
    }
    release(ev);
    release(br);
    return out;
  }

  double nash_conv_chips() {
    double raw = 0.0;
    for (std::size_t seat = 0; seat < 2; ++seat) {
      const ComboValues cv = combo_values(seat);
      for (std::uint32_t i = 0; i < kC; ++i)
        raw += (cv.br[i] - cv.ev[i]) * static_cast<double>(kCompat);
    }
    return raw / kNorm;
  }

  // Full forward reach per seat, mirroring the evaluator's mapped-edge
  // propagation order (a suppressed edge keeps the parent row at its
  // off-closure child; those rows are never consumed by node records).
  std::array<std::vector<std::vector<double>>, 2> forward_reach() {
    build_sigma();
    std::array<std::vector<std::vector<double>>, 2> out;
    for (std::size_t seat = 0; seat < 2; ++seat) {
      std::vector<std::vector<double>> reach(tree_->size());
      reach[tree_->root_index()].assign(kC, 1.0);
      for (std::size_t n = 0; n < tree_->size(); ++n) {
        if (reach[n].empty() || !tree_->node(n).is_action() || !reachable_[n])
          continue;
        const TreeNode& node = tree_->node(n);
        for (std::size_t s = 0; s < slot_edges_[n].size(); ++s) {
          const std::size_t edge = slot_edges_[n][s];
          std::vector<double> child_reach = reach[n];
          if (node.actor == seat)
            for (std::uint32_t i = 0; i < kC; ++i)
              child_reach[i] = reach[n][i] * avg(n, i, edge);
          reach[node.children[edge]] = std::move(child_reach);
        }
        // Off-closure children retain an unscaled parent row purely for
        // cross-instrument table-shape parity with the external reach
        // recomputation. These rows are never live reach: every consumer
        // skips kNoNode nodes, so nothing reads or propagates them.
        for (std::size_t edge = 0; edge < node.children.size(); ++edge) {
          if (std::find(slot_edges_[n].begin(), slot_edges_[n].end(), edge) != slot_edges_[n].end())
            continue;
          reach[node.children[edge]] = reach[n];
        }
      }
      out[seat] = std::move(reach);
    }
    return out;
  }

  // Allowed-only NashConv telescope per seat: full forward reach times the
  // local best-response gap, summed over mask-reachable own-seat nodes.
  std::array<double, 2> telescope() {
    build_sigma();
    const auto reach = forward_reach();
    std::array<double, 2> totals{0.0, 0.0};
    for (std::size_t viewer = 0; viewer < 2; ++viewer) {
      std::array<std::vector<double>, 2> pi{std::vector<double>(kC, 1.0),
                                            std::vector<double>(kC, 1.0)};
      const std::size_t id =
          gap_walk(viewer, tree_->root_index(), pi, reach[viewer], totals[viewer]);
      release(id);
    }
    return totals;
  }

  std::vector<std::vector<double>> snapshot_sigma() {
    build_sigma();
    std::vector<std::vector<double>> out(tree_->size());
    for (std::size_t n = 0; n < tree_->size(); ++n)
      if (sigma_[n] != nullptr)
        out[n] = *sigma_[n];
    return out;
  }

 private:
  using Reaches = std::array<std::vector<double>, 2>;

  void layout(const MixedMenuMask& mask) {
    actor_.assign(tree_->size(), 2);
    slot_edges_.assign(tree_->size(), {});
    reg_.assign(tree_->size(), nullptr);
    sum_.assign(tree_->size(), nullptr);
    sigma_.assign(tree_->size(), nullptr);
    action_nodes_ = 0;
    compact_slots_ = 0;
    reference_edges_ = 0;
    for (std::size_t n = 0; n < tree_->size(); ++n) {
      const TreeNode& node = tree_->node(n);
      if (node.is_action())
        reference_edges_ += node.actions.size();
      if (!node.is_action() || !reachable_[n])
        continue;
      ++action_nodes_;
      actor_[n] = static_cast<std::uint8_t>(node.actor);
      for (std::size_t a = 0; a < node.actions.size(); ++a)
        if (mask.allowed[n][a])
          slot_edges_[n].push_back(a);
      const std::size_t slots = slot_edges_[n].size();
      compact_slots_ += slots;
      reg_[n] = new std::vector<double>(kC * slots, 0.0);
      sum_[n] = new std::vector<double>(kC * slots, 0.0);
    }
    for (int k = 0; k < 64; ++k) {
      pool_.emplace_back(std::make_unique<std::vector<double>>(kC, 0.0));
      slots_.push_back(pool_.back().get());
      free_.push_back(slots_.size() - 1);
    }
    // Hot per-iteration storage is allocated ONLY on the mask-reachable
    // closure, indexed by the original tree node id. Off-closure nodes keep
    // empty vectors, so the working set and the per-iteration matvec scale
    // with the restricted game, not the full reference tree.
    reach_.clear();
    reach_.resize(tree_->size());
    value_.assign(tree_->size(), {});
    for (std::size_t n = 0; n < tree_->size(); ++n) {
      if (!reachable_[n])
        continue;
      reach_[n][0].assign(kC, 0.0);
      reach_[n][1].assign(kC, 0.0);
      value_[n].assign(kC, 0.0);
      const NodeKind kind = tree_->node(n).kind;
      if (kind == NodeKind::TerminalShowdown)
        showdown_nodes_.push_back(n);
      else if (kind == NodeKind::TerminalFold)
        fold_nodes_.push_back(n);
    }
  }

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
    fold0_.assign(tree_->size(), 0.0);
    stake_.assign(tree_->size(), 0.0);
    for (std::size_t n = 0; n < tree_->size(); ++n) {
      const TreeNode& node = tree_->node(n);
      if (!node.is_terminal())
        continue;
      const auto& pl = tree_->terminal(node.terminal);
      if (node.kind == NodeKind::TerminalFold)
        fold0_[n] = static_cast<double>(pl.chip_utility[0]);
      else
        stake_[n] = static_cast<double>(pl.seats[pl.live_order[0]].contributed);
    }
  }

  std::size_t acquire() {
    if (free_.empty()) {
      pool_.emplace_back(std::make_unique<std::vector<double>>(kC, 0.0));
      slots_.push_back(pool_.back().get());
      free_.push_back(slots_.size() - 1);
    }
    const std::size_t id = free_.back();
    free_.pop_back();
    return id;
  }
  void release(std::size_t id) { free_.push_back(id); }
  std::vector<double>& vec(std::size_t id) { return *slots_[id]; }

  // RM+ over compact allowed slots.
  double rm(std::size_t n, std::uint32_t i, std::size_t s) const {
    const std::size_t S = slot_edges_[n].size();
    double pos = 0.0;
    for (std::size_t k = 0; k < S; ++k) {
      const double r = (*reg_[n])[i * S + k];
      if (r > 0.0)
        pos += r;
    }
    if (pos <= 0.0)
      return 1.0 / static_cast<double>(S);
    const double r = (*reg_[n])[i * S + s];
    return r > 0.0 ? r / pos : 0.0;
  }

  void clip_regrets() {
    for (std::size_t n = 0; n < tree_->size(); ++n)
      if (reg_[n] != nullptr)
        for (double& r : *reg_[n])
          if (r < 0.0)
            r = 0.0;
  }

  // Build reference-width average rows with exact zero bits on suppressed
  // edges; tot==0 combos fall back to the uniform 1/|allowed| distribution.
  void build_sigma() {
    for (std::vector<double>* p : sigma_)
      delete p;
    sigma_.assign(tree_->size(), nullptr);
    for (std::size_t n = 0; n < tree_->size(); ++n) {
      if (reg_[n] == nullptr)
        continue;
      const std::size_t S = slot_edges_[n].size();
      const std::size_t width = tree_->node(n).actions.size();
      auto* s = new std::vector<double>(kC * width, 0.0);
      for (std::uint32_t i = 0; i < kC; ++i) {
        double tot = 0.0;
        for (std::size_t k = 0; k < S; ++k)
          tot += (*sum_[n])[i * S + k];
        for (std::size_t k = 0; k < S; ++k) {
          const double pr = tot > 0.0 ? (*sum_[n])[i * S + k] / tot : 1.0 / static_cast<double>(S);
          (*s)[i * width + slot_edges_[n][k]] = pr;
        }
      }
      sigma_[n] = s;
    }
  }

  double avg(std::size_t n, std::uint32_t i, std::size_t edge) const {
    const std::size_t width = tree_->node(n).actions.size();
    return sigma_[n] == nullptr ? 0.0 : (*sigma_[n])[i * width + edge];
  }

  void run_traversal(std::size_t trav) {
    const std::size_t opp = trav ^ 1;
    const std::size_t root = tree_->root_index();
    for (std::uint32_t i = 0; i < kC; ++i) {
      reach_[root][0][i] = 1.0;
      reach_[root][1][i] = 1.0;
    }
    for (std::size_t n = 0; n < tree_->size(); ++n) {
      if (!tree_->node(n).is_action() || !reachable_[n])
        continue;
      const std::size_t actor = actor_[n];
      const TreeNode& node = tree_->node(n);
      for (std::size_t edge : slot_edges_[n]) {
        const std::size_t c = node.children[edge];
        reach_[c][0] = reach_[n][0];
        reach_[c][1] = reach_[n][1];
        if (actor != trav) {
          const std::size_t s = static_cast<std::size_t>(
              std::find(slot_edges_[n].begin(), slot_edges_[n].end(), edge) -
              slot_edges_[n].begin());
          for (std::uint32_t i = 0; i < kC; ++i)
            reach_[c][actor][i] = reach_[n][actor][i] * rm(n, i, s);
        }
      }
    }
    for (std::size_t n : fold_nodes_) {
      const std::vector<double>& r = reach_[n][opp];
      const double c = trav == 0 ? fold0_[n] : -fold0_[n];
      double total = 0.0;
      for (std::uint32_t j = 0; j < kC; ++j)
        total += r[j];
      std::vector<double>& out = value_[n];
      for (std::uint32_t i = 0; i < kC; ++i)
        out[i] = c * total;
    }
    workers_->parallel_for(kC, [&](std::uint32_t i) {
      for (std::size_t n : showdown_nodes_) {
        const std::vector<double>& r = reach_[n][opp];
        const double stake = stake_[n];
        double acc = 0.0;
        if (trav == 0) {
          const std::int8_t* row =
              reinterpret_cast<const std::int8_t*>(a0_.data()) + static_cast<std::size_t>(i) * kC;
          for (std::uint32_t j = 0; j < kC; ++j)
            acc += static_cast<double>(row[j]) * r[j];
        } else {
          for (std::uint32_t j = 0; j < kC; ++j)
            acc -= static_cast<double>(a0_[static_cast<std::size_t>(j) * kC + i]) * r[j];
        }
        value_[n][i] = stake * acc;
      }
    });
    for (std::size_t k = tree_->size(); k-- > 0;) {
      const std::size_t n = k;
      if (!tree_->node(n).is_action() || !reachable_[n])
        continue;
      const std::size_t actor = actor_[n];
      const TreeNode& node = tree_->node(n);
      std::vector<double>& V = value_[n];
      if (actor == trav) {
        const std::vector<double>& own_reach = reach_[n][trav];
        for (std::uint32_t i = 0; i < kC; ++i) {
          double ev = 0.0;
          for (std::size_t s = 0; s < slot_edges_[n].size(); ++s)
            ev += rm(n, i, s) * value_[node.children[slot_edges_[n][s]]][i];
          V[i] = ev;
          const double w = avg_weight_[trav] * own_reach[i];
          for (std::size_t s = 0; s < slot_edges_[n].size(); ++s) {
            (*reg_[n])[i * slot_edges_[n].size() + s] +=
                value_[node.children[slot_edges_[n][s]]][i] - ev;
            (*sum_[n])[i * slot_edges_[n].size() + s] += w * rm(n, i, s);
          }
        }
      } else {
        for (std::uint32_t i = 0; i < kC; ++i) {
          double acc = 0.0;
          for (std::size_t edge : slot_edges_[n])
            acc += value_[node.children[edge]][i];
          V[i] = acc;
        }
      }
    }
  }

  std::size_t terminal_value(std::size_t n, std::size_t viewer, const Reaches& pi) {
    const std::size_t opp = viewer ^ 1;
    const std::size_t id = acquire();
    std::vector<double>& out = vec(id);
    const TreeNode& node = tree_->node(n);
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

  // Post-solve evaluation over allowed slots only; br=true best-responds at
  // the viewer's own nodes.
  std::size_t eval(std::size_t viewer, bool br, std::size_t n, Reaches& pi) {
    const TreeNode& node = tree_->node(n);
    if (node.is_terminal())
      return terminal_value(n, viewer, pi);
    if (node.is_chance())
      throw std::runtime_error("mixed_menu: chance node in river-root tree");
    const std::size_t actor = actor_[n];
    const std::size_t out = acquire();
    std::fill(vec(out).begin(), vec(out).end(), 0.0);

    if (actor == viewer && br) {
      std::vector<std::size_t> kids;
      kids.reserve(slot_edges_[n].size());
      for (std::size_t edge : slot_edges_[n])
        kids.push_back(eval(viewer, br, node.children[edge], pi));
      for (std::uint32_t i = 0; i < kC; ++i) {
        double best = vec(kids[0])[i];
        for (std::size_t k = 1; k < kids.size(); ++k)
          best = std::max(best, vec(kids[k])[i]);
        vec(out)[i] = best;
      }
      for (std::size_t id : kids)
        release(id);
      return out;
    }

    if (actor == viewer) {
      std::vector<std::size_t> kids;
      kids.reserve(slot_edges_[n].size());
      for (std::size_t edge : slot_edges_[n])
        kids.push_back(eval(viewer, br, node.children[edge], pi));
      for (std::uint32_t i = 0; i < kC; ++i)
        for (std::size_t k = 0; k < kids.size(); ++k)
          vec(out)[i] += avg(n, i, slot_edges_[n][k]) * vec(kids[k])[i];
      for (std::size_t id : kids)
        release(id);
      return out;
    }

    const std::vector<double> saved = pi[actor];
    for (std::size_t edge : slot_edges_[n]) {
      for (std::uint32_t j = 0; j < kC; ++j)
        pi[actor][j] = saved[j] * avg(n, j, edge);
      const std::size_t child = eval(viewer, br, node.children[edge], pi);
      for (std::uint32_t i = 0; i < kC; ++i)
        vec(out)[i] += vec(child)[i];
      release(child);
      pi[actor] = saved;
    }
    return out;
  }

  // Allowed-only gap walk accumulating the NashConv telescope for viewer.
  std::size_t gap_walk(std::size_t viewer, std::size_t n, Reaches& pi,
                       const std::vector<std::vector<double>>& full_reach, double& acc) {
    const TreeNode& node = tree_->node(n);
    if (node.is_terminal())
      return terminal_value(n, viewer, pi);
    if (node.is_chance())
      throw std::runtime_error("mixed_menu: chance node in river-root tree");
    const std::size_t out = acquire();
    std::fill(vec(out).begin(), vec(out).end(), 0.0);

    if (node.actor == viewer) {
      std::vector<std::size_t> kids;
      kids.reserve(slot_edges_[n].size());
      for (std::size_t edge : slot_edges_[n])
        kids.push_back(gap_walk(viewer, node.children[edge], pi, full_reach, acc));
      for (std::uint32_t i = 0; i < kC; ++i) {
        double best = vec(kids[0])[i];
        double policy = 0.0;
        for (std::size_t k = 0; k < kids.size(); ++k) {
          best = std::max(best, vec(kids[k])[i]);
          policy += avg(n, i, slot_edges_[n][k]) * vec(kids[k])[i];
        }
        const double gap = best - policy;
        acc += full_reach[n][i] * gap;
        vec(out)[i] = best;  // BR walk: deeper own-node deviations stay folded in
      }
      for (std::size_t id : kids)
        release(id);
      return out;
    }

    const std::vector<double> saved = pi[node.actor];
    for (std::size_t edge : slot_edges_[n]) {
      for (std::uint32_t j = 0; j < kC; ++j)
        pi[node.actor][j] = saved[j] * avg(n, j, edge);
      const std::size_t child = gap_walk(viewer, node.children[edge], pi, full_reach, acc);
      for (std::uint32_t i = 0; i < kC; ++i)
        vec(out)[i] += vec(child)[i];
      release(child);
      pi[node.actor] = saved;
    }
    return out;
  }

  GameDef def_;
  std::unique_ptr<AbstractTree> tree_;
  std::unique_ptr<Pool> workers_;
  std::vector<bool> reachable_;
  std::vector<std::array<int, 2>> holdings_;
  std::vector<std::int8_t> a0_;
  std::vector<double> fold0_;
  std::vector<double> stake_;
  std::vector<std::uint8_t> actor_;
  std::vector<std::vector<std::size_t>> slot_edges_;
  std::vector<std::vector<double>*> reg_;
  std::vector<std::vector<double>*> sum_;
  std::vector<std::vector<double>*> sigma_;
  std::size_t action_nodes_ = 0;
  std::size_t compact_slots_ = 0;
  std::size_t reference_edges_ = 0;
  std::array<double, 2> avg_weight_{1.0, 1.0};
  std::vector<std::array<std::vector<double>, 2>> reach_;
  std::vector<std::vector<double>> value_;
  std::vector<std::size_t> showdown_nodes_;
  std::vector<std::size_t> fold_nodes_;
  std::vector<std::unique_ptr<std::vector<double>>> pool_;
  std::vector<std::vector<double>*> slots_;
  std::vector<std::size_t> free_;
};

}  // namespace

void validate_mask(const AbstractTree& reference, const MixedMenuMask& mask) {
  if (mask.allowed.size() != reference.size())
    throw std::invalid_argument("mixed_menu: mask node count does not match the reference tree");
  for (std::size_t n = 0; n < reference.size(); ++n) {
    const TreeNode& node = reference.node(n);
    if (!node.is_action()) {
      if (!mask.allowed[n].empty())
        throw std::invalid_argument("mixed_menu: non-action node carries a mask row");
      continue;
    }
    if (mask.allowed[n].size() != node.actions.size())
      throw std::invalid_argument("mixed_menu: mask row width does not match the node menu");
  }
  const std::vector<bool> reachable = reachable_closure(reference, mask.allowed);
  for (std::size_t n = 0; n < reference.size(); ++n) {
    if (!reference.node(n).is_action() || !reachable[n])
      continue;
    const TreeNode& node = reference.node(n);
    if (std::none_of(mask.allowed[n].begin(), mask.allowed[n].end(),
                     [](bool allowed) { return allowed; }))
      throw std::invalid_argument("mixed_menu: reachable action node has an empty allowed set");
    for (std::size_t a = 0; a < node.actions.size(); ++a) {
      if (!mask.allowed[n][a] && !is_aggressive(node.actions[a].type))
        throw std::invalid_argument("mixed_menu: a passive legality edge cannot be suppressed");
    }
  }
}

void validate_mask_subset_map(const AbstractTree& reference, const SubsetMap& map) {
  if (map.node_map.size() != reference.size() || map.edge_map.size() != reference.size())
    throw std::invalid_argument("mixed_menu: subset map size does not match the reference tree");
  const std::size_t root = reference.root_index();
  if (map.node_map[root] == SubsetMap::kNoNode)
    throw std::invalid_argument("mixed_menu: root must stay mapped");
  for (std::size_t n = 0; n < reference.size(); ++n) {
    const TreeNode& node = reference.node(n);
    const bool mapped = map.node_map[n] != SubsetMap::kNoNode;
    if (mapped && map.node_map[n] != n)
      throw std::invalid_argument("mixed_menu: node map must be the identity bijection");
    if (!node.is_action()) {
      if (!map.edge_map[n].empty())
        throw std::invalid_argument("mixed_menu: non-action node carries an edge map");
      continue;
    }
    if (!mapped) {
      if (!map.edge_map[n].empty())
        throw std::invalid_argument("mixed_menu: unmapped node carries an edge map");
      continue;
    }
    if (map.edge_map[n].size() != node.actions.size())
      throw std::invalid_argument("mixed_menu: edge map width does not match the node menu");
    bool has_allowed = false;
    for (std::size_t a = 0; a < node.actions.size(); ++a) {
      const int edge = map.edge_map[n][a];
      if (edge == -1) {
        if (!is_aggressive(node.actions[a].type))
          throw std::invalid_argument("mixed_menu: suppressed edge must be aggressive");
        continue;
      }
      if (edge != static_cast<int>(a))
        throw std::invalid_argument("mixed_menu: mapped edge must use the self index");
      has_allowed = true;
    }
    if (!has_allowed)
      throw std::invalid_argument("mixed_menu: mapped action node has no allowed edge");
  }
  // Allowed-edge child closedness: the mapped set must equal the root DFS
  // closure, suppressed edges lead to unmapped children and allowed edges to
  // mapped children.
  std::vector<bool> closure(reference.size(), false);
  closure[root] = true;
  for (std::size_t n = 0; n < reference.size(); ++n) {
    if (!closure[n] || !reference.node(n).is_action())
      continue;
    const TreeNode& node = reference.node(n);
    for (std::size_t a = 0; a < node.children.size(); ++a) {
      const std::size_t child = node.children[a];
      if (map.edge_map[n][a] == -1) {
        if (map.node_map[child] != SubsetMap::kNoNode)
          throw std::invalid_argument("mixed_menu: suppressed edge reaches a mapped child");
        continue;
      }
      if (map.node_map[child] == SubsetMap::kNoNode)
        throw std::invalid_argument("mixed_menu: allowed edge reaches an unmapped child");
      closure[child] = true;
    }
  }
  for (std::size_t n = 0; n < reference.size(); ++n)
    if (closure[n] != (map.node_map[n] != SubsetMap::kNoNode))
      throw std::invalid_argument("mixed_menu: mapped set is not the allowed-edge DFS closure");
}

SubsetMap mask_to_subset_map(const AbstractTree& reference, const MixedMenuMask& mask) {
  validate_mask(reference, mask);
  const std::vector<bool> reachable = reachable_closure(reference, mask.allowed);
  SubsetMap map;
  map.node_map.assign(reference.size(), SubsetMap::kNoNode);
  map.edge_map.assign(reference.size(), {});
  for (std::size_t n = 0; n < reference.size(); ++n) {
    if (!reachable[n])
      continue;
    map.node_map[n] = n;
    if (!reference.node(n).is_action())
      continue;
    const std::size_t width = reference.node(n).actions.size();
    map.edge_map[n].reserve(width);
    for (std::size_t a = 0; a < width; ++a)
      map.edge_map[n].push_back(mask.allowed[n][a] ? static_cast<int>(a) : -1);
  }
  validate_mask_subset_map(reference, map);
  return map;
}

MixedMenuMask make_root_only_widening_mask(const AbstractTree& reference,
                                           const AbstractTree& coarse) {
  // The D184 mapper supplies the baseline coarse allowed sets; only the root
  // and the newly opened reference-only subtrees differ from it. Every
  // action node gets a full-width row; off-closure nodes stay all-false.
  const SubsetMap base = map_bounded_subset(reference, coarse);
  MixedMenuMask mask;
  mask.allowed.assign(reference.size(), {});
  for (std::size_t n = 0; n < reference.size(); ++n)
    if (reference.node(n).is_action())
      mask.allowed[n].assign(reference.node(n).actions.size(), false);
  std::vector<bool> reachable(reference.size(), false);
  const std::size_t root = reference.root_index();
  reachable[root] = true;
  for (std::size_t n = 0; n < reference.size(); ++n) {
    if (!reachable[n] || !reference.node(n).is_action())
      continue;
    const TreeNode& node = reference.node(n);
    std::vector<bool> row(node.actions.size(), false);
    if (n == root || base.node_map[n] == SubsetMap::kNoNode) {
      // Widened root, or reference-only territory opened through a widened
      // ancestor edge: the full cap-0 menu is legal there.
      std::fill(row.begin(), row.end(), true);
    } else {
      for (std::size_t a = 0; a < node.actions.size(); ++a)
        row[a] = base.edge_map[n][a] != -1;
    }
    mask.allowed[n] = std::move(row);
    for (std::size_t a = 0; a < node.children.size(); ++a)
      if (mask.allowed[n][a])
        reachable[node.children[a]] = true;
  }
  validate_mask(reference, mask);
  return mask;
}

MixedMenuSolveResult solve_mixed_menu(const GameDef& def, const ActionAbstraction& reference_action,
                                      const MixedMenuMask& mask, std::uint64_t iterations,
                                      std::size_t threads) {
  MixedMenuGame game(def, reference_action, mask, threads);
  for (std::uint64_t t = 1; t <= iterations; ++t)
    game.iterate_weighted(static_cast<double>(t));  // linear CFR+ average

  MixedMenuSolveResult out;
  out.river.iterations = iterations;
  out.river.action_nodes = game.reachable_action_nodes();
  out.river.combos_per_seat = kC;
  const double nc_chips = game.nash_conv_chips();
  const double bb = def.big_blind > 0 ? static_cast<double>(def.big_blind) : 1.0;
  out.river.nash_conv_chips = nc_chips;
  out.river.nash_conv_bb = nc_chips / bb;
  out.river.nash_conv_pot = def.pot > 0 ? nc_chips / static_cast<double>(def.pot) : nc_chips;
  auto cv0 = game.combo_values(0);
  auto cv1 = game.combo_values(1);
  out.river.root_cfv_seat0 = std::move(cv0.ev);
  out.river.root_br_seat0 = std::move(cv0.br);
  out.river.root_cfv_seat1 = std::move(cv1.ev);
  out.river.root_br_seat1 = std::move(cv1.br);
  out.river.avg_sigma = game.snapshot_sigma();
  out.forward_reach = game.forward_reach();
  out.telescope_raw_chips = game.telescope();
  out.compact_slot_total = game.compact_slot_total();
  out.reference_edge_total = game.reference_edge_total();
  return out;
}

// D208: deterministic public-depth-ordered sparse multi-layer widening. The
// restricted-node aggressive choice is the sorted first aggressive edge; all W
// selection and closure machinery is shared via build_sparse_mask.
MixedMenuMask make_sparse_multilayer_mask(const AbstractTree& reference, const AbstractTree& coarse,
                                          std::size_t widened_budget) {
  const SubsetMap base = map_bounded_subset(reference, coarse);
  return build_sparse_mask(
      reference, base, widened_budget,
      [](const TreeNode& node, const PublicState&) { return first_aggressive_choice(node); });
}

// D217: identical W machinery; only the restricted-node chooser changes to a
// canonical pot-sized nearest-legal selection over existing reference actions.
MixedMenuMask make_pot_fraction_mask(const AbstractTree& reference, const AbstractTree& coarse,
                                     std::size_t widened_budget) {
  const SubsetMap base = map_bounded_subset(reference, coarse);
  return build_sparse_mask(reference, base, widened_budget,
                           [](const TreeNode& node, const PublicState& state) {
                             return pot_fraction_choice(node, state);
                           });
}

}  // namespace bs::rfc0047
