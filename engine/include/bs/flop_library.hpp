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

// RFC 0035: the declared root table the builder trains. A root is the exact
// ledger tuple same_root matches (pot, per-seat stacks/net contributions/
// folded mask) plus its diagnostic labels; the type is the same directory
// entry the packed BSFLIB03 file carries. Phase 1 supports only the
// live-uniform range profile; the reserved line-conditioned value (2) is
// refused. Root ids are the table positions and are assigned by order.
using RootSpec = resident::RootDirectoryEntry;

// RFC 0035: the production single-root shape — every seat live, equal
// contributions, no folds. Keeps the RFC 0033 derivation (chips behind are
// `starting_stack - contribution`) in one place: the CLI defaults to this one
// root, and multi-root builds declare their table explicitly.
RootSpec make_limped_root_spec(std::size_t player_count, poker::Chips starting_stack,
                               poker::Chips contribution,
                               resident::RootLineTag line_tag = resident::RootLineTag::Limped);

// RFC 0035: FNV-1a 64-bit hash of the declared, ORDERED root table. The
// canonical byte stream begins with the player count (one byte) and then, per
// root in table order, the line tag (u8), pot (u64 LE), stacks and
// contributions (player_count × u64 LE), folded mask (player_count × u8),
// live count (u8), and range profile (u8). Fixed field strides make the
// encoding unambiguous without separators; the ordering matters, so two
// different root sets never share a hash.
std::uint64_t root_table_hash(std::size_t player_count, std::span<const RootSpec> roots);

// One trained (root, class) artifact entry in the library. RFC 0027: a class
// is stored as a section inside the packed library, so records name the pair
// by board and carry its exact measurements instead of a filename.
struct ClassEntry {
  std::uint32_t root_id = 0;
  std::array<int, 3> canonical_board{};
  std::size_t stored_rows = 0;  // concrete policy rows persisted
  std::uint64_t completed_iterations = 0;
  std::size_t information_sets = 0;  // trainer's bucket-keyed table size
  std::size_t section_bytes = 0;     // unpadded BSFLIB03 section length
};

// RFC 0035: one root's coverage and storage as recorded in the manifest's
// served_roots array. Every measurement is exact (counted, not estimated).
struct ServedRootEntry {
  RootSpec root;
  std::size_t class_count = 0;       // trained classes for this root
  std::size_t covered_flops = 0;     // of 22,100 concrete flops
  std::size_t total_flops = 0;       // 22,100
  std::uintmax_t section_bytes = 0;  // sum of unpadded section lengths
  std::vector<ClassEntry> classes;
};

