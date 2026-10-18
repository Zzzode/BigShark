// RFC 0026 AC4: cold-open and bucket-lookup microbenchmark for a BSFLIB02
// packed flop library. Reports mmap open (header parse, no full-file hash),
// class-section parse, and a point-lookup throughput for synthetic queries.
// Manual target, NOT a CTest (operates on a large on-disk library).
//
// Usage: bigshark-mmap-cold-open-bench <packed.bin> [lookups]
#include <bs/abstraction.hpp>
#include <bs/mmap_library.hpp>
#include <bs/nseat_trainer.hpp>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <vector>

#include "resident/resident_index.hpp"

using namespace bs::resident;
using clk = std::chrono::steady_clock;

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: %s <packed.bin> [lookups]\n", argv[0]);
    return 2;
  }
  const std::size_t lookups = argc >= 3 ? static_cast<std::size_t>(std::atoll(argv[2])) : 100000;

  for (int run = 0; run < 3; ++run) {
    MmapLibrary lib;
    const auto t0 = clk::now();
    lib.open(argv[1], /*verify_sha256=*/false);
    const auto t1 = clk::now();
    const double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    std::printf("open run %d: %.2f ms for %zu classes\n", run, ms, lib.class_count());
  }

  // Point-lookup throughput on the first class: repeatedly resolve a present
  // (root node, bucket) pair. Derives the root public key bytes for seat 0.
  MmapLibrary lib;
  lib.open(argv[1], false);
  const std::uint32_t class_id = lib.class_id_at(0);
  const auto& board = lib.canonical_board(class_id);
  std::printf("first class_id=%u board=%d,%d,%d nodes=%zu bucket_rows=%zu\n", class_id, board[0],
              board[1], board[2], lib.node_count(class_id), lib.bucket_row_count(class_id));

  int hits = 0;
  std::uint16_t hit_bucket = 0;
  for (std::uint8_t seat = 0; seat < 2; ++seat) {
    std::vector<std::uint8_t> pub;
    pub.push_back(seat);
    pub.push_back(3);
    pub.push_back(static_cast<std::uint8_t>(board[0]));
    pub.push_back(static_cast<std::uint8_t>(board[1]));
    pub.push_back(static_cast<std::uint8_t>(board[2]));
    for (std::uint16_t b = 1; b <= 9; ++b) {
      CompactRowView v;
      if (lib.find_bucket(class_id, pub, b, v)) {
        std::printf("root hit seat=%u bucket=%u count=%u\n", seat, b, v.count);
        ++hits;
        if (hit_bucket == 0)
          hit_bucket = b;
      }
    }
  }
  if (hits == 0) {
    std::fprintf(stderr, "no root bucket present in first class\n");
    return 1;
  }
  // Rebuild the public key for the seat that produced the first hit.
  std::uint8_t hit_seat = 0;
  for (std::uint8_t seat = 0; seat < 2; ++seat) {
    std::vector<std::uint8_t> probe;
    probe.push_back(seat);
    probe.push_back(3);
    probe.push_back(static_cast<std::uint8_t>(board[0]));
    probe.push_back(static_cast<std::uint8_t>(board[1]));
    probe.push_back(static_cast<std::uint8_t>(board[2]));
    CompactRowView v;
    if (lib.find_bucket(class_id, probe, hit_bucket, v)) {
      hit_seat = seat;
      break;
    }
  }
  std::vector<std::uint8_t> pub;
  pub.push_back(hit_seat);
  pub.push_back(3);
  pub.push_back(static_cast<std::uint8_t>(board[0]));
  pub.push_back(static_cast<std::uint8_t>(board[1]));
  pub.push_back(static_cast<std::uint8_t>(board[2]));
  volatile std::size_t sink = 0;
  const auto q0 = clk::now();
  for (std::size_t i = 0; i < lookups; ++i) {
    CompactRowView v;
    if (lib.find_bucket(class_id, pub, hit_bucket, v))
      sink += v.count;
  }
  const auto q1 = clk::now();
  const double ns = std::chrono::duration<double, std::nano>(q1 - q0).count() / lookups;
  std::printf("point lookup: %.1f ns/op over %zu ops (sink %zu)\n", ns, lookups,
              static_cast<std::size_t>(sink));

  // Per-combo bucket evaluation cost (the only new work per opponent combo in
  // the public-belief replay loop): evaluate card_bucket for every off-board
  // holding (1,128 combos on a 3-card board), repeated.
  {
    const std::array<int, 3> b3{board[0], board[1], board[2]};
    std::array<bool, 52> on_board{};
    for (int c : b3)
      on_board[c] = true;
    std::array<int, 2> hole{};
    volatile std::uint32_t sink32 = 0;
    constexpr int kRepeat = 2000;
    const auto bq0 = clk::now();
    for (int rep = 0; rep < kRepeat; ++rep)
      for (int c0 = 0; c0 < 52; ++c0) {
        if (on_board[c0])
          continue;
        for (int c1 = c0 + 1; c1 < 52; ++c1) {
          if (on_board[c1])
            continue;
          hole = {c0, c1};
          sink32 += bs::abstraction::card_bucket(bs::solver::kNSeatCardKind, hole, b3);
        }
      }
    const auto bq1 = clk::now();
    const double ns_b =
        std::chrono::duration<double, std::nano>(bq1 - bq0).count() / (kRepeat * 1128.0);
    std::printf("card bucket: %.1f ns/combo (1128 off-board combos x%d, sink %u)\n", ns_b, kRepeat,
                sink32);
  }
  return 0;
}
