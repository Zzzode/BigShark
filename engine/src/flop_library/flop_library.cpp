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
#include <map>
#include <set>
#include <sstream>
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

// --- RFC 0012 Phase 4: checkpoint round-trip helpers ------------------------
//
// The trainer keys its raw table by (card bucket, tree node); the artifact
// checkpoint keys training rows by (info_id, ordinal), i.e. one row per
// concrete (actor, own combo, board, public-action path) information state.
// The two helpers below perform the forward expansion (trainer -> checkpoint)
// and the reverse mapping (checkpoint -> trainer) by walking the same tree the
// export path uses. Every concrete combo sharing a (bucket, node) carries
// identical regrets, so the reverse map can pick any one representative combo
// per (bucket, node).

// Forward DFS: at each action node, for every off-board combo, look up the
// raw (bucket, node) row and emit one (regret, average_weight) pair per action
// ordinal under the concrete InformationKey. Mirrors export_dfs in
// seat_policy.cpp, but reads the raw table instead of the sealed policy.
void export_training_dfs(const tree::AbstractTree& tree,
                         const std::map<solver::NSeatInformationKey, solver::NSeatRawRow>& raw_rows,
                         std::map<solver::InformationKey, artifacts::TrainingWeights>& rows,
                         std::size_t node_index, poker::GameState& cursor,
                         std::vector<poker::PublicAction>& path) {
  const tree::TreeNode& node = tree.node(node_index);
  if (node.is_terminal() || node.is_flop_deal())
    return;
  if (node.is_chance()) {
    for (std::size_t ci = 0; ci < node.children.size(); ++ci) {
      const tree::TreeNode& child = tree.node(node.children[ci]);
      poker::GameState next = cursor.after_card(child.incoming_card);
      export_training_dfs(tree, raw_rows, rows, node.children[ci], next, path);
    }
    return;
  }

  const std::size_t seat = node.actor;
  const std::span<const int> board = cursor.board();
  const std::vector<int> board_vec(board.begin(), board.end());
  std::array<bool, 52> on_board{};
  for (int card : board)
    on_board[static_cast<std::size_t>(card)] = true;

  for (int c0 = 0; c0 < 52; ++c0) {
    if (on_board[static_cast<std::size_t>(c0)])
      continue;
    for (int c1 = c0 + 1; c1 < 52; ++c1) {
      if (on_board[static_cast<std::size_t>(c1)])
        continue;
      const std::array<int, 2> cards{c0, c1};
      const std::uint32_t bucket =
          abstraction::card_bucket(solver::kNSeatCardKind, cards, board_vec);
      const solver::NSeatInformationKey nk{bucket, node_index};
      const auto it = raw_rows.find(nk);
      if (it == raw_rows.end())
        continue;  // exact miss: no raw row
      const solver::NSeatRawRow& raw = it->second;
      artifacts::TrainingWeights weights;
      weights.reserve(raw.actions.size());
      for (std::size_t a = 0; a < raw.actions.size(); ++a)
        weights.push_back({raw.regrets[a], raw.sums[a]});
      rows.emplace(solver::make_information_key(seat, cards, board, path), std::move(weights));
    }
  }

  for (std::size_t ai = 0; ai < node.actions.size(); ++ai) {
    const poker::Action& action = node.actions[ai];
    poker::GameState next = cursor.after_action(seat, action);
    path.push_back(poker::PublicAction{node.street, seat, action});
    export_training_dfs(tree, raw_rows, rows, node.children[ai], next, path);
    path.pop_back();
  }
}

// Expand the trainer's raw table into concrete per-combo training rows, ready
// for save_training_checkpoint. (Defined outside the anonymous namespace; the
// DFS helper above stays file-local.)
// std::map<...> export_seat_training_rows(...) — see below the namespace.

