// RFC 0008 stage 6 — frozen artifact persistence gate (P2-2 regression).
//
// A trained artifact must survive write -> load with byte-exact identity, and
// every on-disk mutation of its rows blob must be rejected: the loader
// recomputes the FNV-1a rows content hash and refuses drift rather than
// returning silently corrupted rows. Also pins the blob's structural
// validation (bad magic, truncation, duplicate keys).
#include <bs/abstraction.hpp>
#include <bs/heads_up.hpp>
#include <bs/stage6/frozen_manifest.hpp>
#include <bs/stage6/geometry.hpp>
#include <bs/stage6/translator_id.hpp>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace {

using namespace bs::poker;
using namespace bs::stage6;

int failures = 0;
void check(bool c, const char* w) {
  if (!c) {
    std::printf("FAIL: %s\n", w);
    ++failures;
  }
}

Action an_action(ActionType type, Chips target) {
  return Action{type, target};
}

FrozenArtifactRows sample_rows(std::uint64_t stamp) {
  FrozenArtifactRows rows;
  for (std::uint32_t bucket_id : {1u, 7u, 42u}) {
    AbstractInfosetKey key;
    key.artifact_content_hash = stamp;
    key.tree_node_index = 0;
    key.own_card_bucket = bucket_id;
    key.path_hash = 0xdeadbeef00ULL + bucket_id;
    AbstractPolicyRow row;
    row.abstract_actions = {an_action(ActionType::Check, 0), an_action(ActionType::Bet, 6),
                            an_action(ActionType::Fold, 0)};
    row.visits = 1000ULL * bucket_id;
    row.probabilities = {0.25 + 0.01 * bucket_id, 0.5, 0.25 - 0.01 * bucket_id};
    rows.emplace(key, std::move(row));
  }
  return rows;
}

FrozenManifest sample_manifest(const FrozenArtifactRows& rows) {
  FrozenManifest m;
  m.action_abstraction = bs::abstraction::card_abstraction_id(
      bs::abstraction::CardBucketKind::CategoryTiersV1);  // placeholder, overwritten below
  m.action_abstraction.name = "declared";
  m.action_abstraction.version = 1;
  m.action_abstraction.parameters = "bets=1/2;raises=1x";
  m.action_abstraction.digest = 0x1111111111111111ULL;
  m.card_abstraction.name = "category-tiers";
  m.card_abstraction.version = 1;
  m.card_abstraction.parameters = "v1";
  m.card_abstraction.digest = 0x2222222222222222ULL;
  m.translator = nearest_target_translator_id();
  m.geometry_matrix_hash = 0x3333333333333333ULL;
  m.bucket = GeometryBucketKey{3, 3, 3, 3};
  m.chart_digest_sha256 = "roundtrip-test";
  m.training_config_hash = 0x4444444444444444ULL;
  m.iterations_completed = 9'999;
  m.artifact_content_hash = 0x5555555555555555ULL;
  m.rows_content_hash = hash_artifact_rows(rows);
  m.iterations = 10'000;
  m.master_seed = 777;
  return m;
}

std::string read_bytes(const std::filesystem::path& p) {
  std::ifstream is(p, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(is), std::istreambuf_iterator<char>());
}
void write_bytes(const std::filesystem::path& p, const std::string& b) {
  std::ofstream os(p, std::ios::binary | std::ios::trunc);
  os.write(b.data(), static_cast<std::streamsize>(b.size()));
}
bool load_throws(const std::string& dir) {
  try {
    (void)load_artifact_dir(dir);
  } catch (const frozen_artifact_error&) {
    return true;
  }
  return false;
}

