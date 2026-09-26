#include <array>
#include <bs/abstract_tree.hpp>
#include <cstdint>
#include <limits>

namespace bs::tree {

namespace {

using poker::Action;
using poker::Chips;
using poker::ContributionSettlement;
using poker::GamePlayer;
using poker::GameState;
using poker::LegalActions;
using poker::Phase;

// Overflow-checked additive byte charge; throws tree_resource_exhausted before
// the allocation that would cross a bound (or overflow_error on a checked
// product/sum). Every RETAINED heap block the tree owns is charged by its
// allocated CAPACITY before it grows (TreeBuilder::ensure_capacity), so the
// charged total is an UPPER BOUND on retained bytes and the throw precedes the
// crossing reservation. Only bounded TRANSIENT working memory (the DFS frame
// stack, one menu, one public-card list) is uncharged.
class Budget {
 public:
  explicit Budget(const TreeLimits& limits) : limits_(limits) {}

  std::size_t max_nodes() const { return limits_.max_nodes; }

  std::size_t nodes = 0;
  std::size_t depth = 0;

  void charge(std::size_t bytes) {
    if (bytes > std::numeric_limits<std::size_t>::max() - bytes_)
      throw std::overflow_error("tree byte charge overflow");
    bytes_ += bytes;
    if (bytes_ > limits_.max_bytes)
      throw tree_resource_exhausted("abstract tree exceeded the byte cap");
  }

  // Charge (and bounds-check) the capacity GROWTH of a backing array before the
  // caller reserves it. The charge is for the whole grown slab, which is the
  // real allocation.
  void charge_capacity(std::size_t old_capacity, std::size_t new_capacity, std::size_t elem_size) {
    if (elem_size != 0 && new_capacity > std::numeric_limits<std::size_t>::max() / elem_size)
      throw std::overflow_error("tree capacity product overflow");
    charge((new_capacity - old_capacity) * elem_size);
  }

  void add_node(std::size_t path_depth) {
    if (nodes == limits_.max_nodes)
      throw tree_resource_exhausted("abstract tree exceeded the node cap");
    if (path_depth > limits_.max_depth)
      throw tree_resource_exhausted("abstract tree exceeded the depth cap");
    ++nodes;
    depth = std::max(depth, path_depth);
  }

  std::size_t bytes() const { return bytes_; }

