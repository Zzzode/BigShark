// Artifact fault, round-trip, and boundary tests for the schema-v2 seat
// artifacts and the schema-v3 card-abstraction extension (schema v1 was
// retired by RFC 0038).
//
// The test is an independently written oracle: expected canonical key bytes,
// schema facts, application id, table sets, and fault outcomes are asserted
// directly; the artifact writer is not used to vouch for its own output beyond
// lossless round trips. SQLite is linked directly ONLY in this test so it can
// plant constraint/corruption faults and host a deterministic ENOSPC VFS shim.
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <bit>
#include <bs/abstract_tree.hpp>
#include <bs/abstraction.hpp>
#include <bs/eval.hpp>
#include <bs/frontier.hpp>
#include <bs/game_definition.hpp>
#include <bs/heads_up.hpp>
#include <bs/heads_up_solver.hpp>
#include <bs/nseat_trainer.hpp>
#include <bs/seat_policy.hpp>
#include <bs/strategy_artifact.hpp>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "artifacts/artifact_internal.hpp"
#include "gto/heads_up_solver_debug.hpp"
#include "sqlite3.h"

using namespace bs::poker;
using namespace bs::solver;
using namespace bs::artifacts;

#define CHECK(cond)                                                         \
  do {                                                                      \
    if (!(cond)) {                                                          \
      std::printf("CHECK failed: %s (%s:%d)\n", #cond, __FILE__, __LINE__); \
      return 1;                                                             \
    }                                                                       \
  } while (0)

// CHECK variant for helpers that do not return int: aborts the process with a
// diagnostic instead of returning.
#define CHECK_ABORT(cond)                                                   \
  do {                                                                      \
    if (!(cond)) {                                                          \
      std::printf("CHECK failed: %s (%s:%d)\n", #cond, __FILE__, __LINE__); \
      std::abort();                                                         \
    }                                                                       \
  } while (0)

namespace fs = std::filesystem;

namespace {

int card(const char* name) {
  return bs::cardId(std::string(name));
}

bool near(double a, double b, double tolerance = 1e-12) {
  return std::isfinite(a) && std::isfinite(b) && std::abs(a - b) <= tolerance;
}

bool same_bits(double a, double b) {
  return std::bit_cast<std::uint64_t>(a) == std::bit_cast<std::uint64_t>(b);
}

template <typename Exception = ArtifactError, typename F>
bool throws_artifact(F&& operation, std::optional<ArtifactErrorKind> kind = std::nullopt) {
  try {
    operation();
  } catch (const Exception& error) {
    if (kind && error.kind() != *kind) {
      std::printf("wrong error kind: got %d expected %d\n", static_cast<int>(error.kind()),
                  static_cast<int>(*kind));
      return false;
    }
    return true;
  } catch (...) {
    return false;
  }
  return false;
}

// Exact size-fraction comparison used by the seat schedule round trip.
bool same_fraction_exact(const Fraction& a, const Fraction& b) {
  return a.numerator == b.numerator && a.denominator == b.denominator;
}

// --- RFC 0009 W2b schema-v2 seat-generic fixtures ---------------------------

// A 3-seat river-rooted fixture: board_size 5 so the tree has no chance nodes
// and external sampling covers every action node. Same chip shape as the
// heads-up fixtures but with three occupied seats, exercising the game_seats
// table and the seat-generic key grammar (actor 0..2, three-seat event streams
// whose streets the revision-2 grammar carries explicitly).
GameDef three_seat_river_def_v2() {
  GameDef def{};
  def.player_count = 3;
  def.button = 0;
  def.big_blind = 2;
  def.stacks = {1, 1, 1, 0, 0, 0, 0, 0, 0, 0};
  def.contributions = {1, 1, 1, 0, 0, 0, 0, 0, 0, 0};
  def.pot = 3;
  def.board = {card("2c"), card("3d"), card("7h"), card("Ks"), card("9c")};
  def.board_size = 5;
  return def;
}

// Two combos per seat, all off the river board and mutually card-distinct.
// Canonical form: each hand's cards sorted ascending and each seat's range
// sorted by cards, matching the reader's `ORDER BY player, combo` output (the
// round-trip canonicalizes range order, so the fixture must already be in the
// reader's order for a position-by-position comparison).
std::vector<std::vector<WeightedHand>> three_seat_river_ranges_v2() {
  return {
      {{{card("8h"), card("8c")}, 3}, {{card("Ah"), card("Ac")}, 2}},
      {{{card("8s"), card("8d")}, 7}, {{card("As"), card("Ad")}, 5}},
      {{{card("Jh"), card("Jc")}, 13}, {{card("Qh"), card("Qc")}, 11}},
  };
}

// Trains the 3-seat river fixture and exports a concrete SeatTrainingResult,
// the schema-v2 writer's domain record.
SeatTrainingResult trained_seat_result_v2() {
  const GameDef def = three_seat_river_def_v2();
  const bs::tree::AbstractTree tree(def, bs::abstraction::ActionAbstraction::identity());
  const auto ranges = three_seat_river_ranges_v2();
  const NSeatTrainingResult trained =
      train_nseat(tree, ranges, 200, 20260930, NSeatTrainerLimits{});
  if (trained.termination != NSeatTerminationPhase::Complete)
    throw std::runtime_error("seat fixture training did not complete");
  return export_seat_policy(trained, tree, ranges);
}

// --- RFC 0009 W4c schema-v3 card-abstraction fixtures ----------------------

// A two-seat flop-rooted fixture on a suit-canonical board. {2s, 7h, Kd} has
// suits {0,1,2} in rank order, so the suit-canonicalization relabel is the
// identity and the board is its own class representative -- the precondition
// the schema-v3 writer's board-canonical integrity check enforces.
GameDef two_seat_flop_canonical_def_v3() {
  GameDef def{};
  def.player_count = 2;
  def.button = 0;
  def.big_blind = 2;
  def.stacks = {2, 2, 0, 0, 0, 0, 0, 0, 0, 0};
  def.contributions = {1, 1, 0, 0, 0, 0, 0, 0, 0, 0};
  def.pot = 2;
  def.board = {card("2s"), card("7h"), card("Kd"), 0, 0};
  def.board_size = 3;
  return def;
}

// Same ranks {2,7,K} with suits permuted out of canonical order: {2h, 7s, Kd}
// has suits {1,0,2}, so the canonical relabel swaps spades and hearts and the
// board is NOT its own class representative.
GameDef two_seat_flop_noncanonical_def_v3() {
  GameDef def = two_seat_flop_canonical_def_v3();
  def.board = {card("2h"), card("7s"), card("Kd"), 0, 0};
  return def;
}

// Two combos per seat, all off the {2,7,K} flop and mutually card-distinct.
// Canonical form: each hand's cards sorted ascending and each seat's range
// sorted by combo id, matching the reader's `ORDER BY player, combo` output
// (the round-trip canonicalizes range order, so the fixture must already be in
// the reader's order for a position-by-position comparison).
std::vector<std::vector<WeightedHand>> two_seat_flop_ranges_v3() {
  return {
      {{{card("As"), card("Ad")}, 2}, {{card("Ah"), card("Ac")}, 3}},
      {{{card("Qs"), card("Qd")}, 5}, {{card("Qh"), card("Qc")}, 7}},
  };
}

// Trains the two-seat canonical flop fixture and exports a concrete
// SeatTrainingResult under the suit-canonicalization card abstraction, the schema-v3
// writer's domain record.
SeatTrainingResult trained_seat_result_v3() {
  const GameDef def = two_seat_flop_canonical_def_v3();
  const bs::tree::AbstractTree tree(def, bs::abstraction::ActionAbstraction::identity());
  const auto ranges = two_seat_flop_ranges_v3();
  const NSeatTrainingResult trained =
      train_nseat(tree, ranges, 200, 20261001, NSeatTrainerLimits{});
  if (trained.termination != NSeatTerminationPhase::Complete)
    throw std::runtime_error("schema-v3 seat fixture training did not complete");
  return export_seat_policy(trained, tree, ranges, bs::abstraction::suit_canonicalization_id());
}

// --- RFC 0007 preflop flop-terminal fixtures --------------------------------

// A trivial frontier evaluator for artifact round-trip tests: every frontier
// leaf has zero chip utility for every seat. This is a valid (if trivial)
// evaluator — the artifact test verifies persistence, not policy quality.
class ZeroFrontierEvaluator : public bs::gto::FrontierEvaluator {
 public:
  std::vector<double> evaluate(std::span<const int> flop, std::span<const std::array<int, 2>> hands,
                               const bs::tree::TerminalPayload& ledger) const override {
    (void)flop;
    (void)hands;
    return std::vector<double>(ledger.player_count, 0.0);
  }
};

// A two-seat preflop flop-terminal game: 3 BB stacks, standard heads-up blinds
// (button posts SB = big_blind/2, other posts BB). The small stack keeps the
// tree and training fast while exercising the full preflop artifact path.
GameDef two_seat_preflop_def_v2() {
  GameDef def{};
  def.player_count = 2;
  def.button = 0;
  def.big_blind = 2;
  def.stacks = {6, 6, 0, 0, 0, 0, 0, 0, 0, 0};
  def.contributions = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
  def.pot = 3;  // SB 1 + BB 2
  def.board = {-1, -1, -1, 0, 0};
  def.board_size = 0;
  def.preflop = true;
  def.blinds_posted = {1, 2, 0, 0, 0, 0, 0, 0, 0, 0};
  def.terminal = TerminalDepth::Flop;
  return def;
}

// Two combos per seat, all mutually card-distinct. Canonical form: each hand's
// cards sorted ascending and each seat's range sorted by combo id, matching the
// reader's `ORDER BY player, combo` output (the round-trip canonicalizes range
// order, so the fixture must already be in the reader's order).
std::vector<std::vector<WeightedHand>> two_seat_preflop_ranges_v2() {
  return {
      {{{card("Ks"), card("Kd")}, 3}, {{card("As"), card("Ad")}, 2}},
      {{{card("Js"), card("Jd")}, 7}, {{card("Qs"), card("Qd")}, 5}},
  };
}

// Trains the two-seat preflop flop-terminal fixture with the zero frontier
// evaluator and exports a concrete SeatTrainingResult, the schema-v2 writer's domain
// record.
SeatTrainingResult trained_preflop_result_v2() {
  const GameDef def = two_seat_preflop_def_v2();
  const bs::tree::AbstractTree tree(def, bs::abstraction::ActionAbstraction::identity());
  const auto ranges = two_seat_preflop_ranges_v2();
  const ZeroFrontierEvaluator frontier;
  const NSeatTrainingResult trained =
      train_nseat(tree, ranges, 200, 20261003, NSeatTrainerLimits{}, &frontier);
  if (trained.termination != NSeatTerminationPhase::Complete)
    throw std::runtime_error("preflop fixture training did not complete");
  return export_seat_policy(trained, tree, ranges);
}

// RFC 0010: a three-seat preflop flop-terminal game (3 BB stacks, blinds
// clockwise of the button). Mirrors the two-seat fixture above so the 3-way
// artifact round-trip exercises the same persistence path.
GameDef three_seat_preflop_def_v2() {
  GameDef def{};
  def.player_count = 3;
  def.button = 0;
  def.big_blind = 2;
  def.stacks = {6, 6, 6, 0, 0, 0, 0, 0, 0, 0};
  def.contributions = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
  def.pot = 3;  // SB 1 + BB 2
  def.board = {-1, -1, -1, 0, 0};
  def.board_size = 0;
  def.preflop = true;
  def.blinds_posted = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
  def.blinds_posted[small_blind_seat(def)] = 1;
  def.blinds_posted[big_blind_seat(def)] = 2;
  def.terminal = TerminalDepth::Flop;
  return def;
}

// Two combos per seat, all mutually card-distinct, in reader-canonical order.
std::vector<std::vector<WeightedHand>> three_seat_preflop_ranges_v2() {
  return {
      {{{card("Ks"), card("Kd")}, 3}, {{card("As"), card("Ad")}, 2}},
      {{{card("Js"), card("Jd")}, 7}, {{card("Qs"), card("Qd")}, 5}},
      {{{card("Ts"), card("Td")}, 11}, {{card("Kh"), card("Ah")}, 13}},
  };
}

SeatTrainingResult trained_preflop_result_v2_3way() {
  const GameDef def = three_seat_preflop_def_v2();
  const bs::tree::AbstractTree tree(def, bs::abstraction::ActionAbstraction::identity());
  const auto ranges = three_seat_preflop_ranges_v2();
  const ZeroFrontierEvaluator frontier;
  const NSeatTrainingResult trained =
      train_nseat(tree, ranges, 200, 20261003, NSeatTrainerLimits{}, &frontier);
  if (trained.termination != NSeatTerminationPhase::Complete)
    throw std::runtime_error("3-way preflop fixture training did not complete");
  return export_seat_policy(trained, tree, ranges);
}

bool same_schedule_v2_exact(const SizeSchedule& a, const SizeSchedule& b) {
  for (std::size_t s = 0; s < 4; ++s) {
    if (a[s].bets.size() != b[s].bets.size() || a[s].raises.size() != b[s].raises.size())
      return false;
    for (std::size_t i = 0; i < a[s].bets.size(); ++i)
      if (!same_fraction_exact(a[s].bets[i], b[s].bets[i]))
        return false;
    for (std::size_t i = 0; i < a[s].raises.size(); ++i)
      if (!same_fraction_exact(a[s].raises[i], b[s].raises[i]))
        return false;
  }
  return true;
}

bool same_ranges_v2_exact(const std::vector<std::vector<WeightedHand>>& a,
                          const std::vector<std::vector<WeightedHand>>& b) {
  if (a.size() != b.size())
    return false;
  for (std::size_t p = 0; p < a.size(); ++p) {
    if (a[p].size() != b[p].size())
      return false;
    for (std::size_t i = 0; i < a[p].size(); ++i)
      if (a[p][i].cards != b[p][i].cards || !same_bits(a[p][i].weight, b[p][i].weight))
        return false;
  }
  return true;
}

// Compare the full seat-generic identity and rows exactly. Two training-time
// stats are deliberately NOT compared against the export's values because the
// artifact does not persist them: a row's visits (the actions table has no
// visits column, so a round-tripped row's visits is 0), and the result's
// information_sets (the manifest stores the concrete stored-state count,
// policy.rows().size(), not the trainer's bucket-keyed count; the reader
// reconstructs that stored count and validates it against the stored states).
int compare_seat_result_exact(const SeatTrainingResult& expected,
                              const SeatTrainingResult& actual) {
  if (!same_game_def(expected.policy.game(), actual.policy.game()))
    return 1;
  if (!(expected.policy.action_id() == actual.policy.action_id()))
    return 1;
  if (!(expected.action_id == actual.action_id))
    return 1;
  if (expected.terminal_depth != actual.terminal_depth)
    return 1;
  if (expected.completed_iterations != actual.completed_iterations)
    return 1;
  if (expected.seed != actual.seed)
    return 1;
  if (expected.prng_state != actual.prng_state)
    return 1;
  if (expected.algorithm_revision != actual.algorithm_revision)
    return 1;
  if (expected.batch_size != actual.batch_size ||
      expected.completed_epochs != actual.completed_epochs)
    return 1;
  if (expected.nodes != actual.nodes)
    return 1;
  if (actual.information_sets != expected.policy.rows().size())
    return 1;
  if (expected.accounted_bytes != actual.accounted_bytes)
    return 1;
  if (!same_schedule_v2_exact(expected.policy.sizes(), actual.policy.sizes()))
    return 1;
  if (!same_ranges_v2_exact(expected.policy.ranges(), actual.policy.ranges()))
    return 1;
  if (expected.policy.rows().size() != actual.policy.rows().size())
    return 1;
  for (const auto& [key, expected_row] : expected.policy.rows()) {
    const auto found = actual.policy.rows().find(key);
    if (found == actual.policy.rows().end())
      return 1;
    if (expected_row.actions != found->second.actions)
      return 1;
    if (expected_row.probabilities.size() != found->second.probabilities.size())
      return 1;
    for (std::size_t i = 0; i < expected_row.probabilities.size(); ++i)
      if (!near(expected_row.probabilities[i], found->second.probabilities[i]))
        return 1;
  }
  return 0;
}

// Rebuild a revision-two seat result around an existing identity with an
// explicit row set. This is the only way to construct content the real
// trainer/export can never produce (writer-side validation tests) or a
// genuinely zero-row checkpoint (the empty-policy publication test);
// SeatPolicy exposes no row mutator on purpose.
SeatTrainingResult seat_result_with_rows(const SeatTrainingResult& base,
                                         const std::map<InformationKey, SeatPolicyRow>& rows) {
  SeatTrainingResult out;
  out.termination = NSeatTerminationPhase::Complete;
  out.algorithm_revision = 2;
  out.batch_size = 1;
  out.completed_iterations = base.completed_iterations;
  out.completed_epochs = base.completed_iterations;  // batch 1: one epoch per sample
  out.seed = base.seed;
  out.prng_state = base.prng_state;
  out.nodes = base.nodes;
  out.action_id = base.action_id;
  out.terminal_depth = base.terminal_depth;
  bs::artifacts::detail::SeatPolicyAssembler::set_identity(
      out.policy, base.policy.game(), base.policy.sizes(), base.policy.ranges(), base.action_id,
      std::nullopt);
  for (const auto& [key, row] : rows)
    bs::artifacts::detail::SeatPolicyAssembler::add_row(out.policy, key, row);
  out.information_sets = rows.size();
  return out;
}

// --- raw sqlite helper (fault injection only; never used by the library) ---

struct RawDb {
  sqlite3* db = nullptr;
  explicit RawDb(const fs::path& path, int flags = SQLITE_OPEN_READWRITE) {
    int rc = sqlite3_open_v2(path.string().c_str(), &db, flags, nullptr);
    if (rc != SQLITE_OK) {
      std::printf("raw open failed: %d\n", rc);
      std::abort();
    }
    exec("PRAGMA foreign_keys=ON;");
  }
  ~RawDb() {
    if (db)
      sqlite3_close_v2(db);
  }
  void exec(const std::string& sql) {
    char* error = nullptr;
    int rc = sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &error);
    if (rc != SQLITE_OK) {
      std::printf("unexpected sqlite failure: %s (%s)\n", error != nullptr ? error : "unknown",
                  sql.c_str());
      sqlite3_free(error);
      std::abort();
    }
    sqlite3_free(error);
  }
  bool try_exec(const std::string& sql) {
    char* error = nullptr;
    int rc = sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &error);
    if (error)
      last_error = error;
    sqlite3_free(error);
    return rc == SQLITE_OK;
  }
  std::int64_t scalar_i64(const std::string& sql) {
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK)
      std::abort();
    int rc = sqlite3_step(stmt);
    std::int64_t value = rc == SQLITE_ROW ? sqlite3_column_int64(stmt, 0) : -1;
    sqlite3_finalize(stmt);
    return value;
  }
  std::string scalar_text(const std::string& sql) {
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK)
      std::abort();
    int rc = sqlite3_step(stmt);
    std::string value =
        rc == SQLITE_ROW ? reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0)) : "";
    sqlite3_finalize(stmt);
    return value;
  }
  std::string last_error;
};