// Reverse DFS: at each action node, for every off-board combo, compute the
// concrete InformationKey and look it up in the checkpoint rows. The first
// combo for a (bucket, node) fills the trainer row; later combos are skipped
// because they carry identical regrets.
void build_initial_dfs(const tree::AbstractTree& tree,
                       const artifacts::CheckpointTrainingState& ckpt,
                       solver::NSeatInitialState& state, std::size_t node_index,
                       poker::GameState& cursor, std::vector<poker::PublicAction>& path) {
  const tree::TreeNode& node = tree.node(node_index);
  if (node.is_terminal() || node.is_flop_deal())
    return;
  if (node.is_chance()) {
    for (std::size_t ci = 0; ci < node.children.size(); ++ci) {
      const tree::TreeNode& child = tree.node(node.children[ci]);
      poker::GameState next = cursor.after_card(child.incoming_card);
      build_initial_dfs(tree, ckpt, state, node.children[ci], next, path);
    }
    return;
  }

  const std::size_t seat = node.actor;
  const std::span<const int> board = cursor.board();
  const std::vector<int> board_vec(board.begin(), board.end());
  std::array<bool, 52> on_board{};
  for (int card : board)
    on_board[static_cast<std::size_t>(card)] = true;

  for (int c0 = 0; c0 < 52; ++c0) {
    if (on_board[static_cast<std::size_t>(c0)])
      continue;
    for (int c1 = c0 + 1; c1 < 52; ++c1) {
      if (on_board[static_cast<std::size_t>(c1)])
        continue;
      const std::array<int, 2> cards{c0, c1};
      const solver::InformationKey key = solver::make_information_key(seat, cards, board, path);
      const auto ckpt_it = ckpt.rows.find(key);
      if (ckpt_it == ckpt.rows.end())
        continue;
      const std::uint32_t bucket =
          abstraction::card_bucket(solver::kNSeatCardKind, cards, board_vec);
      const solver::NSeatInformationKey nk{bucket, node_index};
      if (state.rows.contains(nk))
        continue;  // a representative combo already filled this (bucket, node)
      const artifacts::TrainingWeights& weights = ckpt_it->second;
      solver::NSeatRawRow row;
      row.actions = node.actions;
      if (weights.size() != row.actions.size())
        throw std::runtime_error(
            "checkpoint training row length disagrees with the tree action menu");
      row.regrets.resize(weights.size(), 0.0);
      row.sums.resize(weights.size(), 0.0);
      for (std::size_t a = 0; a < weights.size(); ++a) {
        row.regrets[a] = weights[a].first;
        row.sums[a] = weights[a].second;
      }
      state.rows.emplace(nk, std::move(row));
    }
  }

  for (std::size_t ai = 0; ai < node.actions.size(); ++ai) {
    const poker::Action& action = node.actions[ai];
    poker::GameState next = cursor.after_action(seat, action);
    path.push_back(poker::PublicAction{node.street, seat, action});
    build_initial_dfs(tree, ckpt, state, node.children[ai], next, path);
    path.pop_back();
  }
}

// Map a loaded checkpoint's concrete training rows back to the trainer's
// (bucket, node) table. (Defined outside the anonymous namespace below.)
// solver::NSeatInitialState build_initial_state(...) — see below.

// The offline training caps, shared by the serial and parallel paths.
solver::NSeatTrainerLimits trainer_limits() {
  solver::NSeatTrainerLimits limits;
  limits.max_nodes = 10'000'000;
  limits.max_information_sets = 5'000'000;
  limits.wall = std::chrono::minutes{5};
  return limits;
}

// RFC 0012 Phase 4: the checkpoint interval. A warm-start checkpoint is saved
// after every chunk of this many iterations so a crashed build can resume.
constexpr std::uint64_t kCheckpointInterval = 256;

// Refuse incompatible resume files before entering any failure-cleanup scope.
void preflight_resume(const std::filesystem::path& out_dir, std::size_t class_count, bool resume) {
  if (!resume)
    return;
  for (std::size_t index = 0; index < class_count; ++index) {
    const auto path = out_dir / (class_stem(index) + "-checkpoint.db");
    if (!std::filesystem::exists(path))
      continue;
    const auto checkpoint = artifacts::load_checkpoint(path);
    if (checkpoint.algorithm_revision != artifacts::kDefaultAlgorithmRevisionV2)
      throw std::runtime_error(
          "incompatible checkpoint algorithm; refusing resume without modifying " + path.string());
  }
}

