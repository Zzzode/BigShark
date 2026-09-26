#include <openssl/evp.h>

#include <algorithm>
#include <array>
#include <bs/charts.hpp>
#include <bs/stage6/chart_digest.hpp>
#include <cstdio>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace bs::stage6 {
namespace {

// A small incremental SHA-256 over canonical text fragments. Failure at any
// EVP step is a runtime error rather than a silently wrong digest.
class Sha256 {
 public:
  Sha256() {
    ctx_ = EVP_MD_CTX_new();
    if (!ctx_ || EVP_DigestInit_ex(ctx_, EVP_sha256(), nullptr) != 1)
      throw std::runtime_error("chart digest: SHA-256 init failed");
  }
  ~Sha256() {
    if (ctx_)
      EVP_MD_CTX_free(ctx_);
  }
  Sha256(const Sha256&) = delete;
  Sha256& operator=(const Sha256&) = delete;

  void update(const std::string& fragment) {
    if (EVP_DigestUpdate(ctx_, fragment.data(), fragment.size()) != 1)
      throw std::runtime_error("chart digest: SHA-256 update failed");
  }

  std::string hex() {
    std::array<unsigned char, EVP_MAX_MD_SIZE> out{};
    unsigned int length = 0;
    if (EVP_DigestFinal_ex(ctx_, out.data(), &length) != 1)
      throw std::runtime_error("chart digest: SHA-256 finalize failed");
    std::ostringstream os;
    for (unsigned int i = 0; i < length; ++i) {
      char buf[3];
      std::snprintf(buf, sizeof(buf), "%02x", out[i]);
      os << buf;
    }
    return os.str();
  }

 private:
  EVP_MD_CTX* ctx_ = nullptr;
};

// Emits one labeled, canonicalized field: a path tag, the element count, then
// each element in ascending order. The length framing makes the encoding
// unambiguous (a changed element cannot masquerade as a shifted boundary).
void add_set(Sha256* hash, const std::string& path, const Range169& range) {
  std::vector<std::string> sorted(range.begin(), range.end());
  std::sort(sorted.begin(), sorted.end());
  hash->update("F:" + path + ":" + std::to_string(sorted.size()) + ":");
  for (const std::string& key : sorted)
    hash->update(key + ",");
}

}  // namespace

std::string chart_digest_hex() {
  const Charts& c = charts();
  Sha256 hash;

  // The five RFI ranges, in a fixed position order independent of map layout.
  const std::array<const char*, 5> rfi_positions = {{"UTG", "HJ", "CO", "BTN", "SB"}};
  for (const char* pos : rfi_positions) {
    const auto it = c.rfi.find(pos);
    if (it == c.rfi.end())
      throw std::runtime_error("chart digest: missing rfi range for " + std::string(pos));
    add_set(&hash, std::string("rfi.") + pos, it->second);
  }

  // The five vs-open buckets, each with value/bluff/call.
  const std::array<const char*, 5> vs_buckets = {{"EP", "HJ", "CO", "BTN", "SB"}};
  for (const char* bucket : vs_buckets) {
    const auto it = c.vs.find(bucket);
    if (it == c.vs.end())
      throw std::runtime_error("chart digest: missing vs bucket " + std::string(bucket));
    add_set(&hash, std::string("vs.") + bucket + ".value", it->second.value);
    add_set(&hash, std::string("vs.") + bucket + ".bluff", it->second.bluff);
    add_set(&hash, std::string("vs.") + bucket + ".call", it->second.call);
  }

  // The four standalone sets (5 + 15 + 4 = 24 chart fields).
  add_set(&hash, "fourValue", c.fourValue);
  add_set(&hash, "fourBluff", c.fourBluff);
  add_set(&hash, "vs3Call", c.vs3Call);
  add_set(&hash, "vs4Continue", c.vs4Continue);

  // The 169-entry Chen percentile table in ascending key order.
  const std::unordered_map<std::string, double>& table = preflopPctTable();
  if (table.size() != 169)
    throw std::runtime_error("chart digest: percentile table must have 169 entries");
  std::vector<std::string> keys;
  keys.reserve(table.size());
  for (const auto& [key, value] : table) {
    (void)value;
    keys.push_back(key);
  }
  std::sort(keys.begin(), keys.end());
  hash.update("T:preflopPct:169:");
  for (const std::string& key : keys) {
    // Round-trip-stable fixed precision; the table is constructed from
    // integer ranks, so six decimal places captures every distinct value.
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%s=%.6f;", key.c_str(), table.at(key));
    hash.update(buf);
  }

  return hash.hex();
}

}  // namespace bs::stage6
