// RFC 0049 Stage 1 Gate 1: offline wide-menu tree census.
//
// Bench-only structural measurement. Builds each candidate's full public
// tree under the frozen offline TreeLimits and counts nodes, action nodes,
// menu widths, depth, and projected infosets WITHOUT solving. A tree that
// crosses the offline envelope is returned as a typed rejected row rather
// than propagating tree_resource_exhausted; every other exception still
// propagates. This component links only bigshark_rfc0047_bench and is
// unreachable from production composition roots by the rfc0047 link guard.
#pragma once

#include <bs/abstract_tree.hpp>
#include <bs/abstraction.hpp>
#include <bs/game_definition.hpp>
#include <cstddef>
#include <cstdint>
#include <string>

namespace bs::rfc0047 {

// Frozen RFC 0049 Stage 1 offline envelope (pre-registered; changing these
// requires an RFC amendment and fresh independent review).
constexpr std::size_t kOfflineCensusMaxNodes = 25'000'000;
constexpr std::size_t kOfflineCensusMaxDepth = 256;
constexpr std::size_t kOfflineCensusMaxBytes = std::size_t{1} << 33;  // 8 GiB

// Hard per-node menu ceiling enforced by the BSFLIB pack path
// (resident_index kMaxResidentActions and the abstraction builder).
constexpr std::size_t kOfflinePackMaxMenuActions = 32;

bs::tree::TreeLimits offline_census_limits() noexcept;

struct OfflineCensusRow {
  std::string label;
  std::string family;  // "control" | "raise_cap" | "sizing_grid"
  int raise_cap = 0;
  std::size_t total_nodes = 0;
  std::size_t action_nodes = 0;
  std::size_t max_menu_width = 0;
  std::size_t depth = 0;
  std::size_t projected_infosets = 0;  // action_nodes * kRiverC (1,081)
  std::size_t accounted_bytes = 0;
  bool menu_within_pack_limit = false;
  bool fits = false;  // false => typed census_rejected
};

// Census under the frozen offline envelope.
OfflineCensusRow offline_census(const bs::poker::GameDef& def,
                                const bs::abstraction::ActionAbstraction& menu, std::string label,
                                std::string family, int raise_cap);

// Census under caller-supplied limits (used by the tiny-limit rejection
// native test; production Stage 1 receipts always use the frozen envelope).
OfflineCensusRow offline_census(const bs::poker::GameDef& def,
                                const bs::abstraction::ActionAbstraction& menu,
                                const bs::tree::TreeLimits& limits, std::string label,
                                std::string family, int raise_cap);

}  // namespace bs::rfc0047
