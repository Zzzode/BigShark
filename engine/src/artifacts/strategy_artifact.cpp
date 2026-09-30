#include <algorithm>
#include <array>
#include <bit>
#include <bs/heads_up.hpp>
#include <bs/heads_up_solver.hpp>
#include <bs/strategy_artifact.hpp>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "artifact_internal.hpp"

namespace bs::artifacts {

using abstraction::SizeSchedule;
using poker::Action;
using poker::ActionType;
using poker::Chips;
using poker::GameDef;
using poker::HeadsUpRoot;
using poker::RulesVariant;
using solver::HeadsUpGame;
using solver::InformationKey;
using solver::NSeatTerminationPhase;
using solver::PolicyRow;
using solver::SeatTrainingResult;
using solver::TrainingResult;
using solver::TrainingStatus;
using solver::WeightedHand;

namespace {

namespace dt = detail;

[[noreturn]] void fail(ArtifactErrorKind kind, const std::string& message) {
  throw ArtifactError(kind, message);
}

void check(bool ok, ArtifactErrorKind kind, const std::string& message) {
  if (!ok)
    fail(kind, message);
}

// Normalized positive regret-matching probabilities, the same convention the
// solver uses to publish a policy. Used only as a write/read cross-check.
std::vector<double> normalized(const std::vector<double>& weights) {
  double sum = 0;
  for (double w : weights)
    sum += std::max(0.0, w);
  std::vector<double> out(weights.size(), 1.0 / static_cast<double>(weights.size()));
  if (sum > 0)
    for (std::size_t i = 0; i < weights.size(); ++i)
      out[i] = std::max(0.0, weights[i]) / sum;
  return out;
}

bool same_fraction(const solver::Fraction& a, const solver::Fraction& b) {
  return a.numerator == b.numerator && a.denominator == b.denominator;
}

bool same_size_schedule(const solver::SizeSchedule& a, const solver::SizeSchedule& b) {
  // Only the POSTFLOP entries are compared, and that bound is deliberate rather
  // than incidental: the artifact stores exactly those three (RFC 0007 rollout
  // step 3 widens the store and this loop together), so a schedule restored
  // from an artifact has an empty preflop entry while a freshly built one has
  // the declared default. Comparing the fourth entry here - before the store
  // knows about it - makes every resume fail as an identity mismatch, which is
  // a correct comparison applied one step too early. The two must land in the
  // same change.
  constexpr std::size_t kStoredStreets = 3;
  for (std::size_t street = 0; street < kStoredStreets; ++street) {
    if (a[street].bets.size() != b[street].bets.size() ||
        a[street].raises.size() != b[street].raises.size())
      return false;
    for (std::size_t i = 0; i < a[street].bets.size(); ++i)
      if (!same_fraction(a[street].bets[i], b[street].bets[i]))
        return false;
    for (std::size_t i = 0; i < a[street].raises.size(); ++i)
      if (!same_fraction(a[street].raises[i], b[street].raises[i]))
        return false;
  }
  return true;
}

bool same_ranges(const std::array<std::vector<WeightedHand>, 2>& a,
                 const std::array<std::vector<WeightedHand>, 2>& b) {
  for (std::size_t player = 0; player < 2; ++player) {
    if (a[player].size() != b[player].size())
      return false;
    for (std::size_t i = 0; i < a[player].size(); ++i) {
      if (a[player][i].cards != b[player][i].cards)
        return false;
      // Range weights are normalized by the trainer and stored as IEEE REAL, so
      // resume identity compares the exact double, not a tolerance.
      if (std::bit_cast<std::uint64_t>(a[player][i].weight) !=
          std::bit_cast<std::uint64_t>(b[player][i].weight))
        return false;
    }
  }
  return true;
}

bool same_game(const HeadsUpGame& a, const HeadsUpGame& b) {
  return a.root.flop == b.root.flop && a.root.stacks == b.root.stacks &&
         a.root.contributions == b.root.contributions && a.root.pot == b.root.pot &&
         a.root.big_blind == b.root.big_blind && a.root.button == b.root.button &&
         a.fixed_runout == b.fixed_runout && same_size_schedule(a.sizes, b.sizes) &&
         same_ranges(a.ranges, b.ranges);
}

void validate_probabilities_local(const std::vector<double>& probabilities,
                                  const std::string& field) {
  dt::validate_probabilities(probabilities, field);
}

const char* kind_text(ArtifactKind kind) {
  return kind == ArtifactKind::Checkpoint ? "checkpoint" : "policy";
}

ArtifactKind kind_from_text(std::string_view text) {
  if (text == "checkpoint")
    return ArtifactKind::Checkpoint;
  if (text == "policy")
    return ArtifactKind::Policy;
  fail(ArtifactErrorKind::InvalidSchema, "unknown artifact kind: " + std::string(text));
}

const char* validation_text(ValidationState state) {
  switch (state) {
    case ValidationState::Unvalidated:
      return "unvalidated";
    case ValidationState::Validated:
      return "validated";
    case ValidationState::Rejected:
      return "rejected";
  }
  return "unvalidated";
}

ValidationState validation_from_text(std::string_view text) {
  if (text == "unvalidated")
    return ValidationState::Unvalidated;
  if (text == "validated")
    return ValidationState::Validated;
  if (text == "rejected")
    return ValidationState::Rejected;
  fail(ArtifactErrorKind::InvalidSchema, "unknown validation state");
}

const char* run_status_text(RunStatus status) {
  return status == RunStatus::Complete ? "complete" : "resource_limit";
}

RunStatus run_status_from_text(std::string_view text) {
  if (text == "complete")
    return RunStatus::Complete;
  if (text == "resource_limit")
    return RunStatus::ResourceLimit;
  fail(ArtifactErrorKind::InvalidSchema, "unknown training run status");
}

void validate_root(const HeadsUpRoot& root) {
  std::array<bool, 52> used{};
  for (int card : root.flop) {
    check(card >= 0 && card < 52 && !used[card], ArtifactErrorKind::InvalidArgument,
          "invalid or duplicate flop card");
    used[card] = true;
  }
  check(root.button <= 1, ArtifactErrorKind::InvalidArgument, "button outside seats");
  check(root.pot > 0 && root.pot <= kMaxStoredChips, ArtifactErrorKind::InvalidArgument,
        "pot outside the 2^53-1 profile");
  check(root.big_blind > 0 && root.big_blind <= kMaxStoredChips, ArtifactErrorKind::InvalidArgument,
        "big blind outside the numeric profile");
  for (Chips stack : root.stacks)
    check(stack <= kMaxStoredChips, ArtifactErrorKind::InvalidArgument,
          "stack outside the 2^53-1 profile");
  for (Chips contribution : root.contributions)
    check(contribution <= kMaxStoredChips, ArtifactErrorKind::InvalidArgument,
          "contribution outside the 2^53-1 profile");
}

void validate_game(const HeadsUpGame& game) {
  validate_root(game.root);
  std::array<bool, 52> used{};
  for (int card : game.root.flop)
    used[card] = true;
  for (auto fixed : game.fixed_runout) {
    if (!fixed)
      continue;
    check(*fixed >= 0 && *fixed < 52 && !used[*fixed], ArtifactErrorKind::InvalidArgument,
          "invalid or duplicate fixed runout card");
    used[*fixed] = true;
  }
  for (std::size_t player = 0; player < 2; ++player) {
    check(!game.ranges[player].empty() && game.ranges[player].size() <= 1326,
          ArtifactErrorKind::InvalidArgument, "range must contain 1..1326 combos");
    for (const WeightedHand& hand : game.ranges[player]) {
      check(hand.cards[0] >= 0 && hand.cards[1] < 52 && hand.cards[0] < hand.cards[1],
            ArtifactErrorKind::InvalidArgument, "range combo cards invalid or unsorted");
      check(!used[hand.cards[0]] && !used[hand.cards[1]], ArtifactErrorKind::InvalidArgument,
            "range combo shares a public card");
      check(std::isfinite(hand.weight) && hand.weight >= 0, ArtifactErrorKind::InvalidArgument,
            "range weight must be finite and nonnegative");
    }
  }
  for (const auto& street : game.sizes)
    for (const auto* fractions : {&street.bets, &street.raises})
      for (const solver::Fraction& fraction : *fractions) {
        check(fraction.numerator > 0 && fraction.numerator <= kMaxStoredChips &&
                  fraction.denominator > 0 && fraction.denominator <= kMaxStoredChips,
              ArtifactErrorKind::InvalidArgument, "size fraction outside the numeric profile");
      }
}

// Validates that raw training rows agree with the immutable policy and returns
// the policy row count. Regrets may be negative but must be finite; average
// weights are nonnegative and normalize to the published probabilities.
std::size_t validate_content(const TrainingResult& result, const TrainingRows& rows,
                             ArtifactKind kind) {
  const HeadsUpGame& game = result.policy.game();
  validate_game(game);
  const auto& policy_rows = result.policy.rows();
  if (kind == ArtifactKind::Policy) {
    check(!policy_rows.empty(), ArtifactErrorKind::InvalidArgument,
          "cannot publish an empty policy");
    check(result.status == TrainingStatus::Complete, ArtifactErrorKind::InvalidArgument,
          "cannot publish a resource-limited run");
  }
  if (kind == ArtifactKind::Checkpoint && !policy_rows.empty())
    check(rows.size() == policy_rows.size(), ArtifactErrorKind::InvalidArgument,
          "checkpoint raw rows must cover every policy row");
  for (const auto& [key, policy_row] : policy_rows) {
    check(key.size() >= 4, ArtifactErrorKind::InvalidArgument, "information key too short");
    dt::validate_probabilities(policy_row.probabilities, "policy probabilities");
    check(policy_row.actions.size() == policy_row.probabilities.size(),
          ArtifactErrorKind::InvalidArgument, "policy action/probability length mismatch");
    const auto found = rows.find(key);
    if (kind == ArtifactKind::Checkpoint && !policy_rows.empty()) {
      check(found != rows.end(), ArtifactErrorKind::InvalidArgument,
            "checkpoint missing a raw training row");
      const TrainingRow& raw = found->second;
      check(raw.actions == policy_row.actions, ArtifactErrorKind::InvalidArgument,
            "raw row actions differ from policy actions");
      check(raw.regrets.size() == policy_row.actions.size() &&
                raw.average_weights.size() == policy_row.actions.size(),
            ArtifactErrorKind::InvalidArgument, "raw row vector length mismatch");
      for (double regret : raw.regrets)
        dt::require_finite(regret, "cumulative regret");
      for (double average : raw.average_weights) {
        dt::require_finite(average, "average weight");
        check(average >= 0, ArtifactErrorKind::InvalidArgument,
              "average weight must be nonnegative");
      }
      const std::vector<double> rebuilt = normalized(raw.average_weights);
      for (std::size_t i = 0; i < rebuilt.size(); ++i)
        check(std::abs(rebuilt[i] - policy_row.probabilities[i]) <= 1e-12,
              ArtifactErrorKind::InvalidArgument,
              "published probability is not the normalized average weight");
    }
    // Every key must have a canonical revision-1 encoding.
    (void)encode_public_key(key, kArtifactSchemaVersion);
  }
  if (kind == ArtifactKind::Checkpoint && !policy_rows.empty()) {
    for (const auto& [key, raw] : rows)
      check(policy_rows.contains(key), ArtifactErrorKind::InvalidArgument,
            "raw row has no policy row");
  }
  return policy_rows.size();
}

ArtifactManifest manifest_for(const TrainingResult& result, const CheckpointProvenance& provenance,
                              ArtifactKind kind, ValidationState validation) {
  check(!provenance.algorithm_revision.empty() && !provenance.engine_revision.empty(),
        ArtifactErrorKind::InvalidArgument, "manifest provenance strings must be non-empty");
  check(provenance.prng_identifier == kSampledPrngIdentifier ||
            provenance.prng_identifier == kFullTraversalPrngIdentifier,
        ArtifactErrorKind::InvalidArgument, "unsupported PRNG identifier");
  ArtifactManifest manifest;
  manifest.kind = kind;
  manifest.algorithm_revision = provenance.algorithm_revision;
  manifest.engine_revision = provenance.engine_revision;
  manifest.validation = validation;
  manifest.completed_iterations = result.completed_iterations;
  manifest.prng_identifier = provenance.prng_identifier;
  manifest.seed = result.seed;
  manifest.prng_state = result.prng_state;
  manifest.run_status =
      result.status == TrainingStatus::Complete ? RunStatus::Complete : RunStatus::ResourceLimit;
  manifest.nodes = result.nodes;
  manifest.information_sets = result.information_sets;
  manifest.accounted_bytes = result.accounted_bytes;
  return manifest;
}

}  // namespace

namespace detail {

// --- SQLite RAII ---

ArtifactErrorKind classify_sqlite(int rc) {
  const int primary = rc & 0xFF;
  if (primary == SQLITE_NOMEM)
    return ArtifactErrorKind::CapacityExceeded;
  if (primary == SQLITE_CONSTRAINT || primary == SQLITE_FORMAT || primary == SQLITE_NOTADB)
    return ArtifactErrorKind::Corrupt;
  if (primary == SQLITE_READONLY)
    return ArtifactErrorKind::Io;
  if (primary == SQLITE_FULL || primary == SQLITE_IOERR || primary == SQLITE_CANTOPEN ||
      primary == SQLITE_NOMEM)
    return ArtifactErrorKind::Io;
  return ArtifactErrorKind::Sqlite;
}

void throw_sqlite_error(sqlite3* db, int rc, const std::string& operation) {
  const char* message = db != nullptr ? sqlite3_errmsg(db) : "sqlite error";
  throw ArtifactError(classify_sqlite(rc),
                      operation + ": " + message + " (sqlite rc=" + std::to_string(rc) + ")", rc);
}

Db& Db::operator=(Db&& other) noexcept {
  if (this != &other) {
    close();
    db_ = other.db_;
    other.db_ = nullptr;
  }
  return *this;
}

Db::~Db() {
  close();
}

void Db::close() noexcept {
  if (db_ != nullptr) {
    sqlite3_close_v2(db_);
    db_ = nullptr;
  }
}

void Db::open(const std::string& path, int flags, const std::string& operation) {
  close();
  static_assert(SQLITE_VERSION_NUMBER >= 3037000, "RFC 0005 requires SQLite 3.37 or newer");
  if (sqlite3_libversion_number() < 3037000)
    throw ArtifactError(ArtifactErrorKind::UnsupportedVersion,
                        "SQLite runtime older than 3.37: " + std::string(sqlite3_libversion()));
  const int rc = sqlite3_open_v2(path.c_str(), &db_, flags, nullptr);
  if (rc != SQLITE_OK) {
    std::string message = sqlite3_errmsg(db_ != nullptr ? db_ : nullptr);
    close();
    throw ArtifactError(
        ArtifactErrorKind::Io,
        operation + ": cannot open database: " + message + " (rc=" + std::to_string(rc) + ")", rc);
  }
  sqlite3_busy_timeout(db_, 10000);
}

void Db::exec(std::string_view sql, const std::string& operation) {
  char* error = nullptr;
  const int rc = sqlite3_exec(db_, std::string(sql).c_str(), nullptr, nullptr, &error);
  if (rc != SQLITE_OK) {
    std::string message = error != nullptr ? error : "unknown error";
    sqlite3_free(error);
    throw_sqlite_error(db_, rc, operation + ": " + message);
  }
}

std::int64_t Db::pragma_i64(std::string_view pragma, const std::string& operation) {
  std::string sql = "PRAGMA " + std::string(pragma);
  sqlite3_stmt* stmt = nullptr;
  const int rc = sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr);
  if (rc != SQLITE_OK)
    throw_sqlite_error(db_, rc, operation);
  const int step = sqlite3_step(stmt);
  std::int64_t value = 0;
  if (step == SQLITE_ROW)
    value = sqlite3_column_int64(stmt, 0);
  sqlite3_finalize(stmt);
  if (step != SQLITE_ROW && step != SQLITE_DONE)
    throw_sqlite_error(db_, step, operation);
  return value;
}

std::string Db::pragma_text(std::string_view pragma, const std::string& operation) {
  std::string sql = "PRAGMA " + std::string(pragma);
  sqlite3_stmt* stmt = nullptr;
  const int rc = sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr);
  if (rc != SQLITE_OK)
    throw_sqlite_error(db_, rc, operation);
  const int step = sqlite3_step(stmt);
  std::string value;
  if (step == SQLITE_ROW) {
    const unsigned char* text = sqlite3_column_text(stmt, 0);
    if (text != nullptr)
      value = reinterpret_cast<const char*>(text);
  }
  sqlite3_finalize(stmt);
  if (step != SQLITE_ROW && step != SQLITE_DONE)
    throw_sqlite_error(db_, step, operation);
  return value;
}

