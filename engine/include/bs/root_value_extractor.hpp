// RFC 0013 Stage 1: root-value extraction from a solved NSeatPolicy.
//
// Walks the solved abstract tree for a specific joint deal and computes the
// expected per-seat chip utility at the root:
//   - Action node: look up the policy row by (own_card_bucket, node_index),
//     enumerate every menu action weighted by its average-policy probability.
//     If the row is missing (external-sampling MCCFR did not seal this info
//     set), fall back to a uniform distribution — the trainer's own
//     current_strategy on a fresh row — matching the continuation-range
//     export's fallback.
//   - Chance node: enumerate every legal runout card (board-only list minus
//     all hole cards), average uniformly.
//   - TerminalFold: return the payload's stored chip_utility.
//   - TerminalShowdown: settle with the live seats' hole cards.
//
// This is the policy-following traversal that produces the values stored in
// the NSeatDeclaredFrontierTable. On small enumerable fixtures it is
// cross-checked against stage6::exact_deal_utility.
#pragma once

#include <array>
#include <bs/abstract_tree.hpp>
#include <bs/game_definition.hpp>
#include <bs/nseat_trainer.hpp>
#include <cstddef>
#include <memory>
#include <span>
#include <vector>

namespace bs::gto {

// Solver-local projection of a representative postflop tree. Public-card
// labels and terminal boards remain private; requests supply the real board.
class PostflopEvaluationTemplate {
 public:
  struct Node {
    tree::NodeKind kind;
    std::size_t actor;
    std::span<const poker::Action> actions;
    std::span<const std::size_t> children;
    std::size_t terminal;
  };

  explicit PostflopEvaluationTemplate(tree::AbstractTree representative);
  bool matches(const solver::NSeatPolicy& policy) const;
  Node node(std::size_t index) const;
  std::span<const std::int64_t> fold_utility(std::size_t terminal) const;
  std::span<const std::size_t> live_order(std::size_t terminal) const;
  std::size_t size() const noexcept;
  std::size_t accounted_bytes() const noexcept;

 private:
  std::unique_ptr<const tree::AbstractTree> representative_;
};

// Extract the expected per-seat chip utility for one fixed joint deal at the
// root of the solved tree. `deal` has one sorted hole-card pair per seat.
// The walk is exact (no sampling): every action and every legal runout card
// is enumerated. Missing policy rows fall back to uniform (the trainer's
// default strategy on an unsealed info set). Throws std::invalid_argument on
// a deal-size mismatch or std::runtime_error on an unexpected node kind.
std::vector<double> extract_root_value(const solver::NSeatPolicy& policy,
                                       const tree::AbstractTree& tree,
                                       const poker::GameState& root_state,
                                       std::span<const std::array<int, 2>> deal);

// The same exact kernel over a structural template. Rejects mismatched
// non-board configuration or action-abstraction identity.
std::vector<double> extract_root_value(const solver::NSeatPolicy& policy,
                                       const PostflopEvaluationTemplate& evaluation,
                                       const poker::GameState& root_state,
                                       std::span<const std::array<int, 2>> deal);

}  // namespace bs::gto
