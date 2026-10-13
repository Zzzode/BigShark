// RFC 0014: ATF1 tree codec roundtrip test.
//
// Builds a small postflop tree, saves it to a temp file, loads it back, and
// verifies the loaded tree matches the original in structure and content.
// This is a CTest, not a manual benchmark.
#include <array>
#include <bs/abstract_tree.hpp>
#include <bs/abstraction.hpp>
#include <bs/game_definition.hpp>
#include <bs/postflop_config.hpp>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

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
using bs::gto::make_postflop_abstraction;
using bs::gto::make_postflop_def;

// Build a small postflop tree (bucket 0 = SPR 0.5, smallest tree).
std::unique_ptr<AbstractTree> build_test_tree() {
  const std::array<int, 3> flop = {0, 1, 2};  // 2c 2d 2h
  const GameDef def = make_postflop_def(flop, 0);
  const auto abstraction = make_postflop_abstraction(0);
  TreeLimits limits;
  limits.max_bytes = std::size_t{1} << 30;  // 1 GiB
  limits.max_nodes = 2'000'000;
  return std::make_unique<AbstractTree>(def, abstraction, limits);
}

// Verify two trees have identical structure. Returns 0 on success, 1 on failure.
int verify_trees_match(const AbstractTree& a, const AbstractTree& b) {
  CHECK(a.size() == b.size());
  CHECK(a.terminals().size() == b.terminals().size());
  CHECK(a.depth() == b.depth());

  const auto& nodes_a = a.nodes();
  const auto& nodes_b = b.nodes();
  CHECK(nodes_a.size() == nodes_b.size());

  for (std::size_t i = 0; i < nodes_a.size(); ++i) {
    const TreeNode& na = nodes_a[i];
    const TreeNode& nb = nodes_b[i];
    CHECK(na.kind == nb.kind);
    CHECK(na.street == nb.street);
    CHECK(na.actor == nb.actor);
    CHECK(na.children.size() == nb.children.size());
    for (std::size_t c = 0; c < na.children.size(); ++c)
      CHECK(na.children[c] == nb.children[c]);
    CHECK(na.actions.size() == nb.actions.size());
    for (std::size_t ai = 0; ai < na.actions.size(); ++ai) {
      CHECK(na.actions[ai].type == nb.actions[ai].type);
      CHECK(na.actions[ai].target_total == nb.actions[ai].target_total);
    }
    CHECK(na.incoming_action.type == nb.incoming_action.type);
    CHECK(na.incoming_action.target_total == nb.incoming_action.target_total);
    CHECK(na.incoming_card == nb.incoming_card);
    CHECK(na.terminal == nb.terminal);
  }

  // Verify terminal payloads.
  const auto& terms_a = a.terminals();
  const auto& terms_b = b.terminals();
  CHECK(terms_a.size() == terms_b.size());
  for (std::size_t i = 0; i < terms_a.size(); ++i) {
    const TerminalPayload& ta = terms_a[i];
    const TerminalPayload& tb = terms_b[i];
    CHECK(ta.player_count == tb.player_count);
    CHECK(ta.folded == tb.folded);
    CHECK(ta.board_size == tb.board_size);
    for (std::size_t j = 0; j < ta.board_size; ++j)
      CHECK(ta.board[j] == tb.board[j]);
    CHECK(ta.live_count == tb.live_count);
    for (std::size_t j = 0; j < ta.live_count; ++j)
      CHECK(ta.live_order[j] == tb.live_order[j]);
    for (std::size_t s = 0; s < ta.player_count; ++s) {
      CHECK(ta.seats[s].contributed == tb.seats[s].contributed);
      CHECK(ta.seats[s].refunded == tb.seats[s].refunded);
      CHECK(ta.seats[s].stack == tb.seats[s].stack);
      CHECK(ta.seats[s].folded == tb.seats[s].folded);
    }
    if (ta.folded) {
      CHECK(ta.chip_utility.size() == tb.chip_utility.size());
      for (std::size_t j = 0; j < ta.chip_utility.size(); ++j)
        CHECK(ta.chip_utility[j] == tb.chip_utility[j]);
    }
  }
  return 0;
}

}  // namespace

int main() {
  std::printf("=== RFC 0014: ATF1 tree codec roundtrip ===\n");

  // Build a test tree.
  auto original = build_test_tree();
  std::printf("  built tree: %zu nodes, %zu terminals, depth %zu\n", original->size(),
              original->terminals().size(), original->depth());
  CHECK(original->size() > 0);

  // Save to a temp file.
  const std::filesystem::path tmp_path =
      std::filesystem::temp_directory_path() / "bs_test_tree.atf";
  original->save(tmp_path);
  std::printf("  saved to %s\n", tmp_path.string().c_str());

  // Load it back.
  const std::array<int, 3> flop = {0, 1, 2};
  const GameDef def = make_postflop_def(flop, 0);
  const auto abstraction = make_postflop_abstraction(0);
  TreeLimits limits;
  limits.max_bytes = std::size_t{1} << 30;
  limits.max_nodes = 2'000'000;
  auto loaded = AbstractTree::load(tmp_path, def, abstraction, limits);
  std::printf("  loaded: %zu nodes, %zu terminals, depth %zu\n", loaded.size(),
              loaded.terminals().size(), loaded.depth());

  // Verify structure.
  if (verify_trees_match(*original, loaded) != 0)
    return 1;
  std::printf("  structure match: OK\n");

  // Clean up.
  std::error_code ec;
  std::filesystem::remove(tmp_path, ec);

  std::printf("  ALL CHECKS PASSED\n");
  return 0;
}
