// flop_library.cpp — RFC 0009 W4c-iii: offline flop class library builder.
//
// See the header for the contract. This file is offline solver-side tooling:
// it links the solver and artifact libraries but never the service, host, or
// protocol targets. Every measurement in the manifest is counted, never
// estimated.
#include <algorithm>
#include <array>
#include <bs/abstract_tree.hpp>
#include <bs/abstraction.hpp>
#include <bs/equity_frontier.hpp>
#include <bs/eval.hpp>
#include <bs/flop_library.hpp>
#include <bs/game_definition.hpp>
#include <bs/heads_up.hpp>
#include <bs/heads_up_solver.hpp>  // WeightedHand
#include <bs/nseat_trainer.hpp>
#include <bs/seat_policy.hpp>
#include <bs/strategy_artifact.hpp>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <set>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

namespace bs::flop_library {

namespace {

// The canonical class count and concrete flop count, pinned by W4a's golden
// digest (0xaf83bd016a92d2d6) and the test_abstraction enumeration.
constexpr std::size_t kTotalClasses = 1755;
constexpr std::size_t kTotalFlops = 22100;
constexpr poker::Chips kDefaultBigBlind = 2;
const char* kLibraryEngineRevision = "rfc0009-w4ciii-flop-library-v1";

bool overlaps_board(const std::array<int, 2>& combo, const std::array<int, 3>& board) {
  for (int c : combo)
    for (int b : board)
      if (c == b)
        return true;
  return false;
}

// The declared range minus any combo sharing a card with the class board.
// Board-overlapping combos have zero reach (the board card is already dealt),
// so they are dropped rather than carried as dead identity.
std::vector<solver::WeightedHand> filter_range(const std::vector<solver::WeightedHand>& range,
                                               const std::array<int, 3>& board) {
  std::vector<solver::WeightedHand> out;
  out.reserve(range.size());
  for (const auto& h : range) {
    if (!overlaps_board(h.cards, board))
      out.push_back(h);
  }
  return out;
}

poker::GameDef make_def(const std::array<int, 3>& board, poker::Chips stack,
                        poker::Chips contribution, std::size_t player_count) {
  poker::GameDef def{};
  def.player_count = static_cast<std::uint8_t>(player_count);
  def.button = 0;
  def.big_blind = kDefaultBigBlind;
  for (std::size_t s = 0; s < player_count; ++s) {
    def.stacks[s] = stack;
    def.contributions[s] = contribution;
  }
  def.pot = static_cast<poker::Chips>(player_count * contribution);
  def.board = {board[0], board[1], board[2], 0, 0};
  def.board_size = 3;
  // Flop-terminal: the tree stops after flop action. The frontier evaluator
  // supplies exact all-in-at-flop equity, eliminating turn/river chance
  // branching (1,980 runout paths per action line) that dominated the
  // river-terminal artifact size (~597 MB/class → a few MB/class).
  def.terminal = poker::TerminalDepth::Flop;
  return def;
}

std::string class_stem(std::size_t index) {
  char buf[16];
  std::snprintf(buf, sizeof(buf), "class-%04zu", index);
  return buf;
}

// The artifact codebase serializes 64-bit digests as 16-digit lowercase hex
// (see u64_to_hex in the artifacts library); the manifest follows the same
// convention so a double-based JSON parser never mangles a value above 2^53.
std::string digest_hex(std::uint64_t value) {
  char buf[17];
  std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(value));
  return buf;
}

std::string json_escape(const std::string& s) {
  std::string out;
  out.reserve(s.size() + 2);
  for (char c : s) {
    switch (c) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned>(c));
          out += buf;
        } else {
          out += c;
        }
    }
  }
  return out;
}

}  // namespace

std::vector<std::array<int, 3>> enumerate_classes() {
  std::set<std::array<int, 3>> classes;
  for (int a = 0; a < 52; ++a)
    for (int b = a + 1; b < 52; ++b)
      for (int c = b + 1; c < 52; ++c) {
        const std::array<int, 3> board{a, b, c};
        classes.insert(abstraction::canonicalize(board).board);
      }
  return {classes.begin(), classes.end()};
}

std::size_t count_covered_flops(const std::vector<std::array<int, 3>>& covered) {
  const std::set<std::array<int, 3>> covered_set(covered.begin(), covered.end());
  std::size_t count = 0;
  for (int a = 0; a < 52; ++a)
    for (int b = a + 1; b < 52; ++b)
      for (int c = b + 1; c < 52; ++c) {
        const std::array<int, 3> board{a, b, c};
        if (covered_set.count(abstraction::canonicalize(board).board))
          ++count;
      }
  return count;
}

