// RFC 0026 AC2 verifier: exhaustive SQLite -> BSFLIB03 equivalence.
//
// For every class of a packed library this reloads the canonical schema-v3
// SQLite artifact and checks, for EVERY stored concrete row, that the
// bucket-keyed mmap lookup returns a bitwise-identical action menu and
// probability vector; and that every off-board holding whose bucket was never
// trained is a miss. It is a correctness gate, not a shipped runtime path.
//
// Usage: bigshark-flop-library-verify-pack <sqlite-dir> <packed.bin>
#include <algorithm>
#include <array>
#include <bs/abstraction.hpp>
#include <bs/mmap_library.hpp>
#include <bs/nseat_trainer.hpp>
#include <bs/seat_policy.hpp>
#include <bs/strategy_artifact.hpp>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <map>
#include <set>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "resident/resident_index.hpp"

namespace fs = std::filesystem;
using namespace bs;
using bs::resident::CompactRowView;
using bs::resident::MmapLibrary;
using bs::resident::ProbabilityLayout;
using bs::resident::ResidentIndex;

namespace {

double defer_prob(const CompactRowView& row, std::size_t i) {
  switch (row.prob_layout) {
    case ProbabilityLayout::Codebook8:
      return row.codebook[row.probability_indices[i]];
    case ProbabilityLayout::Codebook16:
      return row.codebook[row.probability_indices_16[i]];
    case ProbabilityLayout::Doubles:
      return row.probabilities[i];
  }
  return 0.0;
}

// Derive the public compact key ([seat][board_size][board][5-byte actions])
// from a concrete information key word vector.
std::vector<std::uint8_t> public_key(const std::vector<std::uint64_t>& words) {
  std::vector<std::uint8_t> out;
  const std::size_t board_size = static_cast<std::size_t>(words[3]);
  out.push_back(static_cast<std::uint8_t>(words[0]));
  out.push_back(static_cast<std::uint8_t>(board_size));
  for (std::size_t i = 0; i < board_size; ++i)
    out.push_back(static_cast<std::uint8_t>(words[4 + i]));
  for (std::size_t w = 4 + board_size; w + 3 < words.size(); w += 4) {
    out.push_back(static_cast<std::uint8_t>(words[w]));
    out.push_back(static_cast<std::uint8_t>(words[w + 1]));
    out.push_back(static_cast<std::uint8_t>(words[w + 2]));
    out.push_back(static_cast<std::uint8_t>(words[w + 3] & 0xffu));
    out.push_back(static_cast<std::uint8_t>((words[w + 3] >> 8) & 0xffu));
  }
  return out;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 3) {
    std::fprintf(stderr, "usage: %s <sqlite-dir> <packed.bin>\n", argv[0]);
    return 2;
  }
  const fs::path dir = argv[1];
  const fs::path bin = argv[2];

  MmapLibrary lib;
  try {
    lib.open(bin, /*verify_sha256=*/false);
  } catch (const std::exception& e) {
    std::fprintf(stderr, "open failed: %s\n", e.what());
    return 1;
  }

  std::uint64_t rows_checked = 0;
  std::uint64_t holdings_checked = 0;
  std::uint64_t classes_checked = 0;
  std::uint64_t mismatches = 0;

