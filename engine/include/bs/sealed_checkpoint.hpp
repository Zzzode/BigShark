#ifndef BS_SEALED_CHECKPOINT_HPP
#define BS_SEALED_CHECKPOINT_HPP

// RFC 0028: sealed warm-start checkpoints.
//
// A flop-library warm-start checkpoint stores the trainer's own table, keyed by
// NSeatInformationKey {own_card_bucket, node_index}, instead of expanding every
// (bucket, node) row onto every concrete hole-card combo. The expansion is a
// 392x redundancy on the six-way full-range class (1,100,736 concrete states
// over 2,808 distinct weight vectors), so this file is ~187 KB where the
// schema-v3 SQLite checkpoint was 347.8 MB.
//
// The format carries a magic, a version, an exact payload length, a payload
// digest, both abstraction digests, and the node and bucket caps, so a torn or
// foreign file is rejected before any trainer state exists. It links no
// database.

#include <bs/abstraction.hpp>
#include <bs/nseat_trainer.hpp>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>

namespace bs::artifacts {

// The sealed file's fixed header length. Payload offsets are relative to the
// payload start, not to the file.
inline constexpr std::uint32_t kSealedCheckpointHeaderBytes = 184;

// Version of the BSWARM01 layout. Bumped only by a redesign of this file.
inline constexpr std::uint32_t kSealedCheckpointVersion = 1;

// The canonical provenance string for a numeric trainer revision, or nullptr
// when this build does not know the revision. The header stores both a numeric
// revision (which the trainer consumes) and this 64-byte string (which carries
// the refusal message). A known revision must agree with its string; an
// unknown one is structurally valid but semantically incompatible, so the
// caller refuses it instead of treating it as corruption.
const char* sealed_revision_string(std::uint32_t algorithm_revision_id);

// Identity and iteration state written alongside the rows. `rows` is the
// trainer's master table, moved in and out without conversion.
struct SealedCheckpointState {
  // The trainer's numeric revision, and the provenance string stored beside
  // it. A writer fills both (sealed_revision_string derives the string for a
  // known revision); a loader reports both and leaves the compatibility
  // judgement to the caller.
  std::uint32_t algorithm_revision_id = solver::kNSeatAlgorithmRevision;
  std::string algorithm_revision;
  std::uint64_t abstraction_digest = 0;
  // The card-abstraction identity. Only its digest is stored: an
  // AbstractionId's name/version/parameters cannot be reconstructed from the
  // file, so carrying a half-built struct would invite an identity comparison
  // that silently compares defaults. nullopt means "no card abstraction".
  std::optional<std::uint64_t> card_abstraction_digest;
  std::uint64_t completed_iterations = 0;
  std::uint64_t batch_size = 1;
  std::uint64_t completed_epochs = 0;
  // Upper bounds the file was written under: every node index must be below
  // node_cap and every bucket below bucket_cap. Loading rejects rows outside
  // them, which is how a file written for another tree shape fails closed.
  std::uint32_t node_cap = 0;
  std::uint32_t bucket_cap = 0;
  // The card kind the buckets were computed under.
  abstraction::CardBucketKind key_kind = solver::kNSeatCardKind;
  // RFC 0030: BSWARM02 whole-tree shape witness (FNV-1a 64 over every node's
  // index, kind, and ordered menu). A resume hard-refuses a mismatch before
  // rows are adopted. Required on save and load; absent only when a caller is
  // constructing state in memory.
  std::optional<std::uint64_t> tree_shape_digest;
  std::map<solver::NSeatInformationKey, solver::NSeatRawRow> rows;
};

// Thrown for every structural or identity failure. The message names the
// rejected field so a caller can log it and tell discardable corruption apart
// from a hard identity refusal.
class SealedCheckpointError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

// Write `state` to `path`, atomically: a unique temp file in the same
// directory is filled, fsynced, and renamed over the destination, followed by
// a directory fsync. A failure leaves any previous file at `path` intact and
// removes the temp file.
//
// Throws SealedCheckpointError when the state cannot be represented: a bucket
// outside key_kind's range, a node index at or above node_cap, a row whose
// action/regret/sum lengths disagree, a non-finite value, a revision string
// that does not fit 64 bytes, or an I/O failure.
void save_sealed_checkpoint(const std::filesystem::path& path, const SealedCheckpointState& state);

// Read and validate the file at `path`, returning the trainer state.
//
// Throws SealedCheckpointError naming the failed check: magic, version,
// header length, payload length, payload digest, revision string against
// numeric revision, key_kind, bucket or node bounds, dimension mismatches,
// non-finite or negative accumulators, or action targets above the packed
// 16-bit ceiling.
SealedCheckpointState load_sealed_checkpoint(const std::filesystem::path& path);

// Validate only the header of the file at `path`, without decoding the
// payload. This is what a resume preflight uses: it separates "torn, discard
// and retrain" from "well-formed but incompatible, refuse". Returns the
// header fields with an empty `rows`.
SealedCheckpointState inspect_sealed_checkpoint(const std::filesystem::path& path);

}  // namespace bs::artifacts

#endif  // BS_SEALED_CHECKPOINT_HPP
