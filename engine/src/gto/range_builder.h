// range_builder.h — action-line river range construction (L2).
//
// Turns a preflop continuation set (169-hand chart membership) into the 1326-
// combo weight RANGE each side brings to the river, then strength-stratifies it
// to a cap so the exact solver stays inside the live time budget. Cross-player
// card blocking is applied later, inside the game, not here.
//
// This module constructs the preflop seed range. range_tracker applies
// approximate flop and turn action-line narrowing before the river solve.
#pragma once
#include <bs/range.hpp>
#include <string>
#include <vector>

namespace bs::gto {

// Add every concrete combo represented by one 169-hand key (6 pairs, 4 suited,
// 12 offsuit) that does not use a board card. Combos get weight `w`.
void Expand169(const std::string& key169, const std::vector<int>& board, float w, Range& out);

// Build a river range from a preflop continuation set. A null/empty set means
// the whole deck (all non-board combos). Result is strength-stratified to at
// most `cap` combos (uniform weights), preserving the weak..strong shape.
Range BuildRiverRange(const std::vector<int>& board, const std::vector<std::string>* preflop_set,
                      int cap);

// Strength-stratified cap of an already-weighted range: keep at most `cap`
// non-blocking combos, sampled evenly across board-strength rank so value,
// catchers and air all survive proportionally.
Range CapRange(const std::vector<int>& board, const Range& in, int cap);

}  // namespace bs::gto
