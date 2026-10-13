// RFC 0013 Stage 2: NSeatPolicy binary serialization (implementation).
// RFC 0014: zstd compression (NSPZ format) for on-disk policies.

#include <zstd.h>

#include <bs/nseat_policy_codec.hpp>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace bs::solver {

namespace {

constexpr char kMagic[4] = {'N', 'S', 'P', '1'};
constexpr char kMagicCompressed[4] = {'N', 'S', 'P', 'Z'};
constexpr std::uint32_t kVersion = 1;
constexpr std::size_t kHeaderSize = 36;
constexpr int kZstdLevel = 3;
using StoredRow = std::pair<const NSeatInformationKey, NSeatPolicyRow>;
constexpr std::size_t kMapRowBytes = sizeof(StoredRow) + 64;

void require_valid(bool valid, const char* what) {
  if (!valid)
    throw std::runtime_error(std::string("nseat_policy_codec: ") + what);
}

class LoadBudget {
 public:
  explicit LoadBudget(std::size_t limit) : limit_(limit) {}
  void charge(std::size_t count, std::size_t width = 1) {
    if (count > (limit_ - bytes_) / width)
      throw tree::tree_resource_exhausted("nseat policy load exceeded the byte cap");
    bytes_ += count * width;
  }
  void release(std::size_t bytes) { bytes_ -= bytes; }

 private:
  std::size_t limit_;
  std::size_t bytes_ = 0;
};

class Cursor {
 public:
  explicit Cursor(const std::vector<char>& bytes) : bytes_(bytes) {}
  template <typename T>
  T read() {
    require_valid(sizeof(T) <= remaining(), "truncated payload");
    T value;
    std::memcpy(&value, bytes_.data() + offset_, sizeof(T));
    offset_ += sizeof(T);
    return value;
  }
  void skip(std::size_t count) {
    require_valid(count <= remaining(), "truncated payload padding");
    offset_ += count;
  }
  std::size_t remaining() const { return bytes_.size() - offset_; }

