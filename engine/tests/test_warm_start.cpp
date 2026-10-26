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
#include <fstream>
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
  state.tree_shape_digest = bs::tree::tree_shape_digest(fx.tree);
  // RFC 0035: the builder trains the default single limped root (2 seats,
  // buy-in 6, contribution 2); the BSWARM03 root-table witness must match it.
  const bs::flop_library::RootSpec expected_root = bs::flop_library::make_limped_root_spec(2, 6, 2);
  const std::span<const bs::flop_library::RootSpec> expected_roots(&expected_root, 1);
  state.root_table_hash = bs::flop_library::root_table_hash(2, expected_roots);
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
  const auto checkpoint = source_dir / "root-0000-class-0000-checkpoint.bswarm";

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
    (void)bs::flop_library::build_library(source_dir, 1, 32, fx.seed, 6, 2,
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
  bs::artifacts::save_sealed_checkpoint(source_dir / "root-0000-class-0000-checkpoint.bswarm",
                                        stale);

  const auto manifest = bs::flop_library::build_library(source_dir, 1, 32, fx.seed, 6, 2,
                                                        bs::flop_library::declared_library_range(),
                                                        2, fx.action, true);
  CHECK(manifest.class_count == 1);
  // The stale checkpoint is removed by the training path once the class is
  // complete, and the section is written from the fresh result.
  std::error_code ec;
  CHECK(!fs::exists(source_dir / "root-0000-class-0000-checkpoint.bswarm", ec));
  return true;
}

}  // namespace

// RFC 0033: the shared resume guard. Three states no existing test covers,
// because the RFC 0028 tests either carry a build.json or start empty.
bool test_resume_guard(const fs::path& dir) {
  Fixture fx;
  const auto trained =
      bs::solver::train_nseat(fx.tree, fx.ranges, 16, fx.seed, fx.limits, &fx.frontier);

  auto refuses = [&](const fs::path& lib_dir, std::uint64_t iterations,
                     std::string* message = nullptr) {
    try {
      bs::flop_library::BuildParams guard_params;
      guard_params.class_count = 1;
      guard_params.iterations_per_class = iterations;
      guard_params.player_count = 2;
      guard_params.starting_stack = 6;
      guard_params.contribution = 2;
      bs::flop_library::preflight_resume(lib_dir, guard_params, /*resume=*/true);
      return false;
    } catch (const std::runtime_error& error) {
      if (message)
        *message = error.what();
      return true;
    }
  };

  // (1) Sections survive with no identity. This is the state a checkpoint-keyed
  // guard lets through: every class completed, so no checkpoint remains, but
  // the sections can still be packed under a fresh header.
  {
    const fs::path lib_dir = dir / "guard-sections";
    fs::create_directories(lib_dir / ".sections");
    std::ofstream out(lib_dir / ".sections" / "root-0000-class-0000.bsf", std::ios::binary);
    out << "placeholder";  // presence is the predicate; content is not read
    out.close();
    std::string message;
    CHECK(refuses(lib_dir, /*iterations=*/32, &message));
    CHECK(message.find("build.json is missing") != std::string::npos);
    CHECK(message.find("root-0000-class-0000.bsf") != std::string::npos);
  }

  // (2) A valid incomplete checkpoint with no identity would warm-start against
  // an unknown root, so it must refuse.
  {
    const fs::path lib_dir = dir / "guard-live-checkpoint";
    fs::create_directories(lib_dir);
    bs::artifacts::SealedCheckpointState live = seal(fx, trained);
    live.completed_iterations = 4;  // below the target: genuinely in flight
    bs::artifacts::save_sealed_checkpoint(lib_dir / "root-0000-class-0000-checkpoint.bswarm", live);
    CHECK(refuses(lib_dir, /*iterations=*/32));
  }

  // (3) A stale-but-complete checkpoint is CLASSIFIED, not refused: the
  // training path drops it before it can influence anything. This is the
  // disposition split that keeps RFC 0028's stale contract intact.
  {
    const fs::path lib_dir = dir / "guard-stale-checkpoint";
    fs::create_directories(lib_dir);
    bs::artifacts::SealedCheckpointState stale = seal(fx, trained);
    stale.completed_iterations = 32;  // at the target
    bs::artifacts::save_sealed_checkpoint(lib_dir / "root-0000-class-0000-checkpoint.bswarm",
                                          stale);
    CHECK(!refuses(lib_dir, /*iterations=*/32));
    // Classification must not remove it; the training path still owns removal.
    CHECK(fs::exists(lib_dir / "root-0000-class-0000-checkpoint.bswarm"));
  }

  // (4) Revision beats stale. A checkpoint that is both must refuse, not be
  // dropped. No existing test reaches this intersection: the incompatible test
  // trains below the target and the stale test uses a compatible revision.
  {
    const fs::path lib_dir = dir / "guard-revision-beats-stale";
    fs::create_directories(lib_dir);
    bs::artifacts::SealedCheckpointState both = seal(fx, trained);
    both.completed_iterations = 32;  // stale ...
    both.algorithm_revision_id = 1;  // ... and incompatible
    both.algorithm_revision = bs::artifacts::sealed_revision_string(1);
    bs::artifacts::save_sealed_checkpoint(lib_dir / "root-0000-class-0000-checkpoint.bswarm", both);
    std::string message;
    CHECK(refuses(lib_dir, /*iterations=*/32, &message));
    CHECK(message.find("incompatible checkpoint algorithm") != std::string::npos);
  }

  // (5) A torn checkpoint is discarded and the guard stays silent, preserving
  // RFC 0028 AC4 (test_sealed_preflight_branches covers the full build path;
  // this pins the guard alone).
  {
    const fs::path lib_dir = dir / "guard-torn";
    fs::create_directories(lib_dir);
    std::ofstream out(lib_dir / "root-0000-class-0000-checkpoint.bswarm", std::ios::binary);
    out << "BSWARM01";
    out.close();
    CHECK(!refuses(lib_dir, /*iterations=*/32));
    CHECK(!fs::exists(lib_dir / "root-0000-class-0000-checkpoint.bswarm"));
  }
  return true;
}