 private:
  const TreeLimits& limits_;
  std::size_t bytes_ = 0;
};

// Deepest cover across every other LIVE, non-folded seat: the maximum street
// total such a seat can reach. Used by the 3+ profile menu; at two seats it has
// one term and matches the heads-up single-opponent total.
Chips deepest_cover(const GameState& state, std::size_t actor) {
  Chips cover = 0;
  const auto& players = state.players();
  for (std::size_t seat : state.live_players()) {
    if (seat == actor)
      continue;
    const GamePlayer& p = players[seat];
    Chips total = p.street_committed;
    if (p.stack > std::numeric_limits<Chips>::max() - total)
      throw std::overflow_error("cover sum overflow");
    total += p.stack;
    cover = std::max(cover, total);
  }
  return cover;
}

// Builds the L2 menu for a node using the profile-neutral rule. The L2 action
// abstraction owns the fractions; L3 supplies the profile's cover. The single
// exported implementation lives below the anonymous namespace
// (bs::tree::abstract_node_menu); deep cover is the file-local helper above.

TerminalPayload make_terminal(const GameState& state, bool folded, Budget& budget) {
  TerminalPayload payload;
  payload.player_count = state.player_count();
  payload.folded = folded;
  payload.board_size = static_cast<std::uint8_t>(state.board().size());
  for (std::size_t i = 0; i < state.board().size() && i < payload.board.size(); ++i)
    payload.board[i] = state.board()[i];
  const auto& players = state.players();
  for (std::size_t seat = 0; seat < state.player_count(); ++seat) {
    payload.seats[seat].contributed = players[seat].contributed;
    payload.seats[seat].refunded = players[seat].refunded;
    payload.seats[seat].stack = players[seat].stack;
    payload.seats[seat].folded = players[seat].folded;
  }
  payload.live_count = state.live_players().size();
  for (std::size_t i = 0; i < state.live_players().size(); ++i)
    payload.live_order[i] = state.live_players()[i];
  if (folded) {
    // The settled vector is transient working memory; charge its slab before
    // the tree takes ownership, so a leaf that would cross the byte bound
    // throws rather than retains the payout array.
    ContributionSettlement settled = state.settle_fold();
    budget.charge_capacity(0, settled.chip_utility.capacity(),
                           sizeof(decltype(settled.chip_utility)::value_type));
    payload.chip_utility = std::move(settled.chip_utility);
  }
  return payload;
}

// Reserve backing capacity under the byte budget, charging the grown slab
// BEFORE it is allocated. The growth policy is an explicit doubling (not the
// allocator's unspecified geometric factor), and reserve requests that exact
// target, so the charged capacity is the slab actually retained and there is
// no unaccounted reallocation slack.
template <class T>
void grow_to_fit(std::vector<T>& vec, std::size_t need, Budget& budget) {
  if (need <= vec.capacity())
    return;
  std::size_t target = vec.capacity() != 0 ? vec.capacity() : 1;
  while (target < need) {
    if (target > std::numeric_limits<std::size_t>::max() / 2)
      throw std::overflow_error("tree capacity doubling overflow");
    target *= 2;
  }
  budget.charge_capacity(vec.capacity(), target, sizeof(T));
  vec.reserve(target);
}

}  // namespace

// Exported shared L3 menu rule and public-card list (declared in
// abstract_tree.hpp). Defined OUTSIDE the anonymous namespace so the builder
// and the stage-6 trainer bind to the same external symbol.
std::vector<Action> abstract_node_menu(const GameState& state,
                                       const abstraction::ActionAbstraction& action,
                                       const LegalActions& legal) {
  const std::size_t actor = *state.actor();
  const GamePlayer& hero = state.players()[actor];
  if (state.player_count() == 2) {
    std::size_t opponent = actor;
    for (std::size_t seat : state.live_players())
      if (seat != actor)
        opponent = seat;
    const GamePlayer& other = state.players()[opponent];
    abstraction::MenuContext ctx;
    ctx.street = state.street();
    ctx.pot = state.pot();
    ctx.actor_committed = hero.street_committed;
    ctx.opponent_committed = other.street_committed;
    ctx.opponent_stack = other.stack;
    return action.menu(legal, ctx);
  }
  abstraction::MultiwayMenuContext ctx;
  ctx.street = state.street();
  ctx.pot = state.pot();
  ctx.actor_committed = hero.street_committed;
  ctx.cover = deepest_cover(state, actor);
  return action.multiway_menu(legal, ctx);
}

std::vector<int> public_runout_cards(const GameState& state) {
  std::array<bool, 52> used{};
  for (int card : state.board())
    used[card] = true;
  std::vector<int> cards;
  for (int c = 0; c < 52; ++c)
    if (!used[c])
      cards.push_back(c);
  return cards;
}

// In bs::tree (not the anonymous namespace) so its name matches the
// `friend class TreeBuilder` declaration on AbstractTree.
class TreeBuilder {
 public:
  TreeBuilder(AbstractTree& tree, Budget& budget) : tree_(tree), budget_(budget) {}

