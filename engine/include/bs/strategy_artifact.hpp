// RFC 0005 Stage 5 strategy artifact boundary.
//
// This is the only public surface of `bigshark_artifacts`. It serializes RFC
// 0004 heads-up training checkpoints and immutable, validated policy exports.
// Every function exchanges engine domain records from bs/heads_up_solver.hpp
// and bs/heads_up.hpp; SQLite and OpenSSL handles, headers, and statements are
// private to the implementation and never appear here. The library is offline
// only: no host, service, or live-decision path links it in this stage.
//
// Artifact v1: SQLite database, application_id 0x42534754 ("BSGT"),
// user_version 1, STRICT tables, foreign keys, prepared statements, and the
// RFC 0005 canonical ASCII information key. Checkpoints are mutable files
// updated by one transaction per complete iteration (rollback journaling,
// synchronous=FULL). A policy export is a fresh, read-only file published to a
// new generation path by an atomic same-filesystem link, identified by
// SHA-256 of its bytes. Readers are bounded and treat the file as untrusted.
#pragma once

#include <array>
#include <bs/abstraction.hpp>
#include <bs/game_definition.hpp>
#include <bs/heads_up_solver.hpp>
#include <bs/seat_policy.hpp>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace bs::artifacts {

// "BSGT" stored in the SQLite application_id header word.
inline constexpr std::uint32_t kArtifactApplicationId = 0x42534754u;
inline constexpr std::uint32_t kArtifactSchemaVersion = 1;
// RFC 0004 numeric profile: chip amounts fit exactly in 53-bit mantissa.
inline constexpr std::uint64_t kMaxStoredChips = (std::uint64_t{1} << 53) - 1;
inline constexpr std::uint64_t kDefaultMaxArtifactBytes = std::uint64_t{1} << 33;  // 8 GiB
inline constexpr int kArtifactPageCacheKiB = 64 * 1024;                            // 64 MiB
// Process-wide hard ceiling on SQLite heap use while parsing an untrusted
// artifact. 512 MiB is far below physical memory and far above the parse
// footprint of any legitimate artifact, but bounds view/trigger "zip bomb"
// amplification before the unbounded 8 GiB file size would allow tens of
// gigabytes of parser heap. The vendored SQLite 3.50 amalgamation exposes only
// the process-global sqlite3_hard_heap_limit64 (no per-connection variant),
// and bigshark_artifacts is the only in-process consumer of this privately
// linked SQLite copy.
inline constexpr std::int64_t kReaderHeapBoundBytes = 512LL * 1024 * 1024;
inline constexpr double kProbabilitySumTolerance = 1e-12;

inline constexpr const char* kDefaultAlgorithmRevision = "rfc0004-rev1-full-kSimple-prng1";
inline constexpr const char* kFullTraversalPrngIdentifier = "none";
inline constexpr const char* kSampledPrngIdentifier = "splitmix64-rev1";
inline constexpr const char* kDefaultEngineRevision = "bigshark-cpp/rfc0005-stage5";
inline constexpr const char* kNumericProfileV1 =
    "ieee754-binary64;chips<=9007199254740991;sqlite-real;key-rev1";
inline constexpr const char* kRulesIdentifierV1 = "rfc0004-heads-up-flop-v1";
inline constexpr const char* kUtilityIdentifierV1 = "zero-sum-chip-net-v1";

// RFC 0009 D3: schema v2 is the seat-generic profile. The DDL set, key grammar,
// rules identifier, and numeric profile are all version-keyed; a v1 artifact
// loads with an unchanged digest and a v2 artifact is rejected by a v1 reader.
inline constexpr std::uint32_t kArtifactSchemaVersionV2 = 2;
inline constexpr const char* kNumericProfileV2 =
    "ieee754-binary64;chips<=9007199254740991;sqlite-real;key-rev2";
