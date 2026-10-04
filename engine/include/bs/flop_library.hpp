// flop_library.hpp — RFC 0009 W4c-iii: offline flop class library builder.
//
// Trains and publishes per-class flop-rooted schema-v3 artifacts (D5's
// "precomputed flop libraries"): each artifact is trained on one canonical
// suit-isomorphism class representative and self-describes as a class policy
// via the suit-canonicalization card-abstraction declaration. The resident
// layer (W4c-ii) canonicalizes a query board into the artifact's coordinates
// before lookup, so one trained artifact serves every concrete flop in its
// class.
//
// This is offline solver-side tooling (the D5 dependency rule): it links the
// solver and artifact libraries but never the service, host, or protocol
// targets. A library's coverage and storage are measured and reported
// honestly in its manifest, never claimed as complete.
#pragma once

#include <array>
#include <bs/abstraction.hpp>
#include <bs/game_definition.hpp>
#include <bs/heads_up.hpp>         // poker::Chips
#include <bs/heads_up_solver.hpp>  // WeightedHand
#include <bs/nseat_trainer.hpp>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace bs::flop_library {

// One trained class artifact in the library.
struct ClassEntry {
  std::array<int, 3> canonical_board{};
  std::string artifact_name;  // filename within the library directory
  std::string sha256_hex;
  std::size_t stored_rows = 0;  // concrete policy rows persisted
  std::uint64_t completed_iterations = 0;
  std::size_t information_sets = 0;
};

// The honest record of one library build, written as manifest.json beside the
// artifacts. Every measurement is exact (counted, not estimated); the
// coverage fraction is covered_flops / total_flops.
struct LibraryManifest {
  abstraction::AbstractionId card_id;  // suit-canonical-v1
  std::size_t player_count = 0;
  poker::Chips big_blind = 0;
  poker::Chips stack = 0;         // remaining per seat (chips)
  poker::Chips contribution = 0;  // per-seat pot contribution (chips)
  std::uint64_t iterations_per_class = 0;
  std::uint64_t seed = 0;
  std::vector<ClassEntry> classes;
  // Honest measurements.
  std::size_t class_count = 0;       // classes in this library
  std::size_t total_classes = 0;     // 1,755
  std::size_t covered_flops = 0;     // of 22,100 concrete flops
  std::size_t total_flops = 0;       // 22,100
  std::uintmax_t storage_bytes = 0;  // sum of published artifact sizes
};

// Enumerate all 1,755 canonical flop classes, sorted by canonical board id
// (the declared selection order for the first library).
std::vector<std::array<int, 3>> enumerate_classes();

// Count of the 22,100 concrete flops whose canonical class is in `covered`.
std::size_t count_covered_flops(const std::vector<std::array<int, 3>>& covered);

// The declared first-library range: a fixed set of premium holdings used for
// every seat. Board-overlapping combos are filtered per class at build time.
std::vector<solver::WeightedHand> declared_library_range();

// Build a class library: train and publish a schema-v3 artifact for each of
// the first `class_count` canonical classes (by board-id order). Writes
// `class-NNNN.db` artifacts and `manifest.json` into `out_dir`. Throws on any
// failure (no partial library is published).
//
// `player_count` sets the number of seats (2..10). All seats use the same
// range and stack profile. The manifest records the player count so the
// resident layer can select the right library for a multi-way hand.
LibraryManifest build_library(const std::filesystem::path& out_dir, std::size_t class_count,
                              std::uint64_t iterations_per_class, std::uint64_t seed,
                              poker::Chips stack, poker::Chips contribution,
                              const std::vector<solver::WeightedHand>& range,
                              std::size_t player_count = 2);

// Write the manifest as JSON to `out_dir / "manifest.json"`.
void write_manifest(const LibraryManifest& manifest, const std::filesystem::path& out_dir);

}  // namespace bs::flop_library
