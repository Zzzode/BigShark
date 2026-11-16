// RFC 0050 Stage 1 shared exact-menu convergence helpers.
//
// Bench-only components used by the calibration probe, the frozen
// convergence probe, and the native test. Stage 1 solves the EXACT cap-0
// menu on the measured-smallest decision-bearing anchor, so the bounded
// tree and the reference tree are the same cap-0 tree and the subset map
// is a full identity: every reference edge maps to a bounded edge, no
// extension subtree exists, and the pure-menu term must read zero at
// every rung. A nonzero reading is a structural contradiction (the menu
// was not actually exact), not numeric noise, and aborts the gate with a
// distinct typed status rather than being folded into a convergence
// verdict.
#pragma once

#include <array>
#include <bs/abstract_tree.hpp>
#include <bs/abstraction.hpp>
#include <bs/game_definition.hpp>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "offline_convergence.hpp"
#include "union_menu.hpp"

namespace bs::rfc0047 {

// Frozen Stage 1 ladder and wall, identical to the RFC 0049 Gate 2
// constants and hard-coded here (no argv override; amending them is an
// RFC amendment).
inline constexpr std::array<std::uint64_t, 9> kRfc0050Stage1Rungs{
    100, 200, 400, 800, 1'500, 3'000, 6'000, 12'000, 20'000};
inline constexpr double kRfc0050Stage1WallMs = 1'800'000.0;  // 1,800 s

// Pre-registered structural zero tolerance for the pure-menu term in BB.
// Derivation: the reach-bound telescope aggregates at most
// (action nodes x 1,081 combos) per-entry menu gaps, each bounded in
// practice by the 1e-9 chip cross-walk tolerance of the native reach-bound
// tests (~1.7e6 entries x 1e-9 chips ~= 1.7e-3 chips), divided by the BB
// normalization 1,081 x 990 x big_blind (~2.14e6 at BB 2), i.e. an
// accumulation bound near 8e-10 BB. 1e-8 BB keeps roughly an order of
// magnitude of FP margin while staying 2e5 times below the 0.002 BB gate.
// A reading outside this tolerance is a structural failure, not a
// convergence result.
inline constexpr double kRfc0050PureMenuZeroBb = 1e-8;

struct Rfc0050Stage1Domain {
  bs::abstraction::ActionAbstraction menu;
  bs::tree::AbstractTree reference_tree;
  bs::tree::AbstractTree bounded_tree;
  SubsetMap subset;
};

// Build the exact domain: two separately constructed cap-0 trees (the
// scoring instrument keeps distinct reference/bounded objects by
// contract) and their full-identity subset map. Throws if the map is not
// identity, which would make the zero-menu claim invalid.
Rfc0050Stage1Domain build_stage1_domain(const bs::poker::GameDef& def,
                                        const bs::tree::TreeLimits& limits);

// The D296 structural guard. True only when the measured pure-menu term
// is inside the pre-registered zero tolerance.
bool pure_menu_reads_zero(double pure_menu_term_bb);

// Calibration extrapolation from one measured steady-state per-iteration
// wall (milliseconds) and one measured reach-bound scoring wall per rung.
// Pure arithmetic so the native test can pin it; every figure is an
// estimate that the real gate receipt replaces.
struct Rfc0050Calibration {
  double ms_per_iteration = 0.0;
  double ms_per_rung_scoring = 0.0;
  double projected_20k_wall_s = 0.0;          // solve only
  double projected_full_ladder_wall_s = 0.0;  // solve plus nine scorings
  double iterations_inside_wall = 0.0;        // floor of solve-only capacity
  std::uint64_t last_rung_inside_wall = 0;    // highest rung whose total fits
};

Rfc0050Calibration extrapolate_stage1_calibration(double ms_per_iteration,
                                                  double ms_per_rung_scoring);

}  // namespace bs::rfc0047