inline constexpr const char* kRulesIdentifierV2 = "rfc0009-unified-flop-v1";
// The seat-generic trainer's declared algorithm: sampled external-sampling
// MCCFR with the kFull own-reach-weighted average and splitmix64.
inline constexpr const char* kDefaultAlgorithmRevisionV2 = "rfc0009-nseat-rev1-sampled-splitmix64";

enum class ArtifactKind { Checkpoint, Policy };
enum class ValidationState { Unvalidated, Validated, Rejected };
enum class RunStatus { Complete, ResourceLimit };

enum class ArtifactErrorKind {
  Io,                  // Filesystem, lock, fsync, or path failure.
  Sqlite,              // SQLite reported an error; sqlite_code is populated.
  Corrupt,             // Structural corruption/truncation/integrity failure.
  UnsupportedVersion,  // Wrong application_id or user_version.
  InvalidSchema,       // Missing/extra/non-STRICT objects or malformed text.
  IdentityMismatch,    // Resume/publish identity differs canonically.
  InvalidValue,        // Probability, finiteness, range, or integer failure.
  DigestMismatch,      // SHA-256 differs from the expected digest.
  FileTooLarge,        // File exceeds the declared bound.
  CapacityExceeded,    // Bounded parse/heap allocation limit hit.
  AlreadyExists,       // Refusing to overwrite a generation or checkpoint.
  InvalidArgument,     // Caller-supplied records are inconsistent.
};

class ArtifactError : public std::runtime_error {
 public:
  ArtifactError(ArtifactErrorKind kind, std::string message, int sqlite_code = 0)
      : std::runtime_error(std::move(message)), kind_(kind), sqlite_code_(sqlite_code) {}

  ArtifactErrorKind kind() const noexcept { return kind_; }
  int sqlite_code() const noexcept { return sqlite_code_; }

 private:
  ArtifactErrorKind kind_;
  int sqlite_code_;
};

using Sha256Digest = std::array<std::uint8_t, 32>;

struct ArtifactManifest {
  ArtifactKind kind = ArtifactKind::Checkpoint;
  std::string algorithm_revision = kDefaultAlgorithmRevision;
  std::uint32_t information_key_revision = 1;
  std::string numeric_profile = kNumericProfileV1;
  ValidationState validation = ValidationState::Unvalidated;
  std::string engine_revision = kDefaultEngineRevision;
  std::uint64_t completed_iterations = 0;
  std::string prng_identifier = kSampledPrngIdentifier;
  std::uint64_t seed = 0;
  std::uint64_t prng_state = 0;
  RunStatus run_status = RunStatus::Complete;
  std::size_t nodes = 0;
  std::size_t information_sets = 0;
  std::size_t accounted_bytes = 0;
  // RFC 0009 D3 (schema v2 only): the declared action abstraction and terminal
  // depth. The v1 writer/reader leave these at their defaults; the v2 writer
  // derives them from the SeatTrainingResult and the v2 reader reconstructs the
  // SeatPolicy's action_id from them.
  std::string abstraction_name;
  std::uint32_t abstraction_version = 0;
  std::string abstraction_parameters;
  std::uint64_t abstraction_digest = 0;
  poker::TerminalDepth terminal_depth = poker::TerminalDepth::River;
};

// Raw per-abstract-action training state. The policy stores normalized
// probabilities; the checkpoint additionally stores cumulative regret and the
// unnormalized average weight so a future solver resume entry can continue
// without recomputing completed iterations.
struct TrainingRow {
  std::vector<poker::Action> actions;
  std::vector<double> regrets{};
  std::vector<double> average_weights{};
};

using TrainingRows = std::map<solver::InformationKey, TrainingRow>;

