#include "turn_solver.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <bs/abstraction.hpp>
#include <bs/eval.hpp>
#include <bs/game_definition.hpp>
#include <bs/heads_up_solver.hpp>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <vector>

#include "thread_pool.hpp"

namespace bs::rfc0047 {

namespace {

using bs::poker::GameDef;
using bs::poker::GameState;
using bs::poker::TerminalDepth;
using bs::tree::AbstractTree;
using bs::tree::NodeKind;
using bs::tree::TreeNode;
using Val = std::vector<double>;
using Reaches = std::array<Val, 2>;

constexpr double kTurnNorm = static_cast<double>(kTurnC) * static_cast<double>(kTurnCompat);

// Two-seat turn-rooted range-vectorized CFR with oracle-matrix river leaves.
class TurnGame {
 public:
  explicit TurnGame(const TurnSolveConfig& cfg) : cfg_(cfg), def_(cfg.def), pool_(cfg.threads) {
    if (def_.player_count != 2 || def_.board_size != 4 || def_.terminal != TerminalDepth::River)
      throw std::invalid_argument("turn_solver: needs a 2-seat four-card river-terminal def");
    tree_ = std::make_unique<AbstractTree>(
        def_, bs::abstraction::ActionAbstraction::identity(std::uint8_t(1)));
    for (std::uint32_t t = 0; t < kTurnC; ++t) {
      turn_holdings_.push_back(holding_for_dense(std::span<const int>(def_.board.data(), 4), t));
      turn_mask_.push_back((std::uint64_t(1) << turn_holdings_.back()[0]) |
                           (std::uint64_t(1) << turn_holdings_.back()[1]));
    }
    map_leaves();
    precompute_folds();
    layout();
  }

  // Releases the per-node regret/sum tables. The class already owns
  // non-copyable members (the worker Pool and the tree unique_ptr), so a
  // destructor introduces no slicing or double-free risk. Without it every
  // TurnGame leaks its layout() tables for the process lifetime, which
  // LeakSanitizer reports on the repeated in-process turn solves used by the
  // S3/G5 serve and G3 perturbation harness. Mirrors the RiverGame ownership
  // cleanup; the CFR algorithm, policy output, and numerics are untouched.
  ~TurnGame() {
    for (std::vector<double>* p : reg_)
      delete p;
    for (std::vector<double>* p : sum_)
      delete p;
  }

  void clip_regrets() {
    for (std::size_t n = 0; n < tree_->size(); ++n)
      if (reg_[n] != nullptr)
        for (double& r : *reg_[n])
          if (r < 0.0)
            r = 0.0;
  }

  void iterate_weighted(double weight) {
    for (int trav = 0; trav < 2; ++trav) {
      Reaches pi{Val(kTurnC, 1.0), Val(kTurnC, 1.0)};
      avg_weight_ = weight;
      GameState st(def_);
      walk(st, tree_->root_index(), trav, pi);
    }
    if (cfg_.rm_plus)
      clip_regrets();  // RM+: regret matching reads only positive regret
  }

  std::size_t turn_action_node_count() const { return action_nodes_; }
  std::size_t tree_node_count() const { return tree_->size(); }

  double nash_conv_chips() {
    build_sigma();
    // evaluate() returns RAW per-combo CFV (summed over the 1035 compatible
    // opponent deals, with chance averaged), so NashConv is the summed gap
    // divided by the number of joint deals kTurnC*kTurnCompat (NOT multiplied
    // by kTurnCompat again - unlike the river kernel whose combo_values are
    // pre-normalized per deal).
    double raw = 0.0;
    for (int viewer = 0; viewer < 2; ++viewer) {
      Val ro(kTurnC, 1.0);
      GameState st(def_);
      const Val ev = evaluate(viewer, false, st, tree_->root_index(), ro);
      Val ro2(kTurnC, 1.0);
      GameState st2(def_);
      const Val br = evaluate(viewer, true, st2, tree_->root_index(), ro2);
      for (std::uint32_t i = 0; i < kTurnC; ++i)
        raw += br[i] - ev[i];
    }
    return raw / kTurnNorm;
  }

  std::vector<std::vector<double>> average_sigma() {
    build_sigma();
    return sigma_;
  }

  // Independent per-combo root equilibrium value (ev, br=false) and responder
  // best response (br, br=true) for one seat, normalized per compatible
  // opponent deal (the raw evaluate CFV sums over kTurnCompat deals, so divide
  // once). Both walks read only the materialized average strategy and exact
  // leaf/settlement payoff, never the CFR regret/average tables.
  struct RootComboValues {
    Val ev;
    Val br;
  };
  RootComboValues root_combo_values(std::size_t viewer) {
    build_sigma();
    Val ro(kTurnC, 1.0);
    GameState st(def_);
    Val raw_ev = evaluate(viewer, false, st, tree_->root_index(), ro);
    Val ro2(kTurnC, 1.0);
    GameState st2(def_);
    Val raw_br = evaluate(viewer, true, st2, tree_->root_index(), ro2);
    RootComboValues out{Val(kTurnC, 0.0), Val(kTurnC, 0.0)};
    for (std::uint32_t i = 0; i < kTurnC; ++i) {
      out.ev[i] = raw_ev[i] / static_cast<double>(kTurnCompat);
      out.br[i] = raw_br[i] / static_cast<double>(kTurnCompat);
    }
    return out;
  }

