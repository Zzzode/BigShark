// flop_library.cpp — RFC 0009 W4c-iii: offline flop class library builder.
//
// See the header for the contract. This file is offline solver-side tooling:
// it links the solver and artifact libraries but never the service, host, or
// protocol targets. Every measurement in the manifest is counted, never
// estimated.
#include <fcntl.h>
#include <unistd.h>

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
#include <bs/range.hpp>  // comboIndex
#include <bs/sealed_checkpoint.hpp>
#include <bs/seat_policy.hpp>
#include <bs/strategy_artifact.hpp>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
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

// RFC 0035: the sole root constructor. A root declares its exact ledger
// (pot, per-seat chips behind, net contributions, folded mask); this copies
// it onto the class game def and pins the flop board and flop-terminal depth.
// The ledger itself is constructed by the declarer (make_limped_root_spec for
// the production single-root shape, or an explicit multi-root table), which
// is where RFC 0033's buy-in derivation lives.
poker::GameDef make_def(const RootSpec& root, const std::array<int, 3>& board,
                        std::size_t player_count) {
  poker::GameDef def{};
  def.player_count = static_cast<std::uint8_t>(player_count);
  def.button = 0;
  def.big_blind = kDefaultBigBlind;
  for (std::size_t s = 0; s < player_count; ++s) {
    def.stacks[s] = static_cast<poker::Chips>(root.stacks[s]);
    def.contributions[s] = static_cast<poker::Chips>(root.contributions[s]);
    def.folded[s] = root.folded[s] != 0;
  }
  def.pot = static_cast<poker::Chips>(root.pot);
  def.board = {board[0], board[1], board[2], 0, 0};
  def.board_size = 3;
  // Flop-terminal: the tree stops after flop action. The frontier evaluator
  // supplies exact all-in-at-flop equity, eliminating turn/river chance
  // branching (1,980 runout paths per action line) that dominated the
  // river-terminal artifact size (~597 MB/class → a few MB/class).
  def.terminal = poker::TerminalDepth::Flop;
  return def;
}

// The declared root table for a build: params.roots verbatim when explicit,
// otherwise the single limped root derived from the positional arguments.
// Assigns the table-position root ids and validates the table the same way
// the assembler/reader will, so a producer bug fails before any training.
std::vector<RootSpec> effective_roots(const BuildParams& params) {
  std::vector<RootSpec> roots;
  if (params.roots.empty()) {
    roots.push_back(
        make_limped_root_spec(params.player_count, params.starting_stack, params.contribution));
  } else {
    roots = params.roots;
    for (std::size_t r = 0; r < roots.size(); ++r)
      roots[r].root_id = static_cast<std::uint32_t>(r);
  }
  if (roots.empty())
    throw std::invalid_argument("build: at least one root is required");
  for (const RootSpec& root : roots) {
    if (root.range_profile != resident::RootRangeProfile::LiveUniform)
      throw std::invalid_argument(
          "build: reserved range_profile 2 (line-conditioned) is not representable in BSFLIB03 "
          "(one range vector per section)");
    if (static_cast<std::uint8_t>(root.line_tag) < 1 ||
        static_cast<std::uint8_t>(root.line_tag) > 3)
      throw std::invalid_argument("build: root has an invalid line_tag");
    std::uint64_t contributed = 0;
    std::uint8_t live = 0;
    for (std::size_t s = 0; s < params.player_count; ++s) {
      contributed += root.contributions[s];
      if (root.folded[s]) {
        if (root.stacks[s] == 0)
          throw std::invalid_argument(
              "build: a folded seat must keep chips behind (a zero stack is all-in)");
      } else {
        ++live;
      }
    }
    if (live < 2)
      throw std::invalid_argument("build: a root needs at least two live seats");
    if (root.live_count != 0 && root.live_count != live)
      throw std::invalid_argument("build: root live_count disagrees with its folded mask");
    if (root.pot != contributed)
      throw std::invalid_argument("build: root pot must equal the sum of contributions");
    for (std::size_t s = params.player_count; s < 10; ++s)
      if (root.stacks[s] != 0 || root.contributions[s] != 0 || root.folded[s] != 0)
        throw std::invalid_argument("build: root carries ledger bytes at or beyond player_count");
  }
  for (std::size_t i = 0; i < roots.size(); ++i)
    for (std::size_t j = 0; j < i; ++j) {
      bool same = roots[i].pot == roots[j].pot;
      for (std::size_t s = 0; same && s < 10; ++s)
        same = roots[i].stacks[s] == roots[j].stacks[s] &&
               roots[i].contributions[s] == roots[j].contributions[s] &&
               roots[i].folded[s] == roots[j].folded[s];
      if (same)
        throw std::invalid_argument(
            "build: two declared roots share one ledger; an ambiguous library is not built");
    }
  return roots;
}

std::string pair_stem(std::uint32_t root_id, std::size_t class_index) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "root-%04u-class-%04zu", root_id, class_index);
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

// RFC 0027: per-class section staging directory under the output dir.
constexpr char kSectionsDir[] = ".sections";

// Write `bytes` to `path` durably: write a sibling `.tmp`, fsync it, rename.
void write_file_atomically(const std::filesystem::path& path,
                           const std::vector<std::uint8_t>& bytes) {
  const std::filesystem::path temp = path.string() + ".tmp";
  {
    std::ofstream out(temp, std::ios::binary | std::ios::trunc);
    if (!out)
      throw std::runtime_error("cannot open " + temp.string() + " for write");
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
    if (!out)
      throw std::runtime_error("failed writing " + temp.string());
    out.flush();
    if (!out)
      throw std::runtime_error("failed flushing " + temp.string());
  }
  const int fd = ::open(temp.string().c_str(), O_RDONLY);
  if (fd < 0)
    throw std::runtime_error("cannot open " + temp.string() + " for fsync");
  if (::fsync(fd) != 0) {
    ::close(fd);
    throw std::runtime_error("fsync failed for " + temp.string());
  }
  ::close(fd);
  std::filesystem::rename(temp, path);
}

// One completed pair as recorded by a worker: the root and class index plus
// the counters the parent cannot cheaply re-derive from a section header.
struct FragmentClass {
  std::uint32_t root_id = 0;
  std::size_t index = 0;
  ClassEntry entry;
};

// Append one JSON line to an owner's fragment. Each worker (and the serial
// builder) owns its own file, so no locking is needed.
void append_fragment_line(const std::filesystem::path& sections_dir, const std::string& tag,
                          const FragmentClass& fc);

// The shared game identity recorded in the packed library header. RFC 0035:
// the header carries only the file-level constants (player count, big blind,
// button, flop-terminal depth, the fraction schedule, and the full RFC 0030
// action identity); the per-root pot/stacks/contributions/folded live in the
// directory. The first class board is irrelevant to the header, so no board
// is required to construct it.
solver::UnifiedGame make_unified_game(const BuildParams& params) {
  solver::UnifiedGame game;
  game.def.player_count = static_cast<std::uint8_t>(params.player_count);
  game.def.button = 0;
  game.def.big_blind = kDefaultBigBlind;
  game.def.pot = 0;
  game.def.board = {0, 0, 0, 0, 0};
  game.def.board_size = 0;
  game.def.terminal = poker::TerminalDepth::Flop;
  game.sizes = params.action.schedule();
  game.action_id = params.action.id();
  return game;
}

std::vector<std::array<int, 3>> covered_boards(const std::vector<ClassEntry>& classes) {
  std::vector<std::array<int, 3>> boards;
  boards.reserve(classes.size());
  for (const ClassEntry& entry : classes)
    boards.push_back(entry.canonical_board);
  return boards;
}

// Refuse incompatible resume files before entering any failure-cleanup scope.
// A checkpoint left torn by a SIGKILL/crash mid-write (bad SQLite header,
// failed integrity check, or stale rollback journal) is not a usable warm
// start: remove it together with its journal so the class trains from
// scratch rather than aborting the whole resume. A structurally valid
// checkpoint with an incompatible algorithm revision is still a hard error.