Stmt::Stmt(sqlite3* db, std::string_view sql, const std::string& operation) : db_(db) {
  const int rc = sqlite3_prepare_v2(db, std::string(sql).c_str(), -1, &stmt_, nullptr);
  if (rc != SQLITE_OK)
    throw_sqlite_error(db, rc, operation + " [" + std::string(sql) + "]");
}

Stmt::~Stmt() {
  sqlite3_finalize(stmt_);
}

void Stmt::bind_i64(int index, std::int64_t value) {
  const int rc = sqlite3_bind_int64(stmt_, index, value);
  if (rc != SQLITE_OK)
    throw_sqlite_error(db_, rc, "bind integer");
}

void Stmt::bind_u64_chips(int index, std::uint64_t value) {
  if (value > kMaxStoredChips)
    throw ArtifactError(ArtifactErrorKind::InvalidArgument,
                        "integer exceeds the 2^53-1 numeric profile");
  bind_i64(index, static_cast<std::int64_t>(value));
}

void Stmt::bind_double(int index, double value) {
  if (!std::isfinite(value))
    throw ArtifactError(ArtifactErrorKind::InvalidArgument, "cannot bind non-finite REAL");
  const int rc = sqlite3_bind_double(stmt_, index, value);
  if (rc != SQLITE_OK)
    throw_sqlite_error(db_, rc, "bind real");
}

void Stmt::bind_text(int index, std::string_view value) {
  const int rc = sqlite3_bind_text(stmt_, index, value.data(), static_cast<int>(value.size()),
                                   SQLITE_TRANSIENT);
  if (rc != SQLITE_OK)
    throw_sqlite_error(db_, rc, "bind text");
}

void Stmt::bind_null(int index) {
  const int rc = sqlite3_bind_null(stmt_, index);
  if (rc != SQLITE_OK)
    throw_sqlite_error(db_, rc, "bind null");
}

bool Stmt::step(const std::string& operation) {
  const int rc = sqlite3_step(stmt_);
  if (rc == SQLITE_ROW)
    return true;
  if (rc == SQLITE_DONE)
    return false;
  throw_sqlite_error(db_, rc, operation);
}

void Stmt::reset_and_clear() {
  sqlite3_reset(stmt_);
  sqlite3_clear_bindings(stmt_);
}

std::int64_t Stmt::column_i64(int index) const {
  return sqlite3_column_int64(stmt_, index);
}

double Stmt::column_double(int index) const {
  return sqlite3_column_double(stmt_, index);
}

std::string_view Stmt::column_text(int index) const {
  const unsigned char* text = sqlite3_column_text(stmt_, index);
  const int bytes = sqlite3_column_bytes(stmt_, index);
  if (text == nullptr)
    return {};
  return {reinterpret_cast<const char*>(text), static_cast<std::size_t>(bytes)};
}

bool Stmt::column_null(int index) const {
  return sqlite3_column_type(stmt_, index) == SQLITE_NULL;
}

}  // namespace detail

