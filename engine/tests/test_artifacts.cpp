// RFC 0005 Stage 5 artifact fault, round-trip, split-run, and boundary tests.
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

using bs::artifacts::detail::PolicyAssembler;

int card(const char* name) {
  return bs::cardId(std::string(name));
}

TrainingLimits fast_limits() {
  TrainingLimits limits;
  limits.max_nodes = 200000000;
  limits.max_information_sets = 20000000;
  limits.time = std::chrono::minutes{10};
  return limits;
}

// Two chips behind, three abstract actions at decision nodes, both runouts
// fixed so complete-tree policies are small and deterministic.
HeadsUpGame multi_size_fixture() {
  HeadsUpGame game;
  game.root = {{card("2c"), card("3d"), card("7h")}, {2, 2}, {1, 1}, 2, 1, 1};
  game.ranges[0] = {{{card("As"), card("Ks")}, 2}, {{card("Qh"), card("Jh")}, 3}};
  game.ranges[1] = {{{card("Ac"), card("Kc")}, 5}, {{card("Qd"), card("Jd")}, 7}};
  game.fixed_runout = {card("9h"), card("8s")};
  return game;
}

// The debug drivers take the game as given; the public HeadsUpTrainer
// constructor canonicalizes sorted combos and per-player max-normalized
// weights before training. Artifacts only ever store the canonical game, so
// tests canonicalize with the exact trainer transformation.
HeadsUpGame canonical_fixture() {
  HeadsUpGame game = multi_size_fixture();
  for (auto& range : game.ranges) {
    double maximum = 0;
    for (auto& hand : range) {
      std::sort(hand.cards.begin(), hand.cards.end());
      maximum = std::max(maximum, hand.weight);
    }
    for (auto& hand : range)
      hand.weight /= maximum;
    std::sort(range.begin(), range.end(),
              [](const auto& a, const auto& b) { return a.cards < b.cards; });
  }
  return game;
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

// Compare the full canonical game identity exactly.
bool same_fraction_exact(const Fraction& a, const Fraction& b) {
  return a.numerator == b.numerator && a.denominator == b.denominator;
}
bool same_schedule_exact(const SizeSchedule& a, const SizeSchedule& b) {
  for (std::size_t s = 0; s < 3; ++s) {
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
bool same_ranges_exact(const std::array<std::vector<WeightedHand>, 2>& a,
                       const std::array<std::vector<WeightedHand>, 2>& b) {
  for (std::size_t p = 0; p < 2; ++p) {
    if (a[p].size() != b[p].size())
      return false;
    for (std::size_t i = 0; i < a[p].size(); ++i) {
      if (a[p][i].cards != b[p][i].cards || !same_bits(a[p][i].weight, b[p][i].weight))
        return false;
    }
  }
  return true;
}
bool same_game(const HeadsUpGame& a, const HeadsUpGame& b) {
  return a.root.flop == b.root.flop && a.root.stacks == b.root.stacks &&
         a.root.contributions == b.root.contributions && a.root.pot == b.root.pot &&
         a.root.big_blind == b.root.big_blind && a.root.button == b.root.button &&
         a.fixed_runout == b.fixed_runout && same_schedule_exact(a.sizes, b.sizes) &&
         same_ranges_exact(a.ranges, b.ranges);
}

int compare_policy_exact(const HeadsUpPolicy& expected, const HeadsUpPolicy& actual,
                         const TrainingRows& expected_raw, const TrainingRows& actual_raw) {
  if (!same_game(expected.game(), actual.game()))
    return 1;
  if (expected.rows().size() != actual.rows().size())
    return 1;
  for (const auto& [key, expected_row] : expected.rows()) {
    const auto found = actual.rows().find(key);
    if (found == actual.rows().end())
      return 1;
    if (expected_row.actions != found->second.actions)
      return 1;
    if (expected_row.probabilities.size() != found->second.probabilities.size())
      return 1;
    double sum = 0;
    for (std::size_t i = 0; i < expected_row.probabilities.size(); ++i) {
      // Canonical doubles are stored as SQLite REAL; require bit-exact replay
      // for the full-traversal oracle, and the declared tolerance otherwise.
      if (!near(expected_row.probabilities[i], found->second.probabilities[i]))
        return 1;
      sum += found->second.probabilities[i];
    }
    if (std::abs(sum - 1.0) > 1e-12)
      return 1;
  }
  if (expected_raw.size() != actual_raw.size())
    return 1;
  for (const auto& [key, row] : expected_raw) {
    const auto found = actual_raw.find(key);
    if (found == actual_raw.end())
      return 1;
    if (row.actions != found->second.actions ||
        row.regrets.size() != found->second.regrets.size() ||
        row.average_weights.size() != found->second.average_weights.size())
      return 1;
    for (std::size_t i = 0; i < row.regrets.size(); ++i) {
      if (!same_bits(row.regrets[i], found->second.regrets[i]))
        return 1;
      if (!same_bits(row.average_weights[i], found->second.average_weights[i]))
        return 1;
    }
  }
  return 0;
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
// the v2 writer's domain record.
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

// Rebuild a result's immutable policy around a modified game for identity tests.
TrainingResult rehome(const TrainingResult& result, HeadsUpGame game) {
  TrainingResult copy = result;
  PolicyAssembler::set_game(copy.policy, game);
  return copy;
}

// The debug driver returns solver DebugRow maps; the artifact boundary takes
// TrainingRows with the same actions/regrets/average-weight vectors.
TrainingRows to_training_rows(const std::map<InformationKey, DebugRow>& debug_rows) {
  TrainingRows out;
  for (const auto& [key, row] : debug_rows)
    out.emplace(key, TrainingRow{row.actions, row.regrets, row.sums});
  return out;
}

}  // namespace

// ---------------------------------------------------------------------------
// test 1: schema oracle, full-traversal round trip, independent-process read
// ---------------------------------------------------------------------------

static int test_roundtrip(const fs::path& dir) {
  const HeadsUpGame game = canonical_fixture();
  const DebugTrainingOutput trained = HeadsUpSolverDebug::train_full(game, 16, fast_limits());
  CHECK(trained.result.status == TrainingStatus::Complete);
  CHECK(!trained.result.policy.rows().empty());
  const TrainingRows trained_rows = to_training_rows(trained.rows);
  CHECK(trained_rows.size() == trained.result.policy.rows().size());

  const fs::path path = dir / "roundtrip-checkpoint.db";
  CheckpointProvenance provenance;
  provenance.prng_identifier = kFullTraversalPrngIdentifier;
  provenance.engine_revision = "roundtrip-engine-1";
  create_checkpoint(path, trained.result, trained_rows, provenance);
  CHECK(throws_artifact([&] { create_checkpoint(path, trained.result, trained_rows, provenance); },
                        ArtifactErrorKind::AlreadyExists));

  // Independent schema oracle through a raw connection.
  {
    const std::string header = read_file_bytes(path).substr(0, 16);
    CHECK(header == std::string("SQLite format 3\000", 16));
    RawDb raw(path, SQLITE_OPEN_READONLY);
    CHECK(raw.scalar_i64("PRAGMA application_id") == static_cast<std::int64_t>(0x42534754));
    CHECK(raw.scalar_i64("PRAGMA user_version") == 1);
    CHECK(raw.scalar_text("PRAGMA journal_mode") == "delete");
    CHECK(raw.scalar_text("PRAGMA synchronous") == "2");

    sqlite3_stmt* tables = nullptr;
    CHECK(sqlite3_prepare_v2(raw.db,
                             "SELECT name, sql FROM sqlite_schema WHERE type='table'"
                             " ORDER BY name",
                             -1, &tables, nullptr) == SQLITE_OK);
    const std::vector<std::string> expected{
        "actions", "bounds", "game",    "information_states", "manifest", "measurements",
        "ranges",  "sizes",  "training"};
    std::vector<std::string> seen;
    while (sqlite3_step(tables) == SQLITE_ROW) {
      const std::string name = reinterpret_cast<const char*>(sqlite3_column_text(tables, 0));
      const std::string sql = reinterpret_cast<const char*>(sqlite3_column_text(tables, 1));
      seen.push_back(name);
      CHECK(sql.size() >= 6 && sql.substr(sql.size() - 6) == "STRICT");
    }
    sqlite3_finalize(tables);
    CHECK(seen == expected);

    CHECK(raw.scalar_i64("SELECT COUNT(*) FROM sizes") == 15);
    CHECK(raw.scalar_i64("SELECT COUNT(*) FROM ranges") == 4);
    CHECK(raw.scalar_i64("SELECT COUNT(*) FROM information_states") ==
          static_cast<std::int64_t>(trained.result.policy.rows().size()));
    CHECK(raw.scalar_i64("SELECT COUNT(*) FROM training") == static_cast<std::int64_t>([&] {
            std::size_t total = 0;
            for (const auto& [key, row] : trained_rows)
              total += row.actions.size();
            return total;
          }()));

    sqlite3_stmt* sums = nullptr;
    CHECK(sqlite3_prepare_v2(raw.db,
                             "SELECT SUM(probability) FROM actions GROUP BY info_id"
                             " HAVING ABS(SUM(probability) - 1.0) > 1e-12",
                             -1, &sums, nullptr) == SQLITE_OK);
    CHECK(sqlite3_step(sums) == SQLITE_DONE);
    sqlite3_finalize(sums);

    // Foreign keys are really declared.
    CHECK(raw.try_exec("PRAGMA foreign_keys=ON;") == true);
  }

  // Same-process lossless read.
  const LoadedArtifact loaded = load_artifact(path);
  CHECK(loaded.sha256_hex.size() == 64);
  CHECK(loaded.file_bytes > 0);
  const auto& bundle = loaded.bundle;
  CHECK(bundle.manifest.kind == ArtifactKind::Checkpoint);
  CHECK(bundle.manifest.prng_identifier == kFullTraversalPrngIdentifier);
  CHECK(bundle.manifest.engine_revision == "roundtrip-engine-1");
  CHECK(bundle.manifest.completed_iterations == 16);
  CHECK(bundle.manifest.seed == 0 && bundle.manifest.prng_state == 0);
  if (compare_policy_exact(trained.result.policy, bundle.result.policy, trained_rows,
                           bundle.rows) != 0) {
    std::printf("roundtrip policy comparison failed\n");
    return 1;
  }
  for (const auto& [key, row] : bundle.result.policy.rows()) {
    (void)row;
    CHECK(encode_public_key(decode_public_key(encode_public_key(key, kArtifactSchemaVersion),
                                              kArtifactSchemaVersion, static_cast<int>(key[0]),
                                              static_cast<int>(key[1]), static_cast<int>(key[2])),
                            kArtifactSchemaVersion) ==
          encode_public_key(key, kArtifactSchemaVersion));
  }

  // Independent reader in a forked child process (fresh sqlite handles, no
  // shared memory with the writer).
  pid_t child = fork();
  if (child == 0) {
    int rc = 0;
    try {
      const LoadedArtifact child_loaded = load_artifact(path);
      const HeadsUpGame child_game = canonical_fixture();
      const DebugTrainingOutput child_trained =
          HeadsUpSolverDebug::train_full(child_game, 16, fast_limits());
      rc = compare_policy_exact(child_trained.result.policy, child_loaded.bundle.result.policy,
                                to_training_rows(child_trained.rows), child_loaded.bundle.rows);
      if (child_loaded.bundle.manifest.completed_iterations != 16)
        rc = 1;
    } catch (...) {
      rc = 1;
    }
    _exit(rc);
  }
  CHECK(child > 0);
  int status = 0;
  CHECK(waitpid(child, &status, 0) == child);
  CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);

  // Immutable publication round trip.
  const fs::path policy_dir = dir / "policy";
  fs::create_directory(policy_dir);
  const fs::path policy_path = policy_dir / "generation-0001.db";
  const PublishedPolicy published = publish_policy(path, policy_path, "publish-engine-1");
  CHECK(published.sha256_hex == sha256_file_hex(policy_path));
  CHECK(published.file_bytes == fs::file_size(policy_path));
  const LoadedArtifact published_loaded =
      load_artifact(policy_path, LoadOptions{published.file_bytes, published.sha256});
  CHECK(published_loaded.bundle.manifest.kind == ArtifactKind::Policy);
  CHECK(published_loaded.bundle.manifest.validation == ValidationState::Validated);
  CHECK(published_loaded.bundle.rows.empty());
  CHECK(published_loaded.bundle.result.policy.rows().size() == trained.result.policy.rows().size());
  for (const auto& [key, row] : trained.result.policy.rows()) {
    const auto found = published_loaded.bundle.result.policy.rows().find(key);
    CHECK(found != published_loaded.bundle.result.policy.rows().end());
    for (std::size_t i = 0; i < row.probabilities.size(); ++i)
      CHECK(near(row.probabilities[i], found->second.probabilities[i]));
  }
  return 0;
}

// ---------------------------------------------------------------------------
// test 1b: RFC 0009 W2b schema-v2 seat-generic checkpoint round trip
// ---------------------------------------------------------------------------

static int test_roundtrip_v2(const fs::path& dir) {
  const SeatTrainingResult exported = trained_seat_result_v2();
  CHECK(!exported.policy.rows().empty());
  CHECK(exported.policy.game().player_count == 3);
  CHECK(exported.terminal_depth == TerminalDepth::River);

  const fs::path path = dir / "roundtrip-v2-checkpoint.db";
  SeatCheckpointProvenance provenance;
  provenance.engine_revision = "roundtrip-v2-engine-1";
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

    // Manifest and game identity carry the v2 declarations.
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
    CHECK(raw.scalar_text("SELECT utility_id FROM game") == kUtilityIdentifierV1);
  }

  // Same-process lossless read.
  const LoadedArtifact loaded = load_artifact(path);
  CHECK(loaded.sha256_hex.size() == 64);
  CHECK(loaded.file_bytes > 0);
  const auto& bundle = loaded.bundle;
  CHECK(bundle.manifest.kind == ArtifactKind::Checkpoint);
  CHECK(bundle.manifest.information_key_revision == 2);
  CHECK(bundle.manifest.engine_revision == "roundtrip-v2-engine-1");
  CHECK(bundle.manifest.completed_iterations == exported.completed_iterations);
  CHECK(bundle.manifest.terminal_depth == TerminalDepth::River);
  CHECK(bundle.nseat.has_value());
  // The v1 arm stays default for a v2 file.
  CHECK(bundle.result.policy.rows().empty());
  CHECK(bundle.rows.empty());
  if (compare_seat_result_exact(exported, *bundle.nseat) != 0) {
    std::printf("v2 roundtrip seat-result comparison failed\n");
    return 1;
  }
  for (const auto& [key, row] : bundle.nseat->policy.rows()) {
    (void)row;
    CHECK(encode_public_key(decode_public_key(encode_public_key(key, kArtifactSchemaVersionV2),
                                              kArtifactSchemaVersionV2, static_cast<int>(key[0]),
                                              static_cast<int>(key[1]), static_cast<int>(key[2])),
                            kArtifactSchemaVersionV2) ==
          encode_public_key(key, kArtifactSchemaVersionV2));
  }

  // The v2 file is byte-stable: a second write of the same result has the same
  // SHA-256 (rollback journal mode, deterministic page allocation).
  const fs::path path2 = dir / "roundtrip-v2-checkpoint-2.db";
  create_checkpoint(path2, exported, provenance);
  CHECK(sha256_file_hex(path) == sha256_file_hex(path2));

  // RFC 0009 W2c: probe_artifact and publish_policy now support schema v2. The
  // probe carries the seat-generic identity (game_def/ranges/sizes); the
  // resident projection of a two-seat flop-rooted v2 source onto the heads-up
  // view is exercised by the resident-policy test, not here.
  const fs::path policy_dir = dir / "policy-v2";
  fs::create_directory(policy_dir);
  const fs::path policy_path = policy_dir / "generation-v2-0001.db";
  const PublishedPolicy published_v2 = publish_policy(path, policy_path, "publish-v2-engine-1");
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
  // The v1 identity arm stays default for a v2 probe.
  CHECK(probe.game.ranges[0].empty());
  CHECK(probe.game.ranges[1].empty());

  // The published policy round-trips through the v2 loader with the v1 arm
  // default.
  const LoadedArtifact published_loaded =
      load_artifact(policy_path, LoadOptions{published_v2.file_bytes, published_v2.sha256});
  CHECK(published_loaded.bundle.manifest.kind == ArtifactKind::Policy);
  CHECK(published_loaded.bundle.nseat.has_value());
  CHECK(published_loaded.bundle.result.policy.rows().empty());
  if (compare_seat_result_exact(exported, *published_loaded.bundle.nseat) != 0) {
    std::printf("v2 published-policy seat-result comparison failed\n");
    return 1;
  }

  // RFC 0009 D3: the reader validates player < player_count in C++, because a
  // SQL CHECK is write-time-only and can be stripped from a tampered file. A
  // state whose player is inside the 0..9 digit domain but outside the 3-seat
  // game is rejected.
  {
    const fs::path tampered = dir / "roundtrip-v2-tampered-player.db";
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
    const fs::path tampered = dir / "roundtrip-v2-tampered-seat.db";
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
// test 2: canonical key oracle (hand-authored bytes + malformed rejection)
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
    return decode_public_key(text, kArtifactSchemaVersion, player, sorted[0], sorted[1]);
  };

  const InformationKey k_root = information_key(state, own);
  CHECK(encode_public_key(k_root, kArtifactSchemaVersion) == "0|3,6,21|");
  CHECK(decode("0|3,6,21|", 0) == k_root);

  HeadsUpState after_check = state.after_action(0, {ActionType::Check});
  const InformationKey k_check = information_key(after_check, own);
  CHECK(encode_public_key(k_check, kArtifactSchemaVersion) == "0|3,6,21|0:x:-");
  CHECK(decode("0|3,6,21|0:x:-", 1) == k_check);

  HeadsUpState turn_state = after_check.after_action(1, {ActionType::Check}).after_card(turn);
  const InformationKey k_turn = information_key(turn_state, own);
  CHECK(encode_public_key(k_turn, kArtifactSchemaVersion) == "1|3,6,21,29|0:x:-,1:x:-");
  CHECK(decode("1|3,6,21,29|0:x:-,1:x:-", 0) == k_turn);

  HeadsUpState river_state = turn_state.after_action(0, {ActionType::Check})
                                 .after_action(1, {ActionType::Check})
                                 .after_card(river);
  const InformationKey k_river = information_key(river_state, own);
  CHECK(encode_public_key(k_river, kArtifactSchemaVersion) ==
        "2|3,6,21,29,24|0:x:-,1:x:-,0:x:-,1:x:-");
  CHECK(decode("2|3,6,21,29,24|0:x:-,1:x:-,0:x:-,1:x:-", 0) == k_river);

  HeadsUpState after_bet = state.after_action(0, {ActionType::Bet, 2});
  CHECK(encode_public_key(information_key(after_bet, own), kArtifactSchemaVersion) ==
        "0|3,6,21|0:b:2");
  HeadsUpState after_raise = after_bet.after_action(1, {ActionType::Raise, 4});
  CHECK(encode_public_key(information_key(after_raise, own), kArtifactSchemaVersion) ==
        "0|3,6,21|0:b:2,1:r:4");

  const std::vector<std::string> malformed{
      "3|3,6,21|",                          // bad street
      "0|3,6,21,29|",                       // boards disagree with street
      "0|52,6,21|",                         // bad card
      "0|3,6,21|2:x:-",                     // bad actor
      "0|3,6,21|0:b:-",                     // aggressive needs target
      "0|3,6,21|0:x:1",                     // check must not carry target
      "0|3,6,21|0:d:29",                    // d events are not revision 1
      "1|3,6,21,29|0:x:-",                  // one event, zero round closures
      "2|3,6,21,29,24|0:x:-,1:x:-",         // one closure, street 2
      "0|3,6,21|0:x",                       // missing field
      "0|3,6,21|0:b:02",                    // leading zero
      "0|3,6,21|0:b:9007199254740992",      // over 2^53-1
      "0|3,6,21|0:x:-,1:x:-,0:x:-,1:x:-|",  // extra field
  };
  for (const auto& text : malformed)
    CHECK(throws_artifact([&] { (void)decode(text, 0); }, ArtifactErrorKind::InvalidSchema));

  // Own combo shares a board card: rejected even with otherwise valid text.
  CHECK(throws_artifact(
      [&] { (void)decode_public_key("0|3,6,21|", kArtifactSchemaVersion, 0, card("2c"), 51); },
      ArtifactErrorKind::InvalidSchema));
  return 0;
}

// ---------------------------------------------------------------------------
// test 3: split-run equality and deterministic export digest
// ---------------------------------------------------------------------------

static int test_split_run(const fs::path& dir) {
  const HeadsUpGame fixture = canonical_fixture();
  constexpr std::uint64_t kSeed = 43;
  constexpr std::uint64_t kN = 60;

  const DebugTrainingOutput at_n =
      HeadsUpSolverDebug::train_sampled(fixture, kN, kSeed, fast_limits());
  CHECK(at_n.result.status == TrainingStatus::Complete);
  const TrainingRows at_n_rows = to_training_rows(at_n.rows);
  CheckpointProvenance provenance;
  provenance.prng_identifier = kSampledPrngIdentifier;

  const fs::path path_a = dir / "split-a.db";
  const fs::path path_b = dir / "split-b.db";
  create_checkpoint(path_a, at_n.result, at_n_rows, provenance);
  std::error_code ec;
  fs::remove(path_b, ec);
  fs::copy_file(path_a, path_b);

  // The file copy reads identically at the N checkpoint.
  const LoadedArtifact loaded_n = load_artifact(path_b);
  CHECK(loaded_n.bundle.manifest.completed_iterations == kN);
  CHECK(compare_policy_exact(at_n.result.policy, loaded_n.bundle.result.policy, at_n_rows,
                             loaded_n.bundle.rows) == 0);

  // A continues the canonical pinned run uninterrupted to 2N.
  const DebugTrainingOutput a_2n =
      HeadsUpSolverDebug::train_sampled(fixture, 2 * kN, kSeed, fast_limits());
  const TrainingRows a_2n_rows = to_training_rows(a_2n.rows);
  commit_checkpoint(path_a, a_2n.result, a_2n_rows, provenance);

  // B is a fresh process-style restore: rehydrate ONLY the rows, PRNG state,
  // and completed count loaded from A's checkpoint, then run N more
  // iterations through the debug resume driver. No in-memory table from the
  // first run crosses this boundary.
  std::map<InformationKey, DebugRow> restored;
  for (const auto& [key, raw] : loaded_n.bundle.rows)
    restored.emplace(key, DebugRow{raw.actions, raw.regrets, raw.average_weights});
  CHECK(restored.size() == at_n_rows.size());
  const DebugTrainingOutput b_resumed = HeadsUpSolverDebug::resume_sampled(
      loaded_n.bundle.result.policy.game(), kN, kSeed, loaded_n.bundle.manifest.prng_state, kN,
      restored, fast_limits());
  CHECK(b_resumed.result.completed_iterations == 2 * kN);
  // Node telemetry is cumulative across the resumed training: carry the
  // checkpoint's visited-node counter so uninterrupted and resumed runs
  // describe the same total work. Each driver invocation also counts the
  // one-time joint-deal setup enumeration (one visit per range pair in
  // joint_deals), so the second leg subtracts that fixed setup cost once.
  const std::size_t joint_pairs = loaded_n.bundle.result.policy.game().ranges[0].size() *
                                  loaded_n.bundle.result.policy.game().ranges[1].size();
  DebugTrainingOutput b_resumed_total = b_resumed;
  CHECK(b_resumed_total.result.nodes >= joint_pairs);
  b_resumed_total.result.nodes += loaded_n.bundle.manifest.nodes - joint_pairs;
  const TrainingRows b_resumed_rows = to_training_rows(b_resumed_total.rows);
  commit_checkpoint(path_b, b_resumed_total.result, b_resumed_rows, provenance);

  // The checkpoint-resumed run equals the uninterrupted run: probabilities
  // within 1e-12 and raw regret/average state bit-exact (the arithmetic order
  // is the pinned order in both runs).
  double max_probability_delta = 0.0;
  for (const auto& [key, row] : a_2n.result.policy.rows()) {
    const auto found = b_resumed.result.policy.rows().find(key);
    CHECK(found != b_resumed.result.policy.rows().end());
    CHECK(row.actions == found->second.actions);
    for (std::size_t i = 0; i < row.probabilities.size(); ++i) {
      max_probability_delta = std::max(
          max_probability_delta, std::abs(row.probabilities[i] - found->second.probabilities[i]));
    }
  }
  std::printf("split-run max policy probability delta: %.3g\n", max_probability_delta);
  CHECK(max_probability_delta <= 1e-12);
  CHECK(compare_policy_exact(a_2n.result.policy, b_resumed.result.policy, a_2n_rows,
                             b_resumed_rows) == 0);

  const LoadedArtifact final_a = load_artifact(path_a);
  const LoadedArtifact final_b = load_artifact(path_b);
  CHECK(final_a.bundle.manifest.completed_iterations == 2 * kN);
  CHECK(final_b.bundle.manifest.completed_iterations == 2 * kN);
  CHECK(final_a.bundle.manifest.prng_state == final_b.bundle.manifest.prng_state);
  CHECK(compare_policy_exact(final_a.bundle.result.policy, final_b.bundle.result.policy,
                             final_a.bundle.rows, final_b.bundle.rows) == 0);

  // Independently published generations must be byte-identical.
  const fs::path dir_a = dir / "gen-a";
  const fs::path dir_b = dir / "gen-b";
  fs::create_directories(dir_a);
  fs::create_directories(dir_b);
  const PublishedPolicy gen_a = publish_policy(path_a, dir_a / "gen.db");
  const PublishedPolicy gen_b = publish_policy(path_b, dir_b / "gen.db");
  CHECK(gen_a.sha256_hex == gen_b.sha256_hex);
  CHECK(read_file_bytes(dir_a / "gen.db") == read_file_bytes(dir_b / "gen.db"));
  return 0;
}

// ---------------------------------------------------------------------------
// test 4: resume identity matching
// ---------------------------------------------------------------------------

static int test_resume_identity(const fs::path& dir) {
  const HeadsUpGame fixture = canonical_fixture();
  const DebugTrainingOutput at_n = HeadsUpSolverDebug::train_sampled(fixture, 40, 7, fast_limits());
  const DebugTrainingOutput at_2n =
      HeadsUpSolverDebug::train_sampled(fixture, 80, 7, fast_limits());
  // Canonical game identity actually stored in the checkpoint.
  const HeadsUpGame& game = at_n.result.policy.game();
  const TrainingRows at_n_rows = to_training_rows(at_n.rows);
  const TrainingRows at_2n_rows = to_training_rows(at_2n.rows);
  CheckpointProvenance provenance;
  provenance.prng_identifier = kSampledPrngIdentifier;

  const fs::path base = dir / "identity-base.db";
  create_checkpoint(base, at_n.result, at_n_rows, provenance);

  auto reject_case = [&](const TrainingResult& candidate,
                         const CheckpointProvenance& candidate_provenance) {
    const fs::path trial = dir / ("identity-trial-" + std::to_string(std::rand()) + ".db");
    std::error_code ec;
    fs::copy_file(base, trial, fs::copy_options::overwrite_existing);
    if (!throws_artifact(
            [&] { commit_checkpoint(trial, candidate, at_2n_rows, candidate_provenance); },
            ArtifactErrorKind::IdentityMismatch))
      return false;
    const LoadedArtifact still = load_artifact(trial);
    return still.bundle.manifest.completed_iterations == 40;
  };

  HeadsUpGame changed;
  changed = game;
  changed.root.stacks[0] += 1;
  CHECK(reject_case(rehome(at_2n.result, changed), provenance));
  changed = game;
  changed.root.button = 1 - game.root.button;
  CHECK(reject_case(rehome(at_2n.result, changed), provenance));
  changed = game;
  changed.root.pot += 1;
  CHECK(reject_case(rehome(at_2n.result, changed), provenance));
  changed = game;
  changed.root.flop[0] = card("2s");
  CHECK(reject_case(rehome(at_2n.result, changed), provenance));
  changed = game;
  changed.fixed_runout[0] = card("9s");
  CHECK(reject_case(rehome(at_2n.result, changed), provenance));
  changed = game;
  changed.ranges[0][0].weight *= 0.5;
  CHECK(reject_case(rehome(at_2n.result, changed), provenance));
  changed = game;
  changed.sizes[0].bets[0] = {2, 3};
  CHECK(reject_case(rehome(at_2n.result, changed), provenance));

  CheckpointProvenance other_algorithm = provenance;
  other_algorithm.algorithm_revision = "other-revision";
  CHECK(reject_case(at_2n.result, other_algorithm));
  CheckpointProvenance other_prng = provenance;
  other_prng.prng_identifier = kFullTraversalPrngIdentifier;
  CHECK(reject_case(at_2n.result, other_prng));
  TrainingResult other_seed = at_2n.result;
  other_seed.seed += 1;
  CHECK(reject_case(other_seed, provenance));

  // Cannot commit training state into a published policy.
  const fs::path policy_dir = dir / "identity-policy";
  fs::create_directories(policy_dir);
  const fs::path policy_path = policy_dir / "gen.db";
  publish_policy(base, policy_path);
  CHECK(
      throws_artifact([&] { commit_checkpoint(policy_path, at_2n.result, at_2n_rows, provenance); },
                      ArtifactErrorKind::IdentityMismatch));

  // A changed ENGINE revision still resumes; it is build provenance, not game
  // identity. The manifest records the new engine revision.
  CheckpointProvenance new_engine = provenance;
  new_engine.engine_revision = "next-engine-2";
  const fs::path engine_trial = dir / "identity-engine.db";
  fs::copy_file(base, engine_trial);
  commit_checkpoint(engine_trial, at_2n.result, at_2n_rows, new_engine);
  const LoadedArtifact advanced = load_artifact(engine_trial);
  CHECK(advanced.bundle.manifest.completed_iterations == 80);
  CHECK(advanced.bundle.manifest.engine_revision == "next-engine-2");
  return 0;
}

// ---------------------------------------------------------------------------
// test 5: ENOSPC fault injection and real killed-writer rounds
// ---------------------------------------------------------------------------

static int test_crash_and_full_disk(const fs::path& dir) {
  const HeadsUpGame game = canonical_fixture();
  const DebugTrainingOutput at_n = HeadsUpSolverDebug::train_sampled(game, 40, 11, fast_limits());
  const DebugTrainingOutput at_2n = HeadsUpSolverDebug::train_sampled(game, 80, 11, fast_limits());
  CheckpointProvenance provenance;
  provenance.prng_identifier = kSampledPrngIdentifier;
  const TrainingRows at_n_rows = to_training_rows(at_n.rows);
  const TrainingRows at_2n_rows = to_training_rows(at_2n.rows);

  const fs::path good = dir / "crash-good.db";
  create_checkpoint(good, at_n.result, at_n_rows, provenance);

  // ENOSPC at create time across several write budgets: the failed create
  // removes its database file and no readable artifact is left.
  for (long long budget : {1LL, 4096LL, 16384LL, 40000LL, 65536LL}) {
    const fs::path starved = dir / ("starved-create-" + std::to_string(budget) + ".db");
    fault_vfs::arm(starved.string(), budget);
    bool threw = false;
    try {
      create_checkpoint(starved, at_n.result, at_n_rows, provenance);
    } catch (const ArtifactError& error) {
      threw = true;
      if (error.kind() != ArtifactErrorKind::Io)
        std::printf("ENOSPC create budget=%lld kind=%d rc=%d: %s\n", budget,
                    static_cast<int>(error.kind()), error.sqlite_code(), error.what());
      CHECK(error.kind() == ArtifactErrorKind::Io);
    }
    fault_vfs::disarm();
    CHECK(threw);
    std::error_code ec;
    fs::remove(fs::path(starved.string() + "-journal"), ec);
    CHECK(!fs::exists(starved, ec));
  }

  // ENOSPC during the commit transaction: after hot-journal recovery the file
  // must present the complete pre-commit state, exactly N, never a partial.
  for (long long budget : {1LL, 4096LL, 8192LL, 16384LL}) {
    const fs::path starved = dir / ("starved-commit-" + std::to_string(budget) + ".db");
    std::error_code ec;
    fs::copy_file(good, starved, fs::copy_options::overwrite_existing, ec);
    fault_vfs::arm(starved.string(), budget);
    bool threw = false;
    try {
      commit_checkpoint(starved, at_2n.result, at_2n_rows, provenance);
    } catch (const ArtifactError&) {
      threw = true;
    }
    fault_vfs::disarm();
    CHECK(threw);
    // Operator-style recovery: a writable connection replays the hot journal.
    {
      RawDb recovery(starved, SQLITE_OPEN_READWRITE);
      recovery.exec("SELECT COUNT(*) FROM manifest;");
    }
    const LoadedArtifact reverted = load_artifact(starved);
    CHECK(reverted.bundle.manifest.completed_iterations == 40);
    CHECK(compare_policy_exact(at_n.result.policy, reverted.bundle.result.policy, at_n_rows,
                               reverted.bundle.rows) == 0);
  }

  // Real process kills, with a 19 MB multi-page transaction.
  for (int round = 0; round < 6; ++round) {
    CHECK(run_crash_round(good, dir / ("crash-pre-" + std::to_string(round) + ".db"), false,
                          false) == 0);
  }
  CHECK(run_crash_round(good, dir / "crash-committed.db", true, false) == 0);
  CHECK(run_crash_round(good, dir / "crash-no-journal.db", false, true) == 0);

  // The seed checkpoint itself is untouched.
  const LoadedArtifact still_good = load_artifact(good);
  CHECK(still_good.bundle.manifest.completed_iterations == 40);
  return 0;
}

// ---------------------------------------------------------------------------
// test 6: corruption, version, schema, and validation rejection
// ---------------------------------------------------------------------------

static int test_corruption(const fs::path& dir) {
  const HeadsUpGame game = canonical_fixture();
  const DebugTrainingOutput trained =
      HeadsUpSolverDebug::train_sampled(game, 40, 13, fast_limits());
  CheckpointProvenance provenance;
  provenance.prng_identifier = kSampledPrngIdentifier;
  const TrainingRows trained_rows = to_training_rows(trained.rows);
  const fs::path source = dir / "corrupt-source.db";
  create_checkpoint(source, trained.result, trained_rows, provenance);

  auto scratch = [&](const std::string& name) {
    const fs::path path = dir / name;
    fs::copy_file(source, path, fs::copy_options::overwrite_existing);
    return path;
  };

  // Unknown application id and schema version.
  {
    const fs::path path = scratch("bad-appid.db");
    RawDb raw(path);
    raw.exec("PRAGMA application_id=1;");
    CHECK(
        throws_artifact([&] { (void)load_artifact(path); }, ArtifactErrorKind::UnsupportedVersion));
  }
  {
    // A v1 file whose user_version is bumped to 2 is now a *supported* version,
    // so open_validated selects the schema-v2 DDL set; the v1 stored SQL does
    // not match it and is rejected as InvalidSchema (wrong-version stored
    // text), not UnsupportedVersion.
    const fs::path path = scratch("bad-version.db");
    RawDb raw(path);
    raw.exec("PRAGMA user_version=2;");
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
  // NOT NULL REAL cell (training.regret). SQL itself converts such literals to
  // NULL and would reject the insert, so the values are injected at the byte
  // level. SQLite integrity_check flags stored NaN as a NULL in a NOT NULL
  // column, while infinities pass integrity and must be caught by the reader's
  // own finite-REAL validation.
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
          "UPDATE training SET regret = -72539182736.125 WHERE rowid = "
          "(SELECT rowid FROM training LIMIT 1)");
      CHECK(raw.scalar_i64("SELECT COUNT(*) FROM training WHERE regret = -72539182736.125") == 1);
    }
    rewrite_unique_real(path, kRealSentinel, bits);
    CHECK(throws_artifact([&] { (void)load_artifact(path); }, expected));
  }

  // Probability sum violation with per-cell values inside [0,1]. The selected
  // state has two or three actions in this fixture, so all-0.2 sums to 0.4 or
  // 0.6 and cannot accidentally equal one.
  {
    const fs::path path = scratch("bad-sum.db");
    RawDb raw(path);
    const std::int64_t action_count = raw.scalar_i64(
        "SELECT COUNT(*) FROM actions WHERE info_id = (SELECT MIN(id) FROM"
        " information_states)");
    CHECK(action_count >= 2 && action_count <= 3);
    raw.exec(
        "UPDATE actions SET probability = 0.2 WHERE info_id = (SELECT MIN(id)"
        " FROM information_states)");
    CHECK(throws_artifact([&] { (void)load_artifact(path); }, ArtifactErrorKind::InvalidValue));
  }
  // Integer beyond the 2^53-1 profile, planted in a table column whose SQL
  // CHECK enforces only INTEGER storage.
  {
    const fs::path path = scratch("bad-int.db");
    RawDb raw(path);
    raw.exec(
        "INSERT INTO measurements (id, fixture, seed_hex, iterations, metric,"
        " metric_class, value) VALUES (1, 'f', NULL, 9007199254740992, 'm',"
        " 'estimated', 1.0)");
    CHECK(throws_artifact([&] { (void)load_artifact(path); }, ArtifactErrorKind::InvalidValue));
  }

  // Writer-side content validation.
  {
    TrainingRows bad = trained_rows;
    const auto first = bad.begin();
    bad[first->first].regrets[0] = std::nan("");
    CHECK(throws_artifact(
        [&] { create_checkpoint(dir / "never-nan.db", trained.result, bad, provenance); }));
    bad = trained_rows;
    bad[first->first].average_weights[0] = -0.25;
    CHECK(throws_artifact(
        [&] { create_checkpoint(dir / "never-negative.db", trained.result, bad, provenance); }));
    bad = trained_rows;
    bad[first->first].regrets.push_back(0.0);
    CHECK(throws_artifact(
        [&] { create_checkpoint(dir / "never-mismatch.db", trained.result, bad, provenance); }));
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
// test 7: immutable publication guarantees
// ---------------------------------------------------------------------------

static int test_immutable(const fs::path& dir) {
  const HeadsUpGame game = canonical_fixture();
  const DebugTrainingOutput trained =
      HeadsUpSolverDebug::train_sampled(game, 40, 19, fast_limits());
  CheckpointProvenance provenance;
  provenance.prng_identifier = kSampledPrngIdentifier;
  const TrainingRows trained_rows = to_training_rows(trained.rows);
  const fs::path checkpoint = dir / "immutable-source.db";
  create_checkpoint(checkpoint, trained.result, trained_rows, provenance);

  const fs::path gen_dir = dir / "immutable-gen";
  fs::create_directories(gen_dir);
  const fs::path gen = gen_dir / "generation-1.db";
  const PublishedPolicy published = publish_policy(checkpoint, gen);

  // Never overwrites an existing generation.
  CHECK(throws_artifact([&] { (void)publish_policy(checkpoint, gen); },
                        ArtifactErrorKind::AlreadyExists));

  // No temp names left in the directory.
  for (const auto& entry : fs::directory_iterator(gen_dir))
    CHECK(entry.path().filename().string().rfind(".bs-policy-", 0) != 0);

  // File mode is read-only (0444).
  struct stat st{};
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
  // when the caller does not pin a digest (the digest-pinned load above is
  // rejected first on the SHA-256 mismatch).
  flip_byte(edited, 69);
  LoadOptions expected;
  expected.expected_sha256 = published.sha256;
  CHECK(throws_artifact([&] { (void)load_artifact(edited, expected); },
                        ArtifactErrorKind::DigestMismatch));
  CHECK(throws_artifact([&] { (void)load_artifact(edited); }));

  // A zero-iteration checkpoint is valid but can never be published: the
  // policy is empty.
  const DebugTrainingOutput empty_run =
      HeadsUpSolverDebug::train_sampled(canonical_fixture(), 0, 1, fast_limits());
  CHECK(empty_run.result.status == TrainingStatus::Complete);
  const TrainingRows no_rows;
  const fs::path empty_checkpoint = dir / "empty-checkpoint.db";
  create_checkpoint(empty_checkpoint, empty_run.result, no_rows, provenance);
  const LoadedArtifact empty_loaded = load_artifact(empty_checkpoint);
  CHECK(empty_loaded.bundle.result.policy.rows().empty());
  CHECK(throws_artifact([&] { (void)publish_policy(empty_checkpoint, dir / "empty-gen.db"); },
                        ArtifactErrorKind::InvalidArgument));

  // Reopening with the expected digest works.
  const LoadedArtifact again = load_artifact(gen, expected);
  CHECK(again.sha256_hex == published.sha256_hex);
  return 0;
}

// ---------------------------------------------------------------------------
// test 8: independent adversarial regressions (P1-1, P1-3, P2-2, P2-4, P2-5)
// ---------------------------------------------------------------------------

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

static int test_adversarial(const fs::path& dir) {
  const HeadsUpGame game = canonical_fixture();
  const DebugTrainingOutput trained =
      HeadsUpSolverDebug::train_sampled(game, 40, 23, fast_limits());
  CheckpointProvenance provenance;
  provenance.prng_identifier = kSampledPrngIdentifier;
  const TrainingRows trained_rows = to_training_rows(trained.rows);
  const fs::path source = dir / "adv-source.db";
  create_checkpoint(source, trained.result, trained_rows, provenance);

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

  // --- P1-1: CHECK-stripped rows with canonical DDL text restored ----------
  // If the C++ domain checks were removed, street=16 would index a 3-element
  // array: silent OOB in release, stack-buffer-overflow under ASan.
  const char* kSizesNoCheck =
      "CREATE TABLE sizes (street INTEGER NOT NULL, kind INTEGER NOT NULL,"
      " ordinal INTEGER NOT NULL, numerator INTEGER NOT NULL,"
      " denominator INTEGER NOT NULL, PRIMARY KEY (street, kind, ordinal)) STRICT";
  const char* kRangesNoCheck =
      "CREATE TABLE ranges (player INTEGER NOT NULL, combo INTEGER NOT NULL,"
      " weight REAL NOT NULL, PRIMARY KEY (player, combo)) STRICT";

  struct BadSizeRow {
    const char* name;
    const char* values;
  };
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
           {"adv-range-player2", "(2, 0, 1.0)"},
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

  // P1-1 residual: 2^32-congruent integers must NOT pass a small-domain check
  // via int64->int narrowing. Strip the information_states/actions CHECKs,
  // plant values that narrow to valid small ints, restore the canonical DDL,
  // and require rejection.
  {
    std::string canonical_states;
    std::string canonical_actions;
    {
      RawDb raw(source, SQLITE_OPEN_READONLY);
      canonical_states =
          raw.scalar_text("SELECT sql FROM sqlite_schema WHERE name='information_states'");
      canonical_actions = raw.scalar_text("SELECT sql FROM sqlite_schema WHERE name='actions'");
      CHECK(!canonical_states.empty() && !canonical_actions.empty());
    }
    const char* kStatesNoCheck =
        "CREATE TABLE information_states (id INTEGER PRIMARY KEY, player INTEGER NOT NULL,"
        " card0 INTEGER NOT NULL, card1 INTEGER NOT NULL, public_key TEXT NOT NULL,"
        " UNIQUE (player, card0, card1, public_key)) STRICT";
    const char* kActionsNoCheck =
        "CREATE TABLE actions (info_id INTEGER NOT NULL, ordinal INTEGER NOT NULL,"
        " kind INTEGER NOT NULL, target INTEGER, probability REAL NOT NULL,"
        " PRIMARY KEY (info_id, ordinal)) STRICT";

    // player=2^32 narrows to 0; card0=2^32 -> 0; card1=2^32+1 -> 1.
    {
      const fs::path path = scratch("adv-congruent-states.db");
      overwrite_table_sql(path, "information_states", kStatesNoCheck);
      {
        RawDb update(path);
        update.exec(
            "UPDATE information_states SET player = 4294967296,"
            " card0 = 4294967296, card1 = 4294967297 WHERE id ="
            " (SELECT MIN(id) FROM information_states);");
      }
      overwrite_table_sql(path, "information_states", canonical_states);
      CHECK(throws_artifact([&] { (void)load_artifact(path); }, ArtifactErrorKind::InvalidSchema));
    }
    // Negative control: 2^32+52 narrows to 52 and must be rejected too (it
    // already was); proves the column genuinely holds the planted raw values.
    {
      const fs::path path = scratch("adv-congruent-card52.db");
      overwrite_table_sql(path, "information_states", kStatesNoCheck);
      {
        RawDb update(path);
        update.exec(
            "UPDATE information_states SET card0 = 0, card1 = 4294967348"
            " WHERE id = (SELECT MIN(id) FROM information_states);");
      }
      overwrite_table_sql(path, "information_states", canonical_states);
      CHECK(throws_artifact([&] { (void)load_artifact(path); }));
    }
    // actions.kind = 2^32+1 narrows to 1 (Check).
    {
      const fs::path path = scratch("adv-congruent-actions.db");
      overwrite_table_sql(path, "actions", kActionsNoCheck);
      {
        RawDb update(path);
        update.exec(
            "UPDATE actions SET kind = 4294967297 WHERE ordinal = 0 AND info_id ="
            " (SELECT MIN(id) FROM information_states);");
      }
      overwrite_table_sql(path, "actions", canonical_actions);
      CHECK(throws_artifact([&] { (void)load_artifact(path); }, ArtifactErrorKind::InvalidValue));
    }
  }

  // --- P2-2: duplicate public board cards -----------------------------------
  for (const auto& [name, key] : std::vector<std::pair<std::string, std::string>>{
           {"adv-dup-flop", "0|3,3,3|"},
           {"adv-turn-repeats-flop", "1|3,6,21,3|"},
       }) {
    const fs::path path = scratch(name + ".db");
    {
      RawDb raw(path);
      const std::int64_t first_id = raw.scalar_i64("SELECT MIN(id) FROM information_states");
      raw.exec("UPDATE information_states SET public_key = '" + key +
               "' WHERE id = " + std::to_string(first_id) + ";");
    }
    CHECK(throws_artifact([&] { (void)load_artifact(path); }, ArtifactErrorKind::InvalidSchema));
    CHECK(throws_artifact(
        [&] {
          const fs::path bad_gen = dir / (name + "-gen.db");
          (void)publish_policy(path, bad_gen);
        },
        ArtifactErrorKind::InvalidSchema));
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
    fs::copy_file(wal_base, path, fs::copy_options::overwrite_existing, ec);
    fs::copy_file(fs::path(wal_base.string() + "-wal"), fs::path(path.string() + "-wal"),
                  fs::copy_options::overwrite_existing, ec);
    CHECK(throws_artifact([&] { (void)load_artifact(path); }));
  }

  // (ii) A legacy (version-1) main file next to a stray committed -wal must
  // still be rejected by the sidecar check, and the immutable reader never
  // opens or creates sidecars: no -shm appears and the -wal is untouched.
  {
    const fs::path gen_dir = dir / "adv-wal-gen";
    fs::create_directories(gen_dir);
    const fs::path gen = gen_dir / "gen.db";
    const PublishedPolicy published = publish_policy(source, gen);
    const fs::path victim = dir / "adv-wal-legacy.db";
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
      {"roundtrip", test_roundtrip},
      {"roundtrip-v2", test_roundtrip_v2},
      {"canonical-keys", test_canonical_keys},
      {"split-run", test_split_run},
      {"resume-identity", test_resume_identity},
      {"crash-full-disk", test_crash_and_full_disk},
      {"corruption", test_corruption},
      {"immutable", test_immutable},
      {"adversarial", test_adversarial},
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
