// RFC 0048 Path A stage-1 (D176): benchmark-only OFFLINE public-tree footprint
// census for wide raise caps and alternative bet-sizing grids. This runs NO
// equilibrium solve; it builds each public AbstractTree and measures the exact
// node/byte/infoset footprint and one-time build cost against the artifact
// envelope. The feasibility verdict is computed from deterministic structural
// counts, never from wall time or RSS (those are environmental evidence only).
//
// Two independent widening levers, both expressed through the existing
// ActionAbstraction API (no new abstraction):
//   Lever 1 RAISE CAP  -> ActionAbstraction::identity(k)
//   Lever 2 SIZING GRID -> ActionAbstraction::declared(schedule, MinAndCap, k)
//
// F1 (independent review): the rfc0047 stratum GameDef helpers reject cap > 2,
// but the cap that actually shapes a tree lives on the ActionAbstraction, not
// the GameDef. Callers therefore build the GameDef once with the fixed literal
// cap 1 and pass the real cap through the abstraction constructed here.
#pragma once

#include <bs/abstract_tree.hpp>
#include <bs/abstraction.hpp>
#include <cstdint>
#include <string>

#include "strata.hpp"

namespace bs::rfc0047 {

// The three representative sizing grids (Lever 2). All use CoverSeeds::
// MinAndCap so they differ only in pot fractions and stay comparable to
// identity; the raise cap is supplied separately (stage 1 fixes it at 1 to
// isolate the sizing lever from the raise-count lever).
enum class SizingGrid {
  Coarse,    // bets {1/2}, raises {1/1} (fraction-schedule precedent)
  Identity,  // default_size_schedule (bets 1/3,3/4,3/2; raises 1/2,1)
  Wide,      // bets 1/4,1/2,3/4,1,3/2; raises 1/2,1,2
};

const char* sizing_grid_name(SizingGrid grid);

// Build the declared abstraction for one sizing grid. The schedule is
// default-constructed (valid on every street) and only the postflop streets are
// overridden, because reduced() eagerly validates all four streets.
bs::abstraction::ActionAbstraction make_grid_abstraction(SizingGrid grid, std::uint8_t raise_cap);

// One solve-free public-tree measurement.
struct GridBudget {
  std::string label;      // human-readable instance key
  std::string root_kind;  // "turn" | "reached_river"
  std::string lever;      // "raise_cap" | "sizing_grid"
  std::uint8_t raise_cap = 1;
  std::string grid;  // sizing grid name, "" for the identity lever
  // Tree build outcome. A resource boundary is a first-class negative result,
  // not an error: only tree_resource_exhausted is mapped here; every other
  // exception (invalid_argument/bad_alloc/overflow_error) propagates to the
  // caller because it signals a probe/environment fault, not an envelope limit.
  bool resource_boundary = false;
  std::string boundary_reason;  // tree_resource_exhausted::what() when boundary
  // Structural counts (all zero when boundary).
  std::size_t total_nodes = 0;
  std::size_t action_nodes = 0;
  std::size_t turn_action_nodes = 0;
  std::size_t river_action_nodes = 0;
  std::size_t chance_nodes = 0;
  std::size_t terminal_nodes = 0;
  std::size_t accounted_bytes = 0;
  std::size_t max_actions_per_node = 0;
  double mean_actions_per_action_node = 0.0;  // menu-width proxy for stage 2
  // Dense infoset estimate using the frozen tree_budget convention
  // (action_nodes * live_combos + live_combos, the +combos being the one-per-
  // combo -x gadget row). Reported per street because a turn-rooted tree mixes
  // 1,128-combo turn rows and 1,081-combo river rows.
  std::uint32_t turn_combos = 0;
  std::uint32_t river_combos = 0;

  // Three distinct units (D179 stage-2 review F1-F5). They must never be
  // compared against the same envelope.
  //
  // U1 ONLINE RESIDENT (the only unit gated by ResolveLimits 1e6 infosets):
  // the turn trunk plus ONE reached river subtree - the largest single chance
  // child. A turn-rooted tree fans out to 48 river cards but RFC 0047 solves one
  // reached card per serve, so only one river subtree is co-resident.
  std::uint64_t u1_turn_infosets = 0;
  std::uint64_t u1_reached_river_infosets_max = 0;
  std::uint64_t u1_online_resident_infosets = 0;
  bool u1_under_1m_infoset = false;  // ResolveLimits online peak
  //
  // U2 OFFLINE BSNCV ARTIFACT ROWS (cv_oracle OracleRow): one per
  // (frontier leaf, seat, 1081 combo); menu-width-independent per-row storage.
  // Gated by the G4 census storage budget, NOT by a 1e6-row count (none exists).
  std::uint64_t u2_artifact_rows = 0;
  std::uint64_t u2_artifact_bytes = 0;  // rows * sizeof(OracleRow)
  //
  // U3 WHOLE-TRUNK BUILD GRAND TOTAL: sums ALL 48 river fanout action nodes.
  // Never co-resident; bounded by the build TreeLimits (node/byte) only, never
  // by the online 1e6 infoset peak.
  std::uint64_t u3_all_river_infosets = 0;
  std::uint64_t u3_build_grand_total_infosets = 0;

  // Build-envelope verdicts (U3): TreeLimits 2,000,000 nodes / 1 GiB. These are
  // the only envelope flags a resource boundary row is allowed to carry; all
  // stay false on a boundary so bytes==0 can never read as feasible.
  bool under_tree_node_limit = false;
  bool under_byte_limit = false;
};

// Build one tree and compute its GridBudget. Throws only non-boundary faults
// (invalid_argument/bad_alloc/overflow_error); tree_resource_exhausted is
// captured into resource_boundary. root_kind is a label ("turn" or
// "reached_river") copied into the result. The default TreeLimits overload uses
// the production 2M-node / 1 GiB bounds; the explicit-limits overload exists so
// the boundary branch can be exercised deterministically in a native test
// without building toward 1 GiB.
GridBudget measure_grid_budget(const bs::poker::GameDef& def, const std::string& root_kind,
                               const bs::abstraction::ActionAbstraction& action,
                               const std::string& label, std::uint8_t raise_cap,
                               const std::string& grid_name);
GridBudget measure_grid_budget(const bs::poker::GameDef& def, const std::string& root_kind,
                               const bs::abstraction::ActionAbstraction& action,
                               const std::string& label, std::uint8_t raise_cap,
                               const std::string& grid_name, const bs::tree::TreeLimits& limits);

// Envelope constants, exposed for the native test. Node/byte bound the U3
// offline build; the infoset limit is the U1 online-peak ResolveLimits gate and
// must never be applied to the U3 whole-trunk grand total (D179 review F2/F3).
inline constexpr std::size_t kPathANodeLimit = 2'000'000;
inline constexpr std::size_t kPathAByteLimit = std::size_t{1} << 30;  // 1 GiB
inline constexpr std::uint64_t kPathAInfosetLimit = 1'000'000;        // U1 only

std::string grid_budget_csv_header();
std::string to_csv(const GridBudget& b);

}  // namespace bs::rfc0047
