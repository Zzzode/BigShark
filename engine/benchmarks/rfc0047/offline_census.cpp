#include "offline_census.hpp"

#include <utility>

#include "turn_solver.hpp"

using bs::tree::AbstractTree;
using bs::tree::tree_resource_exhausted;

namespace bs::rfc0047 {

bs::tree::TreeLimits offline_census_limits() noexcept {
  return bs::tree::TreeLimits{kOfflineCensusMaxNodes, kOfflineCensusMaxDepth,
                              kOfflineCensusMaxBytes};
}

OfflineCensusRow offline_census(const bs::poker::GameDef& def,
                                const bs::abstraction::ActionAbstraction& menu,
                                const bs::tree::TreeLimits& limits, std::string label,
                                std::string family, int raise_cap) {
  OfflineCensusRow row;
  row.label = std::move(label);
  row.family = std::move(family);
  row.raise_cap = raise_cap;
  try {
    const AbstractTree tree(def, menu, limits);
    std::size_t action_nodes = 0;
    std::size_t max_menu_width = 0;
    for (const auto& node : tree.nodes()) {
      if (!node.is_action())
        continue;
      ++action_nodes;
      max_menu_width = std::max(max_menu_width, node.actions.size());
    }
    row.total_nodes = tree.size();
    row.action_nodes = action_nodes;
    row.max_menu_width = max_menu_width;
    row.depth = tree.depth();
    row.projected_infosets = action_nodes * kRiverC;
    row.accounted_bytes = tree.accounted_bytes();
    row.menu_within_pack_limit = max_menu_width <= kOfflinePackMaxMenuActions;
    row.fits = true;
  } catch (const bs::tree::tree_resource_exhausted&) {
    // Typed envelope rejection: leave counters at zero and report fits=false.
    row.fits = false;
  }
  return row;
}

OfflineCensusRow offline_census(const bs::poker::GameDef& def,
                                const bs::abstraction::ActionAbstraction& menu, std::string label,
                                std::string family, int raise_cap) {
  return offline_census(def, menu, offline_census_limits(), std::move(label), std::move(family),
                        raise_cap);
}

}  // namespace bs::rfc0047