  // Counterfactual reach per config leaf under the average strategy (see
  // TurnLeafReach in the header for the frozen formula).
  std::vector<TurnLeafReach> export_leaf_reaches() {
    build_sigma();
    std::vector<TurnLeafReach> out(cfg_.leaves.size());
    std::vector<ReachAcc> acc(cfg_.leaves.size());
    for (std::size_t k = 0; k < cfg_.leaves.size(); ++k) {
      out[k].child_node = cfg_.leaves[k].child_node;
      acc[k].rho = {Val(kRiverC, 0.0), Val(kRiverC, 0.0)};
    }
    Val p0(kTurnC, 1.0), p1(kTurnC, 1.0);
    GameState st(def_);
    reach_walk(st, tree_->root_index(), p0, p1, acc);
    for (std::size_t k = 0; k < cfg_.leaves.size(); ++k)
      for (int seat = 0; seat < 2; ++seat) {
        for (double& x : acc[k].rho[seat])
          x /= kTurnNorm;
        out[k].rho[seat] = std::move(acc[k].rho[seat]);
      }
    return out;
  }

 private:
  struct Leaf {
    const double* matrix = nullptr;
    // turn dense combo -> five-card dense index, or -1 if it blocks the card.
    std::vector<std::int32_t> dense5;
  };

  struct ReachAcc {
    std::array<Val, 2> rho;
  };

  const TurnSolveConfig& cfg_;
  GameDef def_;
  Pool pool_;
  std::unique_ptr<AbstractTree> tree_;
  std::unordered_map<std::size_t, Leaf> leaf_by_child_;
  std::unordered_map<std::size_t, std::size_t> cfg_index_by_child_;
  std::vector<double> fold0_;
  std::vector<std::array<int, 2>> turn_holdings_;  // 1128 dense turn combos
  std::vector<std::uint64_t> turn_mask_;           // 52-bit card mask per combo

  std::vector<std::int8_t> actor_;
  std::vector<std::uint32_t> menu_;
  std::vector<std::vector<double>*> reg_;
  std::vector<std::vector<double>*> sum_;
  std::vector<std::vector<double>> sigma_;
  std::size_t action_nodes_ = 0;
  double avg_weight_ = 1.0;

  std::vector<std::unique_ptr<Val>> scratch_pool_;
  std::vector<Val*> scratch_slots_;
  std::vector<std::size_t> scratch_free_;

  std::span<const int> bd4() const { return std::span<const int>(def_.board.data(), 4); }

  void map_leaves() {
    for (std::size_t k = 0; k < cfg_.leaves.size(); ++k) {
      const TurnLeafValue& lv = cfg_.leaves[k];
      if (lv.matrix == nullptr)
        throw std::invalid_argument("turn_solver: leaf has null matrix");
      cfg_index_by_child_.emplace(lv.child_node, k);
      Leaf leaf;
      leaf.matrix = lv.matrix;
      leaf.dense5.assign(kTurnC, -1);
      std::array<bool, 52> on_board{};
      for (int c : lv.board5)
        on_board[c] = true;
      for (std::uint32_t t = 0; t < kTurnC; ++t) {
        const std::array<int, 2> h = holding_for_dense(bd4(), t);
        if (on_board[h[0]] || on_board[h[1]])
          continue;  // turn combo blocks this river card
        leaf.dense5[t] = static_cast<std::int32_t>(
            dense_pair_index(std::span<const int>(lv.board5.data(), 5), h));
      }
      leaf_by_child_.emplace(lv.child_node, std::move(leaf));
    }
  }

  void precompute_folds() {
    fold0_.assign(tree_->size(), 0.0);
    for (std::size_t n = 0; n < tree_->size(); ++n) {
      const TreeNode& node = tree_->node(n);
      if (node.kind == NodeKind::TerminalFold)
        fold0_[n] = static_cast<double>(tree_->terminal(node.terminal).chip_utility[0]);
    }
  }

  void layout() {
    actor_.assign(tree_->size(), 2);
    menu_.assign(tree_->size(), 0);
    reg_.assign(tree_->size(), nullptr);
    sum_.assign(tree_->size(), nullptr);
    for (std::size_t n = 0; n < tree_->size(); ++n) {
      const TreeNode& node = tree_->node(n);
      // Only TURN action nodes carry regret vectors. A river action node that
      // is also a frontier child is intercepted as a matrix terminal and is
      // never recursed into.
      if (!node.is_action() || leaf_by_child_.count(n) != 0)
        continue;
      ++action_nodes_;
      actor_[n] = static_cast<std::int8_t>(node.actor);
      menu_[n] = static_cast<std::uint32_t>(node.actions.size());
      reg_[n] = new std::vector<double>(kTurnC * menu_[n], 0.0);
      sum_[n] = new std::vector<double>(kTurnC * menu_[n], 0.0);
    }
    for (int k = 0; k < 96; ++k) {
      scratch_pool_.emplace_back(std::make_unique<Val>(kTurnC, 0.0));
      scratch_slots_.push_back(scratch_pool_.back().get());
      scratch_free_.push_back(scratch_slots_.size() - 1);
    }
  }

  Val& vec(std::size_t id) { return *scratch_slots_[id]; }
  std::size_t acquire() {
    if (scratch_free_.empty()) {
      scratch_pool_.emplace_back(std::make_unique<Val>(kTurnC, 0.0));
      scratch_slots_.push_back(scratch_pool_.back().get());
      scratch_free_.push_back(scratch_slots_.size() - 1);
    }
    std::size_t id = scratch_free_.back();
    scratch_free_.pop_back();
    return id;
  }
  void release(std::size_t id) { scratch_free_.push_back(id); }

