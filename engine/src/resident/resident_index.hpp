// Compact immutable resident index over a HeadsUpPolicy's rows.
//
// The Stage 5 artifact reader hands us a std::map<InformationKey, PolicyRow>
// (variable-length vector keys, separately allocated actions/probabilities).
// At resident construction time that map is flattened into four contiguous
// immutable buffers: one key blob, one action blob, one probability blob, and
// an open-addressing slot table plus fixed-size row records. Warm lookups hash
// a caller-supplied fixed key span, probe with full key comparisons, and
// resolve offsets into the blobs. They never allocate.
#pragma once

#include <array>
#include <bs/heads_up.hpp>
#include <bs/heads_up_solver.hpp>
#include <bs/seat_policy.hpp>  // SeatPolicyRow
#include <bs/unified_game.hpp>
#include <cstddef>
#include <cstdint>
#include <map>
#include <span>
#include <utility>
#include <vector>

namespace bs::resident {

struct CompactRowView {
  std::uint16_t count = 0;
  const poker::Action* actions = nullptr;
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

  // Exact-key lookup. The span is encoded exactly like
  // solver::information_key output. Returns false on a miss; on a hit fills
  // the view with pointers into the immutable blobs.
  bool find(std::span<const std::uint64_t> key, CompactRowView& out) const;

  std::size_t row_count() const noexcept { return row_count_; }
  std::size_t probability_count() const noexcept { return probability_count_; }

  // Honest byte footprint of the resident buffers at their actual capacities.
  std::size_t resident_bytes() const noexcept;

  // Conservative tight upper bound on the resident footprint of one root
  // from an artifact probe's SQL aggregates, BEFORE the policy rows are
  // materialized. It mirrors build()'s exact reservations (eight key-header
  // bytes plus key words, one 16-byte action and one double per stored
  // action, one fixed record per state, and the 50 percent-load power-of-two
  // slot table) and overcharges the immutable game-copy vectors with slack
  // for their allocator capacities. Accepted roots are still measured
  // exactly after the index is built; the pre-gate never accepts a root the
  // exact measurement would refuse.
  static std::size_t estimate_bytes(std::size_t row_count, std::size_t action_count,
                                    std::size_t total_key_words, const solver::UnifiedGame& game);

  // Test support: iterate stored keys in construction order.
  std::span<const std::uint64_t> key_at(std::size_t row) const;

 private:
  struct RowRecord {
    std::uint32_t key_offset = 0;
    std::uint32_t actions_offset = 0;
    std::uint32_t probabilities_offset = 0;
    std::uint16_t key_length = 0;
    std::uint16_t action_count = 0;
  };

  struct Slot {
    std::uint64_t hash = 0;
    std::uint32_t row_plus_one = 0;  // zero means empty
    std::uint32_t reserved = 0;
  };

  void resolve(const RowRecord& record, CompactRowView& out) const;

  // Shared flattening for the two row types. Only `.actions` and
  // `.probabilities` are read; the v2 `visits` field is not resident.
  template <class Row>
  void build_rows(const std::map<solver::InformationKey, Row>& rows);

  std::vector<std::uint8_t> key_blob_;
  std::vector<poker::Action> action_blob_;
  std::vector<double> probability_blob_;
  std::vector<RowRecord> rows_;
  std::vector<Slot> slots_;
  std::size_t row_count_ = 0;
  std::size_t probability_count_ = 0;
  std::size_t slot_mask_ = 0;
};

}  // namespace bs::resident
