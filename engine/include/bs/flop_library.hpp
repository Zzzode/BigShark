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
#include <bs/abstract_tree.hpp>
#include <bs/abstraction.hpp>
#include <bs/game_definition.hpp>
#include <bs/heads_up.hpp>         // poker::Chips
#include <bs/heads_up_solver.hpp>  // WeightedHand
#include <bs/nseat_trainer.hpp>
#include <bs/seat_policy.hpp>  // InformationKey
#include <bs/strategy_artifact.hpp>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
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
  abstraction::AbstractionId card_id;    // suit-canonical-v1
  abstraction::AbstractionId action_id;  // identity or declared-coarse
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
//
// `action` selects the action abstraction. The identity (default) seeds the
// legal minimum and all-in cap at every aggressive node, which is exact but
// explodes the tree at deeper stacks. The declared-coarse abstraction
// (RFC 0008 DeclaredOnly) removes those forced seeds, making 10 BB+ training
// practical at the cost of a coarser menu.
//
// RFC 0012 Phase 4: when `resume` is true, each class warm-starts from its
// `class-NNNN-checkpoint.db` if present (verifying the abstraction digests
// before continuing), and a checkpoint is written every 256 iterations so a
// crashed build can resume. The per-iteration PRNG streams depend on the
// absolute iteration number, so a warm-started run is bitwise-identical to a
// from-scratch run at the same iteration count.
LibraryManifest build_library(
    const std::filesystem::path& out_dir, std::size_t class_count,
    std::uint64_t iterations_per_class, std::uint64_t seed, poker::Chips stack,
    poker::Chips contribution, const std::vector<solver::WeightedHand>& range,
    std::size_t player_count = 2,
    abstraction::ActionAbstraction action = abstraction::ActionAbstraction::identity(),
    bool resume = false);

// RFC 0012 Phase 1: parameters for a library build, shared by the serial,
// parallel-worker, and fragment-merge paths.
struct BuildParams {
  std::size_t class_count = 0;
  std::uint64_t iterations_per_class = 0;
  std::uint64_t seed = 0;
  poker::Chips stack = 0;
  poker::Chips contribution = 0;
  std::vector<solver::WeightedHand> range;
  std::size_t player_count = 2;
  abstraction::ActionAbstraction action = abstraction::ActionAbstraction::identity();
  bool resume = false;
};

// RFC 0012 Phase 1: parallel worker entry point. Trains the classes at indices
// `worker_id`, `worker_id + parallel`, `worker_id + 2*parallel`, ... below
// class_count, writing each class artifact directly to `out_dir` and a manifest
// fragment to `out_dir / "manifest-part-<worker_id>.json"`. Called by a forked
// child process; the parent detects failure via the exit status. Each worker's
// SQLite files are independent (one class per file), so concurrent workers
// never write the same file.
void build_library_worker(const std::filesystem::path& out_dir, std::size_t worker_id,
                          std::size_t parallel, const BuildParams& params);

// RFC 0012 Phase 1: merge `manifest-part-*.json` fragments into the final
// `manifest.json`. Collects every worker's ClassEntry records, sorts them by
// class index, recomputes covered_flops and storage_bytes from the on-disk
// artifacts (counted, not trusted from the fragments), and writes
// `manifest.json`. Throws if a fragment or class artifact is missing.
LibraryManifest merge_fragments(const std::filesystem::path& out_dir, const BuildParams& params);

// RFC 0012 Phase 4: expand a trainer result's raw (bucket, node) table into
// concrete per-combo training rows (one (regret, average_weight) pair per
// action ordinal), ready for save_training_checkpoint. Walks the same tree as
// export_seat_policy; every concrete combo sharing a (bucket, node) carries
// identical regrets.
std::map<solver::InformationKey, artifacts::TrainingWeights> export_seat_training_rows(
    const solver::NSeatTrainingResult& trained, const tree::AbstractTree& tree);

// RFC 0012 Phase 4: map a loaded checkpoint's concrete training rows back to
// the trainer's (bucket, node) table for warm-start. Walks the tree to resolve
// each InformationKey to its (bucket, node); the first concrete combo for a
// (bucket, node) fills the row (all combos share identical regrets). The
// returned state's completed_iterations is the checkpoint's.
solver::NSeatInitialState build_initial_state(const artifacts::CheckpointTrainingState& ckpt,
                                              const tree::AbstractTree& tree);

// Write the manifest as JSON to `out_dir / "manifest.json"`.
void write_manifest(const LibraryManifest& manifest, const std::filesystem::path& out_dir);

}  // namespace bs::flop_library
