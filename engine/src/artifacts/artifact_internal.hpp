// Private implementation plumbing for bigshark_artifacts. This header and
// everything under engine/src/artifacts is private to the static library:
// neither SQLite nor these helpers may appear in public artifact headers or
// cross the boundary into solver/service code.
#pragma once

#include <array>
#include <bs/heads_up.hpp>
#include <bs/heads_up_solver.hpp>
#include <bs/strategy_artifact.hpp>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "sqlite3.h"

namespace bs::artifacts::detail {

// Throws ArtifactError with SQLite context. `rc` is the failing return code.
[[noreturn]] void throw_sqlite_error(sqlite3* db, int rc, const std::string& operation);

// Maps a SQLite primary/extended result code to the artifact error taxonomy.
ArtifactErrorKind classify_sqlite(int rc);

// Every table is STRICT and created in foreign-key dependency order.
extern const char* const kSchemaDdl[];

// A SQLite database connection. All opens enable foreign keys; readers are
// additionally pinned read-only/query-only/defensive.
class Db {
 public:
  Db() = default;
  ~Db();
  Db(const Db&) = delete;
  Db& operator=(const Db&) = delete;
  Db(Db&& other) noexcept : db_(other.db_) { other.db_ = nullptr; }
  Db& operator=(Db&& other) noexcept;

  void open(const std::string& path, int flags, const std::string& operation);
  // Executes zero or more SQL statements; throws on any error.
  void exec(std::string_view sql, const std::string& operation);
  // scalar PRAGMA integer
  std::int64_t pragma_i64(std::string_view pragma, const std::string& operation);
  // scalar PRAGMA text
  std::string pragma_text(std::string_view pragma, const std::string& operation);
  void close() noexcept;
  sqlite3* get() const noexcept { return db_; }

 private:
  sqlite3* db_ = nullptr;
};

class Stmt {
 public:
  Stmt(sqlite3* db, std::string_view sql, const std::string& operation);
  ~Stmt();
  Stmt(const Stmt&) = delete;
  Stmt& operator=(const Stmt&) = delete;

  void bind_i64(int index, std::int64_t value);
  void bind_u64_chips(int index, std::uint64_t value);
  void bind_double(int index, double value);
  void bind_text(int index, std::string_view value);
  void bind_null(int index);
  // true on SQLITE_ROW, false on SQLITE_DONE.
  bool step(const std::string& operation);
  void reset_and_clear();

  std::int64_t column_i64(int index) const;
  double column_double(int index) const;
  std::string_view column_text(int index) const;
  bool column_null(int index) const;

 private:
  sqlite3* db_ = nullptr;
  sqlite3_stmt* stmt_ = nullptr;
};

// --- filesystem / digest helpers (POSIX, implemented in artifact_digest.cpp) ---

Sha256Digest sha256_file(const std::filesystem::path& path, std::uint64_t max_file_bytes);
// Validates the SQLite magic, legacy (non-WAL) format bytes, page geometry,
// and agreement between the byte length and the committed in-header database
// size before SQLite opens the file. Throws Corrupt on torn, WAL, or
// truncated physical files.
void validate_database_header(const std::filesystem::path& path, std::uint64_t file_bytes);
// Rejects any unhashed sibling journal file (<path>-wal, -shm, -journal) that
// SQLite would replay transparently, bypassing the main-file digest.
void reject_sqlite_sidecars(const std::filesystem::path& path);
// Process-once SQLite hard heap bound for untrusted-file parsing (P2-4).
void ensure_reader_heap_bound();
std::string to_hex(const Sha256Digest& digest);
std::uint64_t file_size_required(const std::filesystem::path& path, std::uint64_t max_file_bytes);
void fsync_file(const std::filesystem::path& path);
void fsync_directory(const std::filesystem::path& directory);
// link(2) with EEXIST translated to ArtifactError(AlreadyExists).
void exclusive_hard_link(const std::filesystem::path& from, const std::filesystem::path& to);
void make_read_only(const std::filesystem::path& path);
// mkstemp-based unique file in directory; returns a path that exists empty.
std::filesystem::path make_unique_temp(const std::filesystem::path& directory);

// --- codec helpers (implemented in artifact_codec.cpp) ---

std::string u64_to_hex(std::uint64_t value);
std::uint64_t hex_to_u64(std::string_view text);
bool is_hex_word(std::string_view text);

int action_kind_id(poker::ActionType type);
poker::ActionType action_kind_from_id(int id);
char action_kind_char(poker::ActionType type);

// Canonical zero-based id of the unordered combo (card0 < card1), range
// 0..1325 over the 1326 hands.
int combo_id(std::array<int, 2> cards);
std::array<int, 2> combo_cards(int combo);

// Throws ArtifactError(InvalidValue) unless value is finite.
double require_finite(double value, const std::string& field);
// Throws unless the probabilities are finite, nonnegative, sum to one within
// 1e-12, and the row is non-empty.
void validate_probabilities(const std::vector<double>& probabilities, const std::string& field);

// Assembles immutable solver domain records after validation. It is the one
// production consumer granted access to HeadsUpPolicy's private rows.
class PolicyAssembler {
 public:
  static void set_game(solver::HeadsUpPolicy& policy, const solver::HeadsUpGame& game);
  static void add_row(solver::HeadsUpPolicy& policy, solver::InformationKey key,
                      solver::PolicyRow row);
};

// Applies the checkpoint-writer connection pragmas (rollback journaling and
// synchronous=FULL) and then reads them back ON THE SAME CONNECTION, failing
// closed if they did not take effect. Split so the verification is whitebox
// testable; a pragma is per-connection and never observable cross-connection.
void apply_writer_pragmas(Db& db);
void verify_writer_pragmas(Db& db);

// Opens an artifact for immutable, untrusted reading: percent-encoded
// file: URI with ?immutable=1 plus READONLY/URI flags, then the defensive
// reader pragmas. No journal replay or sidecar access occurs.
void open_immutable_reader(Db& db, const std::filesystem::path& path);

}  // namespace bs::artifacts::detail