// A complete checkpoint or policy artifact in domain-record form. `result`
// carries the reconstructed HeadsUpGame (through result.policy.game()), the
// immutable policy, counters, seed, and PRNG state. `rows` is empty for a
// published policy and complete for a checkpoint.
//
// RFC 0009 D3 (schema v2): a v2 artifact populates `nseat` instead of `result`
// and `rows`. The two arms are mutually exclusive — a v1 load leaves `nseat`
// nullopt and a v2 load leaves `result`/`rows` at their defaults. Callers that
// need the seat-generic policy dispatch on `nseat.has_value()`.
struct ArtifactBundle {
  ArtifactManifest manifest;
  solver::TrainingResult result;
  TrainingRows rows;
  std::optional<solver::SeatTrainingResult> nseat;
};

struct CheckpointProvenance {
  std::string algorithm_revision = kDefaultAlgorithmRevision;
  std::string prng_identifier = kSampledPrngIdentifier;
  std::string engine_revision = kDefaultEngineRevision;
  ValidationState validation = ValidationState::Unvalidated;
};

// RFC 0009 D3: provenance for a seat-generic (schema v2) checkpoint. Mirrors
// CheckpointProvenance with the v2 algorithm default.
struct SeatCheckpointProvenance {
  std::string algorithm_revision = kDefaultAlgorithmRevisionV2;
  std::string prng_identifier = kSampledPrngIdentifier;
  std::string engine_revision = kDefaultEngineRevision;
  ValidationState validation = ValidationState::Unvalidated;
};

// Create a brand-new checkpoint containing one complete iteration's state. The
// path must not exist. The complete write is one transaction. Throws
// ArtifactError; on failure no readable artifact is left at the path.
void create_checkpoint(const std::filesystem::path& path, const solver::TrainingResult& result,
                       const TrainingRows& rows, const CheckpointProvenance& provenance = {});

// RFC 0009 D3: create a brand-new schema-v2 checkpoint from a seat-generic
// training result. The path must not exist. The complete write is one
// transaction. Throws ArtifactError; on failure no readable artifact is left at
// the path. Only rooted flop/turn/river games (board_size 3..5) are accepted;
// a preflop profile is rejected until one ships.
void create_checkpoint(const std::filesystem::path& path, const solver::SeatTrainingResult& result,
                       const SeatCheckpointProvenance& provenance = {});

// Replace the mutable training/policy state of an existing checkpoint with the
// next complete iteration, in one transaction. The full canonical identity
// (root board and street, button, blinds, stacks, contributions, pot, utility,
// every range weight, the ordered rational sizing schedule, algorithm/key/
// numeric revisions, PRNG identifier, and seed) must match the stored identity
// exactly; otherwise IdentityMismatch is returned before any write.
void commit_checkpoint(const std::filesystem::path& path, const solver::TrainingResult& result,
                       const TrainingRows& rows, const CheckpointProvenance& provenance = {});

struct LoadOptions {
  // Files larger than this are rejected before SQLite touches the file.
  std::uint64_t max_file_bytes = kDefaultMaxArtifactBytes;
  // When set, the SHA-256 of the file bytes must equal this digest before the
  // database is opened.
  std::optional<Sha256Digest> expected_sha256;
};

struct LoadedArtifact {
  ArtifactBundle bundle;
  Sha256Digest sha256{};
  std::string sha256_hex;
  std::uint64_t file_bytes = 0;
};

// Open a checkpoint or published policy read-only, verify size and (when
// given) digest, pin the schema version, reject unknown objects, run
// integrity/foreign-key checks, and validate every probability and REAL. All
// policy rows are eagerly loaded into domain records; Stage 6 owns bounded
// resident subset selection. The page cache is capped at 64 MiB; extension
// loading is compiled out and untrusted schemas are disabled.
LoadedArtifact load_artifact(const std::filesystem::path& path, const LoadOptions& options = {});

