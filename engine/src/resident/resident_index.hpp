// Compact immutable resident index over a HeadsUpPolicy's rows.
//
// The Stage 5 artifact reader hands us a std::map<InformationKey, PolicyRow>
// (variable-length vector keys, separately allocated actions/probabilities).
// At resident construction time that map is flattened into four contiguous
// immutable buffers: one compact key blob, one compact action blob, one
// probability blob (exact doubles), and an open-addressing slot table plus
// fixed-size row records. Warm lookups hash a caller-supplied compact key,
// probe with full key comparisons, and resolve offsets into the blobs. They
// never allocate.
//
// Compression (vs the original word-key/double-probability layout):
//   - Information keys are packed from uint64 words (32 bytes per history
//     action) to 5 bytes per history action, a ~6x reduction on the key blob.
//   - Actions are packed from poker::Action (16 bytes) to CompactAction
//     (3 bytes), a 5x reduction on the action blob.
//   - Hash slots are reduced from 16 to 8 bytes (32-bit hash), a 2x
//     reduction on the slot table.
// Probabilities stay as double: the public-belief reach computation multiplies
// path probabilities and the resident contract is exact marginals, so a lossy
// probability encoding is not acceptable here.
#pragma once

#include <bs/heads_up.hpp>
#include <bs/heads_up_solver.hpp>
#include <bs/seat_policy.hpp>  // SeatPolicyRow
#include <bs/unified_game.hpp>
#include <cstddef>
#include <cstdint>
#include <map>
#include <span>
#include <vector>

namespace bs::resident {

// Packed 3-byte action: type (0-4, matching poker::ActionType) and target
// total (0-65535, enough for any trained artifact's chip values).
#pragma pack(push, 1)
struct CompactAction {
  std::uint8_t type = 0;
  std::uint16_t target = 0;

  constexpr bool operator==(const CompactAction&) const = default;
  constexpr bool operator==(const poker::Action& other) const noexcept {
    return type == static_cast<std::uint8_t>(other.type) &&
           static_cast<std::uint64_t>(target) == other.target_total;
  }
};
#pragma pack(pop)
static_assert(sizeof(CompactAction) == 3);

// Maximum actions per resident row. The public-view expansion scratch is
// sized to this, and the abstraction builder caps menus at 32 actions, so a
// trained artifact never exceeds it. build_rows rejects anything larger so a
// row that would overflow the scratch can never be stored.
inline constexpr std::size_t kMaxResidentActions = 32;

struct CompactRowView {
  std::uint16_t count = 0;
  const CompactAction* actions = nullptr;
  const double* probabilities = nullptr;
};

class ResidentIndex {
 public:
  ResidentIndex();
  ~ResidentIndex();
  ResidentIndex(const ResidentIndex&) = delete;
  ResidentIndex& operator=(const ResidentIndex&) = delete;
  ResidentIndex(ResidentIndex&&) noexcept;
  ResidentIndex& operator=(ResidentIndex&&) noexcept;

  // Flatten one complete policy's rows into the immutable buffers.
  void build(const std::map<solver::InformationKey, solver::PolicyRow>& rows);
  // RFC 0009 D4: flatten a schema-v2 seat-generic policy's rows. The row layout
  // is identical (actions + probabilities); the v2 visits field is not resident.
  void build(const std::map<solver::InformationKey, solver::SeatPolicyRow>& rows);

  // Compact an information key (uint64 words) into a byte string. Returns the
  // compact size in bytes, or 0 if the output buffer is too small or the key
  // is malformed. The compact format is:
  //   [actor:1][own0:1][own1:1][board_size:1][board:board_size]
  //   then per history action: [street:1][seat:1][type:1][target:2 LE]
  static std::size_t compact_information_key(std::span<const std::uint64_t> words,
                                             std::span<std::uint8_t> output);

  // Exact-key lookup with a compact key produced by compact_information_key.
  // Returns false on a miss; on a hit fills the view with pointers into the
  // immutable blobs.
  bool find(std::span<const std::uint8_t> compact_key, CompactRowView& out) const;

  std::size_t row_count() const noexcept { return row_count_; }
  std::size_t probability_count() const noexcept { return probability_count_; }

  // Honest byte footprint of the resident buffers at their actual capacities.
  std::size_t resident_bytes() const noexcept;

  // Conservative tight upper bound on the resident footprint of one root
  // from an artifact probe's SQL aggregates, BEFORE the policy rows are
  // materialized. It mirrors build()'s exact reservations (two key-header
  // bytes plus compact key bytes, one 3-byte compact action and one exact
  // double per stored action, one fixed record per state, and the
  // 50 percent-load power-of-two slot table) and overcharges the immutable
  // game-copy vectors with slack for their allocator capacities. Accepted
  // roots are still measured exactly after the index is built; the pre-gate
  // never accepts a root the exact measurement would refuse.
  static std::size_t estimate_bytes(std::size_t row_count, std::size_t action_count,
                                    std::size_t total_key_words, const solver::UnifiedGame& game);

  // Test support: decompact a stored key back to uint64 words.
  std::vector<std::uint64_t> key_at(std::size_t row) const;

 private:
  struct RowRecord {
    std::uint32_t key_offset = 0;
    std::uint32_t actions_offset = 0;
    std::uint32_t probabilities_offset = 0;
    std::uint16_t key_length = 0;  // compact key bytes (excludes the 2-byte length prefix)
    std::uint16_t action_count = 0;
  };

  struct Slot {
    std::uint32_t hash = 0;
    std::uint32_t row_plus_one = 0;  // zero means empty
  };

  void resolve(const RowRecord& record, CompactRowView& out) const;

  // Shared flattening for the two row types. Only `.actions` and
  // `.probabilities` are read; the v2 `visits` field is not resident.
  template <class Row>
  void build_rows(const std::map<solver::InformationKey, Row>& rows);

  std::vector<std::uint8_t> key_blob_;      // [2-byte LE length][compact key] per row
  std::vector<CompactAction> action_blob_;  // 3 bytes per action
  std::vector<double> probability_blob_;    // 8 bytes per action (exact)
  std::vector<RowRecord> rows_;
  std::vector<Slot> slots_;
  std::size_t row_count_ = 0;
  std::size_t probability_count_ = 0;
  std::size_t slot_mask_ = 0;
};

}  // namespace bs::resident