  double rm(std::size_t n, std::uint32_t h, std::size_t a) const {
    const std::size_t A = menu_[n];
    double pos = 0.0;
    for (std::size_t k = 0; k < A; ++k) {
      const double r = (*reg_[n])[h * A + k];
      if (r > 0.0)
        pos += r;
    }
    if (pos <= 0.0)
      return 1.0 / static_cast<double>(A);
    const double r = (*reg_[n])[h * A + a];
    return r > 0.0 ? r / pos : 0.0;
  }

  // Frontier matrix leaf folded with opponent reach into a caller-provided
  // vector. Pure (no shared scratch), safe to run concurrently across the 48
  // river chance children.
  void leaf_into(std::size_t n, std::size_t viewer, const Val& ro, Val& out) const {
    const Leaf& leaf = leaf_by_child_.at(n);
    std::fill(out.begin(), out.end(), 0.0);
    const double* m = leaf.matrix;
    for (std::uint32_t ti = 0; ti < kTurnC; ++ti) {
      const std::int32_t i5 = leaf.dense5[ti];
      if (i5 < 0)
        continue;
      const std::uint64_t imask = turn_mask_[ti];
      double acc = 0.0;
      for (std::uint32_t tj = 0; tj < kTurnC; ++tj) {
        if (ro[tj] == 0.0)
          continue;
        const std::int32_t j5 = leaf.dense5[tj];
        if (j5 < 0 || (imask & turn_mask_[tj]) != 0)
          continue;  // blocks the river card, or shares a hole card
        acc += viewer == 0 ? ro[tj] * m[static_cast<std::size_t>(i5) * kRiverC + j5]
                           : -ro[tj] * m[static_cast<std::size_t>(j5) * kRiverC + i5];
      }
      out[ti] = acc;
    }
  }

  std::size_t leaf_value(std::size_t n, std::size_t viewer, const Val& ro) {
    const std::size_t id = acquire();
    leaf_into(n, viewer, ro, vec(id));
    return id;
  }

  void fold_into(std::size_t n, std::size_t viewer, const Val& ro, Val& out) const {
    const double c = viewer == 0 ? fold0_[n] : -fold0_[n];
    for (std::uint32_t ti = 0; ti < kTurnC; ++ti) {
      double total = 0.0;
      const std::uint64_t imask = turn_mask_[ti];
      for (std::uint32_t tj = 0; tj < kTurnC; ++tj)
        if ((imask & turn_mask_[tj]) == 0)
          total += ro[tj];
      out[ti] = c * total;
    }
  }

  std::size_t fold_value(std::size_t n, std::size_t viewer, const Val& ro) {
    const std::size_t id = acquire();
    fold_into(n, viewer, ro, vec(id));
    return id;
  }

  std::size_t walk(GameState& state, std::size_t n, int trav, Reaches& pi) {
    const TreeNode& node = tree_->node(n);
    if (leaf_by_child_.count(n) != 0)
      return leaf_value(n, trav, pi[trav ^ 1]);
    if (node.kind == NodeKind::TerminalFold)
      return fold_value(n, trav, pi[trav ^ 1]);
    if (node.is_terminal())
      throw std::runtime_error("turn_solver: unexpected terminal in turn tree");

    if (node.is_chance()) {
      // 48 public river children are independent matrix GEMVs: parallel.
      // A joint deal (ti,tj) has 44 live river cards (52 minus the 4-board
      // and the 4 hole cards); the 2 cards each holding blocks are zeroed
      // (i5/j5 < 0), so summing all children with a uniform 1/44 reproduces
      // the exact per-joint-deal expectation. A /46 divisor would be correct
      // only if opponent-held cards were NOT masked - with the shared-hole
      // mask on both sides it attenuates continuation values by 44/46 and
      // creates a spurious fold-vs-continue NashConv floor.
      const std::size_t nc = node.children.size();
      std::vector<Val> results(nc, Val(kTurnC, 0.0));
      const Val& ro = pi[trav ^ 1];
      pool_.parallel_for(nc, [&](std::size_t a) {
        leaf_into(node.children[a], static_cast<std::size_t>(trav), ro, results[a]);
      });
      const std::size_t id = acquire();
      Val& outv = vec(id);
      std::fill(outv.begin(), outv.end(), 0.0);
      for (const Val& r : results)
        for (std::uint32_t i = 0; i < kTurnC; ++i)
          outv[i] += r[i];
      for (std::uint32_t i = 0; i < kTurnC; ++i)
        outv[i] /= 44.0;
      return id;
    }

    const std::size_t actor = node.actor;
    const std::size_t A = menu_[n];
    const Val own_saved = pi[actor];
    std::vector<std::size_t> kids(A);
    for (std::size_t a = 0; a < A; ++a) {
      pi[actor] = own_saved;
      if (actor != static_cast<std::size_t>(trav))
        for (std::uint32_t h = 0; h < kTurnC; ++h)
          pi[actor][h] = own_saved[h] * rm(n, h, a);
      GameState cs = state.after_action(actor, node.actions[a]);
      kids[a] = walk(cs, node.children[a], trav, pi);
    }
    pi[actor] = own_saved;

    const std::size_t out = acquire();
    Val& V = vec(out);
    std::fill(V.begin(), V.end(), 0.0);
    if (actor == static_cast<std::size_t>(trav)) {
      for (std::uint32_t i = 0; i < kTurnC; ++i) {
        double ev = 0.0;
        for (std::size_t a = 0; a < A; ++a)
          ev += rm(n, i, a) * vec(kids[a])[i];
        V[i] = ev;
        // Average-strategy weight matches the proven river kernel: in this
        // traversal convention the traverser's own reach pi[trav][i] is not
        // scaled at own nodes (stays 1), so the linear weight is exact. The
        // earlier sum over opponent reach pi[trav^1] was the REGRET factor
        // pi_-i, not the average-strategy own reach pi_i; using it biased
        // the time-averaged mix and set a permanent NashConv floor.
        for (std::size_t a = 0; a < A; ++a) {
          (*reg_[n])[i * A + a] += vec(kids[a])[i] - ev;
          (*sum_[n])[i * A + a] += avg_weight_ * rm(n, i, a);
        }
      }
    } else {
      for (std::uint32_t i = 0; i < kTurnC; ++i)
        for (std::size_t a = 0; a < A; ++a)
          V[i] += vec(kids[a])[i];
    }
    for (std::size_t a = 0; a < A; ++a)
      release(kids[a]);
    return out;
  }