namespace {

constexpr const char* kWriterPragmas =
    "PRAGMA journal_mode=DELETE;"
    "PRAGMA synchronous=FULL;"
    "PRAGMA temp_store=MEMORY;"
    "PRAGMA foreign_keys=ON;";

constexpr const char* kReaderPragmas =
    "PRAGMA query_only=ON;"
    "PRAGMA temp_store=MEMORY;"
    "PRAGMA foreign_keys=ON;"
    "PRAGMA trusted_schema=OFF;"
    "PRAGMA mmap_size=0;"
    "PRAGMA cache_size=-65536;";

// Schema v1 (RFC 0004 heads-up profile). FROZEN byte-for-byte: the v1 DDL
// text is never edited, and every v1 fixture's stored SQL must match it
// exactly. The seat-generic schema v2 (RFC 0009 D3) lives in kSchemaDdlV2.
const char* const kSchemaDdlV1[] = {
    "CREATE TABLE manifest ("
    " id INTEGER PRIMARY KEY CHECK (id = 1),"
    " kind TEXT NOT NULL CHECK (kind IN ('checkpoint','policy')),"
    " algorithm_revision TEXT NOT NULL,"
    " info_key_revision INTEGER NOT NULL CHECK (info_key_revision = 1),"
    " numeric_profile TEXT NOT NULL,"
    " validation_state TEXT NOT NULL CHECK (validation_state IN"
    " ('unvalidated','validated','rejected')),"
    " engine_revision TEXT NOT NULL,"
    " completed_iterations INTEGER NOT NULL CHECK (completed_iterations >= 0),"
    " prng_identifier TEXT NOT NULL,"
    " seed_hex TEXT NOT NULL CHECK (length(seed_hex) = 16),"
    " prng_state_hex TEXT NOT NULL CHECK (length(prng_state_hex) = 16),"
    " run_status TEXT NOT NULL CHECK (run_status IN ('complete','resource_limit')),"
    " nodes INTEGER NOT NULL CHECK (nodes >= 0),"
    " information_sets INTEGER NOT NULL CHECK (information_sets >= 0),"
    " accounted_bytes INTEGER NOT NULL CHECK (accounted_bytes >= 0)"
    ") STRICT;",

    "CREATE TABLE game ("
    " id INTEGER PRIMARY KEY CHECK (id = 1),"
    " rules_id TEXT NOT NULL,"
    " button INTEGER NOT NULL CHECK (button IN (0,1)),"
    " big_blind INTEGER NOT NULL CHECK (big_blind > 0 AND"
    " big_blind <= 9007199254740991),"
    " root_street INTEGER NOT NULL CHECK (root_street = 0),"
    " flop0 INTEGER NOT NULL CHECK (flop0 BETWEEN 0 AND 51),"
    " flop1 INTEGER NOT NULL CHECK (flop1 BETWEEN 0 AND 51),"
    " flop2 INTEGER NOT NULL CHECK (flop2 BETWEEN 0 AND 51),"
    " fixed_turn INTEGER CHECK (fixed_turn IS NULL OR fixed_turn BETWEEN 0 AND 51),"
    " fixed_river INTEGER CHECK (fixed_river IS NULL OR fixed_river BETWEEN 0 AND 51),"
    " stack0 INTEGER NOT NULL CHECK (stack0 BETWEEN 0 AND 9007199254740991),"
    " stack1 INTEGER NOT NULL CHECK (stack1 BETWEEN 0 AND 9007199254740991),"
    " contribution0 INTEGER NOT NULL CHECK (contribution0 BETWEEN 0 AND 9007199254740991),"
    " contribution1 INTEGER NOT NULL CHECK (contribution1 BETWEEN 0 AND 9007199254740991),"
    " pot INTEGER NOT NULL CHECK (pot > 0 AND pot <= 9007199254740991),"
    " utility_id TEXT NOT NULL"
    ") STRICT;",

    "CREATE TABLE sizes ("
    " street INTEGER NOT NULL CHECK (street BETWEEN 0 AND 2),"
    " kind INTEGER NOT NULL CHECK (kind IN (0,1)),"
    " ordinal INTEGER NOT NULL CHECK (ordinal >= 0 AND ordinal < 32),"
    " numerator INTEGER NOT NULL CHECK (numerator > 0 AND"
    " numerator <= 9007199254740991),"
    " denominator INTEGER NOT NULL CHECK (denominator > 0 AND"
    " denominator <= 9007199254740991),"
    " PRIMARY KEY (street, kind, ordinal)"
    ") STRICT;",

    "CREATE TABLE ranges ("
    " player INTEGER NOT NULL CHECK (player IN (0,1)),"
    " combo INTEGER NOT NULL CHECK (combo BETWEEN 0 AND 1325),"
    " weight REAL NOT NULL CHECK (weight >= 0 AND weight = weight),"
    " PRIMARY KEY (player, combo)"
    ") STRICT;",

    "CREATE TABLE information_states ("
    " id INTEGER PRIMARY KEY CHECK (id >= 0),"
    " player INTEGER NOT NULL CHECK (player IN (0,1)),"
    " card0 INTEGER NOT NULL CHECK (card0 BETWEEN 0 AND 51),"
    " card1 INTEGER NOT NULL CHECK (card1 BETWEEN 0 AND 51 AND card1 > card0),"
    " public_key TEXT NOT NULL,"
    " UNIQUE (player, card0, card1, public_key)"
    ") STRICT;",

    "CREATE TABLE actions ("
    " info_id INTEGER NOT NULL REFERENCES information_states(id) ON DELETE CASCADE,"
    " ordinal INTEGER NOT NULL CHECK (ordinal >= 0 AND ordinal < 32),"
    " kind INTEGER NOT NULL CHECK (kind BETWEEN 0 AND 4),"
    " target INTEGER CHECK (target IS NULL OR (target >= 0 AND"
    " target <= 9007199254740991)),"
    " probability REAL NOT NULL CHECK (probability >= 0 AND probability <= 1),"
    " PRIMARY KEY (info_id, ordinal),"
    " CHECK ((kind IN (3,4)) = (target IS NOT NULL))"
    ") STRICT;",

    "CREATE TABLE training ("
    " info_id INTEGER NOT NULL,"
    " ordinal INTEGER NOT NULL CHECK (ordinal >= 0 AND ordinal < 32),"
    " regret REAL NOT NULL,"
    " average_weight REAL NOT NULL CHECK (average_weight >= 0 AND"
    " average_weight = average_weight),"
    " PRIMARY KEY (info_id, ordinal),"
    " FOREIGN KEY (info_id, ordinal) REFERENCES actions (info_id, ordinal)"
    " ON DELETE CASCADE"
    ") STRICT;",

    "CREATE TABLE bounds ("
    " id INTEGER PRIMARY KEY CHECK (id >= 0),"
    " public_root TEXT NOT NULL,"
    " responder_combo INTEGER CHECK (responder_combo IS NULL OR"
    " responder_combo BETWEEN 0 AND 1325),"
    " baseline_cf_mass REAL CHECK (baseline_cf_mass IS NULL OR"
    " (baseline_cf_mass >= 0 AND baseline_cf_mass = baseline_cf_mass)),"
    " normalized_bound REAL,"
    " certification_method TEXT,"
    " measurements_json TEXT,"
    " UNIQUE (public_root, responder_combo)"
    ") STRICT;",

    "CREATE TABLE measurements ("
    " id INTEGER PRIMARY KEY CHECK (id >= 0),"
    " fixture TEXT NOT NULL,"
    " seed_hex TEXT CHECK (seed_hex IS NULL OR length(seed_hex) = 16),"
    " iterations INTEGER CHECK (iterations IS NULL OR iterations >= 0),"
    " metric TEXT NOT NULL,"
    " metric_class TEXT NOT NULL CHECK (metric_class IN ('exact','estimated')),"
    " value REAL NOT NULL CHECK (value = value AND abs(value) < 1e308),"
    " UNIQUE (fixture, seed_hex, iterations, metric)"
    ") STRICT;",
};

// Schema v2 (RFC 0009 D3): the seat-generic profile. manifest gains a declared
// AbstractionId and terminal depth; game becomes seat-generic (player_count,
// button 0..9, ante, root_street 0..3, terminal_depth) and drops the fixed
// stack0/1/contribution0/1 columns; game_seats stores one (stack, contribution)
// row per occupied seat; ranges/information_states admit player 0..9; sizes
// admits street 0..3; bounds gains the (seat, combo) dimension. bounds has NO
// writer in W2b (validated-when-present). actions/training/measurements are
// byte-identical to v1. The writer supports flop-rooted games only.
const char* const kSchemaDdlV2[] = {
    "CREATE TABLE manifest ("
    " id INTEGER PRIMARY KEY CHECK (id = 1),"
    " kind TEXT NOT NULL CHECK (kind IN ('checkpoint','policy')),"
    " algorithm_revision TEXT NOT NULL,"
    " info_key_revision INTEGER NOT NULL CHECK (info_key_revision = 2),"
    " numeric_profile TEXT NOT NULL,"
    " validation_state TEXT NOT NULL CHECK (validation_state IN"
    " ('unvalidated','validated','rejected')),"
    " engine_revision TEXT NOT NULL,"
    " completed_iterations INTEGER NOT NULL CHECK (completed_iterations >= 0),"
    " prng_identifier TEXT NOT NULL,"
    " seed_hex TEXT NOT NULL CHECK (length(seed_hex) = 16),"
    " prng_state_hex TEXT NOT NULL CHECK (length(prng_state_hex) = 16),"
    " run_status TEXT NOT NULL CHECK (run_status IN ('complete','resource_limit')),"
    " nodes INTEGER NOT NULL CHECK (nodes >= 0),"
    " information_sets INTEGER NOT NULL CHECK (information_sets >= 0),"
    " accounted_bytes INTEGER NOT NULL CHECK (accounted_bytes >= 0),"
    " abstraction_name TEXT NOT NULL,"
    " abstraction_version INTEGER NOT NULL CHECK (abstraction_version >= 0),"
    " abstraction_parameters TEXT NOT NULL,"
    " abstraction_digest TEXT NOT NULL CHECK (length(abstraction_digest) = 16),"
    " terminal_depth INTEGER NOT NULL CHECK (terminal_depth IN (0,1))"
    ") STRICT;",

    "CREATE TABLE game ("
    " id INTEGER PRIMARY KEY CHECK (id = 1),"
    " rules_id TEXT NOT NULL,"
    " player_count INTEGER NOT NULL CHECK (player_count BETWEEN 2 AND 10),"
    " button INTEGER NOT NULL CHECK (button BETWEEN 0 AND 9),"
    " big_blind INTEGER NOT NULL CHECK (big_blind > 0 AND"
    " big_blind <= 9007199254740991),"
    " ante INTEGER NOT NULL CHECK (ante >= 0 AND ante <= 9007199254740991),"
    " root_street INTEGER NOT NULL CHECK (root_street BETWEEN 0 AND 3),"
    " terminal_depth INTEGER NOT NULL CHECK (terminal_depth IN (0,1)),"
    " flop0 INTEGER NOT NULL CHECK (flop0 BETWEEN 0 AND 51),"
    " flop1 INTEGER NOT NULL CHECK (flop1 BETWEEN 0 AND 51),"
    " flop2 INTEGER NOT NULL CHECK (flop2 BETWEEN 0 AND 51),"
    " fixed_turn INTEGER CHECK (fixed_turn IS NULL OR fixed_turn BETWEEN 0 AND 51),"
    " fixed_river INTEGER CHECK (fixed_river IS NULL OR fixed_river BETWEEN 0 AND 51),"
    " pot INTEGER NOT NULL CHECK (pot >= 0 AND pot <= 9007199254740991),"
    " utility_id TEXT NOT NULL"
    ") STRICT;",

    "CREATE TABLE game_seats ("
    " seat INTEGER PRIMARY KEY CHECK (seat BETWEEN 0 AND 9),"
    " stack INTEGER NOT NULL CHECK (stack BETWEEN 0 AND 9007199254740991),"
    " contribution INTEGER NOT NULL CHECK (contribution BETWEEN 0 AND"
    " 9007199254740991)"
    ") STRICT;",

    "CREATE TABLE sizes ("
    " street INTEGER NOT NULL CHECK (street BETWEEN 0 AND 3),"
    " kind INTEGER NOT NULL CHECK (kind IN (0,1)),"
    " ordinal INTEGER NOT NULL CHECK (ordinal >= 0 AND ordinal < 32),"
    " numerator INTEGER NOT NULL CHECK (numerator > 0 AND"
    " numerator <= 9007199254740991),"
    " denominator INTEGER NOT NULL CHECK (denominator > 0 AND"
    " denominator <= 9007199254740991),"
    " PRIMARY KEY (street, kind, ordinal)"
    ") STRICT;",

    "CREATE TABLE ranges ("
    " player INTEGER NOT NULL CHECK (player BETWEEN 0 AND 9),"
    " combo INTEGER NOT NULL CHECK (combo BETWEEN 0 AND 1325),"
    " weight REAL NOT NULL CHECK (weight >= 0 AND weight = weight),"
    " PRIMARY KEY (player, combo)"
    ") STRICT;",

    "CREATE TABLE information_states ("
    " id INTEGER PRIMARY KEY CHECK (id >= 0),"
    " player INTEGER NOT NULL CHECK (player BETWEEN 0 AND 9),"
    " card0 INTEGER NOT NULL CHECK (card0 BETWEEN 0 AND 51),"
    " card1 INTEGER NOT NULL CHECK (card1 BETWEEN 0 AND 51 AND card1 > card0),"
    " public_key TEXT NOT NULL,"
    " UNIQUE (player, card0, card1, public_key)"
    ") STRICT;",

    "CREATE TABLE actions ("
    " info_id INTEGER NOT NULL REFERENCES information_states(id) ON DELETE CASCADE,"
    " ordinal INTEGER NOT NULL CHECK (ordinal >= 0 AND ordinal < 32),"
    " kind INTEGER NOT NULL CHECK (kind BETWEEN 0 AND 4),"
    " target INTEGER CHECK (target IS NULL OR (target >= 0 AND"
    " target <= 9007199254740991)),"
    " probability REAL NOT NULL CHECK (probability >= 0 AND probability <= 1),"
    " PRIMARY KEY (info_id, ordinal),"
    " CHECK ((kind IN (3,4)) = (target IS NOT NULL))"
    ") STRICT;",

    "CREATE TABLE training ("
    " info_id INTEGER NOT NULL,"
    " ordinal INTEGER NOT NULL CHECK (ordinal >= 0 AND ordinal < 32),"
    " regret REAL NOT NULL,"
    " average_weight REAL NOT NULL CHECK (average_weight >= 0 AND"
    " average_weight = average_weight),"
    " PRIMARY KEY (info_id, ordinal),"
    " FOREIGN KEY (info_id, ordinal) REFERENCES actions (info_id, ordinal)"
    " ON DELETE CASCADE"
    ") STRICT;",

    "CREATE TABLE bounds ("
    " id INTEGER PRIMARY KEY CHECK (id >= 0),"
    " public_root TEXT NOT NULL,"
    " seat INTEGER NOT NULL CHECK (seat BETWEEN 0 AND 9),"
    " combo INTEGER NOT NULL CHECK (combo BETWEEN 0 AND 1325),"
    " baseline_cf_mass REAL CHECK (baseline_cf_mass IS NULL OR"
    " (baseline_cf_mass >= 0 AND baseline_cf_mass = baseline_cf_mass)),"
    " normalized_bound REAL,"
    " certification_method TEXT,"
    " measurements_json TEXT,"
    " UNIQUE (public_root, seat, combo)"
    ") STRICT;",

    "CREATE TABLE measurements ("
    " id INTEGER PRIMARY KEY CHECK (id >= 0),"
    " fixture TEXT NOT NULL,"
    " seed_hex TEXT CHECK (seed_hex IS NULL OR length(seed_hex) = 16),"
    " iterations INTEGER CHECK (iterations IS NULL OR iterations >= 0),"
    " metric TEXT NOT NULL,"
    " metric_class TEXT NOT NULL CHECK (metric_class IN ('exact','estimated')),"
    " value REAL NOT NULL CHECK (value = value AND abs(value) < 1e308),"
    " UNIQUE (fixture, seed_hex, iterations, metric)"
    ") STRICT;",
};

void open_writer(dt::Db& db, const std::string& path, bool create) {
  int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_NOMUTEX;
  if (create)
    flags |= SQLITE_OPEN_CREATE;
  db.open(path, flags, "open checkpoint for writing");
#ifndef SQLITE_OMIT_LOAD_EXTENSION
  sqlite3_db_config(db.get(), SQLITE_DBCONFIG_ENABLE_LOAD_EXTENSION, 0, nullptr);
#endif
  dt::apply_writer_pragmas(db);
  dt::verify_writer_pragmas(db);
}

void configure_reader(dt::Db& db) {
#ifndef SQLITE_OMIT_LOAD_EXTENSION
  sqlite3_db_config(db.get(), SQLITE_DBCONFIG_ENABLE_LOAD_EXTENSION, 0, nullptr);
#endif
  sqlite3_db_config(db.get(), SQLITE_DBCONFIG_DEFENSIVE, 1, nullptr);
  db.exec(kReaderPragmas, "reader pragmas");
}

// Percent-encodes every byte of an absolute path that is not an unreserved
// RFC 3986 character and not '/', for use as a file: URI.
std::string percent_encode_path(const std::filesystem::path& path) {
  const std::string raw = std::filesystem::absolute(path).string();
  std::string encoded = "file:";
  static const char kHex[] = "0123456789ABCDEF";
  for (unsigned char byte : raw) {
    const bool unreserved = (byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z') ||
                            (byte >= '0' && byte <= '9') || byte == '-' || byte == '_' ||
                            byte == '.' || byte == '~';
    if (unreserved || byte == '/') {
      encoded.push_back(static_cast<char>(byte));
    } else {
      encoded.push_back('%');
      encoded.push_back(kHex[byte >> 4]);
      encoded.push_back(kHex[byte & 0x0F]);
    }
  }
  encoded += "?immutable=1";
  return encoded;
}

}  // namespace

namespace detail {

void apply_writer_pragmas(Db& db) {
  db.exec(kWriterPragmas, "writer pragmas");
}

void verify_writer_pragmas(Db& db) {
  // These pragmas are per-connection: they never land in the file and cannot
  // be observed through a second connection, so the check must happen on the
  // writer connection itself. journal_mode=DELETE is persistent in the header
  // and reads back as "delete"; synchronous=FULL reads back as 2.
  const std::int64_t synchronous = db.pragma_i64("synchronous", "verify synchronous");
  if (synchronous != 2)
    throw ArtifactError(ArtifactErrorKind::Sqlite,
                        "checkpoint writer could not enforce synchronous=FULL (read back " +
                            std::to_string(synchronous) + ")");
  const std::string journal_mode = db.pragma_text("journal_mode", "verify journal_mode");
  if (journal_mode != "delete")
    throw ArtifactError(ArtifactErrorKind::Sqlite,
                        "checkpoint writer could not enforce rollback journaling (read back '" +
                            journal_mode + "')");
}

void open_immutable_reader(Db& db, const std::filesystem::path& path) {
  const std::string uri = percent_encode_path(path);
  db.open(uri, SQLITE_OPEN_READONLY | SQLITE_OPEN_URI | SQLITE_OPEN_NOMUTEX,
          "open artifact immutable read-only");
  configure_reader(db);
}

}  // namespace detail

namespace {

void write_schema(dt::Db& db) {
  std::string ddl;
  for (const char* statement : kSchemaDdlV1) {
    ddl += statement;
    ddl.push_back('\n');
  }
  db.exec(ddl, "create schema");
}

void insert_manifest(dt::Db& db, const ArtifactManifest& manifest) {
  dt::Stmt insert(db.get(),
                  "INSERT INTO manifest (id, kind, algorithm_revision, info_key_revision,"
                  " numeric_profile, validation_state, engine_revision, completed_iterations,"
                  " prng_identifier, seed_hex, prng_state_hex, run_status, nodes,"
                  " information_sets, accounted_bytes) VALUES (1, ?, ?, ?, ?, ?, ?, ?, ?, ?,"
                  " ?, ?, ?, ?, ?)",
                  "insert manifest");
  int i = 1;
  insert.bind_text(i++, kind_text(manifest.kind));
  insert.bind_text(i++, manifest.algorithm_revision);
  insert.bind_i64(i++, static_cast<std::int64_t>(manifest.information_key_revision));
  insert.bind_text(i++, manifest.numeric_profile);
  insert.bind_text(i++, validation_text(manifest.validation));
  insert.bind_text(i++, manifest.engine_revision);
  insert.bind_u64_chips(i++, manifest.completed_iterations);
  insert.bind_text(i++, manifest.prng_identifier);
  insert.bind_text(i++, dt::u64_to_hex(manifest.seed));
  insert.bind_text(i++, dt::u64_to_hex(manifest.prng_state));
  insert.bind_text(i++, run_status_text(manifest.run_status));
  insert.bind_u64_chips(i++, manifest.nodes);
  insert.bind_u64_chips(i++, manifest.information_sets);
  insert.bind_u64_chips(i++, manifest.accounted_bytes);
  (void)insert.step("insert manifest");
}

void insert_game(dt::Db& db, const HeadsUpGame& game) {
  const HeadsUpRoot& root = game.root;
  dt::Stmt insert(db.get(),
                  "INSERT INTO game (id, rules_id, button, big_blind, root_street, flop0,"
                  " flop1, flop2, fixed_turn, fixed_river, stack0, stack1, contribution0,"
                  " contribution1, pot, utility_id) VALUES (1, ?, ?, ?, 0, ?, ?, ?, ?, ?,"
                  " ?, ?, ?, ?, ?, ?)",
                  "insert game");
  int i = 1;
  insert.bind_text(i++, kRulesIdentifierV1);
  insert.bind_i64(i++, static_cast<std::int64_t>(root.button));
  insert.bind_u64_chips(i++, root.big_blind);
  insert.bind_i64(i++, root.flop[0]);
  insert.bind_i64(i++, root.flop[1]);
  insert.bind_i64(i++, root.flop[2]);
  if (game.fixed_runout[0])
    insert.bind_i64(i++, *game.fixed_runout[0]);
  else
    insert.bind_null(i++);
  if (game.fixed_runout[1])
    insert.bind_i64(i++, *game.fixed_runout[1]);
  else
    insert.bind_null(i++);
  insert.bind_u64_chips(i++, root.stacks[0]);
  insert.bind_u64_chips(i++, root.stacks[1]);
  insert.bind_u64_chips(i++, root.contributions[0]);
  insert.bind_u64_chips(i++, root.contributions[1]);
  insert.bind_u64_chips(i++, root.pot);
  insert.bind_text(i++, kUtilityIdentifierV1);
  (void)insert.step("insert game");
}

void insert_sizes(dt::Db& db, const HeadsUpGame& game) {
  dt::Stmt insert(db.get(),
                  "INSERT INTO sizes (street, kind, ordinal, numerator, denominator)"
                  " VALUES (?, ?, ?, ?, ?)",
                  "insert size");
  for (int street = 0; street < 3; ++street) {
    for (int kind = 0; kind < 2; ++kind) {
      const auto& fractions = kind == 0 ? game.sizes[street].bets : game.sizes[street].raises;
      for (std::size_t ordinal = 0; ordinal < fractions.size(); ++ordinal) {
        insert.reset_and_clear();
        int i = 1;
        insert.bind_i64(i++, street);
        insert.bind_i64(i++, kind);
        insert.bind_i64(i++, static_cast<std::int64_t>(ordinal));
        insert.bind_u64_chips(i++, fractions[ordinal].numerator);
        insert.bind_u64_chips(i++, fractions[ordinal].denominator);
        (void)insert.step("insert size");
      }
    }
  }
}

void insert_ranges(dt::Db& db, const HeadsUpGame& game) {
  dt::Stmt insert(db.get(), "INSERT INTO ranges (player, combo, weight) VALUES (?, ?, ?)",
                  "insert range");
  for (int player = 0; player < 2; ++player)
    for (const WeightedHand& hand : game.ranges[player]) {
      insert.reset_and_clear();
      insert.bind_i64(1, player);
      insert.bind_i64(2, dt::combo_id(hand.cards));
      insert.bind_double(3, hand.weight);
      (void)insert.step("insert range");
    }
}

// Inserts information states, actions, and (optionally) raw training rows.
// State ids are assigned in ascending InformationKey order.
void insert_policy(dt::Db& db, const TrainingResult& result, const TrainingRows& rows,
                   bool include_training) {
  dt::Stmt state_insert(db.get(),
                        "INSERT INTO information_states (id, player, card0, card1,"
                        " public_key) VALUES (?, ?, ?, ?, ?)",
                        "insert information state");
  dt::Stmt action_insert(db.get(),
                         "INSERT INTO actions (info_id, ordinal, kind, target, probability) VALUES"
                         " (?, ?, ?, ?, ?)",
                         "insert action");
  dt::Stmt training_insert(db.get(),
                           "INSERT INTO training (info_id, ordinal, regret,"
                           " average_weight) VALUES (?, ?, ?, ?)",
                           "insert training");
  std::int64_t state_id = 0;
  for (const auto& [key, row] : result.policy.rows()) {
    state_insert.reset_and_clear();
    int i = 1;
    state_insert.bind_i64(i++, state_id);
    state_insert.bind_i64(i++, static_cast<std::int64_t>(key[0]));
    state_insert.bind_i64(i++, static_cast<std::int64_t>(key[1]));
    state_insert.bind_i64(i++, static_cast<std::int64_t>(key[2]));
    state_insert.bind_text(i++, encode_public_key(key, kArtifactSchemaVersion));
    (void)state_insert.step("insert information state");

    for (std::int64_t ordinal = 0; ordinal < static_cast<std::int64_t>(row.actions.size());
         ++ordinal) {
      const Action& action = row.actions[static_cast<std::size_t>(ordinal)];
      action_insert.reset_and_clear();
      int a = 1;
      action_insert.bind_i64(a++, state_id);
      action_insert.bind_i64(a++, ordinal);
      action_insert.bind_i64(a++, dt::action_kind_id(action.type));
      if (action.type == ActionType::Bet || action.type == ActionType::Raise)
        action_insert.bind_u64_chips(a++, action.target_total);
      else
        action_insert.bind_null(a++);
      action_insert.bind_double(a++, row.probabilities[static_cast<std::size_t>(ordinal)]);
      (void)action_insert.step("insert action");
    }

    if (include_training) {
      const auto found = rows.find(key);
      if (found == rows.end())
        fail(ArtifactErrorKind::InvalidArgument, "missing raw row for policy state");
      const TrainingRow& raw = found->second;
      for (std::int64_t ordinal = 0; ordinal < static_cast<std::int64_t>(raw.regrets.size());
           ++ordinal) {
        training_insert.reset_and_clear();
        int t = 1;
        training_insert.bind_i64(t++, state_id);
        training_insert.bind_i64(t++, ordinal);
        training_insert.bind_double(t++, raw.regrets[static_cast<std::size_t>(ordinal)]);
        training_insert.bind_double(t++, raw.average_weights[static_cast<std::size_t>(ordinal)]);
        (void)training_insert.step("insert training");
      }
    }
    ++state_id;
  }
}

void write_all(dt::Db& db, const ArtifactManifest& manifest, const TrainingResult& result,
               const TrainingRows& rows, bool include_training) {
  db.exec("BEGIN IMMEDIATE;", "begin write");
  bool rollback_best_effort = true;
  try {
    db.exec("PRAGMA application_id = 1112754004;", "set application id");
    db.exec("PRAGMA user_version = 1;", "set schema version");
    write_schema(db);
    insert_manifest(db, manifest);
    insert_game(db, result.policy.game());
    insert_sizes(db, result.policy.game());
    insert_ranges(db, result.policy.game());
    insert_policy(db, result, rows, include_training);
    db.exec("COMMIT;", "commit write");
    rollback_best_effort = false;
  } catch (...) {
    // A disk I/O error (e.g. SQLITE_FULL mid-transaction) makes SQLite roll
    // the transaction back automatically; a manual ROLLBACK can then report
    // no active transaction or fail itself, so it is deliberately best
    // effort and must not mask the original failure.
    if (rollback_best_effort) {
      try {
        db.exec("ROLLBACK;", "rollback failed write");
      } catch (...) {
      }
    }
    throw;
  }
}

// --- v2 (RFC 0009 D3) writer: seat-generic ---

void write_schema_v2(dt::Db& db) {
  std::string ddl;
  for (const char* statement : kSchemaDdlV2) {
    ddl += statement;
    ddl.push_back('\n');
  }
  db.exec(ddl, "create schema");
}

// Validates the GameDef identity the v2 writer persists. The v2 profile stores
// rooted flop/turn/river games only: a rooted board carries 3..5 cards, no
// blinds are posted at the root (the GameState constructor seats the declared
// stacks/contributions verbatim, so the stack/contribution round-trip is the
// identity), and no ante is declared until an ante profile ships.
void validate_game_v2(const GameDef& game) {
  check(game.player_count >= 2 && game.player_count <= 10, ArtifactErrorKind::InvalidArgument,
        "player_count must be in 2..10");
  check(game.button < game.player_count, ArtifactErrorKind::InvalidArgument,
        "button outside seats");
  check(game.big_blind > 0 && game.big_blind <= kMaxStoredChips, ArtifactErrorKind::InvalidArgument,
        "big blind outside the numeric profile");
  check(game.ante == 0, ArtifactErrorKind::InvalidArgument,
        "nonzero ante is not supported by the v2 profile");
  check(game.board_size == 3 || game.board_size == 4 || game.board_size == 5,
        ArtifactErrorKind::InvalidArgument,
        "v2 writer supports rooted flop/turn/river games only (board_size 3..5)");
  check(!game.preflop, ArtifactErrorKind::InvalidArgument,
        "v2 writer does not support preflop games");
  check(game.variant == RulesVariant::NoLimitHoldem, ArtifactErrorKind::InvalidArgument,
        "unsupported rules variant");
  std::array<bool, 52> used{};
  for (std::size_t i = 0; i < game.board_size; ++i) {
    const int card = game.board[i];
    check(card >= 0 && card < 52 && !used[card], ArtifactErrorKind::InvalidArgument,
          "invalid or duplicate board card");
    used[card] = true;
  }
  check(game.pot <= kMaxStoredChips, ArtifactErrorKind::InvalidArgument,
        "pot outside the 2^53-1 profile");
  for (std::size_t seat = 0; seat < game.player_count; ++seat) {
    check(game.stacks[seat] <= kMaxStoredChips, ArtifactErrorKind::InvalidArgument,
          "stack outside the 2^53-1 profile");
    check(game.contributions[seat] <= kMaxStoredChips, ArtifactErrorKind::InvalidArgument,
          "contribution outside the 2^53-1 profile");
    check(game.blinds_posted[seat] == 0, ArtifactErrorKind::InvalidArgument,
          "rooted board games must not post blinds at the root");
  }
}

// Validates the seat-generic result the v2 writer persists. Mirrors
// validate_content's policy-row checks; there are no raw training rows because
// SeatPolicyRow carries only actions, probabilities, and visits (visits are not
// persisted -- the actions table is byte-identical to v1).
void validate_content_v2(const SeatTrainingResult& result) {
  const GameDef& game = result.policy.game();
  validate_game_v2(game);
  // The result's identity fields must agree with the policy's.
  check(result.action_id == result.policy.action_id(), ArtifactErrorKind::InvalidArgument,
        "result and policy disagree on action abstraction");
  check(result.terminal_depth == game.terminal, ArtifactErrorKind::InvalidArgument,
        "result and game disagree on terminal depth");
  for (const auto& street : result.policy.sizes())
    for (const auto* fractions : {&street.bets, &street.raises})
      for (const solver::Fraction& fraction : *fractions)
        check(fraction.numerator > 0 && fraction.numerator <= kMaxStoredChips &&
                  fraction.denominator > 0 && fraction.denominator <= kMaxStoredChips,
              ArtifactErrorKind::InvalidArgument, "size fraction outside the numeric profile");
  const auto& ranges = result.policy.ranges();
  check(ranges.size() == game.player_count, ArtifactErrorKind::InvalidArgument,
        "range count must equal player count");
  std::array<bool, 52> board_used{};
  for (std::size_t i = 0; i < game.board_size; ++i)
    board_used[game.board[i]] = true;
  for (std::size_t player = 0; player < game.player_count; ++player) {
    check(!ranges[player].empty() && ranges[player].size() <= 1326,
          ArtifactErrorKind::InvalidArgument, "range must contain 1..1326 combos");
    for (const WeightedHand& hand : ranges[player]) {
      check(hand.cards[0] >= 0 && hand.cards[1] < 52 && hand.cards[0] < hand.cards[1],
            ArtifactErrorKind::InvalidArgument, "range combo cards invalid or unsorted");
      check(!board_used[hand.cards[0]] && !board_used[hand.cards[1]],
            ArtifactErrorKind::InvalidArgument, "range combo shares a public card");
      check(std::isfinite(hand.weight) && hand.weight >= 0, ArtifactErrorKind::InvalidArgument,
            "range weight must be finite and nonnegative");
    }
  }
  for (const auto& [key, row] : result.policy.rows()) {
    check(key.size() >= 4, ArtifactErrorKind::InvalidArgument, "information key too short");
    dt::validate_probabilities(row.probabilities, "policy probabilities");
    check(row.actions.size() == row.probabilities.size(), ArtifactErrorKind::InvalidArgument,
          "policy action/probability length mismatch");
    // Every key must have a canonical revision-2 encoding.
    (void)encode_public_key(key, kArtifactSchemaVersionV2);
  }
}

ArtifactManifest manifest_for_v2(const SeatTrainingResult& result,
                                 const SeatCheckpointProvenance& provenance, ArtifactKind kind,
                                 ValidationState validation) {
  check(!provenance.algorithm_revision.empty() && !provenance.engine_revision.empty(),
        ArtifactErrorKind::InvalidArgument, "manifest provenance strings must be non-empty");
  check(provenance.prng_identifier == kSampledPrngIdentifier ||
            provenance.prng_identifier == kFullTraversalPrngIdentifier,
        ArtifactErrorKind::InvalidArgument, "unsupported PRNG identifier");
  ArtifactManifest manifest;
  manifest.kind = kind;
  manifest.algorithm_revision = provenance.algorithm_revision;
  manifest.information_key_revision = kArtifactSchemaVersionV2;
  manifest.numeric_profile = kNumericProfileV2;
  manifest.engine_revision = provenance.engine_revision;
  manifest.validation = validation;
  manifest.completed_iterations = result.completed_iterations;
  manifest.prng_identifier = provenance.prng_identifier;
  manifest.seed = result.seed;
  manifest.prng_state = result.prng_state;
  manifest.run_status = result.termination == NSeatTerminationPhase::Complete
                            ? RunStatus::Complete
                            : RunStatus::ResourceLimit;
  manifest.nodes = result.nodes;
  // The manifest records the concrete stored-state count, not the trainer's
  // bucket-keyed information-set count (the export expands one bucket row into
  // many concrete combos).
  manifest.information_sets = result.policy.rows().size();
  manifest.accounted_bytes = result.accounted_bytes;
  manifest.abstraction_name = result.action_id.name;
  manifest.abstraction_version = result.action_id.version;
  manifest.abstraction_parameters = result.action_id.parameters;
  manifest.abstraction_digest = result.action_id.digest;
  manifest.terminal_depth = result.terminal_depth;
  return manifest;
}

void insert_manifest_v2(dt::Db& db, const ArtifactManifest& manifest) {
  dt::Stmt insert(db.get(),
                  "INSERT INTO manifest (id, kind, algorithm_revision, info_key_revision,"
                  " numeric_profile, validation_state, engine_revision, completed_iterations,"
                  " prng_identifier, seed_hex, prng_state_hex, run_status, nodes,"
                  " information_sets, accounted_bytes, abstraction_name, abstraction_version,"
                  " abstraction_parameters, abstraction_digest, terminal_depth) VALUES (1, ?,"
                  " ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
                  "insert manifest");
  int i = 1;
  insert.bind_text(i++, kind_text(manifest.kind));
  insert.bind_text(i++, manifest.algorithm_revision);
  insert.bind_i64(i++, static_cast<std::int64_t>(manifest.information_key_revision));
  insert.bind_text(i++, manifest.numeric_profile);
  insert.bind_text(i++, validation_text(manifest.validation));
  insert.bind_text(i++, manifest.engine_revision);
  insert.bind_u64_chips(i++, manifest.completed_iterations);
  insert.bind_text(i++, manifest.prng_identifier);
  insert.bind_text(i++, dt::u64_to_hex(manifest.seed));
  insert.bind_text(i++, dt::u64_to_hex(manifest.prng_state));
  insert.bind_text(i++, run_status_text(manifest.run_status));
  insert.bind_u64_chips(i++, manifest.nodes);
  insert.bind_u64_chips(i++, manifest.information_sets);
  insert.bind_u64_chips(i++, manifest.accounted_bytes);
  insert.bind_text(i++, manifest.abstraction_name);
  insert.bind_i64(i++, static_cast<std::int64_t>(manifest.abstraction_version));
  insert.bind_text(i++, manifest.abstraction_parameters);
  insert.bind_text(i++, dt::u64_to_hex(manifest.abstraction_digest));
  insert.bind_i64(i++, static_cast<std::int64_t>(manifest.terminal_depth));
  (void)insert.step("insert manifest");
}

void insert_game_v2(dt::Db& db, const GameDef& game) {
  dt::Stmt insert(db.get(),
                  "INSERT INTO game (id, rules_id, player_count, button, big_blind, ante,"
                  " root_street, terminal_depth, flop0, flop1, flop2, fixed_turn, fixed_river,"
                  " pot, utility_id) VALUES (1, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
                  "insert game");
  int i = 1;
  insert.bind_text(i++, kRulesIdentifierV2);
  insert.bind_i64(i++, static_cast<std::int64_t>(game.player_count));
  insert.bind_i64(i++, static_cast<std::int64_t>(game.button));
  insert.bind_u64_chips(i++, game.big_blind);
  insert.bind_u64_chips(i++, game.ante);
  // board_size 3..5 maps to root_street 0..2 (Flop..River).
  insert.bind_i64(i++, static_cast<std::int64_t>(game.board_size) - 3);
  insert.bind_i64(i++, static_cast<std::int64_t>(game.terminal));
  insert.bind_i64(i++, game.board[0]);
  insert.bind_i64(i++, game.board[1]);
  insert.bind_i64(i++, game.board[2]);
  if (game.board_size >= 4)
    insert.bind_i64(i++, game.board[3]);
  else
    insert.bind_null(i++);
  if (game.board_size >= 5)
    insert.bind_i64(i++, game.board[4]);
  else
    insert.bind_null(i++);
  insert.bind_u64_chips(i++, game.pot);
  insert.bind_text(i++, kUtilityIdentifierV1);
  (void)insert.step("insert game");
}

void insert_game_seats(dt::Db& db, const GameDef& game) {
  dt::Stmt insert(db.get(), "INSERT INTO game_seats (seat, stack, contribution) VALUES (?, ?, ?)",
                  "insert game seat");
  for (std::size_t seat = 0; seat < game.player_count; ++seat) {
    insert.reset_and_clear();
    insert.bind_i64(1, static_cast<std::int64_t>(seat));
    insert.bind_u64_chips(2, game.stacks[seat]);
    insert.bind_u64_chips(3, game.contributions[seat]);
    (void)insert.step("insert game seat");
  }
}

void insert_sizes_v2(dt::Db& db, const SizeSchedule& sizes) {
  dt::Stmt insert(db.get(),
                  "INSERT INTO sizes (street, kind, ordinal, numerator, denominator)"
                  " VALUES (?, ?, ?, ?, ?)",
                  "insert size");
  for (int street = 0; street < 4; ++street) {
    for (int kind = 0; kind < 2; ++kind) {
      const auto& fractions = kind == 0 ? sizes[street].bets : sizes[street].raises;
      for (std::size_t ordinal = 0; ordinal < fractions.size(); ++ordinal) {
        insert.reset_and_clear();
        int i = 1;
        insert.bind_i64(i++, street);
        insert.bind_i64(i++, kind);
        insert.bind_i64(i++, static_cast<std::int64_t>(ordinal));
        insert.bind_u64_chips(i++, fractions[ordinal].numerator);
        insert.bind_u64_chips(i++, fractions[ordinal].denominator);
        (void)insert.step("insert size");
      }
    }
  }
}

void insert_ranges_v2(dt::Db& db, const std::vector<std::vector<WeightedHand>>& ranges,
                      std::size_t player_count) {
  dt::Stmt insert(db.get(), "INSERT INTO ranges (player, combo, weight) VALUES (?, ?, ?)",
                  "insert range");
  for (std::size_t player = 0; player < player_count; ++player)
    for (const WeightedHand& hand : ranges[player]) {
      insert.reset_and_clear();
      insert.bind_i64(1, static_cast<std::int64_t>(player));
      insert.bind_i64(2, dt::combo_id(hand.cards));
      insert.bind_double(3, hand.weight);
      (void)insert.step("insert range");
    }
}

// Inserts information states and actions. State ids are assigned in ascending
// InformationKey order. There are no training rows: SeatPolicyRow carries no
// regret/average-weight state.
void insert_policy_v2(dt::Db& db, const SeatTrainingResult& result) {
  dt::Stmt state_insert(db.get(),
                        "INSERT INTO information_states (id, player, card0, card1,"
                        " public_key) VALUES (?, ?, ?, ?, ?)",
                        "insert information state");
  dt::Stmt action_insert(db.get(),
                         "INSERT INTO actions (info_id, ordinal, kind, target, probability) VALUES"
                         " (?, ?, ?, ?, ?)",
                         "insert action");
  std::int64_t state_id = 0;
  for (const auto& [key, row] : result.policy.rows()) {
    state_insert.reset_and_clear();
    int i = 1;
    state_insert.bind_i64(i++, state_id);
    state_insert.bind_i64(i++, static_cast<std::int64_t>(key[0]));
    state_insert.bind_i64(i++, static_cast<std::int64_t>(key[1]));
    state_insert.bind_i64(i++, static_cast<std::int64_t>(key[2]));
    state_insert.bind_text(i++, encode_public_key(key, kArtifactSchemaVersionV2));
    (void)state_insert.step("insert information state");

    for (std::int64_t ordinal = 0; ordinal < static_cast<std::int64_t>(row.actions.size());
         ++ordinal) {
      const Action& action = row.actions[static_cast<std::size_t>(ordinal)];
      action_insert.reset_and_clear();
      int a = 1;
      action_insert.bind_i64(a++, state_id);
      action_insert.bind_i64(a++, ordinal);
      action_insert.bind_i64(a++, dt::action_kind_id(action.type));
      if (action.type == ActionType::Bet || action.type == ActionType::Raise)
        action_insert.bind_u64_chips(a++, action.target_total);
      else
        action_insert.bind_null(a++);
      action_insert.bind_double(a++, row.probabilities[static_cast<std::size_t>(ordinal)]);
      (void)action_insert.step("insert action");
    }
    ++state_id;
  }
}

void write_all_v2(dt::Db& db, const ArtifactManifest& manifest, const SeatTrainingResult& result) {
  db.exec("BEGIN IMMEDIATE;", "begin write");
  bool rollback_best_effort = true;
  try {
    db.exec("PRAGMA application_id = 1112754004;", "set application id");
    db.exec("PRAGMA user_version = 2;", "set schema version");
    write_schema_v2(db);
    insert_manifest_v2(db, manifest);
    insert_game_v2(db, result.policy.game());
    insert_game_seats(db, result.policy.game());
    insert_sizes_v2(db, result.policy.sizes());
    insert_ranges_v2(db, result.policy.ranges(), result.policy.game().player_count);
    insert_policy_v2(db, result);
    db.exec("COMMIT;", "commit write");
    rollback_best_effort = false;
  } catch (...) {
    // See write_all: an I/O failure may already have auto-rolled the
    // transaction back; never mask the original error with a failed ROLLBACK.
    if (rollback_best_effort) {
      try {
        db.exec("ROLLBACK;", "rollback failed write");
      } catch (...) {
      }
    }
    throw;
  }
}

// --- reader structures ---

struct StoredState {
  std::int64_t id = 0;
  int player = 0;
  int card0 = 0;
  int card1 = 0;
  std::string public_key;
};

struct StoredAction {
  std::int64_t info_id = 0;
  int ordinal = 0;
  int kind = 0;
  std::optional<Chips> target;
  double probability = 0;
};

struct StoredTraining {
  std::int64_t info_id = 0;
  int ordinal = 0;
  double regret = 0;
  double average_weight = 0;
};

std::int64_t read_bounded_i64(dt::Stmt& stmt, int column, const std::string& field) {
  const std::int64_t value = stmt.column_i64(column);
  if (value < 0 || static_cast<std::uint64_t>(value) > kMaxStoredChips)
    fail(ArtifactErrorKind::InvalidValue, field + ": integer outside 0..2^53-1");
  return value;
}

HeadsUpGame read_game(dt::Db& db) {
  dt::Stmt query(db.get(),
                 "SELECT rules_id, button, big_blind, root_street, flop0, flop1,"
                 " flop2, fixed_turn, fixed_river, stack0, stack1,"
                 " contribution0, contribution1, pot, utility_id FROM game",
                 "read game");
  if (!query.step("read game"))
    fail(ArtifactErrorKind::InvalidSchema, "game table is empty");
  HeadsUpGame game;
  int i = 0;
  const std::string_view rules = query.column_text(i++);
  check(rules == kRulesIdentifierV1, ArtifactErrorKind::UnsupportedVersion,
        "unsupported rules identifier: " + std::string(rules));
  const std::int64_t button_raw = query.column_i64(i++);
  check(button_raw == 0 || button_raw == 1, ArtifactErrorKind::InvalidSchema,
        "game.button outside {0,1}");
  game.root.button = static_cast<std::size_t>(button_raw);
  game.root.big_blind = static_cast<Chips>(read_bounded_i64(query, i++, "big_blind"));
  const std::int64_t root_street = query.column_i64(i++);
  check(root_street == 0, ArtifactErrorKind::UnsupportedVersion, "unsupported root street");
  for (int flop_index = 0; flop_index < 3; ++flop_index) {
    const std::int64_t flop_card = query.column_i64(i++);
    check(flop_card >= 0 && flop_card < 52, ArtifactErrorKind::InvalidValue,
          "game flop card outside 0..51");
    game.root.flop[flop_index] = static_cast<int>(flop_card);
  }
  for (int runout_index = 0; runout_index < 2; ++runout_index) {
    if (query.column_null(i)) {
      game.fixed_runout[runout_index] = std::optional<int>{};
    } else {
      const std::int64_t runout_card = query.column_i64(i);
      check(runout_card >= 0 && runout_card < 52, ArtifactErrorKind::InvalidValue,
            "fixed runout card outside 0..51");
      game.fixed_runout[runout_index] = static_cast<int>(runout_card);
    }
    ++i;
  }
  game.root.stacks[0] = static_cast<Chips>(read_bounded_i64(query, i++, "stack0"));
  game.root.stacks[1] = static_cast<Chips>(read_bounded_i64(query, i++, "stack1"));
  game.root.contributions[0] = static_cast<Chips>(read_bounded_i64(query, i++, "contribution0"));
  game.root.contributions[1] = static_cast<Chips>(read_bounded_i64(query, i++, "contribution1"));
  game.root.pot = static_cast<Chips>(read_bounded_i64(query, i++, "pot"));
  const std::string_view utility = query.column_text(i++);
  check(utility == kUtilityIdentifierV1, ArtifactErrorKind::UnsupportedVersion,
        "unsupported utility identifier: " + std::string(utility));
  check(!query.step("read game"), ArtifactErrorKind::InvalidSchema, "game table not singleton");
  // A default HeadsUpGame carries the pinned default schedule; replace it with
  // exactly the ordered fractions stored in the artifact.
  for (auto& street_sizes : game.sizes) {
    street_sizes.bets.clear();
    street_sizes.raises.clear();
  }
  return game;
}

void read_sizes(dt::Db& db, HeadsUpGame& game) {
  dt::Stmt query(db.get(),
                 "SELECT street, kind, ordinal, numerator, denominator FROM sizes"
                 " ORDER BY street, kind, ordinal",
                 "read sizes");
  std::array<std::array<std::size_t, 2>, 3> next_ordinal{};
  while (query.step("read sizes")) {
    // Every value below came from an untrusted file column. The SQL CHECK
    // constraints are write-time-only and can be stripped from a tampered
    // file, so validate the fixed-array indices in C++ before indexing.
    const std::int64_t street_raw = query.column_i64(0);
    const std::int64_t kind_raw = query.column_i64(1);
    check(street_raw >= 0 && street_raw <= 2, ArtifactErrorKind::InvalidSchema,
          "sizes.street outside 0..2");
    const std::int64_t ordinal = query.column_i64(2);
    check(ordinal >= 0 && ordinal < 32, ArtifactErrorKind::InvalidSchema,
          "sizes.ordinal outside 0..31");
    check(kind_raw == 0 || kind_raw == 1, ArtifactErrorKind::InvalidSchema,
          "sizes.kind must be 0 (bet) or 1 (raise)");
    const int street = static_cast<int>(street_raw);
    const int kind = static_cast<int>(kind_raw);
    const Chips numerator = static_cast<Chips>(read_bounded_i64(query, 3, "numerator"));
    const Chips denominator = static_cast<Chips>(read_bounded_i64(query, 4, "denominator"));
    check(numerator > 0 && denominator > 0, ArtifactErrorKind::InvalidValue,
          "non-positive size fraction");
    check(ordinal == static_cast<std::int64_t>(next_ordinal[street][kind]),
          ArtifactErrorKind::InvalidSchema, "non-contiguous size ordinals");
    ++next_ordinal[street][kind];
    auto& street_sizes = game.sizes[street];
    (kind == 0 ? street_sizes.bets : street_sizes.raises).push_back({numerator, denominator});
  }
}

void read_ranges(dt::Db& db, HeadsUpGame& game) {
  dt::Stmt query(db.get(), "SELECT player, combo, weight FROM ranges ORDER BY player, combo",
                 "read ranges");
  int last_player = -1;
  int last_combo = -1;
  std::size_t rows = 0;
  while (query.step("read ranges")) {
    const std::int64_t player_raw = query.column_i64(0);
    const std::int64_t combo_raw = query.column_i64(1);
    // Untrusted column values: validate before indexing fixed-size storage.
    check(player_raw == 0 || player_raw == 1, ArtifactErrorKind::InvalidSchema,
          "ranges.player outside {0,1}");
    check(combo_raw >= 0 && combo_raw <= 1325, ArtifactErrorKind::InvalidSchema,
          "ranges.combo outside 0..1325");
    const int player = static_cast<int>(player_raw);
    const int combo = static_cast<int>(combo_raw);
    const double weight = query.column_double(2);
    dt::require_finite(weight, "range weight");
    check(weight >= 0, ArtifactErrorKind::InvalidValue, "negative range weight");
    if (player != last_player) {
      check(player == last_player + 1, ArtifactErrorKind::InvalidSchema,
            "range player rows out of order");
      last_combo = -1;
      last_player = player;
    }
    check(combo > last_combo, ArtifactErrorKind::InvalidSchema,
          "range combo rows out of order or duplicated");
    last_combo = combo;
    const auto cards = dt::combo_cards(combo);
    game.ranges[player].push_back(WeightedHand{{cards[0], cards[1]}, weight});
    ++rows;
  }
  check(last_player == 1 && rows >= 2, ArtifactErrorKind::InvalidSchema,
        "both players require at least one range combo");
}

ArtifactManifest read_manifest(dt::Db& db) {
  dt::Stmt query(db.get(),
                 "SELECT kind, algorithm_revision, info_key_revision, numeric_profile,"
                 " validation_state, engine_revision, completed_iterations, prng_identifier,"
                 " seed_hex, prng_state_hex, run_status, nodes, information_sets,"
                 " accounted_bytes FROM manifest",
                 "read manifest");
  if (!query.step("read manifest"))
    fail(ArtifactErrorKind::InvalidSchema, "manifest table is empty");
  ArtifactManifest manifest;
  int i = 0;
  manifest.kind = kind_from_text(query.column_text(i++));
  manifest.algorithm_revision = std::string(query.column_text(i++));
  const std::int64_t info_key_revision_raw = query.column_i64(i++);
  check(info_key_revision_raw == static_cast<std::int64_t>(kArtifactSchemaVersion),
        ArtifactErrorKind::UnsupportedVersion, "unsupported information-key revision");
  manifest.information_key_revision = static_cast<std::uint32_t>(info_key_revision_raw);
  manifest.numeric_profile = std::string(query.column_text(i++));
  check(manifest.numeric_profile == kNumericProfileV1, ArtifactErrorKind::UnsupportedVersion,
        "unsupported numeric profile: " + manifest.numeric_profile);
  manifest.validation = validation_from_text(query.column_text(i++));
  manifest.engine_revision = std::string(query.column_text(i++));
  manifest.completed_iterations =
      static_cast<std::uint64_t>(read_bounded_i64(query, i++, "completed_iterations"));
  manifest.prng_identifier = std::string(query.column_text(i++));
  check(manifest.prng_identifier == kSampledPrngIdentifier ||
            manifest.prng_identifier == kFullTraversalPrngIdentifier,
        ArtifactErrorKind::UnsupportedVersion,
        "unsupported PRNG identifier: " + manifest.prng_identifier);
  const std::string_view seed_hex = query.column_text(i++);
  const std::string_view state_hex = query.column_text(i++);
  check(dt::is_hex_word(seed_hex) && dt::is_hex_word(state_hex), ArtifactErrorKind::InvalidSchema,
        "PRNG words must be 16 lowercase hex digits");
  manifest.seed = dt::hex_to_u64(seed_hex);
  manifest.prng_state = dt::hex_to_u64(state_hex);
  manifest.run_status = run_status_from_text(query.column_text(i++));
  manifest.nodes = static_cast<std::size_t>(read_bounded_i64(query, i++, "nodes"));
  manifest.information_sets =
      static_cast<std::size_t>(read_bounded_i64(query, i++, "information_sets"));
  manifest.accounted_bytes =
      static_cast<std::size_t>(read_bounded_i64(query, i++, "accounted_bytes"));
  check(!manifest.algorithm_revision.empty() && !manifest.engine_revision.empty(),
        ArtifactErrorKind::InvalidSchema, "manifest revision strings are empty");
  check(!query.step("read manifest"), ArtifactErrorKind::InvalidSchema,
        "manifest table is not a singleton");
  return manifest;
}

std::map<std::int64_t, StoredState> read_states(dt::Db& db) {
  std::map<std::int64_t, StoredState> states;
  dt::Stmt query(db.get(),
                 "SELECT id, player, card0, card1, public_key FROM information_states"
                 " ORDER BY id",
                 "read information states");
  std::int64_t expected = 0;
  while (query.step("read information states")) {
    StoredState state;
    state.id = query.column_i64(0);
    check(state.id >= 0, ArtifactErrorKind::InvalidSchema, "negative information state id");
    check(state.id == expected, ArtifactErrorKind::InvalidSchema,
          "information state ids must be contiguous from zero");
    ++expected;
    // Validate the raw int64 values BEFORE narrowing to int; otherwise a value
    // such as 2^32 would wrap to 0 and pass the small-domain check.
    const std::int64_t player_raw = query.column_i64(1);
    const std::int64_t card0_raw = query.column_i64(2);
    const std::int64_t card1_raw = query.column_i64(3);
    state.public_key = std::string(query.column_text(4));
    check(player_raw == 0 || player_raw == 1, ArtifactErrorKind::InvalidSchema,
          "information state player outside {0,1}");
    check(card0_raw >= 0 && card0_raw < card1_raw && card1_raw <= 51,
          ArtifactErrorKind::InvalidValue, "information state own cards out of range");
    state.player = static_cast<int>(player_raw);
    state.card0 = static_cast<int>(card0_raw);
    state.card1 = static_cast<int>(card1_raw);
    states.emplace(state.id, std::move(state));
  }
  return states;
}

std::map<std::pair<std::int64_t, int>, StoredAction> read_actions(dt::Db& db) {
  std::map<std::pair<std::int64_t, int>, StoredAction> actions;
  dt::Stmt query(db.get(),
                 "SELECT info_id, ordinal, kind, target, probability FROM actions"
                 " ORDER BY info_id, ordinal",
                 "read actions");
  std::int64_t last_info = -1;
  int expected_ordinal = 0;
  while (query.step("read actions")) {
    StoredAction action;
    action.info_id = query.column_i64(0);
    const std::int64_t ordinal_raw = query.column_i64(1);
    // Validate kind against the raw int64 before narrowing, so a congruent
    // value such as 2^32+1 cannot wrap to a valid small kind.
    const std::int64_t kind_raw = query.column_i64(2);
    check(action.info_id >= 0, ArtifactErrorKind::InvalidSchema, "negative actions.info_id");
    check(ordinal_raw >= 0 && ordinal_raw < 32, ArtifactErrorKind::InvalidSchema,
          "actions.ordinal outside 0..31");
    check(kind_raw >= 0 && kind_raw <= 4, ArtifactErrorKind::InvalidValue,
          "action kind out of range");
    action.ordinal = static_cast<int>(ordinal_raw);
    action.kind = static_cast<int>(kind_raw);
    if (action.info_id != last_info) {
      check(action.info_id > last_info, ArtifactErrorKind::InvalidSchema,
            "actions info_id ordering");
      last_info = action.info_id;
      expected_ordinal = 0;
    }
    check(action.ordinal == expected_ordinal, ArtifactErrorKind::InvalidSchema,
          "action ordinals must be contiguous from zero");
    ++expected_ordinal;
    if (!query.column_null(3))
      action.target = static_cast<Chips>(read_bounded_i64(query, 3, "action target"));
    const bool aggressive = action.kind == 3 || action.kind == 4;
    check(action.target.has_value() == aggressive, ArtifactErrorKind::InvalidSchema,
          "aggressive actions require a target and others must not carry one");
    action.probability = query.column_double(4);
    dt::require_finite(action.probability, "action probability");
    check(action.probability >= 0 && action.probability <= 1, ArtifactErrorKind::InvalidValue,
          "action probability outside [0,1]");
    actions.emplace(std::make_pair(action.info_id, action.ordinal), std::move(action));
  }
  return actions;
}

std::map<std::pair<std::int64_t, int>, StoredTraining> read_training(dt::Db& db) {
  std::map<std::pair<std::int64_t, int>, StoredTraining> training;
  dt::Stmt query(db.get(),
                 "SELECT info_id, ordinal, regret, average_weight FROM training"
                 " ORDER BY info_id, ordinal",
                 "read training");
  while (query.step("read training")) {
    StoredTraining row;
    row.info_id = query.column_i64(0);
    const std::int64_t ordinal_raw = query.column_i64(1);
    check(row.info_id >= 0 && ordinal_raw >= 0 && ordinal_raw < 32,
          ArtifactErrorKind::InvalidSchema, "training (info_id, ordinal) out of range");
    row.ordinal = static_cast<int>(ordinal_raw);
    row.regret = query.column_double(2);
    row.average_weight = query.column_double(3);
    dt::require_finite(row.regret, "cumulative regret");
    dt::require_finite(row.average_weight, "average weight");
    check(row.average_weight >= 0, ArtifactErrorKind::InvalidValue,
          "negative average training weight");
    training.emplace(std::make_pair(row.info_id, row.ordinal), std::move(row));
  }
  return training;
}

void read_auxiliary_tables(dt::Db& db) {
  dt::Stmt bounds_query(db.get(),
                        "SELECT responder_combo, baseline_cf_mass, normalized_bound FROM bounds",
                        "read bounds");
  while (bounds_query.step("read bounds")) {
    if (!bounds_query.column_null(0)) {
      const std::int64_t combo = bounds_query.column_i64(0);
      check(combo >= 0 && combo <= 1325, ArtifactErrorKind::InvalidValue,
            "bound responder combo out of range");
    }
    if (!bounds_query.column_null(1)) {
      const double mass = bounds_query.column_double(1);
      dt::require_finite(mass, "baseline cf mass");
      check(mass >= 0, ArtifactErrorKind::InvalidValue, "negative baseline cf mass");
    }
    if (!bounds_query.column_null(2))
      dt::require_finite(bounds_query.column_double(2), "normalized bound");
  }

  dt::Stmt measurements_query(
      db.get(), "SELECT seed_hex, iterations, metric, metric_class, value FROM measurements",
      "read measurements");
  while (measurements_query.step("read measurements")) {
    if (!measurements_query.column_null(0)) {
      const std::string_view seed = measurements_query.column_text(0);
      check(dt::is_hex_word(seed), ArtifactErrorKind::InvalidSchema,
            "measurement seed must be 16 lowercase hex digits");
    }
    if (!measurements_query.column_null(1)) {
      const std::int64_t iterations = measurements_query.column_i64(1);
      check(iterations >= 0 && static_cast<std::uint64_t>(iterations) <= kMaxStoredChips,
            ArtifactErrorKind::InvalidValue, "measurement iterations out of range");
    }
    check(!measurements_query.column_text(2).empty(), ArtifactErrorKind::InvalidSchema,
          "measurement metric is empty");
    const std::string_view metric_class = measurements_query.column_text(3);
    check(metric_class == "exact" || metric_class == "estimated", ArtifactErrorKind::InvalidSchema,
          "measurement class must be exact/estimated");
    dt::require_finite(measurements_query.column_double(4), "measurement value");
  }
}

// --- v2 (RFC 0009 D3) reader: seat-generic ---

ArtifactManifest read_manifest_v2(dt::Db& db) {
  dt::Stmt query(db.get(),
                 "SELECT kind, algorithm_revision, info_key_revision, numeric_profile,"
                 " validation_state, engine_revision, completed_iterations, prng_identifier,"
                 " seed_hex, prng_state_hex, run_status, nodes, information_sets,"
                 " accounted_bytes, abstraction_name, abstraction_version,"
                 " abstraction_parameters, abstraction_digest, terminal_depth FROM manifest",
                 "read manifest");
  if (!query.step("read manifest"))
    fail(ArtifactErrorKind::InvalidSchema, "manifest table is empty");
  ArtifactManifest manifest;
  int i = 0;
  manifest.kind = kind_from_text(query.column_text(i++));
  manifest.algorithm_revision = std::string(query.column_text(i++));
  const std::int64_t info_key_revision_raw = query.column_i64(i++);
  check(info_key_revision_raw == static_cast<std::int64_t>(kArtifactSchemaVersionV2),
        ArtifactErrorKind::UnsupportedVersion, "unsupported information-key revision");
  manifest.information_key_revision = static_cast<std::uint32_t>(info_key_revision_raw);
  manifest.numeric_profile = std::string(query.column_text(i++));
  check(manifest.numeric_profile == kNumericProfileV2, ArtifactErrorKind::UnsupportedVersion,
        "unsupported numeric profile: " + manifest.numeric_profile);
  manifest.validation = validation_from_text(query.column_text(i++));
  manifest.engine_revision = std::string(query.column_text(i++));
  manifest.completed_iterations =
      static_cast<std::uint64_t>(read_bounded_i64(query, i++, "completed_iterations"));
  manifest.prng_identifier = std::string(query.column_text(i++));
  check(manifest.prng_identifier == kSampledPrngIdentifier ||
            manifest.prng_identifier == kFullTraversalPrngIdentifier,
        ArtifactErrorKind::UnsupportedVersion,
        "unsupported PRNG identifier: " + manifest.prng_identifier);
  const std::string_view seed_hex = query.column_text(i++);
  const std::string_view state_hex = query.column_text(i++);
  check(dt::is_hex_word(seed_hex) && dt::is_hex_word(state_hex), ArtifactErrorKind::InvalidSchema,
        "PRNG words must be 16 lowercase hex digits");
  manifest.seed = dt::hex_to_u64(seed_hex);
  manifest.prng_state = dt::hex_to_u64(state_hex);
  manifest.run_status = run_status_from_text(query.column_text(i++));
  manifest.nodes = static_cast<std::size_t>(read_bounded_i64(query, i++, "nodes"));
  manifest.information_sets =
      static_cast<std::size_t>(read_bounded_i64(query, i++, "information_sets"));
  manifest.accounted_bytes =
      static_cast<std::size_t>(read_bounded_i64(query, i++, "accounted_bytes"));
  // v2 identity fields.
  manifest.abstraction_name = std::string(query.column_text(i++));
  check(!manifest.abstraction_name.empty(), ArtifactErrorKind::InvalidSchema,
        "abstraction_name is empty");
  const std::int64_t abstraction_version_raw = query.column_i64(i++);
  check(abstraction_version_raw >= 0, ArtifactErrorKind::InvalidSchema,
        "abstraction_version must be nonnegative");
  manifest.abstraction_version = static_cast<std::uint32_t>(abstraction_version_raw);
  manifest.abstraction_parameters = std::string(query.column_text(i++));
  const std::string_view digest_hex = query.column_text(i++);
  check(dt::is_hex_word(digest_hex), ArtifactErrorKind::InvalidSchema,
        "abstraction_digest must be 16 lowercase hex digits");
  manifest.abstraction_digest = dt::hex_to_u64(digest_hex);
  const std::int64_t terminal_depth_raw = query.column_i64(i++);
  check(terminal_depth_raw == 0 || terminal_depth_raw == 1, ArtifactErrorKind::InvalidSchema,
        "terminal_depth must be 0 (river) or 1 (flop)");
  manifest.terminal_depth = static_cast<poker::TerminalDepth>(terminal_depth_raw);
  check(!manifest.algorithm_revision.empty() && !manifest.engine_revision.empty(),
        ArtifactErrorKind::InvalidSchema, "manifest revision strings are empty");
  check(!query.step("read manifest"), ArtifactErrorKind::InvalidSchema,
        "manifest table is not a singleton");
  return manifest;
}

// Reconstructs the GameDef identity from the seat-generic game row and the
// game_seats child table. The reader reconstructs blinds_posted=0 and
// preflop=false (the writer requires both), so same_game_def round-trips
// exactly. variant is always NoLimitHoldem.
GameDef read_game_v2(dt::Db& db) {
  dt::Stmt query(db.get(),
                 "SELECT rules_id, player_count, button, big_blind, ante, root_street,"
                 " terminal_depth, flop0, flop1, flop2, fixed_turn, fixed_river, pot,"
                 " utility_id FROM game",
                 "read game");
  if (!query.step("read game"))
    fail(ArtifactErrorKind::InvalidSchema, "game table is empty");
  GameDef game{};
  int i = 0;
  const std::string_view rules = query.column_text(i++);
  check(rules == kRulesIdentifierV2, ArtifactErrorKind::UnsupportedVersion,
        "unsupported rules identifier: " + std::string(rules));
  const std::int64_t player_count_raw = query.column_i64(i++);
  check(player_count_raw >= 2 && player_count_raw <= 10, ArtifactErrorKind::InvalidSchema,
        "game.player_count outside 2..10");
  game.player_count = static_cast<std::size_t>(player_count_raw);
  const std::int64_t button_raw = query.column_i64(i++);
  check(button_raw >= 0 && button_raw < player_count_raw, ArtifactErrorKind::InvalidSchema,
        "game.button outside seats");
  game.button = static_cast<std::size_t>(button_raw);
  game.big_blind = static_cast<Chips>(read_bounded_i64(query, i++, "big_blind"));
  const Chips ante = static_cast<Chips>(read_bounded_i64(query, i++, "ante"));
  check(ante == 0, ArtifactErrorKind::UnsupportedVersion,
        "nonzero ante is not supported by the v2 profile");
  game.ante = ante;
  const std::int64_t root_street = query.column_i64(i++);
  check(root_street >= 0 && root_street <= 2, ArtifactErrorKind::UnsupportedVersion,
        "v2 reader supports rooted flop/turn/river games only (root_street 0..2)");
  const std::int64_t terminal_depth_raw = query.column_i64(i++);
  check(terminal_depth_raw == 0 || terminal_depth_raw == 1, ArtifactErrorKind::InvalidSchema,
        "game.terminal_depth must be 0 or 1");
  game.terminal = static_cast<poker::TerminalDepth>(terminal_depth_raw);
  for (int flop_index = 0; flop_index < 3; ++flop_index) {
    const std::int64_t flop_card = query.column_i64(i++);
    check(flop_card >= 0 && flop_card < 52, ArtifactErrorKind::InvalidValue,
          "game flop card outside 0..51");
    game.board[flop_index] = static_cast<int>(flop_card);
  }
  // root_street 0 (flop): board_size 3, no fixed cards.
  // root_street 1 (turn): board_size 4, fixed_turn = board[3].
  // root_street 2 (river): board_size 5, fixed_turn = board[3], fixed_river = board[4].
  if (root_street >= 1) {
    check(!query.column_null(i), ArtifactErrorKind::InvalidSchema,
          "turn-rooted game requires a fixed_turn card");
    const std::int64_t turn_card = query.column_i64(i);
    check(turn_card >= 0 && turn_card < 52, ArtifactErrorKind::InvalidValue,
          "fixed_turn card outside 0..51");
    game.board[3] = static_cast<int>(turn_card);
  } else {
    check(query.column_null(i), ArtifactErrorKind::InvalidSchema,
          "flop-rooted game must not carry a fixed_turn card");
  }
  ++i;
  if (root_street >= 2) {
    check(!query.column_null(i), ArtifactErrorKind::InvalidSchema,
          "river-rooted game requires a fixed_river card");
    const std::int64_t river_card = query.column_i64(i);
    check(river_card >= 0 && river_card < 52, ArtifactErrorKind::InvalidValue,
          "fixed_river card outside 0..51");
    game.board[4] = static_cast<int>(river_card);
  } else {
    check(query.column_null(i), ArtifactErrorKind::InvalidSchema,
          "non-river-rooted game must not carry a fixed_river card");
  }
  ++i;
  game.board_size = static_cast<std::uint8_t>(3 + root_street);
  game.pot = static_cast<Chips>(read_bounded_i64(query, i++, "pot"));
  const std::string_view utility = query.column_text(i++);
  check(utility == kUtilityIdentifierV1, ArtifactErrorKind::UnsupportedVersion,
        "unsupported utility identifier: " + std::string(utility));
  check(!query.step("read game"), ArtifactErrorKind::InvalidSchema, "game table not singleton");
  game.preflop = false;
  game.variant = RulesVariant::NoLimitHoldem;
  return game;
}

void read_game_seats(dt::Db& db, GameDef& game) {
  dt::Stmt query(db.get(), "SELECT seat, stack, contribution FROM game_seats ORDER BY seat",
                 "read game seats");
  std::int64_t expected_seat = 0;
  std::size_t rows = 0;
  while (query.step("read game seats")) {
    const std::int64_t seat_raw = query.column_i64(0);
    check(seat_raw >= 0 && seat_raw < static_cast<std::int64_t>(game.player_count),
          ArtifactErrorKind::InvalidSchema, "game_seats.seat outside player_count");
    check(seat_raw == expected_seat, ArtifactErrorKind::InvalidSchema,
          "game_seats must be contiguous from seat 0");
    ++expected_seat;
    const Chips stack = static_cast<Chips>(read_bounded_i64(query, 1, "stack"));
    const Chips contribution = static_cast<Chips>(read_bounded_i64(query, 2, "contribution"));
    game.stacks[static_cast<std::size_t>(seat_raw)] = stack;
    game.contributions[static_cast<std::size_t>(seat_raw)] = contribution;
    ++rows;
  }
  check(rows == game.player_count, ArtifactErrorKind::InvalidSchema,
        "game_seats must have exactly one row per occupied seat");
}

SizeSchedule read_sizes_v2(dt::Db& db) {
  SizeSchedule sizes{};
  for (auto& street : sizes) {
    street.bets.clear();
    street.raises.clear();
  }
  dt::Stmt query(db.get(),
                 "SELECT street, kind, ordinal, numerator, denominator FROM sizes"
                 " ORDER BY street, kind, ordinal",
                 "read sizes");
  std::array<std::array<std::size_t, 2>, 4> next_ordinal{};
  while (query.step("read sizes")) {
    const std::int64_t street_raw = query.column_i64(0);
    const std::int64_t kind_raw = query.column_i64(1);
    check(street_raw >= 0 && street_raw <= 3, ArtifactErrorKind::InvalidSchema,
          "sizes.street outside 0..3");
    const std::int64_t ordinal = query.column_i64(2);
    check(ordinal >= 0 && ordinal < 32, ArtifactErrorKind::InvalidSchema,
          "sizes.ordinal outside 0..31");
    check(kind_raw == 0 || kind_raw == 1, ArtifactErrorKind::InvalidSchema,
          "sizes.kind must be 0 (bet) or 1 (raise)");
    const int street = static_cast<int>(street_raw);
    const int kind = static_cast<int>(kind_raw);
    const Chips numerator = static_cast<Chips>(read_bounded_i64(query, 3, "numerator"));
    const Chips denominator = static_cast<Chips>(read_bounded_i64(query, 4, "denominator"));
    check(numerator > 0 && denominator > 0, ArtifactErrorKind::InvalidValue,
          "non-positive size fraction");
    check(ordinal == static_cast<std::int64_t>(next_ordinal[street][kind]),
          ArtifactErrorKind::InvalidSchema, "non-contiguous size ordinals");
    ++next_ordinal[street][kind];
    auto& street_sizes = sizes[street];
    (kind == 0 ? street_sizes.bets : street_sizes.raises).push_back({numerator, denominator});
  }
  return sizes;
}

std::vector<std::vector<WeightedHand>> read_ranges_v2(dt::Db& db, std::size_t player_count) {
  std::vector<std::vector<WeightedHand>> ranges(player_count);
  dt::Stmt query(db.get(), "SELECT player, combo, weight FROM ranges ORDER BY player, combo",
                 "read ranges");
  std::int64_t last_player = -1;
  std::int64_t last_combo = -1;
  while (query.step("read ranges")) {
    const std::int64_t player_raw = query.column_i64(0);
    const std::int64_t combo_raw = query.column_i64(1);
    check(player_raw >= 0 && player_raw < static_cast<std::int64_t>(player_count),
          ArtifactErrorKind::InvalidSchema, "ranges.player outside player_count");
    check(combo_raw >= 0 && combo_raw <= 1325, ArtifactErrorKind::InvalidSchema,
          "ranges.combo outside 0..1325");
    const int player = static_cast<int>(player_raw);
    const int combo = static_cast<int>(combo_raw);
    const double weight = query.column_double(2);
    dt::require_finite(weight, "range weight");
    check(weight >= 0, ArtifactErrorKind::InvalidValue, "negative range weight");
    if (player_raw != last_player) {
      check(player_raw == last_player + 1, ArtifactErrorKind::InvalidSchema,
            "range player rows out of order");
      last_combo = -1;
      last_player = player_raw;
    }
    check(combo_raw > last_combo, ArtifactErrorKind::InvalidSchema,
          "range combo rows out of order or duplicated");
    last_combo = combo_raw;
    const auto cards = dt::combo_cards(combo);
    ranges[player].push_back(WeightedHand{{cards[0], cards[1]}, weight});
  }
  check(last_player == static_cast<std::int64_t>(player_count) - 1,
        ArtifactErrorKind::InvalidSchema, "every seat requires at least one range combo");
  for (std::size_t p = 0; p < player_count; ++p)
    check(!ranges[p].empty(), ArtifactErrorKind::InvalidSchema,
          "every seat requires at least one range combo");
  return ranges;
}

// Like read_states but admits the seat-generic player domain 0..9, bounded by
// the game's actual player_count (RFC 0009 D3: the reader validates
// player < player_count in C++, since a SQL CHECK is write-time-only and can be
// stripped from a tampered file).
std::map<std::int64_t, StoredState> read_states_v2(dt::Db& db, std::size_t player_count) {
  std::map<std::int64_t, StoredState> states;
  dt::Stmt query(db.get(),
                 "SELECT id, player, card0, card1, public_key FROM information_states"
                 " ORDER BY id",
                 "read information states");
  std::int64_t expected = 0;
  while (query.step("read information states")) {
    StoredState state;
    state.id = query.column_i64(0);
    check(state.id >= 0, ArtifactErrorKind::InvalidSchema, "negative information state id");
    check(state.id == expected, ArtifactErrorKind::InvalidSchema,
          "information state ids must be contiguous from zero");
    ++expected;
    const std::int64_t player_raw = query.column_i64(1);
    const std::int64_t card0_raw = query.column_i64(2);
    const std::int64_t card1_raw = query.column_i64(3);
    state.public_key = std::string(query.column_text(4));
    check(player_raw >= 0 && player_raw <= 9, ArtifactErrorKind::InvalidSchema,
          "information state player outside 0..9");
    check(player_raw < static_cast<std::int64_t>(player_count), ArtifactErrorKind::InvalidSchema,
          "information state player outside player_count");
    check(card0_raw >= 0 && card0_raw < card1_raw && card1_raw <= 51,
          ArtifactErrorKind::InvalidValue, "information state own cards out of range");
    state.player = static_cast<int>(player_raw);
    state.card0 = static_cast<int>(card0_raw);
    state.card1 = static_cast<int>(card1_raw);
    states.emplace(state.id, std::move(state));
  }
  return states;
}

// Validates the v2 bounds table, whose dimension is (public_root, seat, combo)
// rather than v1's responder_combo. The measurements table is byte-identical to
// v1 and is validated by the shared loop.
void read_auxiliary_tables_v2(dt::Db& db, std::size_t player_count) {
  dt::Stmt bounds_query(db.get(),
                        "SELECT public_root, seat, combo, baseline_cf_mass, normalized_bound"
                        " FROM bounds",
                        "read bounds");
  while (bounds_query.step("read bounds")) {
    check(!bounds_query.column_text(0).empty(), ArtifactErrorKind::InvalidSchema,
          "bound public_root is empty");
    const std::int64_t seat = bounds_query.column_i64(1);
    check(seat >= 0 && seat <= 9, ArtifactErrorKind::InvalidValue, "bound seat out of range");
    check(seat < static_cast<std::int64_t>(player_count), ArtifactErrorKind::InvalidSchema,
          "bound seat outside player_count");
    const std::int64_t combo = bounds_query.column_i64(2);
    check(combo >= 0 && combo <= 1325, ArtifactErrorKind::InvalidValue, "bound combo out of range");
    if (!bounds_query.column_null(3)) {
      const double mass = bounds_query.column_double(3);
      dt::require_finite(mass, "baseline cf mass");
      check(mass >= 0, ArtifactErrorKind::InvalidValue, "negative baseline cf mass");
    }
    if (!bounds_query.column_null(4))
      dt::require_finite(bounds_query.column_double(4), "normalized bound");
  }

  dt::Stmt measurements_query(
      db.get(), "SELECT seed_hex, iterations, metric, metric_class, value FROM measurements",
      "read measurements");
  while (measurements_query.step("read measurements")) {
    if (!measurements_query.column_null(0)) {
      const std::string_view seed = measurements_query.column_text(0);
      check(dt::is_hex_word(seed), ArtifactErrorKind::InvalidSchema,
            "measurement seed must be 16 lowercase hex digits");
    }
    if (!measurements_query.column_null(1)) {
      const std::int64_t iterations = measurements_query.column_i64(1);
      check(iterations >= 0 && static_cast<std::uint64_t>(iterations) <= kMaxStoredChips,
            ArtifactErrorKind::InvalidValue, "measurement iterations out of range");
    }
    check(!measurements_query.column_text(2).empty(), ArtifactErrorKind::InvalidSchema,
          "measurement metric is empty");
    const std::string_view metric_class = measurements_query.column_text(3);
    check(metric_class == "exact" || metric_class == "estimated", ArtifactErrorKind::InvalidSchema,
          "measurement class must be exact/estimated");
    dt::require_finite(measurements_query.column_double(4), "measurement value");
  }
}

// A versioned schema set: the canonical DDL statements, the allowed table
// names, and a diagnostic label. open_validated selects one by user_version;
// a file written under one version is rejected by the other's set.
struct SchemaSet {
  std::span<const char* const> ddl;
  const std::set<std::string>& allowed_tables;
  std::string_view label;
};

const std::set<std::string> kAllowedTablesV1{
    "manifest", "game",     "sizes",  "ranges",       "information_states",
    "actions",  "training", "bounds", "measurements",
};

const std::set<std::string> kAllowedTablesV2{
    "manifest",           "game",    "game_seats", "sizes",  "ranges",
    "information_states", "actions", "training",   "bounds", "measurements",
};

const SchemaSet kSchemaV1{kSchemaDdlV1, kAllowedTablesV1, "v1"};
const SchemaSet kSchemaV2{kSchemaDdlV2, kAllowedTablesV2, "v2"};

void verify_schema_objects(dt::Db& db, const SchemaSet& schema) {
  // The stored CREATE text must be byte-identical to the DDL this writer
  // emits. A name/STRICT-suffix whitelist is not enough: integrity_check does
  // not validate CHECK, foreign-key, or type-affinity clauses, and a malicious
  // file can strip them through writable_schema. SQLite stores the submitted
  // CREATE statement verbatim minus the trailing semicolon.
  std::map<std::string, std::string> canonical_ddl;
  for (const char* statement : schema.ddl) {
    std::string ddl(statement);
    if (!ddl.empty() && ddl.back() == ';')
      ddl.pop_back();
    const std::size_t create_table = ddl.find("CREATE TABLE ");
    const std::size_t name_start = create_table + std::string("CREATE TABLE ").size();
    const std::size_t name_end = ddl.find(' ', name_start);
    canonical_ddl.emplace(ddl.substr(name_start, name_end - name_start), std::move(ddl));
  }
  check(canonical_ddl.size() == schema.allowed_tables.size(), ArtifactErrorKind::Corrupt,
        "internal canonical schema table set mismatch");

  dt::Stmt query(db.get(), "SELECT type, name, tbl_name, sql FROM sqlite_schema ORDER BY name",
                 "read schema objects");
  std::set<std::string> tables_seen;
  while (query.step("read schema objects")) {
    const std::string_view type = query.column_text(0);
    const std::string_view name = query.column_text(1);
    const bool sql_is_null = query.column_null(3);
    const std::string_view sql = sql_is_null ? std::string_view{} : query.column_text(3);
    const std::string name_owned(name);
    if (type == "table") {
      const auto canonical = canonical_ddl.find(name_owned);
      check(canonical != canonical_ddl.end(), ArtifactErrorKind::InvalidSchema,
            "unknown table in artifact: " + name_owned);
      check(!sql_is_null && sql == canonical->second, ArtifactErrorKind::InvalidSchema,
            "table definition differs from the canonical schema " + std::string(schema.label) +
                " DDL: " + name_owned);
      tables_seen.insert(name_owned);
    } else if (type == "index") {
      // Only the automatic UNIQUE indexes that back our PRIMARY KEY/UNIQUE
      // constraints may exist; they have no stored CREATE text.
      check(sql_is_null, ArtifactErrorKind::InvalidSchema,
            "index with explicit CREATE text is not allowed in schema " +
                std::string(schema.label) + ": " + name_owned);
      check(name.rfind("sqlite_autoindex_", 0) == 0, ArtifactErrorKind::InvalidSchema,
            "user-created indexes are not allowed in schema " + std::string(schema.label) + ": " +
                name_owned);
    } else {
      // Views, triggers, virtual tables, and any other sqlite_schema row are
      // parse-amplification and identity hazards and are rejected outright.
      fail(ArtifactErrorKind::InvalidSchema,
           "unexpected schema object " + name_owned + " of type " + std::string(type));
    }
  }
  check(tables_seen == schema.allowed_tables, ArtifactErrorKind::InvalidSchema,
        "artifact is missing required tables");
}

void verify_integrity(dt::Db& db) {
  dt::Stmt integrity(db.get(), "PRAGMA integrity_check", "integrity check");
  if (!integrity.step("integrity check") || integrity.column_text(0) != "ok")
    fail(ArtifactErrorKind::Corrupt, "integrity_check did not return ok");
  dt::Stmt fk(db.get(), "PRAGMA foreign_key_check", "foreign key check");
  if (fk.step("foreign key check"))
    fail(ArtifactErrorKind::Corrupt, "foreign_key_check reported violations");
}

// Opens and fully validates an artifact for reading, exactly as load_path.
struct OpenedArtifact {
  dt::Db db;
  Sha256Digest digest;
  std::uint64_t file_bytes = 0;
  // The artifact's schema user_version (1 or 2), selected by open_validated.
  // load_path dispatches on this to reconstruct the matching domain records.
  std::uint32_t schema_version = 0;
};

OpenedArtifact open_validated(const std::filesystem::path& path, const LoadOptions& options) {
  const std::uint64_t file_bytes = dt::file_size_required(path, options.max_file_bytes);
  dt::validate_database_header(path, file_bytes);
  dt::reject_sqlite_sidecars(path);
  const Sha256Digest digest = dt::sha256_file(path, options.max_file_bytes);
  if (options.expected_sha256 && *options.expected_sha256 != digest)
    fail(ArtifactErrorKind::DigestMismatch, "artifact digest mismatch for " + path.string());
  dt::ensure_reader_heap_bound();
  OpenedArtifact opened;
  opened.file_bytes = file_bytes;
  opened.digest = digest;
  dt::open_immutable_reader(opened.db, path);
  const std::int64_t application_id = opened.db.pragma_i64("application_id", "read application id");
  if (application_id != static_cast<std::int64_t>(kArtifactApplicationId))
    fail(ArtifactErrorKind::UnsupportedVersion,
         "unexpected application_id " + std::to_string(application_id));
  const std::int64_t user_version = opened.db.pragma_i64("user_version", "read schema version");
  const SchemaSet* schema = nullptr;
  if (user_version == static_cast<std::int64_t>(kArtifactSchemaVersion))
    schema = &kSchemaV1;
  else if (user_version == static_cast<std::int64_t>(kArtifactSchemaVersionV2))
    schema = &kSchemaV2;
  else
    fail(ArtifactErrorKind::UnsupportedVersion,
         "unsupported artifact schema user_version " + std::to_string(user_version));
  opened.schema_version = static_cast<std::uint32_t>(user_version);
  verify_schema_objects(opened.db, *schema);
  verify_integrity(opened.db);
  return opened;
}

// Reconstructs a seat-generic SeatTrainingResult from a validated schema-v2
// file. The actions and training readers are shared with v1 (their columns are
// byte-identical); the states reader admits seats 0..9 and the training table
// is required empty because SeatPolicyRow carries no regret/average-weight
// state.
LoadedArtifact load_path_v2(OpenedArtifact& opened) {
  dt::Db& db = opened.db;
  const Sha256Digest digest = opened.digest;
  const std::uint64_t file_bytes = opened.file_bytes;

  ArtifactManifest manifest = read_manifest_v2(db);
  GameDef game = read_game_v2(db);
  read_game_seats(db, game);
  SizeSchedule sizes = read_sizes_v2(db);
  auto ranges = read_ranges_v2(db, game.player_count);
  validate_game_v2(game);

  auto states = read_states_v2(db, game.player_count);
  auto actions = read_actions(db);
  auto training = read_training(db);
  read_auxiliary_tables_v2(db, game.player_count);

  // Referential integrity independent of stripped foreign keys.
  for (const auto& [key, stored_action] : actions)
    check(states.contains(key.first), ArtifactErrorKind::InvalidSchema,
          "action row references a missing information state");
  for (const auto& [id, state] : states) {
    (void)state;
    check(actions.lower_bound({id, 0}) != actions.end() &&
              actions.lower_bound({id, 0})->first.first == id,
          ArtifactErrorKind::InvalidSchema,
          "information state has no actions after referential validation");
  }

  // Reconstruct information keys from the canonical text and player column.
  for (const auto& [id, state] : states) {
    (void)decode_public_key(state.public_key, kArtifactSchemaVersionV2, state.player, state.card0,
                            state.card1);
  }

  SeatTrainingResult result;
  result.completed_iterations = manifest.completed_iterations;
  result.seed = manifest.seed;
  result.prng_state = manifest.prng_state;
  result.termination = manifest.run_status == RunStatus::Complete
                           ? NSeatTerminationPhase::Complete
                           : NSeatTerminationPhase::ResourceLimit;
  result.nodes = manifest.nodes;
  result.information_sets = manifest.information_sets;
  result.accounted_bytes = manifest.accounted_bytes;
  result.action_id = {manifest.abstraction_name, manifest.abstraction_version,
                      manifest.abstraction_parameters, manifest.abstraction_digest};
  result.terminal_depth = manifest.terminal_depth;
  // The uint32 algorithm_revision is the trainer's revision; the manifest
  // carries the string identifier. Reconstruct the current revision; the
  // string is the authoritative record.
  result.algorithm_revision = solver::kNSeatAlgorithmRevision;

  dt::SeatPolicyAssembler::set_identity(result.policy, game, sizes, ranges, result.action_id);

  for (const auto& [id, state] : states) {
    InformationKey key = decode_public_key(state.public_key, kArtifactSchemaVersionV2, state.player,
                                           state.card0, state.card1);
    solver::SeatPolicyRow row;
    for (auto it = actions.lower_bound({id, 0}); it != actions.end() && it->first.first == id;
         ++it) {
      const StoredAction& stored = it->second;
      Action action{dt::action_kind_from_id(stored.kind), stored.target.value_or(0)};
      row.actions.push_back(action);
      row.probabilities.push_back(stored.probability);
    }
    check(!row.actions.empty(), ArtifactErrorKind::InvalidSchema,
          "information state has no actions");
    validate_probabilities_local(row.probabilities, "information state " + std::to_string(id));
    dt::SeatPolicyAssembler::add_row(result.policy, key, std::move(row));
  }

  // The v2 writer never writes training rows; a v2 file carrying them is
  // malformed regardless of kind.
  check(training.empty(), ArtifactErrorKind::InvalidSchema,
        "schema-v2 artifact must not carry training rows");
  if (manifest.kind == ArtifactKind::Policy) {
    check(manifest.validation == ValidationState::Validated, ArtifactErrorKind::InvalidSchema,
          "published policy must be validated");
  }
  check(result.information_sets == states.size(), ArtifactErrorKind::InvalidSchema,
        "manifest information_sets disagrees with stored state count");

  ArtifactBundle bundle;
  bundle.manifest = std::move(manifest);
  bundle.nseat = std::move(result);

  LoadedArtifact loaded;
  loaded.bundle = std::move(bundle);
  loaded.sha256 = digest;
  loaded.sha256_hex = dt::to_hex(digest);
  loaded.file_bytes = file_bytes;
  return loaded;
}

LoadedArtifact load_path(const std::filesystem::path& path, const LoadOptions& options) {
  OpenedArtifact opened = open_validated(path, options);
  if (opened.schema_version == kArtifactSchemaVersionV2)
    return load_path_v2(opened);
  dt::Db& db = opened.db;
  const Sha256Digest digest = opened.digest;
  const std::uint64_t file_bytes = opened.file_bytes;

  ArtifactManifest manifest = read_manifest(db);
  HeadsUpGame game = read_game(db);
  read_sizes(db, game);
  read_ranges(db, game);
  validate_game(game);

  auto states = read_states(db);
  auto actions = read_actions(db);
  auto training = read_training(db);
  read_auxiliary_tables(db);

  // Referential integrity independent of stripped foreign keys: every action
  // must belong to an existing state and every training row to an existing
  // action; every state must carry at least one action.
  for (const auto& [key, stored_action] : actions)
    check(states.contains(key.first), ArtifactErrorKind::InvalidSchema,
          "action row references a missing information state");
  for (const auto& [key, stored_training] : training) {
    (void)stored_training;
    check(actions.contains(key), ArtifactErrorKind::InvalidSchema,
          "training row references a missing action");
  }
  for (const auto& [id, state] : states) {
    (void)state;
    check(actions.lower_bound({id, 0}) != actions.end() &&
              actions.lower_bound({id, 0})->first.first == id,
          ArtifactErrorKind::InvalidSchema,
          "information state has no actions after referential validation");
  }

  // Reconstruct information keys from the canonical text and player column and
  // require them to agree with the stored own cards.
  for (const auto& [id, state] : states) {
    // The canonical text must reparse against the own-combo columns; street,
    // board order, event grammar, and card uniqueness are validated inside.
    (void)decode_public_key(state.public_key, kArtifactSchemaVersion, state.player, state.card0,
                            state.card1);
  }

  TrainingResult result;
  result.completed_iterations = manifest.completed_iterations;
  result.seed = manifest.seed;
  result.prng_state = manifest.prng_state;
  result.status = manifest.run_status == RunStatus::Complete ? TrainingStatus::Complete
                                                             : TrainingStatus::ResourceLimit;
  result.nodes = manifest.nodes;
  result.information_sets = manifest.information_sets;
  result.accounted_bytes = manifest.accounted_bytes;
  dt::PolicyAssembler::set_game(result.policy, game);

  TrainingRows raw_rows;
  for (const auto& [id, state] : states) {
    InformationKey key = decode_public_key(state.public_key, kArtifactSchemaVersion, state.player,
                                           state.card0, state.card1);
    PolicyRow row;
    std::vector<double> regrets;
    std::vector<double> averages;
    for (auto it = actions.lower_bound({id, 0}); it != actions.end() && it->first.first == id;
         ++it) {
      const StoredAction& stored = it->second;
      Action action{dt::action_kind_from_id(stored.kind), stored.target.value_or(0)};
      row.actions.push_back(action);
      row.probabilities.push_back(stored.probability);
      const auto training_it = training.find({id, stored.ordinal});
      if (training_it != training.end()) {
        regrets.push_back(training_it->second.regret);
        averages.push_back(training_it->second.average_weight);
      }
    }
    check(!row.actions.empty(), ArtifactErrorKind::InvalidSchema,
          "information state has no actions");
    validate_probabilities_local(row.probabilities, "information state " + std::to_string(id));

    // Capture raw training state before `row` is moved into the policy.
    const bool has_training = !regrets.empty() || !averages.empty();
    if (has_training)
      check(regrets.size() == row.actions.size() && averages.size() == row.actions.size(),
            ArtifactErrorKind::InvalidSchema,
            "training rows must cover every action ordinal of a state");
    TrainingRow raw;
    raw.actions = row.actions;
    raw.regrets = std::move(regrets);
    raw.average_weights = std::move(averages);

    dt::PolicyAssembler::add_row(result.policy, key, std::move(row));

    if (has_training)
      raw_rows.emplace(std::move(key), std::move(raw));
  }

  if (manifest.kind == ArtifactKind::Policy) {
    check(training.empty(), ArtifactErrorKind::InvalidSchema,
          "published policy must not carry a training table");
    check(manifest.validation == ValidationState::Validated, ArtifactErrorKind::InvalidSchema,
          "published policy must be validated");
  } else {
    check(!result.policy.rows().empty() ? raw_rows.size() == states.size() : raw_rows.empty(),
          ArtifactErrorKind::InvalidSchema,
          "checkpoint raw rows must cover every information state");
  }
  check(result.information_sets == states.size(), ArtifactErrorKind::InvalidSchema,
        "manifest information_sets disagrees with stored state count");

  ArtifactBundle bundle;
  bundle.manifest = std::move(manifest);
  bundle.result = std::move(result);
  bundle.rows = std::move(raw_rows);

  LoadedArtifact loaded;
  loaded.bundle = std::move(bundle);
  loaded.sha256 = digest;
  loaded.sha256_hex = dt::to_hex(digest);
  loaded.file_bytes = file_bytes;
  return loaded;
}

// SQL aggregate word count for one canonical key's 64-bit storage layout:
// actor, two own cards, board size (4 header words), the ordered board words,
// then four words per stored betting event. The board and event lists are
// the two pipe-separated text fields of `street|board|events`.
const char* const kProbeKeyAggregateSql =
    "WITH split1 AS ("
    "  SELECT substr(public_key, instr(public_key, '|') + 1) AS rest"
    "  FROM information_states"
    "), split2 AS ("
    "  SELECT substr(rest, 1, instr(rest, '|') - 1) AS board_text,"
    "         substr(rest, instr(rest, '|') + 1) AS event_text"
    "  FROM split1"
    "), measured AS ("
    "  SELECT 4"
    "    + CASE WHEN board_text = '' THEN 0"
    "           ELSE length(board_text) - length(replace(board_text, ',', '')) + 1 END"
    "    + CASE WHEN event_text = '' THEN 0"
    "           ELSE 4 * (length(event_text) - length(replace(event_text, ',', '')) + 1) END"
    "      AS key_words"
    "  FROM split2"
    ")"
    "SELECT COUNT(*), COALESCE(SUM(key_words), 0), COALESCE(MAX(key_words), 0) FROM measured";

ArtifactProbe probe_path(const std::filesystem::path& path, const LoadOptions& options) {
  OpenedArtifact opened = open_validated(path, options);
  dt::Db& db = opened.db;

  ArtifactProbe probe;
  if (opened.schema_version == kArtifactSchemaVersionV2) {
    probe.manifest = read_manifest_v2(db);
  } else {
    probe.manifest = read_manifest(db);
  }
  check(probe.manifest.kind == ArtifactKind::Policy, ArtifactErrorKind::InvalidArgument,
        "resident probe supports published policies only: " + path.string());
  check(probe.manifest.run_status == RunStatus::Complete, ArtifactErrorKind::InvalidArgument,
        "cannot probe a resource-limited policy");

  if (opened.schema_version == kArtifactSchemaVersionV2) {
    // RFC 0009 D4: materialize the seat-generic identity. The resident path
    // projects a two-seat flop-rooted v2 source onto its heads-up view; the
    // probe itself only carries the identity and SQL aggregates.
    GameDef game = read_game_v2(db);
    read_game_seats(db, game);
    probe.sizes = read_sizes_v2(db);
    probe.ranges = read_ranges_v2(db, game.player_count);
    validate_game_v2(game);
    probe.game_def = game;
    read_auxiliary_tables_v2(db, game.player_count);
  } else {
    probe.game = read_game(db);
    read_sizes(db, probe.game);
    read_ranges(db, probe.game);
    validate_game(probe.game);
    read_auxiliary_tables(db);
  }

  dt::Stmt states(db.get(), "SELECT COUNT(*) FROM information_states", "probe information states");
  check(states.step("probe information states"), ArtifactErrorKind::Corrupt,
        "information state aggregate returned no row");
  probe.information_sets = static_cast<std::uint64_t>(states.column_i64(0));
  check(probe.information_sets > 0, ArtifactErrorKind::InvalidSchema,
        "cannot probe an empty policy");
  check(probe.information_sets == probe.manifest.information_sets, ArtifactErrorKind::InvalidSchema,
        "probe information_sets disagrees with the manifest");

  dt::Stmt actions(db.get(), "SELECT COUNT(*) FROM actions", "probe actions");
  check(actions.step("probe actions"), ArtifactErrorKind::Corrupt,
        "action aggregate returned no row");
  probe.action_count = static_cast<std::uint64_t>(actions.column_i64(0));
  check(probe.action_count >= probe.information_sets, ArtifactErrorKind::InvalidSchema,
        "probe found a state without actions");

  dt::Stmt keys(db.get(), kProbeKeyAggregateSql, "probe key aggregate");
  check(keys.step("probe key aggregate"), ArtifactErrorKind::Corrupt,
        "key aggregate returned no row");
  const std::int64_t counted_states = keys.column_i64(0);
  check(static_cast<std::uint64_t>(counted_states) == probe.information_sets,
        ArtifactErrorKind::InvalidSchema, "probe key aggregate count disagrees");
  probe.total_key_words = static_cast<std::uint64_t>(keys.column_i64(1));
  probe.max_key_words = static_cast<std::uint64_t>(keys.column_i64(2));
  check(probe.total_key_words >= probe.information_sets * 4, ArtifactErrorKind::InvalidSchema,
        "probe key word aggregate is impossibly small");

  probe.file_bytes = opened.file_bytes;
  probe.sha256 = opened.digest;
  probe.sha256_hex = dt::to_hex(opened.digest);
  return probe;
}

}  // namespace

void create_checkpoint(const std::filesystem::path& path, const TrainingResult& result,
                       const TrainingRows& rows, const CheckpointProvenance& provenance) {
  ArtifactManifest manifest =
      manifest_for(result, provenance, ArtifactKind::Checkpoint, provenance.validation);
  (void)validate_content(result, rows, ArtifactKind::Checkpoint);
  std::error_code ec;
  if (std::filesystem::exists(path, ec))
    fail(ArtifactErrorKind::AlreadyExists, "checkpoint already exists: " + path.string());
  dt::Db db;
  open_writer(db, path.string(), true);
  try {
    write_all(db, manifest, result, rows, true);
  } catch (...) {
    db.close();
    std::error_code ec;
    std::filesystem::remove(path, ec);
    std::filesystem::remove(std::filesystem::path(path.string() + "-journal"), ec);
    std::filesystem::remove(std::filesystem::path(path.string() + "-wal"), ec);
    throw;
  }
}

void create_checkpoint(const std::filesystem::path& path, const SeatTrainingResult& result,
                       const SeatCheckpointProvenance& provenance) {
  ArtifactManifest manifest =
      manifest_for_v2(result, provenance, ArtifactKind::Checkpoint, provenance.validation);
  validate_content_v2(result);
  std::error_code ec;
  if (std::filesystem::exists(path, ec))
    fail(ArtifactErrorKind::AlreadyExists, "checkpoint already exists: " + path.string());
  dt::Db db;
  open_writer(db, path.string(), true);
  try {
    write_all_v2(db, manifest, result);
  } catch (...) {
    db.close();
    std::error_code ec;
    std::filesystem::remove(path, ec);
    std::filesystem::remove(std::filesystem::path(path.string() + "-journal"), ec);
    std::filesystem::remove(std::filesystem::path(path.string() + "-wal"), ec);
    throw;
  }
}

void commit_checkpoint(const std::filesystem::path& path, const TrainingResult& result,
                       const TrainingRows& rows, const CheckpointProvenance& provenance) {
  validate_content(result, rows, ArtifactKind::Checkpoint);

  // Read the stored identity before opening the replacement transaction so a
  // mismatch never enters a write transaction. This probe uses the same
  // immutable, sidecar-rejecting reader as load_artifact: a checkpoint with
  // stale WAL/SHM/journal state is not committable and must be recovered or
  // rejected first; our own committed checkpoints never leave sidecars.
  ArtifactManifest stored_manifest;
  {
    const std::uint64_t probe_bytes = dt::file_size_required(path, kDefaultMaxArtifactBytes);
    dt::validate_database_header(path, probe_bytes);
    dt::reject_sqlite_sidecars(path);
    dt::ensure_reader_heap_bound();
    dt::Db probe;
    dt::open_immutable_reader(probe, path);
    stored_manifest = read_manifest(probe);
    HeadsUpGame stored_game = read_game(probe);
    read_sizes(probe, stored_game);
    read_ranges(probe, stored_game);
    if (stored_manifest.kind != ArtifactKind::Checkpoint)
      fail(ArtifactErrorKind::IdentityMismatch, "cannot commit iterations into a published policy");
    if (!same_game(stored_game, result.policy.game()))
      fail(ArtifactErrorKind::IdentityMismatch,
           "checkpoint game identity differs; refusing resume");
    if (stored_manifest.algorithm_revision != provenance.algorithm_revision ||
        stored_manifest.numeric_profile != kNumericProfileV1 ||
        stored_manifest.prng_identifier != provenance.prng_identifier ||
        stored_manifest.seed != result.seed)
      fail(ArtifactErrorKind::IdentityMismatch,
           "checkpoint revision or PRNG identity differs; refusing resume");
  }

  ArtifactManifest manifest =
      manifest_for(result, provenance, ArtifactKind::Checkpoint, stored_manifest.validation);

  dt::Db db;
  open_writer(db, path.string(), false);
  db.exec("BEGIN IMMEDIATE;", "begin checkpoint commit");
  try {
    db.exec("DELETE FROM information_states;", "replace policy states");
    insert_policy(db, result, rows, true);
    dt::Stmt update(db.get(),
                    "UPDATE manifest SET completed_iterations = ?,"
                    " prng_state_hex = ?, run_status = ?, nodes = ?,"
                    " information_sets = ?, accounted_bytes = ?, engine_revision = ?"
                    " WHERE id = 1",
                    "update manifest");
    int i = 1;
    update.bind_u64_chips(i++, manifest.completed_iterations);
    update.bind_text(i++, dt::u64_to_hex(manifest.prng_state));
    update.bind_text(i++, run_status_text(manifest.run_status));
    update.bind_u64_chips(i++, manifest.nodes);
    update.bind_u64_chips(i++, manifest.information_sets);
    update.bind_u64_chips(i++, manifest.accounted_bytes);
    update.bind_text(i++, manifest.engine_revision);
    (void)update.step("update manifest");
    db.exec("COMMIT;", "commit checkpoint iteration");
  } catch (...) {
    // See write_all: an I/O failure may already have auto-rolled the
    // transaction back; never mask the original error with a failed ROLLBACK.
    try {
      db.exec("ROLLBACK;", "rollback checkpoint iteration");
    } catch (...) {
    }
    throw;
  }
}

LoadedArtifact load_artifact(const std::filesystem::path& path, const LoadOptions& options) {
  return load_path(path, options);
}

ArtifactProbe probe_artifact(const std::filesystem::path& path, const LoadOptions& options) {
  return probe_path(path, options);
}

PublishedPolicy publish_policy(const std::filesystem::path& checkpoint_path,
                               const std::filesystem::path& destination,
                               const std::string& engine_revision) {
  const LoadedArtifact source = load_artifact(checkpoint_path);
  const bool is_v2 = source.bundle.nseat.has_value();
  check(source.bundle.manifest.kind == ArtifactKind::Checkpoint, ArtifactErrorKind::InvalidArgument,
        "publish source must be a checkpoint: " + checkpoint_path.string());
  check(source.bundle.manifest.run_status == RunStatus::Complete,
        ArtifactErrorKind::InvalidArgument, "cannot publish a resource-limited checkpoint");
  if (is_v2) {
    check(!source.bundle.nseat->policy.rows().empty(), ArtifactErrorKind::InvalidArgument,
          "cannot publish an empty policy");
  } else {
    check(!source.bundle.result.policy.rows().empty(), ArtifactErrorKind::InvalidArgument,
          "cannot publish an empty policy");
  }

  std::error_code ec;
  const std::filesystem::path parent = destination.parent_path();
  check(!parent.empty() && std::filesystem::is_directory(parent, ec), ArtifactErrorKind::Io,
        "destination directory does not exist: " + parent.string());
  if (std::filesystem::exists(destination, ec))
    fail(ArtifactErrorKind::AlreadyExists, "generation already exists: " + destination.string());

  const std::filesystem::path temp = dt::make_unique_temp(parent);
  dt::Db db;
  bool linked = false;
  try {
    open_writer(db, temp.string(), true);
    ArtifactManifest manifest;
    if (is_v2) {
      SeatCheckpointProvenance provenance;
      provenance.algorithm_revision = source.bundle.manifest.algorithm_revision;
      provenance.prng_identifier = source.bundle.manifest.prng_identifier;
      provenance.engine_revision = engine_revision;
      manifest = manifest_for_v2(*source.bundle.nseat, provenance, ArtifactKind::Policy,
                                 ValidationState::Validated);
      validate_content_v2(*source.bundle.nseat);
      write_all_v2(db, manifest, *source.bundle.nseat);
    } else {
      CheckpointProvenance provenance;
      provenance.algorithm_revision = source.bundle.manifest.algorithm_revision;
      provenance.prng_identifier = source.bundle.manifest.prng_identifier;
      provenance.engine_revision = engine_revision;
      manifest = manifest_for(source.bundle.result, provenance, ArtifactKind::Policy,
                              ValidationState::Validated);
      write_all(db, manifest, source.bundle.result, source.bundle.rows, false);
    }
    db.close();

    dt::fsync_file(temp);
    dt::make_read_only(temp);
    dt::fsync_file(temp);

    dt::exclusive_hard_link(temp, destination);
    linked = true;
    dt::fsync_directory(parent);
    std::filesystem::remove(temp, ec);
    dt::fsync_directory(parent);

    const std::uint64_t final_size = dt::file_size_required(destination, kDefaultMaxArtifactBytes);
    const Sha256Digest digest = dt::sha256_file(destination, kDefaultMaxArtifactBytes);

    // Independently reopen the linked generation read-only with the digest.
    LoadOptions verify_options;
    verify_options.expected_sha256 = digest;
    const LoadedArtifact verified = load_artifact(destination, verify_options);
    check(verified.bundle.manifest.kind == ArtifactKind::Policy, ArtifactErrorKind::Corrupt,
          "published generation is not tagged policy");

    PublishedPolicy published;
    published.path = destination;
    published.sha256 = digest;
    published.sha256_hex = dt::to_hex(digest);
    published.file_bytes = final_size;
    return published;
  } catch (...) {
    db.close();
    if (!linked)
      std::filesystem::remove(temp, ec);
    else
      std::filesystem::remove(destination, ec);
    throw;
  }
}

}  // namespace bs::artifacts