// Train one class and publish its artifact. Shared by the serial builder and
// the parallel workers. When `params.resume` is set, warm-starts from an
// existing `class-NNNN-checkpoint.db` (after verifying the abstraction
// digests) and writes a fresh checkpoint every kCheckpointInterval iterations.
// The per-iteration PRNG streams depend on the absolute iteration number, so a
// warm-started run is bitwise-identical to a from-scratch run at the same
// iteration count.
ClassEntry train_one_class(const std::filesystem::path& out_dir, std::size_t class_index,
                           const std::array<int, 3>& board, const BuildParams& params,
                           const abstraction::AbstractionId& card_id) {
  const poker::GameDef def =
      make_def(board, params.stack, params.contribution, params.player_count);
  const std::vector<solver::WeightedHand> seat_range = filter_range(params.range, board);
  if (seat_range.empty())
    throw std::runtime_error("class " + std::to_string(class_index) + " has no off-board combos");
  std::vector<std::vector<solver::WeightedHand>> ranges;
  ranges.reserve(params.player_count);
  for (std::size_t s = 0; s < params.player_count; ++s)
    ranges.push_back(seat_range);

  const tree::AbstractTree tree(def, params.action);
  const gto::EquityFrontierEvaluator frontier;
  const solver::NSeatTrainerLimits limits = trainer_limits();

  const std::string stem = class_stem(class_index);
  const std::filesystem::path checkpoint_path = out_dir / (stem + "-checkpoint.db");
  const std::filesystem::path policy_path = out_dir / (stem + ".db");

  // Warm-start from an existing checkpoint if requested.
  solver::NSeatInitialState state;
  if (params.resume) {
    std::error_code ec;
    if (std::filesystem::exists(checkpoint_path, ec)) {
      const artifacts::CheckpointTrainingState ckpt = artifacts::load_checkpoint(checkpoint_path);
      if (ckpt.abstraction_digest != params.action.id().digest)
        throw std::runtime_error("class " + std::to_string(class_index) +
                                 " checkpoint action-abstraction digest mismatch");
      if (ckpt.card_abstraction != card_id)
        throw std::runtime_error("class " + std::to_string(class_index) +
                                 " checkpoint card-abstraction mismatch");
      state = build_initial_state(ckpt, tree);
      // A checkpoint that already reached the target is stale-but-complete
      // (crash after training, before publish). Drop it and train from scratch
      // so the publish path below has a fresh result to export.
      if (state.completed_iterations >= params.iterations_per_class) {
        std::filesystem::remove(checkpoint_path, ec);
        state = {};
      }
    }
  }

  // Chunk loop: train up to kCheckpointInterval iterations per chunk, saving a
  // warm-start checkpoint after each. The wall budget is the total across all
  // chunks (matching the single-call behavior), so each chunk receives the
  // remaining wall time.
  const auto wall_start = std::chrono::steady_clock::now();
  const auto wall_limit = limits.wall;
  solver::NSeatTrainingResult trained;
  while (state.completed_iterations < params.iterations_per_class) {
    const std::uint64_t remaining_iters = params.iterations_per_class - state.completed_iterations;
    const std::uint64_t chunk = std::min(kCheckpointInterval, remaining_iters);
    const auto elapsed = std::chrono::steady_clock::now() - wall_start;
    const auto remaining_wall =
        std::chrono::duration_cast<std::chrono::milliseconds>(wall_limit - elapsed);
    if (remaining_wall <= std::chrono::milliseconds::zero())
      throw std::runtime_error("class " + std::to_string(class_index) +
                               " exceeded the wall budget before completion");
    solver::NSeatTrainerLimits chunk_limits = limits;
    chunk_limits.wall = remaining_wall;
    trained =
        solver::train_nseat(tree, ranges, chunk, params.seed, chunk_limits, &frontier, &state);
    if (trained.termination != solver::NSeatTerminationPhase::Complete)
      throw std::runtime_error("class " + std::to_string(class_index) +
                               " training did not complete within the wall budget");
    state.completed_iterations = trained.completed_iterations;
    state.algorithm_revision = trained.algorithm_revision;
    state.batch_size = trained.batch_size;

    // Save a warm-start checkpoint for this chunk.
    const solver::SeatTrainingResult exported =
        solver::export_seat_policy(trained, tree, ranges, card_id);
    const auto training_rows = export_seat_training_rows(trained, tree);
    artifacts::SeatCheckpointProvenance provenance;
    provenance.engine_revision = kLibraryEngineRevision;
    artifacts::save_training_checkpoint(checkpoint_path, exported, training_rows, provenance);

    // Hand the raw table back for the next chunk.
    state.rows = std::move(trained.raw_rows);
  }

  std::printf("  class %zu: %zu nodes, %zu info sets, %zu bytes, %llu iterations\n", class_index,
              trained.nodes, trained.information_sets, trained.accounted_bytes,
              static_cast<unsigned long long>(trained.completed_iterations));

  // Final publish. Remove the warm-start checkpoint so create_checkpoint (which
  // refuses to overwrite) can write the policy-only checkpoint.
  std::error_code ec;
  std::filesystem::remove(checkpoint_path, ec);
  const solver::SeatTrainingResult exported =
      solver::export_seat_policy(trained, tree, ranges, card_id);
  artifacts::SeatCheckpointProvenance provenance;
  provenance.engine_revision = kLibraryEngineRevision;
  artifacts::create_checkpoint(checkpoint_path, exported, provenance);
  const artifacts::PublishedPolicy published =
      artifacts::publish_policy(checkpoint_path, policy_path, kLibraryEngineRevision);
  const artifacts::ArtifactProbe probe = artifacts::probe_artifact(policy_path);
  (void)probe;

  ClassEntry entry;
  entry.canonical_board = board;
  entry.artifact_name = policy_path.filename().string();
  entry.sha256_hex = published.sha256_hex;
  entry.stored_rows = exported.policy.rows().size();
  entry.completed_iterations = trained.completed_iterations;
  entry.information_sets = trained.information_sets;

  // The checkpoint is mutable training state; the immutable published policy
  // is the library artifact.
  std::filesystem::remove(checkpoint_path, ec);
  return entry;
}

