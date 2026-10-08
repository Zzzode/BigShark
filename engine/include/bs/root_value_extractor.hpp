// RFC 0013 Stage 1: root-value extraction from a solved NSeatPolicy.
//
// Walks the solved abstract tree for a specific joint deal and computes the
// expected per-seat chip utility at the root:
//   - Action node: look up the policy row by (own_card_bucket, node_index),
//     enumerate every menu action weighted by its average-policy probability.
//   - Chance node: enumerate every legal runout card (board-only list minus
//     all hole cards), average uniformly.
//   - TerminalFold: return the payload's stored chip_utility.
//   - TerminalShowdown: settle with the live seats' hole cards.
//
// This is the policy-following traversal that produces the values stored in
// the NSeatDeclaredFrontierTable. On small enumerable fixtures it is
// cross-checked against stage6::exact_deal_utility.
#pragma once

#include <array>
#include <bs/abstract_tree.hpp>
#include <bs/game_definition.hpp>
#include <bs/nseat_trainer.hpp>
#include <cstddef>
#include <span>
#include <vector>

namespace bs::gto {

// Extract the expected per-seat chip utility for one fixed joint deal at the
// root of the solved tree. `deal` has one sorted hole-card pair per seat.
// The walk is exact (no sampling): every action and every legal runout card
// is enumerated. Throws std::invalid_argument on a missing policy row or an
// unexpected node kind (fail closed).
std::vector<double> extract_root_value(const solver::NSeatPolicy& policy,
                                       const tree::AbstractTree& tree,
                                       const poker::GameState& root_state,
                                       std::span<const std::array<int, 2>> deal);

}  // namespace bs::gto
