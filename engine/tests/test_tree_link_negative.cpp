// RFC 0008 stage 4 link-negative guard: this binary links bigshark_tree WITHOUT
// bigshark_solver, proving L3 builds and is usable from L1+L2 alone (a stray
// solver/transitional-adapter symbol in L3 would fail to link). The real L3
// dependency rule (L3 -> L1 + L2 only) is enforced here at link time.
//
// The token guard (L3 sources never name HeadsUpState/HeadsUpRoot, never
// evaluate private holdings, never include a solver header) is checked by the
// build with a CMake configure-time grep; this binary is the runtime half: it
// constructs a tree and drives GameState against it using only L1/L2/L3.
#include <bs/abstract_tree.hpp>
#include <cstdio>

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
}  // namespace

int main() {
  GameDef def{};
  def.player_count = 3;
  def.button = 0;
  def.big_blind = 2;
  def.stacks = {1, 1, 1, 0, 0, 0, 0, 0, 0, 0};
  def.contributions = {1, 1, 1, 0, 0, 0, 0, 0, 0, 0};
  def.pot = 3;
  // 2c = rank0 suit0 = 0; 3d = rank1 suit2 = 1*4+2 = 6; 7h = rank5 suit1 = 21.
  def.board = {0, 6, 21, 0, 0};
  def.board_size = 3;

  // L3 usable with poker + abstraction only; no solver linked into this binary.
  AbstractTree tree(def, abstraction::ActionAbstraction::identity());
  CHECK(tree.size() > 0);
  CHECK(tree.node(tree.root_index()).is_action() || tree.node(tree.root_index()).is_chance());
  CHECK(tree.action_id() == abstraction::identity_action_id());
  CHECK(tree.def().player_count == 3);

  // Drive the rules (L1) against the tree's root menu with no solver involved.
  GameState state(def);
  if (state.phase() == Phase::Action) {
    const TreeNode& root = tree.node(tree.root_index());
    CHECK(root.actor == *state.actor());
    CHECK(!root.actions.empty());
  }

  std::printf("test_tree_link_negative PASS\n");
  return 0;
}