 private:
  const std::vector<char>& bytes_;
  std::size_t offset_ = 0;
};

std::uint8_t action_type_to_byte(poker::ActionType type) {
  switch (type) {
    case poker::ActionType::Fold:
      return 0;
    case poker::ActionType::Check:
      return 1;
    case poker::ActionType::Call:
      return 2;
    case poker::ActionType::Bet:
      return 3;
    case poker::ActionType::Raise:
      return 4;
  }
  throw std::invalid_argument("nseat_policy_codec: unknown action type");
}

poker::ActionType byte_to_action_type(std::uint8_t b) {
  switch (b) {
    case 0:
      return poker::ActionType::Fold;
    case 1:
      return poker::ActionType::Check;
    case 2:
      return poker::ActionType::Call;
    case 3:
      return poker::ActionType::Bet;
    case 4:
      return poker::ActionType::Raise;
    default:
      throw std::runtime_error("nseat_policy_codec: corrupt action type byte");
  }
}

void write_u32(std::ostream& out, std::uint32_t v) {
  out.write(reinterpret_cast<const char*>(&v), 4);
}

void write_u64(std::ostream& out, std::uint64_t v) {
  out.write(reinterpret_cast<const char*>(&v), 8);
}

void write_i32(std::ostream& out, std::int32_t v) {
  out.write(reinterpret_cast<const char*>(&v), 4);
}

void write_double(std::ostream& out, double v) {
  out.write(reinterpret_cast<const char*>(&v), 8);
}

std::uint32_t read_u32(std::istream& in) {
  std::uint32_t v = 0;
  in.read(reinterpret_cast<char*>(&v), 4);
  require_valid(static_cast<bool>(in), "truncated compressed header");
  return v;
}

std::uint64_t read_u64(std::istream& in) {
  std::uint64_t v = 0;
  in.read(reinterpret_cast<char*>(&v), 8);
  require_valid(static_cast<bool>(in), "truncated compressed header");
  return v;
}

void check_read(std::istream& in, const std::string& what) {
  if (!in)
    throw std::runtime_error("nseat_policy_codec: " + what + " failed");
}

// Serialize the NSP1 payload to a stream.
void serialize_payload(const NSeatPolicy& policy, std::uint8_t spr_bucket, std::ostream& out) {
  const auto& rows = policy.rows();
  const auto& game = policy.game();

  // Header.
  out.write(kMagic, 4);
  write_u32(out, kVersion);
  out.put(static_cast<char>(game.player_count));
  out.put(static_cast<char>(spr_bucket));
  out.put(0);
  out.put(0);  // reserved
  write_u32(out, static_cast<std::uint32_t>(rows.size()));
  write_i32(out, game.board[0]);
  write_i32(out, game.board[1]);
  write_i32(out, game.board[2]);
  for (int i = 0; i < 8; ++i)
    out.put(0);  // padding

  // Rows.
  for (const auto& [key, row] : rows) {
    write_u32(out, key.own_card_bucket);
    write_u64(out, key.node_index);
    write_u32(out, static_cast<std::uint32_t>(row.actions.size()));
    for (int i = 0; i < 4; ++i)
      out.put(0);  // padding

    for (const auto& action : row.actions) {
      out.put(static_cast<char>(action_type_to_byte(action.type)));
      out.put(0);
      out.put(0);
      out.put(0);  // padding
      write_u32(out, static_cast<std::uint32_t>(action.target_total));
    }
    for (double prob : row.probabilities) {
      if (!std::isfinite(prob))
        throw std::invalid_argument("nseat_policy_codec: non-finite probability");
      write_double(out, prob);
    }
  }

  if (!out)
    throw std::runtime_error("nseat_policy_codec: write payload failed");
}

// Decode directly from the bounded buffer, avoiding stream/string copies.
NSeatPolicy deserialize_payload(const std::vector<char>& payload, const poker::GameDef& game,
                                const abstraction::AbstractionId& action_id,
                                std::uint8_t expected_spr_bucket,
                                const std::string& path_for_errors, LoadBudget& budget) {
  Cursor cursor(payload);
  // Header.
  char magic[4];
  for (char& c : magic)
    c = cursor.read<char>();
  if (std::memcmp(magic, kMagic, 4) != 0)
    throw std::runtime_error("nseat_policy_codec: bad payload magic in " + path_for_errors);

  const std::uint32_t version = cursor.read<std::uint32_t>();
  if (version != kVersion)
    throw std::runtime_error("nseat_policy_codec: unsupported version " + std::to_string(version) +
                             " in " + path_for_errors);

  const int seat_count = cursor.read<std::uint8_t>();
  if (static_cast<std::size_t>(seat_count) != game.player_count)
    throw std::runtime_error("nseat_policy_codec: seat count mismatch in " + path_for_errors);

  const int spr_bucket = cursor.read<std::uint8_t>();
  // Backward compatibility: policies saved before the bucket was stored
  // have 0 here regardless of the actual bucket. The filename carries the
  // correct bucket, so accept 0 as "not stored" and validate only when the
  // header actually records a bucket.
  if (spr_bucket != 0 && spr_bucket != static_cast<int>(expected_spr_bucket))
    throw std::runtime_error("nseat_policy_codec: SPR bucket mismatch in " + path_for_errors +
                             " (file=" + std::to_string(spr_bucket) + " expected=" +
                             std::to_string(static_cast<int>(expected_spr_bucket)) + ")");

  require_valid(cursor.read<std::uint16_t>() == 0, "invalid reserved header field");

  const std::uint32_t row_count = cursor.read<std::uint32_t>();
  const std::int32_t flop0 = cursor.read<std::int32_t>();
  const std::int32_t flop1 = cursor.read<std::int32_t>();
  const std::int32_t flop2 = cursor.read<std::int32_t>();

  // Verify the flop matches the game def.
  if (flop0 != game.board[0] || flop1 != game.board[1] || flop2 != game.board[2])
    throw std::runtime_error("nseat_policy_codec: flop mismatch in " + path_for_errors);

  cursor.skip(8);
  require_valid(row_count <= cursor.remaining() / 36, "row count exceeds payload");
  budget.charge(sizeof(NSeatPolicy));
  // Copy construction retains at most each source string's capacity. Include
  // the null terminator and conservatively count inline string storage too.
  budget.charge(action_id.name.capacity());
  budget.charge(1);
  budget.charge(action_id.parameters.capacity());
  budget.charge(1);
  budget.charge(row_count, kMapRowBytes);

  // Rows.
  std::map<NSeatInformationKey, NSeatPolicyRow> rows;

  for (std::uint32_t r = 0; r < row_count; ++r) {
    const std::uint32_t own_card_bucket = cursor.read<std::uint32_t>();
    const std::uint64_t node_index = cursor.read<std::uint64_t>();
    const std::uint32_t action_count = cursor.read<std::uint32_t>();
    cursor.skip(4);
    require_valid(
        game.board_size == 0 ? own_card_bucket < 169 : own_card_bucket >= 1 && own_card_bucket <= 9,
        "invalid own-card bucket");
    require_valid(node_index < tree::kNoNode, "invalid node index");
    const NSeatInformationKey key{own_card_bucket, node_index};
    require_valid(!rows.contains(key), "duplicate information key");
    require_valid(action_count >= 1 && action_count <= 32, "invalid action count");
    require_valid(action_count <= cursor.remaining() / 16, "truncated policy row");
    budget.charge(action_count, sizeof(poker::Action));
    budget.charge(action_count, sizeof(double));

    NSeatPolicyRow row;
    row.actions.resize(action_count);
    row.probabilities.resize(action_count);

    for (std::uint32_t a = 0; a < action_count; ++a) {
      const std::uint8_t type_byte = cursor.read<std::uint8_t>();
      cursor.skip(3);
      const std::uint32_t target = cursor.read<std::uint32_t>();
      row.actions[a] = poker::Action{byte_to_action_type(type_byte), target};
      if (row.actions[a].type == poker::ActionType::Bet ||
          row.actions[a].type == poker::ActionType::Raise)
        require_valid(target > 0, "zero aggressive action target");
      for (std::uint32_t previous = 0; previous < a; ++previous)
        require_valid(row.actions[previous] != row.actions[a], "duplicate row action");
    }
    double sum = 0;
    for (std::uint32_t a = 0; a < action_count; ++a) {
      const double prob = cursor.read<double>();
      require_valid(std::isfinite(prob) && prob >= 0 && prob <= 1, "invalid action probability");
      row.probabilities[a] = prob;
      sum += prob;
    }
    require_valid(std::abs(sum - 1.0) <= 1e-9, "probabilities are not normalized");

    rows.emplace_hint(rows.end(), key, std::move(row));
  }

  require_valid(cursor.remaining() == 0, "trailing payload bytes");

  return NSeatPolicy::from_parts(game, action_id, std::move(rows));
}

}  // namespace

