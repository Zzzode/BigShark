// RFC 0013 Stage 1: postflop-aware frontier evaluation.
//
// Replaces the all-in-at-flop EquityFrontierEvaluator with a postflop-aware
// evaluator that returns CFR-solved postflop equilibrium values from a
// declared table. The table is keyed by (canonical flop class, SPR bucket,
// per-seat sorted hole cards) and stores chip values at the representative
// pot. The evaluator canonicalizes the flop, computes the SPR from the
// ledger, maps it to a bucket, looks up the value, and scales it to the
// actual pot.
//
// The table is a declared input (RFC 0007 option A, generalized to N seats):
// the evaluator never invents a value. A missing entry throws
// std::invalid_argument (fail closed). The equity frontier is retained for
// stack depths where jam/fold is correct (<= 15 BB).
//
// SPR bucketing (RFC 0013): six buckets (0,1], (1,2], (2,4], (4,7], (7,12],
// (12,inf], each solved at its representative SPR (midpoint, last at 15).
// The bucket boundaries and representative SPRs are declared identity;
// changing them is a new table.
#pragma once

#include <array>
#include <bs/frontier.hpp>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace bs::gto {

// SPR bucketing configuration (RFC 0013). The postflop strategy is primarily
// a function of the SPR, so the declared table is keyed by SPR bucket.
struct SprBucketConfig {
  static constexpr std::size_t kBucketCount = 6;

  // Exclusive lower bounds: (0,1], (1,2], (2,4], (4,7], (7,12], (12,inf].
  static constexpr std::array<double, kBucketCount> kLowerBounds = {0.0, 1.0, 2.0, 4.0, 7.0, 12.0};

  // Representative SPR per bucket: midpoint of the range, last at 15.
  static constexpr std::array<double, kBucketCount> kRepresentativeSpr = {0.5, 1.5, 3.0,
                                                                          5.5, 9.5, 15.0};

  // Map an SPR to its bucket index. Throws std::invalid_argument when spr <= 0
  // or spr is not finite.
  static std::size_t bucket_for_spr(double spr);
};

// N-seat declared frontier table (RFC 0013 Stage 1). Generalizes the
// heads-up-only DeclaredFrontierTable to 2..10 seats. Stores per-deal root
// values keyed by (canonical flop class, SPR bucket, per-seat sorted hole
// cards). Values are chip amounts at the representative pot: the oracle stores
// the per-seat root value from the representative solve, and the evaluator
// scales by pot_actual / representative_pot to reach the actual pot.
//
// The table is immutable after construction. Duplicate keys throw
// std::invalid_argument. Lookup is O(1) via a hash map with an exact key
// (same fail-closed, no-collision discipline as the equity cache).
class NSeatDeclaredFrontierTable {
 public:
  // A single table entry: the canonical flop class representative, the SPR
  // bucket index, per-seat sorted hole cards (in the canonical suit system),
  // and per-seat chip values at the representative pot.
  struct Entry {
    std::array<int, 3> canonical_flop{};
    std::uint8_t spr_bucket = 0;
    std::vector<std::array<int, 2>> hands;
    std::vector<double> values;
  };

  // Table metadata: declared identity of the build. The representative pot
  // per bucket is the pot used in the representative solve; the evaluator
  // scales stored values by pot_actual / representative_pot.
  struct Metadata {
    std::array<double, SprBucketConfig::kBucketCount> representative_pot{};
  };

  // Construct from a list of entries and metadata. Duplicate keys throw
  // std::invalid_argument. The table is immutable after construction.
  NSeatDeclaredFrontierTable(std::vector<Entry> entries, Metadata metadata);

  // The table is movable (needed for load() to return by value) but not
  // copyable (a table can hold millions of entries).
  NSeatDeclaredFrontierTable(NSeatDeclaredFrontierTable&&) = default;
  NSeatDeclaredFrontierTable& operator=(NSeatDeclaredFrontierTable&&) = default;

