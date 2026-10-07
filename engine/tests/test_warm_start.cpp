// RFC 0012 Phase 4: warm-start (checkpoint resume) tests.
//
// Trains a class to 256 iterations, saves a warm-start checkpoint, resumes
// from it to 512 iterations, and compares the raw regret/average-weight table
// against a from-scratch 512-iteration run. The per-iteration PRNG streams
// depend on the absolute iteration number, so a warm-started run must be
// bitwise-identical to a from-scratch run at the same iteration count; the
// 1e-9 tolerance is a safety margin for any unforeseen floating-point
// reordering.
#include <array>
#include <bs/abstract_tree.hpp>
#include <bs/abstraction.hpp>
#include <bs/equity_frontier.hpp>
#include <bs/flop_library.hpp>
#include <bs/game_definition.hpp>
#include <bs/nseat_trainer.hpp>
#include <bs/seat_policy.hpp>
#include <bs/strategy_artifact.hpp>
#include <chrono>
#include <cmath>
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
using bs::poker::Chips;

// Build a small flop-rooted flop-terminal game (2 seats, 4 behind, 2 in, pot
// 4) on the 2c2d2h board. The declared premium range (AA/KK/QQ/JJ/AK) has no
// overlap with this board, so all 40 combos are live.
struct Fixture {
  bs::poker::GameDef def{};
  std::vector<std::vector<bs::solver::WeightedHand>> ranges;
  bs::abstraction::ActionAbstraction action = bs::abstraction::ActionAbstraction::identity();
  bs::tree::AbstractTree tree;
  bs::gto::EquityFrontierEvaluator frontier;
  bs::solver::NSeatTrainerLimits limits;
  bs::abstraction::AbstractionId card_id = bs::abstraction::suit_canonicalization_id();
  std::uint64_t seed = 20261002;

  Fixture()
      : tree([&] {
          def.player_count = 2;
          def.button = 0;
          def.big_blind = 2;
          def.stacks[0] = 4;
          def.stacks[1] = 4;
          def.contributions[0] = 2;
          def.contributions[1] = 2;
          def.pot = 4;
          def.board = {0, 1, 2, 0, 0};  // 2c 2d 2h
          def.board_size = 3;
          def.terminal = bs::poker::TerminalDepth::Flop;

          const auto full_range = bs::flop_library::declared_library_range();
          std::array<bool, 52> on_board{};
          for (int c : {0, 1, 2})
            on_board[static_cast<std::size_t>(c)] = true;
          std::vector<bs::solver::WeightedHand> seat_range;
          for (const auto& h : full_range)
            if (!on_board[static_cast<std::size_t>(h.cards[0])] &&
                !on_board[static_cast<std::size_t>(h.cards[1])])
              seat_range.push_back(h);
          ranges = {seat_range, seat_range};

          limits.max_nodes = 10'000'000;
          limits.max_information_sets = 5'000'000;
          limits.wall = std::chrono::minutes{5};
          return bs::tree::AbstractTree(def, action);
        }()) {}
};

