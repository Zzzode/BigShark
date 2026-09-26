// stage6/frozen_artifact.cpp — canonical little-endian serialization and
// FNV-1a content hashing for one frozen geometry-bucket artifact.
//
// Blob layout (rows.bin):
//   magic "BS6ART01" (8 bytes)
//   u64 row_count
//   per row:
//     u64 artifact_content_hash, u64 tree_node_index, u32 own_card_bucket,
//     u64 path_hash
//     u32 action_count; per action: u8 type, u64 target_total
//     u64 visits
//     u32 probability_count; per probability: f64 little-endian
// Keys are written in std::map (lexicographic) order so the bytes are
// canonical. manifest.bin is a second canonical blob of identity fields; the
// loader recomputes the rows hash and refuses any mismatch.
#include <bit>
#include <bs/heads_up.hpp>
#include <bs/stage6/frozen_manifest.hpp>
#include <bs/stage6/infoset_id.hpp>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>

namespace bs::stage6 {

namespace {

constexpr char kMagic[8] = {'B', 'S', '6', 'A', 'R', 'T', '0', '1'};

void append_u8(std::string* out, std::uint8_t v) {
  out->push_back(static_cast<char>(v));
}
void append_u32(std::string* out, std::uint32_t v) {
  for (unsigned s = 0; s < 32; s += 8)
    out->push_back(static_cast<char>((v >> s) & 0xff));
}
void append_u64(std::string* out, std::uint64_t v) {
  for (unsigned s = 0; s < 64; s += 8)
    out->push_back(static_cast<char>((v >> s) & 0xff));
}
void append_f64(std::string* out, double v) {
  std::uint64_t bits;
  std::memcpy(&bits, &v, sizeof(bits));
  append_u64(out, bits);
}
void append_string(std::string* out, const std::string& s) {
  append_u32(out, static_cast<std::uint32_t>(s.size()));
  *out += s;
}

std::uint64_t fnv1a(const std::string& bytes) {
  std::uint64_t hash = 0xcbf29ce484222325ULL;
  for (unsigned char byte : bytes) {
    hash ^= byte;
    hash *= 0x100000001b3ULL;
  }
  return hash;
}

class Reader {
 public:
  explicit Reader(std::string data) : data_(std::move(data)) {}

  std::uint8_t u8() { return static_cast<std::uint8_t>(take(1)[0]); }
  std::uint32_t u32() {
    std::uint32_t v = 0;
    const char* p = take(4);
    for (unsigned s = 0; s < 32; s += 8)
      v |= static_cast<std::uint32_t>(static_cast<unsigned char>(p[s / 8])) << s;
    return v;
  }
  std::uint64_t u64() {
    std::uint64_t v = 0;
    const char* p = take(8);
    for (unsigned s = 0; s < 64; s += 8)
      v |= static_cast<std::uint64_t>(static_cast<unsigned char>(p[s / 8])) << s;
    return v;
  }
  double f64() {
    const std::uint64_t bits = u64();
    double v;
    std::memcpy(&v, &bits, sizeof(v));
    return v;
  }
  std::string str() {
    const std::uint32_t n = u32();
    return std::string(take(n), n);
  }
  void expect_magic() {
    const char* p = take(8);
    if (std::memcmp(p, kMagic, 8) != 0)
      throw frozen_artifact_error("artifact blob has a bad magic header");
  }
  bool eof() const { return pos_ == data_.size(); }