  // Binary serialization (BSF1 format). The file layout is:
  //   128-byte header:
  //     char     magic[4]          = "BSF1"
  //     uint32   version           = 1
  //     uint8    seat_count        (2..10, uniform across all entries)
  //     uint8    flags             = 0 (reserved)
  //     uint16   reserved          = 0
  //     uint32   entry_count
  //     double   representative_pot[6]
  //     char     build_identity[56]  (null-terminated ASCII)
  //     uint8    padding[8]        (zero)
  //   followed by entry_count fixed-size records:
  //     int32    canonical_flop[3]  (12 bytes)
  //     uint8    spr_bucket         (1 byte)
  //     uint8    pad[3]             (3 bytes, zero)
  //     int32    hands[seat_count][2]  (8 * seat_count bytes)
  //     double   values[seat_count]     (8 * seat_count bytes)
  //   Each record is 16 + 16 * seat_count bytes (48 bytes heads-up).
  //   Native little-endian byte order (x86_64 Linux).
  //
  // save() throws std::invalid_argument if the table is empty, has mixed
  // seat counts, or build_identity exceeds 55 chars. Throws std::runtime_error
  // on I/O failure.
  //
  // load() validates magic, version, seat_count, flags, entry_count vs file
  // size, card ids (0..51), SPR bucket (< 6), and value finiteness. Throws
  // std::runtime_error on any mismatch or I/O failure. SHA-256 is NOT
  // computed inside the codec; callers use bs::artifacts::sha256_file_hex.
  void save(const std::filesystem::path& path, std::string_view build_identity) const;
  static NSeatDeclaredFrontierTable load(const std::filesystem::path& path,
                                         std::string* build_identity_out = nullptr);

  // Look up the per-seat chip values for a (canonical flop, SPR bucket,
  // hands) key. Throws std::invalid_argument on a missing entry (fail
  // closed).
  const std::vector<double>& lookup(const std::array<int, 3>& canonical_flop,
                                    std::uint8_t spr_bucket,
                                    std::span<const std::array<int, 2>> hands) const;

  std::size_t size() const noexcept { return table_.size(); }
  const Metadata& metadata() const noexcept { return metadata_; }

 private:
  // Internal key: canonical flop + SPR bucket + seat count + per-seat hands.
  // Equality is exact (operator==), so a hash collision degrades to a bucket
  // collision, never a wrong value.
  struct Key {
    std::array<int, 3> canonical_flop{};
    std::uint8_t spr_bucket = 0;
    std::size_t seat_count = 0;
    std::array<std::array<int, 2>, poker::kMaxUnifiedSeats> hands{};

    bool operator==(const Key&) const = default;
  };

  // FNV-1a hash over the key's bytes, for use as the unordered_map hasher.
  struct KeyHash {
    std::size_t operator()(const Key& key) const noexcept;
  };

  std::unordered_map<Key, std::vector<double>, KeyHash> table_;
  Metadata metadata_;
};

// Postflop-aware frontier evaluator (RFC 0013 Stage 1). Implements
// FrontierEvaluator. At evaluate(flop, hands, ledger):
//   1. Canonicalize the flop to its suit-isomorphism class representative.
//   2. Relabel each seat's hole cards through the canonical relabel and sort.
//   3. Compute the pot (sum of all seats' contributed) and the effective
//      stack (min over live seats of stack); derive SPR = eff_stack / pot.
//   4. Map the SPR to a bucket via the table's declared boundaries.
//   5. Look up the stored value in the declared table.
//   6. Scale by pot_actual / pot_representative and return per-seat values.
//
// A missing entry throws std::invalid_argument (fail closed). The evaluator
// never invents a value and never falls back to all-in equity.
class PostflopFrontierEvaluator : public FrontierEvaluator {
 public:
  explicit PostflopFrontierEvaluator(const NSeatDeclaredFrontierTable& table);

  std::vector<double> evaluate(std::span<const int> flop, std::span<const std::array<int, 2>> hands,
                               const bs::tree::TerminalPayload& ledger) const override;

 private:
  const NSeatDeclaredFrontierTable& table_;
};

}  // namespace bs::gto