fs::path make_temp_dir() {
  const char* tmp = std::getenv("TMPDIR");
  fs::path base = tmp != nullptr ? fs::path(tmp) : fs::path("/tmp");
  std::string templ = (base / "bs-artifacts-XXXXXX").string();
  std::vector<char> buffer(templ.begin(), templ.end());
  buffer.push_back('\0');
  char* made = mkdtemp(buffer.data());
  CHECK_ABORT(made != nullptr);
  return fs::path(std::string(made));
}

std::string read_file_bytes(const fs::path& path) {
  std::FILE* file = std::fopen(path.string().c_str(), "rb");
  CHECK_ABORT(file != nullptr);
  std::string data;
  std::fseek(file, 0, SEEK_END);
  long size = std::ftell(file);
  std::fseek(file, 0, SEEK_SET);
  data.resize(static_cast<std::size_t>(size));
  CHECK_ABORT(std::fread(data.data(), 1, data.size(), file) == data.size());
  std::fclose(file);
  return data;
}

void write_file_bytes(const fs::path& path, const std::string& data) {
  std::FILE* file = std::fopen(path.string().c_str(), "wb");
  CHECK_ABORT(file != nullptr);
  CHECK_ABORT(std::fwrite(data.data(), 1, data.size(), file) == data.size());
  std::fclose(file);
}

void flip_byte(const fs::path& path, long offset) {
  std::string data = read_file_bytes(path);
  data[static_cast<std::size_t>(offset)] = static_cast<char>(data[offset] ^ 0x5A);
  write_file_bytes(path, data);
}

// Rewrite the unique 8-byte big-endian IEEE encoding of `sentinel` in the
// file's bytes to an arbitrary bit pattern. SQLite serializes REAL cells as
// big-endian doubles, so this plants values (NaN/Inf) that SQL itself refuses
// to bind while leaving b-tree structure intact.
void rewrite_unique_real(const fs::path& path, double sentinel, std::uint64_t bits) {
  std::string data = read_file_bytes(path);
  unsigned char raw[8];
  const auto sentinel_bits = std::bit_cast<std::uint64_t>(sentinel);
  for (int i = 0; i < 8; ++i)
    raw[i] = static_cast<unsigned char>((sentinel_bits >> (56 - 8 * i)) & 0xFF);
  std::size_t occurrences = 0;
  std::size_t position = std::string::npos;
  std::size_t from = 0;
  for (;;) {
    const std::size_t found = data.find(reinterpret_cast<const char*>(raw), from, 8);
    if (found == std::string::npos)
      break;
    position = found;
    ++occurrences;
    from = found + 8;
  }
  CHECK_ABORT(occurrences == 1);
  for (int i = 0; i < 8; ++i)
    data[position + i] = static_cast<char>((bits >> (56 - 8 * i)) & 0xFF);
  write_file_bytes(path, data);
}

// --- deterministic SQLITE_FULL VFS shim ---

namespace fault_vfs {

struct Config {
  bool active = false;
  std::string needle;
  long long budget = 0;
  long long used = 0;
};
Config g_config;

struct FaultFile {
  sqlite3_file base;
  sqlite3_file* real = nullptr;
  std::string name;
};

struct FaultVfs {
  sqlite3_vfs base;
  sqlite3_vfs* real = nullptr;
};
FaultVfs g_vfs;

int ff_close(sqlite3_file* file) {
  auto* f = reinterpret_cast<FaultFile*>(file);
  const int rc = f->real->pMethods->xClose(f->real);
  sqlite3_free(f->real);
  // The FaultFile lives in SQLite-owned storage (placement constructed in
  // xOpen), so destroy in place rather than delete.
  f->~FaultFile();
  return rc;
}
int ff_read(sqlite3_file* file, void* buffer, int amount, sqlite3_int64 offset) {
  auto* f = reinterpret_cast<FaultFile*>(file);
  return f->real->pMethods->xRead(f->real, buffer, amount, offset);
}
int ff_write(sqlite3_file* file, const void* buffer, int amount, sqlite3_int64 offset) {
  auto* f = reinterpret_cast<FaultFile*>(file);
  if (g_config.active && f->name.find(g_config.needle) != std::string::npos) {
    g_config.used += amount;
    if (g_config.used > g_config.budget)
      return SQLITE_FULL;
  }
  return f->real->pMethods->xWrite(f->real, buffer, amount, offset);
}
int ff_truncate(sqlite3_file* file, sqlite3_int64 size) {
  auto* f = reinterpret_cast<FaultFile*>(file);
  return f->real->pMethods->xTruncate(f->real, size);
}
int ff_sync(sqlite3_file* file, int flags) {
  auto* f = reinterpret_cast<FaultFile*>(file);
  return f->real->pMethods->xSync(f->real, flags);
}
int ff_file_size(sqlite3_file* file, sqlite3_int64* size) {
  auto* f = reinterpret_cast<FaultFile*>(file);
  return f->real->pMethods->xFileSize(f->real, size);
}
int ff_lock(sqlite3_file* file, int lock) {
  auto* f = reinterpret_cast<FaultFile*>(file);
  return f->real->pMethods->xLock(f->real, lock);
}
int ff_unlock(sqlite3_file* file, int lock) {
  auto* f = reinterpret_cast<FaultFile*>(file);
  return f->real->pMethods->xUnlock(f->real, lock);
}
int ff_reserved_lock(sqlite3_file* file, int* result) {
  auto* f = reinterpret_cast<FaultFile*>(file);
  return f->real->pMethods->xCheckReservedLock(f->real, result);
}
int ff_file_control(sqlite3_file* file, int op, void* arg) {
  auto* f = reinterpret_cast<FaultFile*>(file);
  return f->real->pMethods->xFileControl(f->real, op, arg);
}
int ff_sector_size(sqlite3_file* file) {
  auto* f = reinterpret_cast<FaultFile*>(file);
  return f->real->pMethods->xSectorSize(f->real);
}
int ff_device_characteristics(sqlite3_file* file) {
  auto* f = reinterpret_cast<FaultFile*>(file);
  return f->real->pMethods->xDeviceCharacteristics(f->real);
}
int ff_shm_map(sqlite3_file* file, int page, int size, int write, void volatile** ptr) {
  auto* f = reinterpret_cast<FaultFile*>(file);
  return f->real->pMethods->xShmMap(f->real, page, size, write, ptr);
}
int ff_shm_lock(sqlite3_file* file, int offset, int count, int flags) {
  auto* f = reinterpret_cast<FaultFile*>(file);
  return f->real->pMethods->xShmLock(f->real, offset, count, flags);
}
void ff_shm_barrier(sqlite3_file* file) {
  auto* f = reinterpret_cast<FaultFile*>(file);
  f->real->pMethods->xShmBarrier(f->real);
}
int ff_shm_unmap(sqlite3_file* file, int delete_flags) {
  auto* f = reinterpret_cast<FaultFile*>(file);
  return f->real->pMethods->xShmUnmap(f->real, delete_flags);
}

sqlite3_io_methods k_io = {
    1,
    ff_close,
    ff_read,
    ff_write,
    ff_truncate,
    ff_sync,
    ff_file_size,
    ff_lock,
    ff_unlock,
    ff_reserved_lock,
    ff_file_control,
    ff_sector_size,
    ff_device_characteristics,
    ff_shm_map,
    ff_shm_lock,
    ff_shm_barrier,
    ff_shm_unmap,
    nullptr,  // xFetch (iVersion 2)
    nullptr,  // xUnfetch (iVersion 2)
};

int vfs_open(sqlite3_vfs*, const char* name, sqlite3_file* file, int flags, int* out_flags) {
  auto* fault_file = new (file) FaultFile();
  fault_file->real = static_cast<sqlite3_file*>(sqlite3_malloc(g_vfs.real->szOsFile));
  if (fault_file->real == nullptr)
    return SQLITE_NOMEM;
  int rc = g_vfs.real->xOpen(g_vfs.real, name, fault_file->real, flags, out_flags);
  if (rc != SQLITE_OK) {
    sqlite3_free(fault_file->real);
    return rc;
  }
  if (name != nullptr)
    fault_file->name = name;
  fault_file->base.pMethods = &k_io;
  (void)out_flags;
  return SQLITE_OK;
}
int vfs_delete(sqlite3_vfs*, const char* name, int sync) {
  return g_vfs.real->xDelete(g_vfs.real, name, sync);
}
int vfs_access(sqlite3_vfs*, const char* name, int flags, int* result) {
  return g_vfs.real->xAccess(g_vfs.real, name, flags, result);
}
int vfs_full_path(sqlite3_vfs*, const char* name, int out, char* result) {
  return g_vfs.real->xFullPathname(g_vfs.real, name, out, result);
}
int vfs_randomness(sqlite3_vfs*, int bytes, char* out) {
  return g_vfs.real->xRandomness(g_vfs.real, bytes, out);
}
int vfs_sleep(sqlite3_vfs*, int micro) {
  return g_vfs.real->xSleep(g_vfs.real, micro);
}
int vfs_current_time(sqlite3_vfs*, double* now) {
  return g_vfs.real->xCurrentTime(g_vfs.real, now);
}
int vfs_current_time64(sqlite3_vfs*, sqlite3_int64* now) {
  return g_vfs.real->xCurrentTimeInt64(g_vfs.real, now);
}
int vfs_get_last_error(sqlite3_vfs*, int bytes, char* message) {
  return g_vfs.real->xGetLastError(g_vfs.real, bytes, message);
}

void install() {
  g_vfs.real = sqlite3_vfs_find(nullptr);
  sqlite3_vfs built{};
  g_vfs.base = built;
  g_vfs.base.iVersion = 2;
  g_vfs.base.szOsFile = sizeof(FaultFile);
  g_vfs.base.mxPathname = g_vfs.real->mxPathname;
  g_vfs.base.zName = "bs_artifact_fault_vfs";
  g_vfs.base.xOpen = vfs_open;
  g_vfs.base.xDelete = vfs_delete;
  g_vfs.base.xAccess = vfs_access;
  g_vfs.base.xFullPathname = vfs_full_path;
  // Dynamic loading is compiled out of the vendored SQLite; leave the VFS
  // extension hooks null.
  g_vfs.base.xDlOpen = nullptr;
  g_vfs.base.xDlError = nullptr;
  g_vfs.base.xDlSym = nullptr;
  g_vfs.base.xDlClose = nullptr;
  g_vfs.base.xRandomness = vfs_randomness;
  g_vfs.base.xSleep = vfs_sleep;
  g_vfs.base.xCurrentTime = vfs_current_time;
  g_vfs.base.xGetLastError = vfs_get_last_error;
  g_vfs.base.xCurrentTimeInt64 = vfs_current_time64;
  CHECK_ABORT(sqlite3_vfs_register(&g_vfs.base, 1) == SQLITE_OK);
}

void arm(const std::string& needle, long long budget) {
  g_config.needle = needle;
  g_config.budget = budget;
  g_config.used = 0;
  g_config.active = true;
}
void disarm() {
  g_config.active = false;
}

}  // namespace fault_vfs