  double avg_prob(std::size_t n, std::uint32_t h, std::size_t a) const {
    if (sigma_[n].empty())
      return rm(n, h, a);
    return sigma_[n][h * menu_[n] + a];
  }

  Val evaluate(std::size_t viewer, bool br, GameState& state, std::size_t n, Val& ro) {
    const TreeNode& node = tree_->node(n);
    if (leaf_by_child_.count(n) != 0) {
      const std::size_t id = leaf_value(n, viewer, ro);
      Val v = vec(id);
      release(id);
      return v;
    }
    if (node.kind == NodeKind::TerminalFold) {
      const std::size_t id = fold_value(n, viewer, ro);
      Val v = vec(id);
      release(id);
      return v;
    }
    if (node.is_terminal())
      throw std::runtime_error("turn_solver: unexpected terminal in eval");

    if (node.is_chance()) {
      const std::size_t nc = node.children.size();
      std::vector<Val> results(nc, Val(kTurnC, 0.0));
      pool_.parallel_for(
          nc, [&](std::size_t a) { leaf_into(node.children[a], viewer, ro, results[a]); });
      Val sum(kTurnC, 0.0);
      for (const Val& r : results)
        for (std::uint32_t i = 0; i < kTurnC; ++i)
          sum[i] += r[i];
      for (std::uint32_t i = 0; i < kTurnC; ++i)
        sum[i] /= 44.0;
      return sum;
    }

    const std::size_t actor = node.actor;
    const std::size_t A = menu_[n];
    if (actor == viewer && br) {
      std::vector<Val> kids(A);
      for (std::size_t a = 0; a < A; ++a) {
        GameState cs = state.after_action(actor, node.actions[a]);
        kids[a] = evaluate(viewer, br, cs, node.children[a], ro);
      }
      Val out(kTurnC, 0.0);
      for (std::uint32_t i = 0; i < kTurnC; ++i) {
        double best = kids[0][i];
        for (std::size_t a = 1; a < A; ++a)
          best = std::max(best, kids[a][i]);
        out[i] = best;
      }
      return out;
    }

    const Val saved = ro;
    Val out(kTurnC, 0.0);
    for (std::size_t a = 0; a < A; ++a) {
      ro = saved;
      if (actor != viewer)
        for (std::uint32_t h = 0; h < kTurnC; ++h)
          ro[h] = saved[h] * avg_prob(n, h, a);
      GameState cs = state.after_action(actor, node.actions[a]);
      Val v = evaluate(viewer, br, cs, node.children[a], ro);
      if (actor == viewer) {
        for (std::uint32_t i = 0; i < kTurnC; ++i)
          out[i] += avg_prob(n, i, a) * v[i];
      } else {
        for (std::uint32_t i = 0; i < kTurnC; ++i)
          out[i] += v[i];
      }
    }
    ro = saved;
    return out;
  }

  // Accumulate raw (unnormalized) per-five-card-combo CF reach at one chance
  // child: rho0[i5] += P0(ti)/44 * sum_{live,compatible tj} P1(tj), and the
  // symmetric seat-1 term. Pure over its own output, safe in parallel.
  void reach_chance_into(std::size_t child, const Val& p0, const Val& p1,
                         std::array<Val, 2>& rho) const {
    const Leaf& leaf = leaf_by_child_.at(child);
    std::fill(rho[0].begin(), rho[0].end(), 0.0);
    std::fill(rho[1].begin(), rho[1].end(), 0.0);
    Val s0(kTurnC, 0.0), s1(kTurnC, 0.0);
    for (std::uint32_t ti = 0; ti < kTurnC; ++ti) {
      const std::int32_t i5 = leaf.dense5[ti];
      if (i5 < 0)
        continue;
      const std::uint64_t imask = turn_mask_[ti];
      double o1 = 0.0;
      for (std::uint32_t tj = 0; tj < kTurnC; ++tj) {
        const std::int32_t j5 = leaf.dense5[tj];
        if (j5 >= 0 && (imask & turn_mask_[tj]) == 0) {
          o1 += p1[tj];
          s0[tj] += p0[ti];
        }
      }
      s1[ti] = o1;
    }
    for (std::uint32_t ti = 0; ti < kTurnC; ++ti) {
      const std::int32_t i5 = leaf.dense5[ti];
      if (i5 >= 0)
        rho[0][static_cast<std::size_t>(i5)] += p0[ti] * s1[ti] / 44.0;
    }
    for (std::uint32_t tj = 0; tj < kTurnC; ++tj) {
      const std::int32_t j5 = leaf.dense5[tj];
      if (j5 >= 0)
        rho[1][static_cast<std::size_t>(j5)] += p1[tj] * s0[tj] / 44.0;
    }
  }

