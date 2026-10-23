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
#include <bs/mmap_library.hpp>     // ProbabilityLayout, LibraryIdentity
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

// One trained class artifact in the library. RFC 0027: a class is stored as a
// section inside the packed library, not as a standalone file, so records name
// the class by board and carry its exact measurements instead of a filename.
struct ClassEntry {
  std::array<int, 3> canonical_board{};
  std::size_t stored_rows = 0;  // concrete policy rows persisted
  std::uint64_t completed_iterations = 0;
  std::size_t information_sets = 0;  // trainer's bucket-keyed table size
  std::size_t section_bytes = 0;     // unpadded BSFLIB02 section length
};

// The honest record of one library build, written as manifest.json beside the
// artifacts. Every measurement is exact (counted, not estimated); the
// coverage fraction is covered_flops / total_flops.
struct LibraryManifest {
  abstraction::AbstractionId card_id;    // suit-canonical-v1
  abstraction::AbstractionId action_id;  // identity or declared-coarse
  std::size_t player_count = 0;
  poker::Chips big_blind = 0;
  // RFC 0033: the one root shape every class answers. same_root compares
  // player_count, pot, big_blind, button, board, and per-seat
  // (stacks, contributions, folded), so this is the whole reachable set.
  std::uint32_t served_root_button = 0;
  poker::Chips starting_stack = 0;  // buy-in per seat (chips)
  poker::Chips stack = 0;           // chips behind at the root: starting_stack - contribution
  poker::Chips contribution = 0;    // per-seat pot contribution (chips)
  std::uint64_t iterations_per_class = 0;
  std::uint64_t seed = 0;
  // RFC 0024: cap on full raises per street (0 = no cap). Recorded so the
  // published artifact carries the action-abstraction configuration that
  // shaped its tree; the cap is not part of the AbstractionId digest.
  std::uint8_t max_raises_per_street = 0;
  std::vector<ClassEntry> classes;
  // Canonical boards geometrically undealable for this (range, seat_count):
  // the board leaves too few range cards to give every seat a distinct hand
  // (e.g. KsQsJs against a J+ premium range). Such classes have no legal
  // joint-deal stream under ANY sampler, so they are excluded rather than
  // failed; recorded explicitly so coverage is never silently underclaimed.
  std::vector<std::array<int, 3>> undealable_boards;
  // Honest measurements.
  std::size_t class_count = 0;       // classes in this library
  std::size_t total_classes = 0;     // 1,755
  std::size_t covered_flops = 0;     // of 22,100 concrete flops
  std::size_t total_flops = 0;       // 22,100
  std::uintmax_t storage_bytes = 0;  // size of the packed library file
  // RFC 0027: the packed library file is the artifact.
  std::string library_bin;         // "library.bin" relative to out_dir
  std::string library_bin_sha256;  // lowercase hex, matches <bin>.sha256
  std::uintmax_t library_bin_bytes = 0;
  std::string codebook;  // "codebook16" | "doubles"
  // Training-range identity. `range` names the declared profile, the count is
  // its combo count, and the digest is FNV-1a 64-bit over the canonical sorted
  // (combo_index, weight_bits) pairs so two ranges with the same name and
  // count but different weights are distinguishable.
  std::string range;
  std::size_t range_combo_count = 0;
  std::uint64_t range_digest = 0;
};

// Enumerate all 1,755 canonical flop classes, sorted by canonical board id
// (the declared selection order for the first library).
std::vector<std::array<int, 3>> enumerate_classes();

// Count of the 22,100 concrete flops whose canonical class is in `covered`.
std::size_t count_covered_flops(const std::vector<std::array<int, 3>>& covered);

// The declared first-library range: a fixed set of premium holdings used for
// every seat. Board-overlapping combos are filtered per class at build time.
std::vector<solver::WeightedHand> declared_library_range();

// The full preflop range: all 1,326 unordered hole-card combos at unit
// weight. Board-overlapping combos are filtered per class at build time. The
// trainer still seals rows under the declared card buckets, but the wider
// range gives every bucket realistic reach and opponent support.
std::vector<solver::WeightedHand> declared_full_library_range();

// Whether `seat_count` seats can each be dealt a distinct non-conflicting
// two-card combo from `range` after removing the three board cards. Exact
// bounded depth-first edge packing over the board-filtered range graph
// (returns true if and only if `seat_count` pairwise vertex-disjoint combos
// exist). A false result means the class is geometrically undealable for this
// (range, seat count) and cannot be sampled or trained under any joint dealer
// — such classes are excluded from the build and listed in the manifest, not
// failed.
bool class_is_dealable(const std::array<int, 3>& board, std::size_t seat_count,
                       const std::vector<solver::WeightedHand>& range);