void save_nseat_policy(const NSeatPolicy& policy, std::uint8_t spr_bucket,
                       const std::filesystem::path& path) {
  // Serialize the NSP1 payload to an in-memory buffer.
  std::ostringstream oss(std::ios::binary);
  serialize_payload(policy, spr_bucket, oss);
  const std::string raw = oss.str();

  // Compress with zstd.
  const std::size_t compressed_bound = ZSTD_compressBound(raw.size());
  std::vector<char> compressed(compressed_bound);
  const std::size_t compressed_size =
      ZSTD_compress(compressed.data(), compressed.size(), raw.data(), raw.size(), kZstdLevel);
  if (ZSTD_isError(compressed_size))
    throw std::runtime_error("nseat_policy_codec: zstd compress failed: " +
                             std::string(ZSTD_getErrorName(compressed_size)));
  compressed.resize(compressed_size);

  // Write to a temp file first, then rename — atomic on POSIX so a crash
  // mid-write does not leave a corrupt policy that the oracle's resume path
  // would treat as permanently failed.
  const std::filesystem::path tmp = std::filesystem::path(path.string() + ".tmp");

  std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
  if (!out)
    throw std::runtime_error("nseat_policy_codec: cannot open " + tmp.string() + " for write");

  // NSPZ header.
  out.write(kMagicCompressed, 4);
  write_u32(out, kVersion);
  write_u64(out, static_cast<std::uint64_t>(raw.size()));
  out.write(compressed.data(), static_cast<std::streamsize>(compressed.size()));

  if (!out)
    throw std::runtime_error("nseat_policy_codec: write failed for " + tmp.string());
  out.close();

  std::error_code ec;
  std::filesystem::rename(tmp, path, ec);
  if (ec)
    throw std::runtime_error("nseat_policy_codec: rename failed for " + path.string() + ": " +
                             ec.message());
}

