// RFC 0013 Stage 2: NSeatPolicy binary serialization (implementation).
// RFC 0014: zstd compression (NSPZ format) for on-disk policies.

#include <zstd.h>

#include <bs/nseat_policy_codec.hpp>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
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
  std::uint32_t v;
  in.read(reinterpret_cast<char*>(&v), 4);
  return v;
}

std::uint64_t read_u64(std::istream& in) {
  std::uint64_t v;
  in.read(reinterpret_cast<char*>(&v), 8);
  return v;
}

std::int32_t read_i32(std::istream& in) {
  std::int32_t v;
  in.read(reinterpret_cast<char*>(&v), 4);
  return v;
}

double read_double(std::istream& in) {
  double v;
  in.read(reinterpret_cast<char*>(&v), 8);
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

// Deserialize the NSP1 payload from a stream.
NSeatPolicy deserialize_payload(std::istream& in, const poker::GameDef& game,
                                const abstraction::AbstractionId& action_id,
                                std::uint8_t expected_spr_bucket,
                                const std::string& path_for_errors) {
  // Header.
  char magic[4];
  in.read(magic, 4);
  if (std::memcmp(magic, kMagic, 4) != 0)
    throw std::runtime_error("nseat_policy_codec: bad payload magic in " + path_for_errors);

  const std::uint32_t version = read_u32(in);
  if (version != kVersion)
    throw std::runtime_error("nseat_policy_codec: unsupported version " + std::to_string(version) +
                             " in " + path_for_errors);

  const int seat_count = in.get();
  if (static_cast<std::size_t>(seat_count) != game.player_count)
    throw std::runtime_error("nseat_policy_codec: seat count mismatch in " + path_for_errors);

  const int spr_bucket = in.get();
  // Backward compatibility: policies saved before the bucket was stored
  // have 0 here regardless of the actual bucket. The filename carries the
  // correct bucket, so accept 0 as "not stored" and validate only when the
  // header actually records a bucket.
  if (spr_bucket != 0 && spr_bucket != static_cast<int>(expected_spr_bucket))
    throw std::runtime_error("nseat_policy_codec: SPR bucket mismatch in " + path_for_errors +
                             " (file=" + std::to_string(spr_bucket) + " expected=" +
                             std::to_string(static_cast<int>(expected_spr_bucket)) + ")");

  in.get();
  in.get();  // reserved

  const std::uint32_t row_count = read_u32(in);
  const std::int32_t flop0 = read_i32(in);
  const std::int32_t flop1 = read_i32(in);
  const std::int32_t flop2 = read_i32(in);

  // Verify the flop matches the game def.
  if (flop0 != game.board[0] || flop1 != game.board[1] || flop2 != game.board[2])
    throw std::runtime_error("nseat_policy_codec: flop mismatch in " + path_for_errors);

  for (int i = 0; i < 8; ++i)
    in.get();  // padding

  // Rows.
  std::map<NSeatInformationKey, NSeatPolicyRow> rows;

  for (std::uint32_t r = 0; r < row_count; ++r) {
    const std::uint32_t own_card_bucket = read_u32(in);
    const std::uint64_t node_index = read_u64(in);
    const std::uint32_t action_count = read_u32(in);
    for (int i = 0; i < 4; ++i)
      in.get();  // padding

    NSeatPolicyRow row;
    row.actions.resize(action_count);
    row.probabilities.resize(action_count);

    for (std::uint32_t a = 0; a < action_count; ++a) {
      const std::uint8_t type_byte = static_cast<std::uint8_t>(in.get());
      in.get();
      in.get();
      in.get();  // padding
      const std::uint32_t target = read_u32(in);
      row.actions[a] = poker::Action{byte_to_action_type(type_byte), target};
    }
    for (std::uint32_t a = 0; a < action_count; ++a) {
      const double prob = read_double(in);
      if (!std::isfinite(prob))
        throw std::runtime_error("nseat_policy_codec: non-finite probability in " +
                                 path_for_errors);
      row.probabilities[a] = prob;
    }

    rows.emplace(NSeatInformationKey{own_card_bucket, node_index}, std::move(row));
  }

  check_read(in, "read payload from " + path_for_errors);

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
                              std::uint8_t expected_spr_bucket) {
  std::ifstream in(path, std::ios::binary | std::ios::ate);
  if (!in)
    throw std::runtime_error("nseat_policy_codec: cannot open " + path.string() + " for read");

  const std::size_t file_size = static_cast<std::size_t>(in.tellg());
  in.seekg(0);

  // Read magic to detect format.
  char magic[4];
  in.read(magic, 4);
  check_read(in, "read magic from " + path.string());

  std::vector<char> payload;

  if (std::memcmp(magic, kMagic, 4) == 0) {
    // NSP1 raw format: read the entire file into a buffer.
    in.seekg(0);
    payload.resize(file_size);
    in.read(payload.data(), static_cast<std::streamsize>(file_size));
    check_read(in, "read payload from " + path.string());
  } else if (std::memcmp(magic, kMagicCompressed, 4) == 0) {
    // NSPZ compressed format: read header, decompress.
    const std::uint32_t version = read_u32(in);
    if (version != kVersion)
      throw std::runtime_error("nseat_policy_codec: unsupported NSPZ version " +
                               std::to_string(version) + " in " + path.string());

    const std::uint64_t uncompressed_size = read_u64(in);
    if (uncompressed_size == 0)
      throw std::runtime_error("nseat_policy_codec: zero uncompressed size in " + path.string());

    const std::size_t compressed_size = file_size - 16;  // 4 + 4 + 8
    std::vector<char> compressed(compressed_size);
    in.read(compressed.data(), static_cast<std::streamsize>(compressed_size));
    check_read(in, "read compressed data from " + path.string());

    payload.resize(static_cast<std::size_t>(uncompressed_size));
    const std::size_t actual_size =
        ZSTD_decompress(payload.data(), payload.size(), compressed.data(), compressed.size());
    if (ZSTD_isError(actual_size))
      throw std::runtime_error("nseat_policy_codec: zstd decompress failed for " + path.string() +
                               ": " + std::string(ZSTD_getErrorName(actual_size)));
    payload.resize(actual_size);
  } else {
    throw std::runtime_error("nseat_policy_codec: bad magic in " + path.string());
  }

  // Parse the NSP1 payload from a string stream.
  std::istringstream iss(std::string(payload.data(), payload.size()), std::ios::binary);
  return deserialize_payload(iss, game, action_id, expected_spr_bucket, path.string());
}

std::string policy_file_name(std::span<const int> flop_class, std::uint8_t spr_bucket) {
  char buf[64];
  std::snprintf(buf, sizeof(buf), "policy_%d_%d_%d_b%u.nsp", flop_class[0], flop_class[1],
                flop_class[2], static_cast<unsigned>(spr_bucket));
  return std::string(buf);
}

}  // namespace bs::solver