std::vector<solver::WeightedHand> declared_library_range() {
  std::vector<solver::WeightedHand> range;
  range.reserve(40);
  // Four premium pocket pairs: AA, KK, QQ, JJ (6 combos each).
  static const int kPairRanks[] = {12, 11, 10, 9};  // A, K, Q, J
  for (int r : kPairRanks) {
    for (int s0 = 0; s0 < 4; ++s0)
      for (int s1 = s0 + 1; s1 < 4; ++s1) {
        range.push_back({{r * 4 + s0, r * 4 + s1}, 1.0});
      }
  }
  // AKs (4 combos) and AKo (12 combos). Card ids are rank*4+suit, so the K
  // (rank 11) is always the lower card and the A (rank 12) the higher.
  for (int s0 = 0; s0 < 4; ++s0) {
    range.push_back({{11 * 4 + s0, 12 * 4 + s0}, 1.0});  // suited
    for (int s1 = 0; s1 < 4; ++s1) {
      if (s1 != s0)
        range.push_back({{11 * 4 + s1, 12 * 4 + s0}, 1.0});  // offsuit
    }
  }
  return range;
}

LibraryManifest build_library(const std::filesystem::path& out_dir, std::size_t class_count,
                              std::uint64_t iterations_per_class, std::uint64_t seed,
                              poker::Chips stack, poker::Chips contribution,
                              const std::vector<solver::WeightedHand>& range,
                              std::size_t player_count) {
  if (class_count == 0)
    throw std::invalid_argument("class_count must be positive");
  if (iterations_per_class == 0)
    throw std::invalid_argument("iterations must be positive");
  if (range.empty())
    throw std::invalid_argument("range must be non-empty");
  if (stack == 0 || contribution == 0)
    throw std::invalid_argument("stack and contribution must be positive");
  if (player_count < 2 || player_count > 10)
    throw std::invalid_argument("player_count must be in [2, 10]");

  std::error_code ec;
  std::filesystem::create_directories(out_dir, ec);
  if (ec)
    throw std::runtime_error("cannot create output directory: " + ec.message());

  const std::vector<std::array<int, 3>> classes = enumerate_classes();
  if (classes.size() != kTotalClasses)
    throw std::runtime_error("canonical class count drifted from the pinned " +
                             std::to_string(kTotalClasses));
  if (class_count > classes.size())
    throw std::invalid_argument("class_count exceeds total classes (" +
                                std::to_string(classes.size()) + ")");

  const abstraction::AbstractionId card_id = abstraction::suit_canonicalization_id();

  LibraryManifest manifest;
  manifest.card_id = card_id;
  manifest.player_count = player_count;
  manifest.big_blind = kDefaultBigBlind;
  manifest.stack = stack;
  manifest.contribution = contribution;
  manifest.iterations_per_class = iterations_per_class;
  manifest.seed = seed;
  manifest.total_classes = classes.size();
  manifest.total_flops = kTotalFlops;

  std::vector<std::array<int, 3>> covered;
  covered.reserve(class_count);

  try {
    for (std::size_t i = 0; i < class_count; ++i) {
      const std::array<int, 3> board = classes[i];
      const poker::GameDef def = make_def(board, stack, contribution, player_count);
      const std::vector<solver::WeightedHand> seat_range = filter_range(range, board);
      if (seat_range.empty())
        throw std::runtime_error("class " + std::to_string(i) + " has no off-board combos");
      std::vector<std::vector<solver::WeightedHand>> ranges;
      ranges.reserve(player_count);
      for (std::size_t s = 0; s < player_count; ++s)
        ranges.push_back(seat_range);

      const tree::AbstractTree tree(def, abstraction::ActionAbstraction::identity());
      // Flop-terminal games require a frontier evaluator to supply leaf values.
      // The EquityFrontierEvaluator computes exact all-in-at-flop equity by
      // enumerating all 990 turn/river combos.
      const gto::EquityFrontierEvaluator frontier;
      // Offline training caps: the tree's own 1 GiB byte cap is the binding
      // memory bound; the trainer's node/information-set caps are raised well
      // above the defaults so a shallow-but-nontrivial stack profile completes.
      solver::NSeatTrainerLimits limits;
      limits.max_nodes = 10'000'000;
      limits.max_information_sets = 5'000'000;
      limits.wall = std::chrono::minutes{5};
      const solver::NSeatTrainingResult trained =
          solver::train_nseat(tree, ranges, iterations_per_class, seed, limits, &frontier);
      if (trained.termination != solver::NSeatTerminationPhase::Complete)
        throw std::runtime_error("class " + std::to_string(i) + " training did not complete");

      const solver::SeatTrainingResult exported =
          solver::export_seat_policy(trained, tree, ranges, card_id);

      const std::string stem = class_stem(i);
      const std::filesystem::path checkpoint_path = out_dir / (stem + "-checkpoint.db");
      const std::filesystem::path policy_path = out_dir / (stem + ".db");

      artifacts::SeatCheckpointProvenance provenance;
      provenance.engine_revision = kLibraryEngineRevision;
      artifacts::create_checkpoint(checkpoint_path, exported, provenance);
      const artifacts::PublishedPolicy published =
          artifacts::publish_policy(checkpoint_path, policy_path, kLibraryEngineRevision);

      const artifacts::ArtifactProbe probe = artifacts::probe_artifact(policy_path);

      ClassEntry entry;
      entry.canonical_board = board;
      entry.artifact_name = policy_path.filename().string();
      entry.sha256_hex = published.sha256_hex;
      entry.stored_rows = exported.policy.rows().size();
      entry.completed_iterations = trained.completed_iterations;
      entry.information_sets = trained.information_sets;
      manifest.classes.push_back(entry);
      manifest.storage_bytes += published.file_bytes;
      covered.push_back(board);

      // The checkpoint is mutable training state; the immutable published
      // policy is the library artifact. Remove it so the directory holds only
      // policies.
      std::filesystem::remove(checkpoint_path, ec);
    }
  } catch (...) {
    // A failure mid-build leaves no partial library: remove every published
    // class artifact and any checkpoint from the failed class before
    // rethrowing. Without a manifest the directory is not a valid library, but
    // orphaned read-only .db files would still confuse a later rebuild.
    for (const auto& e : manifest.classes) {
      std::filesystem::remove(out_dir / e.artifact_name, ec);
    }
    for (std::size_t i = 0; i < class_count; ++i) {
      std::filesystem::remove(out_dir / (class_stem(i) + "-checkpoint.db"), ec);
    }
    throw;
  }

  manifest.class_count = manifest.classes.size();
  manifest.covered_flops = count_covered_flops(covered);

  write_manifest(manifest, out_dir);
  return manifest;
}