NSeatPolicy load_nseat_policy(const std::filesystem::path& path, const poker::GameDef& game,
                              const abstraction::AbstractionId& action_id,
                              std::uint8_t expected_spr_bucket, std::size_t max_bytes) {
  poker::validate(game);
  std::ifstream in(path, std::ios::binary | std::ios::ate);
  if (!in)
    throw std::runtime_error("nseat_policy_codec: cannot open " + path.string() + " for read");

  const auto file_end = in.tellg();
  require_valid(file_end >= 0, "cannot determine file size");
  const auto file_size = static_cast<std::uint64_t>(file_end);
  if (file_size > max_bytes ||
      file_size > static_cast<std::uint64_t>(std::numeric_limits<std::streamsize>::max()))
    throw tree::tree_resource_exhausted("nseat policy file exceeded the byte cap");
  LoadBudget budget(max_bytes);
  in.seekg(0);

  // Read magic to detect format.
  char magic[4];
  in.read(magic, 4);
  check_read(in, "read magic from " + path.string());

  std::vector<char> payload;

  if (std::memcmp(magic, kMagic, 4) == 0) {
    // NSP1 raw format: read the entire file into a buffer.
    in.seekg(0);
    require_valid(file_size >= kHeaderSize, "truncated raw header");
    budget.charge(static_cast<std::size_t>(file_size));
    payload.resize(static_cast<std::size_t>(file_size));
    in.read(payload.data(), static_cast<std::streamsize>(file_size));
    check_read(in, "read payload from " + path.string());
  } else if (std::memcmp(magic, kMagicCompressed, 4) == 0) {
    // NSPZ compressed format: read header, decompress.
    require_valid(file_size > 16, "truncated compressed file");
    const std::uint32_t version = read_u32(in);
    if (version != kVersion)
      throw std::runtime_error("nseat_policy_codec: unsupported NSPZ version " +
                               std::to_string(version) + " in " + path.string());

    const std::uint64_t uncompressed_size = read_u64(in);
    require_valid(uncompressed_size >= kHeaderSize, "invalid uncompressed size");
    if (uncompressed_size > max_bytes)
      throw tree::tree_resource_exhausted("nseat policy payload exceeded the byte cap");

    const std::size_t compressed_size = static_cast<std::size_t>(file_size - 16);
    budget.charge(compressed_size);
    budget.charge(static_cast<std::size_t>(uncompressed_size));
    std::vector<char> compressed(compressed_size);
    in.read(compressed.data(), static_cast<std::streamsize>(compressed_size));
    check_read(in, "read compressed data from " + path.string());

    const auto frame_size = ZSTD_getFrameContentSize(compressed.data(), compressed.size());
    require_valid(frame_size != ZSTD_CONTENTSIZE_ERROR, "invalid zstd frame");
    require_valid(frame_size == ZSTD_CONTENTSIZE_UNKNOWN || frame_size == uncompressed_size,
                  "compressed frame size disagrees with header");
    const auto frame_bytes = ZSTD_findFrameCompressedSize(compressed.data(), compressed.size());
    require_valid(!ZSTD_isError(frame_bytes) && frame_bytes == compressed_size,
                  "truncated or trailing compressed frame");
    payload.resize(static_cast<std::size_t>(uncompressed_size));
    const std::size_t actual_size =
        ZSTD_decompress(payload.data(), payload.size(), compressed.data(), compressed.size());
    if (ZSTD_isError(actual_size))
      throw std::runtime_error("nseat_policy_codec: zstd decompress failed for " + path.string() +
                               ": " + std::string(ZSTD_getErrorName(actual_size)));
    require_valid(actual_size == uncompressed_size, "decompressed size disagrees with header");
    budget.release(compressed_size);
  } else {
    throw std::runtime_error("nseat_policy_codec: bad magic in " + path.string());
  }

  return deserialize_payload(payload, game, action_id, expected_spr_bucket, path.string(), budget);
}

std::size_t nseat_policy_accounted_bytes(const NSeatPolicy& policy) noexcept {
  std::size_t bytes = sizeof(NSeatPolicy);
  const auto charge = [&bytes](std::size_t count, std::size_t width = 1) {
    if (count > (std::numeric_limits<std::size_t>::max() - bytes) / width)
      bytes = std::numeric_limits<std::size_t>::max();
    else
      bytes += count * width;
  };
  charge(policy.action_id().name.capacity());
  charge(1);
  charge(policy.action_id().parameters.capacity());
  charge(1);
  charge(policy.rows().size(), kMapRowBytes);
  for (const auto& [key, row] : policy.rows()) {
    (void)key;
    charge(row.actions.capacity(), sizeof(poker::Action));
    charge(row.probabilities.capacity(), sizeof(double));
  }
  return bytes;
}

std::string policy_file_name(std::span<const int> flop_class, std::uint8_t spr_bucket) {
  char buf[64];
  std::snprintf(buf, sizeof(buf), "policy_%d_%d_%d_b%u.nsp", flop_class[0], flop_class[1],
                flop_class[2], static_cast<unsigned>(spr_bucket));
  return std::string(buf);
}

}  // namespace bs::solver