// --- RFC 0012 Phase 1: manifest fragments -----------------------------------

// One class record in a manifest fragment, carrying the class index so the
// merge can sort without parsing the artifact name.
struct FragmentClass {
  std::size_t index = 0;
  ClassEntry entry;
};

// Write a worker's class records as a JSON fragment.
void write_fragment(const std::filesystem::path& out_dir, std::size_t worker_id,
                    const std::vector<FragmentClass>& classes) {
  const std::filesystem::path path =
      out_dir / ("manifest-part-" + std::to_string(worker_id) + ".json");
  std::ofstream out(path);
  if (!out)
    throw std::runtime_error("cannot open fragment for writing: " + path.string());
  out << "{\n  \"worker_id\": " << worker_id << ",\n  \"classes\": [\n";
  for (std::size_t i = 0; i < classes.size(); ++i) {
    const FragmentClass& fc = classes[i];
    const ClassEntry& e = fc.entry;
    out << "    {\"index\": " << fc.index << ", \"canonical_board\": [" << e.canonical_board[0]
        << ", " << e.canonical_board[1] << ", " << e.canonical_board[2] << "], "
        << "\"artifact_name\": \"" << json_escape(e.artifact_name) << "\", "
        << "\"sha256_hex\": \"" << e.sha256_hex << "\", "
        << "\"stored_rows\": " << e.stored_rows << ", "
        << "\"completed_iterations\": " << e.completed_iterations << ", "
        << "\"information_sets\": " << e.information_sets << "}"
        << (i + 1 < classes.size() ? "," : "") << "\n";
  }
  out << "  ]\n}\n";
  if (!out)
    throw std::runtime_error("failed writing fragment: " + path.string());
}

// A minimal JSON parser for manifest fragments. Handles objects, arrays,
// strings, and integers — the only value kinds a fragment contains.
class FragmentParser {
 public:
  explicit FragmentParser(std::string text) : text_(std::move(text)) {}