bool test_warm_start_matches_scratch(const fs::path& dir) {
  Fixture fx;

  // Phase 1: train 256 iterations from scratch.
  const bs::solver::NSeatTrainingResult r256 =
      bs::solver::train_nseat(fx.tree, fx.ranges, 256, fx.seed, fx.limits, &fx.frontier);
  CHECK(r256.termination == bs::solver::NSeatTerminationPhase::Complete);
  CHECK(r256.completed_iterations == 256);
  CHECK(!r256.raw_rows.empty());

  // Save a warm-start checkpoint with the raw training rows.
  const bs::solver::SeatTrainingResult exported256 =
      bs::solver::export_seat_policy(r256, fx.tree, fx.ranges, fx.card_id);
  const auto training_rows = bs::flop_library::export_seat_training_rows(r256, fx.tree);
  CHECK(!training_rows.empty());
  const fs::path checkpoint_path = dir / "warm-start-checkpoint.db";
  bs::artifacts::SeatCheckpointProvenance provenance;
  provenance.engine_revision = "test-warm-start";
  bs::artifacts::save_training_checkpoint(checkpoint_path, exported256, training_rows, provenance);
  CHECK(fs::exists(checkpoint_path));

  // Load the checkpoint and verify its identity.
  const bs::artifacts::CheckpointTrainingState ckpt =
      bs::artifacts::load_checkpoint(checkpoint_path);
  CHECK(ckpt.completed_iterations == 256);
  CHECK(ckpt.abstraction_digest == fx.action.id().digest);
  CHECK(ckpt.card_abstraction.has_value());
  CHECK(ckpt.card_abstraction.value() == fx.card_id);
  CHECK(!ckpt.rows.empty());

  // Build the trainer's initial state from the checkpoint.
  const bs::solver::NSeatInitialState state = bs::flop_library::build_initial_state(ckpt, fx.tree);
  CHECK(state.completed_iterations == 256);
  CHECK(!state.rows.empty());
  CHECK(state.rows.size() == r256.raw_rows.size());

  // Phase 2: resume from the checkpoint, training 256 more iterations
  // (512 total).
  const bs::solver::NSeatTrainingResult r512_resumed =
      bs::solver::train_nseat(fx.tree, fx.ranges, 256, fx.seed, fx.limits, &fx.frontier, &state);
  CHECK(r512_resumed.termination == bs::solver::NSeatTerminationPhase::Complete);
  CHECK(r512_resumed.completed_iterations == 512);

  // Reference: train 512 iterations from scratch.
  const bs::solver::NSeatTrainingResult r512_scratch =
      bs::solver::train_nseat(fx.tree, fx.ranges, 512, fx.seed, fx.limits, &fx.frontier);
  CHECK(r512_scratch.termination == bs::solver::NSeatTerminationPhase::Complete);
  CHECK(r512_scratch.completed_iterations == 512);

  // Compare the raw regret/average-weight tables.
  CHECK(r512_resumed.raw_rows.size() == r512_scratch.raw_rows.size());
  for (const auto& [key, scratch_row] : r512_scratch.raw_rows) {
    const auto it = r512_resumed.raw_rows.find(key);
    CHECK(it != r512_resumed.raw_rows.end());
    const bs::solver::NSeatRawRow& resumed_row = it->second;
    CHECK(resumed_row.actions.size() == scratch_row.actions.size());
    CHECK(resumed_row.regrets.size() == scratch_row.regrets.size());
    CHECK(resumed_row.sums.size() == scratch_row.sums.size());
    for (std::size_t a = 0; a < scratch_row.regrets.size(); ++a) {
      const double regret_diff = std::fabs(resumed_row.regrets[a] - scratch_row.regrets[a]);
      const double sum_diff = std::fabs(resumed_row.sums[a] - scratch_row.sums[a]);
      if (regret_diff > 1e-9) {
        std::printf(
            "regret mismatch at (bucket=%u, node=%llu, action=%zu): resumed=%.17g "
            "scratch=%.17g diff=%.3g\n",
            key.own_card_bucket, static_cast<unsigned long long>(key.node_index), a,
            resumed_row.regrets[a], scratch_row.regrets[a], regret_diff);
        return false;
      }
      if (sum_diff > 1e-9) {
        std::printf(
            "sum mismatch at (bucket=%u, node=%llu, action=%zu): resumed=%.17g "
            "scratch=%.17g diff=%.3g\n",
            key.own_card_bucket, static_cast<unsigned long long>(key.node_index), a,
            resumed_row.sums[a], scratch_row.sums[a], sum_diff);
        return false;
      }
    }
  }
  return true;
}

bool test_checkpoint_overwrite(const fs::path& dir) {
  // save_training_checkpoint must atomically overwrite an existing file.
  Fixture fx;
  const bs::solver::NSeatTrainingResult r1 =
      bs::solver::train_nseat(fx.tree, fx.ranges, 64, fx.seed, fx.limits, &fx.frontier);
  const bs::solver::SeatTrainingResult exported1 =
      bs::solver::export_seat_policy(r1, fx.tree, fx.ranges, fx.card_id);
  const auto rows1 = bs::flop_library::export_seat_training_rows(r1, fx.tree);
  const fs::path path = dir / "overwrite-checkpoint.db";
  bs::artifacts::SeatCheckpointProvenance provenance;
  provenance.engine_revision = "test-overwrite";
  bs::artifacts::save_training_checkpoint(path, exported1, rows1, provenance);
  const std::string sha1 = bs::artifacts::sha256_file_hex(path);

  // Train more and overwrite.
  const bs::solver::NSeatTrainingResult r2 =
      bs::solver::train_nseat(fx.tree, fx.ranges, 128, fx.seed, fx.limits, &fx.frontier);
  const bs::solver::SeatTrainingResult exported2 =
      bs::solver::export_seat_policy(r2, fx.tree, fx.ranges, fx.card_id);
  const auto rows2 = bs::flop_library::export_seat_training_rows(r2, fx.tree);
  bs::artifacts::save_training_checkpoint(path, exported2, rows2, provenance);
  const std::string sha2 = bs::artifacts::sha256_file_hex(path);
  CHECK(sha1 != sha2);  // the overwrite changed the file

  // The overwritten checkpoint loads with the new iteration count.
  const bs::artifacts::CheckpointTrainingState ckpt = bs::artifacts::load_checkpoint(path);
  CHECK(ckpt.completed_iterations == 128);
  return true;
}

}  // namespace

int main() {
  const fs::path dir = fs::temp_directory_path() / "bs-warm-start-test";
  std::error_code ec;
  fs::remove_all(dir, ec);
  fs::create_directories(dir, ec);

  bool ok = true;
  ok = test_warm_start_matches_scratch(dir) && ok;
  ok = test_checkpoint_overwrite(dir) && ok;

  fs::remove_all(dir, ec);
  if (!ok) {
    std::printf("warm_start: tests FAILED\n");
    return 1;
  }
  std::printf("warm_start: all tests passed\n");
  return 0;
}
