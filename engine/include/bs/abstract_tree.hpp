// RFC 0008 §L3: the abstract public betting tree.
//
// One builder produces an AbstractTree from an L1 GameDef and an L2
// ActionAbstraction. It is a faithful, seat-count-agnostic DESCRIPTION of the
// public game: the abstracted action tree, the public-card chance events, and
// the terminal ledgers. It carries no regrets, no policy, no ranges, no private
// (hole) cards, and does no IO; a solver consumes it from L4.
//
// Dependency: L3 depends only on L1 (GameDef/GameState) and L2
// (ActionAbstraction). L3 sources must never name the transitional
// HeadsUpState/HeadsUpRoot adapters or evaluate private holdings -- the type
// guard below and a link-negative CMake target enforce that.
#pragma once

#include <array>
#include <bs/abstraction.hpp>
#include <bs/game_definition.hpp>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace bs::tree {

using poker::Action;
using poker::GameDef;
using poker::GameState;
using poker::Street;

inline constexpr std::size_t kNoNode = static_cast<std::size_t>(-1);
inline constexpr int kNoCard = -1;

// The shared L3 menu rule: the ordered coarse abstract menu at an action node
// (HU MenuContext at 2 seats, max-cover MultiwayMenuContext at 3+). The tree
// builder and the stage-6 streaming/MCCFR trainer both call THIS so their menus
// can never drift; re-implementation is forbidden.
std::vector<Action> abstract_node_menu(const GameState& state,
                                       const abstraction::ActionAbstraction& action,
                                       const poker::LegalActions& legal);

// Public-card set of a chance node: ascending {0..51} minus the board. The
// trainer's streaming chance ordinal indexes this exact list.
std::vector<int> public_runout_cards(const GameState& state);

// RFC 0007: TerminalFrontier is the leaf of a flop-terminal game
// (TerminalDepth::Flop). The payload carries the same ledger as a showdown
// leaf; the frontier value is supplied by L4 through a FrontierEvaluator.
//
// FlopDeal is a virtual leaf for flop-terminal games: it replaces the entire
// 3-level chance subtree (52×51×50 = 132,600 frontier leaves per preflop
// line). The tree stores no chance nodes or frontier payloads for these
// games; the trainer's walk samples 3 cards inline at the FlopDeal leaf and
// constructs the frontier payload from the GameState it carries. This keeps
// the tree small (hundreds of nodes) at any stack depth.
enum class NodeKind { Action, Chance, TerminalFold, TerminalShowdown, TerminalFrontier, FlopDeal };

// One seat's terminal ledger entry. Folded seats are retained with their GROSS
// contributed amount: N-way side-pot settlement needs every seat's commitment,
// not only the live prefix.
struct TerminalSeat {
  poker::Chips contributed = 0;
  poker::Chips refunded = 0;
  poker::Chips stack = 0;
  bool folded = false;
};

// Terminal data, stored once per leaf in a side table (nodes hold an index into
// it, so Action/Chance nodes do not pay for it). For a fold, `chip_utility` is
// the exact settled payout vector from L1 settle_fold(). For a showdown it is
// empty until holdings are supplied: L4 packs concrete hole cards in ascending
// live-seat order and calls settle_showdown; the ledger + board below are what
// that evaluation needs.
struct TerminalPayload {
  std::size_t player_count = 0;
  std::array<TerminalSeat, poker::kMaxUnifiedSeats> seats{};
  std::array<std::size_t, poker::kMaxUnifiedSeats> live_order{};
  std::size_t live_count = 0;
  std::array<int, 5> board{};
  std::uint8_t board_size = 0;
  bool folded = false;
  std::vector<std::int64_t> chip_utility;  // populated for fold leaves
};

// Constructs a TerminalPayload for a frontier leaf from a GameState. This is
// the walk-side counterpart to the builder's internal make_terminal: a
// FlopDeal leaf stores no payload, so the walk builds one from the GameState
// it carries by value. The payload is never folded and has empty chip_utility
// (the frontier evaluator supplies expectation values).
TerminalPayload make_frontier_payload(const GameState& state);