  void build_sigma() {
    sigma_.assign(tree_->size(), {});
    for (std::size_t n = 0; n < tree_->size(); ++n) {
      if (reg_[n] == nullptr)
        continue;
      sigma_[n].assign(kTurnC * menu_[n], 0.0);
      for (std::uint32_t i = 0; i < kTurnC; ++i) {
        double tot = 0.0;
        for (std::size_t a = 0; a < menu_[n]; ++a)
          tot += (*sum_[n])[i * menu_[n] + a];
        for (std::size_t a = 0; a < menu_[n]; ++a)
          sigma_[n][i * menu_[n] + a] =
              tot > 0.0 ? (*sum_[n])[i * menu_[n] + a] / tot : 1.0 / static_cast<double>(menu_[n]);
      }
    }
  }

  // Propagate BOTH players' average-strategy prefix probabilities and, at
  // each chance node, accumulate raw CF reach into its children's leaves.
  void reach_walk(GameState& state, std::size_t n, const Val& p0, const Val& p1,
                  std::vector<ReachAcc>& acc) {
    const TreeNode& node = tree_->node(n);
    auto cfg_it = cfg_index_by_child_.find(n);
    if (cfg_index_by_child_.find(n) != cfg_index_by_child_.end()) {
      reach_chance_into(n, p0, p1, acc[cfg_it->second].rho);
      return;
    }
    if (node.is_terminal())
      return;  // fold: no river leaf mass
    if (node.is_chance()) {
      const std::size_t nc = node.children.size();
      pool_.parallel_for(nc, [&](std::size_t a) {
        auto it = cfg_index_by_child_.find(node.children[a]);
        if (it == cfg_index_by_child_.end())
          return;  // unexpected: every chance child must be a matrix leaf
        reach_chance_into(node.children[a], p0, p1, acc[it->second].rho);
      });
      return;
    }
    const std::size_t actor = node.actor;
    const std::size_t A = menu_[n];
    const Val& own = actor == 0 ? p0 : p1;
    for (std::size_t a = 0; a < A; ++a) {
      Val np0 = p0, np1 = p1;
      Val& nown = actor == 0 ? np0 : np1;
      for (std::uint32_t h = 0; h < kTurnC; ++h)
        nown[h] = own[h] * avg_prob(n, h, a);
      GameState cs = state.after_action(actor, node.actions[a]);
      reach_walk(cs, node.children[a], np0, np1, acc);
    }
  }
};

// Independent telescoping exact-best-response walker. Deliberately separate
// code from TurnGame (own tree construction, leaf mapping, matrix folds,
// recursion, and chance averaging) so the zero-perturbation equality with
// TurnGame::nash_conv_chips is an independent cross-check rather than the
// same code agreeing with itself.
class HybridWalker {
 public:
  HybridWalker(const TurnSolveConfig& cfg, std::span<const std::vector<double>> node_sigma)
      : cfg_(cfg), def_(cfg.def), pool_(cfg.threads), sigma_(node_sigma) {
    if (def_.player_count != 2 || def_.board_size != 4 || def_.terminal != TerminalDepth::River)
      throw std::invalid_argument("hybrid walker: needs a 2-seat four-card river-terminal def");
    tree_ = std::make_unique<AbstractTree>(
        def_, bs::abstraction::ActionAbstraction::identity(std::uint8_t(1)));
    for (std::uint32_t t = 0; t < kTurnC; ++t) {
      const auto h = holding_for_dense(std::span<const int>(def_.board.data(), 4), t);
      masks_.push_back((std::uint64_t(1) << h[0]) | (std::uint64_t(1) << h[1]));
    }
    for (const TurnLeafValue& lv : cfg_.leaves) {
      WLeaf wl;
      wl.matrix = lv.matrix;
      wl.dense5.assign(kTurnC, -1);
      std::array<bool, 52> on_board{};
      for (int c : lv.board5)
        on_board[c] = true;
      for (std::uint32_t t = 0; t < kTurnC; ++t) {
        const auto h = holding_for_dense(std::span<const int>(def_.board.data(), 4), t);
        if (on_board[h[0]] || on_board[h[1]])
          continue;
        wl.dense5[t] = static_cast<std::int32_t>(
            dense_pair_index(std::span<const int>(lv.board5.data(), 5), h));
      }
      leaves_.emplace(lv.child_node, std::move(wl));
    }
    fold0_.assign(tree_->size(), 0.0);
    for (std::size_t n = 0; n < tree_->size(); ++n) {
      const TreeNode& node = tree_->node(n);
      if (node.kind == NodeKind::TerminalFold)
        fold0_[n] = static_cast<double>(tree_->terminal(node.terminal).chip_utility[0]);
    }
  }

  double turn_gap_chips() {
    double raw = 0.0;
    for (int viewer = 0; viewer < 2; ++viewer) {
      Val ro(kTurnC, 1.0), own(kTurnC, 1.0);
      GameState st(def_);
      double gap = 0.0;
      gap_walk(st, tree_->root_index(), viewer, ro, own, gap);
      raw += gap;
    }
    return raw / kTurnNorm;
  }