  // RFC 0035: iterate every (root, class) pair. Legacy single-root libraries
  // have exactly one root.
  for (std::uint32_t root_id = 0; root_id < lib.root_count(); ++root_id)
    for (std::uint32_t class_id : lib.root_class_ids(root_id)) {
      char stem[32];
      std::snprintf(stem, sizeof(stem), "class-%04u.db", class_id);
      const fs::path artifact = dir / stem;
      artifacts::LoadedArtifact loaded;
      try {
        loaded = artifacts::load_artifact(artifact);
      } catch (const std::exception& e) {
        std::fprintf(stderr, "load failed %s: %s\n", artifact.c_str(), e.what());
        return 1;
      }
      if (!loaded.bundle.nseat) {
        std::fprintf(stderr, "%s is not an nseat artifact\n", artifact.c_str());
        return 1;
      }
      const auto& policy = loaded.bundle.nseat->policy;
      const auto& board = lib.canonical_board(root_id, class_id);

      for (const auto& [key, row] : policy.rows()) {
        if (key.size() < 4) {
          std::fprintf(stderr, "short key in %s\n", stem);
          return 1;
        }
        const std::array<int, 2> hole{static_cast<int>(key[1]), static_cast<int>(key[2])};
        const std::size_t board_size = static_cast<std::size_t>(key[3]);
        std::array<int, 5> b{};
        for (std::size_t i = 0; i < board_size && i < b.size(); ++i)
          b[i] = static_cast<int>(key[4 + i]);
        const std::uint16_t bucket = static_cast<std::uint16_t>(abstraction::card_bucket(
            solver::kNSeatCardKind, hole, std::span<const int>(b.data(), board_size)));
        const auto pub = public_key(key);
        CompactRowView view;
        if (!lib.find_bucket(root_id, class_id,
                             std::span<const std::uint8_t>(pub.data(), pub.size()), bucket, view)) {
          std::fprintf(stderr, "missing bucket row for stored row root %u %s bucket %u\n", root_id,
                       stem, bucket);
          return 1;
        }
        if (view.count != row.actions.size()) {
          std::fprintf(stderr, "menu count mismatch %s\n", stem);
          ++mismatches;
          continue;
        }
        for (std::size_t i = 0; i < row.actions.size(); ++i) {
          if (static_cast<std::uint8_t>(view.actions[i].type) !=
                  static_cast<std::uint8_t>(row.actions[i].type) ||
              view.actions[i].target != row.actions[i].target_total ||
              defer_prob(view, i) != row.probabilities[i]) {
            std::fprintf(stderr, "row mismatch root %u %s bucket %u action %zu\n", root_id, stem,
                         bucket, i);
            ++mismatches;
            break;
          }
        }
        ++rows_checked;
      }

      // Miss check: every off-board holding whose bucket was never trained.
      std::array<bool, 52> on_board{};
      for (int card : board)
        on_board[card] = true;
      // Build the set of trained buckets present by probing the root and
      // history nodes is not possible without keys; instead, infer trained
      // buckets from the artifact rows themselves.
      std::set<std::pair<std::vector<std::uint8_t>, std::uint16_t>> trained;
      for (const auto& [key, row] : policy.rows()) {
        const std::array<int, 2> hole{static_cast<int>(key[1]), static_cast<int>(key[2])};
        const std::size_t board_size = static_cast<std::size_t>(key[3]);
        std::array<int, 5> b{};
        for (std::size_t i = 0; i < board_size && i < b.size(); ++i)
          b[i] = static_cast<int>(key[4 + i]);
        const std::uint16_t bucket = static_cast<std::uint16_t>(abstraction::card_bucket(
            solver::kNSeatCardKind, hole, std::span<const int>(b.data(), board_size)));
        trained.insert({public_key(key), bucket});
      }
      for (int c0 = 0; c0 < 52; ++c0) {
        if (on_board[c0])
          continue;
        for (int c1 = c0 + 1; c1 < 52; ++c1) {
          if (on_board[c1])
            continue;
          // Only the nodes present in the artifact are checked; use the root
          // node public key per acting seat present in the row set.
          for (std::uint8_t seat = 0; seat < policy.game().player_count; ++seat) {
            std::vector<std::uint64_t> root_key;
            root_key.push_back(seat);
            root_key.push_back(c0);
            root_key.push_back(c1);
            root_key.push_back(3);
            root_key.push_back(board[0]);
            root_key.push_back(board[1]);
            root_key.push_back(board[2]);
            const std::array<int, 2> hole{c0, c1};
            const std::uint16_t bucket = static_cast<std::uint16_t>(abstraction::card_bucket(
                solver::kNSeatCardKind, hole, std::span<const int>(board.data(), 3)));
            const auto pub = public_key(root_key);
            CompactRowView view;
            const bool hit = lib.find_bucket(root_id, class_id,
                                             std::span<const std::uint8_t>(pub.data(), pub.size()),
                                             bucket, view);
            const bool expected = trained.count({pub, bucket}) > 0;
            if (hit != expected) {
              std::fprintf(stderr,
                           "miss mismatch root %u %s seat %u holding %d,%d bucket %u hit %d\n",
                           root_id, stem, seat, c0, c1, bucket, hit);
              return 1;
            }
            ++holdings_checked;
          }
        }
      }
      ++classes_checked;
      if (classes_checked % 100 == 0)
        std::fprintf(stderr, "  verified %llu classes, %llu rows\n",
                     static_cast<unsigned long long>(classes_checked),
                     static_cast<unsigned long long>(rows_checked));
    }

  std::printf(
      "verify_pack: %llu classes, %llu stored rows, %llu holding/node checks, %llu "
      "mismatches\n",
      static_cast<unsigned long long>(classes_checked),
      static_cast<unsigned long long>(rows_checked),
      static_cast<unsigned long long>(holdings_checked),
      static_cast<unsigned long long>(mismatches));
  return mismatches == 0 ? 0 : 1;
}