  std::vector<FragmentClass> parse() {
    std::vector<FragmentClass> out;
    skip_ws();
    expect('{');
    skip_ws();
    // Parse "worker_id" (skipped) and "classes".
    while (true) {
      skip_ws();
      if (peek() == '}')
        break;
      const std::string key = parse_string();
      skip_ws();
      expect(':');
      skip_ws();
      if (key == "classes") {
        out = parse_classes_array();
      } else {
        skip_value();
      }
      skip_ws();
      if (peek() == ',') {
        ++pos_;
        continue;
      }
      break;
    }
    expect('}');
    return out;
  }

 private:
  std::string text_;
  std::size_t pos_ = 0;

  char peek() const {
    if (pos_ >= text_.size())
      throw std::runtime_error("fragment: unexpected end of input");
    return text_[pos_];
  }
  void expect(char c) {
    if (peek() != c)
      throw std::runtime_error(std::string("fragment: expected '") + c + "'");
    ++pos_;
  }
  void skip_ws() {
    while (pos_ < text_.size() && (text_[pos_] == ' ' || text_[pos_] == '\t' ||
                                   text_[pos_] == '\n' || text_[pos_] == '\r'))
      ++pos_;
  }
  std::string parse_string() {
    expect('"');
    std::string out;
    while (pos_ < text_.size() && text_[pos_] != '"') {
      if (text_[pos_] == '\\') {
        ++pos_;
        if (pos_ >= text_.size())
          throw std::runtime_error("fragment: bad escape");
        out += text_[pos_];
      } else {
        out += text_[pos_];
      }
      ++pos_;
    }
    expect('"');
    return out;
  }
  std::int64_t parse_int() {
    std::size_t start = pos_;
    if (peek() == '-')
      ++pos_;
    while (pos_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[pos_])))
      ++pos_;
    if (pos_ == start || (pos_ == start + 1 && text_[start] == '-'))
      throw std::runtime_error("fragment: expected integer");
    return std::stoll(text_.substr(start, pos_ - start));
  }
  void skip_value() {
    if (peek() == '"') {
      parse_string();
    } else if (peek() == '{' || peek() == '[') {
      const char open = peek();
      const char close = (open == '{') ? '}' : ']';
      int depth = 0;
      do {
        if (peek() == '"')
          parse_string();
        else {
          if (peek() == open)
            ++depth;
          else if (peek() == close)
            --depth;
          ++pos_;
        }
      } while (depth > 0);
    } else {
      while (pos_ < text_.size() && text_[pos_] != ',' && text_[pos_] != '}' && text_[pos_] != ']')
        ++pos_;
    }
  }
  std::vector<FragmentClass> parse_classes_array() {
    std::vector<FragmentClass> out;
    expect('[');
    skip_ws();
    if (peek() == ']') {
      ++pos_;
      return out;
    }
    while (true) {
      skip_ws();
      out.push_back(parse_class_object());
      skip_ws();
      if (peek() == ',') {
        ++pos_;
        continue;
      }
      break;
    }
    expect(']');
    return out;
  }
  FragmentClass parse_class_object() {
    expect('{');
    FragmentClass fc;
    ClassEntry& e = fc.entry;
    while (true) {
      skip_ws();
      if (peek() == '}')
        break;
      const std::string key = parse_string();
      skip_ws();
      expect(':');
      skip_ws();
      if (key == "index") {
        fc.index = static_cast<std::size_t>(parse_int());
      } else if (key == "canonical_board") {
        expect('[');
        for (int& card : e.canonical_board) {
          skip_ws();
          card = static_cast<int>(parse_int());
          skip_ws();
          if (peek() == ',')
            ++pos_;
        }
        expect(']');
      } else if (key == "artifact_name") {
        e.artifact_name = parse_string();
      } else if (key == "sha256_hex") {
        e.sha256_hex = parse_string();
      } else if (key == "stored_rows") {
        e.stored_rows = static_cast<std::size_t>(parse_int());
      } else if (key == "completed_iterations") {
        e.completed_iterations = static_cast<std::uint64_t>(parse_int());
      } else if (key == "information_sets") {
        e.information_sets = static_cast<std::size_t>(parse_int());
      } else {
        skip_value();
      }
      skip_ws();
      if (peek() == ',') {
        ++pos_;
        continue;
      }
      break;
    }
    expect('}');
    return fc;
  }
};