  // w(leaf,seat,i5) = (BR own-reach - average own-reach) to that five-card
  // combo, /kTurnNorm (same units as the exported equilibrium rho).
  std::vector<TurnLeafGapWeight> leaf_gap_weights() {
    std::unordered_map<std::size_t, std::size_t> idx;
    for (std::size_t k = 0; k < cfg_.leaves.size(); ++k)
      idx.emplace(cfg_.leaves[k].child_node, k);
    std::vector<TurnLeafGapWeight> out(cfg_.leaves.size());
    for (std::size_t k = 0; k < cfg_.leaves.size(); ++k) {
      out[k].child_node = cfg_.leaves[k].child_node;
      for (int seat = 0; seat < 2; ++seat)
        out[k].w[seat].assign(kRiverC, 0.0);
    }
    for (int viewer = 0; viewer < 2; ++viewer) {
      std::vector<Val> br_tab(cfg_.leaves.size(), Val(kRiverC, 0.0));
      std::vector<Val> sg_tab(cfg_.leaves.size(), Val(kRiverC, 0.0));
      {
        Val own(kTurnC, 1.0), opp(kTurnC, 1.0);
        GameState st(def_);
        policy_reach(st, tree_->root_index(), viewer, /*use_br=*/true, own, opp, idx, br_tab);
      }
      {
        Val own(kTurnC, 1.0), opp(kTurnC, 1.0);
        GameState st(def_);
        policy_reach(st, tree_->root_index(), viewer, /*use_br=*/false, own, opp, idx, sg_tab);
      }
      for (std::size_t k = 0; k < cfg_.leaves.size(); ++k)
        for (std::uint32_t c = 0; c < kRiverC; ++c)
          out[k].w[viewer][c] = (br_tab[k][c] - sg_tab[k][c]) / kTurnNorm;
    }
    return out;
  }

  // Accumulate the viewer's OWN-combo arrival reach to a chance child leaf:
  // own[ti]/44 times the compatible live-opponent reach sum, mapped to i5.
  void weight_leaf_into(std::size_t child, std::size_t viewer, const Val& own, const Val& opp,
                        Val& out5) const {
    const WLeaf& wl = leaves_.at(child);
    for (std::uint32_t ti = 0; ti < kTurnC; ++ti) {
      const std::int32_t i5 = wl.dense5[ti];
      if (i5 < 0 || own[ti] == 0.0)
        continue;
      const std::uint64_t imask = masks_[ti];
      double so = 0.0;
      for (std::uint32_t tj = 0; tj < kTurnC; ++tj) {
        const std::int32_t j5 = wl.dense5[tj];
        if (j5 >= 0 && (imask & masks_[tj]) == 0)
          so += opp[tj];
      }
      out5[static_cast<std::size_t>(i5)] += own[ti] * so / 44.0;
    }
  }

  // Reach walk under the viewer's best-response policy (use_br: route each
  // own combo only to its argmax BR action) or the average strategy; opponent
  // always plays average. Accumulates the viewer's own-combo leaf reach.
  void policy_reach(GameState& state, std::size_t n, std::size_t viewer, bool use_br,
                    const Val& own, const Val& opp,
                    const std::unordered_map<std::size_t, std::size_t>& idx,
                    std::vector<Val>& tab) {
    const TreeNode& node = tree_->node(n);
    if (auto it = idx.find(n); it != idx.end()) {
      weight_leaf_into(n, viewer, own, opp, tab[it->second]);
      return;
    }
    if (node.is_terminal())
      return;
    if (node.is_chance()) {
      const std::size_t nc = node.children.size();
      pool_.parallel_for(nc, [&](std::size_t a) {
        auto ci = idx.find(node.children[a]);
        if (ci != idx.end())
          weight_leaf_into(node.children[a], viewer, own, opp, tab[ci->second]);
      });
      return;
    }

    const std::size_t actor = node.actor;
    const std::size_t A = menu(n);
    if (actor == viewer && use_br) {
      // Child BR continuation values at THIS node's opponent reach; gap_walk's
      // returned value carries no own reach, so the per-action argmax is clean.
      std::vector<Val> kv(A);
      for (std::size_t a = 0; a < A; ++a) {
        Val down(kTurnC, 1.0);
        GameState cs = state.after_action(actor, node.actions[a]);
        double dg = 0.0;
        kv[a] = gap_walk(cs, node.children[a], viewer, opp, down, dg);
      }
      for (std::size_t a = 0; a < A; ++a) {
        Val nown(kTurnC, 0.0);
        for (std::uint32_t h = 0; h < kTurnC; ++h) {
          int best = 0;
          for (std::size_t b = 1; b < A; ++b)
            if (kv[b][h] > kv[best][h])
              best = static_cast<int>(b);
          if (best == static_cast<int>(a))
            nown[h] = own[h];
        }
        GameState cs = state.after_action(actor, node.actions[a]);
        policy_reach(cs, node.children[a], viewer, use_br, nown, opp, idx, tab);
      }
      return;
    }

    for (std::size_t a = 0; a < A; ++a) {
      Val nown = own;
      Val nopp = opp;
      if (actor == viewer) {
        for (std::uint32_t h = 0; h < kTurnC; ++h)
          nown[h] = own[h] * sig(n, h, a);
      } else {
        for (std::uint32_t h = 0; h < kTurnC; ++h)
          nopp[h] = opp[h] * sig(n, h, a);
      }
      GameState cs = state.after_action(actor, node.actions[a]);
      policy_reach(cs, node.children[a], viewer, use_br, nown, nopp, idx, tab);
    }
  }

