// RFC 0012 Phase 1: flop library parallel training tests.
//
// Verifies that training the same class set with one serial builder and with
// N strided workers (merged by merge_fragments) produces bitwise-identical
// per-class artifacts. The worker/merge path is the same code the forked
// builder processes run; this test exercises it in-process.
#include <array>
#include <bs/abstract_tree.hpp>
#include <bs/abstraction.hpp>
#include <bs/equity_frontier.hpp>
#include <bs/flop_library.hpp>
#include <bs/nseat_trainer.hpp>
#include <bs/sealed_checkpoint.hpp>
#include <bs/strategy_artifact.hpp>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#define CHECK(cond)                                                         \
  do {                                                                      \
    if (!(cond)) {                                                          \
      std::printf("CHECK failed: %s (%s:%d)\n", #cond, __FILE__, __LINE__); \
      return false;                                                         \
    }                                                                       \
  } while (0)

namespace {

namespace fs = std::filesystem;

bool test_parallel_matches_serial(const fs::path& dir) {
  const auto range = bs::flop_library::declared_library_range();
  const std::size_t class_count = 8;
  const std::uint64_t iterations = 10;  // small: one 256-iteration chunk
  const bs::poker::Chips stack = 6;     // RFC 0033: starting stack; 6 - 2 = 4 behind
  const bs::poker::Chips contribution = 2;
  const std::size_t player_count = 2;
  const std::uint64_t seed = 20261002;

  // Serial build (parallel = 1).
  const fs::path serial_dir = dir / "serial";
  fs::create_directories(serial_dir);
  const bs::flop_library::LibraryManifest serial_manifest = bs::flop_library::build_library(
      serial_dir, class_count, iterations, seed, stack, contribution, range, player_count);

  // Parallel build: 4 workers, each training a strided class range.
  const fs::path parallel_dir = dir / "parallel";
  fs::create_directories(parallel_dir);
  bs::flop_library::BuildParams params;
  params.class_count = class_count;
  params.iterations_per_class = iterations;
  params.seed = seed;
  params.starting_stack = stack;
  params.contribution = contribution;
  params.range = range;
  params.player_count = player_count;
  const std::size_t parallel = 4;
  bs::flop_library::prepare_build_identity(parallel_dir, params, false);
  for (std::size_t w = 0; w < parallel; ++w)
    bs::flop_library::build_library_worker(parallel_dir, w, parallel, params);
  const bs::flop_library::LibraryManifest parallel_manifest =
      bs::flop_library::assemble_library(parallel_dir, params);

  CHECK(serial_manifest.class_count == class_count);
  CHECK(parallel_manifest.class_count == class_count);
  CHECK(serial_manifest.covered_flops == parallel_manifest.covered_flops);
  CHECK(serial_manifest.storage_bytes == parallel_manifest.storage_bytes);

  // RFC 0027: the serial and parallel builds must produce the same packed
  // bytes and identical manifest records.
  CHECK(serial_manifest.library_bin_sha256 == parallel_manifest.library_bin_sha256);
  CHECK(bs::artifacts::sha256_file_hex(serial_dir / serial_manifest.library_bin) ==
        bs::artifacts::sha256_file_hex(parallel_dir / parallel_manifest.library_bin));
  for (std::size_t i = 0; i < class_count; ++i) {
    CHECK(serial_manifest.classes[i].completed_iterations ==
          parallel_manifest.classes[i].completed_iterations);
    CHECK(serial_manifest.classes[i].stored_rows == parallel_manifest.classes[i].stored_rows);
    CHECK(serial_manifest.classes[i].section_bytes == parallel_manifest.classes[i].section_bytes);
    CHECK(serial_manifest.classes[i].canonical_board ==
          parallel_manifest.classes[i].canonical_board);
  }

  // No checkpoint and no section staging directory remain after assembly.
  for (std::size_t i = 0; i < class_count; ++i) {
    char stem[16];
    std::snprintf(stem, sizeof(stem), "class-%04zu", i);
    CHECK(!fs::exists(parallel_dir / (std::string(stem) + "-checkpoint.bswarm")));
  }
  CHECK(!fs::exists(parallel_dir / ".sections"));
  return true;
}

bool test_resume_matches_fresh(const fs::path& dir) {
  // RFC 0012 Phase 4: a resumed build (warm-start from checkpoints) must
  // produce the same artifacts as a fresh build. Train 2 classes to 300
  // iterations (two 256-iteration chunks), then resume into a clean directory
  // and verify the artifacts match a fresh 300-iteration build.
  const auto range = bs::flop_library::declared_library_range();
  const std::size_t class_count = 2;
  const std::uint64_t iterations = 300;  // two chunks: 256 + 44
  const bs::poker::Chips stack = 6;      // RFC 0033: starting stack; 6 - 2 = 4 behind
  const bs::poker::Chips contribution = 2;
  const std::size_t player_count = 2;
  const std::uint64_t seed = 20261002;

  const fs::path fresh_dir = dir / "resume-fresh";
  fs::create_directories(fresh_dir);
  const bs::flop_library::LibraryManifest fresh_manifest = bs::flop_library::build_library(
      fresh_dir, class_count, iterations, seed, stack, contribution, range, player_count);

  // Resumed build: same params, resume=true. No checkpoints exist yet, so this
  // trains from scratch but exercises the chunk-loop + checkpoint path.
  const fs::path resume_dir = dir / "resume-warm";
  fs::create_directories(resume_dir);
  const bs::flop_library::LibraryManifest resume_manifest = bs::flop_library::build_library(
      resume_dir, class_count, iterations, seed, stack, contribution, range, player_count,
      bs::abstraction::ActionAbstraction::identity(), true);

  CHECK(fresh_manifest.class_count == class_count);
  CHECK(resume_manifest.class_count == class_count);
  CHECK(fresh_manifest.library_bin_sha256 == resume_manifest.library_bin_sha256);
  for (std::size_t i = 0; i < class_count; ++i)
    CHECK(resume_manifest.classes[i].completed_iterations == iterations);
  return true;
}

bool test_resume_skips_published_classes(const fs::path& dir) {
  // A second resume over a directory where every class is already PUBLISHED
  // must skip training for all of them (publish refuses to overwrite) and
  // rebuild the manifest from the existing artifacts. This is the crash
  // window between the final publish and the fragment/merge write.
  const auto range = bs::flop_library::declared_library_range();
  const std::size_t class_count = 3;
  const std::uint64_t iterations = 64;
  const bs::poker::Chips stack = 6;  // RFC 0033: starting stack; 6 - 2 = 4 behind
  const bs::poker::Chips contribution = 2;
  const std::size_t player_count = 2;
  const std::uint64_t seed = 20261017;

  const fs::path lib_dir = dir / "resume-published";
  fs::create_directories(lib_dir);
  const bs::flop_library::LibraryManifest first = bs::flop_library::build_library(
      lib_dir, class_count, iterations, seed, stack, contribution, range, player_count);
  CHECK(first.class_count == class_count);
  CHECK(fs::exists(lib_dir / first.library_bin));

  // Second build with resume=true reproduces the same packed bytes.
  const bs::flop_library::LibraryManifest second = bs::flop_library::build_library(
      lib_dir, class_count, iterations, seed, stack, contribution, range, player_count,
      bs::abstraction::ActionAbstraction::identity(), true);
  CHECK(second.class_count == class_count);
  CHECK(second.library_bin_sha256 == first.library_bin_sha256);
  for (std::size_t i = 0; i < class_count; ++i) {
    CHECK(second.classes[i].stored_rows == first.classes[i].stored_rows);
    CHECK(second.classes[i].completed_iterations == iterations);
  }
  return true;
}

bool test_merge_reuses_prior_manifest(const fs::path& dir) {
  // After a complete serial build wrote manifest.json, a parallel worker
  // resume over the same directory must fill the skipped classes' digest and
  // counts from the existing manifest (merge_fragments / read_manifest_index)
  // rather than probing every artifact, and produce identical records.
  const auto range = bs::flop_library::declared_library_range();
  const std::size_t class_count = 4;
  const std::uint64_t iterations = 16;
  const bs::poker::Chips stack = 6;  // RFC 0033: starting stack; 6 - 2 = 4 behind
  const bs::poker::Chips contribution = 2;
  const std::size_t player_count = 2;
  const std::uint64_t seed = 20261017;

  const fs::path lib_dir = dir / "merge-manifest-reuse";
  fs::create_directories(lib_dir);
  const bs::flop_library::LibraryManifest first = bs::flop_library::build_library(
      lib_dir, class_count, iterations, seed, stack, contribution, range, player_count);

  bs::flop_library::BuildParams params;
  params.class_count = class_count;
  params.iterations_per_class = iterations;
  params.seed = seed;
  params.starting_stack = stack;
  params.contribution = contribution;
  params.range = range;
  params.player_count = player_count;
  params.resume = true;
  const std::size_t parallel = 4;
  bs::flop_library::prepare_build_identity(lib_dir, params, params.resume);
  for (std::size_t w = 0; w < parallel; ++w)
    bs::flop_library::build_library_worker(lib_dir, w, parallel, params);
  const bs::flop_library::LibraryManifest merged =
      bs::flop_library::assemble_library(lib_dir, params);

  CHECK(merged.class_count == class_count);
  CHECK(merged.library_bin_sha256 == first.library_bin_sha256);
  for (std::size_t i = 0; i < class_count; ++i) {
    CHECK(merged.classes[i].stored_rows == first.classes[i].stored_rows);
    CHECK(merged.classes[i].information_sets == first.classes[i].information_sets);
    CHECK(merged.classes[i].completed_iterations == first.classes[i].completed_iterations);
    CHECK(merged.classes[i].canonical_board == first.classes[i].canonical_board);
  }
  CHECK(merged.storage_bytes == first.storage_bytes);
  CHECK(merged.covered_flops == first.covered_flops);
  return true;
}

// RFC 0028 AC2: resuming from a sealed checkpoint at each interruption point
// the 256-iteration interval can produce must reproduce an uninterrupted run
// byte for byte.
//
// The checkpoint is produced by the same training path the builder uses: the
// class's own tree, ranges and caps are rebuilt here, trained to the
// interruption point, and serialized with save_sealed_checkpoint. The resumed
// build then goes through the real builder entry point, so this exercises the
// loader, the identity checks and the resume wiring, not just the codec.
bool test_sealed_resume_at_interruption_points(const fs::path& dir) {
  const auto range = bs::flop_library::declared_library_range();
  const std::size_t class_count = 1;
  const std::uint64_t iterations = 1000;  // chunks at 256, 512, 768, 1000
  const bs::poker::Chips stack = 6;       // RFC 0033: starting stack; 6 - 2 = 4 behind
  const bs::poker::Chips contribution = 2;
  const std::size_t player_count = 2;
  const std::uint64_t seed = 20261020;

  const fs::path reference_dir = dir / "sealed-reference";
  fs::create_directories(reference_dir);
  const bs::flop_library::LibraryManifest reference = bs::flop_library::build_library(
      reference_dir, class_count, iterations, seed, stack, contribution, range, player_count);

  // The class-0 board, derived the way the builder derives it.
  const auto classes = bs::flop_library::enumerate_classes();
  CHECK(!classes.empty());
  const std::array<int, 3> board = classes[0];

  // Rebuild the exact game and tree the builder trains. RFC 0033: `stack` here
  // is the starting stack, so chips behind are stack - contribution; the rows
  // sealed below must project onto THIS tree or warm start rejects them.
  bs::poker::GameDef def;
  def.player_count = static_cast<std::uint8_t>(player_count);
  def.button = 0;
  def.big_blind = 2;
  for (std::size_t s = 0; s < player_count; ++s) {
    def.stacks[s] = stack - contribution;
    def.contributions[s] = contribution;
  }
  def.pot = static_cast<bs::poker::Chips>(player_count * contribution);
  def.board = {board[0], board[1], board[2], 0, 0};
  def.board_size = 3;
  def.terminal = bs::poker::TerminalDepth::Flop;
  const bs::abstraction::ActionAbstraction action = bs::abstraction::ActionAbstraction::identity();
  const bs::tree::AbstractTree tree(def, action);
  const bs::gto::EquityFrontierEvaluator frontier;
  const auto card_id = bs::abstraction::suit_canonicalization_id();

  std::array<bool, 52> on_board{};
  for (int c : board)
    on_board[static_cast<std::size_t>(c)] = true;
  std::vector<bs::solver::WeightedHand> seat_range;
  for (const auto& hand : range)
    if (!on_board[static_cast<std::size_t>(hand.cards[0])] &&
        !on_board[static_cast<std::size_t>(hand.cards[1])])
      seat_range.push_back(hand);
  const std::vector<std::vector<bs::solver::WeightedHand>> ranges{seat_range, seat_range};

  bs::solver::NSeatTrainerLimits limits;
  limits.max_nodes = 10'000'000;
  limits.max_information_sets = 5'000'000;
  limits.wall = std::chrono::minutes{5};

  // Interruption points: the three full chunks inside 1,000 iterations.
  for (std::uint64_t stop : {256ULL, 512ULL, 768ULL}) {
    const fs::path lib_dir = dir / ("sealed-interrupt-" + std::to_string(stop));
    fs::create_directories(lib_dir);

    bs::flop_library::BuildParams params;
    params.class_count = class_count;
    params.iterations_per_class = iterations;
    params.seed = seed;
    params.starting_stack = stack;
    params.contribution = contribution;
    params.range = range;
    params.player_count = player_count;
    bs::flop_library::prepare_build_identity(lib_dir, params, /*resume=*/false);

    const bs::solver::NSeatTrainingResult trained =
        bs::solver::train_nseat(tree, ranges, stop, seed, limits, &frontier);
    CHECK(trained.completed_iterations == stop);

    bs::artifacts::SealedCheckpointState sealed;
    sealed.algorithm_revision_id = trained.algorithm_revision;
    sealed.algorithm_revision = bs::artifacts::sealed_revision_string(trained.algorithm_revision);
    sealed.abstraction_digest = action.id().digest;
    sealed.card_abstraction_digest = card_id.digest;
    sealed.completed_iterations = trained.completed_iterations;
    sealed.batch_size = trained.batch_size;
    sealed.completed_epochs = trained.completed_epochs;
    sealed.node_cap = static_cast<std::uint32_t>(tree.nodes().size());
    sealed.bucket_cap = 169;
    sealed.key_kind = bs::solver::kNSeatCardKind;
    sealed.rows = trained.raw_rows;
    const fs::path checkpoint = lib_dir / "class-0000-checkpoint.bswarm";
    bs::artifacts::save_sealed_checkpoint(checkpoint, sealed);

    // G1: a sealed checkpoint for this class is far below the 1 MB bound and
    // nowhere near the 347.8 MB expanded form.
    CHECK(fs::file_size(checkpoint) < 1024 * 1024);

    // Resume to the full 1,000 iterations through the real builder path.
    const bs::flop_library::LibraryManifest resumed = bs::flop_library::build_library(
        lib_dir, class_count, iterations, seed, stack, contribution, range, player_count,
        bs::abstraction::ActionAbstraction::identity(), /*resume=*/true);
    CHECK(resumed.class_count == class_count);
    CHECK(resumed.library_bin_sha256 == reference.library_bin_sha256);
    CHECK(resumed.classes[0].completed_iterations == iterations);
    CHECK(!fs::exists(checkpoint));  // removed once the class completed
  }

  // Stale-but-complete: a checkpoint already at the target iteration count is
  // dropped and the class retrains, because a crash can land after the final
  // chunk's checkpoint but before the section is written.
  {
    const fs::path lib_dir = dir / "sealed-stale-complete";
    fs::create_directories(lib_dir);
    bs::flop_library::BuildParams params;
    params.class_count = class_count;
    params.iterations_per_class = iterations;
    params.seed = seed;
    params.starting_stack = stack;
    params.contribution = contribution;
    params.range = range;
    params.player_count = player_count;
    bs::flop_library::prepare_build_identity(lib_dir, params, /*resume=*/false);

    const bs::solver::NSeatTrainingResult trained =
        bs::solver::train_nseat(tree, ranges, iterations, seed, limits, &frontier);
    bs::artifacts::SealedCheckpointState sealed;
    sealed.algorithm_revision_id = trained.algorithm_revision;
    sealed.algorithm_revision = bs::artifacts::sealed_revision_string(trained.algorithm_revision);
    sealed.abstraction_digest = action.id().digest;
    sealed.card_abstraction_digest = card_id.digest;
    sealed.completed_iterations = trained.completed_iterations;
    sealed.batch_size = trained.batch_size;
    sealed.completed_epochs = trained.completed_epochs;
    sealed.node_cap = static_cast<std::uint32_t>(tree.nodes().size());
    sealed.bucket_cap = 169;
    sealed.key_kind = bs::solver::kNSeatCardKind;
    sealed.rows = trained.raw_rows;
    const fs::path checkpoint = lib_dir / "class-0000-checkpoint.bswarm";
    bs::artifacts::save_sealed_checkpoint(checkpoint, sealed);

    const bs::flop_library::LibraryManifest resumed = bs::flop_library::build_library(
        lib_dir, class_count, iterations, seed, stack, contribution, range, player_count,
        bs::abstraction::ActionAbstraction::identity(), /*resume=*/true);
    CHECK(resumed.library_bin_sha256 == reference.library_bin_sha256);
    CHECK(resumed.classes[0].completed_iterations == iterations);
  }
  return true;
}

// RFC 0028 AC4: a torn sealed checkpoint is discarded with a warning and its
// class retrains, while a well-formed but incompatible one is a hard refusal.
// These are the two branches of the resume preflight, and confusing them is
// the failure mode that would silently retrain an entire library.
bool test_sealed_preflight_branches(const fs::path& dir) {
  const auto range = bs::flop_library::declared_library_range();
  const std::size_t class_count = 1;
  const std::uint64_t iterations = 16;
  const bs::poker::Chips stack = 6;  // RFC 0033: starting stack; 6 - 2 = 4 behind
  const bs::poker::Chips contribution = 2;
  const std::size_t player_count = 2;
  const std::uint64_t seed = 20261020;

  const fs::path reference_dir = dir / "preflight-reference";
  fs::create_directories(reference_dir);
  const bs::flop_library::LibraryManifest reference = bs::flop_library::build_library(
      reference_dir, class_count, iterations, seed, stack, contribution, range, player_count);

  // A torn file: structurally not a checkpoint. The build must succeed by
  // discarding it and retraining, not by refusing.
  {
    const fs::path lib_dir = dir / "preflight-torn";
    fs::create_directories(lib_dir);
    const fs::path checkpoint = lib_dir / "class-0000-checkpoint.bswarm";
    {
      std::ofstream out(checkpoint, std::ios::binary);
      out << "BSWARM01";  // a truncated header: magic only
    }
    const bs::flop_library::LibraryManifest resumed = bs::flop_library::build_library(
        lib_dir, class_count, iterations, seed, stack, contribution, range, player_count,
        bs::abstraction::ActionAbstraction::identity(), /*resume=*/true);
    CHECK(resumed.library_bin_sha256 == reference.library_bin_sha256);
    CHECK(!fs::exists(checkpoint));
  }

  return true;
}

// RFC 0027 AC5: a build interrupted at each durable boundary must resume to
// the same bytes without retraining a completed class.
//   S1: the section is renamed but the fragment record and checkpoint persist.
//   S2: the fragment record is present.
//   A torn JSONL tail (crash mid-append) is discarded and the class recovers
//   from its section.
bool test_resume_at_durable_boundaries(const fs::path& dir) {
  const auto range = bs::flop_library::declared_library_range();
  const std::size_t class_count = 2;
  const std::uint64_t iterations = 64;
  const bs::poker::Chips stack = 6;  // RFC 0033: starting stack; 6 - 2 = 4 behind
  const bs::poker::Chips contribution = 2;
  const std::size_t player_count = 2;
  const std::uint64_t seed = 20261017;

  const fs::path reference_dir = dir / "boundary-reference";
  fs::create_directories(reference_dir);
  const bs::flop_library::LibraryManifest reference = bs::flop_library::build_library(
      reference_dir, class_count, iterations, seed, stack, contribution, range, player_count);

  // Run one worker over the whole set, then simulate each crash window.
  for (const char* window : {"section-only", "fragment-present", "torn-tail"}) {
    const fs::path lib_dir = dir / (std::string("boundary-") + window);
    fs::create_directories(lib_dir);
    bs::flop_library::BuildParams params;
    params.class_count = class_count;
    params.iterations_per_class = iterations;
    params.seed = seed;
    params.starting_stack = stack;
    params.contribution = contribution;
    params.range = range;
    params.player_count = player_count;

    // First pass: one worker completes the classes.
    bs::flop_library::prepare_build_identity(lib_dir, params, params.resume);
    bs::flop_library::build_library_worker(lib_dir, 0, 1, params);
    const fs::path sections = lib_dir / ".sections";

    if (std::string_view(window) == "section-only") {
      // Drop the fragment records: the section alone must be enough.
      for (std::size_t w = 0;; ++w) {
        const fs::path frag = sections / ("manifest-part-" + std::to_string(w) + ".jsonl");
        if (!fs::exists(frag))
          break;
        fs::remove(frag);
      }
    } else if (std::string_view(window) == "torn-tail") {
      // Append a half-written line, as a crash during append leaves.
      std::ofstream out(sections / "manifest-part-0.jsonl", std::ios::app);
      out << "{\"index\": 99, \"canonical_bo";
    }

    params.resume = true;
    const bs::flop_library::LibraryManifest resumed = bs::flop_library::build_library(
        lib_dir, class_count, iterations, seed, stack, contribution, range, player_count,
        bs::abstraction::ActionAbstraction::identity(), true);
    CHECK(resumed.library_bin_sha256 == reference.library_bin_sha256);
    CHECK(resumed.class_count == class_count);
  }
  return true;
}

// RFC 0027: resuming with a different codebook layout (or any identity field)
// must fail closed before any worker starts, because a section does not carry
// the layout kind.
bool test_resume_rejects_layout_change(const fs::path& dir) {
  const auto range = bs::flop_library::declared_library_range();
  const fs::path lib_dir = dir / "resume-layout-change";
  fs::create_directories(lib_dir);
  bs::flop_library::BuildParams params;
  params.class_count = 1;
  params.iterations_per_class = 16;
  params.seed = 20261017;
  params.starting_stack = 6;  // RFC 0033: 6 - 2 = 4 behind, preserving SPR 1
  params.contribution = 2;
  params.range = range;
  params.player_count = 2;
  bs::flop_library::prepare_build_identity(lib_dir, params, params.resume);
  bs::flop_library::build_library_worker(lib_dir, 0, 1, params);

  bool threw = false;
  try {
    (void)bs::flop_library::build_library(lib_dir, 1, 16, 20261017, 6, 2, range, 2,
                                          bs::abstraction::ActionAbstraction::identity(), true,
                                          bs::resident::ProbabilityLayout::Doubles);
  } catch (const std::runtime_error& error) {
    threw = std::string(error.what()).find("codebook") != std::string::npos;
  }
  CHECK(threw);
  return true;
}

}  // namespace

int main() {
  const fs::path dir = fs::temp_directory_path() / "bs-flop-library-parallel-test";
  std::error_code ec;
  fs::remove_all(dir, ec);
  fs::create_directories(dir, ec);

  bool ok = true;
  ok = test_parallel_matches_serial(dir) && ok;
  ok = test_resume_matches_fresh(dir) && ok;
  ok = test_resume_skips_published_classes(dir) && ok;
  ok = test_resume_at_durable_boundaries(dir) && ok;
  ok = test_sealed_resume_at_interruption_points(dir) && ok;
  ok = test_sealed_preflight_branches(dir) && ok;
  ok = test_resume_rejects_layout_change(dir) && ok;
  ok = test_merge_reuses_prior_manifest(dir) && ok;

  fs::remove_all(dir, ec);
  if (!ok) {
    std::printf("flop_library_parallel: tests FAILED\n");
    return 1;
  }
  std::printf("flop_library_parallel: all tests passed\n");
  return 0;
}
