#include "resident_index.hpp"

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace bs::resident {
namespace {

// FNV-1a-style 64-bit mix over the canonical key words. Correctness never
// depends on this hash: every probe hit is verified with a full length and
// word comparison, so collisions only cost an extra probe.
std::uint64_t hash_key(std::span<const std::uint64_t> key) {
  std::uint64_t h = 0xcbf29ce484222325ULL;
  for (std::uint64_t word : key) {
    h ^= word;
    h *= 0x100000001b3ULL;
    h ^= word >> 32;
    h *= 0x9e3779b97f4a7c15ULL;
  }
  h ^= h >> 33;
  h *= 0xff51afd7ed558ccdULL;
  h ^= h >> 33;
  return h ? h : 1ULL;
}

void append_u16(std::vector<std::uint8_t>& blob, std::uint16_t value) {
  blob.push_back(static_cast<std::uint8_t>(value & 0xffu));
  blob.push_back(static_cast<std::uint8_t>((value >> 8) & 0xffu));
}

}  // namespace

ResidentIndex::ResidentIndex() = default;
ResidentIndex::~ResidentIndex() = default;
ResidentIndex::ResidentIndex(ResidentIndex&&) noexcept = default;
ResidentIndex& ResidentIndex::operator=(ResidentIndex&&) noexcept = default;

void ResidentIndex::build(const std::map<solver::InformationKey, solver::PolicyRow>& rows) {
  if (row_count_ != 0)
    throw std::logic_error("resident index is built once");
  row_count_ = rows.size();
  if (row_count_ == 0)
    throw std::invalid_argument("resident index requires at least one policy row");

  // Worst-case reservations sized from the inputs avoid geometric reallocation
  // while the two stores briefly coexist during construction.
  std::size_t total_key_bytes = 0;
  std::size_t total_actions = 0;
  for (const auto& [key, row] : rows) {
    if (key.size() > 0xffff)
      throw std::length_error("resident information key too large");
    // 8-byte header (length plus alignment padding) plus the words.
    total_key_bytes += 8 + key.size() * sizeof(std::uint64_t);
    total_actions += row.actions.size();
  }
  if (total_key_bytes > 0xffffffffULL || total_actions > 0xffffffffULL)
    throw std::length_error("resident index exceeds 32-bit offsets");

  key_blob_.reserve(total_key_bytes);
  action_blob_.reserve(total_actions);
  probability_blob_.reserve(total_actions);
  rows_.reserve(row_count_);

  for (const auto& [key, row] : rows) {
    if (row.actions.size() != row.probabilities.size() || row.actions.size() > 0xffff)
      throw std::invalid_argument("malformed resident policy row");
    RowRecord record;
    record.key_offset = static_cast<std::uint32_t>(key_blob_.size());
    record.key_length = static_cast<std::uint16_t>(key.size());
    append_u16(key_blob_, record.key_length);
    for (int pad = 0; pad < 6; ++pad)
      key_blob_.push_back(0);
    const auto first = key_blob_.size();
    key_blob_.resize(first + key.size() * sizeof(std::uint64_t));
    std::memcpy(key_blob_.data() + first, key.data(), key.size() * sizeof(std::uint64_t));

    record.actions_offset = static_cast<std::uint32_t>(action_blob_.size());
    for (const poker::Action& action : row.actions)
      action_blob_.push_back(action);

    record.probabilities_offset = static_cast<std::uint32_t>(probability_blob_.size());
    for (double probability : row.probabilities)
      probability_blob_.push_back(probability);
    record.action_count = static_cast<std::uint16_t>(row.actions.size());
    rows_.push_back(record);
  }
  probability_count_ = total_actions;

  // Open addressing at a 50 percent load factor with power-of-two capacity.
  std::size_t capacity = 4;
  while (capacity < row_count_ * 2) {
    if (capacity == 0)
      throw std::length_error("resident slot table overflow");
    capacity <<= 1;
  }
  slot_mask_ = capacity - 1;
  slots_.assign(capacity, Slot{});
  for (std::uint32_t row_id = 0; row_id < rows_.size(); ++row_id) {
    const auto key = key_at(row_id);
    const std::uint64_t hash = hash_key(key);
    std::size_t slot = static_cast<std::size_t>(hash & slot_mask_);
    while (slots_[slot].row_plus_one != 0)
      slot = (slot + 1) & slot_mask_;
    slots_[slot] = Slot{hash, row_id + 1, 0};
  }
}

std::span<const std::uint64_t> ResidentIndex::key_at(std::size_t row) const {
  const RowRecord& record = rows_.at(row);
  const auto* data = key_blob_.data() + record.key_offset;
  return {reinterpret_cast<const std::uint64_t*>(data + 8), record.key_length};
}

void ResidentIndex::resolve(const RowRecord& record, CompactRowView& out) const {
  out.count = record.action_count;
  out.actions = action_blob_.data() + record.actions_offset;
  out.probabilities = probability_blob_.data() + record.probabilities_offset;
}

bool ResidentIndex::find(std::span<const std::uint64_t> key, CompactRowView& out) const {
  if (slots_.empty())
    return false;
  const std::uint64_t hash = hash_key(key);
  std::size_t slot = static_cast<std::size_t>(hash & slot_mask_);
  for (;;) {
    const Slot& candidate = slots_[slot];
    if (candidate.row_plus_one == 0)
      return false;
    if (candidate.hash == hash) {
      const RowRecord& record = rows_[candidate.row_plus_one - 1];
      if (record.key_length == key.size()) {
        const auto stored = key_at(candidate.row_plus_one - 1);
        if (std::memcmp(stored.data(), key.data(), key.size() * sizeof(std::uint64_t)) == 0) {
          resolve(record, out);
          return true;
        }
      }
    }
    slot = (slot + 1) & slot_mask_;
  }
}

std::size_t ResidentIndex::resident_bytes() const noexcept {
  return key_blob_.capacity() + action_blob_.capacity() * sizeof(poker::Action) +
         probability_blob_.capacity() * sizeof(double) + rows_.capacity() * sizeof(RowRecord) +
         slots_.capacity() * sizeof(Slot);
}

std::size_t ResidentIndex::estimate_bytes(std::size_t row_count, std::size_t action_count,
                                          std::size_t total_key_words,
                                          const solver::HeadsUpGame& game) {
  // build() reserves exactly these amounts, so the compact portion is an
  // equality with the eventual capacity for any root that is fully loaded.
  std::size_t bytes = row_count * 8 + total_key_words * sizeof(std::uint64_t);
  bytes += action_count * sizeof(poker::Action);
  bytes += action_count * sizeof(double);
  bytes += row_count * sizeof(RowRecord);
  std::size_t capacity = 4;
  while (capacity < row_count * 2)
    capacity <<= 1;
  bytes += capacity * sizeof(Slot);

  // Immutable game copy. Element storage is copied from the reader's vectors
  // and keeps an unknown allocator capacity; charge the declared game-copy
  // accounting constant (RFC 0007 decouples this from struct layout so a field
  // addition cannot move the estimate), a fixed per-vector slack term, and four
  // times the exact element bytes so ordinary geometric growth capacity is
  // always covered. The game copies are tiny relative to the row blobs.
  bytes += solver::kGameCopyAccountingBytes + 8 * 64;
  for (const auto& range : game.ranges)
    bytes += 4 * range.size() * sizeof(solver::WeightedHand);
  for (const auto& street : game.sizes) {
    bytes += 4 * street.bets.size() * sizeof(solver::Fraction);
    bytes += 4 * street.raises.size() * sizeof(solver::Fraction);
  }
  return bytes;
}

}  // namespace bs::resident