// --- real process-kill crash child ---
//
// Opens the database with its own connection, starts a large multi-page
// transaction, updates a singleton value, and either vanishes before COMMIT
// or commits and vanishes. The parent proves the observable state is always
// exactly old or exactly new, never partial.

int crash_child(const char* path, bool commit) {
  sqlite3* db = nullptr;
  if (sqlite3_open_v2(path, &db, SQLITE_OPEN_READWRITE, nullptr) != SQLITE_OK)
    _exit(40);
  auto exec = [&](const char* sql) {
    if (sqlite3_exec(db, sql, nullptr, nullptr, nullptr) != SQLITE_OK)
      _exit(41);
  };
  exec("PRAGMA journal_mode=DELETE;");
  exec("PRAGMA synchronous=FULL;");
  exec("BEGIN IMMEDIATE;");
  exec("UPDATE manifest SET completed_iterations=999 WHERE id=1;");
  sqlite3_stmt* insert = nullptr;
  if (sqlite3_prepare_v2(db,
                         "INSERT INTO measurements (id, fixture, seed_hex,"
                         " iterations, metric, metric_class, value) VALUES"
                         " (?, ?, NULL, 999, ?, 'exact', 1.0)",
                         -1, &insert, nullptr) != SQLITE_OK)
    _exit(42);
  const std::string padding(64 * 1024, 'z');
  for (int i = 0; i < 300; ++i) {
    sqlite3_reset(insert);
    sqlite3_bind_int64(insert, 1, i + 1);
    const std::string fixture = "crash-fixture-" + std::to_string(i);
    sqlite3_bind_text(insert, 2, fixture.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(insert, 3, padding.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(insert) != SQLITE_DONE)
      _exit(43);
  }
  sqlite3_finalize(insert);
  if (commit)
    exec("COMMIT;");
  _exit(commit ? 28 : 27);
}

int run_crash_round(const fs::path& seed_db, const fs::path& round_db, bool commit,
                    bool delete_journal) {
  std::error_code ec;
  fs::remove(round_db, ec);
  fs::remove(fs::path(round_db.string() + "-journal"), ec);
  fs::copy_file(seed_db, round_db);
  pid_t child = fork();
  if (child == 0)
    crash_child(round_db.string().c_str(), commit);
  CHECK(child > 0);
  int status = 0;
  CHECK(waitpid(child, &status, 0) == child);
  CHECK(WIFEXITED(status));
  CHECK(WEXITSTATUS(status) == static_cast<int>(commit ? 28 : 27));

  if (commit) {
    // Durable new state: the committed sentinel must be observable and the
    // file must be intact.
    RawDb raw(round_db, SQLITE_OPEN_READONLY);
    CHECK(raw.scalar_text("PRAGMA integrity_check") == "ok");
    CHECK(raw.scalar_i64("SELECT completed_iterations FROM manifest WHERE id=1") == 999);
    CHECK(raw.scalar_i64("SELECT COUNT(*) FROM measurements") == 300);
    return 0;
  }

  if (delete_journal) {
    // Worst case: the process died mid-commit AND the rollback journal was
    // lost. Recovery is impossible; the bounded reader must reject the file
    // cleanly rather than expose a torn database.
    fs::remove(fs::path(round_db.string() + "-journal"), ec);
    bool rejected = false;
    try {
      (void)load_artifact(round_db);
    } catch (const ArtifactError&) {
      rejected = true;
    }
    CHECK(rejected);
    return 0;
  }

  // Journal intact: a writable connection performs SQLite's hot-journal
  // rollback and the file must then present the complete old state, never a
  // mixture. Opening the writer is the recovery action a real operator takes;
  // the artifact library itself remains strictly read-only.
  {
    RawDb recovery(round_db, SQLITE_OPEN_READWRITE);
    recovery.exec("SELECT COUNT(*) FROM manifest;");  // forces rollback replay
  }
  CHECK(!fs::exists(fs::path(round_db.string() + "-journal"), ec));
  const LoadedArtifact loaded = load_artifact(round_db);
  CHECK(loaded.bundle.manifest.completed_iterations != 999);
  RawDb raw(round_db, SQLITE_OPEN_READONLY);
  CHECK(raw.scalar_text("PRAGMA integrity_check") == "ok");
  CHECK(raw.scalar_i64("SELECT COUNT(*) FROM measurements") == 0);
  return 0;
}

}  // namespace

// ---------------------------------------------------------------------------
// test 1b: RFC 0009 W2b schema-v2 seat-generic checkpoint round trip
// ---------------------------------------------------------------------------

static int test_nseat_algorithm_identity(const fs::path& dir) {
  const SeatTrainingResult serial = trained_seat_result_v2();
  CHECK(serial.algorithm_revision == 2);
  CHECK(serial.batch_size == 1);
  CHECK(serial.completed_epochs == serial.completed_iterations);
  SeatTrainingResult batched = serial;
  batched.completed_iterations = 201;
  batched.batch_size = 8;
  batched.completed_epochs = 26;
  const std::string batch_identity = "rfc0016-nseat-rev2-frozen-batch-8-sampled-splitmix64";
  const auto batch_path = dir / "batch-eight.db";
  create_checkpoint(batch_path, batched);
  const auto loaded_batch = load_artifact(batch_path);
  CHECK(loaded_batch.bundle.manifest.algorithm_revision == batch_identity);
  CHECK(loaded_batch.bundle.nseat.has_value());
  CHECK(compare_seat_result_exact(batched, *loaded_batch.bundle.nseat) == 0);
  CHECK(probe_artifact(publish_policy(batch_path, dir / "batch-eight-policy.db").path)
            .manifest.algorithm_revision == batch_identity);

  SeatCheckpointProvenance mismatch;
  mismatch.algorithm_revision = kDefaultAlgorithmRevisionV2;
  const auto mismatch_path = dir / "mismatch.db";
  CHECK(throws_artifact([&] { create_checkpoint(mismatch_path, batched, mismatch); },
                        ArtifactErrorKind::InvalidArgument));
  CHECK(!fs::exists(mismatch_path));
  auto wrong_epochs = batched;
  --wrong_epochs.completed_epochs;
  CHECK(throws_artifact([&] { create_checkpoint(mismatch_path, wrong_epochs); },
                        ArtifactErrorKind::InvalidArgument));
  auto zero_batch = serial;
  zero_batch.batch_size = 0;
  CHECK(throws_artifact([&] { create_checkpoint(mismatch_path, zero_batch); },
                        ArtifactErrorKind::InvalidArgument));

  // RFC 0038: the revision-1 (algorithm_revision == 1, batch 0) identity is
  // retired; no writer may produce it and no reader branch recognizes it.
  auto legacy = serial;
  legacy.algorithm_revision = 1;
  legacy.batch_size = 0;
  legacy.completed_epochs = 0;
  CHECK(throws_artifact([&] { create_checkpoint(dir / "legacy-checkpoint.db", legacy); },
                        ArtifactErrorKind::InvalidArgument));

  const std::vector<std::string> unknown_identities = {
      "custom-unrecognized-algorithm",
      "rfc0016-nseat-rev2-frozen-batch-0-sampled-splitmix64",
      "rfc0016-nseat-rev2-frozen-batch-01-sampled-splitmix64",
      "rfc0016-nseat-rev2-frozen-batch-+1-sampled-splitmix64",
      "rfc0016-nseat-rev2-frozen-batch--1-sampled-splitmix64",
      "rfc0016-nseat-rev2-frozen-batch-184467440737095516160-sampled-splitmix64",
      "rfc0016-nseat-rev2-frozen-batch-2-sampled-splitmix64-extra",
  };
  for (std::size_t index = 0; index < unknown_identities.size(); ++index) {
    SeatCheckpointProvenance unknown;
    unknown.algorithm_revision = unknown_identities[index];
    const auto path = dir / ("unknown-" + std::to_string(index) + ".db");
    create_checkpoint(path, serial, unknown);
    const auto published =
        publish_policy(path, dir / ("unknown-policy-" + std::to_string(index) + ".db"));
    const auto loaded = load_artifact(published.path);
    CHECK(loaded.bundle.manifest.algorithm_revision == unknown.algorithm_revision);
    auto expected = serial;
    expected.algorithm_revision = 0;
    expected.batch_size = 0;
    expected.completed_epochs = 0;
    CHECK(compare_seat_result_exact(expected, *loaded.bundle.nseat) == 0);
    // Unknown provenance is preserved when explicitly republished, never
    // silently stamped with the compiler's current revision.
    const auto copy = dir / ("unknown-republished-" + std::to_string(index) + ".db");
    const auto republished = publish_policy(path, copy);
    CHECK(load_artifact(republished.path).bundle.manifest.algorithm_revision ==
          unknown.algorithm_revision);
    CHECK(throws_artifact([&] { create_checkpoint(mismatch_path, *loaded.bundle.nseat); },
                          ArtifactErrorKind::InvalidArgument));
  }
  return 0;
}

