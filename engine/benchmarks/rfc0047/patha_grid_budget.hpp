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
  std::uint64_t turn_infosets = 0;
  std::uint64_t river_infosets = 0;
  std::uint64_t conservative_infoset_sum = 0;
  // Deterministic feasibility against the artifact envelope.
  bool under_tree_node_limit = false;  // TreeLimits.max_nodes (2,000,000)
  bool under_byte_limit = false;       // TreeLimits.max_bytes (1 GiB)
  bool under_1m_infoset = false;       // the offline infoset envelope
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

// The frozen-envelope constants the verdict uses, exposed for the native test.
inline constexpr std::size_t kPathANodeLimit = 2'000'000;
inline constexpr std::size_t kPathAByteLimit = std::size_t{1} << 30;  // 1 GiB
inline constexpr std::uint64_t kPathAInfosetLimit = 1'000'000;

std::string grid_budget_csv_header();
std::string to_csv(const GridBudget& b);

}  // namespace bs::rfc0047
