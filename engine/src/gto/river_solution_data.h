// river_solution_data.h — internal payload for the public RiverSolution
// facade. Not installed; only the GTO translation units include it.
#pragma once
#include <array>
#include <bs/river_gto.hpp>

#include "cfr_solver.h"

namespace bs::gto {

struct RiverSolutionData {
  RiverSolveResult meta;
  std::array<NodePolicy, 6> node;
};

}  // namespace bs::gto
