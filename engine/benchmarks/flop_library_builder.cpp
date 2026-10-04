// flop_library_builder.cpp — RFC 0009 W4c-iii: offline flop class library builder.
//
// Builds the first trained class library: per-class flop-rooted schema-v3
// artifacts for the first N canonical classes (by board-id order), plus a
// manifest recording the honest coverage and storage measurements. Offline
// only: no service, host, protocol, network, or credentials.
//
// Usage: bigshark-flop-library-builder <output-dir> [class-count] [iterations] [player-count]
#include <bs/flop_library.hpp>
#include <bs/heads_up.hpp>  // poker::Chips
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

int main(int argc, char** argv) {
  if (argc < 2 || argc > 5) {
    std::fprintf(stderr,
                 "usage: %s <output-dir> [class-count] [iterations] [player-count]\n"
                 "  class-count   number of canonical classes to train (default 8)\n"
                 "  iterations    MCCFR iterations per class      (default 1000)\n"
                 "  player-count  seats in the game, 2..10         (default 2)\n",
                 argv[0]);
    return 2;
  }
  const std::filesystem::path out_dir = argv[1];
  const std::size_t class_count = argc > 2 ? std::stoull(argv[2]) : 8;
  const std::uint64_t iterations = argc > 3 ? std::strtoull(argv[3], nullptr, 10) : 1000;
  const std::size_t player_count = argc > 4 ? std::stoull(argv[4]) : 2;
  const std::uint64_t seed = 20261002;
  const bs::poker::Chips stack = 4;
  const bs::poker::Chips contribution = 2;

  const auto range = bs::flop_library::declared_library_range();
  const bs::flop_library::LibraryManifest manifest = bs::flop_library::build_library(
      out_dir, class_count, iterations, seed, stack, contribution, range, player_count);

  const double coverage_pct = 100.0 * static_cast<double>(manifest.covered_flops) /
                              static_cast<double>(manifest.total_flops);
  std::printf("flop library: %zu classes, %zu/%zu flops covered (%.4f%%), %llu bytes stored, %zu players\n",
              manifest.class_count, manifest.covered_flops, manifest.total_flops, coverage_pct,
              static_cast<unsigned long long>(manifest.storage_bytes), manifest.player_count);
  for (const auto& c : manifest.classes) {
    std::printf("  class board {%d, %d, %d}: %s, %llu rows, %llu iterations\n",
                c.canonical_board[0], c.canonical_board[1], c.canonical_board[2],
                c.artifact_name.c_str(), static_cast<unsigned long long>(c.stored_rows),
                static_cast<unsigned long long>(c.completed_iterations));
  }
  return 0;
}
