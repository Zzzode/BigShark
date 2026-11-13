// RFC 0048 D202 native test: the per-node mixed-menu (selective widening)
// mechanism on enumerable tiny river roots. Real seedless CFR+ solves of a
// mask-defined restricted game and behavioral assertions only; no
// source-text or log matching. The frozen cap-0 reference, the D184/D192/
// D195 instruments, and every production surface stay untouched.
#include <algorithm>
#include <array>
#include <bs/abstract_tree.hpp>
#include <bs/abstraction.hpp>
#include <bs/eval.hpp>
#include <cmath>
#include <cstdio>
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

#include "../benchmarks/rfc0047/mixed_menu.hpp"
#include "../benchmarks/rfc0047/patha_grid_budget.hpp"
#include "../benchmarks/rfc0047/policy_hash.hpp"
#include "../benchmarks/rfc0047/reach_bound.hpp"
#include "../benchmarks/rfc0047/river_solver.hpp"
#include "../benchmarks/rfc0047/union_menu.hpp"

#define CHECK(cond)                                                         \
  do {                                                                      \
    if (!(cond)) {                                                          \
      std::printf("CHECK failed: %s (%s:%d)\n", #cond, __FILE__, __LINE__); \
      return false;                                                         \
    }                                                                       \
  } while (0)

using namespace bs::rfc0047;
using bs::abstraction::ActionAbstraction;
using bs::tree::AbstractTree;
using bs::tree::tree_resource_exhausted;
using bs::tree::TreeLimits;

namespace {
constexpr std::uint32_t kC = 1081;
constexpr std::uint64_t kIters = 60;
constexpr double kChipTol = 1e-9;
constexpr double kDistTol = 1e-12;
constexpr double kOracleTol = 1e-6;

// Hand-built shallow street-start river root (same family as T13/A3):
// pot 4, 8 chips behind, enumerable menus.
bs::poker::GameDef river_root(std::uint64_t stack_behind) {
  bs::poker::GameDef def{};
  def.player_count = 2;
  def.button = 0;
  def.big_blind = 2;
  def.ante = 0;
  def.stacks = {stack_behind, stack_behind, 0, 0, 0, 0, 0, 0, 0, 0};
  def.contributions = {2, 2, 0, 0, 0, 0, 0, 0, 0, 0};
  def.pot = 4;
  const std::array<int, 5> board = {bs::cardId("2c"), bs::cardId("3d"), bs::cardId("7h"),
                                    bs::cardId("Js"), bs::cardId("9c")};
  def.board = board;
  def.board_size = 5;
  def.preflop = false;
  def.terminal = bs::poker::TerminalDepth::River;
  return def;
}

struct Fixture {
  bs::poker::GameDef def;
  ActionAbstraction coarse;
  ActionAbstraction reference;
};

Fixture make_fixture(std::uint64_t stack_behind = 8) {
  return {river_root(stack_behind), make_grid_abstraction(SizingGrid::Coarse, 1),
          ActionAbstraction::identity(0)};
}

bool throws_invalid(const std::function<void()>& f) {
  try {
    f();
  } catch (const std::invalid_argument&) {
    return true;
  }
  return false;
}

// M1: raw mask validation rejects every malformed shape and the passive-edge
// removal, and accepts the pre-selected candidate.
bool test_m1_mask_validation() {
  const Fixture f = make_fixture();
  const AbstractTree ref_tree(f.def, f.reference);
  const AbstractTree coarse_tree(f.def, f.coarse);
  const MixedMenuMask good = make_root_only_widening_mask(ref_tree, coarse_tree);
  validate_mask(ref_tree, good);  // does not throw

  // Wrong node count.
  MixedMenuMask bad_size = good;
  bad_size.allowed.emplace_back();
  CHECK(throws_invalid([&] { validate_mask(ref_tree, bad_size); }));

  // Row width mismatch at an action node.
  MixedMenuMask bad_width = good;
  for (std::size_t n = 0; n < ref_tree.size(); ++n) {
    if (!ref_tree.node(n).is_action())
      continue;
    bad_width.allowed[n].push_back(true);
    break;
  }
  CHECK(throws_invalid([&] { validate_mask(ref_tree, bad_width); }));

  // Empty allowed set at a reachable action node.
  MixedMenuMask bad_empty = good;
  const std::size_t root = ref_tree.root_index();
  std::fill(bad_empty.allowed[root].begin(), bad_empty.allowed[root].end(), false);
  CHECK(throws_invalid([&] { validate_mask(ref_tree, bad_empty); }));

  // A row at a non-action node must be empty.
  MixedMenuMask bad_terminal = good;
  for (std::size_t n = 0; n < ref_tree.size(); ++n) {
    if (!ref_tree.node(n).is_action()) {
      bad_terminal.allowed[n].assign(1, true);
      break;
    }
  }
  CHECK(throws_invalid([&] { validate_mask(ref_tree, bad_terminal); }));

  // Passive-edge removal is banned: find a reachable node with a passive
  // (fold/check/call) edge and suppress it.
  MixedMenuMask bad_passive = good;
  bool found_passive = false;
  for (std::size_t n = 0; n < ref_tree.size() && !found_passive; ++n) {
    if (!ref_tree.node(n).is_action())
      continue;
    const auto& node = ref_tree.node(n);
    for (std::size_t a = 0; a < node.actions.size(); ++a) {
      const bool aggressive = node.actions[a].type == bs::poker::ActionType::Bet ||
                              node.actions[a].type == bs::poker::ActionType::Raise;
      if (!aggressive && bad_passive.allowed[n][a]) {
        bad_passive.allowed[n][a] = false;
        found_passive = true;
        break;
      }
    }
  }
  CHECK(found_passive);
  CHECK(throws_invalid([&] { validate_mask(ref_tree, bad_passive); }));
  return true;
}

// M2: the standalone single-tree subset-map validator accepts the derived
// map and rejects every D184-analog invariant violation.
bool test_m2_subset_map_validator() {
  const Fixture f = make_fixture();
  const AbstractTree ref_tree(f.def, f.reference);
  const AbstractTree coarse_tree(f.def, f.coarse);
  const MixedMenuMask mask = make_root_only_widening_mask(ref_tree, coarse_tree);
  const SubsetMap good = mask_to_subset_map(ref_tree, mask);
  validate_mask_subset_map(ref_tree, good);  // does not throw
  CHECK(good.node_map.at(ref_tree.root_index()) == ref_tree.root_index());

  // Non-identity node map entry.
  SubsetMap bad_identity = good;
  for (std::size_t n = 0; n < ref_tree.size(); ++n) {
    if (bad_identity.node_map[n] != SubsetMap::kNoNode && n != ref_tree.root_index()) {
      bad_identity.node_map[n] = (n + 1 < ref_tree.size()) ? n + 1 : n - 1;
      break;
    }
  }
  CHECK(throws_invalid([&] { validate_mask_subset_map(ref_tree, bad_identity); }));

  // Out-of-range edge index.
  SubsetMap bad_range = good;
  for (std::size_t n = 0; n < ref_tree.size(); ++n) {
    if (!bad_range.edge_map[n].empty()) {
      bad_range.edge_map[n][0] = static_cast<int>(ref_tree.node(n).actions.size()) + 4;
      break;
    }
  }
  CHECK(throws_invalid([&] { validate_mask_subset_map(ref_tree, bad_range); }));

  // Suppressed (-1) edge on a passive action.
  SubsetMap bad_passive = good;
  bool found_passive = false;
  for (std::size_t n = 0; n < ref_tree.size() && !found_passive; ++n) {
    if (!ref_tree.node(n).is_action())
      continue;
    const auto& node = ref_tree.node(n);
    for (std::size_t a = 0; a < node.actions.size(); ++a) {
      const bool aggressive = node.actions[a].type == bs::poker::ActionType::Bet ||
                              node.actions[a].type == bs::poker::ActionType::Raise;
      if (!aggressive && bad_passive.edge_map[n][a] != -1) {
        bad_passive.edge_map[n][a] = -1;
        found_passive = true;
        break;
      }
    }
  }
  CHECK(found_passive);
  CHECK(throws_invalid([&] { validate_mask_subset_map(ref_tree, bad_passive); }));

  // Allowed edge reaching a kNoNode child (broken child closedness): flip a
  // suppressed edge back to allowed while its child subtree stays unmapped.
  SubsetMap bad_closure = good;
  bool built_closure = false;
  for (std::size_t n = 0; n < ref_tree.size() && !built_closure; ++n) {
    if (bad_closure.edge_map[n].empty())
      continue;
    const auto& kids = ref_tree.node(n).children;
    for (std::size_t a = 0; a < kids.size(); ++a) {
      if (bad_closure.edge_map[n][a] != -1)
        continue;
      CHECK(bad_closure.node_map[kids[a]] == SubsetMap::kNoNode);
      bad_closure.edge_map[n][a] = static_cast<int>(a);
      built_closure = true;
      break;
    }
  }
  CHECK(built_closure);
  CHECK(throws_invalid([&] { validate_mask_subset_map(ref_tree, bad_closure); }));
  return true;
}

// M3: the re-expanded snapshot is legal — finite, non-negative, normalized
// over allowed slots with exact zero bits on every suppressed reference edge.
bool test_m3_policy_legality(const Fixture& f, const MixedMenuSolveResult& solved,
                             const SubsetMap& map, const AbstractTree& ref_tree) {
  for (std::size_t n = 0; n < ref_tree.size(); ++n) {
    if (map.node_map[n] == SubsetMap::kNoNode) {
      CHECK(solved.river.avg_sigma[n].empty());
      continue;
    }
    if (!ref_tree.node(n).is_action())
      continue;
    const auto& node = ref_tree.node(n);
    const std::size_t width = node.actions.size();
    const auto& row = solved.river.avg_sigma[n];
    CHECK(row.size() == width * kC);
    for (std::uint32_t i = 0; i < kC; ++i) {
      double allowed_sum = 0.0;
      for (std::size_t a = 0; a < width; ++a) {
        const double pr = row[i * width + a];
        CHECK(std::isfinite(pr) && pr >= 0.0);
        if (map.edge_map[n][a] == -1) {
          CHECK(pr == 0.0);  // exact zero bit, not a small tolerance
        } else {
          allowed_sum += pr;
        }
      }
      CHECK(std::fabs(allowed_sum - 1.0) <= kDistTol);
    }
  }
  return true;
}

// M3b: an all-allowed mask defines exactly the cap-0 game, so the mixed
// solver must reproduce the river kernel's average policy and NashConv
// bitwise (same CFR+ dynamics over the full menu). Run on two distinct
// fixtures so the equivalence cannot be an edge-specific coincidence.
bool test_m3b_alltrue_matches_kernel(std::uint64_t stack_behind) {
  const Fixture f = make_fixture(stack_behind);
  const AbstractTree ref_tree(f.def, f.reference);
  MixedMenuMask alltrue;
  alltrue.allowed.resize(ref_tree.size());
  for (std::size_t n = 0; n < ref_tree.size(); ++n)
    if (ref_tree.node(n).is_action())
      alltrue.allowed[n].assign(ref_tree.node(n).actions.size(), true);
  const MixedMenuSolveResult mixed = solve_mixed_menu(f.def, f.reference, alltrue, kIters, 1);

  RiverSolveConfig cap0_cfg;
  cap0_cfg.def = f.def;
  cap0_cfg.iterations = kIters;
  cap0_cfg.linear_averaging = true;
  cap0_cfg.action_abstraction = f.reference;
  const RiverSolveResult kernel = solve_river_subgame(cap0_cfg, {});

  CHECK(mixed.river.avg_sigma.size() == kernel.avg_sigma.size());
  for (std::size_t n = 0; n < ref_tree.size(); ++n) {
    CHECK(mixed.river.avg_sigma[n].empty() == kernel.avg_sigma[n].empty());
    for (std::size_t k = 0; k < kernel.avg_sigma[n].size(); ++k)
      CHECK(mixed.river.avg_sigma[n][k] == kernel.avg_sigma[n][k]);
  }
  CHECK(mixed.river.nash_conv_bb == kernel.nash_conv_bb);
  return true;
}

// M4: structural support locality at non-root nodes plus overall policy
// distinctness from both coarse and full cap-0. Numeric identity across
// different games is never asserted (review L1).
bool test_m4_locality_and_distinctness(const Fixture& f, const MixedMenuMask& mask,
                                       const MixedMenuSolveResult& solved,
                                       const AbstractTree& ref_tree,
                                       const AbstractTree& coarse_tree) {
  const SubsetMap base = map_bounded_subset(ref_tree, coarse_tree);
  const SubsetMap map = mask_to_subset_map(ref_tree, mask);
  const std::size_t root = ref_tree.root_index();

  // (a) Pre-solve structural equivalence: every non-root coarse-mapped node
  // keeps exactly the coarse allowed reference-edge set.
  for (std::size_t n = 0; n < ref_tree.size(); ++n) {
    if (n == root || base.node_map[n] == SubsetMap::kNoNode || !ref_tree.node(n).is_action())
      continue;
    for (std::size_t a = 0; a < ref_tree.node(n).actions.size(); ++a)
      CHECK(mask.allowed[n][a] == (base.edge_map[n][a] != -1));
  }

  // (b1) Distinct from the full cap-0 solution: compare on the common
  // non-empty node set — cap-0 carries rows inside mask-unreachable
  // subtrees where the mixed snapshot intentionally has none.
  RiverSolveConfig cap0_cfg;
  cap0_cfg.def = f.def;
  cap0_cfg.iterations = kIters;
  cap0_cfg.linear_averaging = true;
  cap0_cfg.action_abstraction = f.reference;
  const RiverSolveResult cap0 = solve_river_subgame(cap0_cfg, {});
  std::vector<std::vector<double>> mix_rows;
  std::vector<std::vector<double>> cap0_rows;
  for (std::size_t n = 0; n < ref_tree.size(); ++n) {
    if (solved.river.avg_sigma[n].empty() || cap0.avg_sigma[n].empty())
      continue;
    CHECK(solved.river.avg_sigma[n].size() == cap0.avg_sigma[n].size());
    mix_rows.push_back(solved.river.avg_sigma[n]);
    cap0_rows.push_back(cap0.avg_sigma[n]);
  }
  CHECK(!mix_rows.empty());
  CHECK(hash_average_snapshot(mix_rows) != hash_average_snapshot(cap0_rows));
  const double l1_cap0 = max_snapshot_l1(mix_rows, cap0_rows);
  CHECK(l1_cap0 > kDistTol);

  // (b2) Distinct from the standalone coarse solution on the paired edges.
  RiverSolveConfig coarse_cfg;
  coarse_cfg.def = f.def;
  coarse_cfg.iterations = kIters;
  coarse_cfg.linear_averaging = true;
  coarse_cfg.action_abstraction = f.coarse;
  const RiverSolveResult coarse = solve_river_subgame(coarse_cfg, {});
  double max_pair_l1 = 0.0;
  for (std::size_t n = 0; n < ref_tree.size(); ++n) {
    if (base.node_map[n] == SubsetMap::kNoNode || !ref_tree.node(n).is_action())
      continue;
    const std::size_t nb = base.node_map[n];
    const std::size_t width_ref = ref_tree.node(n).actions.size();
    const std::size_t width_b = coarse_tree.node(nb).actions.size();
    for (std::uint32_t i = 0; i < kC; ++i) {
      double d = 0.0;
      for (std::size_t ar = 0; ar < width_ref; ++ar) {
        const int ab = base.edge_map[n][ar];
        if (ab == -1)
          continue;
        const double pmix = solved.river.avg_sigma[n][i * width_ref + ar];
        const double pcoarse = coarse.avg_sigma[nb][i * width_b + static_cast<std::size_t>(ab)];
        d += std::fabs(pmix - pcoarse);
      }
      max_pair_l1 = std::max(max_pair_l1, d);
    }
  }
  CHECK(max_pair_l1 > kDistTol);
  return true;
}

// M5: the external union/reach evaluator, wired with the reference tree as
// both arguments and the mask subset map (review L2), reproduces the solver
// root CFV under both completion arms.
bool test_m5_external_oracle(const Fixture& f, const MixedMenuSolveResult& solved,
                             const SubsetMap& map, const AbstractTree& ref_tree) {
  for (const CapExtensionCompletion arm :
       {CapExtensionCompletion::Uniform, CapExtensionCompletion::BrFavorable}) {
    const UnionRootValues v =
        root_values_from_snapshot(f.def, solved.river, ref_tree, ref_tree, map, arm);
    for (std::uint32_t i = 0; i < kC; ++i) {
      CHECK(std::fabs(v.ev_seat0[i] - solved.river.root_cfv_seat0[i] * 990.0) <= kOracleTol);
      CHECK(std::fabs(v.ev_seat1[i] - solved.river.root_cfv_seat1[i] * 990.0) <= kOracleTol);
    }
  }
  return true;
}

// M6: full-menu best response still sees suppressed edges (positive menu
// option value cannot be laundered), and the solver's own allowed-edge
// telescope equals its bounded NashConv root total.
bool test_m6_br_preservation_and_telescope(const Fixture& f, const MixedMenuSolveResult& solved,
                                           const SubsetMap& map, const AbstractTree& ref_tree) {
  const ReachBoundResult rb = reach_bound_from_snapshot(f.def, solved.river, ref_tree, ref_tree,
                                                        map, CapExtensionCompletion::Uniform);

  // Full-menu BR preservation: some mapped node has a strictly positive
  // full-vs-bounded gap (T9 existence convention). D195 established that the
  // single-walk bounded_gap is only a one-level upper bound nodewise, so only
  // the aggregate sandwich is exact — no per-node menu telescope identity.
  bool positive_option = false;
  double column_sum = 0.0;
  for (std::size_t s = 0; s < 2; ++s) {
    for (const NodeGapRecord& r : rb.records[s]) {
      for (std::uint32_t i = 0; i < kC; ++i) {
        const double menu_gap = r.delta[i] - r.bounded_gap[i];
        CHECK(menu_gap >= -kChipTol);
        if (menu_gap > kChipTol)
          positive_option = true;
        column_sum += r.own_reach[i] * r.bounded_gap[i];
      }
    }
  }
  if (!positive_option)
    std::printf("M6 diagnostic: no positive menu option on 8-chip root\n");
  CHECK(positive_option);

  // The instrument reports the solver's own allowed-edge NashConv verbatim,
  // and the D195 aggregate sandwich holds: bounded NC <= bounded-column
  // telescope <= full-menu exact exploitability.
  CHECK(rb.bounded_nash_conv_bb == solved.river.nash_conv_bb);
  const double column_bb = column_sum / rb.bb_normalization;
  CHECK(column_bb + 1e-12 >= rb.bounded_nash_conv_bb);
  CHECK(column_bb <= rb.exact_exploitability_bb + 1e-12);
  // The pure menu term is exact minus the solver's self-consistent bounded NC.
  const double pure_menu_bb = rb.exact_exploitability_bb - rb.bounded_nash_conv_bb;
  CHECK(pure_menu_bb >= -1e-12);

  // Internal allowed-edge telescope equals the root BR-EV total per seat.
  for (std::size_t s = 0; s < 2; ++s) {
    const auto& br = s == 0 ? solved.river.root_br_seat0 : solved.river.root_br_seat1;
    const auto& ev = s == 0 ? solved.river.root_cfv_seat0 : solved.river.root_cfv_seat1;
    double root_total = 0.0;
    for (std::uint32_t i = 0; i < kC; ++i)
      root_total += (br[i] - ev[i]) * 990.0;
    const double tol = std::max(1e-7, 1e-12 * std::fabs(root_total));
    CHECK(std::fabs(solved.telescope_raw_chips[s] - root_total) <= tol);
  }
  return true;
}

// M7: structural public-only mask plus bitwise forward-reach conservation.
bool test_m7_reach_conservation(const Fixture& f, const MixedMenuSolveResult& solved,
                                const SubsetMap& map, const AbstractTree& ref_tree) {
  // The mask type carries no combo dimension: one node-indexed row only.
  CHECK(solved.river.avg_sigma.size() == ref_tree.size());
  const std::size_t root = ref_tree.root_index();
  for (std::size_t seat = 0; seat < 2; ++seat) {
    CHECK(solved.forward_reach[seat].size() == ref_tree.size());
    for (double x : solved.forward_reach[seat][root])
      CHECK(x == 1.0);  // root full forward reach
    const auto expected = recompute_forward_reach(ref_tree, ref_tree, map, solved.river, seat);
    validate_supplied_reach(ref_tree, ref_tree, map, solved.river, seat,
                            solved.forward_reach[seat]);  // bitwise, zero tolerance
    for (std::size_t n = 0; n < ref_tree.size(); ++n) {
      if (expected[n].empty())
        CHECK(solved.forward_reach[seat][n].empty());
    }
  }
  return true;
}

// M8(ii) helper: the widened root itself has no suppressed edge, so its
// LOCAL menu gap is identically zero (best and bounded-best range over the
// same edges). The extension option value the root cannot shed lives at
// DEEPER restricted nodes inside its subtree; scan those, parameterized with
// the T9 deeper-stack fallback.
bool has_positive_restricted_menu_gap(std::uint64_t stack_behind) {
  const Fixture g = make_fixture(stack_behind);
  const AbstractTree ref(g.def, g.reference);
  const AbstractTree coarse(g.def, g.coarse);
  const MixedMenuMask m = make_root_only_widening_mask(ref, coarse);
  const SubsetMap sm = mask_to_subset_map(ref, m);
  const MixedMenuSolveResult sol = solve_mixed_menu(g.def, g.reference, m, kIters, 1);
  const ReachBoundResult rb =
      reach_bound_from_snapshot(g.def, sol.river, ref, ref, sm, CapExtensionCompletion::Uniform);
  for (std::size_t s = 0; s < 2; ++s)
    for (const NodeGapRecord& r : rb.records[s]) {
      if (std::none_of(r.edge_map.begin(), r.edge_map.end(), [](int edge) { return edge == -1; }))
        continue;  // locally widened nodes have zero local menu gap by construction
      for (std::uint32_t i = 0; i < kC; ++i)
        if (r.delta[i] - r.bounded_gap[i] > kChipTol)
          return true;
    }
  return false;
}

// M8: the corrected subtree gap identity (review F1/L4).
bool test_m8_subtree_gap_identity(const Fixture& f, const MixedMenuSolveResult& solved,
                                  const SubsetMap& map, const AbstractTree& ref_tree) {
  const std::size_t root = ref_tree.root_index();

  // (i) Root is locally fully widened but NOT subtree extension-free,
  // evaluated per seat.
  for (int edge : map.edge_map[root])
    CHECK(edge != -1);
  for (std::size_t seat = 0; seat < 2; ++seat)
    CHECK(!subtree_is_extension_free(ref_tree, map, root, seat));

  const ReachBoundResult rb = reach_bound_from_snapshot(f.def, solved.river, ref_tree, ref_tree,
                                                        map, CapExtensionCompletion::Uniform);

  // (ii) Empirical existence scan: a positive MENU gap (not merely the
  // equilibrium residual) appears at a restricted node inside the root
  // subtree, with the T9 deeper-stack fallback if the shallow root shows
  // none.
  bool positive_menu_gap = has_positive_restricted_menu_gap(8);
  if (!positive_menu_gap) {
    std::printf("M8 diagnostic: no restricted menu gap on 8-chip root; trying 20-chip\n");
    positive_menu_gap = has_positive_restricted_menu_gap(20);
  }
  CHECK(positive_menu_gap);

  // (iii) Zero menu gap is pinned only at genuinely extension-free nodes.
  std::size_t pinned = 0;
  for (std::size_t s = 0; s < 2; ++s) {
    for (const NodeGapRecord& r : rb.records[s]) {
      if (!subtree_is_extension_free(ref_tree, map, r.reference_node, s))
        continue;
      ++pinned;
      for (std::uint32_t i = 0; i < kC; ++i)
        CHECK(std::fabs(r.delta[i] - r.bounded_gap[i]) <= kChipTol);
    }
  }
  CHECK(pinned > 0);
  return true;
}

// M9: repeat solves are bitwise identical.
bool test_m9_determinism(const Fixture& f, const MixedMenuMask& mask,
                         const MixedMenuSolveResult& first) {
  const MixedMenuSolveResult second = solve_mixed_menu(f.def, f.reference, mask, kIters, 1);
  CHECK(hash_average_snapshot(first.river.avg_sigma) ==
        hash_average_snapshot(second.river.avg_sigma));
  CHECK(max_snapshot_l1(first.river.avg_sigma, second.river.avg_sigma) == 0.0);
  CHECK(first.river.nash_conv_bb == second.river.nash_conv_bb);
  for (std::size_t s = 0; s < 2; ++s)
    CHECK(first.telescope_raw_chips[s] == second.telescope_raw_chips[s]);
  return true;
}

// M10: resource fail-close is typed reference-construction propagation, and
// masked storage is nodewise bounded by the reference envelope.
bool test_m10_resource_boundary(const Fixture& f, const MixedMenuMask& mask,
                                const MixedMenuSolveResult& solved) {
  TreeLimits tight;
  tight.max_nodes = 5;  // below even the root plus its first edge children
  tight.max_bytes = std::size_t{1} << 40;
  tight.max_depth = 256;
  bool typed_throw = false;
  try {
    const AbstractTree rejected(f.def, f.reference, tight);
    (void)rejected;
  } catch (const tree_resource_exhausted&) {
    typed_throw = true;
  }
  CHECK(typed_throw);

  // Compact allowed slots can never exceed the reference edge total.
  CHECK(solved.compact_slot_total <= solved.reference_edge_total);
  CHECK(solved.compact_slot_total > 0);
  return true;
}

}  // namespace

int main() {
  const Fixture f = make_fixture();
  const AbstractTree ref_tree(f.def, f.reference);
  const AbstractTree coarse_tree(f.def, f.coarse);
  const MixedMenuMask mask = make_root_only_widening_mask(ref_tree, coarse_tree);
  const SubsetMap map = mask_to_subset_map(ref_tree, mask);
  const MixedMenuSolveResult solved = solve_mixed_menu(f.def, f.reference, mask, kIters, 1);

  if (!test_m1_mask_validation() || !test_m2_subset_map_validator() ||
      !test_m3_policy_legality(f, solved, map, ref_tree) || !test_m3b_alltrue_matches_kernel(8) ||
      !test_m3b_alltrue_matches_kernel(20) ||
      !test_m4_locality_and_distinctness(f, mask, solved, ref_tree, coarse_tree) ||
      !test_m5_external_oracle(f, solved, map, ref_tree) ||
      !test_m6_br_preservation_and_telescope(f, solved, map, ref_tree) ||
      !test_m7_reach_conservation(f, solved, map, ref_tree) ||
      !test_m8_subtree_gap_identity(f, solved, map, ref_tree) ||
      !test_m9_determinism(f, mask, solved) || !test_m10_resource_boundary(f, mask, solved))
    return 1;
  std::printf("rfc0047_mixed_menu: all checks passed\n");
  return 0;
}
