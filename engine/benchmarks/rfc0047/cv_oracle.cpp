#include "cv_oracle.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <stdexcept>

namespace bs::rfc0047 {

// Compact self-contained SHA-256 (FIPS 180-4), used only for artifact
// integrity inside the guarded benchmark. Verified against the FIPS 180-4
// one-block vector "abc" in the codec native test.
class Sha256 {
 public:
  void update(const std::uint8_t* data, std::size_t len) {
    for (std::size_t i = 0; i < len; ++i) {
      buffer_[buf_len_++] = data[i];
      if (buf_len_ == 64) {
        process(buffer_.data());
        buf_len_ = 0;
        bit_len_ += 512;
      }
    }
  }
  std::array<std::uint8_t, 32> finish() {
    const std::uint64_t total_bits = bit_len_ + static_cast<std::uint64_t>(buf_len_) * 8;
    buffer_[buf_len_++] = 0x80;
    if (buf_len_ > 56) {
      while (buf_len_ < 64)
        buffer_[buf_len_++] = 0;
      process(buffer_.data());
      buf_len_ = 0;
    }
    while (buf_len_ < 56)
      buffer_[buf_len_++] = 0;
    for (int i = 7; i >= 0; --i)
      buffer_[buf_len_++] = static_cast<std::uint8_t>((total_bits >> (8 * i)) & 0xFF);
    process(buffer_.data());
    std::array<std::uint8_t, 32> out{};
    for (int i = 0; i < 8; ++i)
      for (int j = 0; j < 4; ++j)
        out[i * 4 + j] = static_cast<std::uint8_t>((state_[i] >> (24 - 8 * j)) & 0xFF);
    return out;
  }

