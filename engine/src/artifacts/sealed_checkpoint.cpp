// RFC 0028: the BSWARM01 sealed warm-start checkpoint codec.
//
// See bs/sealed_checkpoint.hpp for the format contract and the rationale. This
// file is deliberately self-contained: it links no database and performs no
// tree walk, so a checkpoint round trip is a direct encode/decode of the
// trainer's own NSeatInformationKey -> NSeatRawRow table.

#include <array>
#include <bs/mmap_library.hpp>  // resident::CompactAction
#include <bs/sealed_checkpoint.hpp>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string_view>
#include <utility>
#include <vector>

#include "artifact_internal.hpp"

namespace bs::artifacts {

// Revision 2 is the only revision this build can produce or resume.
const char* const kRevisionTwoString = "rfc0016-nseat-rev2-frozen-batch-1-sampled-splitmix64";
// Revision 1 is not producible by this build, but a sealed file may carry it
// (a checkpoint written before a revision bump). Its string is known so the
// codec can accept the structure and let the caller refuse the semantics.
const char* const kRevisionOneString = "rfc0009-nseat-rev1-sampled-splitmix64";

const char* sealed_revision_string(std::uint32_t algorithm_revision_id) {
  switch (algorithm_revision_id) {
    case solver::kNSeatAlgorithmRevision:
      return kRevisionTwoString;
    case 1:
      return kRevisionOneString;
    default:
      return nullptr;
  }
}

namespace {

using solver::NSeatInformationKey;
using solver::NSeatRawRow;

constexpr char kMagic[8] = {'B', 'S', 'W', 'A', 'R', 'M', '0', '1'};

// The packed action target is 16 bits, so a checkpoint cannot carry a menu
// whose aggressive target exceeds it. This is the same ceiling BSFLIB02
// accepts, and it is checked rather than truncated.
constexpr std::uint64_t kMaxPackedTarget = 0xffffu;

constexpr std::size_t kAlgorithmRevisionChars = 64;

const char* const kRevisionTwoString = "rfc0016-nseat-rev2-frozen-batch-1-sampled-splitmix64";
// Revision 1 is not producible by this build, but a sealed file may carry it
// (a checkpoint written before a revision bump). Its string is known so the
// codec can accept the structure and let the caller refuse the semantics.
const char* const kRevisionOneString = "rfc0009-nseat-rev1-sampled-splitmix64";

// Header field offsets. Every field is written at its offset so the layout in
// the RFC and the bytes on disk cannot drift apart.
constexpr std::size_t kOffMagic = 0;
constexpr std::size_t kOffVersion = 8;
constexpr std::size_t kOffHeaderBytes = 12;
constexpr std::size_t kOffKeyKind = 16;
constexpr std::size_t kOffAlgorithmRevisionId = 24;
constexpr std::size_t kOffCompletedIterations = 32;
constexpr std::size_t kOffBatchSize = 40;
constexpr std::size_t kOffCompletedEpochs = 48;
constexpr std::size_t kOffAbstractionDigest = 56;
constexpr std::size_t kOffCardAbstractionDigest = 64;
constexpr std::size_t kOffCardAbstractionPresent = 72;
constexpr std::size_t kOffNodeCap = 80;
constexpr std::size_t kOffBucketCap = 84;
constexpr std::size_t kOffKeyCount = 88;
constexpr std::size_t kOffTotalActions = 96;
constexpr std::size_t kOffPayloadBytes = 104;
constexpr std::size_t kOffPayloadDigest = 112;
constexpr std::size_t kOffAlgorithmRevision = 120;

static_assert(kOffAlgorithmRevision + kAlgorithmRevisionChars == kSealedCheckpointHeaderBytes,
              "the fixed header length must cover the revision string");

// The digest is written only when an identity is present. The present flag
// keeps an absent identity distinct from a present one whose digest happens to
// be zero.
std::uint64_t card_digest_of(const std::optional<std::uint64_t>& digest) {
  return digest.value_or(0);
}

void put_u8(std::vector<std::uint8_t>& buf, std::size_t at, std::uint8_t value) {
  buf[at] = value;
}

void put_u32(std::vector<std::uint8_t>& buf, std::size_t at, std::uint32_t value) {
  for (int i = 0; i < 4; ++i)
    buf[at + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>((value >> (i * 8)) & 0xffu);
}

void put_u64(std::vector<std::uint8_t>& buf, std::size_t at, std::uint64_t value) {
  for (int i = 0; i < 8; ++i)
    buf[at + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>((value >> (i * 8)) & 0xffu);
}

std::uint8_t get_u8(std::span<const std::uint8_t> buf, std::size_t at) {
  return buf[at];
}

std::uint32_t get_u32(std::span<const std::uint8_t> buf, std::size_t at) {
  std::uint32_t value = 0;
  for (int i = 0; i < 4; ++i)
    value |= static_cast<std::uint32_t>(buf[at + static_cast<std::size_t>(i)]) << (i * 8);
  return value;
}

std::uint64_t get_u64(std::span<const std::uint8_t> buf, std::size_t at) {
  std::uint64_t value = 0;
  for (int i = 0; i < 8; ++i)
    value |= static_cast<std::uint64_t>(buf[at + static_cast<std::size_t>(i)]) << (i * 8);
  return value;
}

std::uint64_t fnv1a_bytes(std::span<const std::uint8_t> bytes) {
  std::uint64_t hash = 0xcbf29ce484222325ULL;
  for (std::uint8_t byte : bytes) {
    hash ^= byte;
    hash *= 0x100000001b3ULL;
  }
  return hash;
}

[[noreturn]] void fail(const std::string& what) {
  throw SealedCheckpointError(what);
}

// The bucket ceiling for a card kind, matching abstraction::card_bucket's real
// range. These are not interchangeable guesses: with a board present,
// Preflop169 *falls back to* CategoryTiersV1, so its postflop buckets are the
// category values 1..9, not 0..168. Identity returns the raw evaluator score,
// whose range is the evaluator's own encoding, so it cannot be bounded here
// and is rejected rather than approximated.
std::uint32_t bucket_ceiling(abstraction::CardBucketKind kind) {
  switch (kind) {
    case abstraction::CardBucketKind::CategoryTiersV1:
      return 16;  // (score >> 20) & 0xF
    case abstraction::CardBucketKind::Preflop169:
      return 169;  // preflop 0..168; the postflop fallback is a subset
    case abstraction::CardBucketKind::Identity:
      fail(
          "sealed checkpoint: the Identity card kind is not supported; its bucket "
          "values are evaluator scores with no declared range");
  }
  fail("sealed checkpoint: unknown card bucket kind");
}

// Validate the in-memory state before any byte is written. This is the writer
// side of G5: a row whose three arrays disagree, a non-finite accumulator, or a
// key outside the declared caps is a producer bug and is refused rather than
// encoded.
void validate_state(const SealedCheckpointState& state) {
  if (sealed_revision_string(state.algorithm_revision_id) == nullptr)
    fail("sealed checkpoint: cannot write unknown algorithm revision " +
         std::to_string(state.algorithm_revision_id));
  if (!state.algorithm_revision.empty() &&
      state.algorithm_revision != sealed_revision_string(state.algorithm_revision_id))
    fail("sealed checkpoint: provenance string disagrees with the numeric revision");
  if (state.node_cap == 0)
    fail("sealed checkpoint: node_cap must be positive");
  if (state.bucket_cap == 0)
    fail("sealed checkpoint: bucket_cap must be positive");
  const std::uint32_t ceiling = bucket_ceiling(state.key_kind);
  for (const auto& [key, row] : state.rows) {
    if (key.own_card_bucket >= state.bucket_cap || key.own_card_bucket >= ceiling)
      fail("sealed checkpoint: bucket " + std::to_string(key.own_card_bucket) +
           " is outside the declared range");
    if (key.node_index >= state.node_cap)
      fail("sealed checkpoint: node index " + std::to_string(key.node_index) +
           " is at or above node_cap");
    if (row.actions.size() != row.regrets.size() || row.actions.size() != row.sums.size())
      fail("sealed checkpoint: row arrays disagree in length");
    if (row.actions.size() > 0xffffu)
      fail("sealed checkpoint: action menu exceeds the packed action count");
    for (std::size_t a = 0; a < row.actions.size(); ++a) {
      const poker::Action& action = row.actions[a];
      if (static_cast<std::uint8_t>(action.type) > 4)
        fail("sealed checkpoint: action type is outside 0..4");
      if (action.target_total > kMaxPackedTarget)
        fail("sealed checkpoint: action target exceeds the 16-bit packed ceiling");
      if (!std::isfinite(row.regrets[a]))
        fail("sealed checkpoint: cumulative regret is not finite");
      if (!std::isfinite(row.sums[a]))
        fail("sealed checkpoint: average weight is not finite");
    }
  }
}

// Serialize the payload. Rows are emitted in the map's own order, which is the
// (bucket, node_index) ascending order the format specifies.
std::vector<std::uint8_t> encode_payload(const SealedCheckpointState& state,
                                         std::uint64_t& total_actions_out) {
  std::uint64_t total_actions = 0;
  for (const auto& [key, row] : state.rows) {
    (void)key;
    total_actions += row.actions.size();
  }
  total_actions_out = total_actions;

  const std::size_t key_count = state.rows.size();
  const std::size_t keys_bytes = key_count * (4 + 8);
  const std::size_t offsets_bytes = key_count * (4 + 4);
  const std::size_t visits_bytes = key_count * 8;
  const std::size_t actions_bytes = static_cast<std::size_t>(total_actions) * 3;
  const std::size_t doubles_bytes = static_cast<std::size_t>(total_actions) * 8 * 2;
  const std::size_t payload_bytes =
      keys_bytes + offsets_bytes + visits_bytes + actions_bytes + doubles_bytes;
  // Actions offsets are u32, so the payload must stay addressable by them.
  // A class this large would indicate a corpus-wide table was passed by
  // mistake, not a real single-flop training state.
  if (payload_bytes > 0xffffffffu)
    fail("sealed checkpoint: payload exceeds the 32-bit offset space");

  std::vector<std::uint8_t> payload;
  payload.resize(payload_bytes);
  std::size_t at = 0;

  // Keys, as the two arrays the format declares: bucket[N] then node_index[N].
  for (const auto& [key, row] : state.rows) {
    (void)row;
    put_u32(payload, at, key.own_card_bucket);
    at += 4;
  }
  for (const auto& [key, row] : state.rows) {
    (void)row;
    put_u64(payload, at, key.node_index);
    at += 8;
  }
  // Offsets are payload-relative byte offsets into the actions blob, which
  // starts after the keys/offsets/visits blocks. A reader validates the
  // implied run length against actions_count, since an offset alone is not
  // self-describing.
  const std::size_t action_blob_start = keys_bytes + offsets_bytes + visits_bytes;
  std::size_t action_cursor = action_blob_start;
  for (const auto& [key, row] : state.rows) {
    (void)key;
    put_u32(payload, at, static_cast<std::uint32_t>(action_cursor));
    at += 4;
    action_cursor += row.actions.size() * 3;
  }
  for (const auto& [key, row] : state.rows) {
    (void)key;
    put_u32(payload, at, static_cast<std::uint32_t>(row.actions.size()));
    at += 4;
  }
  // Visits.
  for (const auto& [key, row] : state.rows) {
    (void)key;
    put_u64(payload, at, row.visits);
    at += 8;
  }
  // Actions.
  for (const auto& [key, row] : state.rows) {
    (void)key;
    for (const poker::Action& action : row.actions) {
      payload[at] = static_cast<std::uint8_t>(action.type);
      const std::uint16_t target = static_cast<std::uint16_t>(action.target_total);
      payload[at + 1] = static_cast<std::uint8_t>(target & 0xffu);
      payload[at + 2] = static_cast<std::uint8_t>((target >> 8) & 0xffu);
      at += 3;
    }
  }
  // Regrets, then sums, each contiguous over the same action order.
  for (const auto& [key, row] : state.rows) {
    (void)key;
    for (double value : row.regrets) {
      std::uint64_t bits;
      std::memcpy(&bits, &value, sizeof(bits));
      put_u64(payload, at, bits);
      at += 8;
    }
  }
  for (const auto& [key, row] : state.rows) {
    (void)key;
    for (double value : row.sums) {
      std::uint64_t bits;
      std::memcpy(&bits, &value, sizeof(bits));
      put_u64(payload, at, bits);
      at += 8;
    }
  }
  if (at != payload_bytes)
    fail("sealed checkpoint: internal payload length mismatch");
  return payload;
}

// The parsed header, before any payload decode.
struct Header {
  std::uint32_t version = 0;
  std::uint32_t header_bytes = 0;
  abstraction::CardBucketKind key_kind = abstraction::CardBucketKind::Preflop169;
  std::uint32_t algorithm_revision_id = 0;
  std::uint64_t completed_iterations = 0;
  std::uint64_t batch_size = 0;
  std::uint64_t completed_epochs = 0;
  std::uint64_t abstraction_digest = 0;
  std::uint64_t card_abstraction_digest = 0;
  bool card_abstraction_present = false;
  std::uint32_t node_cap = 0;
  std::uint32_t bucket_cap = 0;
  std::uint64_t key_count = 0;
  std::uint64_t total_actions = 0;
  std::uint64_t payload_bytes = 0;
  std::uint64_t payload_digest = 0;
  std::string algorithm_revision;
};

std::vector<std::uint8_t> read_file(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in)
    fail("sealed checkpoint: cannot open " + path.string());
  in.seekg(0, std::ios::end);
  const std::streamoff size = in.tellg();
  if (size < 0)
    fail("sealed checkpoint: cannot size " + path.string());
  in.seekg(0, std::ios::beg);
  std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
  if (size > 0)
    in.read(reinterpret_cast<char*>(bytes.data()), size);
  if (!in && size > 0)
    fail("sealed checkpoint: short read on " + path.string());
  return bytes;
}

// Decode and validate the header. A file shorter than the fixed header is torn;
// a version or magic mismatch is foreign; every other check names its field.
Header decode_header(std::span<const std::uint8_t> bytes, const std::filesystem::path& path) {
  const std::string where = " (" + path.string() + ")";
  if (bytes.size() < kSealedCheckpointHeaderBytes)
    fail("sealed checkpoint: file is shorter than the fixed header" + where);
  if (std::memcmp(bytes.data() + kOffMagic, kMagic, sizeof(kMagic)) != 0)
    fail("sealed checkpoint: bad magic" + where);
  Header h;
  h.version = get_u32(bytes, kOffVersion);
  if (h.version != kSealedCheckpointVersion)
    fail("sealed checkpoint: unsupported version " + std::to_string(h.version) + where);
  h.header_bytes = get_u32(bytes, kOffHeaderBytes);
  if (h.header_bytes != kSealedCheckpointHeaderBytes)
    fail("sealed checkpoint: unexpected header length " + std::to_string(h.header_bytes) + where);
  const std::uint8_t kind = get_u8(bytes, kOffKeyKind);
  // CardBucketKind is a two-valued-then-three enum; reject anything outside it
  // rather than silently mapping a foreign value onto a valid kind.
  switch (kind) {
    case 0:
      h.key_kind = abstraction::CardBucketKind::Identity;
      break;
    case 1:
      h.key_kind = abstraction::CardBucketKind::CategoryTiersV1;
      break;
    case 2:
      h.key_kind = abstraction::CardBucketKind::Preflop169;
      break;
    default:
      fail("sealed checkpoint: unknown card bucket kind " + std::to_string(kind) + where);
  }
  h.algorithm_revision_id = get_u32(bytes, kOffAlgorithmRevisionId);
  h.completed_iterations = get_u64(bytes, kOffCompletedIterations);
  h.batch_size = get_u64(bytes, kOffBatchSize);
  h.completed_epochs = get_u64(bytes, kOffCompletedEpochs);
  h.abstraction_digest = get_u64(bytes, kOffAbstractionDigest);
  h.card_abstraction_digest = get_u64(bytes, kOffCardAbstractionDigest);
  h.card_abstraction_present = get_u8(bytes, kOffCardAbstractionPresent) != 0;
  h.node_cap = get_u32(bytes, kOffNodeCap);
  h.bucket_cap = get_u32(bytes, kOffBucketCap);
  if (h.node_cap == 0)
    fail("sealed checkpoint: node_cap is zero" + where);
  if (h.bucket_cap == 0)
    fail("sealed checkpoint: bucket_cap is zero" + where);
  if (h.bucket_cap > bucket_ceiling(h.key_kind))
    fail("sealed checkpoint: bucket_cap exceeds the card kind's range" + where);
  h.key_count = get_u64(bytes, kOffKeyCount);
  h.total_actions = get_u64(bytes, kOffTotalActions);
  h.payload_bytes = get_u64(bytes, kOffPayloadBytes);
  h.payload_digest = get_u64(bytes, kOffPayloadDigest);

  // The revision string must be NUL-terminated inside its 64 bytes and must
  // agree with the numeric revision, which is what the trainer consumes.
  const char* revision = reinterpret_cast<const char*>(bytes.data() + kOffAlgorithmRevision);
  std::size_t length = 0;
  while (length < kAlgorithmRevisionChars && revision[length] != '\0')
    ++length;
  if (length == kAlgorithmRevisionChars)
    fail("sealed checkpoint: algorithm revision string is not terminated" + where);
  h.algorithm_revision.assign(revision, length);

  // Reject a header/payload size disagreement before allocating anything: the
  // payload length is derived from the counts, so a mismatch is corruption.
  // The comparison is written without multiplication overflow: key_count * 28
  // would wrap for an adversarial count and could then "match" a small file,
  // letting the decoder read past the end. Bounding each count by the payload
  // bytes first makes every product below exact.
  if (h.key_count > h.payload_bytes / 28 || h.total_actions > h.payload_bytes / 19)
    fail("sealed checkpoint: row counts exceed the payload length" + where);
  const std::uint64_t expected_payload = h.key_count * 28 + h.total_actions * 19;
  if (h.payload_bytes != expected_payload)
    fail("sealed checkpoint: payload length disagrees with the row counts" + where);
  if (bytes.size() != static_cast<std::size_t>(kSealedCheckpointHeaderBytes) +
                          static_cast<std::size_t>(h.payload_bytes))
    fail("sealed checkpoint: file length disagrees with the header" + where);
  return h;
}

}  // namespace

void save_sealed_checkpoint(const std::filesystem::path& path, const SealedCheckpointState& state) {
  validate_state(state);

  std::uint64_t total_actions = 0;
  const std::vector<std::uint8_t> payload = encode_payload(state, total_actions);

  std::vector<std::uint8_t> header(kSealedCheckpointHeaderBytes, 0);
  std::memcpy(header.data() + kOffMagic, kMagic, sizeof(kMagic));
  put_u32(header, kOffVersion, kSealedCheckpointVersion);
  put_u32(header, kOffHeaderBytes, kSealedCheckpointHeaderBytes);
  put_u8(header, kOffKeyKind, static_cast<std::uint8_t>(state.key_kind));
  put_u32(header, kOffAlgorithmRevisionId, state.algorithm_revision_id);
  put_u64(header, kOffCompletedIterations, state.completed_iterations);
  put_u64(header, kOffBatchSize, state.batch_size);
  put_u64(header, kOffCompletedEpochs, state.completed_epochs);
  put_u64(header, kOffAbstractionDigest, state.abstraction_digest);
  put_u64(header, kOffCardAbstractionDigest, card_digest_of(state.card_abstraction_digest));
  put_u8(header, kOffCardAbstractionPresent, state.card_abstraction_digest.has_value() ? 1 : 0);
  put_u32(header, kOffNodeCap, state.node_cap);
  put_u32(header, kOffBucketCap, state.bucket_cap);
  put_u64(header, kOffKeyCount, state.rows.size());
  put_u64(header, kOffTotalActions, total_actions);
  put_u64(header, kOffPayloadBytes, payload.size());
  put_u64(header, kOffPayloadDigest, fnv1a_bytes(payload));
  const char* revision = sealed_revision_string(state.algorithm_revision_id);
  std::memcpy(header.data() + kOffAlgorithmRevision, revision, std::strlen(revision));

  std::error_code ec;
  const std::filesystem::path parent = path.parent_path();
  if (parent.empty() || !std::filesystem::is_directory(parent, ec))
    fail("sealed checkpoint: directory does not exist: " + parent.string());

  const std::filesystem::path temp = detail::make_unique_temp(parent);
  {
    std::ofstream out(temp, std::ios::binary | std::ios::trunc);
    if (!out) {
      fail("sealed checkpoint: cannot create " + temp.string());
    }
    out.write(reinterpret_cast<const char*>(header.data()),
              static_cast<std::streamsize>(header.size()));
    if (!payload.empty())
      out.write(reinterpret_cast<const char*>(payload.data()),
                static_cast<std::streamsize>(payload.size()));
    out.flush();
    if (!out) {
      std::filesystem::remove(temp, ec);
      fail("sealed checkpoint: write failed for " + temp.string());
    }
  }
  try {
    detail::fsync_file(temp);
  } catch (...) {
    std::filesystem::remove(temp, ec);
    throw;
  }
  // Atomic replace, matching the SQLite writer's discipline: a reader sees the
  // old checkpoint or the new one, never a torn write.
  if (::rename(temp.c_str(), path.c_str()) != 0) {
    const int err = errno;
    std::filesystem::remove(temp, ec);
    fail("sealed checkpoint: rename over " + path.string() +
         " failed: " + std::string(std::strerror(err)));
  }
  detail::fsync_directory(parent);
}

SealedCheckpointState inspect_sealed_checkpoint(const std::filesystem::path& path) {
  const std::vector<std::uint8_t> bytes = read_file(path);
  const Header h = decode_header(bytes, path);
  // Report the revision; the caller decides whether it is incompatible. A
  // structural failure here is corruption (discard and retrain), while an
  // unknown revision is a semantic refusal, and the two must not be confused.
  SealedCheckpointState state;
  state.algorithm_revision = h.algorithm_revision;
  state.algorithm_revision_id = h.algorithm_revision_id;
  state.abstraction_digest = h.abstraction_digest;
  if (h.card_abstraction_present)
    state.card_abstraction_digest = h.card_abstraction_digest;
  state.completed_iterations = h.completed_iterations;
  state.batch_size = h.batch_size;
  state.completed_epochs = h.completed_epochs;
  state.node_cap = h.node_cap;
  state.bucket_cap = h.bucket_cap;
  state.key_kind = h.key_kind;
  return state;
}

SealedCheckpointState load_sealed_checkpoint(const std::filesystem::path& path) {
  const std::vector<std::uint8_t> bytes = read_file(path);
  const Header h = decode_header(bytes, path);
  const std::string where = " (" + path.string() + ")";

  // A known revision must agree with its canonical string; an unknown one is
  // returned as-is for the caller to refuse. Treating an unknown revision as
  // corruption would make the preflight discard and retrain it, which is the
  // opposite of the required hard refusal.
  const char* expected_revision = sealed_revision_string(h.algorithm_revision_id);
  if (expected_revision != nullptr && h.algorithm_revision != std::string(expected_revision))
    fail("sealed checkpoint: revision string disagrees with the numeric revision" + where);

  const std::span<const std::uint8_t> payload(bytes.data() + kSealedCheckpointHeaderBytes,
                                              static_cast<std::size_t>(h.payload_bytes));
  if (fnv1a_bytes(payload) != h.payload_digest)
    fail("sealed checkpoint: payload digest mismatch" + where);

  const std::size_t key_count = static_cast<std::size_t>(h.key_count);
  const std::size_t total_actions = static_cast<std::size_t>(h.total_actions);
  const std::size_t buckets_start = 0;
  const std::size_t nodes_start = key_count * 4;
  const std::size_t offsets_start = key_count * (4 + 8);
  const std::size_t counts_start = offsets_start + key_count * 4;
  const std::size_t visits_start = key_count * (4 + 8 + 4 + 4);
  const std::size_t action_blob_start = visits_start + key_count * 8;
  const std::size_t regrets_start = action_blob_start + total_actions * 3;
  const std::size_t sums_start = regrets_start + total_actions * 8;

  SealedCheckpointState state;
  state.algorithm_revision = h.algorithm_revision;
  state.algorithm_revision_id = h.algorithm_revision_id;
  state.abstraction_digest = h.abstraction_digest;
  if (h.card_abstraction_present)
    state.card_abstraction_digest = h.card_abstraction_digest;
  state.completed_iterations = h.completed_iterations;
  state.batch_size = h.batch_size;
  state.completed_epochs = h.completed_epochs;
  state.node_cap = h.node_cap;
  state.bucket_cap = h.bucket_cap;
  state.key_kind = h.key_kind;

  const std::uint32_t ceiling = bucket_ceiling(h.key_kind);
  NSeatInformationKey previous{};
  bool have_previous = false;
  for (std::size_t i = 0; i < key_count; ++i) {
    const std::uint32_t bucket = get_u32(payload, buckets_start + i * 4);
    const std::uint64_t node_index = get_u64(payload, nodes_start + i * 8);
    if (bucket >= h.bucket_cap || bucket >= ceiling)
      fail("sealed checkpoint: bucket " + std::to_string(bucket) +
           " is outside the declared range" + where);
    if (node_index >= h.node_cap)
      fail("sealed checkpoint: node index " + std::to_string(node_index) +
           " is at or above node_cap" + where);
    NSeatInformationKey key{bucket, node_index};
    // The format is sorted and duplicate-free. A violation means the file was
    // assembled by something other than this writer.
    if (have_previous && !(previous < key))
      fail("sealed checkpoint: keys are not strictly ascending" + where);
    previous = key;
    have_previous = true;

    const std::uint32_t actions_offset = get_u32(payload, offsets_start + i * 4);
    const std::uint32_t actions_count = get_u32(payload, counts_start + i * 4);
    // The offset must land on the implied run start and the count must fit the
    // blob. An offset that merely lies inside the blob is not enough: the
    // regrets and sums blocks are indexed by the same global ordinal, so a
    // shifted run would silently read another key's values.
    const std::uint64_t first_ordinal =
        (static_cast<std::uint64_t>(actions_offset) - action_blob_start) / 3;
    const std::uint64_t expected_offset = action_blob_start + first_ordinal * 3;
    if (actions_offset < action_blob_start || expected_offset != actions_offset)
      fail("sealed checkpoint: action offset " + std::to_string(actions_offset) +
           " is not a run boundary" + where);
    if (static_cast<std::uint64_t>(actions_count) + first_ordinal > total_actions)
      fail("sealed checkpoint: action run exceeds the action blob" + where);

    const std::size_t visits_at = visits_start + i * 8;
    NSeatRawRow row;
    row.visits = get_u64(payload, visits_at);
    row.actions.reserve(actions_count);
    row.regrets.reserve(actions_count);
    row.sums.reserve(actions_count);
    for (std::size_t a = 0; a < actions_count; ++a) {
      const std::size_t at = static_cast<std::size_t>(actions_offset) + a * 3;
      const std::uint8_t type = payload[at];
      if (type > 4)
        fail("sealed checkpoint: action type is outside 0..4" + where);
      const std::uint16_t target = static_cast<std::uint16_t>(
          payload[at + 1] | (static_cast<std::uint16_t>(payload[at + 2]) << 8));
      poker::Action action;
      action.type = static_cast<poker::ActionType>(type);
      action.target_total = target;
      row.actions.push_back(action);

      const std::uint64_t ordinal = first_ordinal + a;
      std::uint64_t regret_bits =
          get_u64(payload, regrets_start + static_cast<std::size_t>(ordinal) * 8);
      std::uint64_t sum_bits = get_u64(payload, sums_start + static_cast<std::size_t>(ordinal) * 8);
      double regret;
      double sum;
      std::memcpy(&regret, &regret_bits, sizeof(regret));
      std::memcpy(&sum, &sum_bits, sizeof(sum));
      if (!std::isfinite(regret))
        fail("sealed checkpoint: cumulative regret is not finite" + where);
      if (!std::isfinite(sum))
        fail("sealed checkpoint: average weight is not finite" + where);
      row.regrets.push_back(regret);
      row.sums.push_back(sum);
    }
    state.rows.emplace(key, std::move(row));
  }
  if (state.rows.size() != key_count)
    fail("sealed checkpoint: duplicate keys collapsed during decode" + where);
  return state;
}

}  // namespace bs::artifacts