static int test_roundtrip_v2(const fs::path& dir) {
  const SeatTrainingResult exported = trained_seat_result_v2();
  CHECK(!exported.policy.rows().empty());
  CHECK(exported.policy.game().player_count == 3);
  CHECK(exported.terminal_depth == TerminalDepth::River);

  const fs::path path = dir / "roundtrip-schema-v2-checkpoint.db";
  SeatCheckpointProvenance provenance;
  provenance.engine_revision = "roundtrip-schema-v2-engine-1";
  create_checkpoint(path, exported, provenance);
  CHECK(throws_artifact([&] { create_checkpoint(path, exported, provenance); },
                        ArtifactErrorKind::AlreadyExists));

  // Independent schema oracle through a raw connection.
  {
    const std::string header = read_file_bytes(path).substr(0, 16);
    CHECK(header == std::string("SQLite format 3\000", 16));
    RawDb raw(path, SQLITE_OPEN_READONLY);
    CHECK(raw.scalar_i64("PRAGMA application_id") == static_cast<std::int64_t>(0x42534754));
    CHECK(raw.scalar_i64("PRAGMA user_version") == 2);
    CHECK(raw.scalar_text("PRAGMA journal_mode") == "delete");
    CHECK(raw.scalar_text("PRAGMA synchronous") == "2");

    sqlite3_stmt* tables = nullptr;
    CHECK(sqlite3_prepare_v2(raw.db,
                             "SELECT name, sql FROM sqlite_schema WHERE type='table'"
                             " ORDER BY name",
                             -1, &tables, nullptr) == SQLITE_OK);
    const std::vector<std::string> expected{
        "actions",  "bounds",       "game",   "game_seats", "information_states",
        "manifest", "measurements", "ranges", "sizes",      "training"};
    std::vector<std::string> seen;
    while (sqlite3_step(tables) == SQLITE_ROW) {
      const std::string name = reinterpret_cast<const char*>(sqlite3_column_text(tables, 0));
      const std::string sql = reinterpret_cast<const char*>(sqlite3_column_text(tables, 1));
      seen.push_back(name);
      CHECK(sql.size() >= 6 && sql.substr(sql.size() - 6) == "STRICT");
    }
    sqlite3_finalize(tables);
    CHECK(seen == expected);

    std::size_t expected_sizes = 0;
    for (const auto& street : exported.policy.sizes())
      expected_sizes += street.bets.size() + street.raises.size();
    CHECK(raw.scalar_i64("SELECT COUNT(*) FROM sizes") ==
          static_cast<std::int64_t>(expected_sizes));
    CHECK(raw.scalar_i64("SELECT COUNT(*) FROM ranges") == 6);
    CHECK(raw.scalar_i64("SELECT COUNT(*) FROM game_seats") == 3);
    CHECK(raw.scalar_i64("SELECT COUNT(*) FROM information_states") ==
          static_cast<std::int64_t>(exported.policy.rows().size()));
    CHECK(raw.scalar_i64("SELECT COUNT(*) FROM training") == 0);

    sqlite3_stmt* sums = nullptr;
    CHECK(sqlite3_prepare_v2(raw.db,
                             "SELECT SUM(probability) FROM actions GROUP BY info_id"
                             " HAVING ABS(SUM(probability) - 1.0) > 1e-12",
                             -1, &sums, nullptr) == SQLITE_OK);
    CHECK(sqlite3_step(sums) == SQLITE_DONE);
    sqlite3_finalize(sums);

    // Manifest and game identity carry the schema-v2 declarations.
    CHECK(raw.scalar_i64("SELECT info_key_revision FROM manifest") == 2);
    CHECK(raw.scalar_text("SELECT numeric_profile FROM manifest") == kNumericProfileV2);
    CHECK(raw.scalar_i64("SELECT terminal_depth FROM manifest") ==
          static_cast<std::int64_t>(TerminalDepth::River));
    CHECK(raw.scalar_i64("SELECT abstraction_version FROM manifest") ==
          static_cast<std::int64_t>(exported.action_id.version));
    CHECK(!raw.scalar_text("SELECT abstraction_name FROM manifest").empty());
    CHECK(raw.scalar_text("SELECT rules_id FROM game") == kRulesIdentifierV2);
    CHECK(raw.scalar_i64("SELECT player_count FROM game") == 3);
    CHECK(raw.scalar_i64("SELECT root_street FROM game") == 2);  // board_size 5 -> river
    CHECK(raw.scalar_i64("SELECT terminal_depth FROM game") ==
          static_cast<std::int64_t>(TerminalDepth::River));
    CHECK(raw.scalar_text("SELECT utility_id FROM game") == kUtilityIdentifier);
  }

  // Same-process lossless read.
  const LoadedArtifact loaded = load_artifact(path);
  CHECK(loaded.sha256_hex.size() == 64);
  CHECK(loaded.file_bytes > 0);
  const auto& bundle = loaded.bundle;
  CHECK(bundle.manifest.kind == ArtifactKind::Checkpoint);
  CHECK(bundle.manifest.information_key_revision == 2);
  CHECK(bundle.manifest.engine_revision == "roundtrip-schema-v2-engine-1");
  CHECK(bundle.manifest.completed_iterations == exported.completed_iterations);
  CHECK(bundle.manifest.terminal_depth == TerminalDepth::River);
  CHECK(bundle.nseat.has_value());
  if (compare_seat_result_exact(exported, *bundle.nseat) != 0) {
    std::printf("schema-v2 roundtrip seat-result comparison failed\n");
    return 1;
  }
  for (const auto& [key, row] : bundle.nseat->policy.rows()) {
    (void)row;
    CHECK(encode_public_key(decode_public_key(encode_public_key(key), static_cast<int>(key[0]),
                                              static_cast<int>(key[1]),
                                              static_cast<int>(key[2]))) == encode_public_key(key));
  }

  // The schema-v2 file is byte-stable: a second write of the same result has the same
  // SHA-256 (rollback journal mode, deterministic page allocation).
  const fs::path path2 = dir / "roundtrip-schema-v2-checkpoint-2.db";
  create_checkpoint(path2, exported, provenance);
  CHECK(sha256_file_hex(path) == sha256_file_hex(path2));

  // RFC 0009 W2c: probe_artifact and publish_policy now support schema schema-v2. The
  // probe carries the seat-generic identity (game_def/ranges/sizes); the
  // resident projection of a two-seat flop-rooted schema-v2 source onto the heads-up
  // view is exercised by the resident-policy test, not here.
  const fs::path policy_dir = dir / "policy-schema-v2";
  fs::create_directory(policy_dir);
  const fs::path policy_path = policy_dir / "generation-schema-v2-0001.db";
  const PublishedPolicy published_v2 =
      publish_policy(path, policy_path, "publish-schema-v2-engine-1");
  CHECK(published_v2.sha256_hex == sha256_file_hex(policy_path));
  CHECK(published_v2.file_bytes == fs::file_size(policy_path));

  const ArtifactProbe probe = probe_artifact(policy_path);
  CHECK(probe.manifest.kind == ArtifactKind::Policy);
  CHECK(probe.manifest.validation == ValidationState::Validated);
  CHECK(probe.manifest.information_key_revision == 2);
  CHECK(probe.game_def.has_value());
  CHECK(probe.game_def->player_count == 3);
  CHECK(probe.game_def->board_size == 5);
  CHECK(probe.ranges.has_value());
  CHECK(probe.ranges->size() == 3);
  CHECK(probe.sizes.has_value());
  CHECK(*probe.sizes == exported.policy.sizes());
  CHECK(probe.information_sets == exported.policy.rows().size());
  CHECK(probe.action_count >= probe.information_sets);

  // The published policy round-trips through the schema-v2 loader.
  const LoadedArtifact published_loaded =
      load_artifact(policy_path, LoadOptions{published_v2.file_bytes, published_v2.sha256});
  CHECK(published_loaded.bundle.manifest.kind == ArtifactKind::Policy);
  CHECK(published_loaded.bundle.nseat.has_value());
  if (compare_seat_result_exact(exported, *published_loaded.bundle.nseat) != 0) {
    std::printf("schema-v2 published-policy seat-result comparison failed\n");
    return 1;
  }

  // RFC 0009 D3: the reader validates player < player_count in C++, because a
  // SQL CHECK is write-time-only and can be stripped from a tampered file. A
  // state whose player is inside the 0..9 digit domain but outside the 3-seat
  // game is rejected.
  {
    const fs::path tampered = dir / "roundtrip-schema-v2-tampered-player.db";
    fs::copy_file(path, tampered);
    {
      RawDb raw(tampered);
      raw.exec(
          "UPDATE information_states SET player = 7 WHERE id ="
          " (SELECT MIN(id) FROM information_states);");
    }
    CHECK(
        throws_artifact([&] { (void)load_artifact(tampered); }, ArtifactErrorKind::InvalidSchema));
  }

  // The bounds (seat, combo) dimension gets the same player_count bound.
  {
    const fs::path tampered = dir / "roundtrip-schema-v2-tampered-seat.db";
    fs::copy_file(path, tampered);
    {
      RawDb raw(tampered);
      raw.exec(
          "INSERT INTO bounds (public_root, seat, combo) VALUES"
          " ('tamper-root', 7, 0);");
    }
    CHECK(
        throws_artifact([&] { (void)load_artifact(tampered); }, ArtifactErrorKind::InvalidSchema));
  }
  return 0;
}

// ---------------------------------------------------------------------------
// test 1c: RFC 0009 W4c schema-v3 card-abstraction checkpoint round trip
// ---------------------------------------------------------------------------

