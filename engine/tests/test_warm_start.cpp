// RFC 0028: warm-start (sealed checkpoint resume) tests.
//
// The sealed checkpoint stores the trainer's own (bucket, node) table, so this
// test exercises the boundary directly: train to a chunk boundary, save, load,
// resume, and compare against a from-scratch run at the same iteration count.
// The per-iteration PRNG streams depend on the absolute iteration number, so a
// warm-started run must be bitwise-identical to a from-scratch run.
//
// Three jobs are kept from the RFC 0012 version of this test, at the sealed
// boundary: (a) chunk-boundary resume reproduces an uninterrupted run; (b) an
// incompatible algorithm revision is still refused, and the refusal is a hard
// error that leaves the files untouched; (c) the atomic-overwrite behavior
// moved to save_sealed_checkpoint. The SQLite-specific assertions went with the
// functions they exercised (see RFC 0028, "Builder changes").
#include <array>
#include <bs/abstract_tree.hpp>
#include <bs/abstraction.hpp>
#include <bs/equity_frontier.hpp>
#include <bs/flop_library.hpp>
#include <bs/game_definition.hpp>
#include <bs/nseat_trainer.hpp>
#include <bs/sealed_checkpoint.hpp>
#include <bs/seat_policy.hpp>
#include <bs/strategy_artifact.hpp>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <stdexcept>
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

  // The sealed caps the builder derives from the tree.
  std::uint32_t node_cap() const { return static_cast<std::uint32_t>(tree.nodes().size()); }
};

// Serialize a training result's raw table as the builder does.
bs::artifacts::SealedCheckpointState seal(const Fixture& fx,
                                          const bs::solver::NSeatTrainingResult& trained) {
  bs::artifacts::SealedCheckpointState state;
  state.algorithm_revision_id = trained.algorithm_revision;
  const char* revision = bs::artifacts::sealed_revision_string(trained.algorithm_revision);
  if (revision == nullptr)
    throw std::runtime_error("unknown trainer algorithm revision in test fixture");
  state.algorithm_revision = revision;
  state.abstraction_digest = fx.action.id().digest;
  state.card_abstraction_digest = fx.card_id.digest;
  state.completed_iterations = trained.completed_iterations;
  state.batch_size = trained.batch_size;
  state.completed_epochs = trained.completed_epochs;
  state.node_cap = fx.node_cap();
  state.bucket_cap = 169;
  state.key_kind = bs::solver::kNSeatCardKind;
  state.rows = trained.raw_rows;
  return state;
}

bool same_raw_rows(const std::map<bs::solver::NSeatInformationKey, bs::solver::NSeatRawRow>& a,
                   const std::map<bs::solver::NSeatInformationKey, bs::solver::NSeatRawRow>& b) {
  if (a.size() != b.size())
    return false;
  for (const auto& [key, row] : b) {
    const auto it = a.find(key);
    if (it == a.end())
      return false;
    const bs::solver::NSeatRawRow& got = it->second;
    if (got.visits != row.visits)
      return false;
    if (got.actions.size() != row.actions.size() || got.regrets.size() != row.regrets.size() ||
        got.sums.size() != row.sums.size())
      return false;
    for (std::size_t i = 0; i < row.actions.size(); ++i) {
      if (got.actions[i].type != row.actions[i].type ||
          got.actions[i].target_total != row.actions[i].target_total)
        return false;
      if (std::memcmp(&got.regrets[i], &row.regrets[i], sizeof(double)) != 0)
        return false;
      if (std::memcmp(&got.sums[i], &row.sums[i], sizeof(double)) != 0)
        return false;
    }
  }
  return true;
}