// Exact search for `needed` pairwise vertex-disjoint edges in a small graph
// (at most 40 range combos over the 52-card pool; the library asks for at most
// 10 seats). A greedy packing is NOT exact on arbitrary graphs (it underpacks
// K2,2-like structures), and a general Edmonds blossom is needless at this
// size, so a bounded DFS assigns one edge per seat and prunes on card
// conflicts. The first solution returns immediately on every dense feasible
// board; an infeasible board exhausts the small search in microseconds.
bool find_disjoint_edges(const std::vector<std::array<int, 2>>& edges, std::size_t seat,
                         std::size_t needed, std::array<bool, 52>& used) {
  if (seat == needed)
    return true;
  for (const std::array<int, 2>& edge : edges) {
    if (used[edge[0]] || used[edge[1]])
      continue;
    used[edge[0]] = true;
    used[edge[1]] = true;
    if (find_disjoint_edges(edges, seat + 1, needed, used))
      return true;
    used[edge[0]] = false;
    used[edge[1]] = false;
  }
  return false;
}

// Geometric feasibility: can `seat_count` seats each be dealt a distinct
// two-card combo from the board-filtered range with no shared card? A greedy
// packing is not exact on arbitrary range graphs (it underpacks K2,2-like
// structures), so this runs the exact bounded DFS above.
//
// Returns false only when the board leaves too few usable cards for
// `seat_count` disjoint combos. Such a class has no legal joint-deal stream
// under ANY sampler and is excluded from the library rather than crashing the
// build (e.g. KsQsJs against a J+ range removes three of the sixteen range
// cards and no six-seat deal exists).
bool class_is_dealable_impl(const std::array<int, 3>& board, std::size_t seat_count,
                            const std::vector<solver::WeightedHand>& range) {
  std::array<bool, 52> on_board{};
  for (int c : board)
    on_board[c] = true;
  std::vector<std::array<int, 2>> edges;
  edges.reserve(range.size());
  for (const solver::WeightedHand& h : range) {
    if (h.weight == 0.0)
      continue;
    if (on_board[h.cards[0]] || on_board[h.cards[1]])
      continue;
    edges.push_back(h.cards);
  }
  // Cardinality short-circuit: 2*seat_count distinct usable cards are needed.
  std::array<bool, 52> usable{};
  std::size_t usable_cards = 0;
  for (const std::array<int, 2>& e : edges) {
    if (!usable[e[0]]) {
      usable[e[0]] = true;
      ++usable_cards;
    }
    if (!usable[e[1]]) {
      usable[e[1]] = true;
      ++usable_cards;
    }
  }
  if (usable_cards < 2 * seat_count)
    return false;
  std::array<bool, 52> used{};
  return find_disjoint_edges(edges, 0, seat_count, used);
}

