// stage6/geometry.hpp — R3b reachable flop-geometry matrix enumeration.
//
// The trained postflop candidate is keyed by materialized rooted-flop trees,
// and a rooted tree bakes concrete chip totals into every node index. Limped
// pots, single-raised pots, 3-bet pots and 4-bet pots, crossed with live-seat
// subsets, are therefore DISTINCT artifacts. This component enumerates the
// finite set of flop chip geometries the measured composed candidate and the
// R8 deviator can reach, without dealing a single card.
//
// At every preflop state it asks the PINNED chart preflop policy what action
// each of the 169 holdings maps to (charts are deterministic preflop), unions
// that with the finite R8 deviation grid, and branches over the distinct
// concrete (action kind, target total) pairs. Distinct totals are mandatory:
// one state can emit both a jam (raiseMax) and a sized raise. The walk uses
// real GameState transitions; when preflop closes into the flop deal it
// records a GeometrySignature.
#pragma once

#include <array>
#include <bs/game_definition.hpp>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace bs::stage6 {

// A deduplicated flop chip geometry, button-canonical (seat 0 = button).
// Concrete board cards are deliberately excluded: tree topology is
// board-value-independent. Two flops with the same chips and live set share an
// artifact.
struct GeometrySignature {
  std::size_t player_count = 0;
  // Chips behind at the flop, per seat, button-relative order.
  std::array<poker::Chips, 10> stacks{};
  // Total each seat put in preflop (dead money at the rooted flop), per seat.
  std::array<poker::Chips, 10> contributed{};
  // Seats that reach the flop (did not fold), button-relative seat indices.
  std::vector<std::size_t> live;
  poker::Chips pot = 0;

  std::string to_string() const;
  bool operator==(const GeometrySignature&) const = default;
};

// The chart/deviation-grid flop geometry ENUMERATORS (which drive the pinned
// chart policy) are declared in stage6/geometry_enumerator.hpp and compiled
// into bigshark_stage6_eval. The pure signature/bucketing surface below is in
// bigshark_stage6_core and carries no policy dependency.

// Revision 3: pot-scale bucket key. Strategy is pot-scale invariant, so
// geometries collapse onto (pot in whole big blinds, count of live seats,
// count of live seats with postflop stack behind). Buckets with zero acting
// seats are all-in-at-flop runouts (no policy queried). This is a named
// abstraction (geometry quantization), separate from card bucketing.
struct GeometryBucketKey {
  std::size_t player_count = 0;
  poker::Chips pot_bb = 0;       // pot rounded to whole big blinds (2 chips)
  std::size_t live_count = 0;    // seats reaching the flop
  std::size_t acting_count = 0;  // live seats with stack behind (>=1)

  std::string to_string() const;
  bool operator==(const GeometryBucketKey&) const = default;
};

// Maps an exact signature to its pot-scale bucket key.
GeometryBucketKey bucket_key_for(const GeometrySignature& sig, poker::Chips big_blind);

// Deterministic 64-bit public-token for a bucket key: the FNV-1a of its
// canonical string. The streaming trainer's PublicPath and every frozen
// artifact seed their four-token prefix from this value, so the token is
// stable across processes and independent of the bucket's member signatures.
std::uint64_t geometry_bucket_token(const GeometryBucketKey& key);

// Builds the flop chip geometry of a LIVE GameState at the instant the flop is
// dealt (state.phase() == Deal with three board cards, or the first Action
// state rooted on a flop). Reads only public chip fields, exactly as the
// enumerator's record_signature does, so a simulator flop and an enumerated
// signature use one construction. Throws std::invalid_argument if the state is
// not at a three-card flop.
GeometrySignature flop_signature(const poker::GameState& state);

// Typed refusal raised when a simulated flop's geometry has no frozen artifact.
// R3b requires the simulator's lookup to be total: an uncovered signature is a
// harness/config error that must abort the measurement, never silently fall
// back to a clamp, a different tree, or a skipped hand.
class stage6_geometry_uncovered : public std::runtime_error {
 public:
  explicit stage6_geometry_uncovered(const std::string& what) : std::runtime_error(what) {}
};

// A frozen, deduplicated set of exact flop geometries the measured candidate
// has artifacts for, keyed for TOTAL lookup. The simulator computes each reached
// flop's signature and calls require_covered(); a miss throws
// stage6_geometry_uncovered. Construction content-hashes the membership so the
// frozen matrix a run used can be recorded and compared.
class GeometryCoverage {
 public:
  GeometryCoverage() = default;
  explicit GeometryCoverage(std::vector<GeometrySignature> covered);

  bool covers(const GeometrySignature& sig) const;
  // Returns the signature or throws stage6_geometry_uncovered.
  GeometrySignature require_covered(const GeometrySignature& sig) const;

  std::size_t size() const noexcept { return covered_.size(); }
  const std::vector<GeometrySignature>& signatures() const noexcept { return covered_; }
  // Deterministic FNV-1a-style content hash over the sorted, deduplicated
  // signatures' canonical strings, so the frozen matrix is reproducible.
  std::uint64_t content_hash() const noexcept;

 private:
  std::vector<GeometrySignature> covered_;  // sorted, deduplicated by to_string
};

// One bucketed geometry with the reduced representative GameDef inputs. The
// representative is a LIVE-COUNT-ONLY rooted game (a rooted GameDef cannot
// carry folded seats), with equal acting stacks set to the MINIMUM acting
// depth among members and dead money reconciled to an exact pot.
// `actionable` is false for all-in-at-flop buckets (pure runout).
struct GeometryBucket {
  GeometryBucketKey key;
  bool actionable = false;
  // The big-blind denomination the bucket was quantized under; the reduced
  // rooted GameDef needs it even though the root is past blind posting.
  poker::Chips big_blind = 0;
  std::vector<GeometrySignature> members;
  std::vector<poker::Chips> representative_stacks;   // per live seat
  std::vector<poker::Chips> representative_contrib;  // per live seat (dead)
  poker::Chips representative_pot = 0;
  poker::Chips min_acting_stack = 0;
  bool mixed_all_in_for_less = false;  // no-mix guard flag (must stay false)
};

// Quantizes exact signatures into deduplicated buckets and computes reduced
// representative inputs. Throws std::runtime_error if a bucket mixes an
// all-in-for-less live seat with acting members (the no-mix guard) or the
// reconciled pot cannot be formed from integer chips.
std::vector<GeometryBucket> bucket_geometries(std::size_t player_count, poker::Chips big_blind,
                                              const std::vector<GeometrySignature>& exact);

}  // namespace bs::stage6
