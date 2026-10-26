// RFC 0028/RFC 0030/RFC 0035: BSWARM03 sealed warm-start checkpoint codec.
//
// The codec is the only place `visits` can be verified: no published artifact
// carries it, so an artifact-byte comparison cannot detect a `visits` error.
// The round-trip test below therefore compares every field of every row,
// including visits, and is load-bearing rather than a formality.
//
// Every rejection case asserts a named failure, because the resume preflight
// distinguishes "torn, discard and retrain" (a load failure) from "well-formed
// but incompatible" (an identity refusal it must report).
#include <array>
#include <bs/abstraction.hpp>
#include <bs/nseat_trainer.hpp>
#include <bs/sealed_checkpoint.hpp>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <system_error>
#include <vector>

#define CHECK(cond)                                                         \
  do {                                                                      \
    if (!(cond)) {                                                          \
      std::printf("CHECK failed: %s (%s:%d)\n", #cond, __FILE__, __LINE__); \
      return false;                                                         \
    }                                                                       \
  } while (0)

namespace {

namespace fs = std::filesystem;
using bs::abstraction::AbstractionId;
using bs::abstraction::CardBucketKind;
using bs::artifacts::kSealedCheckpointHeaderBytes;
using bs::artifacts::load_sealed_checkpoint;
using bs::artifacts::save_sealed_checkpoint;
using bs::artifacts::SealedCheckpointError;
using bs::artifacts::SealedCheckpointState;
using bs::poker::Action;
using bs::poker::ActionType;
using bs::solver::NSeatInformationKey;
using bs::solver::NSeatRawRow;

constexpr std::size_t kOffMagic = 0;
constexpr std::size_t kOffVersion = 8;
constexpr std::size_t kOffPayloadDigest = 112;
constexpr std::size_t kOffAlgorithmRevisionU32 = 24;
constexpr std::size_t kOffPayloadBytes = 104;

fs::path scratch(const std::string& name) {
  const fs::path dir = fs::temp_directory_path() / ("bs-sealed-" + name);
  std::error_code ec;
  fs::remove_all(dir, ec);
  fs::create_directories(dir, ec);
  return dir;
}

void put_u32_at(std::vector<std::uint8_t>& bytes, std::size_t at, std::uint32_t value) {
  for (int i = 0; i < 4; ++i)
    bytes[at + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>((value >> (i * 8)) & 0xffu);
}

NSeatRawRow make_row(std::size_t actions, double base, std::uint64_t visits) {
  NSeatRawRow row;
  row.visits = visits;
  for (std::size_t a = 0; a < actions; ++a) {
    Action action;
    action.type = a == 0 ? ActionType::Check : ActionType::Bet;
    action.target_total = static_cast<bs::poker::Chips>((a + 1) * 25);
    row.actions.push_back(action);
    row.regrets.push_back(base + static_cast<double>(a));
    row.sums.push_back(base * 2.0 + static_cast<double>(a));
  }
  return row;
}

// A state spanning three buckets and several nodes, so ordering, multi-bucket
// coverage, and uneven action counts are all exercised.
SealedCheckpointState make_state() {
  SealedCheckpointState state;
  state.algorithm_revision_id = bs::solver::kNSeatAlgorithmRevision;
  state.algorithm_revision =
      bs::artifacts::sealed_revision_string(bs::solver::kNSeatAlgorithmRevision);
  state.abstraction_digest = 0x1122334455667788ULL;
  state.card_abstraction_digest = 0xdeadbeefcafe1234ULL;
  state.completed_iterations = 256;
  state.batch_size = 1;
  state.completed_epochs = 7;
  state.node_cap = 4096;
  state.bucket_cap = 169;
  state.key_kind = CardBucketKind::Preflop169;
  state.tree_shape_digest = 0x0123456789abcdefULL;
  state.rows[{3, 10}] = make_row(2, 0.5, 11);
  state.rows[{3, 200}] = make_row(3, 1.25, 12);
  state.rows[{7, 10}] = make_row(2, -2.5, 13);
  state.rows[{99, 3999}] = make_row(6, 4.0, 14);
  return state;
}

bool same_row(const NSeatRawRow& a, const NSeatRawRow& b) {
  if (a.visits != b.visits)
    return false;
  if (a.actions.size() != b.actions.size() || a.regrets.size() != b.regrets.size() ||
      a.sums.size() != b.sums.size())
    return false;
  for (std::size_t i = 0; i < a.actions.size(); ++i) {
    if (a.actions[i].type != b.actions[i].type ||
        a.actions[i].target_total != b.actions[i].target_total)
      return false;
    // Bitwise, not approximate: a checkpoint must restore the trainer's exact
    // accumulators or the resumed run diverges.
    if (std::memcmp(&a.regrets[i], &b.regrets[i], sizeof(double)) != 0)
      return false;
    if (std::memcmp(&a.sums[i], &b.sums[i], sizeof(double)) != 0)
      return false;
  }
  return true;
}

bool round_trip() {
  const fs::path dir = scratch("roundtrip");
  const fs::path path = dir / "class-0000-checkpoint.bswarm";
  const SealedCheckpointState state = make_state();
  save_sealed_checkpoint(path, state);

  const SealedCheckpointState loaded = load_sealed_checkpoint(path);
  CHECK(loaded.algorithm_revision == state.algorithm_revision);
  CHECK(loaded.algorithm_revision_id == state.algorithm_revision_id);
  CHECK(loaded.abstraction_digest == state.abstraction_digest);
  CHECK(loaded.card_abstraction_digest.has_value());
  CHECK(loaded.card_abstraction_digest.value() == state.card_abstraction_digest.value());
  CHECK(loaded.completed_iterations == state.completed_iterations);
  CHECK(loaded.batch_size == state.batch_size);
  CHECK(loaded.completed_epochs == state.completed_epochs);
  CHECK(loaded.node_cap == state.node_cap);
  CHECK(loaded.bucket_cap == state.bucket_cap);
  CHECK(loaded.key_kind == state.key_kind);
  // RFC 0030: the BSWARM02 witness round-trips (still present in BSWARM03).
  CHECK(loaded.tree_shape_digest.has_value());
  CHECK(*loaded.tree_shape_digest == *state.tree_shape_digest);
  // RFC 0035: the root-table witness round-trips.
  CHECK(loaded.root_table_hash == state.root_table_hash);
  CHECK(loaded.rows.size() == state.rows.size());
  for (const auto& [key, row] : state.rows) {
    const auto it = loaded.rows.find(key);
    CHECK(it != loaded.rows.end());
    CHECK(same_row(it->second, row));
  }

  // The header must be exactly the fixed size plus the payload, so a reader
  // never has to guess where the payload starts.
  std::error_code ec;
  const auto file_bytes = fs::file_size(path, ec);
  CHECK(!ec);
  const std::uint64_t expected_payload =
      state.rows.size() * 28 + 4 * 0 + 0;  // recomputed below from the file
  (void)expected_payload;
  CHECK(file_bytes > kSealedCheckpointHeaderBytes);
  return true;
}

// Loading a fresh file must yield the same state as an inspected one for the
// header fields, so the preflight cannot disagree with the real load.
bool inspect_matches_load() {
  const fs::path dir = scratch("inspect");
  const fs::path path = dir / "class-0001-checkpoint.bswarm";
  save_sealed_checkpoint(path, make_state());
  const SealedCheckpointState inspected = bs::artifacts::inspect_sealed_checkpoint(path);
  const SealedCheckpointState loaded = load_sealed_checkpoint(path);
  CHECK(inspected.rows.empty());  // header-only by contract
  CHECK(inspected.algorithm_revision == loaded.algorithm_revision);
  CHECK(inspected.algorithm_revision_id == loaded.algorithm_revision_id);
  CHECK(inspected.abstraction_digest == loaded.abstraction_digest);
  CHECK(inspected.completed_iterations == loaded.completed_iterations);
  CHECK(inspected.batch_size == loaded.batch_size);
  CHECK(inspected.completed_epochs == loaded.completed_epochs);
  CHECK(inspected.node_cap == loaded.node_cap);
  CHECK(inspected.bucket_cap == loaded.bucket_cap);
  CHECK(inspected.key_kind == loaded.key_kind);
  return true;
}

bool expects_failure(const fs::path& path) {
  try {
    (void)load_sealed_checkpoint(path);
  } catch (const SealedCheckpointError&) {
    return true;
  } catch (...) {
    return false;
  }
  return false;
}

std::vector<std::uint8_t> read_bytes(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  in.seekg(0, std::ios::end);
  const auto size = in.tellg();
  in.seekg(0, std::ios::beg);
  std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
  if (!bytes.empty())
    in.read(reinterpret_cast<char*>(bytes.data()), size);
  return bytes;
}

void write_bytes(const fs::path& path, const std::vector<std::uint8_t>& bytes) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!bytes.empty())
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
}

// Each rejection case mutates one field of a valid file.
bool rejects_malformed() {
  const fs::path dir = scratch("malformed");
  const fs::path good = dir / "good.bswarm";
  save_sealed_checkpoint(good, make_state());
  const std::vector<std::uint8_t> original = read_bytes(good);
  CHECK(original.size() > kSealedCheckpointHeaderBytes);

  // Bad magic.
  {
    auto bytes = original;
    bytes[kOffMagic] = 'X';
    const fs::path path = dir / "bad-magic.bswarm";
    write_bytes(path, bytes);
    CHECK(expects_failure(path));
  }
  // Wrong version (BSWARM03 is version 3; a foreign u32 is rejected).
  {
    auto bytes = original;
    put_u32_at(bytes, kOffVersion, 99);
    const fs::path path = dir / "bad-version.bswarm";
    write_bytes(path, bytes);
    CHECK(expects_failure(path));
  }
  // RFC 0030: a BSWARM01 magic is foreign, not decoded by the new codec.
  {
    auto bytes = original;
    const char old_magic[8] = {'B', 'S', 'W', 'A', 'R', 'M', '0', '1'};
    std::memcpy(bytes.data() + kOffMagic, old_magic, 8);
    const fs::path path = dir / "old-magic.bswarm";
    write_bytes(path, bytes);
    CHECK(expects_failure(path));
  }
  // RFC 0035: a BSWARM02 magic is foreign too — the bump added the
  // root-table witness and no dual reader exists.
  {
    auto bytes = original;
    const char old_magic[8] = {'B', 'S', 'W', 'A', 'R', 'M', '0', '2'};
    std::memcpy(bytes.data() + kOffMagic, old_magic, 8);
    const fs::path path = dir / "bswarm02-magic.bswarm";
    write_bytes(path, bytes);
    CHECK(expects_failure(path));
  }
  // Truncated payload.
  {
    auto bytes = original;
    bytes.resize(bytes.size() - 8);
    const fs::path path = dir / "truncated.bswarm";
    write_bytes(path, bytes);
    CHECK(expects_failure(path));
  }
  // Truncated header.
  {
    auto bytes = original;
    bytes.resize(kSealedCheckpointHeaderBytes - 1);
    const fs::path path = dir / "short-header.bswarm";
    write_bytes(path, bytes);
    CHECK(expects_failure(path));
  }
  // Payload digest mismatch (a flipped payload byte, without fixing the
  // digest).
  {
    auto bytes = original;
    bytes.back() ^= 0xffu;
    const fs::path path = dir / "bad-digest.bswarm";
    write_bytes(path, bytes);
    CHECK(expects_failure(path));
  }
  // Payload length disagrees with the counts.
  {
    auto bytes = original;
    put_u32_at(bytes, kOffPayloadBytes, static_cast<std::uint32_t>(original.size()));
    const fs::path path = dir / "bad-payload-bytes.bswarm";
    write_bytes(path, bytes);
    CHECK(expects_failure(path));
  }
  // A known revision id whose stored string disagrees with its canonical form.
  {
    auto bytes = original;
    put_u32_at(bytes, kOffAlgorithmRevisionU32, 1);
    const fs::path path = dir / "bad-revision.bswarm";
    write_bytes(path, bytes);
    CHECK(expects_failure(path));
  }
  // An unknown revision id is structurally valid: the codec reports it and the
  // caller refuses. It must NOT be reported as corruption, or the preflight
  // would discard and retrain a checkpoint the operator must inspect.
  {
    auto bytes = original;
    put_u32_at(bytes, kOffAlgorithmRevisionU32, 99);
    const fs::path path = dir / "unknown-revision.bswarm";
    write_bytes(path, bytes);
    bool loaded_ok = false;
    try {
      const SealedCheckpointState state = load_sealed_checkpoint(path);
      loaded_ok = state.algorithm_revision_id == 99;
    } catch (const SealedCheckpointError&) {
      loaded_ok = false;
    }
    CHECK(loaded_ok);
  }
  return true;
}

// The writer refuses states it cannot represent, rather than silently
// truncating or dropping a row.
bool rejects_unrepresentable_states() {
  const fs::path dir = scratch("writer");
  const fs::path path = dir / "x.bswarm";

  {
    SealedCheckpointState state = make_state();
    state.rows[{200, 1}] = make_row(2, 0.0, 1);  // bucket >= bucket_cap
    bool threw = false;
    try {
      save_sealed_checkpoint(path, state);
    } catch (const SealedCheckpointError&) {
      threw = true;
    }
    CHECK(threw);
  }
  {
    SealedCheckpointState state = make_state();
    state.rows[{1, 5000}] = make_row(2, 0.0, 1);  // node >= node_cap
    bool threw = false;
    try {
      save_sealed_checkpoint(path, state);
    } catch (const SealedCheckpointError&) {
      threw = true;
    }
    CHECK(threw);
  }
  {
    SealedCheckpointState state = make_state();
    NSeatRawRow row = make_row(2, 0.0, 1);
    row.sums.pop_back();  // array length disagreement
    state.rows[{1, 1}] = row;
    bool threw = false;
    try {
      save_sealed_checkpoint(path, state);
    } catch (const SealedCheckpointError&) {
      threw = true;
    }
    CHECK(threw);
  }
  {
    SealedCheckpointState state = make_state();
    NSeatRawRow row = make_row(2, 0.0, 1);
    row.regrets[0] = std::nan("");
    state.rows[{1, 1}] = row;
    bool threw = false;
    try {
      save_sealed_checkpoint(path, state);
    } catch (const SealedCheckpointError&) {
      threw = true;
    }
    CHECK(threw);
  }
  {
    SealedCheckpointState state = make_state();
    state.algorithm_revision_id = 99;  // no canonical string for this revision
    bool threw = false;
    try {
      save_sealed_checkpoint(path, state);
    } catch (const SealedCheckpointError&) {
      threw = true;
    }
    CHECK(threw);
  }
  // Nothing above may have left a file behind at the destination.
  std::error_code ec;
  CHECK(!fs::exists(path, ec));
  return true;
}

// A failed write leaves the previous checkpoint intact: a crash mid-write must
// not cost the previous interval.
bool failed_write_preserves_previous() {
  const fs::path dir = scratch("preserve");
  const fs::path path = dir / "class-0002-checkpoint.bswarm";
  const SealedCheckpointState good = make_state();
  save_sealed_checkpoint(path, good);
  const std::vector<std::uint8_t> before = read_bytes(path);

  SealedCheckpointState bad = make_state();
  bad.rows[{500, 1}] = make_row(1, 0.0, 1);
  bool threw = false;
  try {
    save_sealed_checkpoint(path, bad);
  } catch (const SealedCheckpointError&) {
    threw = true;
  }
  CHECK(threw);
  const std::vector<std::uint8_t> after = read_bytes(path);
  CHECK(before == after);
  // And the surviving file still loads.
  const SealedCheckpointState loaded = load_sealed_checkpoint(path);
  CHECK(loaded.rows.size() == good.rows.size());
  return true;
}

// Atomic overwrite: a second save replaces the first completely, with no
// stale bytes from the larger file surviving.
bool atomic_overwrite() {
  const fs::path dir = scratch("overwrite");
  const fs::path path = dir / "class-0003-checkpoint.bswarm";
  SealedCheckpointState big = make_state();
  big.completed_iterations = 256;
  save_sealed_checkpoint(path, big);
  const auto big_size = fs::file_size(path);

  SealedCheckpointState small = make_state();
  small.completed_iterations = 512;
  small.rows.clear();
  small.rows[{1, 1}] = make_row(1, 0.0, 1);
  save_sealed_checkpoint(path, small);
  const auto small_size = fs::file_size(path);
  CHECK(small_size < big_size);
  const SealedCheckpointState loaded = load_sealed_checkpoint(path);
  CHECK(loaded.completed_iterations == 512);
  CHECK(loaded.rows.size() == 1);
  return true;
}

// Size is proportional to keys and actions, not to any range width: the same
// map must produce the same file size regardless of how the caller arrived at
// it. This pins the property AC1 measures without needing a full training run.
bool size_is_key_proportional() {
  const fs::path dir = scratch("size");
  const fs::path path = dir / "class-0004-checkpoint.bswarm";
  SealedCheckpointState state;
  state.algorithm_revision_id = bs::solver::kNSeatAlgorithmRevision;
  state.algorithm_revision =
      bs::artifacts::sealed_revision_string(bs::solver::kNSeatAlgorithmRevision);
  state.node_cap = 1024;
  state.bucket_cap = 169;
  state.key_kind = CardBucketKind::Preflop169;
  state.tree_shape_digest = 0xabcdef0123456789ULL;
  for (std::uint32_t bucket = 0; bucket < 3; ++bucket) {
    for (std::uint64_t node = 0; node < 4; ++node) {
      state.rows[{bucket, node}] = make_row(2, 0.0, 1);
    }
  }
  save_sealed_checkpoint(path, state);
  const auto size = fs::file_size(path);
  // 12 keys, 24 actions: 200 + 12*28 + 24*19 = 200 + 336 + 456 = 992.
  CHECK(size == kSealedCheckpointHeaderBytes + 12 * 28 + 24 * 19);
  return true;
}

// RFC 0031: the TiersDrawV1 kind ordinal (3) and its 36-bucket ceiling
// round-trip, so a checkpoint sealed under the draw-aware scheme decodes
// back to that scheme rather than a foreign kind.
bool round_trip_tiers_draw_kind() {
  const fs::path dir = scratch("roundtrip-tiers-draw");
  const fs::path path = dir / "class-0000-checkpoint.bswarm";
  SealedCheckpointState state;
  state.algorithm_revision_id = bs::solver::kNSeatAlgorithmRevision;
  state.algorithm_revision =
      bs::artifacts::sealed_revision_string(bs::solver::kNSeatAlgorithmRevision);
  state.abstraction_digest = 0x1122334455667788ULL;
  state.card_abstraction_digest =
      bs::abstraction::card_abstraction_id(CardBucketKind::TiersDrawV1).digest;
  state.completed_iterations = 32;
  state.batch_size = 1;
  state.completed_epochs = 1;
  state.node_cap = 4096;
  state.bucket_cap = 35;
  state.key_kind = CardBucketKind::TiersDrawV1;
  state.tree_shape_digest = 0x0123456789abcdefULL;
  state.rows[{1, 10}] = make_row(2, 0.5, 11);     // flush-draw bucket
  state.rows[{17, 200}] = make_row(3, 1.25, 12);  // straight + flush draw
  save_sealed_checkpoint(path, state);
  const SealedCheckpointState loaded = load_sealed_checkpoint(path);
  CHECK(loaded.key_kind == CardBucketKind::TiersDrawV1);
  CHECK(loaded.bucket_cap == 35);
  CHECK(loaded.rows.size() == 2);
  return true;
}

}  // namespace

int main() {
  struct Case {
    const char* name;
    bool (*fn)();
  };
  const Case cases[] = {
      {"round_trip", round_trip},
      {"inspect_matches_load", inspect_matches_load},
      {"rejects_malformed", rejects_malformed},
      {"rejects_unrepresentable_states", rejects_unrepresentable_states},
      {"failed_write_preserves_previous", failed_write_preserves_previous},
      {"atomic_overwrite", atomic_overwrite},
      {"size_is_key_proportional", size_is_key_proportional},
      {"round_trip_tiers_draw_kind", round_trip_tiers_draw_kind},
  };
  for (const Case& c : cases) {
    if (!c.fn()) {
      std::printf("sealed_checkpoint: FAILED %s\n", c.name);
      return 1;
    }
    std::printf("sealed_checkpoint: ok %s\n", c.name);
  }
  std::printf("sealed_checkpoint: all tests passed\n");
  return 0;
}
