#include "resident_index.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <map>
#include <span>
#include <stdexcept>
#include <vector>

namespace bs::resident {
namespace {

// FNV-1a-style 32-bit mix over the compact key bytes. Correctness never
// depends on this hash: every probe hit is verified with a full length and
// byte comparison, so collisions only cost an extra probe.
std::uint32_t hash_key(std::span<const std::uint8_t> key) {
  std::uint64_t h = 0xcbf29ce484222325ULL;
  for (std::uint8_t byte : key) {
    h ^= byte;
    h *= 0x100000001b3ULL;
  }
  h ^= h >> 33;
  h *= 0xff51afd7ed558ccdULL;
  h ^= h >> 33;
  const auto result = static_cast<std::uint32_t>(h);
  return result ? result : 1u;
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

std::size_t ResidentIndex::compact_information_key(std::span<const std::uint64_t> words,
                                                   std::span<std::uint8_t> output) {
  if (words.size() < 4)
    return 0;
  const auto board_size = words[3];
  // Board size is 0 (preflop), 3 (flop), 4 (turn), or 5 (river); 1 and 2 are
  // never produced by make_information_key.
  if (board_size != 0 && board_size != 3 && board_size != 4 && board_size != 5)
    return 0;
  if (words.size() < 4 + board_size)
    return 0;
  const std::size_t action_words = words.size() - 4 - board_size;
  if (action_words % 4 != 0)
    return 0;
  const std::size_t num_actions = action_words / 4;
  const std::size_t needed = 4 + static_cast<std::size_t>(board_size) + num_actions * 5;
  if (output.size() < needed)
    return 0;

  // Range-check every field that is truncated to uint8 so two distinct word
  // keys can never compact to the same byte string (which would let find()
  // return the wrong row). All bounds match make_information_key's validation.
  if (words[0] > 9 || words[1] > 51 || words[2] > 51)
    return 0;
  for (std::size_t i = 0; i < board_size; ++i)
    if (words[4 + i] > 51)
      return 0;
  for (std::size_t a = 0; a < num_actions; ++a) {
    const std::size_t base = 4 + board_size + a * 4;
    if (words[base] > 3 || words[base + 1] > 9 || words[base + 2] > 4)
      return 0;
  }

  output[0] = static_cast<std::uint8_t>(words[0]);  // actor
  output[1] = static_cast<std::uint8_t>(words[1]);  // own0
  output[2] = static_cast<std::uint8_t>(words[2]);  // own1
  output[3] = static_cast<std::uint8_t>(board_size);
  std::size_t off = 4;
  for (std::size_t i = 0; i < board_size; ++i)
    output[off++] = static_cast<std::uint8_t>(words[4 + i]);
  for (std::size_t a = 0; a < num_actions; ++a) {
    const std::size_t base = 4 + board_size + a * 4;
    output[off++] = static_cast<std::uint8_t>(words[base]);      // street
    output[off++] = static_cast<std::uint8_t>(words[base + 1]);  // seat
    output[off++] = static_cast<std::uint8_t>(words[base + 2]);  // type
    const auto target = words[base + 3];
    if (target > 0xffff)
      return 0;  // target exceeds uint16 range
    output[off++] = static_cast<std::uint8_t>(target & 0xffu);
    output[off++] = static_cast<std::uint8_t>((target >> 8) & 0xffu);
  }
  return needed;
}

template <class Row>
void ResidentIndex::build_rows(const std::map<solver::InformationKey, Row>& rows) {
  if (row_count_ != 0)
    throw std::logic_error("resident index is built once");
  row_count_ = rows.size();
  if (row_count_ == 0)
    throw std::invalid_argument("resident index requires at least one policy row");

  std::size_t total_key_bytes = 0;
  std::size_t total_actions = 0;
  // Collect distinct probability bit patterns to decide on the codebook. A
  // bitwise key keeps the codebook lossless: two doubles that compare equal
  // but differ in their bits (for example +0.0 and -0.0) stay distinct, so a
  // deferred value is always bitwise identical to the stored one.
  std::map<std::uint64_t, std::uint8_t> codebook_index;
  for (const auto& [key, row] : rows) {
    if (row.actions.size() != row.probabilities.size() || row.actions.size() > kMaxResidentActions)
      throw std::invalid_argument("malformed resident policy row");
    // Validate that the key can be compacted and measure its compact size.
    std::array<std::uint8_t, 512> compact_buf{};
    const std::size_t compact_size = compact_information_key(key, compact_buf);
    if (compact_size == 0)
      throw std::invalid_argument("resident information key cannot be compacted");
    if (compact_size > 0xffff)
      throw std::length_error("resident compact key too large");
    total_key_bytes += 2 + compact_size;  // 2-byte length prefix
    total_actions += row.actions.size();
    for (double probability : row.probabilities) {
      std::uint64_t bits;
      std::memcpy(&bits, &probability, sizeof(bits));
      codebook_index.emplace(bits, 0);  // index assigned below
    }
  }
  if (total_key_bytes > 0xffffffffULL || total_actions > 0xffffffffULL)
    throw std::length_error("resident index exceeds 32-bit offsets");

  // Use the per-root codebook when at most 256 distinct values exist (the
  // common case for a trained artifact, which has a few hundred distinct
  // probabilities at most). Otherwise fall back to one exact double per
  // action. The codebook is built in sorted bit-pattern order so its layout
  // is deterministic across builds.
  const bool use_codebook = codebook_index.size() <= 256;
  use_codebook_ = use_codebook;
  if (use_codebook) {
    codebook_.reserve(codebook_index.size());
    std::uint8_t index = 0;
    for (auto& [bits, slot] : codebook_index) {
      slot = index++;
      double probability;
      std::memcpy(&probability, &bits, sizeof(probability));
      codebook_.push_back(probability);
    }
  }

  key_blob_.reserve(total_key_bytes);
  action_blob_.reserve(total_actions);
  if (use_codebook)
    probability_index_.reserve(total_actions);
  else
    probability_blob_.reserve(total_actions);
  rows_.reserve(row_count_);

  for (const auto& [key, row] : rows) {
    // Compact the key (deterministic; same result as the validation pass).
    std::array<std::uint8_t, 512> compact_buf{};
    const std::size_t compact_size = compact_information_key(key, compact_buf);
    if (compact_size == 0)
      throw std::logic_error("resident key compaction failed on second pass");

    RowRecord record;
    record.key_offset = static_cast<std::uint32_t>(key_blob_.size());
    record.key_length = static_cast<std::uint16_t>(compact_size);
    append_u16(key_blob_, static_cast<std::uint16_t>(compact_size));
    key_blob_.insert(key_blob_.end(), compact_buf.begin(), compact_buf.begin() + compact_size);

    record.actions_offset = static_cast<std::uint32_t>(action_blob_.size());
    for (const poker::Action& action : row.actions) {
      if (action.target_total > 0xffff)
        throw std::invalid_argument("resident action target exceeds uint16 range");
      CompactAction compact;
      compact.type = static_cast<std::uint8_t>(action.type);
      compact.target = static_cast<std::uint16_t>(action.target_total);
      action_blob_.push_back(compact);
    }

    if (use_codebook) {
      record.probabilities_offset = static_cast<std::uint32_t>(probability_index_.size());
      for (double probability : row.probabilities) {
        std::uint64_t bits;
        std::memcpy(&bits, &probability, sizeof(bits));
        probability_index_.push_back(codebook_index.at(bits));
      }
    } else {
      record.probabilities_offset = static_cast<std::uint32_t>(probability_blob_.size());
      for (double probability : row.probabilities)
        probability_blob_.push_back(probability);
    }
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
    const RowRecord& record = rows_[row_id];
    const auto* key_data = key_blob_.data() + record.key_offset + 2;
    const std::uint32_t hash = hash_key({key_data, record.key_length});
    std::size_t slot = static_cast<std::size_t>(hash & slot_mask_);
    while (slots_[slot].row_plus_one != 0)
      slot = (slot + 1) & slot_mask_;
    slots_[slot] = Slot{hash, row_id + 1};
  }
}

void ResidentIndex::build(const std::map<solver::InformationKey, solver::PolicyRow>& rows) {
  build_rows(rows);
}

void ResidentIndex::build(const std::map<solver::InformationKey, solver::SeatPolicyRow>& rows) {
  build_rows(rows);
}

std::vector<std::uint64_t> ResidentIndex::key_at(std::size_t row) const {
  const RowRecord& record = rows_.at(row);
  const auto* data = key_blob_.data() + record.key_offset + 2;
  const std::size_t board_size = data[3];
  const std::size_t actions = (record.key_length - 4 - board_size) / 5;
  std::vector<std::uint64_t> words(4 + board_size + actions * 4);
  words[0] = data[0];  // actor
  words[1] = data[1];  // own0
  words[2] = data[2];  // own1
  words[3] = data[3];  // board_size
  std::size_t off = 4;
  for (std::size_t i = 0; i < board_size; ++i)
    words[off++] = data[4 + i];
  for (std::size_t a = 0; a < actions; ++a) {
    const std::size_t base = 4 + board_size + a * 5;
    words[off++] = data[base];      // street
    words[off++] = data[base + 1];  // seat
    words[off++] = data[base + 2];  // type
    words[off++] = static_cast<std::uint16_t>(data[base + 3]) |
                   (static_cast<std::uint16_t>(data[base + 4]) << 8);  // target
  }
  return words;
}

void ResidentIndex::resolve(const RowRecord& record, CompactRowView& out) const {
  out.count = record.action_count;
  out.actions = action_blob_.data() + record.actions_offset;
  if (use_codebook_) {
    out.codebook = codebook_.data();
    out.probability_indices = probability_index_.data() + record.probabilities_offset;
    out.probabilities = nullptr;
  } else {
    out.codebook = nullptr;
    out.probability_indices = nullptr;
    out.probabilities = probability_blob_.data() + record.probabilities_offset;
  }
}

bool ResidentIndex::find(std::span<const std::uint8_t> compact_key, CompactRowView& out) const {
  if (slots_.empty())
    return false;
  const std::uint32_t hash = hash_key(compact_key);
  std::size_t slot = static_cast<std::size_t>(hash & slot_mask_);
  for (;;) {
    const Slot& candidate = slots_[slot];
    if (candidate.row_plus_one == 0)
      return false;
    if (candidate.hash == hash) {
      const RowRecord& record = rows_[candidate.row_plus_one - 1];
      if (record.key_length == compact_key.size()) {
        const auto* stored = key_blob_.data() + record.key_offset + 2;
        if (std::memcmp(stored, compact_key.data(), compact_key.size()) == 0) {
          resolve(record, out);
          return true;
        }
      }
    }
    slot = (slot + 1) & slot_mask_;
  }
}

std::size_t ResidentIndex::resident_bytes() const noexcept {
  return key_blob_.capacity() + action_blob_.capacity() * sizeof(CompactAction) +
         probability_blob_.capacity() * sizeof(double) + codebook_.capacity() * sizeof(double) +
         probability_index_.capacity() + rows_.capacity() * sizeof(RowRecord) +
         slots_.capacity() * sizeof(Slot);
}

std::size_t ResidentIndex::estimate_bytes(std::size_t row_count, std::size_t action_count,
                                          std::size_t total_key_words,
                                          const solver::UnifiedGame& game) {
  // Compact key blob: 2-byte length prefix per row + compact key data.
  // Compact key: 4 header + board_size + 5*actions bytes.
  // total_key_words = 4 + board_size + 4*actions, so compact <= 5*words/4.
  std::size_t bytes = row_count * 2 + (total_key_words * 5 + 3) / 4;
  // Compact actions: 3 bytes each.
  bytes += action_count * sizeof(CompactAction);
  // Probabilities: charge the worst case across both storage modes. The
  // fallback stores one exact double per action (8*A bytes). The codebook
  // stores one uint8 index per action (A bytes) plus a per-root dictionary of
  // at most 256 distinct doubles (256*8 bytes, reserved exactly at build time
  // so capacity equals the distinct count). The codebook is only active when
  // the row set has at most 256 distinct values, so its dictionary cost is
  // bounded; for small action counts the fixed dictionary can exceed the
  // doubles layout, hence the max. This keeps the pre-gate a true upper bound:
  // it never accepts a root the exact post-build measurement would refuse.
  bytes += std::max(action_count * sizeof(double), action_count + 256 * sizeof(double));
  // Row records.
  bytes += row_count * sizeof(RowRecord);
  // Slot table: power-of-two at 50% load, 8 bytes per slot.
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
  bytes += solver::kUnifiedGameCopyAccountingBytes + 8 * 64;
  for (const auto& range : game.ranges)
    bytes += 4 * range.size() * sizeof(solver::WeightedHand);
  for (const auto& street : game.sizes) {
    bytes += 4 * street.bets.size() * sizeof(solver::Fraction);
    bytes += 4 * street.raises.size() * sizeof(solver::Fraction);
  }
  return bytes;
}

}  // namespace bs::resident
