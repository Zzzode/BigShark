// stage6/geometry_enumerator.hpp — the policy-linked flop geometry
// enumerators, kept in bigshark_stage6_eval (they drive the pinned chart
// preflop policy through the GameState->Ctx adapter). The pure geometry
// types and bucketing live in geometry.hpp / bigshark_stage6_core.
#pragma once

#include <bs/stage6/geometry.hpp>
#include <string>
#include <vector>

namespace bs::stage6 {

// Enumerates every distinct flop geometry reachable under the composed chart
// preflop profile plus the full deviation grid at one deviator seat.
std::vector<GeometrySignature> enumerate_flop_geometries(
    std::size_t player_count, poker::Chips big_blind,
    std::vector<std::string>* origin_lines = nullptr);

// Chart-only reach: the matrix the composed candidate actually trains on.
std::vector<GeometrySignature> enumerate_chart_flop_geometries(
    std::size_t player_count, poker::Chips big_blind, std::vector<std::string>* lines = nullptr);

}  // namespace bs::stage6