void write_manifest(const LibraryManifest& manifest, const std::filesystem::path& out_dir) {
  const std::filesystem::path path = out_dir / "manifest.json";
  std::ofstream out(path);
  if (!out)
    throw std::runtime_error("cannot open manifest for writing: " + path.string());

  out << "{\n";
  out << "  \"card_abstraction_name\": \"" << json_escape(manifest.card_id.name) << "\",\n";
  out << "  \"card_abstraction_version\": " << manifest.card_id.version << ",\n";
  out << "  \"card_abstraction_parameters\": \"" << json_escape(manifest.card_id.parameters)
      << "\",\n";
  out << "  \"card_abstraction_digest\": \"" << digest_hex(manifest.card_id.digest) << "\",\n";
  out << "  \"player_count\": " << manifest.player_count << ",\n";
  out << "  \"big_blind\": " << manifest.big_blind << ",\n";
  out << "  \"stack\": " << manifest.stack << ",\n";
  out << "  \"contribution\": " << manifest.contribution << ",\n";
  out << "  \"iterations_per_class\": " << manifest.iterations_per_class << ",\n";
  out << "  \"seed\": " << manifest.seed << ",\n";
  out << "  \"classes\": [\n";
  for (std::size_t i = 0; i < manifest.classes.size(); ++i) {
    const ClassEntry& e = manifest.classes[i];
    out << "    {\n";
    out << "      \"canonical_board\": [" << e.canonical_board[0] << ", " << e.canonical_board[1]
        << ", " << e.canonical_board[2] << "],\n";
    out << "      \"artifact_name\": \"" << json_escape(e.artifact_name) << "\",\n";
    out << "      \"sha256_hex\": \"" << e.sha256_hex << "\",\n";
    out << "      \"stored_rows\": " << e.stored_rows << ",\n";
    out << "      \"completed_iterations\": " << e.completed_iterations << ",\n";
    out << "      \"information_sets\": " << e.information_sets << "\n";
    out << "    }" << (i + 1 < manifest.classes.size() ? "," : "") << "\n";
  }
  out << "  ],\n";
  out << "  \"class_count\": " << manifest.class_count << ",\n";
  out << "  \"total_classes\": " << manifest.total_classes << ",\n";
  out << "  \"covered_flops\": " << manifest.covered_flops << ",\n";
  out << "  \"total_flops\": " << manifest.total_flops << ",\n";
  out << "  \"storage_bytes\": " << manifest.storage_bytes << "\n";
  out << "}\n";
  if (!out)
    throw std::runtime_error("failed writing manifest: " + path.string());
}

}  // namespace bs::flop_library
