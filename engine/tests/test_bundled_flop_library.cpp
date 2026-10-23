// RFC 0033: the published-artifact test. Loads the bundled six-way library and
// requires that a REAL hand's flop root reaches it. This is the test whose
// absence let the defect ship: hero_decision's only callers built synthetic
// GameDefs, so no test ever asked whether the shipped artifact answers a state
// the table actually deals.
//
// The artifact is gitignored (.gitignore: /artifacts/) and CI does not fetch it,
// so this test skips with an explicit message when the file is absent. It
// carries the `benchmark` label, which is the only exclusion mechanism CI has
// (tools/ci/verify-linux.sh runs `ctest -LE benchmark` for the default pass),
// so it is available on demand with `-L benchmark` and never a CI gate. The
// convention gate that DOES run in CI is test_flop_library, which builds its
// own library.
#include <array>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#include "bs/game_definition.hpp"
#include "bs/mmap_library.hpp"

namespace fs = std::filesystem;

namespace {

int failures = 0;

#define CHECK(cond)                                                        \
  do {                                                                     \
    if (!(cond)) {                                                         \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      ++failures;                                                          \
    }                                                                      \
  } while (0)

// The exact comparison resolve_root makes (resident_policy.cpp same_root).
bool same_root(const bs::poker::GameDef& a, const bs::poker::GameDef& b) {
  if (a.player_count != b.player_count || a.pot != b.pot || a.big_blind != b.big_blind ||
      a.button != b.button)
    return false;
  if (a.board_size != b.board_size)
    return false;
  for (unsigned i = 0; i < a.board_size; ++i)
    if (a.board[i] != b.board[i])
      return false;
  for (unsigned p = 0; p < a.player_count; ++p)
    if (a.stacks[p] != b.stacks[p] || a.contributions[p] != b.contributions[p] ||
        a.folded[p] != b.folded[p])
      return false;
  return true;
}

// A six-handed 100 BB table: every seat buys in for `buyin`, posts `contribution`
// preflop, and nobody raises, so the flop root has `buyin - contribution` behind
// and `contribution` in per seat.
bs::poker::GameDef limped_flop(const std::array<int, 3>& board, bs::poker::Chips buyin,
                               bs::poker::Chips contribution, std::size_t seats) {
  bs::poker::GameDef d{};
  d.player_count = static_cast<std::uint8_t>(seats);
  d.button = 0;
  d.big_blind = 2;
  d.preflop = false;
  d.terminal = bs::poker::TerminalDepth::Flop;
  d.board_size = 3;
  d.board = {board[0], board[1], board[2], 0, 0};
  for (std::size_t s = 0; s < seats; ++s) {
    d.stacks[s] = buyin - contribution;
    d.contributions[s] = contribution;
  }
  d.pot = static_cast<bs::poker::Chips>(seats * contribution);
  return d;
}

}  // namespace

int main(int argc, char** argv) {
  const fs::path dir =
      argc > 1 ? fs::path(argv[1]) : fs::path("artifacts/flop-library-6way-fullrange");
  const fs::path bin = dir / "library.bin";
  std::error_code ec;
  if (!fs::exists(bin, ec)) {
    std::printf(
        "bundled_flop_library: SKIPPED (no artifact at %s). This test requires the published "
        "library; it is gitignored and CI does not fetch it.\n",
        bin.string().c_str());
    return 0;
  }

  bs::resident::MmapLibrary lib;
  lib.open(bin, /*verify_sha256=*/false);
  bs::poker::GameDef record = lib.game().def;
  const auto& board = lib.canonical_board(lib.class_id_at(0));
  record.board = {board[0], board[1], board[2], 0, 0};
  record.board_size = 3;

  // Chip conservation: the defect recorded the buy-in in `stacks` while also
  // recording the contribution, giving 1,212 chips in a 1,200-chip game.
  {
    bs::poker::Chips total = record.pot;
    for (std::size_t s = 0; s < record.player_count; ++s)
      total += record.stacks[s];
    CHECK(total == record.player_count * (record.stacks[0] + record.contributions[0]));
  }

  // The load-bearing assertion: a real hand must reach it.
  {
    const bs::poker::GameDef real =
        limped_flop(board, /*buyin=*/200, /*contribution=*/2, record.player_count);
    CHECK(same_root(record, real));
  }

  // A one-chip change must NOT reach it, which pins the exactness contract
  // rather than only the happy path.
  {
    const bs::poker::GameDef off =
        limped_flop(board, /*buyin=*/201, /*contribution=*/2, record.player_count);
    CHECK(!same_root(record, off));
  }

  // A raised pot must not reach it either: one class stores one root SHAPE, so
  // equal contributions are part of the contract.
  {
    bs::poker::GameDef raised =
        limped_flop(board, /*buyin=*/200, /*contribution=*/6, record.player_count);
    raised.pot = static_cast<bs::poker::Chips>(record.player_count * 6);
    CHECK(!same_root(record, raised));
  }

  if (failures != 0) {
    std::fprintf(stderr, "bundled_flop_library: %d FAILED\n", failures);
    return 1;
  }
  std::printf("bundled_flop_library: all tests passed (root stacks=%llu contributions=%llu)\n",
              static_cast<unsigned long long>(record.stacks[0]),
              static_cast<unsigned long long>(record.contributions[0]));
  return 0;
}