// Lightweight, fully validating startup probe for a published policy. It
// performs the same physical checks as load_artifact (size, digest, sidecar
// rejection, immutable open, application/schema version, canonical schema
// objects, integrity check) and materializes only the manifest, the game
// identity (root, ordered rational sizing schedule, bit-exact declared
// ranges), and SQL aggregates over the stored information states and
// actions. It never reconstructs policy rows, training rows, vectors of
// actions, or probabilities, so a large root can be budget-gated before the
// full eager load. The probe changes no Stage 5 read or publication
// behavior; callers that actually advertise the root still call
// load_artifact afterward.
struct ArtifactProbe {
  ArtifactManifest manifest;
  // Revision-1 identity. Default-constructed for a schema-v2 probe, where the
  // seat-generic fields below carry the identity instead.
  solver::HeadsUpGame game;
  // RFC 0009 D4 (schema v2 only): the seat-generic identity. Populated only
  // for a v2 probe. The resident path projects a two-seat flop-rooted v2
  // identity onto its heads-up view; turn/river-rooted and 3+-seat v2 sources
  // are refused there as LoadFailed until the state layer generalizes.
  std::optional<poker::GameDef> game_def;
  std::optional<std::vector<std::vector<solver::WeightedHand>>> ranges;
  std::optional<abstraction::SizeSchedule> sizes;
  std::uint64_t information_sets = 0;
  std::uint64_t action_count = 0;
  // Sum and maximum of stored canonical key lengths in 64-bit words.
  std::uint64_t total_key_words = 0;
  std::uint64_t max_key_words = 0;
  std::uint64_t file_bytes = 0;
  Sha256Digest sha256{};
  std::string sha256_hex;
};
ArtifactProbe probe_artifact(const std::filesystem::path& path, const LoadOptions& options = {});

struct PublishedPolicy {
  std::filesystem::path path;
  Sha256Digest sha256{};
  std::string sha256_hex;
  std::uint64_t file_bytes = 0;
};

// Validate a checkpoint and publish its policy as a fresh immutable SQLite
// generation. The destination must not exist. A unique temporary file is
// created in the destination directory (same filesystem), filled in one
// transaction, validated, closed, fsynced, and linked into place with an
// exclusive link followed by a directory fsync; the temporary name is removed.
// The published file is read-only and its training table is empty. The
// SHA-256
// digest and byte size of the final file are verified before return.
PublishedPolicy publish_policy(const std::filesystem::path& checkpoint_path,
                               const std::filesystem::path& destination,
                               const std::string& engine_revision = kDefaultEngineRevision);

// SHA-256 of a file's bytes, streaming within the artifact reader bounds.
std::string sha256_file_hex(const std::filesystem::path& path,
                            std::uint64_t max_file_bytes = kDefaultMaxArtifactBytes);

// Encode an information key's public portion as the RFC 0005 canonical ASCII
// key `street|board_ids|events` (actor and own cards live in the
// information_states columns). Public deals are represented by board_ids, so
// revision-1 events contain only f/x/c/b/r triples; event streets are assigned
// by the RFC 0004 round-end grammar.
//
// `revision` selects the key grammar and has NO default: every caller declares
// which grammar it writes. Revision 1 is frozen (actor 0..1, board_count 3..5,
// street 0..2). Revision 2 admits the seat-generic grammar (actor 0..9,
// board_count in {0,3,4,5} where 0 is the preflop form, street 0..3 with
// street 3 denoting preflop). Throws ArtifactError(InvalidArgument) on any
// other revision.
std::string encode_public_key(const solver::InformationKey& key, std::uint32_t revision);

// Inverse of encode_public_key. `player`, `card0`, and `card1` come from the
// information_states columns (sorted own combo, 0 <= card0 < card1 < 52).
// `revision` selects the grammar and has NO default; it must match the
// artifact's `info_key_revision`. Revision 1 decodes only the frozen heads-up
// grammar; revision 2 additionally admits seats 0..9 and the preflop form.
// Throws ArtifactError(InvalidSchema) on anything that is not a canonical key
// for the selected revision's grammar.
solver::InformationKey decode_public_key(const std::string& text, std::uint32_t revision,
                                         int player, int card0, int card1);

}  // namespace bs::artifacts
