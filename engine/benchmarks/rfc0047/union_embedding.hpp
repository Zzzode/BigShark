// RFC 0048 Path A Stage 2b (D184): benchmark-only union-menu embedding
// measurement. Derived from the frozen cap_embedding evaluator and
// intentionally kept in separate translation units: the frozen G5 cap-0
// instrument (cap_embedding.*) stays byte-stable, while this component is the
// candidate reference domain the Stage-2b addendum authorizes.
//
// Shared conventions with the frozen evaluator (review advisory L2):
//   - same normalization: C(47,2)=1081 combos, 990 compatible joint deals,
//     big-blind conversion;
//   - one EV walk (positive mapped mass only; an unmapped extension is a
//     logic_error fail-closed tripwire) and one exact BR walk per seat;
//   - opponent-reach snapshot scaling identical to the river kernel;
//   - the same two extension completion arms (Uniform 1/A; BrFavorable
//     per-combo max);
//   - the embedding identity e_embed^u = e_Gg(sigma_g) + non-negative pure
//     menu term, valid because the bounded menu is a subset of the reference
//     menu at every node and BR over a superset menu weakly dominates.
//
// The structural difference is the map: every mass, zero-mass, and sigma
// decision here goes through SubsetMap::edge_map (exact Action equality),
// never through prefix indices, so interleaved subsets and true unions are
// first-class. Every result carries the mechanical candidate-domain labels
// union-candidate / supporting-evidence-not-m2-g5-pass: a union number can
// never be recorded as an M2 or G5 cap0 verdict.
#pragma once

#include <bs/abstract_tree.hpp>
#include <bs/abstraction.hpp>
#include <cstddef>
#include <cstdint>

#include "cap_embedding.hpp"  // CapExtensionCompletion, kCapMaterialityGateBb
#include "river_solver.hpp"
#include "union_menu.hpp"

namespace bs::rfc0047 {

// Mechanical evidence markings (Stage-2b addendum subsection G).
inline constexpr const char* kUnionReferenceLabel = "union-candidate";
inline constexpr const char* kUnionM2Marking = "supporting-evidence-not-m2-g5-pass";

struct UnionMateriality {
  // Fixed strings, emitted on every row so the candidate domain is
  // mechanically distinguishable from the frozen G5 cap0 instrument.
  const char* reference_label = kUnionReferenceLabel;
  const char* m2_marking = kUnionM2Marking;
  // Source/artifact identity: abstraction id digests distinguish a declared
  // union from an identity(0) object; tree shape digests establish structural
  // equality independently of the id.
  std::uint64_t bounded_abstraction_digest = 0;
  std::uint64_t reference_abstraction_digest = 0;
  std::uint64_t bounded_shape_digest = 0;
  std::uint64_t reference_shape_digest = 0;
  std::size_t reference_action_nodes = 0;
  std::size_t bounded_action_nodes = 0;
  std::size_t mapped_nodes = 0;
  // Always 0 and enforced at runtime: the EV walk follows only positive
  // embedded-mass mapped edges and can never reach an extension subtree.
  std::size_t unmapped_ev_reachable_nodes = 0;
  // The bounded policy's own exact NashConv inside its bounded game.
  double bounded_nash_conv_bb = 0.0;
  // Exact exploitability of the embedded bounded policy inside the reference
  // (cap0 or union) game.
  double embedded_exploitability_bb = 0.0;
  // embedded - bounded NashConv; non-negative by the superset-BR proof.
  double pure_menu_term_bb = 0.0;
  // Candidate-domain verdict ONLY: embedded <= 0.002 BB under this reference.
  // Never an M2 or G5 pass; the row markings above say so mechanically.
  bool candidate_gate_pass = false;
};

// Solve the bounded policy under `bounded_action`, embed it into the explicit
// `reference_action` tree via the generalized subset map, and evaluate the
// union embedding identity. Both abstractions must be MinAndCap; the caller
// decides whether the reference is identity(0) (a direct subset case) or a
// union. Throws std::invalid_argument on every Stage-2b precondition failure.
UnionMateriality measure_subset_materiality(
    const bs::poker::GameDef& river_def, const bs::abstraction::ActionAbstraction& bounded_action,
    const bs::abstraction::ActionAbstraction& reference_action, std::uint64_t iterations,
    std::size_t threads, CapExtensionCompletion completion);

// Convenience entry: build the uncapped union reference
// declared(union(default, bounded) schedule, MinAndCap, 0) and measure
// against it.
UnionMateriality measure_union_materiality(const bs::poker::GameDef& river_def,
                                           const bs::abstraction::ActionAbstraction& bounded_action,
                                           std::uint64_t iterations, std::size_t threads,
                                           CapExtensionCompletion completion);

// Diagnostic oracle: raw (not divided by the 990 compatible-deal
// normalization) per-combo root chip sums. The EV walk is
// completion-independent and must reproduce the bounded kernel's root CFV
// times 990 to floating-point precision; the BR walk covers the full
// reference menu and is completion-dependent. Benchmark/test-only.
struct UnionRootValues {
  std::vector<double> ev_seat0;
  std::vector<double> br_seat0;
  std::vector<double> ev_seat1;
  std::vector<double> br_seat1;
};

UnionRootValues subset_root_values(const bs::poker::GameDef& river_def,
                                   const bs::abstraction::ActionAbstraction& bounded_action,
                                   const bs::abstraction::ActionAbstraction& reference_action,
                                   std::uint64_t iterations, std::size_t threads,
                                   CapExtensionCompletion completion);

}  // namespace bs::rfc0047
