// RFC 0008 stage 4 L3 fidelity oracle.
//
// This is an INDEPENDENT enumerator: it consumes the built AbstractTree but
// derives the expected structure itself from raw GameState, never calling
// ActionAbstraction::menu / build_action_menu / build_multiway_action_menu.
// At each node it independently classifies kind/actor/street, independently
// codes the fold/check/call prefix, ceil pot fractions, min/max-cover clamp,
// sort/unique menu, independently lists the public chance set, and checks fold
// payout vectors against GameState::settle_fold and showdown ledger contents.
//
// It runs the SAME GameState expansion the builder does and walks the built
// tree in lockstep, so a node the builder fails to emit, a wrong menu element,
// a wrong chance child, or a wrong terminal kind/payout cannot pass. Exact node
// counts are CHECKs (never prints), at BOTH 2 and 3 seats.
#include <algorithm>
#include <array>
#include <bs/abstract_tree.hpp>
#include <bs/eval.hpp>
#include <cstdio>
#include <string>
#include <vector>

#define CHECK(cond)                                                         \
  do {                                                                      \
    if (!(cond)) {                                                          \
      std::printf("CHECK failed: %s (%s:%d)\n", #cond, __FILE__, __LINE__); \
      return 1;                                                             \
    }                                                                       \
  } while (0)

namespace {

using namespace bs;
using namespace bs::tree;
using namespace bs::poker;
using bs::abstraction::default_size_schedule;
using bs::abstraction::Fraction;
using bs::abstraction::SizeSchedule;
using bs::abstraction::StreetSizes;

int card(const char* name) {
  return cardId(std::string(name));
}

// ---- independently coded menu rule (no L2 builder call) -------------------
// Mirrors the shipped rule from first principles: ceil(a/b) for uint64.
Chips ceil_div(Chips a, Chips b) {
  return a / b + (a % b != 0 ? Chips{1} : Chips{0});
}
Chips ceil_frac(Chips amount, const Fraction& f) {
  return ceil_div(amount * f.numerator, f.denominator);
}

// Deepest cover over live non-folded seats other than actor (3+ rule); the
// two-seat caller passes exactly one such seat.
Chips deepest_cover(const GameState& s, std::size_t actor) {
  Chips cover = 0;
  for (std::size_t seat : s.live_players()) {
    if (seat == actor)
      continue;
    const auto& p = s.players()[seat];
    cover = std::max(cover, p.street_committed + p.stack);
  }
  return cover;
}

// Independent ordered menu from raw legal(); never touches L2's builder.
std::vector<Action> independent_menu(const GameState& s, const LegalActions& legal,
                                     const SizeSchedule& schedule) {
  std::vector<Action> out;
  if (legal.fold)
    out.push_back({ActionType::Fold});
  if (legal.check)
    out.push_back({ActionType::Check});
  if (legal.call)
    out.push_back({ActionType::Call});
  if (!legal.aggressive)
    return out;
  const std::size_t actor = *s.actor();
  const TargetRange& b = *legal.aggressive;
  const Chips cover = deepest_cover(s, actor);
  const Chips cap = std::max(b.minimum, std::min(b.maximum, cover));
  const Chips base = s.players()[actor].street_committed + legal.call_amount;
  const Chips pot_after = s.pot() + legal.call_amount;
  std::vector<Chips> targets{b.minimum, cap};
  const StreetSizes& st = schedule[static_cast<std::size_t>(s.street())];
  const auto& fracs = (b.type == ActionType::Bet) ? st.bets : st.raises;
  for (const Fraction& f : fracs) {
    Chips t = base + ceil_frac(pot_after, f);
    targets.push_back(std::clamp(t, b.minimum, cap));
  }
  std::sort(targets.begin(), targets.end());
  targets.erase(std::unique(targets.begin(), targets.end()), targets.end());
  for (Chips t : targets)
    out.push_back({b.type, t});
  return out;
}

std::vector<int> independent_chance(const GameState& s) {
  std::array<bool, 52> used{};
  for (int c : s.board())
    used[c] = true;
  std::vector<int> cards;
  for (int c = 0; c < 52; ++c)
    if (!used[c])
      cards.push_back(c);
  return cards;
}

struct Counts {
  long nodes = 0, action = 0, chance = 0, fold = 0, showdown = 0;
  long menu_mismatches = 0, chance_mismatches = 0;
  long fold_payout_bad = 0, edge_bad = 0;
};

// DFS lockstep: walk the independent GameState expansion following the built
// node's indexed children. Each frame pairs (tree node index, GameState).
int verify_node(const AbstractTree& tree, const SizeSchedule& schedule, Counts& c, std::size_t ni,
                const GameState& state, std::size_t depth) {
  const TreeNode& n = tree.node(ni);
  ++c.nodes;
  if (depth > 64) {
    std::printf("depth runaway\n");
    return 1;
  }

  // Every Action/Chance child edge must be bidirectional.
  for (std::size_t child : n.children) {
    if (tree.node(child).parent != ni) {
      ++c.edge_bad;
      std::printf("child %zu does not point back to %zu\n", child, ni);
      return 1;
    }
  }

  if (state.phase() == Phase::Folded) {
    CHECK(n.kind == NodeKind::TerminalFold);
    ++c.fold;
    CHECK(n.terminal != kNoNode);
    const TerminalPayload& tp = tree.terminal(n.terminal);
    CHECK(tp.folded);
    // Fold payout vector must equal the L1 settlement.
    const auto settled = state.settle_fold();
    if (tp.chip_utility != settled.chip_utility) {
      ++c.fold_payout_bad;
      std::printf("fold payout vector mismatch at node %zu\n", ni);
      return 1;
    }
    CHECK(n.children.empty());
    return 0;
  }
  if (state.phase() == Phase::Showdown) {
    CHECK(n.kind == NodeKind::TerminalShowdown);
    ++c.showdown;
    CHECK(n.terminal != kNoNode);
    const TerminalPayload& tp = tree.terminal(n.terminal);
    CHECK(!tp.folded);
    // Ledger: live-order ascending and board present.
    CHECK(tp.live_count == state.live_players().size());
    for (std::size_t i = 0; i < tp.live_count; ++i)
      CHECK(tp.live_order[i] == state.live_players()[i]);
    CHECK(tp.board_size == state.board().size());
    for (std::size_t i = 0; i < state.board().size(); ++i)
      CHECK(tp.board[i] == state.board()[i]);
    // Folded seats must be present with folded=true (N-way side pots need them).
    bool saw_folded_ledger = false;
    for (std::size_t seat = 0; seat < state.player_count(); ++seat) {
      if (tp.seats[seat].contributed != state.players()[seat].contributed ||
          tp.seats[seat].refunded != state.players()[seat].refunded ||
          tp.seats[seat].stack != state.players()[seat].stack ||
          tp.seats[seat].folded != state.players()[seat].folded) {
        std::printf("showdown ledger seat %zu mismatch at node %zu\n", seat, ni);
        return 1;
      }
      if (tp.seats[seat].folded)
        saw_folded_ledger = true;
    }
    (void)saw_folded_ledger;
    CHECK(n.children.empty());
    return 0;
  }
  if (state.phase() == Phase::Deal) {
    CHECK(n.kind == NodeKind::Chance);
    ++c.chance;
    const std::vector<int> want = independent_chance(state);
    if (n.children.size() != want.size()) {
      ++c.chance_mismatches;
      std::printf("chance degree node %zu: tree %zu want %zu (board %zu)\n", ni, n.children.size(),
                  want.size(), state.board().size());
      return 1;
    }
    // The tree's chance children carry the concrete card in ascending order.
    for (std::size_t i = 0; i < want.size(); ++i) {
      const TreeNode& child = tree.node(n.children[i]);
      if (child.incoming_card != want[i]) {
        ++c.chance_mismatches;
        std::printf("chance card order node %zu idx %zu: got %d want %d\n", ni, i,
                    child.incoming_card, want[i]);
        return 1;
      }
      if (verify_node(tree, schedule, c, n.children[i], state.after_card(want[i]), depth + 1) != 0)
        return 1;
    }
    return 0;
  }

  // Action node.
  CHECK(n.kind == NodeKind::Action);
  ++c.action;
  CHECK(n.actor == *state.actor());
  const LegalActions legal = state.legal();
  const std::vector<Action> want_menu = independent_menu(state, legal, default_size_schedule());
  if (n.actions != want_menu) {
    ++c.menu_mismatches;
    std::printf("menu mismatch at node %zu (street %d actor %zu): tree %zu want %zu\n", ni,
                (int)state.street(), n.actor, n.actions.size(), want_menu.size());
    return 1;
  }
  CHECK(n.children.size() == want_menu.size());
  const std::size_t actor = *state.actor();
  for (std::size_t i = 0; i < want_menu.size(); ++i) {
    const TreeNode& child = tree.node(n.children[i]);
    if (!(child.incoming_action == want_menu[i])) {
      std::printf("action edge label mismatch node %zu idx %zu\n", ni, i);
      return 1;
    }
    if (verify_node(tree, schedule, c, n.children[i], state.after_action(actor, want_menu[i]),
                    depth + 1) != 0)
      return 1;
  }
  return 0;
}

// Two-seat fixture (rooted flop). The independent menu in verify_node already
// reproduces the shipped RFC 0007 rule from raw legal(); the equality of that
// independent menu to the solver's abstract_actions is asserted separately in
// the L4 conformance (which owns the GameDef->HeadsUpRoot projection).
GameDef two_seat_rooted(std::size_t stacks0, std::size_t stacks1) {
  GameDef d{};
  d.player_count = 2;
  d.button = 1;
  d.big_blind = 1;
  d.stacks = {stacks0, stacks1, 0, 0, 0, 0, 0, 0, 0, 0};
  d.contributions = {1, 1, 0, 0, 0, 0, 0, 0, 0, 0};
  d.pot = 2;
  d.board = {card("2c"), card("3d"), card("7h"), 0, 0};
  d.board_size = 3;
  return d;
}

GameDef three_seat_rooted(std::array<Chips, 3> stacks) {
  GameDef d{};
  d.player_count = 3;
  d.button = 0;
  d.big_blind = 2;
  for (std::size_t i = 0; i < 3; ++i)
    d.stacks[i] = stacks[i];
  d.contributions = {1, 1, 1, 0, 0, 0, 0, 0, 0, 0};
  d.pot = 3;
  d.board = {card("2c"), card("3d"), card("7h"), 0, 0};
  d.board_size = 3;
  return d;
}

}  // namespace

int main() {
  SizeSchedule schedule = default_size_schedule();

  // --- Two-seat rooted flop, stack 1 (the same shape family as the solver
  // goldens). Pinned counts from an independent measurement. ---
  {
    GameDef def = two_seat_rooted(1, 1);
    AbstractTree tree(def, bs::abstraction::ActionAbstraction::identity());
    Counts c;
    GameState root(def);
    if (verify_node(tree, schedule, c, tree.root_index(), root, 1) != 0)
      return 1;
    std::printf("2p: nodes=%ld A=%ld C=%ld F=%ld S=%ld\n", c.nodes, c.action, c.chance, c.fold,
                c.showdown);
    CHECK(c.nodes == 31124);
    CHECK(c.action == 9608);
    CHECK(c.chance == 248);
    CHECK(c.fold == 4804);
    CHECK(c.showdown == 16464);
    CHECK(c.menu_mismatches == 0 && c.chance_mismatches == 0 && c.fold_payout_bad == 0);
    // Every node was visited.
    CHECK(static_cast<std::size_t>(c.nodes) == tree.size());
    // Chance partial-board coverage: a rooted flop starts at board size 3; the
    // turn chance children are 49 (52-3), and river 48. Assert a turn node and
    // river node with those exact degrees exist.
    bool saw_turn_49 = false, saw_river_48 = false;
    for (const TreeNode& n : tree.nodes()) {
      if (n.is_chance() && n.children.size() == 49)
        saw_turn_49 = true;
      if (n.is_chance() && n.children.size() == 48)
        saw_river_48 = true;
    }
    CHECK(saw_turn_49);
    CHECK(saw_river_48);
  }

  // --- Three-seat rooted flop, stack 1 each: exercises the existential
  // multiway legal rule and the max-cover menu, plus mid-hand folds (fold does
  // not end the hand at 3 seats). ---
  {
    GameDef def = three_seat_rooted({1, 1, 1});
    AbstractTree tree(def, bs::abstraction::ActionAbstraction::identity());
    Counts c;
    GameState root(def);
    if (verify_node(tree, schedule, c, tree.root_index(), root, 1) != 0)
      return 1;
    std::printf("3p s1: nodes=%ld A=%ld C=%ld F=%ld S=%ld\n", c.nodes, c.action, c.chance, c.fold,
                c.showdown);
    CHECK(c.nodes == 102827);
    CHECK(c.action == 28824);
    CHECK(c.chance == 941);
    CHECK(c.fold == 7206);
    CHECK(c.showdown == 65856);
    CHECK(c.menu_mismatches == 0 && c.chance_mismatches == 0 && c.fold_payout_bad == 0);
    CHECK(static_cast<std::size_t>(c.nodes) == tree.size());
    // Multiway fold does NOT end the hand: a player folds and the others carry
    // on to a showdown. At 3 seats stack 1 there are both 2-way showdowns
    // (one seat folded mid-hand, its gross contribution still in the ledger)
    // and 3-way showdowns (nobody folded). This is the mid-hand-fold coverage;
    // a fold TERMINAL itself always has exactly one surviving seat.
    long two_way_with_a_folder = 0, three_way = 0;
    for (const TerminalPayload& tp : tree.terminals()) {
      if (tp.folded)
        continue;
      int folders = 0;
      for (std::size_t seat = 0; seat < def.player_count; ++seat)
        folders += tp.seats[seat].folded ? 1 : 0;
      if (tp.live_count == 2 && folders == 1)
        ++two_way_with_a_folder;
      if (tp.live_count == 3 && folders == 0)
        ++three_way;
    }
    CHECK(two_way_with_a_folder == 42336);
    CHECK(three_way == 23520);
  }

  // --- Three-seat asymmetric stacks {1,1,2}: a deeper seat survives, varying
  // the cover. Pinned from independent measurement. ---
  {
    GameDef def = three_seat_rooted({1, 1, 2});
    AbstractTree tree(def, bs::abstraction::ActionAbstraction::identity());
    Counts c;
    GameState root(def);
    if (verify_node(tree, schedule, c, tree.root_index(), root, 1) != 0)
      return 1;
    std::printf("3p s{1,1,2}: nodes=%ld F=%ld S=%ld\n", c.nodes, c.fold, c.showdown);
    CHECK(c.nodes == 162316);
    CHECK(c.fold == 7206);
    CHECK(c.showdown == 79968);
    CHECK(static_cast<std::size_t>(c.nodes) == tree.size());
  }

  // --- Three-seat asymmetric stacks {2,3,3}: the two deep seats can reach a
  // HIGHER total than the short seat AND both totals clear the minimum bet, so
  // deepest-cover is observable in an actor's MENU. At the root (flop, actor
  // seat 1, no bet yet) the first other live seat is the short seat 0 (cover
  // 2) while the max cover is seat 2 (cover 3); cover 2 yields {check, bet 2}
  // and cover 3 yields {check, bet 2, bet 3}. A first-cover mutant therefore
  // changes node 0's menu and cannot pass. The shallower {1,1,2}/{1,2,2}
  // shapes do NOT cover this: there the short cover sits at or below the
  // minimum, so max(minimum, ...) floors the cap and both rules agree. Pinned
  // from independent measurement. ---
  {
    GameDef def = three_seat_rooted({2, 3, 3});
    AbstractTree tree(def, bs::abstraction::ActionAbstraction::identity());
    Counts c;
    GameState root(def);
    if (verify_node(tree, schedule, c, tree.root_index(), root, 1) != 0)
      return 1;
    std::printf("3p s{2,3,3}: nodes=%ld A=%ld C=%ld F=%ld S=%ld depth=%zu\n", c.nodes, c.action,
                c.chance, c.fold, c.showdown, tree.depth());
    CHECK(c.nodes == 547535);
    CHECK(c.action == 216752);
    CHECK(c.chance == 3309);
    CHECK(c.fold == 35826);
    CHECK(c.showdown == 291648);
    CHECK(c.menu_mismatches == 0 && c.chance_mismatches == 0 && c.fold_payout_bad == 0);
    CHECK(static_cast<std::size_t>(c.nodes) == tree.size());
    CHECK(tree.depth() == 15);
    long two_way_with_a_folder = 0, three_way = 0;
    for (const TerminalPayload& tp : tree.terminals()) {
      if (tp.folded)
        continue;
      int folders = 0;
      for (std::size_t seat = 0; seat < def.player_count; ++seat)
        folders += tp.seats[seat].folded ? 1 : 0;
      if (tp.live_count == 2 && folders == 1)
        ++two_way_with_a_folder;
      if (tp.live_count == 3 && folders == 0)
        ++three_way;
    }
    CHECK(two_way_with_a_folder == 183456);
    CHECK(three_way == 108192);
  }

  // --- TreeLimits: a node cap below the true size throws the typed, NON
  // invalid_argument exhaustion and leaves no tree. ---
  {
    GameDef def = two_seat_rooted(1, 1);
    TreeLimits lim;
    lim.max_nodes = 100;
    bool threw_typed = false;
    bool threw_invalid = false;
    try {
      AbstractTree tree(def, bs::abstraction::ActionAbstraction::identity(), lim);
      (void)tree;
    } catch (const tree_resource_exhausted&) {
      threw_typed = true;
    } catch (const std::invalid_argument&) {
      threw_invalid = true;
    }
    CHECK(threw_typed);
    CHECK(!threw_invalid);
  }

  // --- Depth cap: a cap below the pinned depth throws the typed exhaustion.
  // The {2,3,3} tree reaches depth 15, so a cap of 4 must be crossed while the
  // node and byte caps stay permissive (isolating the depth check). ---
  {
    GameDef def = three_seat_rooted({2, 3, 3});
    TreeLimits lim;
    lim.max_nodes = 10'000'000;
    lim.max_depth = 4;
    lim.max_bytes = std::size_t{1} << 40;
    bool threw_typed = false, threw_invalid = false;
    try {
      AbstractTree tree(def, bs::abstraction::ActionAbstraction::identity(), lim);
      (void)tree;
    } catch (const tree_resource_exhausted&) {
      threw_typed = true;
    } catch (const std::invalid_argument&) {
      threw_invalid = true;
    }
    CHECK(threw_typed);
    CHECK(!threw_invalid);
  }

  // --- Byte cap: a tight byte bound throws the typed exhaustion while node
  // and depth caps stay permissive (isolating the byte check). The bound is
  // charged BEFORE the crossing slab, so the constructor fails. ---
  {
    GameDef def = two_seat_rooted(1, 1);
    TreeLimits lim;
    lim.max_nodes = 10'000'000;
    lim.max_depth = 256;
    lim.max_bytes = 1024;
    bool threw_typed = false, threw_invalid = false;
    try {
      AbstractTree tree(def, bs::abstraction::ActionAbstraction::identity(), lim);
      (void)tree;
    } catch (const tree_resource_exhausted&) {
      threw_typed = true;
    } catch (const std::invalid_argument&) {
      threw_invalid = true;
    }
    CHECK(threw_typed);
    CHECK(!threw_invalid);
  }

  // --- Preflop honest exhaustion: a zero-action all-in-blind preflop root is a
  // pure public-card tree. The flop is filled one card at a time, so the full
  // ordered five-card runout has 52*51*50*49*48 = 311,875,200 river leaves,
  // already far past the DEFAULT 2,000,000 node cap, so a full unconditioned
  // preflop tree is not materializable and construction must fail with the
  // typed exhaustion rather than a truncated tree. The fan-out is pinned as a
  // constant (cheap), and a small custom cap exercises the throw without
  // allocating the full tree in this suite. ---
  {
    constexpr std::size_t river_leaves =
        std::size_t{52} * 51 * 50 * 49 * 48;  // ordered public board runouts
    CHECK(river_leaves == 311'875'200);
    CHECK(river_leaves > TreeLimits{}.max_nodes);

    GameDef def{};
    def.player_count = 2;
    def.button = 1;
    def.big_blind = 2;
    def.stacks = {2, 2, 0, 0, 0, 0, 0, 0, 0, 0};
    def.contributions = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    def.pot = 2;
    def.board = {-1, -1, -1, 0, 0};
    def.board_size = 0;
    def.preflop = true;
    def.blinds_posted = {2, 1, 0, 0, 0, 0, 0, 0, 0, 0};
    TreeLimits lim;
    lim.max_nodes = 100'000;  // far below the true 6.5M chance fan-out
    bool threw_typed = false, threw_invalid = false;
    try {
      AbstractTree tree(def, bs::abstraction::ActionAbstraction::identity(), lim);
      (void)tree;
    } catch (const tree_resource_exhausted&) {
      threw_typed = true;
    } catch (const std::invalid_argument&) {
      threw_invalid = true;
    }
    CHECK(threw_typed);
    CHECK(!threw_invalid);
  }

  std::printf("test_abstract_tree_fidelity PASS\n");
  return 0;
}
