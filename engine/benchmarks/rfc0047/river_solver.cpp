#include "river_solver.hpp"

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

using bs::poker::GameDef;
using bs::poker::GameState;
using bs::poker::TerminalDepth;
using bs::tree::AbstractTree;
using bs::tree::NodeKind;
using bs::tree::TreeNode;

namespace {

constexpr std::uint32_t kC = 1081;      // C(47,2) own combos on a river board
constexpr std::uint32_t kCompat = 990;  // C(45,2) compatible opponent combos
constexpr double kNorm = static_cast<double>(kC) * kCompat;

// Deterministic full-deal zero-sum cap-1 river solver (V3 kernel). Every
// traversal enumerates every compatible joint deal; no RNG. Terminal
// showdown payoff is one dense signed matrix matvec (the win/tie matrix is
// board-fixed and shared by every showdown leaf; only the stake differs);
// fold payoff is combo-independent. Per-iteration work uses a reusable vector
// pool and in-place opponent-reach scaling (zero heap allocation in the loop).
class RiverGame {
 public:
  explicit RiverGame(
      const GameDef& d, std::size_t threads, std::uint8_t action_cap,
      const std::optional<bs::abstraction::ActionAbstraction>& explicit_action = std::nullopt,
      bool rm_plus = true)
      : def_(d), rm_plus_(rm_plus) {
    if (def_.player_count != 2 || def_.board_size != 5 || def_.terminal != TerminalDepth::River)
      throw std::invalid_argument("river_solver: needs a 2-seat five-card river-root def");
    workers_ = std::make_unique<Pool>(threads);
    std::array<int, 5> bd{};
    for (int i = 0; i < 5; ++i)
      bd[i] = def_.board[i];
    // Single menu construction site: an explicit declared/grid abstraction is
    // the RFC 0048 Stage-2b union-reference path; nullopt reproduces the
    // identity(action_cap) tree every existing numeric path already solves.
    const bs::abstraction::ActionAbstraction action =
        explicit_action.value_or(bs::abstraction::ActionAbstraction::identity(action_cap));
    tree_ = std::make_unique<AbstractTree>(d, action);
    holdings_.resize(kC);
    for (std::uint32_t x = 0; x < kC; ++x)
      holdings_[x] = holding_for_dense(std::span<const int>(bd.data(), 5), x);
    precompute(bd);
    layout();
  }

  const AbstractTree& tree() const { return *tree_; }
  std::size_t action_node_count() const { return action_nodes_; }

  // Snapshot the built average strategy keyed by tree node (empty at
  // non-action nodes). Benchmark-only fixed-policy consumers read this instead
  // of touching the internal regret/sum tables.
  std::vector<std::vector<double>> snapshot_sigma() {
    build_sigma();
    std::vector<std::vector<double>> out(tree_->size());
    for (std::size_t n = 0; n < tree_->size(); ++n)
      if (sigma_[n] != nullptr)
        out[n] = *sigma_[n];
    return out;
  }

  // Releases the per-node regret/sum/snapshot tables. The class already owns
  // unique_ptr members (tree, worker pool) and is therefore non-copyable, so a
  // destructor introduces no slicing or double-free risk. Freeing these makes
  // repeated in-process benchmark solves (the cap-k materiality sweep) retain a
  // bounded resident footprint instead of leaking every game's tables.
  ~RiverGame() {
    for (std::vector<double>* p : reg_)
      delete p;
    for (std::vector<double>* p : sum_)
      delete p;
    for (std::vector<double>* p : sigma_)
      delete p;
  }

  // CFR traversal via explicit reach / fused terminal eval / backprop passes.
  // Restructuring the recursive walk this way lets every showdown-leaf matvec
  // run inside ONE fork-join region per traversal (one barrier, not one per
  // leaf). All per-combo reductions keep their exact serial operation order,
  // so the produced regrets/average strategy are bitwise identical to the
  // recursive kernel at every thread count.
  void iterate_weighted(double weight) {
    avg_weight_[0] = weight;
    avg_weight_[1] = weight;
    run_traversal(0);
    run_traversal(1);
    // D251: skip post-update clipping for vanilla RM; rm() still reads only
    // the positive part, so strategy legality is unchanged either way.
    if (rm_plus_)
      clip_regrets();
  }