 private:
  const char* take(std::size_t n) {
    if (pos_ + n > data_.size())
      throw frozen_artifact_error("artifact blob is truncated");
    const char* p = data_.data() + pos_;
    pos_ += n;
    return p;
  }
  std::string data_;
  std::size_t pos_ = 0;
};

std::string encode_rows(const FrozenArtifactRows& rows, std::uint64_t* content_hash) {
  std::string out;
  out.append(kMagic, 8);
  append_u64(&out, rows.size());
  for (const auto& [key, row] : rows) {
    append_u64(&out, key.artifact_content_hash);
    append_u64(&out, key.tree_node_index);
    append_u32(&out, key.own_card_bucket);
    append_u64(&out, key.path_hash);
    if (row.abstract_actions.size() != row.probabilities.size())
      throw frozen_artifact_error("artifact row actions/probabilities length mismatch");
    append_u32(&out, static_cast<std::uint32_t>(row.abstract_actions.size()));
    for (const poker::Action& action : row.abstract_actions) {
      append_u8(&out, static_cast<std::uint8_t>(static_cast<int>(action.type)));
      append_u64(&out, action.target_total);
    }
    append_u64(&out, row.visits);
    append_u32(&out, static_cast<std::uint32_t>(row.probabilities.size()));
    for (double p : row.probabilities)
      append_f64(&out, p);
  }
  *content_hash = fnv1a(out);
  return out;
}

std::string encode_manifest(const FrozenManifest& m) {
  std::string out;
  out.append(kMagic, 8);
  append_string(&out, m.action_abstraction.name);
  append_u32(&out, m.action_abstraction.version);
  append_string(&out, m.action_abstraction.parameters);
  append_u64(&out, m.action_abstraction.digest);
  append_string(&out, m.card_abstraction.name);
  append_u32(&out, m.card_abstraction.version);
  append_string(&out, m.card_abstraction.parameters);
  append_u64(&out, m.card_abstraction.digest);
  append_string(&out, m.translator.name);
  append_u32(&out, m.translator.version);
  append_string(&out, m.translator.parameters);
  append_u64(&out, m.translator.digest);
  append_u64(&out, m.geometry_matrix_hash);
  append_u64(&out, m.bucket.player_count);
  append_u64(&out, m.bucket.pot_bb);
  append_u64(&out, m.bucket.live_count);
  append_u64(&out, m.bucket.acting_count);
  append_string(&out, m.chart_digest_sha256);
  append_u64(&out, m.training_config_hash);
  append_u64(&out, m.iterations_completed);
  append_u64(&out, m.artifact_content_hash);
  append_u64(&out, m.rows_content_hash);
  append_u64(&out, m.iterations);
  append_u64(&out, m.master_seed);
  return out;
}

void write_file(const std::filesystem::path& path, const std::string& bytes) {
  std::ofstream os(path, std::ios::binary | std::ios::trunc);
  if (!os)
    throw frozen_artifact_error("cannot open artifact file for write: " + path.string());
  os.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  if (!os)
    throw frozen_artifact_error("failed writing artifact file: " + path.string());
}

std::string read_file(const std::filesystem::path& path) {
  std::ifstream is(path, std::ios::binary);
  if (!is)
    throw frozen_artifact_error("cannot open artifact file for read: " + path.string());
  return std::string(std::istreambuf_iterator<char>(is), std::istreambuf_iterator<char>());
}

}  // namespace

std::uint64_t hash_artifact_rows(const FrozenArtifactRows& rows) {
  std::uint64_t hash = 0;
  (void)encode_rows(rows, &hash);
  return hash;
}

void write_artifact_dir(const std::string& dir, const FrozenManifest& manifest_in,
                        const FrozenArtifactRows& rows) {
  FrozenManifest manifest = manifest_in;
  std::string rows_blob = encode_rows(rows, &manifest.rows_content_hash);
  const std::filesystem::path root(dir);
  std::filesystem::create_directories(root);
  write_file(root / "rows.bin", rows_blob);
  write_file(root / "manifest.bin", encode_manifest(manifest));
}

LoadedFrozenArtifact load_artifact_dir(const std::string& dir) {
  const std::filesystem::path root(dir);
  Reader rows_reader(read_file(root / "rows.bin"));
  rows_reader.expect_magic();
  const std::uint64_t row_count = rows_reader.u64();
  FrozenArtifactRows rows;
  for (std::uint64_t i = 0; i < row_count; ++i) {
    AbstractInfosetKey key;
    key.artifact_content_hash = rows_reader.u64();
    key.tree_node_index = rows_reader.u64();
    key.own_card_bucket = rows_reader.u32();
    key.path_hash = rows_reader.u64();
    AbstractPolicyRow row;
    const std::uint32_t action_count = rows_reader.u32();
    row.abstract_actions.reserve(action_count);
    for (std::uint32_t a = 0; a < action_count; ++a) {
      const auto type = static_cast<poker::ActionType>(rows_reader.u8());
      const poker::Chips target = rows_reader.u64();
      row.abstract_actions.push_back(poker::Action{type, target});
    }
    row.visits = rows_reader.u64();
    const std::uint32_t prob_count = rows_reader.u32();
    row.probabilities.reserve(prob_count);
    for (std::uint32_t p = 0; p < prob_count; ++p)
      row.probabilities.push_back(rows_reader.f64());
    if (rows.emplace(key, std::move(row)).second == false)
      throw frozen_artifact_error("artifact blob contains a duplicate information-set key");
  }
  if (!rows_reader.eof())
    throw frozen_artifact_error("artifact rows blob has trailing bytes");

  // The manifest blob carries the recorded rows hash; recompute from the rows
  // actually decoded and compare in the caller-facing struct.
  Reader manifest_reader(read_file(root / "manifest.bin"));
  manifest_reader.expect_magic();
  FrozenManifest m;
  m.action_abstraction.name = manifest_reader.str();
  m.action_abstraction.version = manifest_reader.u32();
  m.action_abstraction.parameters = manifest_reader.str();
  m.action_abstraction.digest = manifest_reader.u64();
  m.card_abstraction.name = manifest_reader.str();
  m.card_abstraction.version = manifest_reader.u32();
  m.card_abstraction.parameters = manifest_reader.str();
  m.card_abstraction.digest = manifest_reader.u64();
  m.translator.name = manifest_reader.str();
  m.translator.version = manifest_reader.u32();
  m.translator.parameters = manifest_reader.str();
  m.translator.digest = manifest_reader.u64();
  m.geometry_matrix_hash = manifest_reader.u64();
  m.bucket.player_count = manifest_reader.u64();
  m.bucket.pot_bb = manifest_reader.u64();
  m.bucket.live_count = manifest_reader.u64();
  m.bucket.acting_count = manifest_reader.u64();
  m.chart_digest_sha256 = manifest_reader.str();
  m.training_config_hash = manifest_reader.u64();
  m.iterations_completed = manifest_reader.u64();
  m.artifact_content_hash = manifest_reader.u64();
  const std::uint64_t recorded_rows_hash = manifest_reader.u64();
  m.rows_content_hash = recorded_rows_hash;
  m.iterations = manifest_reader.u64();
  m.master_seed = manifest_reader.u64();

  const std::uint64_t actual_rows_hash = hash_artifact_rows(rows);
  if (actual_rows_hash != recorded_rows_hash)
    throw frozen_artifact_error("artifact rows content hash does not match the manifest");
  return {m, std::move(rows)};
}

}  // namespace bs::stage6
