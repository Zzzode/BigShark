// RFC 0012 Phase 2: consolidated binary format round-trip tests.
//
// Packs a small set of synthetic class policies into a .bin file, mmaps it,
// and verifies that every row, game identity field, range, and count is
// bitwise-identical to the input. Also exercises SHA-256 sidecar verification
// and miss handling.
#include <array>
#include <bs/abstraction.hpp>
#include <bs/game_definition.hpp>
#include <bs/heads_up_solver.hpp>
#include <bs/mmap_library.hpp>
#include <bs/seat_policy.hpp>
#include <bs/unified_game.hpp>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <span>
#include <string>
#include <vector>

#include "resident/resident_index.hpp"

namespace fs = std::filesystem;
using namespace bs;
using bs::resident::CompactRowView;
using bs::resident::ProbabilityLayout;
using bs::resident::ResidentIndex;

namespace {

int g_failures = 0;
#define CHECK(cond)                                                        \
  do {                                                                     \
    if (!(cond)) {                                                         \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      ++g_failures;                                                        \
    }                                                                      \
  } while (0)

// Build a synthetic information key for a root node (no history actions).
solver::InformationKey make_key(std::size_t actor, std::array<int, 2> own,
                                std::span<const int> board) {
  solver::InformationKey key;
  key.push_back(actor);
  key.push_back(static_cast<std::uint64_t>(own[0]));
  key.push_back(static_cast<std::uint64_t>(own[1]));
  key.push_back(static_cast<std::uint64_t>(board.size()));
  for (int card : board)
    key.push_back(static_cast<std::uint64_t>(card));
  return key;
}

// Build a small set of synthetic rows for one class.
std::map<solver::InformationKey, solver::SeatPolicyRow> make_rows(std::span<const int> board) {
  std::map<solver::InformationKey, solver::SeatPolicyRow> rows;
  const poker::Action check{poker::ActionType::Check, 0};
  const poker::Action bet{poker::ActionType::Bet, 100};
  // Three rows with distinct probability pairs so the codebook has 6 values.
  for (int i = 0; i < 3; ++i) {
    const std::array<int, 2> own{i * 2, i * 2 + 1};
    const double p = 0.1 + 0.1 * i;
    rows[make_key(0, own, board)] = solver::SeatPolicyRow{
        .actions = {check, bet}, .probabilities = {p, 1.0 - p}, .visits = 100};
  }
  return rows;
}

// Build a small declared range.
std::vector<solver::WeightedHand> make_range() {
  return {
      {{0, 1}, 1.0},
      {{2, 3}, 1.0},
      {{4, 5}, 0.5},
  };
}

// Build the shared game identity.
solver::UnifiedGame make_game() {
  solver::UnifiedGame game;
  game.def.player_count = 2;
  game.def.button = 0;
  game.def.big_blind = 2;
  game.def.stacks[0] = 200;
  game.def.stacks[1] = 200;
  game.def.contributions[0] = 1;
  game.def.contributions[1] = 2;
  game.def.pot = 3;
  game.def.board_size = 0;  // set per-class
  game.def.terminal = poker::TerminalDepth::Flop;
  game.sizes = abstraction::default_size_schedule();
  return game;
}

// Compact a key for MmapLibrary::find.
std::size_t compact_key(const solver::InformationKey& key, std::array<std::uint8_t, 512>& buf) {
  return ResidentIndex::compact_information_key(
      std::span<const std::uint64_t>(key.data(), key.size()),
      std::span<std::uint8_t>(buf.data(), buf.size()));
}

// Defer a probability from a CompactRowView.
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

bool test_pack_and_mmap(const fs::path& dir) {
  const fs::path bin_path = dir / "test_library.bin";

  const std::array<int, 3> board0{0, 1, 2};
  const std::array<int, 3> board1{4, 5, 6};

  auto rows0 = make_rows(board0);
  auto rows1 = make_rows(board1);
  auto range = make_range();
  solver::UnifiedGame game = make_game();

  std::array<resident::PackClassInput, 2> inputs{};
  inputs[0].class_id = 0;
  inputs[0].canonical_board = board0;
  inputs[0].rows = &rows0;
  inputs[0].range = &range;
  inputs[1].class_id = 1;
  inputs[1].canonical_board = board1;
  inputs[1].rows = &rows1;
  inputs[1].range = &range;

  // Pack.
  try {
    resident::pack_binary_library(bin_path, game, inputs);
  } catch (const std::exception& e) {
    std::fprintf(stderr, "pack threw: %s\n", e.what());
    return false;
  }
  CHECK(fs::exists(bin_path));
  CHECK(fs::exists(bin_path.string() + ".sha256"));

  // Open and verify.
  resident::MmapLibrary lib;
  try {
    lib.open(bin_path);
  } catch (const std::exception& e) {
    std::fprintf(stderr, "open threw: %s\n", e.what());
    return false;
  }

  CHECK(lib.class_count() == 2);
  CHECK(lib.class_id_at(0) == 0);
  CHECK(lib.class_id_at(1) == 1);
  CHECK(lib.canonical_board(0) == board0);
  CHECK(lib.canonical_board(1) == board1);
  CHECK(lib.info_set_count(0) == 3);
  CHECK(lib.info_set_count(1) == 3);
  CHECK(lib.action_count(0) == 6);  // 3 rows x 2 actions
  CHECK(lib.action_count(1) == 6);
  CHECK(!lib.sha256_hex().empty());

  // Game identity.
  const solver::UnifiedGame& lib_game = lib.game();
  CHECK(lib_game.def.player_count == 2);
  CHECK(lib_game.def.big_blind == 2);
  CHECK(lib_game.def.stacks[0] == 200);
  CHECK(lib_game.def.stacks[1] == 200);
  CHECK(lib_game.def.contributions[0] == 1);
  CHECK(lib_game.def.contributions[1] == 2);
  CHECK(lib_game.def.pot == 3);
  CHECK(lib_game.def.terminal == poker::TerminalDepth::Flop);

  // Ranges.
  const auto& range0 = lib.class_range(0);
  CHECK(range0.size() == range.size());
  for (std::size_t i = 0; i < range.size(); ++i) {
    CHECK(range0[i].cards == range[i].cards);
    CHECK(range0[i].weight == range[i].weight);
  }

  // Board lookup.
  CHECK(lib.class_id_for_board(board0) == 0);
  CHECK(lib.class_id_for_board(board1) == 1);
  CHECK(lib.class_id_for_board({8, 9, 10}) == -1);

  // Row round-trip: verify every row in class 0.
  for (const auto& [key, expected] : rows0) {
    std::array<std::uint8_t, 512> compact_buf{};
    const std::size_t csz = compact_key(key, compact_buf);
    CHECK(csz > 0);
    CompactRowView row_view;
    CHECK(lib.find(0, std::span<const std::uint8_t>(compact_buf.data(), csz), row_view));
    CHECK(row_view.count == expected.actions.size());
    for (std::size_t i = 0; i < expected.actions.size(); ++i) {
      CHECK(static_cast<std::uint8_t>(row_view.actions[i].type) ==
            static_cast<std::uint8_t>(expected.actions[i].type));
      CHECK(row_view.actions[i].target == expected.actions[i].target_total);
      CHECK(defer_prob(row_view, i) == expected.probabilities[i]);
    }
  }

  // Miss: unknown class.
  {
    std::array<std::uint8_t, 512> compact_buf{};
    const auto key = make_key(0, {0, 1}, board0);
    const std::size_t csz = compact_key(key, compact_buf);
    CompactRowView row_view;
    CHECK(!lib.find(99, std::span<const std::uint8_t>(compact_buf.data(), csz), row_view));
  }

  // Miss: unknown key in known class.
  {
    const auto key = make_key(0, {50, 51}, board0);  // cards not in rows
    std::array<std::uint8_t, 512> compact_buf{};
    const std::size_t csz = compact_key(key, compact_buf);
    CompactRowView row_view;
    CHECK(!lib.find(0, std::span<const std::uint8_t>(compact_buf.data(), csz), row_view));
  }

  return g_failures == 0;
}

bool test_sha256_verification(const fs::path& dir) {
  const fs::path bin_path = dir / "test_sha.bin";
  const std::array<int, 3> board{0, 1, 2};
  auto rows = make_rows(board);
  auto range = make_range();
  solver::UnifiedGame game = make_game();

  resident::PackClassInput input;
  input.class_id = 0;
  input.canonical_board = board;
  input.rows = &rows;
  input.range = &range;

  try {
    resident::pack_binary_library(bin_path, game, std::span(&input, 1));
  } catch (const std::exception& e) {
    std::fprintf(stderr, "pack threw: %s\n", e.what());
    return false;
  }

  // Corrupt the sidecar.
  const fs::path sha_path = bin_path.string() + ".sha256";
  std::string sha_content;
  {
    std::ifstream f(sha_path, std::ios::binary);
    sha_content.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
  }
  // Flip the first hex character.
  if (!sha_content.empty()) {
    sha_content[0] = (sha_content[0] == '0') ? '1' : '0';
    std::ofstream f(sha_path, std::ios::binary | std::ios::trunc);
    f << sha_content;
  }

  resident::MmapLibrary lib;
  bool threw = false;
  try {
    lib.open(bin_path);
  } catch (const std::exception&) {
    threw = true;
  }
  CHECK(threw);  // corrupted sidecar must reject

  return g_failures == 0;
}

}  // namespace

int main() {
  const fs::path dir = fs::temp_directory_path() / "bs_test_mmap_library";
  fs::remove_all(dir);
  fs::create_directories(dir);

  bool ok = true;
  ok = test_pack_and_mmap(dir) && ok;
  ok = test_sha256_verification(dir) && ok;

  fs::remove_all(dir);

  if (!ok || g_failures > 0) {
    std::fprintf(stderr, "mmap_library: %d failures\n", g_failures);
    return 1;
  }
  std::printf("mmap_library: all tests passed\n");
  return 0;
}