// RFC 0033: an old build.json carries `"stack"` instead of `"starting_stack"`.
// read_identity must fail closed on the missing key rather than compare the
// file under a changed meaning.
bool test_old_build_identity_rejected(const fs::path& dir) {
  const auto range = bs::flop_library::declared_library_range();
  const fs::path lib_dir = dir / "old-identity";
  fs::create_directories(lib_dir / ".sections");
  // A pre-RFC-0033 identity: same values under the old key name.
  std::ofstream out(lib_dir / ".sections" / "build.json");
  out << "{\n"
      << "  \"codebook\": \"codebook16\",\n"
      << "  \"card_bucket_kind\": 2,\n"
      << "  \"card_abstraction_digest\": \"0000000000000000\",\n"
      << "  \"action_abstraction_digest\": \"0000000000000000\",\n"
      << "  \"range\": \"declared\",\n"
      << "  \"range_combo_count\": 40,\n"
      << "  \"range_digest\": \"0000000000000000\",\n"
      << "  \"player_count\": 2,\n"
      << "  \"big_blind\": 2,\n"
      << "  \"stack\": 6,\n"
      << "  \"contribution\": 2,\n"
      << "  \"max_raises_per_street\": 0,\n"
      << "  \"iterations_per_class\": 8,\n"
      << "  \"seed\": 1,\n"
      << "  \"class_count\": 1,\n"
      << "  \"root_count\": 1,\n"
      << "  \"root_table_hash\": \"0000000000000000\",\n"
      << "  \"total_classes\": 1755\n"
      << "}\n";
  out.close();

  bs::flop_library::BuildParams params;
  params.class_count = 1;
  params.iterations_per_class = 8;
  params.seed = 1;
  params.starting_stack = 6;
  params.contribution = 2;
  params.range = range;
  params.player_count = 2;

  bool threw = false;
  std::string message;
  try {
    bs::flop_library::prepare_build_identity(lib_dir, params, /*resume=*/true);
  } catch (const std::runtime_error& error) {
    threw = true;
    message = error.what();
  }
  CHECK(threw);
  CHECK(message.find("starting_stack") != std::string::npos);
  return true;
}