  void build() {
    GameState root(tree_.def_);
    // Recursion holds the GameState by value; do it iteratively on an explicit
    // stack so node ids are assigned in stable order and we never risk native
    // stack overflow on a deep tree. Each parent's children vector is appended
    // in ascending edge order even though the work stack is LIFO.
    //
    // Backing capacity is grown ONLY through grow_to_fit, which charges the
    // grown slab before reserving it; hence every retained heap block (node
    // array, terminal array, per-node actions and children, fold payout) is
    // inside the byte cap and the bound is checked ahead of the allocation.
    struct Frame {
      GameState state;
      std::size_t parent;
      std::size_t depth;
      Action via_action{};
      int via_card = kNoCard;
    };
    std::vector<Frame> stack;
    stack.push_back(Frame{std::move(root), kNoNode, 1, {}, kNoCard});
    while (!stack.empty()) {
      Frame frame = std::move(stack.back());
      stack.pop_back();
      TreeNode node;
      node.parent = frame.parent;
      node.street = frame.state.street();
      if (frame.parent != kNoNode) {
        node.incoming_action = frame.via_action;
        node.incoming_card = frame.via_card;
      }
      budget_.add_node(frame.depth);

      const Phase phase = frame.state.phase();
      if (phase == Phase::Folded) {
        node.kind = NodeKind::TerminalFold;
        node.terminal = tree_.terminals_.size();
        TerminalPayload payload = make_terminal(frame.state, true, budget_);
        grow_to_fit(tree_.terminals_, tree_.terminals_.size() + 1, budget_);
        tree_.terminals_.push_back(std::move(payload));
      } else if (phase == Phase::Showdown) {
        node.kind = NodeKind::TerminalShowdown;
        node.terminal = tree_.terminals_.size();
        TerminalPayload payload = make_terminal(frame.state, false, budget_);
        grow_to_fit(tree_.terminals_, tree_.terminals_.size() + 1, budget_);
        tree_.terminals_.push_back(std::move(payload));
      } else if (phase == Phase::Deal) {
        node.kind = NodeKind::Chance;
      } else {
        node.kind = NodeKind::Action;
        node.actor = *frame.state.actor();
        std::vector<Action> menu =
            abstract_node_menu(frame.state, tree_.action_, frame.state.legal());
        // Bounded transient (the L2 builder rejects a menu over 32); charge the
        // slab the node retains before it takes ownership.
        budget_.charge_capacity(0, menu.capacity(), sizeof(Action));
        node.actions = std::move(menu);
      }

      node.index = tree_.nodes_.size();
      grow_to_fit(tree_.nodes_, tree_.nodes_.size() + 1, budget_);
      if (frame.parent != kNoNode) {
        std::vector<std::size_t>& siblings = tree_.nodes_[frame.parent].children;
        grow_to_fit(siblings, siblings.size() + 1, budget_);
        siblings.push_back(node.index);
      }
      tree_.nodes_.push_back(std::move(node));

      // Enqueue children. Reverse the natural order so LIFO popping expands them
      // in ascending edge order; each parent's children vector (appended above
      // in expansion order) then reads in the L1/L2 enumeration order.
      if (phase == Phase::Deal) {
        const std::vector<int> cards = public_runout_cards(frame.state);
        for (auto it = cards.rbegin(); it != cards.rend(); ++it) {
          stack.push_back(Frame{frame.state.after_card(*it), node.index, frame.depth + 1, {}, *it});
        }
      } else if (phase == Phase::Action) {
        const TreeNode& stored = tree_.nodes_[node.index];
        for (auto it = stored.actions.rbegin(); it != stored.actions.rend(); ++it) {
          stack.push_back(Frame{frame.state.after_action(*frame.state.actor(), *it), node.index,
                                frame.depth + 1, *it, kNoCard});
        }
      }
    }
    tree_.depth_ = budget_.depth;
  }

 private:
  AbstractTree& tree_;
  Budget& budget_;
};

AbstractTree::AbstractTree(GameDef def, abstraction::ActionAbstraction action, TreeLimits limits)
    : def_(std::move(def)), action_(std::move(action)), limits_(limits) {
  poker::validate(def_);
  Budget budget(limits_);
  TreeBuilder builder(*this, budget);
  builder.build();
  accounted_bytes_ = budget.bytes();
}

}  // namespace bs::tree