// Train one (root, class) pair and serialize its BSFLIB03 section. Shared by
// the serial builder and the parallel workers. When `params.resume` is set,
// warm-starts from the pair's `root-NNNN-class-NNNN-checkpoint.bswarm` after
// verifying the abstraction digests, the RFC 0030 tree-shape witness, and the
// RFC 0035 root-table hash, and writes a fresh checkpoint every
// kCheckpointInterval iterations. The per-iteration PRNG streams depend on
// the absolute iteration number, so a warm-started run is bitwise-identical
// to a from-scratch run at the same iteration count. RFC 0027: the exported
// rows go straight to the shared serializer; no per-class SQLite policy
// artifact is written.
ClassEntry train_one_class(const std::filesystem::path& out_dir, const RootSpec& root,
                           std::size_t class_index, const std::array<int, 3>& board,
                           const BuildParams& params, const abstraction::AbstractionId& card_id,
                           std::uint64_t root_hash, const std::string& tag) {
  const std::uint32_t root_id = root.root_id;
  const poker::GameDef def = make_def(root, board, params.player_count);
  const std::vector<solver::WeightedHand> seat_range = filter_range(params.range, board);
  if (seat_range.empty())
    throw std::runtime_error("class " + std::to_string(class_index) + " has no off-board combos");
  // Live-uniform (the only phase-1 profile): every seat, folded seats
  // included, gets the board-filtered range. The joint dealer draws the
  // folded seats' cards from this range purely for blockage.
  std::vector<std::vector<solver::WeightedHand>> ranges;
  ranges.reserve(params.player_count);
  for (std::size_t s = 0; s < params.player_count; ++s)
    ranges.push_back(seat_range);

  const tree::AbstractTree tree(def, params.action);
  const gto::EquityFrontierEvaluator frontier;
  const solver::NSeatTrainerLimits limits = trainer_limits();

  const std::string stem = pair_stem(root_id, class_index);
  const std::filesystem::path checkpoint_path = out_dir / (stem + "-checkpoint.bswarm");
  // The sealed checkpoint declares the bounds it was written under so a file
  // from a different tree shape fails closed on load rather than landing rows
  // on the wrong nodes.
  const std::uint32_t node_cap = static_cast<std::uint32_t>(tree.nodes().size());
  const std::uint32_t bucket_cap = 169;  // Preflop169: buckets 0..168
  // RFC 0030: the BSWARM02 whole-tree witness. Computed once per class tree;
  // a resume recomputes this and hard-refuses any mismatch before adopting
  // checkpoint rows.
  const std::uint64_t tree_witness = tree::tree_shape_digest(tree);

  // Warm-start from an existing checkpoint if requested.
  solver::NSeatInitialState state;
  if (params.resume) {
    std::error_code ec;
    if (std::filesystem::exists(checkpoint_path, ec)) {
      const artifacts::SealedCheckpointState ckpt =
          artifacts::load_sealed_checkpoint(checkpoint_path);
      if (ckpt.abstraction_digest != params.action.id().digest)
        throw std::runtime_error("class " + std::to_string(class_index) +
                                 " checkpoint action-abstraction digest mismatch");
      if (ckpt.card_abstraction_digest != card_id.digest)
        throw std::runtime_error("class " + std::to_string(class_index) +
                                 " checkpoint card-abstraction mismatch");
      // RFC 0031: the bucket scheme is a build identity, not only a digest.
      // Rows keyed under a different CardBucketKind cannot warm this tree.
      if (static_cast<std::uint8_t>(ckpt.key_kind) !=
          static_cast<std::uint8_t>(solver::kNSeatCardKind))
        throw std::runtime_error("class " + std::to_string(class_index) +
                                 " checkpoint card bucket kind mismatch");
      // RFC 0030: the tree-shape witness. This is the ONLY check that proves a
      // checkpoint row's node ordinal still names the same node (digests,
      // revision, and node/bucket caps cannot). Hard-refuse and preserve the
      // file; it precedes the stale-but-complete branch.
      if (!ckpt.tree_shape_digest.has_value() || *ckpt.tree_shape_digest != tree_witness)
        throw std::runtime_error(
            "class " + std::to_string(class_index) +
            " checkpoint tree shape does not match the current tree (expected " +
            [&] {
              char buf[19];
              std::snprintf(buf, sizeof(buf), "%016llx",
                            static_cast<unsigned long long>(tree_witness));
              return std::string(buf);
            }() +
            ", found " +
            [&] {
              if (!ckpt.tree_shape_digest.has_value())
                return std::string("none");
              char buf[19];
              std::snprintf(buf, sizeof(buf), "%016llx",
                            static_cast<unsigned long long>(*ckpt.tree_shape_digest));
              return std::string(buf);
            }() +
            "); refusing resume; remove the stale checkpoint or resume a build "
            "with matching parameters");
      // RFC 0035: the declared root table witness. The tree-shape witness
      // guards roots whose trees differ; this names the root SET by its
      // ordered ledger table, so a checkpoint staged under another table is
      // refused by name even when the per-pair tree shape agrees.
      if (ckpt.root_table_hash != root_hash)
        throw std::runtime_error(
            "root " + std::to_string(root_id) + " class " + std::to_string(class_index) +
            " checkpoint root table hash does not match the declared root table (expected " +
            digest_hex(root_hash) + ", found " + digest_hex(ckpt.root_table_hash) + ")");
      state.rows = ckpt.rows;
      state.completed_iterations = ckpt.completed_iterations;
      state.algorithm_revision = ckpt.algorithm_revision_id;
      state.batch_size = static_cast<std::size_t>(ckpt.batch_size);
      if (state.rows.empty())
        throw std::runtime_error("class " + std::to_string(class_index) +
                                 " checkpoint carries no training rows");
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

    // Save a warm-start checkpoint for this chunk. RFC 0028: this is a direct
    // serialization of the trainer's own (bucket, node) table. The former
    // per-chunk export_seat_policy call fed only the SQLite writer and did the
    // same full combo expansion, so it is gone; the final export at class
    // completion below is the one that produces the section.
    artifacts::SealedCheckpointState checkpoint;
    checkpoint.algorithm_revision_id = trained.algorithm_revision;
    const char* revision_string = artifacts::sealed_revision_string(trained.algorithm_revision);
    if (revision_string == nullptr)
      throw std::runtime_error("class " + std::to_string(class_index) +
                               " produced an unknown trainer algorithm revision");
    checkpoint.algorithm_revision = revision_string;
    checkpoint.abstraction_digest = params.action.id().digest;
    checkpoint.card_abstraction_digest = card_id.digest;
    checkpoint.completed_iterations = trained.completed_iterations;
    checkpoint.batch_size = trained.batch_size;
    checkpoint.completed_epochs = trained.completed_epochs;
    checkpoint.node_cap = node_cap;
    checkpoint.bucket_cap = bucket_cap;
    checkpoint.key_kind = solver::kNSeatCardKind;
    checkpoint.tree_shape_digest = tree_witness;
    checkpoint.root_table_hash = root_hash;
    checkpoint.rows = trained.raw_rows;
    artifacts::save_sealed_checkpoint(checkpoint_path, checkpoint);

    // Hand the raw table back for the next chunk.
    state.rows = std::move(trained.raw_rows);
  }

  std::printf("  root %u class %zu: %zu nodes, %zu info sets, %zu bytes, %llu iterations\n",
              root_id, class_index, trained.nodes, trained.information_sets,
              trained.accounted_bytes,
              static_cast<unsigned long long>(trained.completed_iterations));

  // Final export and section serialization. The warm-start checkpoint is
  // mutable training state and is removed once the class is complete; the
  // durable completion evidence is the section file the caller writes.
  std::error_code ec;
  std::filesystem::remove(checkpoint_path, ec);
  const solver::SeatTrainingResult exported =
      solver::export_seat_policy(trained, tree, ranges, card_id);

  resident::PackClassInput input;
  input.root_id = root_id;
  input.class_id = static_cast<std::uint32_t>(class_index);
  input.canonical_board = board;
  input.rows = &exported.policy.rows();
  input.range = &seat_range;
  input.action_abstraction_digest = exported.policy.action_id().digest;
  input.card_abstraction_digest = card_id.digest;
  const std::vector<std::uint8_t> section = resident::serialize_class_section(input, params.layout);

  ClassEntry entry;
  entry.root_id = root_id;
  entry.canonical_board = board;
  entry.stored_rows = exported.policy.rows().size();
  entry.completed_iterations = trained.completed_iterations;
  entry.information_sets = trained.information_sets;
  entry.section_bytes = section.size();

  // Write the section as a durable, atomically published file. `.tmp` +
  // fsync + rename means a crash leaves either the old state or the complete
  // section, never a torn one.
  const std::filesystem::path sections_dir = out_dir / kSectionsDir;
  std::filesystem::create_directories(sections_dir, ec);
  if (ec)
    throw std::runtime_error("cannot create sections directory: " + ec.message());
  const std::filesystem::path section_path =
      sections_dir /
      resident::root_class_section_name(root_id, static_cast<std::uint32_t>(class_index));
  write_file_atomically(section_path, section);
  // Durable completion evidence: the section (S1) and its fragment record
  // (S2). The record is appended by the single process that owns this class.
  append_fragment_line(sections_dir, tag, FragmentClass{root_id, class_index, entry});
  return entry;
}

// True when this (root, class) pair already has a durable section from an
// earlier attempt.
bool class_section_exists(const std::filesystem::path& out_dir, std::uint32_t root_id,
                          std::size_t class_index) {
  std::error_code ec;
  return std::filesystem::exists(
      out_dir / kSectionsDir /
          resident::root_class_section_name(root_id, static_cast<std::uint32_t>(class_index)),
      ec);
}

// --- RFC 0027: build identity and worker fragments -------------------------

// The recorded build identity. Written before any worker starts and checked on
// resume, because a section does not carry the codebook layout kind and the
// reader does not bounds-check codebook indices. Any difference fails closed.
struct BuildIdentity {
  std::string codebook;
  std::uint8_t card_bucket_kind = 0;  // RFC 0031: abstraction::CardBucketKind ordinal
  std::uint64_t card_abstraction_digest = 0;
  std::uint64_t action_abstraction_digest = 0;
  std::string range;
  std::size_t range_combo_count = 0;
  std::uint64_t range_digest = 0;
  std::size_t player_count = 0;
  poker::Chips big_blind = 0;
  // RFC 0033: the CLI argument, which is the starting stack (chips behind plus
  // the contribution). Distinct from LibraryManifest::stack, which is chips
  // behind at the root. The rename makes an old build.json fail closed on the
  // missing key rather than be compared under a changed meaning.
  poker::Chips starting_stack = 0;
  poker::Chips contribution = 0;
  std::uint8_t max_raises_per_street = 0;
  std::uint64_t iterations_per_class = 0;
  std::uint64_t seed = 0;
  std::size_t class_count = 0;
  std::size_t total_classes = 0;
  // RFC 0035: the ordered declared root table. Two runs with different root
  // sets can never share sections or checkpoints.
  std::uint64_t root_table_hash = 0;
  std::size_t root_count = 0;
};

std::string codebook_name(resident::ProbabilityLayout layout) {
  switch (layout) {
    case resident::ProbabilityLayout::Codebook16:
      return "codebook16";
    case resident::ProbabilityLayout::Doubles:
      return "doubles";
    case resident::ProbabilityLayout::Codebook8:
      return "codebook8";
  }
  return "codebook16";
}

BuildIdentity make_identity(const BuildParams& params, const abstraction::AbstractionId& card_id,
                            std::span<const RootSpec> roots) {
  BuildIdentity id;
  id.codebook = codebook_name(params.layout);
  id.card_bucket_kind = static_cast<std::uint8_t>(solver::kNSeatCardKind);
  id.card_abstraction_digest = card_id.digest;
  id.action_abstraction_digest = params.action.id().digest;
  id.range = params.range_name;
  id.range_combo_count = params.range.size();
  id.range_digest = range_digest(params.range);
  id.player_count = params.player_count;
  id.big_blind = kDefaultBigBlind;
  id.starting_stack = params.starting_stack;
  id.contribution = params.contribution;
  id.max_raises_per_street = params.action.max_raises_per_street();
  id.iterations_per_class = params.iterations_per_class;
  id.seed = params.seed;
  id.class_count = params.class_count;
  id.total_classes = kTotalClasses;
  id.root_table_hash = flop_library::root_table_hash(params.player_count, roots);
  id.root_count = roots.size();
  return id;
}

std::string identity_json(const BuildIdentity& id) {
  std::ostringstream out;
  out << "{\n";
  out << "  \"codebook\": \"" << json_escape(id.codebook) << "\",\n";
  out << "  \"card_bucket_kind\": " << static_cast<int>(id.card_bucket_kind) << ",\n";
  out << "  \"card_abstraction_digest\": \"" << digest_hex(id.card_abstraction_digest) << "\",\n";
  out << "  \"action_abstraction_digest\": \"" << digest_hex(id.action_abstraction_digest)
      << "\",\n";
  out << "  \"range\": \"" << json_escape(id.range) << "\",\n";
  out << "  \"range_combo_count\": " << id.range_combo_count << ",\n";
  out << "  \"range_digest\": \"" << digest_hex(id.range_digest) << "\",\n";
  out << "  \"player_count\": " << id.player_count << ",\n";
  out << "  \"big_blind\": " << id.big_blind << ",\n";
  out << "  \"starting_stack\": " << id.starting_stack << ",\n";
  out << "  \"contribution\": " << id.contribution << ",\n";
  out << "  \"max_raises_per_street\": " << static_cast<int>(id.max_raises_per_street) << ",\n";
  out << "  \"iterations_per_class\": " << id.iterations_per_class << ",\n";
  out << "  \"seed\": " << id.seed << ",\n";
  out << "  \"class_count\": " << id.class_count << ",\n";
  out << "  \"root_count\": " << id.root_count << ",\n";
  out << "  \"root_table_hash\": \"" << digest_hex(id.root_table_hash) << "\",\n";
  out << "  \"total_classes\": " << id.total_classes << "\n";
  out << "}\n";
  return out.str();
}

// Read the parts of a build.json needed to compare identities. A minimal
// scanner keeps the builder free of a JSON dependency; the file is written by
// this function only, so its shape is fixed.
BuildIdentity read_identity(const std::filesystem::path& path) {
  std::ifstream in(path);
  if (!in)
    throw std::runtime_error("cannot open " + path.string());
  std::ostringstream ss;
  ss << in.rdbuf();
  const std::string text = ss.str();
  auto str_value = [&](const std::string& key) -> std::string {
    const std::string needle = "\"" + key + "\": \"";
    const std::size_t pos = text.find(needle);
    if (pos == std::string::npos)
      throw std::runtime_error("build identity is missing " + key);
    const std::size_t start = pos + needle.size();
    const std::size_t end = text.find('"', start);
    if (end == std::string::npos)
      throw std::runtime_error("build identity has a malformed " + key);
    return text.substr(start, end - start);
  };
  auto num_value = [&](const std::string& key) -> std::uint64_t {
    const std::string needle = "\"" + key + "\": ";
    const std::size_t pos = text.find(needle);
    if (pos == std::string::npos)
      throw std::runtime_error("build identity is missing " + key);
    return std::strtoull(text.c_str() + pos + needle.size(), nullptr, 10);
  };
  BuildIdentity id;
  id.codebook = str_value("codebook");
  id.card_bucket_kind = static_cast<std::uint8_t>(num_value("card_bucket_kind"));
  id.range = str_value("range");
  id.card_abstraction_digest =
      std::strtoull(str_value("card_abstraction_digest").c_str(), nullptr, 16);
  id.action_abstraction_digest =
      std::strtoull(str_value("action_abstraction_digest").c_str(), nullptr, 16);
  id.range_combo_count = static_cast<std::size_t>(num_value("range_combo_count"));
  id.range_digest = std::strtoull(str_value("range_digest").c_str(), nullptr, 16);
  id.player_count = static_cast<std::size_t>(num_value("player_count"));
  id.big_blind = static_cast<poker::Chips>(num_value("big_blind"));
  id.starting_stack = static_cast<poker::Chips>(num_value("starting_stack"));
  id.contribution = static_cast<poker::Chips>(num_value("contribution"));
  id.max_raises_per_street = static_cast<std::uint8_t>(num_value("max_raises_per_street"));
  id.iterations_per_class = num_value("iterations_per_class");
  id.seed = num_value("seed");
  id.class_count = static_cast<std::size_t>(num_value("class_count"));
  id.total_classes = static_cast<std::size_t>(num_value("total_classes"));
  id.root_count = static_cast<std::size_t>(num_value("root_count"));
  id.root_table_hash = std::strtoull(str_value("root_table_hash").c_str(), nullptr, 16);
  return id;
}

void write_identity(const std::filesystem::path& sections_dir, const BuildIdentity& id) {
  std::error_code ec;
  std::filesystem::create_directories(sections_dir, ec);
  if (ec)
    throw std::runtime_error("cannot create sections directory: " + ec.message());
  const std::string text = identity_json(id);
  write_file_atomically(sections_dir / "build.json",
                        std::vector<std::uint8_t>(text.begin(), text.end()));
}

// Fail closed unless `current` matches `requested` field for field. A smaller
// class_count is as fatal as a larger one: it would skip classes, assemble a
// smaller library, and then delete the completed sections for the larger run.
void require_identity_match(const BuildIdentity& requested, const BuildIdentity& current) {
  auto mismatch = [](const char* field, const std::string& want, const std::string& got) {
    throw std::runtime_error(std::string("resume refused: build identity field `") + field +
                             "` is `" + got + "`, requested `" + want + "`");
  };
  auto mismatch_num = [&](const char* field, std::uint64_t want, std::uint64_t got) {
    mismatch(field, std::to_string(want), std::to_string(got));
  };
  if (requested.codebook != current.codebook)
    mismatch("codebook", requested.codebook, current.codebook);
  if (requested.card_bucket_kind != current.card_bucket_kind)
    mismatch_num("card_bucket_kind", requested.card_bucket_kind, current.card_bucket_kind);
  if (requested.card_abstraction_digest != current.card_abstraction_digest)
    mismatch_num("card_abstraction_digest", requested.card_abstraction_digest,
                 current.card_abstraction_digest);
  if (requested.action_abstraction_digest != current.action_abstraction_digest)
    mismatch_num("action_abstraction_digest", requested.action_abstraction_digest,
                 current.action_abstraction_digest);
  if (requested.range != current.range)
    mismatch("range", requested.range, current.range);
  if (requested.range_combo_count != current.range_combo_count)
    mismatch_num("range_combo_count", requested.range_combo_count, current.range_combo_count);
  if (requested.range_digest != current.range_digest)
    mismatch_num("range_digest", requested.range_digest, current.range_digest);
  if (requested.player_count != current.player_count)
    mismatch_num("player_count", requested.player_count, current.player_count);
  if (requested.big_blind != current.big_blind)
    mismatch_num("big_blind", requested.big_blind, current.big_blind);
  if (requested.starting_stack != current.starting_stack)
    mismatch_num("starting_stack", requested.starting_stack, current.starting_stack);
  if (requested.contribution != current.contribution)
    mismatch_num("contribution", requested.contribution, current.contribution);
  if (requested.max_raises_per_street != current.max_raises_per_street)
    mismatch_num("max_raises_per_street", requested.max_raises_per_street,
                 current.max_raises_per_street);
  if (requested.iterations_per_class != current.iterations_per_class)
    mismatch_num("iterations_per_class", requested.iterations_per_class,
                 current.iterations_per_class);
  if (requested.seed != current.seed)
    mismatch_num("seed", requested.seed, current.seed);
  if (requested.class_count != current.class_count)
    mismatch_num("class_count", requested.class_count, current.class_count);
  if (requested.total_classes != current.total_classes)
    mismatch_num("total_classes", requested.total_classes, current.total_classes);
  if (requested.root_count != current.root_count)
    mismatch_num("root_count", requested.root_count, current.root_count);
  if (requested.root_table_hash != current.root_table_hash)
    mismatch_num("root_table_hash", requested.root_table_hash, current.root_table_hash);
}

// Append one JSON line to an owner's fragment; see the declaration above.
void append_fragment_line(const std::filesystem::path& sections_dir, const std::string& tag,
                          const FragmentClass& fc) {
  const std::filesystem::path path = sections_dir / ("manifest-part-" + tag + ".jsonl");
  const ClassEntry& e = fc.entry;
  std::ostringstream line;
  line << "{\"root\": " << fc.root_id << ", \"index\": " << fc.index << ", \"canonical_board\": ["
       << e.canonical_board[0] << ", " << e.canonical_board[1] << ", " << e.canonical_board[2]
       << "], "
       << "\"stored_rows\": " << e.stored_rows << ", "
       << "\"completed_iterations\": " << e.completed_iterations << ", "
       << "\"information_sets\": " << e.information_sets << ", "
       << "\"section_bytes\": " << e.section_bytes << "}\n";
  std::ofstream out(path, std::ios::binary | std::ios::app);
  if (!out)
    throw std::runtime_error("cannot append fragment " + path.string());
  out << line.str();
  out.flush();
  if (!out)
    throw std::runtime_error("failed appending fragment " + path.string());
}

// Read every fragment line. A trailing partial line is discarded (a crash
// during append is covered by the section file); a complete but malformed line
// is a hard error.
// Parse one fixed-shape JSONL record written by append_fragment_line. Returns
// false when the text is not a complete record (used to discard a torn tail).
bool parse_fragment_line(const std::string& text, FragmentClass& out) {
  auto number_after = [&](const std::string& key, std::uint64_t& value) -> bool {
    const std::string needle = "\"" + key + "\": ";
    const std::size_t pos = text.find(needle);
    if (pos == std::string::npos)
      return false;
    const char* begin = text.c_str() + pos + needle.size();
    char* end = nullptr;
    value = std::strtoull(begin, &end, 10);
    return end != begin;
  };
  std::uint64_t root = 0, index = 0, rows = 0, iters = 0, infos = 0, bytes = 0;
  if (!number_after("root", root) || !number_after("index", index) ||
      !number_after("stored_rows", rows) || !number_after("completed_iterations", iters) ||
      !number_after("information_sets", infos) || !number_after("section_bytes", bytes))
    return false;
  const std::string board_key = "\"canonical_board\": [";
  const std::size_t board_pos = text.find(board_key);
  if (board_pos == std::string::npos)
    return false;
  const char* p = text.c_str() + board_pos + board_key.size();
  char* end = nullptr;
  for (int i = 0; i < 3; ++i) {
    out.entry.canonical_board[i] = static_cast<int>(std::strtol(p, &end, 10));
    if (end == p)
      return false;
    p = end;
    while (*p == ' ' || *p == ',')
      ++p;
  }
  out.root_id = static_cast<std::uint32_t>(root);
  out.index = static_cast<std::size_t>(index);
  out.entry.root_id = static_cast<std::uint32_t>(root);
  out.entry.stored_rows = static_cast<std::size_t>(rows);
  out.entry.completed_iterations = iters;
  out.entry.information_sets = static_cast<std::size_t>(infos);
  out.entry.section_bytes = static_cast<std::size_t>(bytes);
  return true;
}

// Read every fragment line. A trailing partial line is discarded (a crash
// during append is covered by the section file); a complete but malformed line
// is a hard error.
std::vector<FragmentClass> read_fragments(const std::filesystem::path& sections_dir) {
  std::vector<FragmentClass> out;
  std::error_code ec;
  std::vector<std::string> tags;
  for (std::size_t w = 0;; ++w) {
    const std::filesystem::path probe =
        sections_dir / ("manifest-part-" + std::to_string(w) + ".jsonl");
    if (!std::filesystem::exists(probe, ec))
      break;
    tags.push_back(std::to_string(w));
  }
  tags.push_back("serial");
  for (const std::string& tag : tags) {
    const std::filesystem::path path = sections_dir / ("manifest-part-" + tag + ".jsonl");
    if (!std::filesystem::exists(path, ec))
      continue;
    std::ifstream in(path, std::ios::binary);
    if (!in)
      throw std::runtime_error("cannot open fragment " + path.string());
    std::string line;
    std::vector<std::string> lines;
    while (std::getline(in, line)) {
      if (!line.empty() && line.back() == '\r')
        line.pop_back();
      lines.push_back(line);
    }
    // getline drops the final newline silently, so position alone cannot
    // distinguish a torn tail; only the LAST line may be torn.
    for (std::size_t i = 0; i < lines.size(); ++i) {
      if (lines[i].empty())
        continue;
      FragmentClass fc;
      if (!parse_fragment_line(lines[i], fc)) {
        if (i + 1 == lines.size())
          break;  // torn tail
        throw std::runtime_error("fragment " + path.string() + " has a malformed line " +
                                 std::to_string(i + 1));
      }
      out.push_back(std::move(fc));
    }
  }
  return out;
}

}  // namespace