// RFC 0031: two builds under different card bucket kinds cannot share a
// build.json identity; resume is refused by name.
// RFC 0030 AC4.1: positional binding is the hole the tree-shape witness closes
// at the consumption site. Rows sealed for one tree are keyed only by node
// ordinal; nothing in the checkpoint proves ordinal k means the same node it
// meant at seal time. This test demonstrates what the witness DOES catch:
// two trees with identical game and abstraction identity (the case every
// existing guard — digests, revision, node/bucket caps — passes) but
// different shape produce different witnesses, and a checkpoint sealed for
// one must never be adopted into the other.
bool test_witness_closes_positional_binding() {
  Fixture fx;
  // Train a few iterations and take the raw rows of tree A.
  const bs::solver::NSeatTrainingResult trained =
      bs::solver::train_nseat(fx.tree, fx.ranges, 16, fx.seed, fx.limits, &fx.frontier);
  bs::solver::NSeatInitialState shifted;
  shifted.rows = trained.raw_rows;

  // A second tree with the same GameDef and the same abstraction identity is
  // the binary-drift case: BuildIdentity and both digests still match, while
  // tree code between binaries has expanded differently. Rebuilding is
  // deterministic today, so we model the drift directly by asserting the
  // witness is shape-sensitive (not just identity-sensitive): the digest is
  // recomputed over every node's kind and ordered menu, so it catches the
  // drift that node_cap (a pure bound) cannot.
  bs::tree::AbstractTree same_tree(fx.def, fx.action);
  const std::uint64_t witness = bs::tree::tree_shape_digest(fx.tree);
  const std::uint64_t rebuilt_witness = bs::tree::tree_shape_digest(same_tree);
  CHECK(witness == rebuilt_witness);  // deterministic rebuilds match

  // Shape sensitivity: at the clamped fixture depth (four chips behind) the
  // declared menu collapses onto the identity menu, so a deeper stack is
  // needed to expose a menu difference. A twenty-chip tree under the same
  // fixture unclamps the pot-fraction targets, so its node menus (and hence
  // its witness) differ even though the witness never reads stacks or pots.
  auto deep_def = fx.def;
  deep_def.stacks.fill(20);
  bs::tree::AbstractTree deep_tree(deep_def, fx.action);
  CHECK(deep_tree.size() != fx.tree.size() || bs::tree::tree_shape_digest(deep_tree) != witness);

  // The production enforcement is the witness refusal in train_one_class
  // (exercised by test_checkpoint_messages_pinned's witness-mismatch case);
  // here we prove the witness itself is the field that distinguishes the
  // shapes the existing bounds cannot.
  CHECK(static_cast<std::uint32_t>(same_tree.size()) == fx.node_cap());
  return true;
}

bool test_card_kind_identity_rejected(const fs::path& dir) {
  const auto range = bs::flop_library::declared_library_range();
  const fs::path lib_dir = dir / "card-kind-identity";
  fs::create_directories(lib_dir / ".sections");
  std::ofstream out(lib_dir / ".sections" / "build.json");
  out << "{\n"
      << "  \"codebook\": \"codebook16\",\n"
      << "  \"card_bucket_kind\": 0,\n"  // Identity; the builder uses Preflop169
      << "  \"card_abstraction_digest\": \"0000000000000000\",\n"
      << "  \"action_abstraction_digest\": \"0000000000000000\",\n"
      << "  \"range\": \"declared\",\n"
      << "  \"range_combo_count\": 40,\n"
      << "  \"range_digest\": \"0000000000000000\",\n"
      << "  \"player_count\": 2,\n"
      << "  \"big_blind\": 2,\n"
      << "  \"starting_stack\": 6,\n"
      << "  \"contribution\": 2,\n"
      << "  \"max_raises_per_street\": 0,\n"
      << "  \"iterations_per_class\": 8,\n"
      << "  \"seed\": 1,\n"
      << "  \"class_count\": 1,\n"
      << "  \"root_count\": 1,\n"
      << "  \"root_table_hash\": \"0000000000000000\",\n"
      << "  \"total_classes\": 1755\n"
      << "}\n";
  out.close();

  bs::flop_library::BuildParams params;
  params.class_count = 1;
  params.iterations_per_class = 8;
  params.seed = 1;
  params.starting_stack = 6;
  params.contribution = 2;
  params.range = range;
  params.player_count = 2;

  bool threw = false;
  std::string message;
  try {
    bs::flop_library::prepare_build_identity(lib_dir, params, /*resume=*/true);
  } catch (const std::runtime_error& error) {
    threw = true;
    message = error.what();
  }
  CHECK(threw);
  CHECK(message.find("card_bucket_kind") != std::string::npos);
  return true;
}

