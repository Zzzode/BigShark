// RFC 0012 Phase 3: lazy loading tests.
//
// Builds a small flop class library, constructs a ResidentPolicySet in lazy
// mode, and verifies that:
//   1. No artifacts are loaded at build time (resident_bytes == 0).
//   2. Root-level public_belief queries match the eager path.
//   3. hero_decision queries match the eager path.
//   4. The LRU cache evicts entries when the capacity is exceeded.
#include <array>
#include <bs/abstraction.hpp>
#include <bs/equity_frontier.hpp>
#include <bs/flop_library.hpp>
#include <bs/game_definition.hpp>
#include <bs/heads_up.hpp>
#include <bs/mmap_library.hpp>
#include <bs/nseat_trainer.hpp>
#include <bs/resident_policy.hpp>
#include <bs/seat_policy.hpp>
#include <bs/strategy_artifact.hpp>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace bs;

namespace {

int g_failures = 0;
#define CHECK(cond)                                                        \
  do {                                                                     \
    if (!(cond)) {                                                         \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      ++g_failures;                                                        \
    }                                                                      \
  } while (0)

// Construct the GameDef for one class root from the manifest identity.
poker::GameDef class_root_def(const flop_library::LibraryManifest& manifest,
                              std::size_t class_index) {
  poker::GameDef def{};
  def.player_count = manifest.player_count;
  def.button = 0;
  def.big_blind = manifest.big_blind;
  for (std::size_t s = 0; s < manifest.player_count; ++s) {
    def.stacks[s] = manifest.stack;
    def.contributions[s] = manifest.contribution;
  }
  def.pot = static_cast<poker::Chips>(manifest.player_count * manifest.contribution);
  const auto& board = manifest.classes[class_index].canonical_board;
  def.board = {board[0], board[1], board[2], 0, 0};
  def.board_size = 3;
  def.terminal = poker::TerminalDepth::Flop;
  return def;
}

// Build the shared game def for lazy mode (board zeroed; set per-class).
poker::GameDef shared_def(const flop_library::LibraryManifest& manifest) {
  poker::GameDef def{};
  def.player_count = manifest.player_count;
  def.button = 0;
  def.big_blind = manifest.big_blind;
  for (std::size_t s = 0; s < manifest.player_count; ++s) {
    def.stacks[s] = manifest.stack;
    def.contributions[s] = manifest.contribution;
  }
  def.pot = static_cast<poker::Chips>(manifest.player_count * manifest.contribution);
  def.board_size = 0;
  def.terminal = poker::TerminalDepth::Flop;
  return def;
}

// Find a hero combo from the declared range that does not share a card with
// the board.
std::array<int, 2> find_hero_combo(const std::array<int, 3>& board) {
  const auto range = flop_library::declared_library_range();
  for (const auto& hand : range) {
    bool blocked = false;
    for (int b : board) {
      if (hand.cards[0] == b || hand.cards[1] == b) {
        blocked = true;
        break;
      }
    }
    if (!blocked)
      return hand.cards;
  }
  return {0, 1};  // fallback (will likely miss)
}

// RFC 0027: the direct builder no longer writes per-class SQLite policies, but
// the lazy SQLite cache still reads them. This fixture reproduces the retired
// publish step for a small class set so the lazy path keeps its own test until
// RFC 0027's removal condition retires it.
std::vector<std::string> write_sqlite_class_artifacts(const fs::path& dir,
                                                      const flop_library::LibraryManifest& manifest,
                                                      std::uint64_t iterations) {
  const auto range = flop_library::declared_library_range();
  std::vector<std::string> artifact_names;
  const auto classes = flop_library::enumerate_classes();
  const auto card_id = abstraction::suit_canonicalization_id();
  for (std::size_t i = 0; i < manifest.class_count; ++i) {
    const auto& board = manifest.classes[i].canonical_board;
    poker::GameDef def{};
    def.player_count = manifest.player_count;
    def.button = 0;
    def.big_blind = manifest.big_blind;
    for (std::size_t s = 0; s < manifest.player_count; ++s) {
      def.stacks[s] = manifest.stack;
      def.contributions[s] = manifest.contribution;
    }
    def.pot = static_cast<poker::Chips>(manifest.player_count * manifest.contribution);
    def.board = {board[0], board[1], board[2], 0, 0};
    def.board_size = 3;
    def.terminal = poker::TerminalDepth::Flop;

    std::vector<solver::WeightedHand> seat_range;
    for (const auto& hand : range) {
      bool blocked = false;
      for (int b : board)
        if (hand.cards[0] == b || hand.cards[1] == b)
          blocked = true;
      if (!blocked)
        seat_range.push_back(hand);
    }
    std::vector<std::vector<solver::WeightedHand>> ranges(manifest.player_count, seat_range);
    const tree::AbstractTree tree(def, abstraction::ActionAbstraction::identity());
    solver::NSeatTrainerLimits limits;
    limits.wall = std::chrono::minutes(5);
    const gto::EquityFrontierEvaluator frontier;
    const auto trained = solver::train_nseat(tree, ranges, iterations, 42, limits, &frontier);
    const auto exported = solver::export_seat_policy(trained, tree, ranges, card_id);

    artifacts::SeatCheckpointProvenance provenance;
    provenance.engine_revision = "test-lazy-fixture";
    const fs::path checkpoint = dir / ("immutable-source-" + std::to_string(i) + ".db");
    artifacts::create_checkpoint(checkpoint, exported, provenance);
    const std::string name = "class-" + std::to_string(i) + ".db";
    (void)artifacts::publish_policy(checkpoint, dir / name);
    std::error_code ec;
    fs::remove(checkpoint, ec);
    artifact_names.push_back(name);
  }
  return artifact_names;
}
bool test_lazy_loading(const fs::path& dir) {
  // RFC 0027: the direct builder writes no per-class SQLite policy, so this
  // lazy-loading test builds its roots from the packed library's own classes
  // (the lazy cache reads SQLite artifacts, which is why this test still
  // produces them explicitly through the artifact writer below).
  const auto range = flop_library::declared_library_range();
  const flop_library::LibraryManifest manifest =
      flop_library::build_library(dir, 2, 100, 42, 6, 2, range);
  CHECK(manifest.class_count == 2);

  const std::vector<std::string> artifact_names = write_sqlite_class_artifacts(dir, manifest, 100);
  std::vector<resident::SupportedRootSpec> specs;
  for (std::size_t i = 0; i < manifest.classes.size(); ++i) {
    resident::SupportedRootSpec spec;
    spec.path = dir / artifact_names[i];
    artifacts::Sha256Digest digest{};
    const std::string hex = artifacts::sha256_file_hex(spec.path);
    for (std::size_t b = 0; b < 32; ++b) {
      auto nib = [](char c) -> int {
        if (c >= '0' && c <= '9')
          return c - '0';
        return c - 'a' + 10;
      };
      digest[b] = static_cast<std::uint8_t>((nib(hex[2 * b]) << 4) | nib(hex[2 * b + 1]));
    }
    spec.expected_sha256 = digest;
    spec.canonical_board = manifest.classes[i].canonical_board;
    specs.push_back(std::move(spec));
  }

  // --- Eager build (reference) ---
  resident::ResidentOptions eager_options;
  eager_options.budget_bytes = resident::kDefaultResidentBudgetBytes;
  std::vector<resident::RootLoadResult> eager_results;
  resident::ResidentPolicySet eager_set =
      resident::ResidentPolicySet::build(specs, eager_options, &eager_results);
  CHECK(eager_set.advertised_roots() == 2);

  // --- Lazy build (cache capacity = 1 for LRU testing) ---
  resident::ResidentOptions lazy_options;
  lazy_options.budget_bytes = resident::kDefaultResidentBudgetBytes;
  lazy_options.lazy_cache_capacity = 1;
  lazy_options.shared_game_def = shared_def(manifest);
  std::vector<resident::RootLoadResult> lazy_results;
  resident::ResidentPolicySet lazy_set =
      resident::ResidentPolicySet::build(specs, lazy_options, &lazy_results);
  CHECK(lazy_set.advertised_roots() == 2);

  // Verify lazy build did not load any artifacts.
  for (const auto& result : lazy_results) {
    CHECK(result.status == resident::RootStatus::Advertised);
    CHECK(result.resident_bytes == 0);
  }

  // --- Query comparison: public_belief at the root for each class ---
  for (std::size_t c = 0; c < manifest.class_count; ++c) {
    const poker::GameDef def = class_root_def(manifest, c);
    const poker::GameState state(def);
    const std::span<const poker::PublicAction> no_history{};

    resident::ResidentScratch eager_scratch{};
    const resident::ResidentAnswer eager_answer =
        eager_set.public_belief(state, no_history, std::nullopt, eager_scratch);

    resident::ResidentScratch lazy_scratch{};
    const resident::ResidentAnswer lazy_answer =
        lazy_set.public_belief(state, no_history, std::nullopt, lazy_scratch);

    CHECK(eager_answer.hit == lazy_answer.hit);
    CHECK(eager_answer.reason == lazy_answer.reason);
    if (eager_answer.hit && lazy_answer.hit) {
      CHECK(eager_answer.public_reach_seats == lazy_answer.public_reach_seats);
      // Compare marginal distributions.
      for (std::size_t seat = 0; seat < eager_answer.public_reach_seats; ++seat) {
        const auto& eager_marg = *eager_answer.public_reach[seat];
        const auto& lazy_marg = *lazy_answer.public_reach[seat];
        for (std::size_t combo = 0; combo < 1326; ++combo) {
          CHECK(eager_marg[combo] == lazy_marg[combo]);
        }
      }
    }
  }

  // --- Query comparison: hero_decision ---
  for (std::size_t c = 0; c < manifest.class_count; ++c) {
    const poker::GameDef def = class_root_def(manifest, c);
    const poker::GameState state(def);
    const std::span<const poker::PublicAction> no_history{};
    const auto hero_cards = find_hero_combo(manifest.classes[c].canonical_board);

    resident::ResidentScratch eager_scratch{};
    const resident::ResidentAnswer eager_answer =
        eager_set.hero_decision(state, no_history, hero_cards, std::nullopt, eager_scratch);

    resident::ResidentScratch lazy_scratch{};
    const resident::ResidentAnswer lazy_answer =
        lazy_set.hero_decision(state, no_history, hero_cards, std::nullopt, lazy_scratch);

    CHECK(eager_answer.hit == lazy_answer.hit);
    CHECK(eager_answer.reason == lazy_answer.reason);
    if (eager_answer.hit && lazy_answer.hit) {
      CHECK(eager_answer.hero_row.size == lazy_answer.hero_row.size);
      for (std::size_t i = 0; i < eager_answer.hero_row.size; ++i) {
        CHECK(eager_answer.hero_row.actions[i].type == lazy_answer.hero_row.actions[i].type);
        CHECK(eager_answer.hero_row.actions[i].target_total ==
              lazy_answer.hero_row.actions[i].target_total);
        CHECK(eager_answer.hero_row.probabilities[i] == lazy_answer.hero_row.probabilities[i]);
      }
    }
  }

  // --- LRU eviction: query class 0, then class 1, then class 0 again ---
  // With cache capacity = 1, each query evicts the previous class.
  {
    const poker::GameDef def0 = class_root_def(manifest, 0);
    const poker::GameState state0(def0);
    const std::span<const poker::PublicAction> no_history{};

    resident::ResidentScratch scratch{};
    // First query: loads class 0 into cache.
    auto answer = lazy_set.public_belief(state0, no_history, std::nullopt, scratch);
    CHECK(answer.hit);

    // Query class 1: evicts class 0, loads class 1.
    const poker::GameDef def1 = class_root_def(manifest, 1);
    const poker::GameState state1(def1);
    answer = lazy_set.public_belief(state1, no_history, std::nullopt, scratch);
    CHECK(answer.hit);

    // Query class 0 again: evicts class 1, reloads class 0.
    answer = lazy_set.public_belief(state0, no_history, std::nullopt, scratch);
    CHECK(answer.hit);
  }

  return g_failures == 0;
}

bool test_lazy_mmap_bypass(const fs::path& dir) {
  // When binary_library_path is set, lazy mode is bypassed (mmap wins).
  // Build a library, pack it, then verify that mmap mode works.
  const auto range = flop_library::declared_library_range();
  const flop_library::LibraryManifest manifest =
      flop_library::build_library(dir / "lib", 2, 50, 99, 6, 2, range);
  CHECK(manifest.class_count == 2);

  // RFC 0027: the builder already wrote the packed library.
  const fs::path bin_path = dir / "lib" / manifest.library_bin;
  CHECK(fs::exists(bin_path));

  // Build with both binary_library_path and lazy_cache_capacity set.
  resident::ResidentOptions options;
  options.binary_library_path = bin_path;
  options.lazy_cache_capacity = 1;                 // should be ignored
  options.shared_game_def = shared_def(manifest);  // should be ignored
  std::vector<resident::RootLoadResult> results;
  resident::ResidentPolicySet set = resident::ResidentPolicySet::build({}, options, &results);
  CHECK(set.advertised_roots() == 2);
  for (const auto& result : results) {
    CHECK(result.status == resident::RootStatus::Advertised);
  }

  // Query the mmap set.
  const poker::GameDef def = class_root_def(manifest, 0);
  const poker::GameState state(def);
  const std::span<const poker::PublicAction> no_history{};
  resident::ResidentScratch scratch{};
  const resident::ResidentAnswer answer =
      set.public_belief(state, no_history, std::nullopt, scratch);
  CHECK(answer.hit);

  return g_failures == 0;
}

}  // namespace

int main() {
  const fs::path dir = fs::temp_directory_path() / "bs_test_lazy_loading";
  fs::remove_all(dir);
  fs::create_directories(dir);

  bool ok = true;
  ok = test_lazy_loading(dir / "lazy") && ok;
  ok = test_lazy_mmap_bypass(dir / "bypass") && ok;

  fs::remove_all(dir);

  if (!ok || g_failures > 0) {
    std::fprintf(stderr, "lazy_loading: %d failures\n", g_failures);
    return 1;
  }
  std::printf("lazy_loading: all tests passed\n");
  return 0;
}