// RFC 0033's derivation, exposed as the one production root: every seat live,
// equal contributions, no folds, pot the sum of contributions. `starting_stack`
// is the buy-in, so per-seat chips behind are `starting_stack - contribution`;
// recording the buy-in in `stacks` would double-count the contribution and
// make the root chip-non-conserving (the RFC 0033 defect).
RootSpec make_limped_root_spec(std::size_t player_count, poker::Chips starting_stack,
                               poker::Chips contribution, resident::RootLineTag line_tag) {
  if (starting_stack <= contribution)
    throw std::invalid_argument("make_limped_root_spec: starting stack must exceed contribution");
  RootSpec root;
  root.root_id = 0;
  root.line_tag = line_tag;
  root.range_profile = resident::RootRangeProfile::LiveUniform;
  for (std::size_t s = 0; s < player_count; ++s) {
    root.stacks[s] = static_cast<std::uint64_t>(starting_stack - contribution);
    root.contributions[s] = static_cast<std::uint64_t>(contribution);
    root.folded[s] = 0;
  }
  root.pot = static_cast<std::uint64_t>(player_count) * static_cast<std::uint64_t>(contribution);
  root.live_count = static_cast<std::uint8_t>(player_count);
  root.class_count = 0;
  return root;
}

// RFC 0035: canonical ordered root-table hash. Fixed-stride binary fields
// under FNV-1a 64, prefixed by the player count; see flop_library.hpp.
std::uint64_t root_table_hash(std::size_t player_count, std::span<const RootSpec> roots) {
  std::uint64_t h = 0xcbf29ce484222325ULL;
  auto mix8 = [&](std::uint8_t v) {
    h ^= v;
    h *= 0x100000001b3ULL;
  };
  auto mix64 = [&](std::uint64_t v) {
    for (int i = 0; i < 8; ++i)
      mix8(static_cast<std::uint8_t>((v >> (i * 8)) & 0xffu));
  };
  mix8(static_cast<std::uint8_t>(player_count));
  for (const RootSpec& root : roots) {
    mix8(static_cast<std::uint8_t>(root.line_tag));
    mix64(root.pot);
    for (std::size_t s = 0; s < player_count; ++s)
      mix64(root.stacks[s]);
    for (std::size_t s = 0; s < player_count; ++s)
      mix64(root.contributions[s]);
    for (std::size_t s = 0; s < player_count; ++s)
      mix8(root.folded[s] ? 1 : 0);
    std::uint8_t live = 0;
    for (std::size_t s = 0; s < player_count; ++s)
      if (!root.folded[s])
        ++live;
    mix8(live);
    mix8(static_cast<std::uint8_t>(root.range_profile));
  }
  return h;
}