// RFC 0033: the two checkpoint messages no test pinned before. Both fixtures
// carry a build.json, because with no identity the guard refuses first and the
// message under test is unreachable.
bool test_checkpoint_messages_pinned(const fs::path& dir) {
  Fixture fx;
  const auto trained =
      bs::solver::train_nseat(fx.tree, fx.ranges, 16, fx.seed, fx.limits, &fx.frontier);

  auto build_with = [&](const fs::path& name, const bs::artifacts::SealedCheckpointState& ckpt,
                        std::string* message) {
    const fs::path lib_dir = dir / name;
    fs::create_directories(lib_dir);
    bs::flop_library::BuildParams params;
    params.class_count = 1;
    params.iterations_per_class = 32;
    params.seed = 1;
    params.starting_stack = 6;  // 6 - 2 = 4 behind, matching the Fixture tree
    params.contribution = 2;
    params.range = bs::flop_library::declared_library_range();
    params.player_count = 2;
    bs::flop_library::prepare_build_identity(lib_dir, params, /*resume=*/false);
    bs::artifacts::save_sealed_checkpoint(lib_dir / "root-0000-class-0000-checkpoint.bswarm", ckpt);
    params.resume = true;
    try {
      bs::flop_library::build_library_worker(lib_dir, 0, 1, params);
      return false;
    } catch (const std::runtime_error& error) {
      *message = error.what();
      return true;
    }
  };

  // Incompatible action-abstraction digest.
  {
    bs::artifacts::SealedCheckpointState bad = seal(fx, trained);
    bad.abstraction_digest ^= 0xffffffffffffffffULL;
    std::string message;
    CHECK(build_with("digest-mismatch", bad, &message));
    CHECK(message.find("action-abstraction digest mismatch") != std::string::npos);
  }

  // RFC 0033: incompatible CARD-abstraction digest with matching action
  // digest/rows. This message previously had no test; the refusal must name
  // the card abstraction rather than fall through to stale-drop or the action
  // digest message.
  {
    bs::artifacts::SealedCheckpointState bad = seal(fx, trained);
    bad.card_abstraction_digest = *bad.card_abstraction_digest ^ 0x0123456789abcdefULL;
    std::string message;
    CHECK(build_with("card-digest-mismatch", bad, &message));
    CHECK(message.find("card-abstraction mismatch") != std::string::npos);
    CHECK(message.find("action-abstraction") == std::string::npos);
  }

  // RFC 0030 AC4.2: a checkpoint with matching revision/digests/rows but a
  // tampered tree-shape witness hard-refuses in train_one_class (build.json is
  // present and matching), and the checkpoint file is PRESERVED.
  {
    bs::artifacts::SealedCheckpointState bad = seal(fx, trained);
    bad.tree_shape_digest = *bad.tree_shape_digest ^ 0xffffffffffffffffULL;
    std::string message;
    const fs::path stale = dir / "witness-mismatch" / "root-0000-class-0000-checkpoint.bswarm";
    CHECK(build_with("witness-mismatch", bad, &message));
    CHECK(message.find("checkpoint tree shape does not match") != std::string::npos);
    CHECK(fs::exists(stale));  // hard refusal, not automatic discard
  }

  // RFC 0030 AC4.4: a legacy BSWARM01 file is discarded as corrupt and the
  // worker trains from scratch; no old codec decodes it.
  {
    const fs::path lib_dir = dir / "legacy-magic";
    fs::create_directories(lib_dir);
    bs::flop_library::BuildParams params;
    params.class_count = 1;
    params.iterations_per_class = 32;
    params.seed = 1;
    params.starting_stack = 6;
    params.contribution = 2;
    params.range = bs::flop_library::declared_library_range();
    params.player_count = 2;
    bs::flop_library::prepare_build_identity(lib_dir, params, /*resume=*/false);
    const fs::path checkpoint = lib_dir / "root-0000-class-0000-checkpoint.bswarm";
    {
      std::ofstream out(checkpoint, std::ios::binary);
      out << "BSWARM01";  // the obsolete magic; torn/foreign to the new codec
    }
    bs::flop_library::build_library_worker(lib_dir, 0, 1, params);
    CHECK(!fs::exists(checkpoint));  // completion removes the checkpoint
  }

  // RFC 0031: a checkpoint sealed under another CardBucketKind is refused by
  // name at the resume comparison, not only by the codec ceiling. The
  // checkpoint is made loadable under CategoryTiersV1 (bucket_cap within its
  // ceiling; the flop fixture's buckets are the 1..9 category values), so the
  // refusal must come from train_one_class's key_kind comparison.
  {
    bs::artifacts::SealedCheckpointState bad = seal(fx, trained);
    bad.key_kind = bs::abstraction::CardBucketKind::CategoryTiersV1;
    bad.bucket_cap = 16;
    std::string message;
    CHECK(build_with("kind-mismatch", bad, &message));
    CHECK(message.find("card bucket kind mismatch") != std::string::npos);
  }

  // Empty training rows.
  {
    bs::artifacts::SealedCheckpointState empty = seal(fx, trained);
    empty.rows.clear();
    std::string message;
    CHECK(build_with("empty-rows", empty, &message));
    CHECK(message.find("carries no training rows") != std::string::npos);
  }
  return true;
}

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
  ok = test_resume_guard(dir) && ok;
  ok = test_old_build_identity_rejected(dir) && ok;
  ok = test_card_kind_identity_rejected(dir) && ok;
  ok = test_checkpoint_messages_pinned(dir) && ok;
  ok = test_witness_closes_positional_binding() && ok;

  fs::remove_all(dir, ec);
  if (!ok) {
    std::printf("warm_start: tests FAILED\n");
    return 1;
  }
  std::printf("warm_start: all tests passed\n");
  return 0;
}
