// RFC 0048 D239 native test: TurnGame table-ownership lifecycle under
// LeakSanitizer. The fixture goes through the REAL TurnGame constructor
// (map_leaves -> precompute_folds -> layout, including the per-node
// regret/sum heap allocations) and its real destructor, with ZERO CFR
// iterations, BR walks, river solves, or substitute allocators. The w1
// cap-1 config is the same stratum identity the original ASan RC8 leak
// receipt was recorded on, and the pinned 3610 turn action nodes equal the
// "3610 objects" allocation count in that LSan stack. Leaf matrix contents
// are never read during construction, so one shared zero table stands in
// for the 432 river continuation matrices; only the non-null pointer
// contract is exercised. Behavioral assertions only, no source-text or log
// matching. Bench-only; production surfaces stay untouched.
#include <array>
#include <bs/eval.hpp>
#include <cstdio>
#include <stdexcept>
#include <vector>

#include "../benchmarks/rfc0047/frontier_enumerator.hpp"
#include "../benchmarks/rfc0047/strata.hpp"
#include "../benchmarks/rfc0047/turn_solver.hpp"

#define CHECK(cond)                                                         \
  do {                                                                      \
    if (!(cond)) {                                                          \
      std::printf("CHECK failed: %s (%s:%d)\n", #cond, __FILE__, __LINE__); \
      return false;                                                         \
    }                                                                       \
  } while (0)

using namespace bs::rfc0047;

namespace {
// The exact allocation count pinned by the original ASan RC8 LSan receipt
// (3610 reg vectors and 3610 sum vectors per TurnGame on this stratum).
constexpr std::size_t kExpectedW1ActionNodes = 3610;
constexpr std::size_t kExpectedW1Leaves = 432;

TurnSolveConfig make_lifecycle_config() {
  const StratumSpec* spec = find_stratum("w1");
  if (spec == nullptr)
    throw std::runtime_error("w1 stratum missing");
  // Tree-walk enumeration only: no river subgame is solved anywhere here.
  const std::vector<FrontierLeaf> leaves = enumerate_frontiers(*spec, 1);
  if (leaves.size() != kExpectedW1Leaves)
    throw std::runtime_error("unexpected w1 frontier leaf count");

  // Construction never dereferences a matrix, so every leaf shares one
  // zero-filled table that satisfies the non-null pointer contract.
  static const std::vector<double> zero_matrix(static_cast<std::size_t>(kRiverC) * kRiverC, 0.0);
  static std::vector<TurnLeafValue> leaf_values;
  leaf_values.clear();
  leaf_values.reserve(leaves.size());
  for (const FrontierLeaf& leaf : leaves)
    leaf_values.push_back({leaf.child_node, leaf.board5, zero_matrix.data()});

  TurnSolveConfig cfg;
  cfg.def = make_stratum_def(*spec, 1);
  cfg.iterations = 0;  // no CFR: lifecycle probe never iterates.
  cfg.linear_averaging = true;
  cfg.threads = 1;
  cfg.leaves = leaf_values;
  return cfg;
}

bool test_real_allocation_counts() {
  const TurnSolveConfig cfg = make_lifecycle_config();
  const TurnGameLifecycleInfo info = turn_game_lifecycle_probe(cfg);
  CHECK(info.turn_action_nodes == kExpectedW1ActionNodes);
  CHECK(info.tree_size > info.turn_action_nodes);
  return true;
}

bool test_repeated_construction_is_bounded() {
  const TurnSolveConfig cfg = make_lifecycle_config();
  // Two back-to-back real construct/destroy cycles. Without ownership
  // cleanup LeakSanitizer accounts both games' tables (2x the pinned
  // count); with the destructor the resident footprint is bounded.
  for (int round = 0; round < 2; ++round) {
    const TurnGameLifecycleInfo info = turn_game_lifecycle_probe(cfg);
    CHECK(info.turn_action_nodes == kExpectedW1ActionNodes);
  }
  return true;
}

}  // namespace

int main() {
  if (!test_real_allocation_counts() || !test_repeated_construction_is_bounded())
    return 1;
  std::printf("rfc0047_turn_lifecycle: all checks passed\n");
  return 0;
}