  // Per-config-leaf unnormalized arrival mass W_k = (1/44) sum over live,
  // compatible joint deals of p0(ti)*p1(tj), under the fixed average strategy.
  std::vector<double> leaf_masses() {
    std::vector<double> w(cfg_.leaves.size(), 0.0);
    std::unordered_map<std::size_t, std::size_t> idx;
    for (std::size_t k = 0; k < cfg_.leaves.size(); ++k)
      idx.emplace(cfg_.leaves[k].child_node, k);
    Val p0(kTurnC, 1.0), p1(kTurnC, 1.0);
    GameState st(def_);
    mass_walk(st, tree_->root_index(), p0, p1, idx, w);
    return w;
  }

 private:
  struct WLeaf {
    const double* matrix = nullptr;
    std::vector<std::int32_t> dense5;
  };

  const TurnSolveConfig& cfg_;
  GameDef def_;
  Pool pool_;
  std::span<const std::vector<double>> sigma_;
  std::unique_ptr<AbstractTree> tree_;
  std::unordered_map<std::size_t, WLeaf> leaves_;
  std::vector<double> fold0_;
  std::vector<std::uint64_t> masks_;

  std::size_t menu(std::size_t n) const {
    return static_cast<std::size_t>(tree_->node(n).actions.size());
  }
  double sig(std::size_t n, std::uint32_t h, std::size_t a) const {
    const std::size_t A = menu(n);
    const std::vector<double>& v = sigma_[n];
    if (v.empty())
      return 1.0 / static_cast<double>(A);
    return v[h * A + a];
  }

  void leaf_fold(std::size_t n, std::size_t viewer, const Val& ro, Val& out) const {
    const WLeaf& wl = leaves_.at(n);
    std::fill(out.begin(), out.end(), 0.0);
    const double* m = wl.matrix;
    for (std::uint32_t ti = 0; ti < kTurnC; ++ti) {
      const std::int32_t i5 = wl.dense5[ti];
      if (i5 < 0)
        continue;
      const std::uint64_t imask = masks_[ti];
      double acc = 0.0;
      for (std::uint32_t tj = 0; tj < kTurnC; ++tj) {
        if (ro[tj] == 0.0)
          continue;
        const std::int32_t j5 = wl.dense5[tj];
        if (j5 < 0 || (imask & masks_[tj]) != 0)
          continue;
        acc += viewer == 0 ? ro[tj] * m[static_cast<std::size_t>(i5) * kRiverC + j5]
                           : -ro[tj] * m[static_cast<std::size_t>(j5) * kRiverC + i5];
      }
      out[ti] = acc;
    }
  }

  void fold_fold(std::size_t n, std::size_t viewer, const Val& ro, Val& out) const {
    const double c = viewer == 0 ? fold0_[n] : -fold0_[n];
    for (std::uint32_t ti = 0; ti < kTurnC; ++ti) {
      const std::uint64_t imask = masks_[ti];
      double total = 0.0;
      for (std::uint32_t tj = 0; tj < kTurnC; ++tj)
        if ((imask & masks_[tj]) == 0)
          total += ro[tj];
      out[ti] = c * total;
    }
  }

  // Returns the best-response value vector (raw, summed over compatible
  // opponent deals) for `viewer`; accumulates that viewer's telescoping
  // infoset gap into the returned double alongside via gap_ref.
  Val gap_walk(GameState& state, std::size_t n, std::size_t viewer, const Val& ro, const Val& own,
               double& gap) {
    const TreeNode& node = tree_->node(n);
    if (leaves_.count(n) != 0) {
      Val v(kTurnC, 0.0);
      leaf_fold(n, viewer, ro, v);
      return v;
    }
    if (node.kind == NodeKind::TerminalFold) {
      Val v(kTurnC, 0.0);
      fold_fold(n, viewer, ro, v);
      return v;
    }
    if (node.is_terminal())
      throw std::runtime_error("hybrid walker: unexpected terminal");

    if (node.is_chance()) {
      const std::size_t nc = node.children.size();
      std::vector<Val> kids(nc, Val(kTurnC, 0.0));
      pool_.parallel_for(nc,
                         [&](std::size_t a) { leaf_fold(node.children[a], viewer, ro, kids[a]); });
      Val sum(kTurnC, 0.0);
      for (const Val& v : kids)
        for (std::uint32_t i = 0; i < kTurnC; ++i)
          sum[i] += v[i];
      for (std::uint32_t i = 0; i < kTurnC; ++i)
        sum[i] /= 44.0;
      return sum;
    }

    const std::size_t actor = node.actor;
    const std::size_t A = menu(n);
    std::vector<Val> kids(A);
    for (std::size_t a = 0; a < A; ++a) {
      Val nro = ro;
      Val nown = own;
      if (actor != viewer)
        for (std::uint32_t h = 0; h < kTurnC; ++h)
          nro[h] = ro[h] * sig(n, h, a);
      else
        for (std::uint32_t h = 0; h < kTurnC; ++h)
          nown[h] = own[h] * sig(n, h, a);
      GameState cs = state.after_action(actor, node.actions[a]);
      kids[a] = gap_walk(cs, node.children[a], viewer, nro, nown, gap);
    }
    Val out(kTurnC, 0.0);
    if (actor == viewer) {
      for (std::uint32_t i = 0; i < kTurnC; ++i) {
        double best = kids[0][i];
        double ev = 0.0;
        for (std::size_t a = 0; a < A; ++a) {
          best = std::max(best, kids[a][i]);
          ev += sig(n, i, a) * kids[a][i];
        }
        gap += own[i] * (best - ev);
        out[i] = best;
      }
    } else {
      for (std::uint32_t i = 0; i < kTurnC; ++i)
        for (std::size_t a = 0; a < A; ++a)
          out[i] += kids[a][i];
    }
    return out;
  }