  void run_traversal(std::size_t trav) {
    const std::size_t opp = trav ^ 1;
    const std::size_t root = tree_->root_index();
    for (std::uint32_t i = 0; i < kC; ++i) {
      reach_[root][0][i] = 1.0;
      reach_[root][1][i] = 1.0;
    }
    // Top-down reach propagation (DFS build guarantees parent index < child).
    // Only the acting (opponent) player's reach is scaled by its RM+ strategy;
    // the traverser's own counterfactual reach is left unchanged, matching the
    // recursive counterfactual form.
    for (std::size_t n = 0; n < tree_->size(); ++n) {
      if (!tree_->node(n).is_action())
        continue;
      const std::size_t actor = actor_[n];
      const std::size_t A = menu_[n];
      const std::vector<std::size_t>& kids = tree_->node(n).children;
      for (std::size_t a = 0; a < A; ++a) {
        const std::size_t c = kids[a];
        reach_[c][0] = reach_[n][0];
        reach_[c][1] = reach_[n][1];
        if (actor != trav)
          for (std::uint32_t i = 0; i < kC; ++i)
            reach_[c][actor][i] = reach_[n][actor][i] * rm(n, i, a);
      }
    }
    // Fold terminals: serial O(kC), combo-independent payoff.
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
    // ALL showdown matvecs in a single fork-join region. Row task i fills each
    // leaf's i-th value with a serial j-reduction; distinct leaves write
    // distinct vectors, so tasks never share writable state.
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
    // Bottom-up backprop and RM+ regret / linear-average updates.
    for (std::size_t k = tree_->size(); k-- > 0;) {
      const std::size_t n = k;
      if (!tree_->node(n).is_action())
        continue;
      const std::size_t actor = actor_[n];
      const std::size_t A = menu_[n];
      const std::vector<std::size_t>& kids = tree_->node(n).children;
      std::vector<double>& V = value_[n];
      if (actor == trav) {
        const std::vector<double>& own_reach = reach_[n][trav];
        for (std::uint32_t i = 0; i < kC; ++i) {
          double ev = 0.0;
          for (std::size_t a = 0; a < A; ++a)
            ev += rm(n, i, a) * value_[kids[a]][i];
          V[i] = ev;
          const double w = avg_weight_[trav] * own_reach[i];
          for (std::size_t a = 0; a < A; ++a) {
            (*reg_[n])[i * A + a] += value_[kids[a]][i] - ev;
            (*sum_[n])[i * A + a] += w * rm(n, i, a);
          }
        }
      } else {
        for (std::uint32_t i = 0; i < kC; ++i) {
          double acc = 0.0;
          for (std::size_t a = 0; a < A; ++a)
            acc += value_[kids[a]][i];
          V[i] = acc;
        }
      }
    }
  }

  // Per-combo equilibrium-game value (both average) and best-response value
  // (this seat best-responds), each normalized per compatible opponent deal.
  struct ComboValues {
    std::vector<double> ev;
    std::vector<double> br;
  };