std::vector<FragmentClass> read_fragment(const std::filesystem::path& path) {
  std::ifstream in(path);
  if (!in)
    throw std::runtime_error("cannot open fragment: " + path.string());
  std::ostringstream ss;
  ss << in.rdbuf();
  FragmentParser parser(ss.str());
  return parser.parse();
}

}  // namespace

std::map<solver::InformationKey, artifacts::TrainingWeights> export_seat_training_rows(
    const solver::NSeatTrainingResult& trained, const tree::AbstractTree& tree) {
  std::map<solver::InformationKey, artifacts::TrainingWeights> rows;
  poker::GameState cursor{tree.def()};
  std::vector<poker::PublicAction> path;
  export_training_dfs(tree, trained.raw_rows, rows, tree.root_index(), cursor, path);
  return rows;
}

solver::NSeatInitialState build_initial_state(const artifacts::CheckpointTrainingState& ckpt,
                                              const tree::AbstractTree& tree) {
  if (ckpt.algorithm_revision != artifacts::kDefaultAlgorithmRevisionV2)
    throw std::invalid_argument("checkpoint requires revision-two batch-one identity for resume");
  solver::NSeatInitialState state;
  state.algorithm_revision = solver::kNSeatAlgorithmRevision;
  state.batch_size = 1;
  state.completed_iterations = ckpt.completed_iterations;
  poker::GameState cursor{tree.def()};
  std::vector<poker::PublicAction> path;
  build_initial_dfs(tree, ckpt, state, tree.root_index(), cursor, path);
  if (state.rows.empty())
    throw std::runtime_error("checkpoint training rows matched no tree node");
  return state;
}

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
                              std::size_t player_count, abstraction::ActionAbstraction action,
                              bool resume) {
  BuildParams params;
  params.class_count = class_count;
  params.iterations_per_class = iterations_per_class;
  params.seed = seed;
  params.stack = stack;
  params.contribution = contribution;
  params.range = range;
  params.player_count = player_count;
  params.action = action;
  params.resume = resume;

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

  preflight_resume(out_dir, class_count, resume);

  const abstraction::AbstractionId card_id = abstraction::suit_canonicalization_id();

  LibraryManifest manifest;
  manifest.card_id = card_id;
  manifest.action_id = action.id();
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
      ClassEntry entry = train_one_class(out_dir, i, classes[i], params, card_id);
      manifest.classes.push_back(entry);
      manifest.storage_bytes += [&] {
        std::error_code e2;
        const auto sz = std::filesystem::file_size(out_dir / entry.artifact_name, e2);
        if (e2)
          throw std::runtime_error("cannot stat published artifact: " + e2.message());
        return sz;
      }();
      covered.push_back(classes[i]);
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

void build_library_worker(const std::filesystem::path& out_dir, std::size_t worker_id,
                          std::size_t parallel, const BuildParams& params) {
  if (parallel == 0)
    throw std::invalid_argument("parallel must be positive");
  if (worker_id >= parallel)
    throw std::invalid_argument("worker_id must be < parallel");
  if (params.class_count == 0)
    throw std::invalid_argument("class_count must be positive");
  if (params.iterations_per_class == 0)
    throw std::invalid_argument("iterations must be positive");
  if (params.range.empty())
    throw std::invalid_argument("range must be non-empty");
  if (params.stack == 0 || params.contribution == 0)
    throw std::invalid_argument("stack and contribution must be positive");
  if (params.player_count < 2 || params.player_count > 10)
    throw std::invalid_argument("player_count must be in [2, 10]");

  std::error_code ec;
  std::filesystem::create_directories(out_dir, ec);
  if (ec)
    throw std::runtime_error("cannot create output directory: " + ec.message());

  const std::vector<std::array<int, 3>> classes = enumerate_classes();
  if (params.class_count > classes.size())
    throw std::invalid_argument("class_count exceeds total classes");
  preflight_resume(out_dir, params.class_count, params.resume);
  const abstraction::AbstractionId card_id = abstraction::suit_canonicalization_id();

  std::vector<FragmentClass> fragment;
  for (std::size_t i = worker_id; i < params.class_count; i += parallel) {
    ClassEntry entry = train_one_class(out_dir, i, classes[i], params, card_id);
    fragment.push_back({i, entry});
  }
  write_fragment(out_dir, worker_id, fragment);
}

LibraryManifest merge_fragments(const std::filesystem::path& out_dir, const BuildParams& params) {
  const std::vector<std::array<int, 3>> classes = enumerate_classes();
  if (params.class_count > classes.size())
    throw std::invalid_argument("class_count exceeds total classes");
  const abstraction::AbstractionId card_id = abstraction::suit_canonicalization_id();

  // Collect every worker's fragment.
  std::vector<FragmentClass> all;
  for (std::size_t w = 0;; ++w) {
    const std::filesystem::path fragment_path =
        out_dir / ("manifest-part-" + std::to_string(w) + ".json");
    std::error_code ec;
    if (!std::filesystem::exists(fragment_path, ec))
      break;  // workers are numbered 0..N-1 contiguously
    auto part = read_fragment(fragment_path);
    all.insert(all.end(), std::make_move_iterator(part.begin()),
               std::make_move_iterator(part.end()));
  }
  if (all.size() != params.class_count)
    throw std::runtime_error("merge: expected " + std::to_string(params.class_count) +
                             " class records across fragments, found " +
                             std::to_string(all.size()));

  // Sort by class index and verify contiguity.
  std::sort(all.begin(), all.end(),
            [](const FragmentClass& a, const FragmentClass& b) { return a.index < b.index; });
  for (std::size_t i = 0; i < all.size(); ++i) {
    if (all[i].index != i)
      throw std::runtime_error("merge: fragment class indices are not contiguous from zero");
  }

  LibraryManifest manifest;
  manifest.card_id = card_id;
  manifest.action_id = params.action.id();
  manifest.player_count = params.player_count;
  manifest.big_blind = kDefaultBigBlind;
  manifest.stack = params.stack;
  manifest.contribution = params.contribution;
  manifest.iterations_per_class = params.iterations_per_class;
  manifest.seed = params.seed;
  manifest.total_classes = classes.size();
  manifest.total_flops = kTotalFlops;

  std::vector<std::array<int, 3>> covered;
  covered.reserve(all.size());
  for (const FragmentClass& fc : all) {
    const std::filesystem::path artifact_path = out_dir / fc.entry.artifact_name;
    std::error_code ec;
    if (!std::filesystem::exists(artifact_path, ec))
      throw std::runtime_error("merge: missing class artifact " + fc.entry.artifact_name);
    manifest.classes.push_back(fc.entry);
    manifest.storage_bytes += std::filesystem::file_size(artifact_path, ec);
    if (ec)
      throw std::runtime_error("merge: cannot stat " + fc.entry.artifact_name + ": " +
                               ec.message());
    covered.push_back(fc.entry.canonical_board);
  }

  manifest.class_count = manifest.classes.size();
  manifest.covered_flops = count_covered_flops(covered);

  write_manifest(manifest, out_dir);

  // Remove the fragments now that the manifest is written.
  std::error_code ec;
  for (std::size_t w = 0;; ++w) {
    const std::filesystem::path fragment_path =
        out_dir / ("manifest-part-" + std::to_string(w) + ".json");
    if (!std::filesystem::exists(fragment_path, ec))
      break;
    std::filesystem::remove(fragment_path, ec);
  }
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
  out << "  \"action_abstraction_name\": \"" << json_escape(manifest.action_id.name) << "\",\n";
  out << "  \"action_abstraction_version\": " << manifest.action_id.version << ",\n";
  out << "  \"action_abstraction_parameters\": \"" << json_escape(manifest.action_id.parameters)
      << "\",\n";
  out << "  \"action_abstraction_digest\": \"" << digest_hex(manifest.action_id.digest) << "\",\n";
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