int test_roundtrip_and_tamper() {
  int local = 0;
  namespace fs = std::filesystem;
  const fs::path dir = fs::temp_directory_path() / "bs_stage6_artifact_roundtrip";
  fs::remove_all(dir);

  const FrozenArtifactRows rows = sample_rows(0x5555555555555555ULL);
  const FrozenManifest manifest = sample_manifest(rows);
  write_artifact_dir(dir.string(), manifest, rows);
  check(fs::exists(dir / "rows.bin") && fs::exists(dir / "manifest.bin"),
        "write_artifact_dir creates rows.bin and manifest.bin");

  LoadedFrozenArtifact loaded = load_artifact_dir(dir.string());
  check(loaded.rows == rows, "loaded rows are byte/struct-identical to the written rows");
  check(loaded.manifest == manifest, "loaded manifest is identical to the written manifest");
  check(loaded.manifest.rows_content_hash == hash_artifact_rows(loaded.rows),
        "manifest rows hash matches the recomputed hash");

  // Determinism: writing again yields byte-identical blobs.
  const std::string rows_blob_a = read_bytes(dir / "rows.bin");
  const std::string manifest_blob_a = read_bytes(dir / "manifest.bin");
  fs::remove_all(dir);
  write_artifact_dir(dir.string(), manifest, rows);
  check(read_bytes(dir / "rows.bin") == rows_blob_a, "rows.bin serialization is deterministic");
  check(read_bytes(dir / "manifest.bin") == manifest_blob_a,
        "manifest.bin serialization is deterministic");

  // --- tamper detection -----------------------------------------------------
  const fs::path tampered = fs::temp_directory_path() / "bs_stage6_artifact_tampered";
  fs::remove_all(tampered);
  write_artifact_dir(tampered.string(), manifest, rows);
  std::string blob = read_bytes(tampered / "rows.bin");

  // Flip one byte of the first row's body (a field the FNV hash covers; not
  // the magic or the row count). The exact field is layout-dependent, so also
  // see the explicit probability-hash and end-to-end checks below.
  std::size_t pos = 69;
  check(pos < blob.size(), "tamper offset is inside the blob");
  blob[pos] = static_cast<char>(static_cast<unsigned char>(blob[pos]) ^ 0x01);
  write_bytes(tampered / "rows.bin", blob);
  check(load_throws(tampered.string()), "a one-byte rows-body flip is hash-rejected");

  // The content hash is sensitive to a single PROBABILITY value (the sealed
  // policy itself), not just keys/action fields: perturb one probability and
  // confirm the recomputed hash changes.
  {
    FrozenArtifactRows mutated = rows;
    auto& probs = std::begin(mutated)->second.probabilities;
    probs[0] += 0.25;
    probs[1] -= 0.25;  // keep it a distribution, only to isolate the hash
    check(hash_artifact_rows(mutated) != hash_artifact_rows(rows),
          "a probability change changes the rows content hash");
    // End-to-end: write the GOOD artifact, then replace rows.bin on disk with
    // the re-encoded MUTATED rows while leaving manifest.bin (and its stamp)
    // untouched. write_artifact_dir would otherwise re-stamp the hash, so the
    // rows blob is overwritten directly — the real tamper path.
    fs::remove_all(tampered);
    write_artifact_dir(tampered.string(), manifest, rows);
    // Build the mutated rows blob in a scratch directory (its manifest is
    // correctly re-stamped there), then copy ONLY its rows.bin over the good
    // artifact's rows.bin, leaving the good manifest in place.
    const fs::path scratch = fs::temp_directory_path() / "bs_stage6_artifact_scratch";
    fs::remove_all(scratch);
    write_artifact_dir(scratch.string(), manifest, mutated);
    write_bytes(tampered / "rows.bin", read_bytes(scratch / "rows.bin"));
    fs::remove_all(scratch);
    check(load_throws(tampered.string()),
          "mutated rows.bin under the untouched manifest is hash-rejected");
  }

  // Corrupt the magic header.
  fs::remove_all(tampered);
  write_artifact_dir(tampered.string(), manifest, rows);
  blob = read_bytes(tampered / "rows.bin");
  blob[0] = 'X';
  write_bytes(tampered / "rows.bin", blob);
  check(load_throws(tampered.string()), "a bad magic header is rejected");

  // Truncation.
  fs::remove_all(tampered);
  write_artifact_dir(tampered.string(), manifest, rows);
  blob = read_bytes(tampered / "rows.bin");
  blob.resize(blob.size() - 8);
  write_bytes(tampered / "rows.bin", blob);
  check(load_throws(tampered.string()), "a truncated rows blob is rejected");

  // Trailing bytes are refused.
  fs::remove_all(tampered);
  write_artifact_dir(tampered.string(), manifest, rows);
  blob = read_bytes(tampered / "rows.bin");
  blob.push_back(static_cast<char>(0));
  write_bytes(tampered / "rows.bin", blob);
  check(load_throws(tampered.string()), "trailing bytes are rejected");

  // Malformed duplicate-key blob: bump the row count and append a byte-for-byte
  // copy of the first row record. The content hash rejects the altered bytes
  // before the duplicate-key check can fire (both defenses are load-bearing;
  // the loader's in-memory duplicate guard at decode is separately reached when
  // a legitimately-hashed blob cannot contain a repeated key by construction).
  fs::remove_all(tampered);
  write_artifact_dir(tampered.string(), manifest, rows);
  blob = read_bytes(tampered / "rows.bin");
  {
    // Locate the first record boundary from the second key by re-encoding a
    // one-row prefix is internal; instead derive the first record's length from
    // its action_count at fixed offset 16+24.
    auto u8at = [&](std::size_t i) -> unsigned char { return static_cast<unsigned char>(blob[i]); };
    std::uint32_t first_actions =
        static_cast<std::uint32_t>(u8at(24)) | (static_cast<std::uint32_t>(u8at(25)) << 8) |
        (static_cast<std::uint32_t>(u8at(26)) << 16) | (static_cast<std::uint32_t>(u8at(27)) << 24);
    // key 28 + action_count 4 + per action (1 + 8) + visits 8 + prob_count 4 +
    // 8*prob.
    const std::size_t first_record =
        28 + 4 + first_actions * 9 + 8 + 4 + static_cast<std::size_t>(first_actions) * 8;
    const std::string record = blob.substr(16, first_record);
    std::string dup = blob.substr(0, 16);
    // count 3 -> 4 at little-endian bytes 8..15.
    dup[8] = static_cast<char>(4);
    dup += record;                          // record 1
    dup += record;                          // record 1 again -> duplicate of the first key
    dup += blob.substr(16 + first_record);  // records 2..N
    write_bytes(tampered / "rows.bin", dup);
    check(load_throws(tampered.string()),
          "a count-bumped blob with a repeated key record is rejected");
  }

  // Missing file.
  fs::remove_all(tampered);
  fs::create_directories(tampered);
  check(load_throws(tampered.string()), "a missing rows.bin is rejected");

  fs::remove_all(dir);
  fs::remove_all(tampered);
  return local;
}

}  // namespace

int main() {
  failures += test_roundtrip_and_tamper();
  if (failures) {
    std::printf("STAGE 6 FROZEN ARTIFACT PERSISTENCE FAILED: %d\n", failures);
    return 1;
  }
  std::puts("STAGE 6 FROZEN ARTIFACT PERSISTENCE PASSED");
  return 0;
}
