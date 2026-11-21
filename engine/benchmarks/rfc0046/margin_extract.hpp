// RFC 0046 Stage-1 measurement-only per-combo margin extraction.
//
// Computes the locked turn-boundary margin for each responder concrete combo:
//
//   b(c) = (sum over compatible hero deals d of w(d) * C_resp(d)) / m(c)
//
// where C_resp(d) is the responder's exact expected continuation value
// through the cap-1 turn+river subtree under a fixed reference behavior
// policy (the candidate trunk turn policy plus a concrete river policy; for
// the Gate-2 timing/certification cells a long concrete oracle solve supplies
// it), averaged over the uniform 48-card river chance by exact enumeration,
// and m(c) = sum_d w(d) is the combo's blocker-aware counterfactual mass.
// Benchmark-only.
#pragma once

#include <cstdint>
#include <map>
#include <vector>

#include "concrete_keyer.hpp"
#include "partition_walker.hpp"

namespace bs::rfc0046 {

struct MarginExtractionInput {
  const tree::AbstractTree* tree = nullptr;
  const ConcreteCardKeyer* keyer = nullptr;
  std::size_t hero = 0;
  std::size_t responder = 1;
  std::array<std::vector<bs::solver::WeightedHand>, 2> ranges;
  const PartitionPolicy* reference = nullptr;
};

// Returns dense responder bucket -> {margin b(c) in chip units, mass m(c)}.
std::map<std::uint32_t, std::array<double, 2>> extract_per_combo_margins(
    const MarginExtractionInput& input);

}  // namespace bs::rfc0046