// RFC 0033: the resume decision, shared by the serial library path and the
// parallel builder parent so the two cannot drift. Its contract, in order:
//
//   1. torn checkpoint            -> discard and retrain
//   2. incompatible revision      -> refuse
//   3. incompatible digest/rows   -> deferred to train_one_class, which runs
//                                    these before the stale removal
//   4. stale but complete         -> classify as disposable (NOT removed here;
//                                    train_one_class removes it after the
//                                    digest and empty-row checks)
//   5. valid state, no identity   -> refuse
//
// Dispositions 2 and 4 can apply to the same file with opposite outcomes, so
// the order is load-bearing: revision and the deferred digest checks must win
// over the stale classification, or a hard refusal would silently become a
// full retrain.
void preflight_resume(const std::filesystem::path& out_dir, const BuildParams& params,
                      bool resume) {
  if (!resume)
    return;
  const std::vector<RootSpec> roots = effective_roots(params);
  const std::filesystem::path sections_dir = out_dir / kSectionsDir;
  const bool have_identity = std::filesystem::exists(sections_dir / "build.json");
  std::vector<std::string> unidentified;
  for (const RootSpec& root : roots)
    for (std::size_t index = 0; index < params.class_count; ++index) {
      const std::string stem = pair_stem(root.root_id, index);
      // A completed pair keeps its section and deletes its checkpoint, so the
      // two are complementary: the checkpoint scan alone protects the work
      // that would be discarded anyway. Sections are published atomically, so
      // a present .bsf is always complete and "valid" means "present".
      const std::filesystem::path section =
          sections_dir /
          resident::root_class_section_name(root.root_id, static_cast<std::uint32_t>(index));
      std::error_code ec;
      if (!have_identity && std::filesystem::exists(section, ec))
        unidentified.push_back(section.string());

      const auto path = out_dir / (stem + "-checkpoint.bswarm");
      if (!std::filesystem::exists(path, ec))
        continue;
      artifacts::SealedCheckpointState checkpoint;
      bool corrupt = false;
      try {
        // Header-only: the payload is decoded on the training path, where a
        // mismatch is actionable. A failure here (including a foreign BSWARM01/
        // 02 magic after the RFC 0035 bump) means the file cannot be a
        // checkpoint this build wrote.
        checkpoint = artifacts::inspect_sealed_checkpoint(path);
      } catch (const artifacts::SealedCheckpointError&) {
        corrupt = true;
      } catch (const std::runtime_error&) {
        corrupt = true;
      }
      if (corrupt) {
        std::fprintf(stderr, "warning: discarding corrupt checkpoint %s; root/class will retrain\n",
                     path.string().c_str());
        std::filesystem::remove(path, ec);
        continue;
      }
      // Disposition 2 before 4: a checkpoint can carry an incompatible revision
      // AND be stale, and the refusal must not be downgraded to a retrain.
      if (checkpoint.algorithm_revision != artifacts::kDefaultAlgorithmRevisionV2 ||
          checkpoint.algorithm_revision_id != solver::kNSeatAlgorithmRevision)
        throw std::runtime_error(
            "incompatible checkpoint algorithm; refusing resume without modifying " +
            path.string());
      if (!have_identity) {
        // Disposition 4: classified, not removed. Removing here would run ahead
        // of the digest and empty-row checks in train_one_class, turning those
        // refusals into a silent retrain.
        if (checkpoint.completed_iterations >= params.iterations_per_class)
          continue;
        unidentified.push_back(path.string());
      }
    }
  if (!have_identity && !unidentified.empty()) {
    std::string list;
    for (const std::string& name : unidentified)
      list += "\n  " + name;
    throw std::runtime_error(
        "resume requested but .sections/build.json is missing while reusable state survives; "
        "refusing rather than warm-starting against an unknown root. Delete these files or run "
        "without --resume:" +
        list);
  }
}