static int test_roundtrip_v3(const fs::path& dir) {
  const SeatTrainingResult exported = trained_seat_result_v3();
  CHECK(!exported.policy.rows().empty());
  CHECK(exported.policy.game().player_count == 2);
  CHECK(exported.card_id.has_value());
  CHECK(exported.card_id == bs::abstraction::suit_canonicalization_id());
  CHECK(exported.policy.card_id() == bs::abstraction::suit_canonicalization_id());
  // The stored flop is the canonical class representative.
  const std::array<int, 3> flop{exported.policy.game().board[0], exported.policy.game().board[1],
                                exported.policy.game().board[2]};
  CHECK(bs::abstraction::canonicalize(flop).board == flop);

  const fs::path path = dir / "roundtrip-schema-v3-checkpoint.db";
  SeatCheckpointProvenance provenance;
  provenance.engine_revision = "roundtrip-schema-v3-engine-1";
  create_checkpoint(path, exported, provenance);
  CHECK(throws_artifact([&] { create_checkpoint(path, exported, provenance); },
                        ArtifactErrorKind::AlreadyExists));

  // Independent schema oracle through a raw connection.
  {
    RawDb raw(path, SQLITE_OPEN_READONLY);
    CHECK(raw.scalar_i64("PRAGMA application_id") == static_cast<std::int64_t>(0x42534754));
    CHECK(raw.scalar_i64("PRAGMA user_version") == 3);
    CHECK(raw.scalar_text("SELECT card_abstraction_name FROM manifest") == exported.card_id->name);
    CHECK(raw.scalar_i64("SELECT card_abstraction_version FROM manifest") ==
          static_cast<std::int64_t>(exported.card_id->version));
    CHECK(raw.scalar_text("SELECT card_abstraction_parameters FROM manifest") ==
          exported.card_id->parameters);
    CHECK(raw.scalar_text("SELECT card_abstraction_digest FROM manifest") ==
          bs::artifacts::detail::u64_to_hex(exported.card_id->digest));
    // The schema-v2 action-abstraction columns are still present and correct.
    CHECK(raw.scalar_i64("SELECT abstraction_version FROM manifest") ==
          static_cast<std::int64_t>(exported.action_id.version));
    CHECK(raw.scalar_i64("SELECT terminal_depth FROM manifest") ==
          static_cast<std::int64_t>(TerminalDepth::River));
    // The schema-v3 manifest has exactly the schema-v2 table set plus the four card columns.
    CHECK(raw.scalar_i64("SELECT COUNT(*) FROM pragma_table_info('manifest')") == 24);
  }

  // Same-process lossless read.
  const LoadedArtifact loaded = load_artifact(path);
  CHECK(loaded.sha256_hex.size() == 64);
  const auto& bundle = loaded.bundle;
  CHECK(bundle.manifest.kind == ArtifactKind::Checkpoint);
  CHECK(bundle.manifest.information_key_revision == 2);
  CHECK(bundle.manifest.engine_revision == "roundtrip-schema-v3-engine-1");
  CHECK(bundle.manifest.card_abstraction.has_value());
  CHECK(bundle.manifest.card_abstraction == bs::abstraction::suit_canonicalization_id());
  CHECK(bundle.nseat.has_value());
  CHECK(bundle.nseat->card_id.has_value());
  CHECK(bundle.nseat->card_id == bs::abstraction::suit_canonicalization_id());
  CHECK(bundle.nseat->policy.card_id() == bs::abstraction::suit_canonicalization_id());
  if (compare_seat_result_exact(exported, *bundle.nseat) != 0) {
    std::printf("schema-v3 roundtrip seat-result comparison failed\n");
    return 1;
  }

  // Byte-stability: a second write of the same result has the same SHA-256.
  const fs::path path2 = dir / "roundtrip-schema-v3-checkpoint-2.db";
  create_checkpoint(path2, exported, provenance);
  CHECK(sha256_file_hex(path) == sha256_file_hex(path2));

  // Probe and publish expose the card abstraction.
  const fs::path policy_dir = dir / "policy-schema-v3";
  fs::create_directory(policy_dir);
  const fs::path policy_path = policy_dir / "generation-schema-v3-0001.db";
  const PublishedPolicy published_v3 =
      publish_policy(path, policy_path, "publish-schema-v3-engine-1");
  CHECK(published_v3.sha256_hex == sha256_file_hex(policy_path));
  CHECK(published_v3.file_bytes == fs::file_size(policy_path));

  const ArtifactProbe probe = probe_artifact(policy_path);
  CHECK(probe.manifest.kind == ArtifactKind::Policy);
  CHECK(probe.manifest.validation == ValidationState::Validated);
  CHECK(probe.manifest.card_abstraction.has_value());
  CHECK(probe.manifest.card_abstraction == bs::abstraction::suit_canonicalization_id());
  CHECK(probe.game_def.has_value());
  CHECK(probe.game_def->player_count == 2);
  CHECK(probe.game_def->board_size == 3);
  CHECK(probe.ranges.has_value());
  CHECK(probe.ranges->size() == 2);
  CHECK(probe.sizes.has_value());
  CHECK(probe.information_sets == exported.policy.rows().size());

  // The published policy round-trips through the schema-v3 loader.
  const LoadedArtifact published_loaded =
      load_artifact(policy_path, LoadOptions{published_v3.file_bytes, published_v3.sha256});
  CHECK(published_loaded.bundle.manifest.kind == ArtifactKind::Policy);
  CHECK(published_loaded.bundle.nseat.has_value());
  CHECK(published_loaded.bundle.nseat->card_id == bs::abstraction::suit_canonicalization_id());
  if (compare_seat_result_exact(exported, *published_loaded.bundle.nseat) != 0) {
    std::printf("schema-v3 published-policy seat-result comparison failed\n");
    return 1;
  }

  // Digest verification: a tampered card_abstraction_digest is rejected.
  {
    const fs::path tampered = dir / "roundtrip-schema-v3-tampered-digest.db";
    fs::copy_file(path, tampered);
    {
      RawDb raw(tampered);
      raw.exec("UPDATE manifest SET card_abstraction_digest = '0000000000000000';");
    }
    CHECK(
        throws_artifact([&] { (void)load_artifact(tampered); }, ArtifactErrorKind::InvalidSchema));
  }

  // Board-canonical integrity: a class policy trained on a non-canonical board
  // but declaring suit-canonicalization is rejected at create time.
  {
    const GameDef bad_def = two_seat_flop_noncanonical_def_v3();
    const std::array<int, 3> bad_flop{bad_def.board[0], bad_def.board[1], bad_def.board[2]};
    CHECK(bs::abstraction::canonicalize(bad_flop).board != bad_flop);
    const bs::tree::AbstractTree tree(bad_def, bs::abstraction::ActionAbstraction::identity());
    const auto ranges = two_seat_flop_ranges_v3();
    const NSeatTrainingResult trained =
        train_nseat(tree, ranges, 200, 20261001, NSeatTrainerLimits{});
    CHECK(trained.termination == NSeatTerminationPhase::Complete);
    const SeatTrainingResult bad_export =
        export_seat_policy(trained, tree, ranges, bs::abstraction::suit_canonicalization_id());
    const fs::path bad_path = dir / "roundtrip-schema-v3-noncanonical.db";
    CHECK(throws_artifact([&] { create_checkpoint(bad_path, bad_export, provenance); },
                          ArtifactErrorKind::InvalidArgument));
  }

  // A board-class (schema-v3) artifact cannot carry a conditional fixed
  // runout: it serves an abstract board class, not one concrete runout.
  // 9h is off the {2s,7h,Kd} flop, so only the v3-specific guard rejects.
  {
    const fs::path tampered = dir / "roundtrip-schema-v3-runout-ck.db";
    fs::copy_file(path, tampered);
    {
      RawDb raw(tampered);
      raw.exec("UPDATE game SET fixed_turn = 30;");  // 9h
    }
    CHECK(
        throws_artifact([&] { (void)load_artifact(tampered); }, ArtifactErrorKind::InvalidSchema));
  }
  {
    const fs::path tampered = dir / "roundtrip-schema-v3-runout-policy.db";
    fs::copy_file(policy_path, tampered);
    chmod(tampered.string().c_str(), S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
    {
      RawDb raw(tampered);
      raw.exec("UPDATE game SET fixed_turn = 30;");  // 9h
    }
    CHECK(
        throws_artifact([&] { (void)load_artifact(tampered); }, ArtifactErrorKind::InvalidSchema));
    CHECK(
        throws_artifact([&] { (void)probe_artifact(tampered); }, ArtifactErrorKind::InvalidSchema));
  }
  return 0;
}

// ---------------------------------------------------------------------------
// test 1c: RFC 0007 preflop flop-terminal checkpoint round trip
// ---------------------------------------------------------------------------

static int test_roundtrip_preflop(const fs::path& dir) {
  const SeatTrainingResult exported = trained_preflop_result_v2();
  CHECK(!exported.policy.rows().empty());
  CHECK(exported.policy.game().player_count == 2);
  CHECK(exported.policy.game().board_size == 0);
  CHECK(exported.policy.game().preflop);
  CHECK(exported.terminal_depth == TerminalDepth::Flop);

  const fs::path path = dir / "roundtrip-preflop-checkpoint.db";
  SeatCheckpointProvenance provenance;
  provenance.engine_revision = "roundtrip-preflop-engine-1";
  create_checkpoint(path, exported, provenance);
  CHECK(throws_artifact([&] { create_checkpoint(path, exported, provenance); },
                        ArtifactErrorKind::AlreadyExists));

  // Independent schema oracle through a raw connection.
  {
    RawDb raw(path, SQLITE_OPEN_READONLY);
    CHECK(raw.scalar_i64("PRAGMA user_version") == 2);
    CHECK(raw.scalar_text("SELECT rules_id FROM game") == kRulesIdentifierPreflop);
    CHECK(raw.scalar_i64("SELECT player_count FROM game") == 2);
    CHECK(raw.scalar_i64("SELECT root_street FROM game") == 3);  // preflop
    CHECK(raw.scalar_i64("SELECT terminal_depth FROM game") ==
          static_cast<std::int64_t>(TerminalDepth::Flop));
    CHECK(raw.scalar_i64("SELECT flop0 FROM game") == 0);  // stored but never read
    CHECK(raw.scalar_i64("SELECT flop1 FROM game") == 0);
    CHECK(raw.scalar_i64("SELECT flop2 FROM game") == 0);
    CHECK(raw.scalar_text("SELECT utility_id FROM game") == kUtilityIdentifier);
    CHECK(raw.scalar_i64("SELECT COUNT(*) FROM game_seats") == 2);
    CHECK(raw.scalar_i64("SELECT COUNT(*) FROM information_states") ==
          static_cast<std::int64_t>(exported.policy.rows().size()));
    CHECK(raw.scalar_i64("SELECT COUNT(*) FROM training") == 0);
  }

  // Same-process lossless read.
  const LoadedArtifact loaded = load_artifact(path);
  CHECK(loaded.bundle.nseat.has_value());
  const GameDef& read_game = loaded.bundle.nseat->policy.game();
  CHECK(read_game.player_count == 2);
  CHECK(read_game.board_size == 0);
  CHECK(read_game.preflop);
  CHECK(read_game.terminal == TerminalDepth::Flop);
  CHECK(read_game.big_blind == 2);
  CHECK(read_game.blinds_posted[0] == 1);  // button posts SB
  CHECK(read_game.blinds_posted[1] == 2);  // other posts BB
  CHECK(read_game.pot == 3);
  if (compare_seat_result_exact(exported, *loaded.bundle.nseat) != 0) {
    std::printf("preflop roundtrip seat-result comparison failed\n");
    return 1;
  }

  // Byte stability: a second write of the same result has the same SHA-256.
  const fs::path path2 = dir / "roundtrip-preflop-checkpoint-2.db";
  create_checkpoint(path2, exported, provenance);
  CHECK(sha256_file_hex(path) == sha256_file_hex(path2));

  // Publish and probe.
  const fs::path policy_dir = dir / "policy-preflop";
  fs::create_directory(policy_dir);
  const fs::path policy_path = policy_dir / "generation-preflop-0001.db";
  const PublishedPolicy published = publish_policy(path, policy_path, "publish-preflop-1");
  CHECK(published.sha256_hex == sha256_file_hex(policy_path));

  const ArtifactProbe probe = probe_artifact(policy_path);
  CHECK(probe.manifest.kind == ArtifactKind::Policy);
  CHECK(probe.manifest.validation == ValidationState::Validated);
  CHECK(probe.game_def.has_value());
  CHECK(probe.game_def->player_count == 2);
  CHECK(probe.game_def->board_size == 0);
  CHECK(probe.game_def->preflop);
  CHECK(probe.game_def->terminal == TerminalDepth::Flop);
  CHECK(probe.ranges.has_value());
  CHECK(probe.ranges->size() == 2);
  CHECK(probe.sizes.has_value());
  CHECK(probe.information_sets == exported.policy.rows().size());

  // The published policy round-trips through the schema-v2 loader.
  const LoadedArtifact published_loaded =
      load_artifact(policy_path, LoadOptions{published.file_bytes, published.sha256});
  CHECK(published_loaded.bundle.manifest.kind == ArtifactKind::Policy);
  CHECK(published_loaded.bundle.nseat.has_value());
  if (compare_seat_result_exact(exported, *published_loaded.bundle.nseat) != 0) {
    std::printf("preflop published-policy seat-result comparison failed\n");
    return 1;
  }

  // A flop-rooted schema-v2 artifact must NOT match the preflop rules identifier and
  // vice versa: the reader dispatches on rules_id, so a preflop root_street
  // with the flop-rooted rules_id is rejected as UnsupportedVersion.
  {
    const fs::path tampered = dir / "roundtrip-preflop-tampered-rules.db";
    fs::copy_file(path, tampered);
    {
      RawDb raw(tampered, SQLITE_OPEN_READWRITE);
      raw.exec("UPDATE game SET rules_id = 'rfc0009-unified-flop-schema-v1' WHERE id = 1");
    }
    CHECK(throws_artifact([&] { (void)load_artifact(tampered); },
                          ArtifactErrorKind::UnsupportedVersion));
  }

  return 0;
}

// ---------------------------------------------------------------------------
// test 1d: RFC 0010 3-way preflop flop-terminal checkpoint round trip
// ---------------------------------------------------------------------------

static int test_roundtrip_preflop_3way(const fs::path& dir) {
  const SeatTrainingResult exported = trained_preflop_result_v2_3way();
  CHECK(!exported.policy.rows().empty());
  CHECK(exported.policy.game().player_count == 3);
  CHECK(exported.policy.game().board_size == 0);
  CHECK(exported.policy.game().preflop);
  CHECK(exported.terminal_depth == TerminalDepth::Flop);

  const fs::path path = dir / "roundtrip-preflop-3way-checkpoint.db";
  SeatCheckpointProvenance provenance;
  provenance.engine_revision = "roundtrip-preflop-3way-engine-1";
  create_checkpoint(path, exported, provenance);

  // Independent schema oracle through a raw connection.
  {
    RawDb raw(path, SQLITE_OPEN_READONLY);
    CHECK(raw.scalar_i64("PRAGMA user_version") == 2);
    // The writer stores the preflop rules identifier for all seat counts;
    // the RFC 0010 multiway identifier lives in the builder's manifest, not
    // in the artifact DB (the reader dispatches on this single identifier).
    CHECK(raw.scalar_text("SELECT rules_id FROM game") == kRulesIdentifierPreflop);
    CHECK(raw.scalar_i64("SELECT player_count FROM game") == 3);
    CHECK(raw.scalar_i64("SELECT root_street FROM game") == 3);  // preflop
    CHECK(raw.scalar_i64("SELECT terminal_depth FROM game") ==
          static_cast<std::int64_t>(TerminalDepth::Flop));
    CHECK(raw.scalar_i64("SELECT COUNT(*) FROM game_seats") == 3);
    CHECK(raw.scalar_i64("SELECT COUNT(*) FROM information_states") ==
          static_cast<std::int64_t>(exported.policy.rows().size()));
  }

  // Same-process lossless read: the 3-way GameDef round-trips exactly.
  const LoadedArtifact loaded = load_artifact(path);
  CHECK(loaded.bundle.nseat.has_value());
  const GameDef& read_game = loaded.bundle.nseat->policy.game();
  CHECK(read_game.player_count == 3);
  CHECK(read_game.board_size == 0);
  CHECK(read_game.preflop);
  CHECK(read_game.terminal == TerminalDepth::Flop);
  CHECK(read_game.big_blind == 2);
  CHECK(read_game.pot == 3);
  // 3-seat blinds: button=0, SB=1, BB=2 (clockwise of button).
  CHECK(read_game.blinds_posted[0] == 0);
  CHECK(read_game.blinds_posted[1] == 1);
  CHECK(read_game.blinds_posted[2] == 2);
  if (compare_seat_result_exact(exported, *loaded.bundle.nseat) != 0) {
    std::printf("3-way preflop roundtrip seat-result comparison failed\n");
    return 1;
  }

  return 0;
}

// ---------------------------------------------------------------------------
// test 2: canonical key oracle for the single revision-2 grammar
// (hand-authored bytes + malformed rejection). RFC 0038 removed the
// revision-1 three-field event grammar; every event is actor:kind:target:street.
// ---------------------------------------------------------------------------

static int test_canonical_keys(const fs::path&) {
  HeadsUpRoot root{{card("2c"), card("3d"), card("7h")}, {5, 5}, {1, 1}, 2, 1, 1};
  HeadsUpState state(root);
  const std::array<int, 2> own{card("Ac"), card("Ad")};  // 50,51
  const int turn = card("9h");                           // 30
  const int river = card("8s");                          // 24
  auto decode = [&](const std::string& text, int player) {
    auto sorted = own;
    std::sort(sorted.begin(), sorted.end());
    return decode_public_key(text, player, sorted[0], sorted[1]);
  };

  const InformationKey k_root = information_key(state, own);
  CHECK(encode_public_key(k_root) == "0|3,6,21|");
  CHECK(decode("0|3,6,21|", 0) == k_root);

  HeadsUpState after_check = state.after_action(0, {ActionType::Check});
  const InformationKey k_check = information_key(after_check, own);
  CHECK(encode_public_key(k_check) == "0|3,6,21|0:x:-:0");
  CHECK(decode("0|3,6,21|0:x:-:0", 1) == k_check);

  HeadsUpState turn_state = after_check.after_action(1, {ActionType::Check}).after_card(turn);
  const InformationKey k_turn = information_key(turn_state, own);
  CHECK(encode_public_key(k_turn) == "1|3,6,21,29|0:x:-:0,1:x:-:0");
  CHECK(decode("1|3,6,21,29|0:x:-:0,1:x:-:0", 0) == k_turn);

  HeadsUpState river_state = turn_state.after_action(0, {ActionType::Check})
                                 .after_action(1, {ActionType::Check})
                                 .after_card(river);
  const InformationKey k_river = information_key(river_state, own);
  CHECK(encode_public_key(k_river) == "2|3,6,21,29,24|0:x:-:0,1:x:-:0,0:x:-:1,1:x:-:1");
  CHECK(decode("2|3,6,21,29,24|0:x:-:0,1:x:-:0,0:x:-:1,1:x:-:1", 0) == k_river);

  HeadsUpState after_bet = state.after_action(0, {ActionType::Bet, 2});
  CHECK(encode_public_key(information_key(after_bet, own)) == "0|3,6,21|0:b:2:0");
  HeadsUpState after_raise = after_bet.after_action(1, {ActionType::Raise, 4});
  CHECK(encode_public_key(information_key(after_raise, own)) == "0|3,6,21|0:b:2:0,1:r:4:0");

  // A fold carries its street like every other event. In the seat-generic
  // grammar a fold is NOT terminal: three-or-more-seat hands continue after
  // one seat folds, so events may follow.
  CHECK(encode_public_key(decode("0|3,6,21|0:f:-:0", 0)) == "0|3,6,21|0:f:-:0");
  CHECK(encode_public_key(decode("0|3,6,21|0:f:-:0,1:x:-:0", 0)) == "0|3,6,21|0:f:-:0,1:x:-:0");

  // The preflop form: street token 3, empty board field; preflop events use
  // street 3 and the single allowed decrease wraps 3 -> 0 (preflop to flop).
  {
    const InformationKey preflop = decode("3||", 0);
    CHECK(encode_public_key(preflop) == "3||");
    CHECK(encode_public_key(decode("3||3:x:-:3", 0)) == "3||3:x:-:3");
    CHECK(encode_public_key(decode("3||3:x:-:3,0:x:-:0", 0)) == "3||3:x:-:3,0:x:-:0");
  }

  const std::vector<std::string> malformed{
      "4|3,6,21|",                                  // bad street
      "0|3,6,21,29|",                               // boards disagree with street
      "0|52,6,21|",                                 // bad card
      "0|3,6,21|10:x:-:0",                          // bad actor (seats are 0..9)
      "0|3,6,21|0:b:-:0",                           // aggressive needs target
      "0|3,6,21|0:x:1:0",                           // check must not carry target
      "0|3,6,21|0:d:-:0",                           // unknown event kind
      "0|3,6,21|0:x",                               // missing event field
      "0|3,6,21|0:b:02:0",                          // leading zero
      "0|3,6,21|0:b:9007199254740992:0",            // over 2^53-1
      "0|3,6,21|0:x:-:0,1:x:-:0,0:x:-:1,1:x:-:1|",  // extra field
      "0|3,6,21|0:x:-:1",                           // event street exceeds key street
      "1|3,6,21,29|0:x:-:1,1:x:-:0",                // event streets out of order
      "3|3,6,21|",                                  // preflop key must have empty board field
      "3||0:x:-:1",                                 // preflop event street below 3 is not a wrap
      "3||3:x:-:4",                                 // event street above maximum
  };
  for (const auto& text : malformed)
    CHECK(throws_artifact([&] { (void)decode(text, 0); }, ArtifactErrorKind::InvalidSchema));

  // Own combo shares a board card: rejected even with otherwise valid text.
  CHECK(throws_artifact([&] { (void)decode_public_key("0|3,6,21|", 0, card("2c"), 51); },
                        ArtifactErrorKind::InvalidSchema));
  return 0;
}

// ---------------------------------------------------------------------------
// test 3 (RFC 0038): schema-v1 and unknown-version rejection. The version
// gate runs before schema-object validation, so an empty database carrying
// only the application id and a retired user_version must be refused solely
// on its version, by both load and probe.
// ---------------------------------------------------------------------------

static int test_v1_rejection(const fs::path& dir) {
  for (const int version : {1, 0, 99}) {
    const fs::path path = dir / ("user-version-" + std::to_string(version) + ".db");
    {
      RawDb raw(path, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE);
      raw.exec("PRAGMA application_id = 1112754004;");
      raw.exec("PRAGMA user_version = " + std::to_string(version) + ";");
    }
    CHECK(
        throws_artifact([&] { (void)load_artifact(path); }, ArtifactErrorKind::UnsupportedVersion));
    CHECK(throws_artifact([&] { (void)probe_artifact(path); },
                          ArtifactErrorKind::UnsupportedVersion));
  }
  // An unexpected application id is refused with the same kind regardless of
  // the declared version.
  {
    const fs::path path = dir / "bad-appid.db";
    {
      RawDb raw(path, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE);
      raw.exec("PRAGMA application_id = 1;");
      raw.exec("PRAGMA user_version = 2;");
    }
    CHECK(
        throws_artifact([&] { (void)load_artifact(path); }, ArtifactErrorKind::UnsupportedVersion));
    CHECK(throws_artifact([&] { (void)probe_artifact(path); },
                          ArtifactErrorKind::UnsupportedVersion));
  }
  return 0;
}

// ---------------------------------------------------------------------------
// test 4: ENOSPC fault injection and real killed-writer rounds against
// schema-v2 checkpoints (RFC 0038 counterpart of the retired v1 split/commit
// fault cases). A seat checkpoint is an immutable snapshot: resumable
// commits do not exist, so the writer's only multi-step publication is
// publish_policy, whose atomic temp-file/link transaction is faulted here.
// ---------------------------------------------------------------------------

static int test_crash_and_full_disk(const fs::path& dir) {
  const SeatTrainingResult exported = trained_seat_result_v2();
  SeatCheckpointProvenance provenance;
  provenance.engine_revision = "crash-engine-1";

  const fs::path good = dir / "crash-good.db";
  create_checkpoint(good, exported, provenance);

  // ENOSPC at create time: the failed create removes its database file and no
  // readable artifact is left. Budgets stay at one page or below so they fail
  // every nonempty database regardless of its final size.
  for (const long long budget : {1LL, 4096LL}) {
    const fs::path starved = dir / ("starved-create-" + std::to_string(budget) + ".db");
    fault_vfs::arm(starved.string(), budget);
    bool threw = false;
    try {
      create_checkpoint(starved, exported, provenance);
    } catch (const ArtifactError& error) {
      threw = true;
      CHECK(error.kind() == ArtifactErrorKind::Io);
    }
    fault_vfs::disarm();
    CHECK(threw);
    std::error_code ec;
    fs::remove(fs::path(starved.string() + "-journal"), ec);
    fs::remove(fs::path(starved.string() + "-wal"), ec);
    fs::remove(fs::path(starved.string() + "-shm"), ec);
    CHECK(!fs::exists(starved, ec));
  }

  // ENOSPC during immutable publication: publish_policy throws Io, leaves no
  // generation at the destination, and removes its .bs-policy temp file.
  for (const long long budget : {1LL, 4096LL}) {
    const fs::path gen_dir = dir / ("starved-publish-" + std::to_string(budget));
    fs::create_directories(gen_dir);
    const fs::path dest = gen_dir / "generation-1.db";
    fault_vfs::arm(".bs-policy-", budget);
    bool threw = false;
    try {
      (void)publish_policy(good, dest, "crash-publish-1");
    } catch (const ArtifactError& error) {
      threw = true;
      CHECK(error.kind() == ArtifactErrorKind::Io);
    }
    fault_vfs::disarm();
    CHECK(threw);
    std::error_code ec;
    CHECK(!fs::exists(dest, ec));
    for (const auto& entry : fs::directory_iterator(gen_dir))
      CHECK(entry.path().filename().string().rfind(".bs-policy-", 0) != 0);
  }

  // Real process kills, with a large multi-page transaction.
  for (int round = 0; round < 6; ++round) {
    CHECK(run_crash_round(good, dir / ("crash-pre-" + std::to_string(round) + ".db"), false,
                          false) == 0);
  }
  CHECK(run_crash_round(good, dir / "crash-committed.db", true, false) == 0);
  CHECK(run_crash_round(good, dir / "crash-no-journal.db", false, true) == 0);

  // The seed checkpoint itself is untouched.
  const LoadedArtifact still_good = load_artifact(good);
  CHECK(still_good.bundle.manifest.completed_iterations == exported.completed_iterations);
  return 0;
}

// ---------------------------------------------------------------------------
// test 5: physical corruption, version/schema/validation rejection against a
// schema-v2 checkpoint.
// ---------------------------------------------------------------------------

static int test_corruption(const fs::path& dir) {
  const SeatTrainingResult exported = trained_seat_result_v2();
  const fs::path source = dir / "corrupt-source.db";
  create_checkpoint(source, exported);

  auto scratch = [&](const std::string& name) {
    const fs::path path = dir / name;
    fs::copy_file(source, path, fs::copy_options::overwrite_existing);
    return path;
  };

  // A user_version bumped from 2 to 3 selects the schema-v3 DDL set; the
  // stored schema-v2 manifest text does not match it.
  {
    const fs::path path = scratch("bad-version.db");
    RawDb raw(path);
    raw.exec("PRAGMA user_version=3;");
    CHECK(throws_artifact([&] { (void)load_artifact(path); }, ArtifactErrorKind::InvalidSchema));
  }

  // Page corruption: the b-tree type byte of page 2.
  {
    const fs::path path = scratch("bad-page.db");
    flip_byte(path, 4096);
    CHECK(throws_artifact([&] { (void)load_artifact(path); }));
  }
  // Header magic corruption and an invalid page-size word.
  {
    const fs::path path = scratch("bad-magic.db");
    flip_byte(path, 0);
    CHECK(throws_artifact([&] { (void)load_artifact(path); }));
  }
  {
    const fs::path path = scratch("bad-header.db");
    flip_byte(path, 16);
    CHECK(throws_artifact([&] { (void)load_artifact(path); }));
  }
  {
    const fs::path path = scratch("truncated.db");
    std::string data = read_file_bytes(path);
    write_file_bytes(path, data.substr(0, data.size() / 2));
    CHECK(throws_artifact([&] { (void)load_artifact(path); }));
  }
  {
    const fs::path path = scratch("header-only.db");
    std::string data = read_file_bytes(path);
    write_file_bytes(path, data.substr(0, 4096));
    CHECK(throws_artifact([&] { (void)load_artifact(path); }));
  }

  // Bad foreign key is rejected at write time (FK enforced).
  {
    const fs::path path = scratch("bad-fk.db");
    RawDb raw(path);
    CHECK(
        !raw.try_exec("INSERT INTO actions (info_id, ordinal, kind, target, probability)"
                      " VALUES (999999, 0, 0, NULL, 1.0)"));
  }
  // STRICT rejects a type-violating REAL.
  {
    const fs::path path = scratch("bad-strict.db");
    RawDb raw(path);
    CHECK(!raw.try_exec("INSERT INTO ranges (player, combo, weight) VALUES (0, 100, 'heavy')"));
  }
  // Unknown schema object.
  {
    const fs::path path = scratch("bad-object.db");
    RawDb raw(path);
    raw.exec("CREATE TABLE evil (x INTEGER) STRICT;");
    CHECK(throws_artifact([&] { (void)load_artifact(path); }, ArtifactErrorKind::InvalidSchema));
  }

  // NaN / +Inf / -Inf planted directly into the bytes of an unconstrained
  // NOT NULL REAL cell (training.regret). The v2 training table must stay
  // empty for a real checkpoint, so these rows are also rejected on content;
  // finite-REAL validation runs first and must classify them exactly as the
  // retired v1 cases did. SQL itself cannot bind non-finite doubles, so the
  // values are injected at the byte level.
  constexpr double kRealSentinel = -72539182736.125;
  for (const auto& [label, bits, expected] :
       std::vector<std::tuple<std::string, std::uint64_t, ArtifactErrorKind>>{
           {"nan", 0x7FF8000000000000ULL, ArtifactErrorKind::Corrupt},
           {"pinf", 0x7FF0000000000000ULL, ArtifactErrorKind::InvalidValue},
           {"ninf", 0xFFF0000000000000ULL, ArtifactErrorKind::InvalidValue},
       }) {
    const fs::path path = scratch("bad-real-" + label + ".db");
    {
      RawDb raw(path);
      raw.exec(
          "INSERT INTO training (info_id, ordinal, regret, average_weight)"
          " SELECT info_id, ordinal, -72539182736.125, 1.0 FROM actions"
          " WHERE info_id = 0 AND ordinal = 0;");
      CHECK(raw.scalar_i64("SELECT COUNT(*) FROM training WHERE regret = -72539182736.125") == 1);
    }
    rewrite_unique_real(path, kRealSentinel, bits);
    CHECK(throws_artifact([&] { (void)load_artifact(path); }, expected));
  }

  // Probability normalization violation with per-cell values inside [0,1].
  {
    const fs::path path = scratch("bad-sum.db");
    RawDb raw(path);
    raw.exec(
        "UPDATE actions SET probability = 0 WHERE info_id = (SELECT MIN(id)"
        " FROM information_states);");
    CHECK(throws_artifact([&] { (void)load_artifact(path); }, ArtifactErrorKind::InvalidValue));
  }
  // Integer beyond the 2^53-1 profile in a measurements row.
  {
    const fs::path path = scratch("bad-int.db");
    RawDb raw(path);
    raw.exec(
        "INSERT INTO measurements (id, fixture, seed_hex, iterations, metric,"
        " metric_class, value) VALUES (1, 'f', NULL, 9007199254740992, 'm',"
        " 'estimated', 1.0)");
    CHECK(throws_artifact([&] { (void)load_artifact(path); }, ArtifactErrorKind::InvalidValue));
  }

  // Writer-side content validation: rows a real export can never carry must
  // be rejected at create time, before any file is opened.
  const auto& first_row = exported.policy.rows().begin()->second;
  const InformationKey first_key = exported.policy.rows().begin()->first;
  {
    std::vector<double> probabilities = first_row.probabilities;
    probabilities[0] = std::nan("");
    const SeatTrainingResult bad = seat_result_with_rows(
        exported, {{first_key, SeatPolicyRow{first_row.actions, probabilities, 0}}});
    const fs::path path = dir / "never-nan.db";
    CHECK(throws_artifact([&] { create_checkpoint(path, bad); }));
    CHECK(!fs::exists(path));
  }
  {
    std::vector<double> probabilities(first_row.probabilities.size(), 0.0);
    probabilities[0] = -0.25;
    probabilities[1] = 1.25;
    const SeatTrainingResult bad = seat_result_with_rows(
        exported, {{first_key, SeatPolicyRow{first_row.actions, probabilities, 0}}});
    const fs::path path = dir / "never-negative.db";
    CHECK(throws_artifact([&] { create_checkpoint(path, bad); }));
    CHECK(!fs::exists(path));
  }
  {
    std::vector<double> probabilities(first_row.probabilities.size() + 1,
                                      1.0 / (first_row.probabilities.size() + 1));
    const SeatTrainingResult bad = seat_result_with_rows(
        exported, {{first_key, SeatPolicyRow{first_row.actions, probabilities, 0}}});
    const fs::path path = dir / "never-mismatch.db";
    CHECK(throws_artifact([&] { create_checkpoint(path, bad); }));
    CHECK(!fs::exists(path));
  }

  // Size bound: a reader option below the file size rejects before opening.
  {
    LoadOptions tiny;
    tiny.max_file_bytes = 16;
    CHECK(throws_artifact([&] { (void)load_artifact(source, tiny); },
                          ArtifactErrorKind::FileTooLarge));
  }
  return 0;
}

// ---------------------------------------------------------------------------
// test 6: immutable publication guarantees for schema-v2 generations.
// ---------------------------------------------------------------------------

static int test_immutable(const fs::path& dir) {
  const SeatTrainingResult exported = trained_seat_result_v2();
  const fs::path checkpoint = dir / "immutable-source.db";
  create_checkpoint(checkpoint, exported);

  const fs::path gen_dir = dir / "immutable-gen";
  fs::create_directories(gen_dir);
  const fs::path gen = gen_dir / "generation-1.db";
  const PublishedPolicy published = publish_policy(checkpoint, gen, "immutable-engine-1");

  // Never overwrites an existing generation.
  CHECK(throws_artifact([&] { (void)publish_policy(checkpoint, gen, "immutable-engine-1"); },
                        ArtifactErrorKind::AlreadyExists));

  // No temp names left in the directory.
  for (const auto& entry : fs::directory_iterator(gen_dir))
    CHECK(entry.path().filename().string().rfind(".bs-policy-", 0) != 0);

  // File mode is read-only (0444).
  struct stat st = {};
  CHECK(::stat(gen.string().c_str(), &st) == 0);
  CHECK((st.st_mode & 0777) == (S_IRUSR | S_IRGRP | S_IROTH));
  if (geteuid() != 0) {
    sqlite3* db = nullptr;
    int rc = sqlite3_open_v2(gen.string().c_str(), &db, SQLITE_OPEN_READWRITE, nullptr);
    // The 0444 mode must deny the RW open outright; if the platform still
    // hands one out, any schema write has to fail.
    bool write_failed = rc != SQLITE_OK;
    if (rc == SQLITE_OK) {
      char* error = nullptr;
      int exec_rc = sqlite3_exec(db, "CREATE TABLE evil (x);", nullptr, nullptr, &error);
      sqlite3_free(error);
      write_failed = exec_rc != SQLITE_OK;
    }
    if (db)
      sqlite3_close_v2(db);
    CHECK(write_failed);
  }

  // Byte edit -> digest mismatch before parsing; restore -> opens again.
  const fs::path edited = dir / "immutable-edited.db";
  fs::copy_file(gen, edited);
  chmod(edited.string().c_str(), S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
  // Flip a byte in the application_id header word: independently fatal even
  // when the caller does not pin a digest (the digest-pinned load below is
  // rejected first on the SHA-256 mismatch).
  flip_byte(edited, 69);
  LoadOptions expected;
  expected.expected_sha256 = published.sha256;
  CHECK(throws_artifact([&] { (void)load_artifact(edited, expected); },
                        ArtifactErrorKind::DigestMismatch));
  CHECK(throws_artifact([&] { (void)load_artifact(edited); }));

  // Independent publications of the same checkpoint are byte-identical; this
  // is the schema-v2 counterpart of the retired v1 split-run digest equality
  // (deterministic pages, rollback journal, sorted reads).
  {
    const fs::path dir_a = dir / "gen-a";
    const fs::path dir_b = dir / "gen-b";
    fs::create_directories(dir_a);
    fs::create_directories(dir_b);
    const PublishedPolicy gen_a =
        publish_policy(checkpoint, dir_a / "gen.db", "immutable-engine-1");
    const PublishedPolicy gen_b =
        publish_policy(checkpoint, dir_b / "gen.db", "immutable-engine-1");
    CHECK(gen_a.sha256_hex == gen_b.sha256_hex);
    CHECK(read_file_bytes(dir_a / "gen.db") == read_file_bytes(dir_b / "gen.db"));
  }

  // A zero-row checkpoint is valid but can never be published: the policy is
  // empty.
  SeatTrainingResult empty = seat_result_with_rows(exported, {});
  empty.completed_iterations = 0;
  empty.completed_epochs = 0;
  const fs::path empty_checkpoint = dir / "empty-checkpoint.db";
  create_checkpoint(empty_checkpoint, empty);
  const LoadedArtifact empty_loaded = load_artifact(empty_checkpoint);
  CHECK(empty_loaded.bundle.nseat->policy.rows().empty());
  CHECK(throws_artifact([&] { (void)publish_policy(empty_checkpoint, dir / "empty-gen.db"); },
                        ArtifactErrorKind::InvalidArgument));

  // Reopening with the expected digest works.
  const LoadedArtifact again = load_artifact(gen, expected);
  CHECK(again.sha256_hex == published.sha256_hex);
  return 0;
}

// Rewrites a table's stored CREATE text under writable_schema. An empty sql
// restores NULL. Used to first strip CHECK enforcement, plant a bad row with
// a normal connection, then restore the byte-canonical DDL so the stored
// schema text passes the byte-identical schema check while a constraint-
// violating row survives.
void overwrite_table_sql(const fs::path& path, const std::string& table, const std::string& sql) {
  RawDb raw(path);
  raw.exec("PRAGMA writable_schema=ON;");
  std::string escaped;
  for (char ch : sql) {
    if (ch == '\'')
      escaped += "''";
    else
      escaped.push_back(ch);
  }
  raw.exec("UPDATE sqlite_schema SET sql = '" + escaped + "' WHERE type = 'table' AND name = '" +
           table + "';");
}

// The roundtrip cases have already independently validated these native
// schema-v2/schema-v3 files. Keep only three contiguous states to make each fault
// injection cheap; the ordered reader must still consume both complete cursors.
static int test_seat_stream_adversarial(const fs::path& dir) {
  const char* kStatesNoCheck =
      "CREATE TABLE information_states (id INTEGER PRIMARY KEY, player INTEGER NOT NULL,"
      " card0 INTEGER NOT NULL, card1 INTEGER NOT NULL, public_key TEXT NOT NULL,"
      " UNIQUE (player, card0, card1, public_key)) STRICT";
  const char* kActionsNoCheck =
      "CREATE TABLE actions (info_id INTEGER NOT NULL, ordinal INTEGER NOT NULL,"
      " kind INTEGER NOT NULL, target INTEGER, probability REAL NOT NULL,"
      " PRIMARY KEY (info_id, ordinal)) STRICT";
  const char* kTrainingNoCheck =
      "CREATE TABLE training (info_id INTEGER NOT NULL, ordinal INTEGER NOT NULL,"
      " regret REAL NOT NULL, average_weight REAL NOT NULL,"
      " PRIMARY KEY (info_id, ordinal)) STRICT";

  struct Mutation {
    const char* name;
    const char* table;
    const char* sql;
    // Nonempty detail proves a post-integrity domain/stream check was reached.
    // Empty detail deliberately permits rejection by the unchanged preflight.
    const char* detail;
  };
  const Mutation mutations[] = {
      {"state-prefix-gap", "", "DELETE FROM information_states WHERE id=0;",
       "ids must be contiguous from zero"},
      {"state-middle-gap", "", "DELETE FROM information_states WHERE id=1;",
       "ids must be contiguous from zero"},
      {"missing-actions", "", "DELETE FROM actions WHERE info_id=0;",
       "information state has no actions"},
      {"ordinal-prefix-gap", "", "DELETE FROM actions WHERE info_id=0 AND ordinal=0;",
       "action ordinals must be contiguous from zero"},
      {"ordinal-middle-gap", "", "UPDATE actions SET ordinal=30 WHERE info_id=0 AND ordinal=1;",
       "action ordinals must be contiguous from zero"},
      {"unnormalized", "", "UPDATE actions SET probability=0 WHERE info_id=0;",
       "probabilities do not sum to 1"},
      {"seat-outside-game", "", "UPDATE information_states SET player=9 WHERE id=0;",
       "player outside player_count"},
      {"bad-key", "", "UPDATE information_states SET public_key='invalid' WHERE id=0;",
       "canonical key"},
      {"noncanonical-key", "",
       "UPDATE information_states SET public_key='0'||public_key"
       " WHERE id=0;",
       "leading zero"},
      {"duplicate-board", "", "", "duplicate card on the public board"},
      {"count-mismatch", "", "UPDATE manifest SET information_sets=4;", "stored state count"},
      {"orphan-prefix", "actions", "INSERT INTO actions VALUES (-1,0,1,NULL,1.0);", ""},
      {"orphan-tail", "actions", "INSERT INTO actions VALUES (3,0,1,NULL,1.0);", ""},
      {"actions-with-no-states", "actions",
       "DELETE FROM information_states;"
       " INSERT INTO actions VALUES (3,0,1,NULL,1.0);",
       ""},
      {"overflow-seat", "information_states",
       "UPDATE information_states SET player=4294967296 WHERE id=0;", ""},
      {"overflow-card0", "information_states",
       "UPDATE information_states SET card0=4294967296 WHERE id=0;", ""},
      {"overflow-card1", "information_states",
       "UPDATE information_states SET card1=4294967297 WHERE id=0;", ""},
      {"overflow-kind", "actions",
       "UPDATE actions SET kind=4294967297 WHERE info_id=0 AND ordinal=0;", ""},
      {"overflow-ordinal", "actions",
       "UPDATE actions SET ordinal=4294967296 WHERE info_id=0 AND ordinal=0;", ""},
      {"negative-target", "actions",
       "UPDATE actions SET kind=3,target=-1 WHERE info_id=0 AND ordinal=0;", ""},
      {"overflow-target", "actions",
       "UPDATE actions SET kind=3,target=9007199254740992 WHERE info_id=0 AND ordinal=0;", ""},
      {"missing-aggressive-target", "actions",
       "UPDATE actions SET kind=3,target=NULL WHERE info_id=0 AND ordinal=0;", ""},
      {"unexpected-passive-target", "actions",
       "UPDATE actions SET kind=1,target=1 WHERE info_id=0 AND ordinal=0;", ""},
      {"nonfinite-probability", "actions",
       "UPDATE actions SET probability=1e999 WHERE info_id=0 AND ordinal=0;", ""},
      {"negative-probability", "actions",
       "UPDATE actions SET probability=-0.1 WHERE info_id=0 AND ordinal=0;", ""},
      {"unexpected-training", "", "INSERT INTO training VALUES (0,0,1.0,1.0);",
       "must not carry training rows"},
      {"nonfinite-training-regret", "", "INSERT INTO training VALUES (0,0,1e999,1.0);",
       "non-finite REAL in cumulative regret"},
      {"negative-training-weight", "training", "INSERT INTO training VALUES (0,0,1.0,-1.0);", ""},
  };

  for (const int version : {2, 3}) {
    // Source checkpoint produced by the roundtrip-schema-v2/v3 cases in the
    // shared test workspace; the name must match those case directories.
    const std::string prefix = "roundtrip-schema-v" + std::to_string(version);
    const fs::path original = dir.parent_path() / prefix / (prefix + "-checkpoint.db");
    const fs::path source = dir / ("stream-v" + std::to_string(version) + ".db");
    fs::copy_file(original, source);
    std::map<std::string, std::string> canonical;
    std::string duplicate_board_sql;
    {
      RawDb raw(source);
      CHECK(raw.scalar_i64("SELECT COUNT(*) FROM information_states") >= 3);
      CHECK(raw.scalar_i64("SELECT COUNT(*) FROM actions WHERE info_id=0") >= 2);
      raw.exec(
          "DELETE FROM information_states WHERE id>=3;"
          " UPDATE manifest SET information_sets=3;"
          " VACUUM;");
      const auto card0 = raw.scalar_i64("SELECT card0 FROM information_states WHERE id=0");
      const auto card1 = raw.scalar_i64("SELECT card1 FROM information_states WHERE id=0");
      int duplicate_card = 0;
      while (duplicate_card == card0 || duplicate_card == card1)
        ++duplicate_card;
      int other_card = duplicate_card + 1;
      while (other_card == card0 || other_card == card1)
        ++other_card;
      // Off-hand cards pin duplicate-board validation instead of triggering
      // the earlier own-card-overlap check in either schema.
      duplicate_board_sql = "UPDATE information_states SET public_key='0|" +
                            std::to_string(duplicate_card) + "," + std::to_string(duplicate_card) +
                            "," + std::to_string(other_card) + "|' WHERE id=0;";
      for (const char* table : {"information_states", "actions", "training"})
        canonical[table] = raw.scalar_text("SELECT sql FROM sqlite_schema WHERE name='" +
                                           std::string(table) + "'");
      // The SQL primary key rejects a duplicate ordinal before a malformed
      // file can exist. This is not claimed as streamed-reader branch coverage.
      CHECK(!raw.try_exec(
          "INSERT INTO actions SELECT * FROM actions WHERE info_id=0 AND ordinal=0;"));
    }
    const LoadedArtifact valid = load_artifact(source);
    CHECK(valid.bundle.nseat.has_value());
    CHECK(valid.bundle.nseat->policy.rows().size() == 3);
    CHECK(valid.bundle.nseat->information_sets == 3);
    CHECK(valid.bundle.manifest.card_abstraction.has_value() == (version == 3));
    const std::string source_hash = sha256_file_hex(source);

    // Valid keys need not be ordered by state ID. The insertion hint must fall
    // back correctly without changing the associated action distributions.
    const fs::path reversed = dir / ("stream-v" + std::to_string(version) + "-reversed.db");
    fs::copy_file(source, reversed);
    {
      RawDb raw(reversed);
      raw.exec(
          "PRAGMA foreign_keys=OFF;"
          " UPDATE information_states SET id=id+3;"
          " UPDATE actions SET info_id=info_id+3;"
          " UPDATE information_states SET id=5-id;"
          " UPDATE actions SET info_id=5-info_id;"
          " PRAGMA foreign_keys=ON;");
    }
    const LoadedArtifact reordered = load_artifact(reversed);
    CHECK(compare_seat_result_exact(*valid.bundle.nseat, *reordered.bundle.nseat) == 0);

    for (const Mutation& mutation : mutations) {
      const fs::path path =
          dir / ("stream-v" + std::to_string(version) + "-" + mutation.name + ".db");
      fs::copy_file(source, path);
      const std::string table = mutation.table;
      if (!table.empty())
        overwrite_table_sql(path, table,
                            table == "actions"    ? kActionsNoCheck
                            : table == "training" ? kTrainingNoCheck
                                                  : kStatesNoCheck);
      {
        RawDb raw(path);
        // Orphan cases intentionally bypass write-time FK enforcement. The
        // reader's unchanged FK/integrity preflight remains authoritative.
        const std::string name = mutation.name;
        if (name.starts_with("orphan") || name == "actions-with-no-states")
          raw.exec("PRAGMA foreign_keys=OFF;");
        raw.exec(name == "duplicate-board" ? duplicate_board_sql : mutation.sql);
      }
      if (!table.empty())
        overwrite_table_sql(path, table, canonical.at(table));
      bool rejected = false;
      try {
        (void)load_artifact(path);
      } catch (const ArtifactError& error) {
        rejected = true;
        if (*mutation.detail != '\0') {
          const std::string name = mutation.name;
          const ArtifactErrorKind expected_kind =
              name == "unnormalized" || name == "nonfinite-training-regret"
                  ? ArtifactErrorKind::InvalidValue
                  : ArtifactErrorKind::InvalidSchema;
          CHECK(error.kind() == expected_kind);
        }
        if (*mutation.detail != '\0' &&
            std::string(error.what()).find(mutation.detail) == std::string::npos) {
          std::printf("stream v%d %s failed before expected domain check: %s\n", version,
                      mutation.name, error.what());
          return 1;
        }
      }
      CHECK(rejected);
    }
    // Adversarial copies must not mutate the validated source.
    CHECK(sha256_file_hex(source) == source_hash);
    const LoadedArtifact again = load_artifact(source);
    CHECK(compare_seat_result_exact(*valid.bundle.nseat, *again.bundle.nseat) == 0);
    const fs::path empty = dir / ("stream-v" + std::to_string(version) + "-empty.db");
    fs::copy_file(source, empty);
    {
      RawDb raw(empty);
      raw.exec("DELETE FROM information_states; UPDATE manifest SET information_sets=0;");
    }
    const LoadedArtifact empty_loaded = load_artifact(empty);
    CHECK(empty_loaded.bundle.nseat.has_value());
    CHECK(empty_loaded.bundle.nseat->policy.rows().empty());
  }
  return 0;
}

// ---------------------------------------------------------------------------
// test 8: independent adversarial regressions against the schema-v2 writer
// and reader: P1-1 CHECK-stripped size/range domains and tampered DDL text,
// P1-3 committed -wal sidecar bypass, P2-4 schema-object zip bomb, P2-5
// writer pragma verification. Stream ordering, key/probability domains, and
// 2^32-narrowing regressions are covered by test_seat_stream_adversarial on
// both v2 and v3 sources.
// ---------------------------------------------------------------------------

static int test_adversarial(const fs::path& dir) {
  const SeatTrainingResult exported = trained_seat_result_v2();
  const fs::path source = dir / "adv-source.db";
  create_checkpoint(source, exported);

  std::string canonical_sizes;
  std::string canonical_ranges;
  {
    RawDb raw(source, SQLITE_OPEN_READONLY);
    canonical_sizes = raw.scalar_text("SELECT sql FROM sqlite_schema WHERE name='sizes'");
    canonical_ranges = raw.scalar_text("SELECT sql FROM sqlite_schema WHERE name='ranges'");
    CHECK(!canonical_sizes.empty() && !canonical_ranges.empty());
  }

  auto scratch = [&](const std::string& name) {
    const fs::path path = dir / name;
    fs::copy_file(source, path, fs::copy_options::overwrite_existing);
    return path;
  };

  struct BadSizeRow {
    const char* name;
    const char* values;
  };

  // --- P1-1: CHECK-stripped rows with canonical DDL text restored ----------
  // If the C++ domain checks were removed, street=16 would index a small
  // schedule array: silent OOB in release, stack-buffer-overflow under ASan.
  const char* kSizesNoCheck =
      "CREATE TABLE sizes (street INTEGER NOT NULL, kind INTEGER NOT NULL,"
      " ordinal INTEGER NOT NULL, numerator INTEGER NOT NULL,"
      " denominator INTEGER NOT NULL, PRIMARY KEY (street, kind, ordinal)) STRICT";
  const char* kRangesNoCheck =
      "CREATE TABLE ranges (player INTEGER NOT NULL, combo INTEGER NOT NULL,"
      " weight REAL NOT NULL, PRIMARY KEY (player, combo)) STRICT";

  for (const BadSizeRow& bad : std::vector<BadSizeRow>{
           {"adv-size-street16", "(16, 0, 7, 1, 3)"},
           {"adv-size-street-neg", "(-1, 0, 0, 1, 3)"},
           {"adv-size-kind2", "(0, 2, 0, 1, 3)"},
       }) {
    const fs::path path = scratch(std::string(bad.name) + ".db");
    overwrite_table_sql(path, "sizes", kSizesNoCheck);
    {
      RawDb insert(path);
      insert.exec(std::string("INSERT INTO sizes VALUES ") + bad.values + ";");
    }
    overwrite_table_sql(path, "sizes", canonical_sizes);
    CHECK(throws_artifact([&] { (void)load_artifact(path); }, ArtifactErrorKind::InvalidSchema));
  }

  for (const auto& [name, values] : std::vector<std::pair<std::string, std::string>>{
           // The source game has three occupied seats, so player 3 passes the
           // 0..9 SQL domain but lies outside the game's player_count.
           {"adv-range-player3", "(3, 0, 1.0)"},
           {"adv-range-combo1326", "(0, 1326, 1.0)"},
           {"adv-range-combo-neg", "(0, -1, 1.0)"},
       }) {
    const fs::path path = scratch(name + ".db");
    overwrite_table_sql(path, "ranges", kRangesNoCheck);
    {
      RawDb insert(path);
      insert.exec("INSERT INTO ranges VALUES " + values + ";");
    }
    overwrite_table_sql(path, "ranges", canonical_ranges);
    CHECK(throws_artifact([&] { (void)load_artifact(path); }, ArtifactErrorKind::InvalidSchema));
  }

  // P1-1(c): a tampered CREATE statement (non-canonical but still STRICT and
  // carrying the same table/column set) must be rejected byte-for-byte.
  {
    const fs::path path = scratch("adv-ddl-tampered.db");
    overwrite_table_sql(
        path, "sizes",
        "CREATE TABLE sizes (street INTEGER NOT NULL CHECK (street BETWEEN 0 AND 9),"
        " kind INTEGER NOT NULL, ordinal INTEGER NOT NULL, numerator INTEGER NOT NULL,"
        " denominator INTEGER NOT NULL, PRIMARY KEY (street, kind, ordinal)) STRICT");
    CHECK(throws_artifact([&] { (void)load_artifact(path); }, ArtifactErrorKind::InvalidSchema));
  }

  // --- P2-4: schema-object zip bomb rejection and bounded heap design ------
  {
    const fs::path path = scratch("adv-views.db");
    {
      RawDb raw(path);
      // A modest view cluster is enough to exercise rejection; a real
      // 512 MiB hard-limit trigger needs a >512 MiB fixture and is not
      // fabricated here. The views are parsed during schema load.
      for (int i = 0; i < 500; ++i)
        raw.exec("CREATE VIEW adv_view_" + std::to_string(i) + " AS SELECT 1 AS x;");
    }
    CHECK(throws_artifact([&] { (void)load_artifact(path); }, ArtifactErrorKind::InvalidSchema));
    // The process survives and the hard heap ceiling is installed.
    (void)load_artifact(source);  // a legitimate artifact still loads
    const sqlite3_int64 heap_bound = sqlite3_hard_heap_limit64(-1);
    CHECK(heap_bound >= static_cast<sqlite3_int64>(kReaderHeapBoundBytes));
  }

  // --- P2-5: synchronous=FULL is verified on the writer connection ---------
  {
    const fs::path path = dir / "adv-pragma.db";
    std::error_code ec;
    fs::remove(path, ec);
    bs::artifacts::detail::Db db;
    db.open(path.string(), SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, "pragma whitebox");
    bs::artifacts::detail::apply_writer_pragmas(db);
    bs::artifacts::detail::verify_writer_pragmas(db);  // FULL/delete took effect
    db.exec("PRAGMA synchronous=OFF;", "weaken synchronous");
    CHECK(throws_artifact([&] { bs::artifacts::detail::verify_writer_pragmas(db); },
                          ArtifactErrorKind::Sqlite));
  }

  // --- P1-3: committed -wal sidecar bypass must be impossible --------------
  // Build a genuinely committed WAL mutation in a forked child that exits
  // without closing SQLite (no checkpoint), leaving a real -wal behind.
  const fs::path wal_work = dir / "adv-wal-build";
  fs::create_directories(wal_work);
  const fs::path wal_base = wal_work / "base.db";
  fs::copy_file(source, wal_base);
  {
    pid_t child = fork();
    if (child == 0) {
      sqlite3* db = nullptr;
      // Open by the real host VFS name, not the test's fault-injection shim
      // (registered as the in-process default). An attacker planting a WAL
      // sidecar on disk operates with the platform VFS, not our shim.
      if (sqlite3_open_v2(wal_base.string().c_str(), &db, SQLITE_OPEN_READWRITE, "unix") !=
          SQLITE_OK)
        _exit(50);
      auto child_exec_ok = [&](const char* sql) {
        if (sqlite3_exec(db, sql, nullptr, nullptr, nullptr) != SQLITE_OK)
          _exit(51);
      };
      auto child_scalar = [&](const char* sql) -> std::string {
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK)
          _exit(53);
        std::string value;
        if (sqlite3_step(stmt) == SQLITE_ROW) {
          const unsigned char* text = sqlite3_column_text(stmt, 0);
          if (text)
            value = reinterpret_cast<const char*>(text);
        }
        sqlite3_finalize(stmt);
        return value;
      };
      child_exec_ok("PRAGMA journal_mode=WAL;");
      if (child_scalar("PRAGMA journal_mode;") != "wal")
        _exit(54);
      child_exec_ok("BEGIN IMMEDIATE;");
      child_exec_ok("UPDATE manifest SET completed_iterations=777 WHERE id=1;");
      child_exec_ok("COMMIT;");
      child_exec_ok("PRAGMA wal_sync;");
      _exit(52);
    }
    CHECK(child > 0);
    int status = 0;
    CHECK(waitpid(child, &status, 0) == child);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 52);
  }
  CHECK(fs::exists(fs::path(wal_base.string() + "-wal")));

  // An ordinary connection on the platform VFS transparently sees the WAL
  // value, proving the sidecar genuinely contains committed content the main
  // digest misses.
  {
    const fs::path demo = wal_work / "demo.db";
    std::error_code ec;
    fs::remove(fs::path(demo.string() + "-shm"), ec);
    fs::copy_file(wal_base, demo, fs::copy_options::overwrite_existing, ec);
    fs::copy_file(fs::path(wal_base.string() + "-wal"), fs::path(demo.string() + "-wal"),
                  fs::copy_options::overwrite_existing, ec);
    sqlite3* plain = nullptr;
    CHECK(sqlite3_open_v2(demo.string().c_str(), &plain, SQLITE_OPEN_READONLY, "unix") ==
          SQLITE_OK);
    sqlite3_stmt* select = nullptr;
    CHECK(sqlite3_prepare_v2(plain, "SELECT completed_iterations FROM manifest WHERE id=1", -1,
                             &select, nullptr) == SQLITE_OK);
    CHECK(sqlite3_step(select) == SQLITE_ROW);
    CHECK(sqlite3_column_int64(select, 0) == 777);
    sqlite3_finalize(select);
    sqlite3_close_v2(plain);
  }

  // (i) The artifact reader rejects the WAL-mode main + -wal pair outright;
  // the returned digest can never describe attacker-replayed content.
  {
    const fs::path path = scratch("adv-wal-reject.db");
    std::error_code ec;
    fs::copy_file(fs::path(wal_base.string() + "-wal"), fs::path(path.string() + "-wal"),
                  fs::copy_options::overwrite_existing, ec);
    CHECK(throws_artifact([&] { (void)load_artifact(path); }));
  }

  // (ii) A published immutable generation next to a stray committed -wal
  // must still be rejected by the sidecar check, and the immutable reader
  // never opens or creates sidecars: no -shm appears and the -wal is
  // untouched.
  {
    const fs::path gen_dir = dir / "adv-wal-gen";
    fs::create_directories(gen_dir);
    const fs::path gen = gen_dir / "gen.db";
    const PublishedPolicy published = publish_policy(source, gen, "adv-wal-engine-1");
    const fs::path victim = dir / "adv-wal-published.db";
    fs::copy_file(gen, victim);
    std::error_code ec;
    fs::copy_file(fs::path(wal_base.string() + "-wal"), fs::path(victim.string() + "-wal"),
                  fs::copy_options::overwrite_existing, ec);
    const auto wal_size_before = fs::file_size(fs::path(victim.string() + "-wal"), ec);
    CHECK(throws_artifact([&] { (void)load_artifact(victim); }, ArtifactErrorKind::Corrupt));
    const auto wal_size_after = fs::file_size(fs::path(victim.string() + "-wal"), ec);
    CHECK(wal_size_before == wal_size_after);
    CHECK(!fs::exists(fs::path(victim.string() + "-shm"), ec));
    // Even with the pinned digest of the untouched main file, the sidecar
    // blocks the read.
    LoadOptions pinned;
    pinned.expected_sha256 = published.sha256;
    CHECK(
        throws_artifact([&] { (void)load_artifact(victim, pinned); }, ArtifactErrorKind::Corrupt));
  }

  // (iii) Empty -shm and -journal siblings are rejected as well.
  for (const char* suffix : {"-shm", "-journal"}) {
    const fs::path victim = dir / (std::string("adv-sidecar") + suffix + ".db");
    fs::copy_file(source, victim);
    {
      std::ofstream sidecar(fs::path(victim.string() + suffix).string(),
                            std::ios::binary | std::ios::trunc);
      CHECK(sidecar.is_open());
    }
    CHECK(throws_artifact([&] { (void)load_artifact(victim); }, ArtifactErrorKind::Corrupt));
  }
  return 0;
}

