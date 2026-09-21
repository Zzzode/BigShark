// RFC 0008 stage 3 equivalence gate: the ordered action menu lifted into the L2
// abstraction component must be bit-for-bit identical to the shipped solver
// `abstract_actions` on every reachable state of bounded heads-up games. Equal
// menus mean the solver builds the identical abstract game, so the exact Nash
// equilibrium it computes -- and therefore the identity abstraction's measured
// exploitability error -- is unchanged. This is the stage-3 gate "the existing
// size schedules reproduce their current decisions" at the rule level; the
// replay suite and the pinned test_heads_up_solver sizing vectors cover the
// end-to-end decision level.
#include <array>
#include <bs/abstraction.hpp>
#include <bs/eval.hpp>
#include <bs/heads_up.hpp>
#include <bs/heads_up_solver.hpp>
#include <cstdio>
#include <string>
#include <vector>

using namespace bs;
using namespace bs::poker;
using namespace bs::solver;

#define CHECK(cond)                                                         \
  do {                                                                      \
    if (!(cond)) {                                                          \
      std::printf("CHECK failed: %s (%s:%d)\n", #cond, __FILE__, __LINE__); \
      return 1;                                                             \
    }                                                                       \
  } while (0)

namespace {

int card(const char* name) {
  return cardId(std::string(name));
}

// Pinned node totals across the bounded trees. The flop totals are summed over
// stacks {2,4,12}; the preflop totals come from one deep stack-10 preflop root.
// The identity flop (2720) + custom flop (1384) reproduce the pre-stage-3
// shipped total of 4104; the preflop subtree covers Street::Preflop itself and,
// crucially, a nonzero number of INTERIOR nodes whose fraction-derived target
// sits strictly inside the legal interval (only a deep stack provides these --
// a shallow all-in-only root clamps every fraction to one seed).
constexpr long kIdentityFlopNodes = 2720;
constexpr long kIdentityPreflopNodes = 1156;
constexpr long kIdentityPreflopStreetNodes = 20;
constexpr long kIdentityPreflopInteriorNodes = 2;
constexpr long kCustomFlopNodes = 1384;
constexpr long kCustomPreflopNodes = 908;
constexpr long kCustomPreflopStreetNodes = 20;
constexpr long kCustomPreflopInteriorNodes = 2;

abstraction::MenuContext context_of(const HeadsUpState& state) {
  const auto actor = *state.actor();
  abstraction::MenuContext ctx;
  ctx.street = state.street();
  ctx.pot = state.pot();
  ctx.actor_committed = state.players()[actor].street_committed;
  ctx.opponent_committed = state.players()[1 - actor].street_committed;
  ctx.opponent_stack = state.players()[1 - actor].stack;
  return ctx;
}

int compare_menus(const HeadsUpState& state, const abstraction::ActionAbstraction& abs) {
  const auto shipped = abstract_actions(state, abs.schedule());
  const auto lifted = abs.menu(state.legal(), context_of(state));
  if (shipped != lifted) {
    std::printf("menu mismatch\n");
    std::printf(" shipped:");
    for (const auto& a : shipped)
      std::printf(" %d/%llu", static_cast<int>(a.type),
                  static_cast<unsigned long long>(a.target_total));
    std::printf("\n lifted:");
    for (const auto& a : lifted)
      std::printf(" %d/%llu", static_cast<int>(a.type),
                  static_cast<unsigned long long>(a.target_total));
    std::printf("\n");
    return 1;
  }
  return 0;
}

// A fixed, board-only deck: public cards come from a reserved pool disjoint
// from the flop, so every abstracted game is a legal, fully enumerable
// validation tree. Holes are never needed to enumerate the betting tree.
const std::array<int, 5> kRunout{{card("2d"), card("2h"), card("2s"), card("3c"), card("4c")}};

int walk(HeadsUpState state, const abstraction::ActionAbstraction& abs, int depth, long& nodes,
         long& preflop_street_nodes, long& preflop_interior_nodes) {
  if (depth > 24)
    return 0;
  if (state.phase() == Phase::Deal) {
    // Index by current board size: a preflop root fills 0->1->2 for the flop,
    // then 3 (turn) and 4 (river); a flop-rooted tree starts at 3. This is the
    // one expression that stays valid for both roots (board.size()-3 would read
    // before the array at the preflop root).
    const int c = kRunout[state.board().size()];
    return walk(state.after_card(c), abs, depth + 1, nodes, preflop_street_nodes,
                preflop_interior_nodes);
  }
  if (state.phase() != Phase::Action)
    return 0;
  ++nodes;
  if (state.street() == Street::Preflop) {
    ++preflop_street_nodes;
    // A preflop node only exercises the fraction lists if some declared target
    // lands strictly inside the legal interval. On a shallow all-in-only node
    // (min == cap) every fraction clamps to the single seed and the Preflop=3
    // list cannot influence the menu; a deep stack opens interior targets.
    const LegalActions legal = state.legal();
    if (legal.aggressive && legal.aggressive->minimum < legal.aggressive->maximum) {
      const Chips lo = legal.aggressive->minimum;
      const Chips hi = legal.aggressive->maximum;
      for (const Action& a : abstract_actions(state, abs.schedule())) {
        if ((a.type == ActionType::Bet || a.type == ActionType::Raise) && a.target_total > lo &&
            a.target_total < hi) {
          ++preflop_interior_nodes;
          break;
        }
      }
    }
  }
  if (compare_menus(state, abs) != 0)
    return 1;
  // Drive one branch per menu action: the menu is the abstraction's full legal
  // choice set, so enumerating it covers every action the solver can take.
  for (const Action& action : abstract_actions(state, abs.schedule())) {
    if (walk(state.after_action(*state.actor(), action), abs, depth + 1, nodes,
             preflop_street_nodes, preflop_interior_nodes) != 0)
      return 1;
  }
  return 0;
}

HeadsUpRoot flop_root(Chips stack) {
  // Flop root: board 2c 3d 7h, closed-street contributions equal, button 1.
  return HeadsUpRoot{
      {card("2c"), card("3d"), card("7h")}, {stack, stack}, {1, 1}, 2, 2, 1, false, {0, 0}};
}

HeadsUpRoot preflop_root(Chips stack) {
  // Preflop root: no flop, posted blinds (button posts SB=1, other BB=2),
  // button 0 acts first; the BB keeps its option. This exercises the Preflop=3
  // size-schedule entry the flop-rooted walk never indexes.
  HeadsUpRoot root{};
  root.flop = {-1, -1, -1};  // unset sentinel: card 0 is the real 2c
  root.stacks = {stack, stack};
  root.contributions = {0, 0};
  root.pot = 3;
  root.big_blind = 2;  // button posts the derived SB=1
  root.button = 0;
  root.preflop = true;
  root.blinds_posted = {1, 2};
  return root;
}

int run_schedule(const SizeSchedule& sizes, const char* label, long& flop_nodes,
                 long& preflop_nodes, long& preflop_street_nodes, long& preflop_interior_nodes) {
  // Tiny stacks bound the tree while still exercising every street and short
  // all-in collapses.
  flop_nodes = 0;
  long flop_preflop_street = 0;
  long flop_preflop_interior = 0;
  for (Chips stack : {Chips{2}, Chips{4}, Chips{12}}) {
    HeadsUpGame game;
    game.root = flop_root(stack);
    game.sizes = sizes;
    if (walk(HeadsUpState(game.root), abstraction::ActionAbstraction(sizes), 0, flop_nodes,
             flop_preflop_street, flop_preflop_interior) != 0) {
      std::printf("schedule %s diverged at flop stack %llu\n", label,
                  static_cast<unsigned long long>(stack));
      return 1;
    }
  }
  // A flop-rooted tree can never index the Preflop=3 schedule entry.
  if (flop_preflop_street != 0 || flop_preflop_interior != 0) {
    std::printf("schedule %s saw a preflop-street node from a flop root\n", label);
    return 1;
  }
  // A DEEP-stack (10 chips = 5 BB at 1/2) preflop root is essential: a shallow
  // stack makes every preflop interval all-in-only (minimum == cap), so the
  // declared preflop fractions clamp to one seed and cannot influence a menu.
  // At stack 10 the opener (and the big-blind option) have an interior raise
  // target 8 strictly between the minimum 4 and the all-in 10, making the
  // Preflop=3 fraction lists load-bearing while keeping the tree bounded.
  preflop_nodes = 0;
  preflop_street_nodes = 0;
  preflop_interior_nodes = 0;
  {
    HeadsUpGame game;
    game.root = preflop_root(10);
    game.sizes = sizes;
    if (walk(HeadsUpState(game.root), abstraction::ActionAbstraction(sizes), 0, preflop_nodes,
             preflop_street_nodes, preflop_interior_nodes) != 0) {
      std::printf("schedule %s diverged at preflop root\n", label);
      return 1;
    }
  }
  return 0;
}

}  // namespace

int main() {
  // Identity schedule: the shipped RFC 0007 defaults.
  const auto identity = abstraction::ActionAbstraction::identity();
  CHECK(identity.id() == abstraction::identity_action_id());
  CHECK(identity.schedule() == default_size_schedule());

  long identity_flop = 0;
  long identity_preflop = 0;
  long identity_preflop_street = 0;
  long identity_preflop_interior = 0;
  if (run_schedule(default_size_schedule(), "identity-default", identity_flop, identity_preflop,
                   identity_preflop_street, identity_preflop_interior) != 0)
    return 1;

  // A deliberately non-default schedule (different fractions, an empty raise
  // list on one street) proves the lifted builder follows the declared id's
  // parameters, not a hidden constant, and still matches the solver exactly.
  SizeSchedule custom = default_size_schedule();
  custom[0].bets = {{1, 2}, {1, 1}};
  custom[0].raises = {{1, 1}};
  custom[2].bets = {{2, 1}};
  long custom_flop = 0;
  long custom_preflop = 0;
  long custom_preflop_street = 0;
  long custom_preflop_interior = 0;
  if (run_schedule(custom, "custom", custom_flop, custom_preflop, custom_preflop_street,
                   custom_preflop_interior) != 0)
    return 1;

  // Exact, pinned node counts. These are not just printed: a menu-builder
  // change that silently grows or shrinks an abstract tree (a lost cap, a
  // duplicate fraction target, an off-by-one in rounding) shifts a count and
  // fails the gate. The preflop-street pins prove the Preflop=3 entry is
  // reached; the strictly-positive INTERIOR pin is the load-bearing part -- it
  // guarantees at least one compared preflop menu contains a fraction-derived
  // target strictly inside the legal interval, which a shallow all-in-only root
  // can never provide.
  std::printf(
      "identity flop=%ld preflop=%ld (preflop-street %ld, interior %ld) | custom flop=%ld "
      "preflop=%ld (preflop-street %ld, interior %ld)\n",
      identity_flop, identity_preflop, identity_preflop_street, identity_preflop_interior,
      custom_flop, custom_preflop, custom_preflop_street, custom_preflop_interior);
  CHECK(identity_flop == kIdentityFlopNodes);
  CHECK(identity_preflop == kIdentityPreflopNodes);
  CHECK(identity_preflop_street == kIdentityPreflopStreetNodes);
  CHECK(identity_preflop_interior == kIdentityPreflopInteriorNodes);
  CHECK(custom_flop == kCustomFlopNodes);
  CHECK(custom_preflop == kCustomPreflopNodes);
  CHECK(custom_preflop_street == kCustomPreflopStreetNodes);
  CHECK(custom_preflop_interior == kCustomPreflopInteriorNodes);
  // The custom schedule edits ONLY postflop fractions, so the Street::Preflop
  // states and their interior targets reached from a preflop root must be
  // identical; the postflop subtree reached after the flop deals does differ
  // (hence custom_preflop total != identity_preflop total), which is expected.
  CHECK(custom_preflop_street == identity_preflop_street);
  CHECK(custom_preflop_interior == identity_preflop_interior);
  // The schedule entry that differs is genuinely covered on the flop.
  CHECK(custom_flop != identity_flop);

  std::printf("test_abstraction_equivalence PASS\n");
  return 0;
}
