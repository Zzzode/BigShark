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
// Probabilities are stored losslessly. A trained artifact has far fewer
// distinct probability values than actions (a few hundred at most for
// shallow-stack libraries), so when a root has at most 256 distinct values
// the index stores a per-root codebook of the distinct doubles plus one
// uint8 index per action. Roots with 257..65536 distinct values use a
// uint16 index per action (Codebook16), which serves deeper-stack
// libraries whose distinct count exceeds the uint8 cap. Roots with more
// than 65536 distinct values fall back to one exact double per action.
// Either way the boundary defers the exact doubles into a scratch buffer
// before exposing them, so the public-belief reach computation (which
// multiplies path probabilities and requires exact marginals) sees
// bitwise-identical values.
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

// Probability storage layout for a built resident index. The layout is chosen
// at build time from the count of distinct probability bit patterns:
//   - Codebook8:  <= 256 distinct values (uint8 index per action)
//   - Codebook16: <= 65536 distinct values (uint16 index per action)
//   - Doubles:    > 65536 distinct values (one exact double per action)
// Codebook16 activates compression for deeper-stack libraries whose distinct
// probability count exceeds the uint8 cap but stays within uint16 range.
enum class ProbabilityLayout : std::uint8_t { Doubles, Codebook8, Codebook16 };

struct CompactRowView {
  std::uint16_t count = 0;
  const CompactAction* actions = nullptr;
  // Probability access. Exactly one layout is active per index, selected by
  // prob_layout:
  //   - Doubles: probabilities[i] is the exact double.
  //   - Codebook8: probability_indices[i] (uint8) selects from codebook.
  //   - Codebook16: probability_indices_16[i] (uint16) selects from codebook.
  // The resident boundary defers either layout into a double scratch before
  // exposing probabilities to callers.
  ProbabilityLayout prob_layout = ProbabilityLayout::Doubles;
  const double* probabilities = nullptr;
  const std::uint8_t* probability_indices = nullptr;
  const std::uint16_t* probability_indices_16 = nullptr;
  const double* codebook = nullptr;
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

  // Conservative upper bound on the resident footprint of one root from an
  // artifact probe's SQL aggregates, BEFORE the policy rows are materialized.
  // It mirrors build()'s exact reservations (two key-header bytes plus compact
  // key bytes, one 3-byte compact action per stored action, one fixed record
  // per state, and the 50 percent-load power-of-two slot table), charges the
  // worst-case probability layout across all three storage modes (one exact
  // double per action in the fallback, one uint8 index per action plus a
  // 256-entry codebook in the Codebook8 mode, or one uint16 index per action
  // plus a 65536-entry codebook in the Codebook16 mode), and overcharges the
  // immutable game-copy vectors with slack for their allocator capacities.
  // Accepted roots are still measured exactly after the index is built; the
  // pre-gate never accepts a root the exact measurement would refuse.
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
  // Probability storage. Exactly one layout is active per built index:
  //   - Codebook8: codebook_ + probability_index_ (uint8 per action)
  //   - Codebook16: codebook_ + probability_index_16_ (uint16 per action)
  //   - Doubles: probability_blob_ (one exact double per action)
  // Codebook8 is the common case for shallow-stack artifacts (a few hundred
  // distinct probabilities at most). Codebook16 serves deeper-stack libraries
  // whose distinct count exceeds 256 but stays within 65536. Doubles is the
  // fallback for very large distinct counts.
  std::vector<double> codebook_;
  std::vector<std::uint8_t> probability_index_;
  std::vector<std::uint16_t> probability_index_16_;
  std::vector<double> probability_blob_;  // 8 bytes per action (fallback)
  std::vector<RowRecord> rows_;
  std::vector<Slot> slots_;
  std::size_t row_count_ = 0;
  std::size_t probability_count_ = 0;
  std::size_t slot_mask_ = 0;
  // Explicit layout discriminator so resolve() does not infer the mode from
  // codebook_.empty() (which disagrees for a degenerate zero-probability index).
  ProbabilityLayout prob_layout_ = ProbabilityLayout::Doubles;
};

}  // namespace bs::resident
