// RFC 0049 Stage 1 Gate 1 manual single-instance offline wide-menu census
// probe (NOT a CTest). Enumerates the frozen candidate list on the w1
// reached-river anchor — the sealed Coarse cap-1 control, identity menus at
// raise caps 2..6, and the Wide/Fine cap-1 sizing grids — builds each full
// public tree under the bench offline envelope, and prints one structural
// census row per candidate without solving. The candidate list and envelope
// constants are hard-coded per the RFC's pre-registration change control;
// there are no argv overrides. Tree-only work, so no wall tripwire applies.
//
// Usage: bigshark-rfc0047-offline-census-probe
#include <bs/abstraction.hpp>
#include <cstdio>
#include <string>
#include <vector>

#include "frontier_enumerator.hpp"
#include "offline_census.hpp"
#include "patha_grid_budget.hpp"
#include "strata.hpp"

using namespace bs::rfc0047;
using bs::abstraction::ActionAbstraction;

namespace {

struct Candidate {
  std::string label;
  std::string family;
  int raise_cap;
  ActionAbstraction menu;
};

std::vector<Candidate> frozen_candidates() {
  std::vector<Candidate> out;
  // Sealed control first; a later gate invalidates the campaign if this row
  // drifts from the cap-1 baseline.
  out.push_back(
      {"coarse-cap1-control", "control", 1, make_grid_abstraction(SizingGrid::Coarse, 1)});
  for (int cap = 2; cap <= 6; ++cap) {
    out.push_back({"identity-cap" + std::to_string(cap), "raise_cap", cap,
                   ActionAbstraction::identity(static_cast<std::uint8_t>(cap))});
  }
  out.push_back({"wide-cap1", "sizing_grid", 1, make_grid_abstraction(SizingGrid::Wide, 1)});
  out.push_back({"fine-cap1", "sizing_grid", 1, make_grid_abstraction(SizingGrid::Fine, 1)});
  return out;
}

}  // namespace

int main() {
  const auto leaves = enumerate_frontiers(*find_stratum("w1"), 1);
  if (leaves.empty()) {
    std::fprintf(stderr, "w1 frontier enumeration returned no leaves\n");
    return 2;
  }
  const bs::poker::GameDef& def = leaves.front().river_def;

  std::printf(
      "label,family,raise_cap,status,total_nodes,action_nodes,max_menu_width,"
      "menu_within_pack_limit,depth,projected_infosets,accounted_bytes\n");
  for (const Candidate& c : frozen_candidates()) {
    const OfflineCensusRow row = offline_census(def, c.menu, c.label, c.family, c.raise_cap);
    std::printf("%s,%s,%d,%s,%zu,%zu,%zu,%d,%zu,%zu,%zu\n", row.label.c_str(), row.family.c_str(),
                row.raise_cap, row.fits ? "fits" : "census_rejected", row.total_nodes,
                row.action_nodes, row.max_menu_width, row.menu_within_pack_limit ? 1 : 0, row.depth,
                row.projected_infosets, row.accounted_bytes);
  }
  return 0;
}