bool test_warm_start_matches_scratch(const fs::path& dir) {
  Fixture fx;

  // Phase 1: train 256 iterations from scratch.
  const bs::solver::NSeatTrainingResult r256 =
      bs::solver::train_nseat(fx.tree, fx.ranges, 256, fx.seed, fx.limits, &fx.frontier);
  CHECK(r256.termination == bs::solver::NSeatTerminationPhase::Complete);
  CHECK(r256.completed_iterations == 256);
  CHECK(r256.batch_size == 1 && r256.completed_epochs == 256);
  CHECK(!r256.raw_rows.empty());

  // Save a sealed checkpoint holding exactly the trainer's table.
  const fs::path checkpoint_path = dir / "warm-start-checkpoint.bswarm";
  bs::artifacts::save_sealed_checkpoint(checkpoint_path, seal(fx, r256));
  CHECK(fs::exists(checkpoint_path));

  // Load it back and verify both the identity and the rows.
  const bs::artifacts::SealedCheckpointState ckpt =
      bs::artifacts::load_sealed_checkpoint(checkpoint_path);
  CHECK(ckpt.completed_iterations == 256);
  CHECK(ckpt.algorithm_revision ==
        bs::artifacts::sealed_revision_string(bs::solver::kNSeatAlgorithmRevision));
  CHECK(ckpt.algorithm_revision_id == bs::solver::kNSeatAlgorithmRevision);
  CHECK(ckpt.abstraction_digest == fx.action.id().digest);
  CHECK(ckpt.card_abstraction_digest.has_value());
  CHECK(ckpt.card_abstraction_digest.value() == fx.card_id.digest);
  CHECK(!ckpt.rows.empty());
  CHECK(same_raw_rows(ckpt.rows, r256.raw_rows));

  // Build the trainer's initial state directly from the loaded rows.
  bs::solver::NSeatInitialState state;
  state.rows = ckpt.rows;
  state.completed_iterations = ckpt.completed_iterations;
  state.algorithm_revision = ckpt.algorithm_revision_id;
  state.batch_size = static_cast<std::size_t>(ckpt.batch_size);
  CHECK(state.completed_iterations == 256);
  CHECK(state.algorithm_revision == 2 && state.batch_size == 1);
  CHECK(state.rows.size() == r256.raw_rows.size());

  // Phase 2: resume from the checkpoint, training 256 more iterations
  // (512 total).
  const bs::solver::NSeatTrainingResult r512_resumed =
      bs::solver::train_nseat(fx.tree, fx.ranges, 256, fx.seed, fx.limits, &fx.frontier, &state);
  CHECK(r512_resumed.termination == bs::solver::NSeatTerminationPhase::Complete);
  CHECK(r512_resumed.completed_iterations == 512);
  CHECK(r512_resumed.batch_size == 1 && r512_resumed.completed_epochs == 512);

  // Reference: train 512 iterations from scratch.
  const bs::solver::NSeatTrainingResult r512_scratch =
      bs::solver::train_nseat(fx.tree, fx.ranges, 512, fx.seed, fx.limits, &fx.frontier);
  CHECK(r512_scratch.termination == bs::solver::NSeatTerminationPhase::Complete);
  CHECK(r512_scratch.completed_iterations == 512);
  CHECK(r512_scratch.batch_size == 1 && r512_scratch.completed_epochs == 512);

  // Compare the raw regret/average-weight/visits tables bitwise.
  CHECK(same_raw_rows(r512_resumed.raw_rows, r512_scratch.raw_rows));
  // And the derived policy, which is what the published artifact carries.
  CHECK(r512_resumed.policy.rows().size() == r512_scratch.policy.rows().size());
  for (const auto& [key, row] : r512_scratch.policy.rows()) {
    const auto resumed = r512_resumed.policy.rows().find(key);
    CHECK(resumed != r512_resumed.policy.rows().end());
    CHECK(resumed->second.actions == row.actions);
    CHECK(resumed->second.probabilities == row.probabilities);
  }
  return true;
}