  void mass_walk(GameState& state, std::size_t n, const Val& p0, const Val& p1,
                 const std::unordered_map<std::size_t, std::size_t>& idx, std::vector<double>& w) {
    const TreeNode& node = tree_->node(n);
    if (idx.count(n) != 0) {
      const WLeaf& wl = leaves_.at(n);
      double mass = 0.0;
      for (std::uint32_t ti = 0; ti < kTurnC; ++ti) {
        if (wl.dense5[ti] < 0)
          continue;
        const std::uint64_t imask = masks_[ti];
        for (std::uint32_t tj = 0; tj < kTurnC; ++tj)
          if (wl.dense5[tj] >= 0 && (imask & masks_[tj]) == 0)
            mass += p0[ti] * p1[tj];
      }
      w[idx.at(n)] += mass / 44.0;
      return;
    }
    if (node.is_terminal())
      return;
    if (node.is_chance()) {
      for (std::size_t child : node.children)
        mass_walk(state, child, p0, p1, idx, w);  // state unused at leaf
      return;
    }
    const std::size_t actor = node.actor;
    const std::size_t A = menu(n);
    const Val& ownp = actor == 0 ? p0 : p1;
    for (std::size_t a = 0; a < A; ++a) {
      Val np0 = p0, np1 = p1;
      Val& nown = actor == 0 ? np0 : np1;
      for (std::uint32_t h = 0; h < kTurnC; ++h)
        nown[h] = ownp[h] * sig(n, h, a);
      GameState cs = state.after_action(actor, node.actions[a]);
      mass_walk(cs, node.children[a], np0, np1, idx, w);
    }
  }
};

}  // namespace

TurnSolveResult solve_turn_subgame(const TurnSolveConfig& cfg) {
  TurnGame game(cfg);
  TurnSolveResult out;
  const double bb = cfg.def.big_blind > 0 ? static_cast<double>(cfg.def.big_blind) : 1.0;
  std::size_t cp = 0;
  for (std::uint64_t t = 1; t <= cfg.iterations; ++t) {
    game.iterate_weighted(cfg.linear_averaging ? static_cast<double>(t) : 1.0);
    while (cp < cfg.checkpoints.size() && cfg.checkpoints[cp] <= t) {
      out.history_nash_conv_bb.push_back(game.nash_conv_chips() / bb);
      ++cp;
    }
  }
  out.iterations = cfg.iterations;
  out.turn_action_nodes = game.turn_action_node_count();
  out.tree_size = game.tree_node_count();
  out.sigma = game.average_sigma();
  out.leaf_reaches = game.export_leaf_reaches();
  auto cv0 = game.root_combo_values(0);
  auto cv1 = game.root_combo_values(1);
  out.root_cfv_seat0 = std::move(cv0.ev);
  out.root_br_seat0 = std::move(cv0.br);
  out.root_cfv_seat1 = std::move(cv1.ev);
  out.root_br_seat1 = std::move(cv1.br);
  const double nc = game.nash_conv_chips();
  out.nash_conv_chips = nc;
  out.nash_conv_bb = nc / bb;
  return out;
}

TurnGameLifecycleInfo turn_game_lifecycle_probe(const TurnSolveConfig& cfg) {
  // Real constructor/destructor only: layout() allocates every per-node
  // regret/sum table and the game is destroyed before any iteration runs.
  TurnGame game(cfg);
  return {game.tree_node_count(), game.turn_action_node_count()};
}

HybridNashConv turn_hybrid_nash_conv(const TurnSolveConfig& true_cfg,
                                     std::span<const std::vector<double>> node_sigma,
                                     std::span<const double> leaf_river_nc_chips) {
  if (leaf_river_nc_chips.size() != true_cfg.leaves.size())
    throw std::invalid_argument(
        "turn_hybrid_nash_conv: one river NashConv per config leaf required");
  HybridWalker walker(true_cfg, node_sigma);
  HybridNashConv out;
  out.turn_gap_chips = walker.turn_gap_chips();
  const std::vector<double> w = walker.leaf_masses();
  double river = 0.0;
  for (std::size_t k = 0; k < w.size(); ++k)
    river += w[k] * leaf_river_nc_chips[k];
  out.river_term_chips = river / kTurnNorm;
  const double bb = true_cfg.def.big_blind > 0 ? static_cast<double>(true_cfg.def.big_blind) : 1.0;
  out.turn_gap_bb = out.turn_gap_chips / bb;
  out.river_term_bb = out.river_term_chips / bb;
  return out;
}

std::vector<TurnLeafGapWeight> turn_leaf_gap_weights(
    const TurnSolveConfig& cfg, std::span<const std::vector<double>> node_sigma) {
  HybridWalker walker(cfg, node_sigma);
  return walker.leaf_gap_weights();
}

}  // namespace bs::rfc0047