 private:
  static std::uint32_t rotr(std::uint32_t x, std::uint32_t n) { return (x >> n) | (x << (32 - n)); }
  void process(const std::uint8_t* p) {
    std::uint32_t w[64]{};
    for (int i = 0; i < 16; ++i)
      w[i] = (std::uint32_t{p[i * 4]} << 24) | (std::uint32_t{p[i * 4 + 1]} << 16) |
             (std::uint32_t{p[i * 4 + 2]} << 8) | std::uint32_t{p[i * 4 + 3]};
    for (int i = 16; i < 64; ++i) {
      const std::uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
      const std::uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
      w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    std::uint32_t a = state_[0], b = state_[1], c = state_[2], d = state_[3];
    std::uint32_t e = state_[4], f = state_[5], g = state_[6], h = state_[7];
    static const std::uint32_t k[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4,
        0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe,
        0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f,
        0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
        0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc,
        0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
        0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116,
        0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7,
        0xc67178f2};
    for (int i = 0; i < 64; ++i) {
      const std::uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
      const std::uint32_t ch = (e & f) ^ (~e & g);
      const std::uint32_t t1 = h + S1 + ch + k[i] + w[i];
      const std::uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
      const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
      const std::uint32_t t2 = S0 + maj;
      h = g;
      g = f;
      f = e;
      e = d + t1;
      d = c;
      c = b;
      b = a;
      a = t1 + t2;
    }
    state_[0] += a;
    state_[1] += b;
    state_[2] += c;
    state_[3] += d;
    state_[4] += e;
    state_[5] += f;
    state_[6] += g;
    state_[7] += h;
  }
  std::array<std::uint32_t, 8> state_{0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                      0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
  std::array<std::uint8_t, 64> buffer_{};
  std::size_t buf_len_ = 0;
  std::uint64_t bit_len_ = 0;
};

std::array<std::uint8_t, 32> sha256_bytes(std::span<const std::uint8_t> data) {
  Sha256 h;
  h.update(data.data(), data.size());
  return h.finish();
}

std::string sha256_hex(std::span<const std::uint8_t> data) {
  static const char* kHex = "0123456789abcdef";
  const auto d = sha256_bytes(data);
  std::string out;
  out.reserve(64);
  for (std::uint8_t b : d) {
    out.push_back(kHex[b >> 4]);
    out.push_back(kHex[b & 0xF]);
  }
  return out;
}

namespace {

#pragma pack(push, 1)
struct Header {
  char magic[8];
  std::uint16_t version;
  std::uint8_t shape_id;
  std::uint8_t reserved0;
  std::uint32_t record_count;
  std::uint64_t body_bytes;
  std::uint32_t default_root_pot;
  std::uint32_t reserved1;
};
struct Record {
  std::uint8_t shape_id;
  std::uint8_t board5[5];
  std::uint8_t seat;
  std::uint8_t pad0;
  std::uint32_t frontier_leaf;
  std::uint16_t combo_dense;
  std::uint16_t pad1;
  double cv_chips;
  double error_chips;
  std::uint32_t root_pot;
  std::uint32_t pad2;
};
#pragma pack(pop)
static_assert(sizeof(Header) == 32, "BSNCV01 header must be 32 bytes");
static_assert(sizeof(Record) == 40, "BSNCV01 record must be 40 bytes");

void put_u16(std::uint8_t* p, std::uint16_t v) {
  p[0] = std::uint8_t(v);
  p[1] = std::uint8_t(v >> 8);
}
void put_u32(std::uint8_t* p, std::uint32_t v) {
  p[0] = std::uint8_t(v);
  p[1] = std::uint8_t(v >> 8);
  p[2] = std::uint8_t(v >> 16);
  p[3] = std::uint8_t(v >> 24);
}
void put_u64(std::uint8_t* p, std::uint64_t v) {
  for (int i = 0; i < 8; ++i)
    p[i] = std::uint8_t(v >> (8 * i));
}
std::uint16_t get_u16(const std::uint8_t* p) {
  return std::uint16_t(p[0]) | std::uint16_t(p[1] << 8);
}
std::uint32_t get_u32(const std::uint8_t* p) {
  return std::uint32_t(p[0]) | (std::uint32_t(p[1]) << 8) | (std::uint32_t(p[2]) << 16) |
         (std::uint32_t(p[3]) << 24);
}
std::uint64_t get_u64(const std::uint8_t* p) {
  std::uint64_t v = 0;
  for (int i = 0; i < 8; ++i)
    v |= std::uint64_t(p[i]) << (8 * i);
  return v;
}

void validate_row(const OracleRow& r, std::uint8_t writer_shape) {
  if (r.key.shape_id != writer_shape)
    throw std::runtime_error("BSNCV01: row shape id mismatch");
  if (r.key.seat > 1)
    throw std::runtime_error("BSNCV01: seat must be 0 or 1");
  std::array<bool, 52> seen{};
  for (int c : r.key.board5) {
    if (c > 51 || seen[c])
      throw std::runtime_error("BSNCV01: board5 card invalid/duplicate");
    seen[c] = true;
  }
  // five board cards leave 47 live cards -> C(47,2)=1081 combos.
  if (r.key.combo_dense >= 1081)
    throw std::runtime_error("BSNCV01: combo dense id out of range for five-card board");
  for (int c : r.key.board5)
    seen[c] = true;
  // Reconstruct the holding from the dense index and ensure it does not block.
  std::array<int, 47> live{};
  int n = 0;
  for (int c = 0; c < 52; ++c) {
    bool on_board = false;
    for (int b : r.key.board5)
      if (b == c)
        on_board = true;
    if (!on_board)
      live[n++] = c;
  }
  if (n != 47)
    throw std::runtime_error("BSNCV01: five distinct board cards required");
  const std::uint32_t d = r.key.combo_dense;
  std::uint32_t lo = 0, hi = 46;
  while (lo < hi) {
    const std::uint32_t mid = lo + (hi - lo + 1u) / 2u;
    if (mid * 47u - mid * (mid + 1u) / 2u <= d)
      lo = mid;
    else
      hi = mid - 1u;
  }
  const std::uint32_t off = lo * 47u - lo * (lo + 1u) / 2u;
  if (lo + 1u + (d - off) >= 47)
    throw std::runtime_error("BSNCV01: malformed combo dense id");
  if (!std::isfinite(r.cv_chips))
    throw std::runtime_error("BSNCV01: cv must be finite");
  if (!std::isfinite(r.certified_error_chips) || r.certified_error_chips < 0.0)
    throw std::runtime_error("BSNCV01: certified error must be finite and >= 0");
  if (r.root_pot_chips == 0)
    throw std::runtime_error("BSNCV01: root pot must be positive");
}

}  // namespace

bool OracleKey::operator<(const OracleKey& o) const {
  if (shape_id != o.shape_id)
    return shape_id < o.shape_id;
  if (frontier_leaf != o.frontier_leaf)
    return frontier_leaf < o.frontier_leaf;
  for (int i = 0; i < 5; ++i)
    if (board5[i] != o.board5[i])
      return board5[i] < o.board5[i];
  if (seat != o.seat)
    return seat < o.seat;
  return combo_dense < o.combo_dense;
}

OracleWriter::OracleWriter(std::uint8_t shape_id, std::uint32_t default_root_pot_chips)
    : shape_id_(shape_id), default_pot_(default_root_pot_chips) {
  if (default_root_pot_chips == 0)
    throw std::runtime_error("BSNCV01: default root pot must be positive");
}

void OracleWriter::add(const OracleRow& row) {
  OracleRow r = row;
  if (r.root_pot_chips == 0)
    r.root_pot_chips = default_pot_;
  validate_row(r, shape_id_);
  if (!keys_.insert(r.key).second)
    throw std::runtime_error("BSNCV01: duplicate oracle key");
  rows_.push_back(r);
}

void OracleWriter::write(const std::filesystem::path& path) const {
  std::vector<OracleRow> sorted = rows_;
  std::sort(sorted.begin(), sorted.end(),
            [](const OracleRow& a, const OracleRow& b) { return a.key < b.key; });
  for (std::size_t i = 1; i < sorted.size(); ++i)
    if (!(sorted[i - 1].key < sorted[i].key))
      throw std::runtime_error("BSNCV01: duplicate oracle key");

  std::vector<std::uint8_t> bytes(sizeof(Header) + sorted.size() * sizeof(Record));
  Header h{};
  std::memcpy(h.magic, kCvOracleMagic.data(), 8);
  h.version = kCvOracleVersion;
  h.shape_id = shape_id_;
  h.record_count = static_cast<std::uint32_t>(sorted.size());
  h.body_bytes = static_cast<std::uint64_t>(sorted.size() * sizeof(Record));
  h.default_root_pot = default_pot_;
  std::memcpy(bytes.data(), &h, sizeof(h));

  std::size_t off = sizeof(Header);
  for (const OracleRow& r : sorted) {
    Record rec{};
    rec.shape_id = r.key.shape_id;
    std::memcpy(rec.board5, r.key.board5.data(), 5);
    rec.seat = r.key.seat;
    put_u32(reinterpret_cast<std::uint8_t*>(&rec.frontier_leaf), r.key.frontier_leaf);
    put_u16(reinterpret_cast<std::uint8_t*>(&rec.combo_dense), r.key.combo_dense);
    rec.cv_chips = r.cv_chips;
    rec.error_chips = r.certified_error_chips;
    put_u32(reinterpret_cast<std::uint8_t*>(&rec.root_pot), r.root_pot_chips);
    std::memcpy(bytes.data() + off, &rec, sizeof(rec));
    off += sizeof(rec);
  }
  Sha256 hash;
  hash.update(bytes.data(), bytes.size());
  const auto digest = hash.finish();

  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  if (!f)
    throw std::runtime_error("BSNCV01: cannot open file for write: " + path.string());
  f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  f.write(reinterpret_cast<const char*>(digest.data()), digest.size());
  if (!f)
    throw std::runtime_error("BSNCV01: write failed: " + path.string());
}

OracleReader OracleReader::open(const std::filesystem::path& path) {
  std::ifstream f(path, std::ios::binary | std::ios::ate);
  if (!f)
    throw std::runtime_error("BSNCV01: cannot open oracle: " + path.string());
  const std::streamoff size = f.tellg();
  if (size < static_cast<std::streamoff>(sizeof(Header) + 32))
    throw std::runtime_error("BSNCV01: file too small");
  std::vector<std::uint8_t> all(static_cast<std::size_t>(size));
  f.seekg(0);
  f.read(reinterpret_cast<char*>(all.data()), size);
  if (!f)
    throw std::runtime_error("BSNCV01: read failed");

  const std::vector<std::uint8_t> body(all.begin(), all.end() - 32);
  const std::array<std::uint8_t, 32> want = [&] {
    std::array<std::uint8_t, 32> d{};
    std::memcpy(d.data(), all.data() + all.size() - 32, 32);
    return d;
  }();
  Sha256 hash;
  hash.update(body.data(), body.size());
  if (hash.finish() != want)
    throw std::runtime_error("BSNCV01: digest mismatch (corrupt or tampered)");

  if (body.size() < sizeof(Header))
    throw std::runtime_error("BSNCV01: truncated header");
  Header h{};
  std::memcpy(&h, body.data(), sizeof(h));
  if (std::memcmp(h.magic, kCvOracleMagic.data(), 8) != 0)
    throw std::runtime_error("BSNCV01: bad magic");
  if (h.version != kCvOracleVersion)
    throw std::runtime_error("BSNCV01: unsupported version");
  if (h.reserved0 != 0 || h.reserved1 != 0)
    throw std::runtime_error("BSNCV01: nonzero reserved header bytes");
  const std::uint64_t expect = std::uint64_t{h.record_count} * sizeof(Record);
  if (h.body_bytes != expect)
    throw std::runtime_error("BSNCV01: body size mismatch");
  if (body.size() != sizeof(Header) + expect)
    throw std::runtime_error("BSNCV01: framing mismatch");

  OracleReader r;
  r.shape_id_ = h.shape_id;
  r.default_pot_ = h.default_root_pot;
  r.rows_.reserve(h.record_count);
  for (std::uint32_t i = 0; i < h.record_count; ++i) {
    Record rec{};
    std::memcpy(&rec, body.data() + sizeof(Header) + std::size_t{i} * sizeof(Record), sizeof(rec));
    OracleRow row;
    row.key.shape_id = rec.shape_id;
    std::memcpy(row.key.board5.data(), rec.board5, 5);
    row.key.seat = rec.seat;
    row.key.frontier_leaf = get_u32(reinterpret_cast<const std::uint8_t*>(&rec.frontier_leaf));
    row.key.combo_dense = get_u16(reinterpret_cast<const std::uint8_t*>(&rec.combo_dense));
    if (rec.pad0 != 0 || rec.pad1 != 0 || rec.pad2 != 0)
      throw std::runtime_error("BSNCV01: nonzero reserved record bytes");
    row.cv_chips = rec.cv_chips;
    row.certified_error_chips = rec.error_chips;
    row.root_pot_chips = get_u32(reinterpret_cast<const std::uint8_t*>(&rec.root_pot));
    validate_row(row, r.shape_id_);
    r.rows_.push_back(row);
  }
  for (std::size_t i = 1; i < r.rows_.size(); ++i)
    if (!(r.rows_[i - 1].key < r.rows_[i].key))
      throw std::runtime_error("BSNCV01: rows not sorted/unique");
  return r;
}

const OracleRow* OracleReader::lookup(const OracleKey& key) const {
  auto it = std::lower_bound(rows_.begin(), rows_.end(), key,
                             [](const OracleRow& r, const OracleKey& k) { return r.key < k; });
  if (it != rows_.end() && it->key == key)
    return &*it;
  return nullptr;
}

}  // namespace bs::rfc0047
