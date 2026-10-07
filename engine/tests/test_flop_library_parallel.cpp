// RFC 0012 Phase 1: flop library parallel training tests.
//
// Verifies that training the same class set with one serial builder and with
// N strided workers (merged by merge_fragments) produces bitwise-identical
// per-class artifacts. The worker/merge path is the same code the forked
// builder processes run; this test exercises it in-process.
#include <bs/flop_library.hpp>
#include <bs/strategy_artifact.hpp>
#include <cstdio>
#include <filesystem>
#include <string>
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
  const bs::poker::Chips stack = 4;
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
  params.stack = stack;
  params.contribution = contribution;
  params.range = range;
  params.player_count = player_count;
  const std::size_t parallel = 4;
  for (std::size_t w = 0; w < parallel; ++w)
    bs::flop_library::build_library_worker(parallel_dir, w, parallel, params);
  const bs::flop_library::LibraryManifest parallel_manifest =
      bs::flop_library::merge_fragments(parallel_dir, params);

  CHECK(serial_manifest.class_count == class_count);
  CHECK(parallel_manifest.class_count == class_count);
  CHECK(serial_manifest.covered_flops == parallel_manifest.covered_flops);
  CHECK(serial_manifest.storage_bytes == parallel_manifest.storage_bytes);

  // Per-class artifacts must be bitwise-identical (same SHA-256).
  for (std::size_t i = 0; i < class_count; ++i) {
    char stem[16];
    std::snprintf(stem, sizeof(stem), "class-%04zu.db", i);
    const fs::path serial_artifact = serial_dir / stem;
    const fs::path parallel_artifact = parallel_dir / stem;
    CHECK(fs::exists(serial_artifact));
    CHECK(fs::exists(parallel_artifact));
    const std::string serial_sha = bs::artifacts::sha256_file_hex(serial_artifact);
    const std::string parallel_sha = bs::artifacts::sha256_file_hex(parallel_artifact);
    CHECK(serial_sha == parallel_sha);
    // Manifest entries must agree.
    CHECK(serial_manifest.classes[i].sha256_hex == parallel_manifest.classes[i].sha256_hex);
    CHECK(serial_manifest.classes[i].completed_iterations ==
          parallel_manifest.classes[i].completed_iterations);
    CHECK(serial_manifest.classes[i].stored_rows == parallel_manifest.classes[i].stored_rows);
    CHECK(serial_manifest.classes[i].canonical_board ==
          parallel_manifest.classes[i].canonical_board);
  }

  // No checkpoint or fragment files remain after the merge.
  for (std::size_t i = 0; i < class_count; ++i) {
    char stem[16];
    std::snprintf(stem, sizeof(stem), "class-%04zu", i);
    CHECK(!fs::exists(parallel_dir / (std::string(stem) + "-checkpoint.db")));
  }
  for (std::size_t w = 0; w < parallel; ++w) {
    const fs::path frag = parallel_dir / ("manifest-part-" + std::to_string(w) + ".json");
    CHECK(!fs::exists(frag));
  }
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
  const bs::poker::Chips stack = 4;
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
  for (std::size_t i = 0; i < class_count; ++i) {
    char stem[16];
    std::snprintf(stem, sizeof(stem), "class-%04zu.db", i);
    const std::string fresh_sha = bs::artifacts::sha256_file_hex(fresh_dir / stem);
    const std::string resume_sha = bs::artifacts::sha256_file_hex(resume_dir / stem);
    CHECK(fresh_sha == resume_sha);
    CHECK(resume_manifest.classes[i].completed_iterations == iterations);
  }
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

  fs::remove_all(dir, ec);
  if (!ok) {
    std::printf("flop_library_parallel: tests FAILED\n");
    return 1;
  }
  std::printf("flop_library_parallel: all tests passed\n");
  return 0;
}