int main() {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  fault_vfs::install();
  const fs::path dir = make_temp_dir();
  std::printf("artifact test workspace: %s\n", dir.string().c_str());

  struct Case {
    const char* name;
    int (*fn)(const fs::path&);
  };
  const Case cases[] = {
      {"roundtrip-schema-v2", test_roundtrip_v2},
      {"nseat-algorithm-identity", test_nseat_algorithm_identity},
      {"roundtrip-schema-v3", test_roundtrip_v3},
      {"roundtrip-preflop", test_roundtrip_preflop},
      {"roundtrip-preflop-3way", test_roundtrip_preflop_3way},
      {"canonical-keys", test_canonical_keys},
      {"v1-rejection", test_v1_rejection},
      {"crash-full-disk", test_crash_and_full_disk},
      {"corruption", test_corruption},
      {"immutable", test_immutable},
      {"adversarial", test_adversarial},
      {"seat-stream-adversarial", test_seat_stream_adversarial},
  };
  for (const Case& test_case : cases) {
    const fs::path case_dir = dir / test_case.name;
    fs::create_directories(case_dir);
    std::printf("[ RUN      ] %s\n", test_case.name);
    if (test_case.fn(case_dir) != 0) {
      std::printf("[ FAILED   ] %s (workspace kept at %s)\n", test_case.name, dir.string().c_str());
      return 1;
    }
    std::printf("[       OK ] %s\n", test_case.name);
  }
  std::filesystem::remove_all(dir);
  std::printf("all artifact tests passed\n");
  return 0;
}