bool class_is_dealable(const std::array<int, 3>& board, std::size_t seat_count,
                       const std::vector<solver::WeightedHand>& range) {
  return class_is_dealable_impl(board, seat_count, range);
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

std::vector<solver::WeightedHand> declared_full_library_range() {
  std::vector<solver::WeightedHand> range;
  range.reserve(1326);
  for (int c0 = 0; c0 < 52; ++c0)
    for (int c1 = c0 + 1; c1 < 52; ++c1)
      range.push_back({{c0, c1}, 1.0});
  return range;
}

std::uint64_t range_digest(const std::vector<solver::WeightedHand>& range) {
  std::vector<solver::WeightedHand> sorted = range;
  std::sort(sorted.begin(), sorted.end(),
            [](const solver::WeightedHand& a, const solver::WeightedHand& b) {
              return bs::comboIndex(a.cards[0], a.cards[1]) <
                     bs::comboIndex(b.cards[0], b.cards[1]);
            });
  std::uint64_t h = 0xcbf29ce484222325ULL;
  auto mix = [&h](std::uint64_t value) {
    for (int i = 0; i < 8; ++i) {
      h ^= static_cast<std::uint8_t>((value >> (i * 8)) & 0xffu);
      h *= 0x100000001b3ULL;
    }
  };
  for (const solver::WeightedHand& hand : sorted) {
    mix(static_cast<std::uint64_t>(bs::comboIndex(hand.cards[0], hand.cards[1])));
    std::uint64_t bits;
    std::memcpy(&bits, &hand.weight, sizeof(bits));
    mix(bits);
  }
  return h;
}

// Shared parameter validation for the serial path and the parallel workers.
// Explicit multi-root tables need no positive starting_stack/contribution;
// the single-root default derives its root from them.
void validate_params(const BuildParams& params) {
  if (params.class_count == 0)
    throw std::invalid_argument("class_count must be positive");
  if (params.iterations_per_class == 0)
    throw std::invalid_argument("iterations must be positive");
  if (params.range.empty())
    throw std::invalid_argument("range must be non-empty");
  if (params.player_count < 2 || params.player_count > 10)
    throw std::invalid_argument("player_count must be in [2, 10]");
  if (params.roots.empty()) {
    if (params.starting_stack == 0 || params.contribution == 0)
      throw std::invalid_argument("starting_stack and contribution must be positive");
    if (params.starting_stack <= params.contribution)
      throw std::invalid_argument("starting_stack must exceed the contribution");
  }
  (void)effective_roots(params);
}

LibraryManifest build_library(const std::filesystem::path& out_dir, const BuildParams& params) {
  validate_params(params);

  std::error_code ec;
  std::filesystem::create_directories(out_dir, ec);
  if (ec)
    throw std::runtime_error("cannot create output directory: " + ec.message());

  const std::vector<std::array<int, 3>> classes = enumerate_classes();
  if (classes.size() != kTotalClasses)
    throw std::runtime_error("canonical class count drifted from the pinned " +
                             std::to_string(kTotalClasses));
  if (params.class_count > classes.size())
    throw std::invalid_argument("class_count exceeds total classes (" +
                                std::to_string(classes.size()) + ")");

  const std::vector<RootSpec> roots = effective_roots(params);
  const abstraction::AbstractionId card_id = abstraction::suit_canonicalization_id();
  const std::uint64_t root_hash = root_table_hash(params.player_count, roots);
  const std::filesystem::path sections_dir = out_dir / kSectionsDir;
  preflight_resume(out_dir, params, params.resume);
  // Sections without an identity record cannot be trusted for a resume.
  if (params.resume && !std::filesystem::exists(sections_dir / "build.json", ec))
    std::filesystem::remove_all(sections_dir, ec);
  prepare_build_identity(out_dir, params, params.resume);

  // Serial path: train, serialize, and assemble once. Failure leaves no
  // partial library (the sections and any checkpoint are removed).
  try {
    for (const RootSpec& root : roots)
      for (std::size_t i = 0; i < params.class_count; ++i) {
        if (!class_is_dealable(classes[i], params.player_count, params.range)) {
          if (root.root_id == 0)
            std::printf("class %zu: undealable for %zu seats (board blocks the range); excluded\n",
                        i, params.player_count);
          continue;
        }
        if (params.resume && class_section_exists(out_dir, root.root_id, i))
          continue;
        (void)train_one_class(out_dir, root, i, classes[i], params, card_id, root_hash, "serial");
      }
  } catch (...) {
    std::filesystem::remove_all(sections_dir, ec);
    throw;
  }
  return assemble_library(out_dir, params);
}

LibraryManifest build_library(const std::filesystem::path& out_dir, std::size_t class_count,
                              std::uint64_t iterations_per_class, std::uint64_t seed,
                              poker::Chips starting_stack, poker::Chips contribution,
                              const std::vector<solver::WeightedHand>& range,
                              std::size_t player_count, abstraction::ActionAbstraction action,
                              bool resume, resident::ProbabilityLayout layout,
                              const std::string& range_name) {
  BuildParams params;
  params.class_count = class_count;
  params.iterations_per_class = iterations_per_class;
  params.seed = seed;
  params.starting_stack = starting_stack;
  params.contribution = contribution;
  params.range = range;
  params.player_count = player_count;
  params.action = action;
  params.resume = resume;
  params.layout = layout;
  params.range_name = range_name;
  return build_library(out_dir, params);
}

void prepare_build_identity(const std::filesystem::path& out_dir, const BuildParams& params,
                            bool resume) {
  const abstraction::AbstractionId card_id = abstraction::suit_canonicalization_id();
  const std::vector<RootSpec> roots = effective_roots(params);
  const std::filesystem::path sections_dir = out_dir / kSectionsDir;
  const BuildIdentity requested = make_identity(params, card_id, roots);
  std::error_code ec;
  if (resume && std::filesystem::exists(sections_dir / "build.json", ec)) {
    require_identity_match(requested, read_identity(sections_dir / "build.json"));
    return;
  }
  if (!resume)
    std::filesystem::remove_all(sections_dir, ec);
  write_identity(sections_dir, requested);
}

void build_library_worker(const std::filesystem::path& out_dir, std::size_t worker_id,
                          std::size_t parallel, const BuildParams& params) {
  if (parallel == 0)
    throw std::invalid_argument("parallel must be positive");
  if (worker_id >= parallel)
    throw std::invalid_argument("worker_id must be < parallel");
  validate_params(params);
  const std::vector<RootSpec> roots = effective_roots(params);

  std::error_code ec;
  std::filesystem::create_directories(out_dir, ec);
  if (ec)
    throw std::runtime_error("cannot create output directory: " + ec.message());

  const std::vector<std::array<int, 3>> classes = enumerate_classes();
  if (params.class_count > classes.size())
    throw std::invalid_argument("class_count exceeds total classes");
  const std::filesystem::path sections_dir = out_dir / kSectionsDir;
  std::filesystem::create_directories(sections_dir, ec);
  if (ec)
    throw std::runtime_error("cannot create sections directory: " + ec.message());
  // The parent owns build.json. A worker that finds none must not guess the
  // layout: fail closed before training anything.
  if (!std::filesystem::exists(sections_dir / "build.json", ec))
    throw std::runtime_error(
        "worker: .sections/build.json is missing; the parent must call prepare_build_identity");
  const abstraction::AbstractionId card_id = abstraction::suit_canonicalization_id();
  const std::uint64_t root_hash = root_table_hash(params.player_count, roots);

  for (const RootSpec& root : roots)
    for (std::size_t i = worker_id; i < params.class_count; i += parallel) {
      if (!class_is_dealable(classes[i], params.player_count, params.range))
        continue;  // undealable class: recorded as a coverage exclusion by assembly
      if (class_section_exists(out_dir, root.root_id, i))
        continue;  // completed by an earlier attempt
      (void)train_one_class(out_dir, root, i, classes[i], params, card_id, root_hash,
                            std::to_string(worker_id));
    }
}

LibraryManifest assemble_library(const std::filesystem::path& out_dir, const BuildParams& params) {
  const std::vector<std::array<int, 3>> classes = enumerate_classes();
  if (params.class_count > classes.size())
    throw std::invalid_argument("class_count exceeds total classes");
  const std::vector<RootSpec> roots = effective_roots(params);
  const abstraction::AbstractionId card_id = abstraction::suit_canonicalization_id();
  const std::uint64_t root_hash = root_table_hash(params.player_count, roots);
  const std::filesystem::path sections_dir = out_dir / kSectionsDir;

  // The dealable set is identical for every root: dealability depends only on
  // the board, the seat count, and the range (folded seats still draw cards).
  std::vector<std::array<int, 3>> undealable;
  std::vector<std::uint32_t> expected_ids;
  for (std::size_t i = 0; i < params.class_count; ++i) {
    if (!class_is_dealable(classes[i], params.player_count, params.range)) {
      undealable.push_back(classes[i]);
      continue;
    }
    expected_ids.push_back(static_cast<std::uint32_t>(i));
  }
  if (expected_ids.empty())
    throw std::runtime_error("assembly: no dealable classes in the declared stride");
  for (const RootSpec& root : roots)
    for (std::uint32_t id : expected_ids)
      if (!class_section_exists(out_dir, root.root_id, id))
        throw std::runtime_error("assembly: root " + std::to_string(root.root_id) + " class " +
                                 std::to_string(id) + " has no section (build incomplete)");

  // Section headers are authoritative; fragments supply completed_iterations
  // when present. A fragment naming a pair with no section is corruption.
  std::map<std::pair<std::uint32_t, std::uint32_t>, ClassEntry> records;
  for (const FragmentClass& fc : read_fragments(sections_dir)) {
    if (fc.root_id >= roots.size() || fc.index >= params.class_count ||
        !class_section_exists(out_dir, fc.root_id, fc.index))
      throw std::runtime_error("assembly: fragment names root " + std::to_string(fc.root_id) +
                               " class " + std::to_string(fc.index) + " with no section");
    auto key = std::make_pair(fc.root_id, static_cast<std::uint32_t>(fc.index));
    auto [it, inserted] = records.emplace(key, fc.entry);
    if (!inserted && (it->second.stored_rows != fc.entry.stored_rows ||
                      it->second.completed_iterations != fc.entry.completed_iterations ||
                      it->second.information_sets != fc.entry.information_sets ||
                      it->second.section_bytes != fc.entry.section_bytes))
      throw std::runtime_error("assembly: conflicting fragment records for root " +
                               std::to_string(fc.root_id) + " class " + std::to_string(fc.index));
  }

  // Per-root entries, root-major then class-ascending.
  std::vector<ServedRootEntry> served;
  served.reserve(roots.size());
  std::vector<ClassEntry> all_entries;
  std::vector<resident::RootDirectoryEntry> directory_roots;
  std::vector<std::vector<std::uint32_t>> directory_ids;
  directory_roots.reserve(roots.size());
  directory_ids.reserve(roots.size());
  for (const RootSpec& declared : roots) {
    ServedRootEntry served_root;
    served_root.root = declared;
    directory_roots.push_back(declared);
    std::vector<std::uint32_t>& ids = directory_ids.emplace_back();
    for (std::uint32_t id : expected_ids) {
      ClassEntry entry;
      entry.root_id = declared.root_id;
      entry.canonical_board = classes[id];
      auto it = records.find(std::make_pair(declared.root_id, id));
      if (it != records.end()) {
        entry = it->second;
      } else {
        // Section without a record: reconstruct the counters from its header.
        const auto path = sections_dir / resident::root_class_section_name(declared.root_id, id);
        std::error_code ec;
        entry.section_bytes = static_cast<std::size_t>(std::filesystem::file_size(path, ec));
        if (ec)
          throw std::runtime_error("assembly: cannot stat section " + path.string());
        entry.completed_iterations = params.iterations_per_class;
        // stored_rows and information_sets are not recoverable from a
        // section; use its bucket-row count as the honest public figure. The
        // BSFLIB03 section header places total_bucket_rows at offset 24.
        std::ifstream in(path, std::ios::binary);
        std::array<std::uint8_t, 32> head{};
        in.read(reinterpret_cast<char*>(head.data()), head.size());
        if (!in)
          throw std::runtime_error("assembly: cannot read section header " + path.string());
        const std::uint32_t bucket_rows = static_cast<std::uint32_t>(head[24]) |
                                          (static_cast<std::uint32_t>(head[25]) << 8) |
                                          (static_cast<std::uint32_t>(head[26]) << 16) |
                                          (static_cast<std::uint32_t>(head[27]) << 24);
        entry.stored_rows = bucket_rows;
        entry.information_sets = bucket_rows;
      }
      if (entry.canonical_board != classes[id])
        throw std::runtime_error("assembly: root " + std::to_string(declared.root_id) + " class " +
                                 std::to_string(id) +
                                 " board disagrees between section and fragment");
      ids.push_back(id);
      served_root.classes.push_back(entry);
      all_entries.push_back(std::move(entry));
    }
    served_root.root.class_count = static_cast<std::uint32_t>(ids.size());
    served_root.class_count = ids.size();
    served_root.covered_flops = count_covered_flops(covered_boards(served_root.classes));
    served_root.total_flops = kTotalFlops;
    for (const ClassEntry& e : served_root.classes)
      served_root.section_bytes += e.section_bytes;
    served.push_back(std::move(served_root));
    directory_roots.back().class_count = static_cast<std::uint32_t>(ids.size());
  }

  const std::string bin_name = "library.bin";
  const std::filesystem::path bin_path = out_dir / bin_name;
  resident::LibraryIdentity identity;
  identity.card_abstraction_digest = card_id.digest;
  identity.action_abstraction_digest = params.action.id().digest;
  try {
    resident::assemble_binary_library(bin_path, make_unified_game(params), identity, params.layout,
                                      directory_roots, sections_dir, directory_ids);
  } catch (const std::exception&) {
    std::error_code ec;
    std::filesystem::remove(bin_path, ec);
    std::filesystem::remove(std::filesystem::path(bin_path.string() + ".sha256"), ec);
    throw;
  }
  std::error_code ec;
  const std::uintmax_t bin_bytes = std::filesystem::file_size(bin_path, ec);
  if (ec)
    throw std::runtime_error("assembly: cannot stat " + bin_path.string());

  std::size_t total_covered = 0;
  std::uintmax_t total_section_bytes = 0;
  for (const ServedRootEntry& r : served) {
    total_covered += r.covered_flops;
    total_section_bytes += r.section_bytes;
  }
  (void)total_section_bytes;

  LibraryManifest manifest;
  manifest.card_id = card_id;
  manifest.card_bucket_kind = solver::kNSeatCardKind;
  manifest.action_id = params.action.id();
  manifest.player_count = params.player_count;
  manifest.max_raises_per_street = params.action.max_raises_per_street();
  manifest.big_blind = kDefaultBigBlind;
  manifest.iterations_per_class = params.iterations_per_class;
  manifest.seed = params.seed;
  manifest.total_classes = kTotalClasses;
  manifest.served_roots = std::move(served);
  manifest.classes = std::move(all_entries);
  manifest.undealable_boards = undealable;
  manifest.class_count = manifest.classes.size();
  manifest.covered_flops = total_covered;
  manifest.total_flops = kTotalFlops;
  manifest.storage_bytes = bin_bytes;
  manifest.library_bin = bin_name;
  manifest.library_bin_sha256 = artifacts::sha256_file_hex(bin_path);
  manifest.library_bin_bytes = bin_bytes;
  manifest.codebook = codebook_name(params.layout);
  manifest.range = params.range_name;
  manifest.range_combo_count = params.range.size();
  manifest.range_digest = range_digest(params.range);
  manifest.root_table_hash = root_hash;

  write_manifest(manifest, out_dir);
  std::filesystem::remove_all(sections_dir, ec);
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
  out << "  \"card_bucket_kind\": " << static_cast<int>(manifest.card_bucket_kind) << ",\n";
  out << "  \"action_abstraction_name\": \"" << json_escape(manifest.action_id.name) << "\",\n";
  out << "  \"action_abstraction_version\": " << manifest.action_id.version << ",\n";
  out << "  \"action_abstraction_parameters\": \"" << json_escape(manifest.action_id.parameters)
      << "\",\n";
  out << "  \"action_abstraction_digest\": \"" << digest_hex(manifest.action_id.digest) << "\",\n";
  out << "  \"player_count\": " << manifest.player_count << ",\n";
  out << "  \"max_raises_per_street\": " << static_cast<int>(manifest.max_raises_per_street)
      << ",\n";
  out << "  \"big_blind\": " << manifest.big_blind << ",\n";
  out << "  \"iterations_per_class\": " << manifest.iterations_per_class << ",\n";
  out << "  \"seed\": " << manifest.seed << ",\n";
  out << "  \"range\": \"" << json_escape(manifest.range) << "\",\n";
  out << "  \"range_combo_count\": " << manifest.range_combo_count << ",\n";
  out << "  \"range_digest\": \"" << digest_hex(manifest.range_digest) << "\",\n";
  out << "  \"root_table_hash\": \"" << digest_hex(manifest.root_table_hash) << "\",\n";
  out << "  \"library_bin\": \"" << json_escape(manifest.library_bin) << "\",\n";
  out << "  \"library_bin_sha256\": \"" << manifest.library_bin_sha256 << "\",\n";
  out << "  \"library_bin_bytes\": " << manifest.library_bin_bytes << ",\n";
  out << "  \"codebook\": \"" << json_escape(manifest.codebook) << "\",\n";
  // RFC 0035: one entry per trained root, naming the exact ledger, labels,
  // class coverage, and stored/flop totals. An operator who sees
  // class_count == 1755 must not infer that every flop state is served.
  out << "  \"served_roots\": [\n";
  for (std::size_t r = 0; r < manifest.served_roots.size(); ++r) {
    const ServedRootEntry& sr = manifest.served_roots[r];
    out << "    {\"root_id\": " << sr.root.root_id << ", \"line_tag\": \""
        << resident::to_string(sr.root.line_tag) << "\", \"pot\": " << sr.root.pot
        << ", \"stacks\": [";
    for (std::size_t s = 0; s < manifest.player_count; ++s)
      out << (s ? ", " : "") << sr.root.stacks[s];
    out << "], \"contributions\": [";
    for (std::size_t s = 0; s < manifest.player_count; ++s)
      out << (s ? ", " : "") << sr.root.contributions[s];
    out << "], \"folded\": [";
    for (std::size_t s = 0; s < manifest.player_count; ++s)
      out << (s ? ", " : "") << (sr.root.folded[s] ? "true" : "false");
    out << "], \"live_count\": " << static_cast<int>(sr.root.live_count)
        << ", \"range_profile\": " << static_cast<int>(sr.root.range_profile)
        << ", \"class_count\": " << sr.class_count << ", \"covered_flops\": " << sr.covered_flops
        << ", \"total_flops\": " << sr.total_flops << ", \"section_bytes\": " << sr.section_bytes
        << "}" << (r + 1 < manifest.served_roots.size() ? "," : "") << "\n";
  }
  out << "  ],\n";
  out << "  \"classes\": [\n";
  for (std::size_t i = 0; i < manifest.classes.size(); ++i) {
    const ClassEntry& e = manifest.classes[i];
    out << "    {\n";
    out << "      \"root_id\": " << e.root_id << ",\n";
    out << "      \"canonical_board\": [" << e.canonical_board[0] << ", " << e.canonical_board[1]
        << ", " << e.canonical_board[2] << "],\n";
    out << "      \"stored_rows\": " << e.stored_rows << ",\n";
    out << "      \"completed_iterations\": " << e.completed_iterations << ",\n";
    out << "      \"information_sets\": " << e.information_sets << ",\n";
    out << "      \"section_bytes\": " << e.section_bytes << "\n";
    out << "    }" << (i + 1 < manifest.classes.size() ? "," : "") << "\n";
  }
  out << "  ],\n";
  out << "  \"undealable_boards\": [";
  for (std::size_t i = 0; i < manifest.undealable_boards.size(); ++i) {
    const auto& b = manifest.undealable_boards[i];
    out << (i ? ", [" : "[") << b[0] << ", " << b[1] << ", " << b[2] << "]";
  }
  out << "],\n";
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