// A deterministic construction bound (no time cap: building is deterministic,
// so a wall clock would bind a non-property). Each cap is checked BEFORE the
// allocation that would exceed it. Distinct from an unsupported tree shape.
struct TreeLimits {
  std::size_t max_nodes = 2'000'000;
  std::size_t max_depth = 256;
  std::size_t max_bytes = std::size_t{1} << 30;  // 1 GiB
};

// Thrown when a tree cannot be built within its TreeLimits. Deliberately does
// NOT derive from std::invalid_argument: resource exhaustion is an expected,
// recoverable condition distinct from malformed input, and host
// invalid-argument handlers must not misclassify it as a bad request.
class tree_resource_exhausted : public std::runtime_error {
 public:
  explicit tree_resource_exhausted(const std::string& what) : std::runtime_error(what) {}
};

// One node of the public tree. Nodes are addressed by stable index into the
// tree's node vector (never by pointer: the vector grows during construction).
//
// Edge labels live on the child: an Action parent reaches a child by
// `incoming_action` (the children are menu-aligned and in menu order); a Chance
// parent reaches one by `incoming_card` (a concrete public card id). Chance
// edges carry NO probability -- per-deal conditioning and the uniform measure
// are owned by L4.
struct TreeNode {
  std::size_t index = kNoNode;
  std::size_t parent = kNoNode;
  NodeKind kind = NodeKind::Action;
  Street street = Street::Flop;

  std::vector<std::size_t> children;

  // Action node: the acting seat and the ordered abstract menu. `actions[i]`
  // is the edge that leads to `children[i]`.
  std::size_t actor = kNoNode;
  std::vector<Action> actions;

  // Edge by which this node was reached from its parent.
  Action incoming_action{};
  int incoming_card = kNoCard;

  // Terminal leaf index into AbstractTree::terminals() (kNoNode otherwise).
  // The builder carries the current GameState by value during DFS and records a
  // compact TerminalPayload only at leaves, so internal nodes stay small.
  std::size_t terminal = kNoNode;

  bool is_action() const { return kind == NodeKind::Action; }
  bool is_chance() const { return kind == NodeKind::Chance; }
  bool is_flop_deal() const { return kind == NodeKind::FlopDeal; }
  bool is_terminal() const {
    return kind == NodeKind::TerminalFold || kind == NodeKind::TerminalShowdown ||
           kind == NodeKind::TerminalFrontier;
  }
};

// The materialized tree. Owns its source GameDef and ActionAbstraction by value
// (never by reference: building from a temporary must be safe) and exposes
// their identities for solver dispatch and L5 keying.
class AbstractTree {
 public:
  // Builds the full unconditioned public tree. Throws std::invalid_argument for
  // a malformed GameDef/abstraction and tree_resource_exhausted at a bound; a
  // failed build yields no tree (strong exception safety).
  AbstractTree(GameDef def, abstraction::ActionAbstraction action, TreeLimits limits = {});

  const GameDef& def() const noexcept { return def_; }
  const abstraction::ActionAbstraction& action_abstraction() const noexcept { return action_; }
  const abstraction::AbstractionId& action_id() const noexcept { return action_.id(); }
  const TreeLimits& limits() const noexcept { return limits_; }

  std::size_t root_index() const noexcept { return 0; }
  std::size_t size() const noexcept { return nodes_.size(); }
  std::size_t depth() const noexcept { return depth_; }
  std::size_t accounted_bytes() const noexcept { return accounted_bytes_; }

  const TreeNode& node(std::size_t index) const { return nodes_.at(index); }
  const std::vector<TreeNode>& nodes() const noexcept { return nodes_; }
  const std::vector<TerminalPayload>& terminals() const noexcept { return terminals_; }
  const TerminalPayload& terminal(std::size_t leaf_index) const {
    return terminals_.at(leaf_index);
  }

 private:
  GameDef def_;
  abstraction::ActionAbstraction action_;
  TreeLimits limits_;
  std::vector<TreeNode> nodes_;
  std::vector<TerminalPayload> terminals_;
  std::size_t depth_ = 0;
  std::size_t accounted_bytes_ = 0;

  friend class TreeBuilder;
};

}  // namespace bs::tree