// The honest record of one library build, written as manifest.json beside the
// artifacts. Every measurement is exact (counted, not estimated); the
// coverage fraction is covered_flops / total_flops.
struct LibraryManifest {
  abstraction::AbstractionId card_id;  // suit-canonical-v1
  abstraction::CardBucketKind card_bucket_kind =
      solver::kNSeatCardKind;            // RFC 0031: ordinal pinned in build.json and checkpoints
  abstraction::AbstractionId action_id;  // identity or declared-coarse
  std::size_t player_count = 0;
  poker::Chips big_blind = 0;
  std::uint64_t iterations_per_class = 0;
  std::uint64_t seed = 0;
  // RFC 0024: cap on full raises per street (0 = no cap). RFC 0030 pins it in
  // the action-abstraction digest and records it here for the manifest.
  std::uint8_t max_raises_per_street = 0;
  // RFC 0035: one entry per trained root, each naming the exact ledger, the
  // line/range labels, and the class coverage. This replaces the single
  // served_root block.
  std::vector<ServedRootEntry> served_roots;
  // Flat per-(root,class) records, root-major then class-ascending.
  std::vector<ClassEntry> classes;
  // Canonical boards geometrically undealable for this (range, seat_count):
  // the board leaves too few range cards to give every seat a distinct hand
  // (e.g. KsQsJs against a J+ premium range). Such classes have no legal
  // joint-deal stream under ANY sampler, so they are excluded rather than
  // failed; recorded explicitly so coverage is never silently underclaimed.
  std::vector<std::array<int, 3>> undealable_boards;
  // Honest measurements (totals across every root).
  std::size_t class_count = 0;       // sections in this library
  std::size_t total_classes = 0;     // 1,755
  std::size_t covered_flops = 0;     // sum across roots (of 22,100 concrete flops each)
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
  // RFC 0035: the declared root table hash (build identity / checkpoints).
  std::uint64_t root_table_hash = 0;
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

// Build a class library with the single limped production root: train each
// of the first `class_count` canonical classes (by board-id order) and
// serialize it straight into its BSFLIB03 section, then assemble
// `library.bin` + `manifest.json` into `out_dir` (RFC 0027). Multi-root
// builds go through BuildParams.roots and build_library(BuildParams).
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
// RFC 0012 Phase 4: when `resume` is true, each (root, class) warm-starts
// from its checkpoint if present (verifying the identity witnesses before
// continuing), and a checkpoint is written every 256 iterations. The
// per-iteration PRNG streams depend on the absolute iteration number, so a
// warm-started run is bitwise-identical to a from-scratch run at the same
// iteration count. RFC 0027: sections already staged under `.sections/` are
// skipped, and the requested build identity is checked against
// `.sections/build.json` first.
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
  // RFC 0033: the buy-in for the default single-root shape. The root's chips
  // behind are stack - contribution. Used only when `roots` is empty, in
  // which case the builder declares one live-uniform limped root with equal
  // per-seat ledgers. An explicit multi-root table ignores these.
  poker::Chips starting_stack = 0;
  poker::Chips contribution = 0;
  // RFC 0035: the declared root table. Empty means "one limped root derived
  // from starting_stack/contribution/player_count". Roots are trained in
  // order; root ids are table positions; each root trains the same canonical
  // class stride. The reserved line-conditioned profile (2) is refused.
  std::vector<RootSpec> roots;
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
//   1. torn checkpoint          -> discard and retrain (including any legacy
//                                  BSWARM01 file under the BSWARM02 codec)
//   2. incompatible revision    -> refuse
//   3. incompatible digest/rows -> deferred to `train_one_class`, which runs
//                                  them before the stale removal
//   4. stale but complete       -> classify as disposable; removed later by
//                                  `train_one_class`, never here
//   5. valid state, no identity -> refuse
//
// RFC 0030 adds a worker-side sixth refusal: a BSWARM02 tree-shape witness
// mismatch (or a foreign card bucket kind) hard-refuses in `train_one_class`
// before any row is adopted, with the checkpoint preserved. It is checked
// before the stale-but-complete branch, since a witness mismatch is the
// stronger drift signal.
//
// Dispositions 2 and 4 can apply to the same file with opposite outcomes, so
// the order is load-bearing. The section scan is what protects `library.bin`:
// a completed class keeps its `.bsf` and deletes its checkpoint, so a
// checkpoint-only guard would protect exactly the work that is discarded.
// `iterations_per_class` is required to distinguish a stale-but-complete
// checkpoint from one genuinely in flight; `inspect_sealed_checkpoint` returns
// the completed count header-only.
void preflight_resume(const std::filesystem::path& out_dir, const BuildParams& params, bool resume);

// RFC 0027: establish `.sections/build.json` (the build identity) before any
// worker starts. On a fresh build the parent writes it; on `--resume` it
// validates the existing file and fails closed on any difference. The parent
// owns this file; a direct-worker test path must call the same helper.
// `build_library_worker` fails closed when it is absent.
void prepare_build_identity(const std::filesystem::path& out_dir, const BuildParams& params,
                            bool resume);

// RFC 0027: parallel worker entry point. Trains the (root, class) stride at
// indices `worker_id`, `worker_id + parallel`, ... below class_count for
// EVERY declared root, writing each completed class to
// `.sections/root-NNNN-class-NNNN.bsf` and one JSON line to
// `.sections/manifest-part-<worker_id>.jsonl`. Pairs the parent already has
// under `.sections/` are skipped. Called by a forked child process; the
// parent detects failure via the exit status. Concurrent workers write
// disjoint files, so no locking is required.
void build_library_worker(const std::filesystem::path& out_dir, std::size_t worker_id,
                          std::size_t parallel, const BuildParams& params);

// RFC 0027/RFC 0035: assemble the library after every worker exits. Derives
// the expected dealable class set from `params` (the same set for every
// root), requires a section for every (root, class) pair, reconciles the
// worker fragments, writes `library.bin` + `.sha256` + `manifest.json` with a
// served_roots[] entry per root, and removes `.sections/`. Throws naming the
// missing or inconsistent pair.
LibraryManifest assemble_library(const std::filesystem::path& out_dir, const BuildParams& params);

// RFC 0035: the programmatic multi-root entry point. Builds the library
// described by `params`, declaring params.roots explicitly (an empty table
// becomes the single limped production root). Serial and parallel parent
// callers both go through this; the positional build_library() wrapper is a
// thin adapter over a one-root BuildParams.
LibraryManifest build_library(const std::filesystem::path& out_dir, const BuildParams& params);

// Write the manifest as JSON to `out_dir / "manifest.json"`.
void write_manifest(const LibraryManifest& manifest, const std::filesystem::path& out_dir);

}  // namespace bs::flop_library