// Build a class library: train each of the first `class_count` canonical
// classes (by board-id order) and serialize it straight into its BSFLIB02
// section, then assemble `library.bin` + `manifest.json` into `out_dir`
// (RFC 0027). No per-class SQLite policy artifact is written; the only
// intermediate is the per-class warm-start checkpoint. Throws on any failure
// (no partial library is published).
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
// from-scratch run at the same iteration count. RFC 0027: class sections
// already renamed under `.sections/` are skipped, and the requested build
// identity is checked against `.sections/build.json` first.
LibraryManifest build_library(
    const std::filesystem::path& out_dir, std::size_t class_count,
    std::uint64_t iterations_per_class, std::uint64_t seed, poker::Chips starting_stack,
    poker::Chips contribution, const std::vector<solver::WeightedHand>& range,
    std::size_t player_count = 2,
    abstraction::ActionAbstraction action = abstraction::ActionAbstraction::identity(),
    bool resume = false,
    resident::ProbabilityLayout layout = resident::ProbabilityLayout::Codebook16,
    const std::string& range_name = "declared");

// RFC 0012 Phase 1: parameters for a library build, shared by the serial and
// parallel worker paths.
struct BuildParams {
  std::size_t class_count = 0;
  std::uint64_t iterations_per_class = 0;
  std::uint64_t seed = 0;
  // RFC 0033: the buy-in. The root's chips behind are stack - contribution.
  poker::Chips starting_stack = 0;
  poker::Chips contribution = 0;
  std::vector<solver::WeightedHand> range;
  std::size_t player_count = 2;
  abstraction::ActionAbstraction action = abstraction::ActionAbstraction::identity();
  bool resume = false;
  // RFC 0027: probability layout and the declared range profile name.
  resident::ProbabilityLayout layout = resident::ProbabilityLayout::Codebook16;
  std::string range_name = "declared";
};

// RFC 0027: FNV-1a 64-bit digest over the canonical range: entries in
// ascending combo_index order, each little-endian u32 combo_index followed by
// the little-endian IEEE-754 bits of the weight, with no separators.
std::uint64_t range_digest(const std::vector<solver::WeightedHand>& range);

// RFC 0033: the resume decision, shared by the serial library path and the
// parallel builder parent so the two cannot drift. Contract, applied in order:
//
//   1. torn checkpoint          -> discard and retrain
//   2. incompatible revision    -> refuse
//   3. incompatible digest/rows -> deferred to `train_one_class`, which runs
//                                  them before the stale removal
//   4. stale but complete       -> classify as disposable; removed later by
//                                  `train_one_class`, never here
//   5. valid state, no identity -> refuse
//
// Dispositions 2 and 4 can apply to the same file with opposite outcomes, so
// the order is load-bearing. The section scan is what protects `library.bin`:
// a completed class keeps its `.bsf` and deletes its checkpoint, so a
// checkpoint-only guard would protect exactly the work that is discarded.
// `iterations_per_class` is required to distinguish a stale-but-complete
// checkpoint from one genuinely in flight; `inspect_sealed_checkpoint` returns
// the completed count header-only.
void preflight_resume(const std::filesystem::path& out_dir, std::size_t class_count,
                      std::uint64_t iterations_per_class, bool resume);

// RFC 0027: establish `.sections/build.json` (the build identity) before any
// worker starts. On a fresh build the parent writes it; on `--resume` it
// validates the existing file and fails closed on any difference. The parent
// owns this file; a direct-worker test path must call the same helper.
// `build_library_worker` fails closed when it is absent.
void prepare_build_identity(const std::filesystem::path& out_dir, const BuildParams& params,
                            bool resume);

// RFC 0027: parallel worker entry point. Trains the classes at indices
// `worker_id`, `worker_id + parallel`, `worker_id + 2*parallel`, ... below
// class_count, writing each completed class to
// `.sections/class-NNNN.bsf` and one JSON line to
// `.sections/manifest-part-<worker_id>.jsonl`. Sections the parent already has
// under `.sections/` are skipped. Called by a forked child process; the parent
// detects failure via the exit status. Concurrent workers write disjoint
// files, so no locking is required.
void build_library_worker(const std::filesystem::path& out_dir, std::size_t worker_id,
                          std::size_t parallel, const BuildParams& params);

// RFC 0027: assemble the library after every worker exits. Derives the expected
// dealable class set from `params`, requires a section for each, reconciles the
// worker fragments, writes `library.bin` + `.sha256` + `manifest.json`, and
// removes `.sections/`. Throws naming the missing or inconsistent class.
LibraryManifest assemble_library(const std::filesystem::path& out_dir, const BuildParams& params);

// Write the manifest as JSON to `out_dir / "manifest.json"`.
void write_manifest(const LibraryManifest& manifest, const std::filesystem::path& out_dir);

}  // namespace bs::flop_library
