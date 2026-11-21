// RFC 0046 Stage-1 measurement-only multi-street subgame certifier.
//
// Independent code from turn_gadget.cpp: an exact best-response walker over
// the full turn+river subtree (all legal river cards enumerated, not
// sampled), pooling blocker-aware hero deals per responder concrete combo.
// For each positive-mass responder combo c it computes
//
//   BR_continue(c) = responder value when the responder best-responds at
//                    every turn/river action node while the hero plays the
//                    solved concrete-keyed candidate and chance runs uniform,
//
// and certifies  BR_continue(c) <= b(c) + 1e-6 * root_pot  for every c.
// This is the RFC 0005 responder inequality (certifier.cpp candidate vs
// baseline) extended across both action streets. Benchmark-only.
#pragma once

#include <bs/nseat_trainer.hpp>
#include <cstdint>
#include <map>
#include <vector>

#include "concrete_keyer.hpp"
#include "turn_gadget.hpp"

namespace bs::rfc0046 {

enum class CertifierFailReason {
  Certified,      // bound holds for every positive-mass combo
  MissingRow,     // a reached positive-mass hero infoset had no solved row (underconverged)
  BoundExceeded,  // some responder combo's BR continuation beats b + tol (bound unreachable)
};

struct CertifierResult {
  bool certified = false;
  CertifierFailReason reason = CertifierFailReason::Certified;
  std::size_t positive_combos = 0;
  double max_slack = 0.0;  // max over combos of (BR_continue - b - tol), <=0 certifies
  std::uint32_t worst_bucket = 0;
  std::uint32_t missing_hero_node = 0;  // node index when reason == MissingRow
  std::array<int, 2> missing_hero_cards{-1, -1};
  // Per-combo diagnostics: dense bucket -> {baseline b, BR continuation}.
  std::map<std::uint32_t, std::array<double, 2>> margins;
};

struct CertifierInput {
  const tree::AbstractTree* tree = nullptr;
  const ConcreteCardKeyer* keyer = nullptr;
  std::size_t hero = 0;
  std::size_t responder = 1;
  std::vector<bs::solver::WeightedHand> hero_range;
  std::vector<bs::solver::WeightedHand> responder_range;
  MarginTable margins;
  const std::map<bs::solver::NSeatInformationKey, GadgetRow>* policy = nullptr;
  double pot_tolerance = 1e-6;  // fraction of root pot
  // When true, throw if a positive-mass responder combo has no locked margin.
  bool require_all_margins = false;
};

// Runs the exact BR certification. A missing candidate row for a reached
// positive-mass infoset returns certified=false.
CertifierResult certify_turn_subgame(const CertifierInput& input);

}  // namespace bs::rfc0046