  ComboValues combo_values(std::size_t seat) {
    build_sigma();
    std::array<std::vector<double>, 2> pi{std::vector<double>(kC, 1.0),
                                          std::vector<double>(kC, 1.0)};
    GameState ev_state(def_);
    const std::size_t ev = eval(seat, /*br=*/false, ev_state, tree_->root_index(), pi);
    std::array<std::vector<double>, 2> pi2{std::vector<double>(kC, 1.0),
                                           std::vector<double>(kC, 1.0)};
    GameState br_state(def_);
    const std::size_t br = eval(seat, /*br=*/true, br_state, tree_->root_index(), pi2);
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

  // Seat-0 payoff for EVERY joint private pair (i,j) under the average
  // strategy, raw: M[i*kC+j] = expected u0 if holdings i,j play the average
  // profile. For a physically incompatible pair (shared hole card) the value
  // is the fold-path payoff only (showdown is zeroed by the signed matrix),
  // matching the kernel's terminal-value reach convention; the TURN solver
  // filters these pairs with its own combo mask. A row summed over all j equals
  // the equilibrium root CFV times the 990-deal normalization.
  std::vector<double> equilibrium_deal_matrix() {
    build_sigma();
    std::vector<double> m(static_cast<std::size_t>(kC) * kC, 0.0);
    workers_->parallel_for(kC, [&](std::uint32_t i) {
      for (std::uint32_t j = 0; j < kC; ++j)
        m[static_cast<std::size_t>(i) * kC + j] = scalar_deal(tree_->root_index(), i, j);
    });
    return m;
  }

 private:
  // Simple, allocation-per-call fixed-profile expected u0 for one deal (i,j).
  // Payoff is history-independent; each node only needs who acts (not state).
  double scalar_deal(std::size_t n, std::uint32_t i, std::uint32_t j) {
    const TreeNode& node = tree_->node(n);
    if (node.kind == NodeKind::TerminalFold)
      return fold0_[n];
    if (node.is_terminal())
      return stake_[n] * static_cast<double>(a0_[static_cast<std::size_t>(i) * kC + j]);
    if (node.is_chance())
      throw std::runtime_error("river_solver: chance node in river-root tree");
    const std::uint32_t hand = node.actor == 0 ? i : j;
    const std::size_t A = menu_[n];
    double v = 0.0;
    for (std::size_t a = 0; a < A; ++a)
      v += (*sigma_[n])[hand * A + a] * scalar_deal(node.children[a], i, j);
    return v;
  }

  using Reaches = std::array<std::vector<double>, 2>;

  void precompute(const std::array<int, 5>& bd) {
    // Signed seat-0 showdown matrix A0[i*kC+j]: +1 win, -1 loss, 0 tie or
    // incompatible. Seat-1 payoff uses -A0^T.
    a0_.assign(static_cast<std::size_t>(kC) * kC, 0);
    std::array<bool, 52> used{};
    for (int c : bd)
      used[c] = true;
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
    (void)used;

    // Per-terminal fold constant (seat-0 net chips) and showdown stake.
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

  void layout() {
    actor_.assign(tree_->size(), 2);
    menu_.assign(tree_->size(), 0);
    reg_.assign(tree_->size(), nullptr);
    sum_.assign(tree_->size(), nullptr);
    action_nodes_ = 0;
    for (std::size_t n = 0; n < tree_->size(); ++n) {
      const TreeNode& node = tree_->node(n);
      if (!node.is_action())
        continue;
      ++action_nodes_;
      actor_[n] = static_cast<std::uint8_t>(node.actor);
      menu_[n] = node.actions.size();
      reg_[n] = new std::vector<double>(kC * menu_[n], 0.0);
      sum_[n] = new std::vector<double>(kC * menu_[n], 0.0);
    }
    // Pre-warm the scratch pool generously (vectors reused by the post-solve
    // eval walks; the hot CFR traversal uses the preallocated reach_/value_).
    for (int k = 0; k < 64; ++k) {
      pool_.emplace_back(std::make_unique<std::vector<double>>(kC, 0.0));
      slots_.push_back(pool_.back().get());
      free_.push_back(slots_.size() - 1);
    }
    // Hot-traversal storage: per-node two-seat reach vectors and per-node value
    // vectors, plus the fixed sets of terminal leaves (the full tree is walked
    // every traversal, so these sets are constant).
    reach_.clear();
    reach_.resize(tree_->size());
    for (auto& ra : reach_) {
      ra[0].assign(kC, 0.0);
      ra[1].assign(kC, 0.0);
    }
    value_.assign(tree_->size(), std::vector<double>(kC, 0.0));
    showdown_nodes_.clear();
    fold_nodes_.clear();
    for (std::size_t n = 0; n < tree_->size(); ++n) {
      const TreeNode& node = tree_->node(n);
      if (node.kind == NodeKind::TerminalShowdown)
        showdown_nodes_.push_back(n);
      else if (node.kind == NodeKind::TerminalFold)
        fold_nodes_.push_back(n);
    }
  }

  std::vector<double>& vec(std::size_t id) { return *slots_[id]; }

  std::size_t acquire() {
    if (free_.empty()) {
      pool_.emplace_back(std::make_unique<std::vector<double>>(kC, 0.0));
      slots_.push_back(pool_.back().get());
      free_.push_back(slots_.size() - 1);
    }
    std::size_t id = free_.back();
    free_.pop_back();
    return id;
  }
  void release(std::size_t id) { free_.push_back(id); }

  double rm(std::size_t n, std::uint32_t i, std::size_t a) const {
    const std::size_t A = menu_[n];
    double pos = 0.0;
    for (std::size_t k = 0; k < A; ++k) {
      const double r = (*reg_[n])[i * A + k];
      if (r > 0.0)
        pos += r;
    }
    if (pos <= 0.0)
      return 1.0 / static_cast<double>(A);
    const double r = (*reg_[n])[i * A + a];
    return r > 0.0 ? r / pos : 0.0;
  }

  void clip_regrets() {
    for (std::size_t n = 0; n < tree_->size(); ++n)
      if (reg_[n] != nullptr)
        for (double& r : *reg_[n])
          if (r < 0.0)
            r = 0.0;
  }

  void build_sigma() {
    // build_sigma runs more than once per game (each eval walk rebuilds the
    // average-strategy snapshot); release the previous snapshot instead of
    // leaking its vectors so repeated in-process solves keep a bounded footprint.
    for (std::vector<double>* p : sigma_)
      delete p;
    sigma_.assign(tree_->size(), nullptr);
    for (std::size_t n = 0; n < tree_->size(); ++n) {
      if (menu_[n] == 0)
        continue;
      auto* s = new std::vector<double>(kC * menu_[n], 0.0);
      for (std::uint32_t i = 0; i < kC; ++i) {
        double tot = 0.0;
        for (std::size_t a = 0; a < menu_[n]; ++a)
          tot += (*sum_[n])[i * menu_[n] + a];
        for (std::size_t a = 0; a < menu_[n]; ++a)
          (*s)[i * menu_[n] + a] =
              tot > 0.0 ? (*sum_[n])[i * menu_[n] + a] / tot : 1.0 / static_cast<double>(menu_[n]);
      }
      sigma_[n] = s;
    }
  }

  double avg(std::size_t n, std::uint32_t i, std::size_t a) const {
    return sigma_[n] == nullptr ? 1.0 / static_cast<double>(menu_[n])
                                : (*sigma_[n])[i * menu_[n] + a];
  }

  // Terminal raw CFV per own combo (sums compatible opponent reach). Returns a
  // scratch id. SERIAL: this serves only the handful of post-solve eval walks;
  // the hot per-iteration traversal evaluates all showdown leaves in one fused
  // fork-join region via run_traversal instead.
  std::size_t terminal_value(std::size_t n, std::size_t viewer, const Reaches& pi) {
    const std::size_t opp = viewer ^ 1;
    const std::size_t id = acquire();
    std::vector<double>& out = vec(id);
    const std::vector<double>& r = pi[opp];
    const TreeNode& node = tree_->node(n);
    if (node.kind == NodeKind::TerminalFold) {
      const double c = viewer == 0 ? fold0_[n] : -fold0_[n];
      double total = 0.0;
      for (std::uint32_t j = 0; j < kC; ++j)
        total += r[j];
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
          acc += static_cast<double>(row[j]) * r[j];
        out[i] = stake * acc;
      }
    } else {
      for (std::uint32_t i = 0; i < kC; ++i) {
        double acc = 0.0;
        for (std::uint32_t j = 0; j < kC; ++j) {
          // own i (seat1) vs opp j (seat0): U1 = -A0[j,i].
          acc -= static_cast<double>(a0_[static_cast<std::size_t>(j) * kC + i]) * r[j];
        }
        out[i] = stake * acc;
      }
    }
    return id;
  }

  // Evaluation recursion for viewer: br=true best-responds at viewer nodes,
  // otherwise plays the built average strategy everywhere.
  std::size_t eval(std::size_t viewer, bool br, GameState& state, std::size_t n, Reaches& pi) {
    const TreeNode& node = tree_->node(n);
    if (node.is_terminal())
      return terminal_value(n, viewer, pi);
    if (node.is_chance())
      throw std::runtime_error("river_solver: chance node in river-root tree");
    const std::size_t actor = actor_[n];
    const std::size_t A = menu_[n];
    const std::size_t out = acquire();
    std::vector<double>& V = vec(out);
    std::fill(V.begin(), V.end(), 0.0);

    const bool own = actor == viewer;
    if (own && br) {
      std::vector<std::size_t> kids(A);
      for (std::size_t act = 0; act < A; ++act) {
        GameState cs = state.after_action(actor, node.actions[act]);
        kids[act] = eval(viewer, br, cs, node.children[act], pi);
      }
      for (std::uint32_t i = 0; i < kC; ++i) {
        double best = vec(kids[0])[i];
        for (std::size_t act = 1; act < A; ++act)
          best = std::max(best, vec(kids[act])[i]);
        V[i] = best;
      }
      for (std::size_t act = 0; act < A; ++act)
        release(kids[act]);
    } else if (own) {
      // Viewer's own node under the EQUILIBRIUM (average) strategy: children
      // are counterfactual (own reach excluded), so value = sum_a sigma(a)*V_a
      // weighted by this combo's average probability for each action.
      std::vector<std::size_t> kids(A);
      for (std::size_t act = 0; act < A; ++act) {
        GameState cs = state.after_action(actor, node.actions[act]);
        kids[act] = eval(viewer, br, cs, node.children[act], pi);
      }
      for (std::uint32_t i = 0; i < kC; ++i)
        for (std::size_t act = 0; act < A; ++act)
          V[i] += avg(n, i, act) * vec(kids[act])[i];
      for (std::size_t act = 0; act < A; ++act)
        release(kids[act]);
    } else {
      // Opponent node under its average strategy: scale opponent reach by
      // sigma (restored from a snapshot) and sum the children.
      const std::vector<double> saved = pi[actor];
      for (std::size_t act = 0; act < A; ++act) {
        for (std::uint32_t j = 0; j < kC; ++j)
          pi[actor][j] = saved[j] * avg(n, j, act);
        GameState cs = state.after_action(actor, node.actions[act]);
        const std::size_t child = eval(viewer, br, cs, node.children[act], pi);
        for (std::uint32_t i = 0; i < kC; ++i)
          V[i] += vec(child)[i];
        release(child);
        pi[actor] = saved;
      }
    }
    return out;
  }

  GameDef def_;
  bool rm_plus_;
  std::unique_ptr<AbstractTree> tree_;
  std::unique_ptr<Pool> workers_;
  std::vector<std::array<int, 2>> holdings_;
  std::vector<std::int8_t> a0_;  // dense signed seat-0 showdown matrix
  std::vector<double> fold0_;
  std::vector<double> stake_;
  std::vector<std::uint8_t> actor_;
  std::vector<std::size_t> menu_;
  std::vector<std::vector<double>*> reg_;
  std::vector<std::vector<double>*> sum_;
  std::vector<std::vector<double>*> sigma_;
  std::size_t action_nodes_ = 0;
  std::array<double, 2> avg_weight_{1.0, 1.0};
  // Hot-traversal per-node state (preallocated, no per-iteration allocation):
  // two-seat reach vectors and value vectors indexed by tree node, plus the
  // constant terminal-leaf sets the fused fork-join region iterates over.
  std::vector<std::array<std::vector<double>, 2>> reach_;
  std::vector<std::vector<double>> value_;
  std::vector<std::size_t> showdown_nodes_;
  std::vector<std::size_t> fold_nodes_;
  // Scratch vector pool (id -> pointer), used by the post-solve eval walks.
  std::vector<std::unique_ptr<std::vector<double>>> pool_;
  std::vector<std::vector<double>*> slots_;
  std::vector<std::size_t> free_;
};

}  // namespace

RiverSolveResult solve_river_subgame(const RiverSolveConfig& cfg,
                                     std::span<const std::uint64_t> checkpoints) {
  RiverGame game(cfg.def, cfg.threads, cfg.action_cap, cfg.action_abstraction, cfg.rm_plus);
  RiverSolveResult out;
  out.iterations = cfg.iterations;
  out.action_nodes = game.action_node_count();
  out.combos_per_seat = kC;

  std::size_t cp = 0;
  for (std::uint64_t t = 1; t <= cfg.iterations; ++t) {
    game.iterate_weighted(cfg.linear_averaging ? static_cast<double>(t) : 1.0);
    while (cp < checkpoints.size() && checkpoints[cp] <= t) {
      const double nc = game.nash_conv_chips() / (cfg.def.big_blind > 0 ? cfg.def.big_blind : 1);
      out.history_nash_conv_bb.push_back(nc);
      ++cp;
    }
  }
  const double ncChips = game.nash_conv_chips();
  out.nash_conv_chips = ncChips;
  out.nash_conv_bb = ncChips / (cfg.def.big_blind > 0 ? cfg.def.big_blind : 1);
  out.nash_conv_pot = cfg.def.pot > 0 ? ncChips / cfg.def.pot : ncChips;
  auto cv0 = game.combo_values(0);
  auto cv1 = game.combo_values(1);
  out.root_cfv_seat0 = std::move(cv0.ev);
  out.root_br_seat0 = std::move(cv0.br);
  out.root_cfv_seat1 = std::move(cv1.ev);
  out.root_br_seat1 = std::move(cv1.br);
  if (cfg.compute_deal_matrix)
    out.deal_value_seat0 = game.equilibrium_deal_matrix();
  out.avg_sigma = game.snapshot_sigma();
  return out;
}

ComboNashConv river_combo_values(const RiverSolveConfig& cfg, std::uint64_t iterations,
                                 bool linear_averaging) {
  RiverGame game(cfg.def, cfg.threads, cfg.action_cap, cfg.action_abstraction, cfg.rm_plus);
  for (std::uint64_t t = 1; t <= iterations; ++t)
    game.iterate_weighted(linear_averaging ? static_cast<double>(t) : 1.0);
  // RiverGame::ComboValues is exposed by the same deterministic eval walks;
  // the free function maps it into the public header's flat vectors (values
  // are mean chips per compatible opponent deal, normalized by 990).
  using Inner = decltype(game.combo_values(0));
  Inner c0 = game.combo_values(0);
  Inner c1 = game.combo_values(1);
  ComboNashConv out;
  out.seat0_ev = std::move(c0.ev);
  out.seat0_br = std::move(c0.br);
  out.seat1_ev = std::move(c1.ev);
  out.seat1_br = std::move(c1.br);
  return out;
}

}  // namespace bs::rfc0047
