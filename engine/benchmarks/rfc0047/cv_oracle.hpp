// RFC 0047 Stage-1 offline per-combo turn continuation-value oracle codec
// (skeleton; the deterministic exact BUILDER arrives in S2).
//
// Artifact magic BSNCV01. Immutable, fail-closed, little-endian, content
// integrity by a trailing SHA-256 over every preceding byte (self-contained
// implementation so the guarded benchmark needs no production/artifact
// dependency). Rows are keyed on the POST-CHANCE FIVE-card frontier as RFC
// 0047 B1 requires: (shape id, canonical five-card board, frontier-leaf
// index, seat, dense own-combo) -> chip value and a one-sided certified chip
// error, with the per-key root pot frozen alongside.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <vector>

namespace bs::rfc0047 {

// Self-contained SHA-256 of a byte buffer (FIPS 180-4); exposed so native tests
// can pin the standard one-block vector and so later stages may fingerprint
// builder inputs. Not a production dependency.
std::array<std::uint8_t, 32> sha256_bytes(std::span<const std::uint8_t> data);
std::string sha256_hex(std::span<const std::uint8_t> data);

inline constexpr std::array<char, 8> kCvOracleMagic{'B', 'S', 'N', 'C', 'V', '0', '1', '\n'};
inline constexpr std::uint16_t kCvOracleVersion = 1;

struct OracleKey {
  std::uint8_t shape_id = 0;
  std::array<std::uint8_t, 5> board5{};  // canonical five-card ids 0..51
  std::uint32_t frontier_leaf = 0;       // turn public post-chance leaf index
  std::uint8_t seat = 0;                 // 0 or 1 at HU
  std::uint16_t combo_dense = 0;         // 0..1080 on a five-card board
  bool operator==(const OracleKey&) const = default;
  bool operator<(const OracleKey& o) const;
};

struct OracleRow {
  OracleKey key;
  double cv_chips = 0.0;
  double certified_error_chips = 0.0;  // one-sided, >= 0
  std::uint32_t root_pot_chips = 0;
};

class OracleWriter {
 public:
  explicit OracleWriter(std::uint8_t shape_id, std::uint32_t default_root_pot_chips);
  void add(const OracleRow& row);  // validates fields; duplicates rejected
  // Sorts by key, serializes, appends the digest. Fails closed on IO error.
  void write(const std::filesystem::path& path) const;

 private:
  std::uint8_t shape_id_;
  std::uint32_t default_pot_;
  std::vector<OracleRow> rows_;
  std::set<OracleKey> keys_;  // fast duplicate rejection in add()
};

class OracleReader {
 public:
  // Verifies magic/version/framing/digest/every record, then indexes. Any
  // corruption or malformed field throws std::runtime_error (fail-closed).
  static OracleReader open(const std::filesystem::path& path);

  std::uint8_t shape_id() const { return shape_id_; }
  std::uint32_t default_root_pot() const { return default_pot_; }
  std::size_t size() const { return rows_.size(); }

  // Exact-key lookup; missing returns nullopt (caller fails closed, never
  // substitutes a value).
  const OracleRow* lookup(const OracleKey& key) const;

 private:
  OracleReader() = default;
  std::uint8_t shape_id_ = 0;
  std::uint32_t default_pot_ = 0;
  std::vector<OracleRow> rows_;
};

}  // namespace bs::rfc0047