// A checkpoint written under a different algorithm revision is a hard
// refusal: the builder's preflights must refuse rather than discard and
// retrain, and the files must be left byte-identical.
bool test_incompatible_resume_preserves_files(const fs::path& dir) {
  Fixture fx;
  const auto trained =
      bs::solver::train_nseat(fx.tree, fx.ranges, 16, fx.seed, fx.limits, &fx.frontier);
  const auto exported = bs::solver::export_seat_policy(trained, fx.tree, fx.ranges, fx.card_id);
  const auto source_dir = dir / "refused-resume";
  fs::create_directories(source_dir);
  const auto checkpoint = source_dir / "class-0000-checkpoint.bswarm";

  // A well-formed sealed file whose revision the current trainer rejects. The
  // numeric and string revisions must agree or the codec rejects the file as
  // corrupt before the revision rule is even reached, so both are set.
  bs::artifacts::SealedCheckpointState bad = seal(fx, trained);
  bad.algorithm_revision_id = 1;
  bad.algorithm_revision = bs::artifacts::sealed_revision_string(1);
  bs::artifacts::save_sealed_checkpoint(checkpoint, bad);
  const auto checkpoint_sha = bs::artifacts::sha256_file_hex(checkpoint);

  // The sealed load succeeds: this is structurally valid, incompatibility is a
  // semantic judgement the caller makes.
  const auto loaded = bs::artifacts::load_sealed_checkpoint(checkpoint);
  CHECK(loaded.algorithm_revision_id == 1);

  // The header-only inspection the preflights use agrees with the full load.
  const auto inspected = bs::artifacts::inspect_sealed_checkpoint(checkpoint);
  CHECK(inspected.algorithm_revision_id == 1);
  CHECK(inspected.algorithm_revision == bs::artifacts::sealed_revision_string(1));

  // The trainer independently rejects the numeric identity.
  bs::solver::NSeatInitialState state;
  state.rows = trained.raw_rows;
  state.completed_iterations = trained.completed_iterations;
  state.algorithm_revision = 1;
  state.batch_size = 1;
  bool refused = false;
  try {
    (void)bs::solver::train_nseat(fx.tree, fx.ranges, 1, fx.seed, fx.limits, &fx.frontier, &state);
  } catch (const std::invalid_argument&) {
    refused = true;
  }
  CHECK(refused);

  // The builder's resume path refuses and leaves the checkpoint untouched.
  refused = false;
  try {
    (void)bs::flop_library::build_library(source_dir, 1, 32, fx.seed, 4, 2,
                                          bs::flop_library::declared_library_range(), 2, fx.action,
                                          true);
  } catch (const std::runtime_error& error) {
    refused =
        std::string(error.what()).find("incompatible checkpoint algorithm") != std::string::npos;
  }
  CHECK(refused);
  CHECK(fs::exists(checkpoint));
  CHECK(bs::artifacts::sha256_file_hex(checkpoint) == checkpoint_sha);

  // An unrepresentable state is refused by the writer instead of being
  // silently truncated into an incompatible file.
  bs::artifacts::SealedCheckpointState unrepresentable = seal(fx, trained);
  unrepresentable.algorithm_revision_id = 99;  // no canonical string
  bool writer_refused = false;
  try {
    bs::artifacts::save_sealed_checkpoint(source_dir / "never.bswarm", unrepresentable);
  } catch (const bs::artifacts::SealedCheckpointError&) {
    writer_refused = true;
  }
  CHECK(writer_refused);
  return true;
}

// A newer checkpoint atomically replaces an older one: the iteration count
// advances and the file is completely rewritten (no stale trailing bytes).
bool test_checkpoint_overwrite(const fs::path& dir) {
  Fixture fx;
  const bs::solver::NSeatTrainingResult r64 =
      bs::solver::train_nseat(fx.tree, fx.ranges, 64, fx.seed, fx.limits, &fx.frontier);
  const fs::path path = dir / "overwrite-checkpoint.bswarm";
  bs::artifacts::save_sealed_checkpoint(path, seal(fx, r64));
  const std::string sha1 = bs::artifacts::sha256_file_hex(path);

  const bs::solver::NSeatTrainingResult r128 =
      bs::solver::train_nseat(fx.tree, fx.ranges, 128, fx.seed, fx.limits, &fx.frontier);
  bs::artifacts::save_sealed_checkpoint(path, seal(fx, r128));
  const std::string sha2 = bs::artifacts::sha256_file_hex(path);
  CHECK(sha1 != sha2);  // the overwrite changed the file

  const bs::artifacts::SealedCheckpointState ckpt = bs::artifacts::load_sealed_checkpoint(path);
  CHECK(ckpt.completed_iterations == 128);
  CHECK(same_raw_rows(ckpt.rows, r128.raw_rows));
  return true;
}

// A checkpoint at or past the target iteration count is stale-but-complete: a
// crash after training but before the section was written. The builder drops
// it and retrains, so the publish path has a fresh result to export. This is
// the fourth AC2 case.
bool test_stale_but_complete(const fs::path& dir) {
  const auto source_dir = dir / "stale-complete";
  fs::create_directories(source_dir);
  Fixture fx;
  const bs::solver::NSeatTrainingResult trained =
      bs::solver::train_nseat(fx.tree, fx.ranges, 32, fx.seed, fx.limits, &fx.frontier);
  bs::artifacts::SealedCheckpointState stale = seal(fx, trained);
  stale.completed_iterations = 32;  // exactly the class target below
  bs::artifacts::save_sealed_checkpoint(source_dir / "class-0000-checkpoint.bswarm", stale);

  const auto manifest = bs::flop_library::build_library(source_dir, 1, 32, fx.seed, 4, 2,
                                                        bs::flop_library::declared_library_range(),
                                                        2, fx.action, true);
  CHECK(manifest.class_count == 1);
  // The stale checkpoint is removed by the training path once the class is
  // complete, and the section is written from the fresh result.
  std::error_code ec;
  CHECK(!fs::exists(source_dir / "class-0000-checkpoint.bswarm", ec));
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
  ok = test_incompatible_resume_preserves_files(dir) && ok;
  ok = test_stale_but_complete(dir) && ok;

  fs::remove_all(dir, ec);
  if (!ok) {
    std::printf("warm_start: tests FAILED\n");
    return 1;
  }
  std::printf("warm_start: all tests passed\n");
  return 0;
}
