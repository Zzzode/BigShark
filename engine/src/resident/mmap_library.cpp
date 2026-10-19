// RFC 0026: bucket-deduplicated consolidated mmap-friendly flop class
// library format ("BSFLIB02").
//
// This file implements the pack function (offline tool path) and the
// MmapLibrary class (read-only mmap view). N-seat training seals rows under
// (card_bucket(kNSeatCardKind, holding, board), tree node); the SQLite
// publish path copies one sealed row onto every concrete holding of the
// bucket. The packer reverses that copy, storing one row per (public node,
// trained bucket); the resident projects a concrete holding to its bucket at
// lookup time. Strategy output is bitwise identical and size is independent
// of the training range width.
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <bs/abstraction.hpp>
#include <bs/mmap_library.hpp>
#include <bs/nseat_trainer.hpp>  // kNSeatCardKind
#include <bs/range.hpp>          // comboIndex
#include <bs/seat_policy.hpp>    // SeatPolicyRow (full definition for packing)
#include <bs/strategy_artifact.hpp>
#include <bs/unified_game.hpp>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <vector>

#include "resident_index.hpp"  // CompactRowView, CompactAction, ProbabilityLayout, ResidentIndex

namespace bs::resident {
namespace {

constexpr char kMagic[8] = {'B', 'S', 'F', 'L', 'I', 'B', '0', '2'};
constexpr std::uint32_t kVersion = 2;
constexpr std::size_t kPageSize = 4096;
constexpr std::size_t kMaxSeats = 10;  // poker::kMaxUnifiedSeats

// 64-bit FNV-1a-style mix over the compact key bytes. This is the same mix
// as resident_index.cpp's hash_key but kept at full 64-bit width so the
// sorted-hash binary search has no truncation collisions to resolve.
std::uint64_t hash_key_64(std::span<const std::uint8_t> key) {
  std::uint64_t h = 0xcbf29ce484222325ULL;
  for (std::uint8_t byte : key) {
    h ^= byte;
    h *= 0x100000001b3ULL;
  }
  h ^= h >> 33;
  h *= 0xff51afd7ed558ccdULL;
  h ^= h >> 33;
  return h;
}

std::size_t align_up(std::size_t value, std::size_t alignment) {
  return (value + alignment - 1) & ~(alignment - 1);
}

// Little-endian writers (the format is defined as little-endian; the byte
// loop is correct on any host endianness).
void write_u8(std::vector<std::uint8_t>& buf, std::uint8_t value) {
  buf.push_back(value);
}

void write_u16(std::vector<std::uint8_t>& buf, std::uint16_t value) {
  buf.push_back(static_cast<std::uint8_t>(value & 0xffu));
  buf.push_back(static_cast<std::uint8_t>((value >> 8) & 0xffu));
}

void write_u32(std::vector<std::uint8_t>& buf, std::uint32_t value) {
  buf.push_back(static_cast<std::uint8_t>(value & 0xffu));
  buf.push_back(static_cast<std::uint8_t>((value >> 8) & 0xffu));
  buf.push_back(static_cast<std::uint8_t>((value >> 16) & 0xffu));
  buf.push_back(static_cast<std::uint8_t>((value >> 24) & 0xffu));
}

void write_u64(std::vector<std::uint8_t>& buf, std::uint64_t value) {
  for (int i = 0; i < 8; ++i)
    buf.push_back(static_cast<std::uint8_t>((value >> (i * 8)) & 0xffu));
}

void write_f64(std::vector<std::uint8_t>& buf, double value) {
  std::uint64_t bits;
  std::memcpy(&bits, &value, sizeof(bits));
  write_u64(buf, bits);
}

// Stream a little-endian u64 into an output file (used to patch class_offsets).
void write_u64_to(std::ostream& out, std::uint64_t value) {
  std::uint8_t bytes[8];
  for (int i = 0; i < 8; ++i)
    bytes[i] = static_cast<std::uint8_t>((value >> (i * 8)) & 0xffu);
  out.write(reinterpret_cast<const char*>(bytes), 8);
}

// Little-endian readers from the mmap region. memcpy handles unaligned
// access safely on every architecture.
std::uint8_t read_u8(const std::uint8_t* p) {
  return *p;
}

std::uint16_t read_u16(const std::uint8_t* p) {
  std::uint16_t value;
  std::memcpy(&value, p, sizeof(value));
  return value;
}

std::uint32_t read_u32(const std::uint8_t* p) {
  std::uint32_t value;
  std::memcpy(&value, p, sizeof(value));
  return value;
}

std::uint64_t read_u64(const std::uint8_t* p) {
  std::uint64_t value;
  std::memcpy(&value, p, sizeof(value));
  return value;
}

double read_f64(const std::uint8_t* p) {
  const std::uint64_t bits = read_u64(p);
  double value;
  std::memcpy(&value, &bits, sizeof(value));
  return value;
}

void write_fraction(std::vector<std::uint8_t>& buf, const abstraction::Fraction& frac) {
  write_u64(buf, frac.numerator);
  write_u64(buf, frac.denominator);
}

abstraction::Fraction read_fraction(const std::uint8_t* p) {
  abstraction::Fraction frac;
  frac.numerator = read_u64(p);
  frac.denominator = read_u64(p + 8);
  return frac;
}

ProbabilityLayout codebook_type_from_u8(std::uint8_t value) {
  switch (value) {
    case 0:
      return ProbabilityLayout::Doubles;
    case 8:
      return ProbabilityLayout::Codebook8;
    case 16:
      return ProbabilityLayout::Codebook16;
    default:
      throw std::runtime_error("mmap library: invalid codebook_type " + std::to_string(value));
  }
}

std::uint8_t codebook_type_to_u8(ProbabilityLayout layout) {
  switch (layout) {
    case ProbabilityLayout::Doubles:
      return 0;
    case ProbabilityLayout::Codebook8:
      return 8;
    case ProbabilityLayout::Codebook16:
      return 16;
  }
  return 0;  // unreachable
}

// Per-class grouped data built during the pack pass. Concrete rows are
// collapsed to (public node, trained bucket): all concrete holdings the
// trainer sealed under one card bucket at one public node share one run.
struct ClassBuildData {
  std::uint32_t class_id = 0;
  std::array<int, 3> canonical_board{};

  struct BucketRun {
    std::uint16_t bucket = 0;
    std::vector<std::uint64_t> prob_bits;  // bit pattern per menu action
  };
  struct Node {
    std::uint64_t hash = 0;
    std::vector<poker::Action> menu;
    // Trained bucket runs at this node, keyed by bucket id (sorted on write).
    std::map<std::uint16_t, BucketRun> runs;
  };

  std::map<std::uint64_t, std::size_t> node_by_hash;
  std::vector<Node> nodes;  // sorted by hash before write
  // Distinct probability bit patterns → codebook index (sorted by bits).
  std::map<std::uint64_t, std::uint16_t> codebook_index;
  ProbabilityLayout layout = ProbabilityLayout::Doubles;
  const std::vector<solver::WeightedHand>* range = nullptr;
  std::optional<std::uint64_t> card_digest;
  std::optional<std::uint64_t> action_digest;
};

// Split a concrete compact key (ResidentIndex::compact_information_key output,
// layout [seat][own0][own1][board_size][board...][5 bytes per action]) into the
// public compact key ([seat][board_size][board...][actions], no own cards),
// the holding, and the board span. Returns false on a malformed key.
struct ParsedConcreteKey {
  std::array<int, 2> own{};
  std::array<int, 5> board{};
  std::size_t board_size = 0;
  std::vector<std::uint8_t> public_key;
};

bool parse_concrete_key(std::span<const std::uint8_t> key, ParsedConcreteKey& out) {
  if (key.size() < 4)
    return false;
  const std::size_t board_size = key[3];
  if (board_size != 0 && board_size != 3 && board_size != 4 && board_size != 5)
    return false;
  if (key.size() < 4 + board_size)
    return false;
  const std::size_t action_bytes = key.size() - 4 - board_size;
  if (action_bytes % 5 != 0)
    return false;
  out.own = {static_cast<int>(key[1]), static_cast<int>(key[2])};
  out.board_size = board_size;
  for (std::size_t i = 0; i < board_size; ++i)
    out.board[i] = key[4 + i];
  out.public_key.clear();
  out.public_key.reserve(2 + board_size + action_bytes);
  out.public_key.push_back(key[0]);  // seat
  out.public_key.push_back(static_cast<std::uint8_t>(board_size));
  out.public_key.insert(out.public_key.end(), key.begin() + 4, key.begin() + 4 + board_size);
  out.public_key.insert(out.public_key.end(), key.begin() + 4 + board_size, key.end());
  return true;
}

// Legal bucket range for the compiled-in n-seat card kind: Preflop169 values
// 0..168 at an empty board; postflop it falls back to CategoryTiersV1 1..9.
bool bucket_in_range(std::size_t board_size, std::uint32_t bucket) {
  if (board_size == 0)
    return bucket <= 168;
  return bucket >= 1 && bucket <= 9;
}

// Encode a canonical board as a u64 key for the board→class_id map.
std::uint64_t board_key(const std::array<int, 3>& board) {
  return static_cast<std::uint64_t>(static_cast<std::uint8_t>(board[0])) |
         (static_cast<std::uint64_t>(static_cast<std::uint8_t>(board[1])) << 8) |
         (static_cast<std::uint64_t>(static_cast<std::uint8_t>(board[2])) << 16);
}

}  // namespace

void pack_binary_library(const std::filesystem::path& path, const solver::UnifiedGame& game,
                         std::span<const PackClassInput> classes) {
  if (classes.empty())
    throw std::invalid_argument("pack_binary_library: no classes to pack");
  if (game.def.player_count < 2 || game.def.player_count > kMaxSeats)
    throw std::invalid_argument("pack_binary_library: player_count out of range");
  // One implementation: the streaming writer holds one class resident at a
  // time. The borrowed PackClassInput pointers stay valid for the call.
  PackClassSupplier supplier = [&](std::size_t index) { return classes[index]; };
  pack_binary_library_streaming(path, game, classes.size(), supplier);
}

// ---------------------------------------------------------------------------
// Streaming bucket-deduplicated pack (RFC 0026)
// ---------------------------------------------------------------------------

namespace {

// Validate the shared abstraction digests against the running cross-class
// state. Digests are optional in synthetic test inputs; once any class
// supplies them every class must supply identical values.
void reconcile_digests(const PackClassInput& input, bool& have, std::uint64_t& card_digest,
                       std::uint64_t& action_digest) {
  if (!input.card_abstraction_digest.has_value() && !input.action_abstraction_digest.has_value()) {
    if (have)
      throw std::runtime_error("pack: class " + std::to_string(input.class_id) +
                               " omits abstraction digests that earlier classes supplied");
    return;
  }
  if (!input.card_abstraction_digest.has_value() || !input.action_abstraction_digest.has_value())
    throw std::runtime_error("pack: class " + std::to_string(input.class_id) +
                             " supplies only one of the two abstraction digests");
  if (!have) {
    have = true;
    card_digest = *input.card_abstraction_digest;
    action_digest = *input.action_abstraction_digest;
    return;
  }
  if (*input.card_abstraction_digest != card_digest ||
      *input.action_abstraction_digest != action_digest)
    throw std::runtime_error("pack: class " + std::to_string(input.class_id) +
                             " abstraction digest differs from earlier classes");
}

std::uint8_t prob_entry_width(ProbabilityLayout layout) {
  switch (layout) {
    case ProbabilityLayout::Codebook8:
      return 1;
    case ProbabilityLayout::Codebook16:
      return 2;
    case ProbabilityLayout::Doubles:
      return 8;
  }
  return 8;  // unreachable
}

// RFC 0027: canonicalize the declared range to ascending comboIndex order.
// The SQLite reader has always returned ranges in this order (`ORDER BY
// player, combo`), and the packer therefore wrote it that way. The trainer
// builds `declared_library_range()` in pair/rank order instead, so a direct
// serializer that wrote the caller's vector verbatim would emit different
// bytes for every non-full range. Sorting here makes the section a function of
// the set of weighted combos, not of the caller's order.
const std::vector<solver::WeightedHand>& canonical_range(
    const std::vector<solver::WeightedHand>& range, std::vector<solver::WeightedHand>& scratch) {
  scratch = range;
  std::sort(scratch.begin(), scratch.end(),
            [](const solver::WeightedHand& a, const solver::WeightedHand& b) {
              return bs::comboIndex(a.cards[0], a.cards[1]) <
                     bs::comboIndex(b.cards[0], b.cards[1]);
            });
  for (std::size_t i = 1; i < scratch.size(); ++i)
    if (bs::comboIndex(scratch[i - 1].cards[0], scratch[i - 1].cards[1]) ==
        bs::comboIndex(scratch[i].cards[0], scratch[i].cards[1]))
      throw std::invalid_argument("pack: duplicate combo in the declared range");
  return scratch;
}

// Collapse one class's concrete rows into (public node, trained bucket) runs,
// asserting the losslessness invariants (RFC 0026 G2/G4):
//   * every concrete row at a node shares that node's single action menu;
//   * every concrete row sealing the same (node, bucket) is bitwise identical.
// `range` is the canonicalized range (see canonical_range); nullptr means the
// caller has no range.
ClassBuildData build_class_data(const PackClassInput& input,
                                const std::vector<solver::WeightedHand>* range) {
  ClassBuildData data;
  data.class_id = input.class_id;
  data.canonical_board = input.canonical_board;
  data.range = range;
  data.card_digest = input.card_abstraction_digest;
  data.action_digest = input.action_abstraction_digest;

  for (const auto& [key, row] : *input.rows) {
    if (row.actions.size() != row.probabilities.size() || row.actions.empty() ||
        row.actions.size() > kMaxResidentActions)
      throw std::invalid_argument("pack: malformed row in class " + std::to_string(input.class_id));

    std::array<std::uint8_t, 512> compact{};
    const std::size_t compact_size = ResidentIndex::compact_information_key(
        std::span<const std::uint64_t>(key.data(), key.size()),
        std::span<std::uint8_t>(compact.data(), compact.size()));
    if (compact_size == 0)
      throw std::invalid_argument("pack: key cannot be compacted in class " +
                                  std::to_string(input.class_id));
    ParsedConcreteKey parsed;
    if (!parse_concrete_key(std::span<const std::uint8_t>(compact.data(), compact_size), parsed))
      throw std::invalid_argument("pack: unparseable compact key in class " +
                                  std::to_string(input.class_id));
    const std::uint64_t node_hash = hash_key_64(
        std::span<const std::uint8_t>(parsed.public_key.data(), parsed.public_key.size()));
    const std::array<int, 2> hole{parsed.own[0], parsed.own[1]};
    const std::span<const int> board(parsed.board.data(), parsed.board_size);
    const std::uint32_t bucket32 = abstraction::card_bucket(solver::kNSeatCardKind, hole, board);
    if (!bucket_in_range(parsed.board_size, bucket32))
      throw std::runtime_error("pack: bucket " + std::to_string(bucket32) + " outside the " +
                               (parsed.board_size == 0 ? "Preflop169 0..168" : "category 1..9") +
                               " range in class " + std::to_string(input.class_id));
    const std::uint16_t bucket = static_cast<std::uint16_t>(bucket32);

    ClassBuildData::Node* node = nullptr;
    if (auto it = data.node_by_hash.find(node_hash); it != data.node_by_hash.end()) {
      node = &data.nodes[it->second];
      if (node->menu.size() != row.actions.size())
        throw std::runtime_error("pack: cross-bucket menu length mismatch in class " +
                                 std::to_string(input.class_id));
      for (std::size_t i = 0; i < row.actions.size(); ++i)
        if (node->menu[i].type != row.actions[i].type ||
            node->menu[i].target_total != row.actions[i].target_total)
          throw std::runtime_error("pack: cross-bucket action menu mismatch (class " +
                                   std::to_string(input.class_id) + ", bucket " +
                                   std::to_string(bucket) + ")");
    } else {
      ClassBuildData::Node fresh;
      fresh.hash = node_hash;
      fresh.menu = row.actions;
      data.node_by_hash.emplace(node_hash, data.nodes.size());
      data.nodes.push_back(std::move(fresh));
      node = &data.nodes.back();
    }

    std::vector<std::uint64_t> bits;
    bits.reserve(row.probabilities.size());
    for (double probability : row.probabilities) {
      std::uint64_t pattern;
      std::memcpy(&pattern, &probability, sizeof(pattern));
      bits.push_back(pattern);
      data.codebook_index.emplace(pattern, 0);
    }

    if (auto existing = node->runs.find(bucket); existing != node->runs.end()) {
      if (existing->second.prob_bits != bits)
        throw std::runtime_error(
            "pack: non-identical strategy sealed under the same bucket (class " +
            std::to_string(input.class_id) + ", node hash " + std::to_string(node_hash) +
            ", bucket " + std::to_string(bucket) + ")");
    } else {
      node->runs.emplace(bucket, ClassBuildData::BucketRun{bucket, std::move(bits)});
    }
  }

  std::sort(
      data.nodes.begin(), data.nodes.end(),
      [](const ClassBuildData::Node& a, const ClassBuildData::Node& b) { return a.hash < b.hash; });
  // Detect 64-bit node-hash collisions within the class (distinct public keys
  // hashing equal) — the writer cannot represent two nodes by one hash.
  for (std::size_t i = 1; i < data.nodes.size(); ++i)
    if (data.nodes[i - 1].hash == data.nodes[i].hash)
      throw std::runtime_error("pack: node-hash collision in class " +
                               std::to_string(input.class_id));

  // Indices into the codebook are assigned by write_class_section under the
  // file-level layout, where the value capacity is known.
  return data;
}

// Thrown by a codebook-indexed pass when a class has more distinct
// probability values than the layout can index; the caller reruns the full
// pass with Doubles. RFC 0027: the public exception type lets the builder
// fail closed without a private dependency.
using CodebookTooLarge = CodebookCapacityError;

// Write one class section. Offsets in bucket rows are section-relative byte
// offsets into the probs region (caller patches the probs base).
void write_class_section(std::vector<std::uint8_t>& buf, const ClassBuildData& data,
                         ProbabilityLayout file_layout, std::uint64_t card_digest,
                         std::uint64_t action_digest) {
  (void)card_digest;
  (void)action_digest;  // digests live in the file header, not per class
  const std::uint32_t node_count = static_cast<std::uint32_t>(data.nodes.size());
  std::uint32_t total_menu_actions = 0;
  std::uint32_t total_bucket_rows = 0;
  for (const auto& node : data.nodes) {
    total_menu_actions += static_cast<std::uint32_t>(node.menu.size());
    total_bucket_rows += static_cast<std::uint32_t>(node.runs.size());
  }
  const std::uint8_t width = prob_entry_width(file_layout);
  const std::uint32_t total_prob_entries = [&] {
    std::uint32_t n = 0;
    for (const auto& node : data.nodes)
      for (const auto& [bucket, run] : node.runs)
        n += static_cast<std::uint32_t>(run.prob_bits.size());
    return n;
  }();
  const std::uint32_t codebook_size = (file_layout == ProbabilityLayout::Doubles)
                                          ? 0u
                                          : static_cast<std::uint32_t>(data.codebook_index.size());
  if (file_layout == ProbabilityLayout::Codebook8 && codebook_size > 256)
    throw CodebookCapacityError("pack: class " + std::to_string(data.class_id) +
                                " has > 256 distinct probabilities for Codebook8");
  if (file_layout == ProbabilityLayout::Codebook16 && codebook_size > 65536)
    throw CodebookCapacityError("pack: class " + std::to_string(data.class_id) +
                                " has > 65536 distinct probabilities for Codebook16");
  const std::uint16_t range_count = data.range ? static_cast<std::uint16_t>(data.range->size()) : 0;

  // Assign codebook indices in bit-pattern (map) order for this layout.
  std::map<std::uint64_t, std::uint16_t> local_index;
  if (file_layout != ProbabilityLayout::Doubles) {
    std::uint16_t idx = 0;
    for (auto& [pattern, slot] : data.codebook_index)
      local_index[pattern] = idx++;
  }

  // Fixed section header (40 bytes).
  write_u32(buf, data.class_id);
  write_u8(buf, static_cast<std::uint8_t>(data.canonical_board[0]));
  write_u8(buf, static_cast<std::uint8_t>(data.canonical_board[1]));
  write_u8(buf, static_cast<std::uint8_t>(data.canonical_board[2]));
  write_u8(buf, 0);  // pad to the u32 node_count at offset 8
  write_u32(buf, node_count);
  write_u16(buf, range_count);
  write_u16(buf, 0);  // padding to offset 16
  write_u32(buf, total_menu_actions);
  write_u32(buf, total_bucket_rows);
  write_u32(buf, total_prob_entries);
  write_u32(buf, codebook_size);
  write_u8(buf, 0);  // reserved2 u8[4]
  write_u8(buf, 0);
  write_u8(buf, 0);
  write_u8(buf, 0);
  while (buf.size() % 8 != 0)
    write_u8(buf, 0);  // pad to the 8-aligned range_entries offset (40)

  // Range entries (offset 40, already 8-aligned).
  if (data.range) {
    for (const solver::WeightedHand& hand : *data.range) {
      write_u64(buf, static_cast<std::uint64_t>(bs::comboIndex(hand.cards[0], hand.cards[1])));
      write_f64(buf, hand.weight);
    }
  }
  while (buf.size() % 8 != 0)
    write_u8(buf, 0);

  // Codebook f64[codebook_size] (8-aligned).
  if (codebook_size > 0)
    for (const auto& [pattern, slot] : data.codebook_index) {
      double probability;
      std::memcpy(&probability, &pattern, sizeof(probability));
      write_f64(buf, probability);
    }

  // Fixed-stride node columns.
  for (const auto& node : data.nodes)
    write_u64(buf, node.hash);

  // node_actions_offset is the SECTION-RELATIVE byte offset of each node's
  // menu, so its base is known now: after the bucket rows. Patch the offsets
  // column once the actions_blob start is computed.
  const std::size_t actions_col_pos = buf.size();
  std::uint32_t menu_cursor = 0;
  for (const auto& node : data.nodes) {
    write_u32(buf, menu_cursor * 3);  // placeholder: blob-relative, patched
    menu_cursor += static_cast<std::uint32_t>(node.menu.size());
  }
  for (const auto& node : data.nodes)
    write_u16(buf, static_cast<std::uint16_t>(node.menu.size()));
  if (node_count % 2 != 0)
    write_u16(buf, 0);  // pad to a u32 column

  std::uint32_t bucket_cursor = 0;
  for (const auto& node : data.nodes) {
    write_u32(buf, bucket_cursor);  // first bucket_rows index
    bucket_cursor += static_cast<std::uint32_t>(node.runs.size());
  }
  for (const auto& node : data.nodes)
    write_u16(buf, static_cast<std::uint16_t>(node.runs.size()));
  while (buf.size() % 8 != 0)
    write_u8(buf, 0);
  const std::size_t buckets_start = buf.size();

  // actions_blob follows bucket_rows; probs follows actions. Compute the
  // section-relative probs start so bucket rows can name their prob offsets.
  const std::size_t actions_bytes = static_cast<std::size_t>(total_menu_actions) * 3;
  const std::size_t probs_unaligned =
      buckets_start + static_cast<std::size_t>(total_bucket_rows) * 8 + actions_bytes;
  const std::size_t probs_start = [&] {
    std::size_t p = probs_unaligned;
    if (file_layout == ProbabilityLayout::Codebook16)
      p = (p + 1) & ~std::size_t{1};
    else if (file_layout == ProbabilityLayout::Doubles)
      p = (p + 7) & ~std::size_t{7};
    return p;
  }();

  // bucket_rows (sorted by bucket within each node; node order is hash order).
  std::uint32_t prob_entry_cursor = 0;
  for (const auto& node : data.nodes) {
    for (const auto& [bucket, run] : node.runs) {
      write_u16(buf, bucket);
      write_u16(buf, 0);  // reserved
      write_u32(buf, static_cast<std::uint32_t>(
                         probs_start + static_cast<std::size_t>(prob_entry_cursor) * width));
      prob_entry_cursor += static_cast<std::uint32_t>(run.prob_bits.size());
    }
  }

  // Patch node_actions_offset to be section-relative (actions blob base plus
  // the node's blob-relative position).
  const std::size_t actions_blob_start =
      buckets_start + static_cast<std::size_t>(total_bucket_rows) * 8;
  {
    std::uint32_t run_bytes = 0;
    for (const auto& node : data.nodes) {
      const std::uint32_t abs_off = static_cast<std::uint32_t>(actions_blob_start + run_bytes);
      for (int b = 0; b < 4; ++b)
        buf[actions_col_pos + (&node - data.nodes.data()) * 4 + b] =
            static_cast<std::uint8_t>((abs_off >> (b * 8)) & 0xffu);
      run_bytes += static_cast<std::uint32_t>(node.menu.size()) * 3;
    }
  }

  // actions_blob.
  for (const auto& node : data.nodes)
    for (const poker::Action& action : node.menu) {
      if (action.target_total > 0xffff)
        throw std::invalid_argument("pack: action target exceeds uint16 range");
      write_u8(buf, static_cast<std::uint8_t>(action.type));
      write_u16(buf, static_cast<std::uint16_t>(action.target_total));
    }
  while (buf.size() < probs_start)
    write_u8(buf, 0);

  // probs: one menu-length run per bucket row, node-major bucket order.
  for (const auto& node : data.nodes)
    for (const auto& [bucket, run] : node.runs)
      for (std::uint64_t pattern : run.prob_bits) {
        if (file_layout == ProbabilityLayout::Codebook8) {
          write_u8(buf, local_index.at(pattern));
        } else if (file_layout == ProbabilityLayout::Codebook16) {
          write_u16(buf, local_index.at(pattern));
        } else {
          double probability;
          std::memcpy(&probability, &pattern, sizeof(probability));
          write_f64(buf, probability);
        }
      }
}

}  // namespace

namespace {

// Validate the identity handed to the assembler: both digests or neither.
void validate_identity(const LibraryIdentity& identity) {
  if (identity.card_abstraction_digest.has_value() !=
      identity.action_abstraction_digest.has_value())
    throw std::invalid_argument(
        "assemble_binary_library: identity must supply both abstraction digests or neither");
}

// Build the fixed header (+ sizes + zeroed class_offsets) for a library.
std::vector<std::uint8_t> build_header(const solver::UnifiedGame& game, std::size_t class_count,
                                       ProbabilityLayout layout, const LibraryIdentity& identity) {
  std::size_t sizes_bytes = 0;
  for (const auto& street : game.sizes) {
    sizes_bytes += 2;
    sizes_bytes += street.bets.size() * 16;
    sizes_bytes += street.raises.size() * 16;
  }
  constexpr std::size_t kHeaderFixed = 216;
  const std::size_t header_fixed = kHeaderFixed + sizes_bytes;
  const std::size_t header_total =
      align_up(header_fixed + class_count * sizeof(std::uint64_t), kPageSize);

  std::vector<std::uint8_t> header;
  header.reserve(header_total);
  header.insert(header.end(), kMagic, kMagic + 8);
  write_u32(header, kVersion);
  write_u32(header, static_cast<std::uint32_t>(class_count));
  write_u8(header, codebook_type_to_u8(layout));
  write_u8(header, static_cast<std::uint8_t>(game.def.player_count));
  write_u8(header, static_cast<std::uint8_t>(game.def.button));
  write_u8(header, game.def.terminal == poker::TerminalDepth::Flop ? 0 : 1);
  write_u8(header, static_cast<std::uint8_t>(
                       static_cast<std::underlying_type_t<abstraction::CardBucketKind>>(
                           solver::kNSeatCardKind)));
  for (int i = 0; i < 3; ++i)
    write_u8(header, 0);
  write_u64(header, game.def.big_blind);
  write_u64(header, game.def.pot);
  for (std::size_t i = 0; i < kMaxSeats; ++i)
    write_u64(header, game.def.stacks[i]);
  for (std::size_t i = 0; i < kMaxSeats; ++i)
    write_u64(header, game.def.contributions[i]);
  write_u64(header, identity.card_abstraction_digest.value_or(0));
  write_u64(header, identity.action_abstraction_digest.value_or(0));
  for (const auto& street : game.sizes) {
    write_u8(header, static_cast<std::uint8_t>(street.bets.size()));
    write_u8(header, static_cast<std::uint8_t>(street.raises.size()));
    for (const auto& frac : street.bets)
      write_fraction(header, frac);
    for (const auto& frac : street.raises)
      write_fraction(header, frac);
  }
  for (std::size_t i = 0; i < class_count; ++i)
    write_u64(header, 0);
  header.resize(header_total, 0);
  return header;
}

std::size_t class_offsets_position(const solver::UnifiedGame& game) {
  std::size_t sizes_bytes = 0;
  for (const auto& street : game.sizes) {
    sizes_bytes += 2;
    sizes_bytes += street.bets.size() * 16;
    sizes_bytes += street.raises.size() * 16;
  }
  return 216 + sizes_bytes;
}

// Read one section file into memory after validating its header fields against
// the build parameters. Returns the bytes.
std::vector<std::uint8_t> read_section(const std::filesystem::path& path, std::uint32_t expected_id,
                                       ProbabilityLayout layout) {
  std::ifstream in(path, std::ios::binary);
  if (!in)
    throw std::runtime_error("assemble: cannot open section " + path.string());
  in.seekg(0, std::ios::end);
  const std::streamoff size = in.tellg();
  in.seekg(0, std::ios::beg);
  if (size < 40)
    throw std::runtime_error("assemble: truncated section " + path.string());
  std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
  in.read(reinterpret_cast<char*>(bytes.data()), size);
  if (!in)
    throw std::runtime_error("assemble: short read on section " + path.string());

  const std::uint32_t class_id = read_u32(bytes.data());
  if (class_id != expected_id)
    throw std::runtime_error("assemble: section " + path.string() + " names class " +
                             std::to_string(class_id) + ", expected " +
                             std::to_string(expected_id));
  const std::uint32_t node_count = read_u32(bytes.data() + 8);
  const std::uint16_t range_count = read_u16(bytes.data() + 12);
  const std::uint32_t total_menu_actions = read_u32(bytes.data() + 16);
  const std::uint32_t total_bucket_rows = read_u32(bytes.data() + 20);
  const std::uint32_t total_prob_entries = read_u32(bytes.data() + 24);
  const std::uint32_t codebook_size = read_u32(bytes.data() + 28);

  // Recompute the exact byte length the writer must have produced for this
  // layout. This is the authoritative consistency check: a section does not
  // record the layout kind, so a mismatched layout shows up here.
  std::size_t expected = 40 + static_cast<std::size_t>(range_count) * 16;
  expected = align_up(expected, 8);
  expected +=
      (layout == ProbabilityLayout::Doubles ? 0 : static_cast<std::size_t>(codebook_size)) * 8;
  expected += static_cast<std::size_t>(node_count) * (8 + 4 + 2);
  if (node_count % 2 != 0)
    expected += 2;
  expected += static_cast<std::size_t>(node_count) * (4 + 2);
  expected = align_up(expected, 8);
  expected += static_cast<std::size_t>(total_bucket_rows) * 8;
  expected += static_cast<std::size_t>(total_menu_actions) * 3;
  if (layout == ProbabilityLayout::Codebook16)
    expected = (expected + 1) & ~std::size_t{1};
  else if (layout == ProbabilityLayout::Doubles)
    expected = (expected + 7) & ~std::size_t{7};
  expected += static_cast<std::size_t>(total_prob_entries) * prob_entry_width(layout);
  if (bytes.size() != expected)
    throw std::runtime_error(
        "assemble: section " + path.string() + " is " + std::to_string(bytes.size()) +
        " bytes; length " + std::to_string(expected) + " was required by its counters under `" +
        (layout == ProbabilityLayout::Codebook16
             ? "codebook16"
             : (layout == ProbabilityLayout::Doubles ? "doubles" : "codebook8")) +
        "` (layout mismatch or corruption)");
  return bytes;
}

}  // namespace

std::vector<std::uint8_t> serialize_class_section(const PackClassInput& input,
                                                  ProbabilityLayout layout) {
  if (input.rows == nullptr)
    throw std::invalid_argument("serialize_class_section: rows are required");
  std::vector<solver::WeightedHand> range_scratch;
  const std::vector<solver::WeightedHand>* range = nullptr;
  if (input.range != nullptr) {
    range = &canonical_range(*input.range, range_scratch);
  }
  ClassBuildData data = build_class_data(input, range);
  std::vector<std::uint8_t> section;
  write_class_section(section, data, layout, 0, 0);
  return section;
}

void assemble_binary_library(const std::filesystem::path& path, const solver::UnifiedGame& game,
                             const LibraryIdentity& identity, ProbabilityLayout layout,
                             const std::filesystem::path& section_dir,
                             std::span<const std::uint32_t> class_ids) {
  if (class_ids.empty())
    throw std::invalid_argument("assemble_binary_library: no classes to assemble");
  if (game.def.player_count < 2 || game.def.player_count > kMaxSeats)
    throw std::invalid_argument("assemble_binary_library: player_count out of range");
  validate_identity(identity);
  for (std::size_t i = 1; i < class_ids.size(); ++i)
    if (class_ids[i] <= class_ids[i - 1])
      throw std::invalid_argument("assemble_binary_library: class ids must be strictly ascending");

  // Validate every section before writing any output byte.
  std::vector<std::vector<std::uint8_t>> sections;
  sections.reserve(class_ids.size());
  for (std::uint32_t class_id : class_ids) {
    char name[32];
    std::snprintf(name, sizeof(name), "class-%04u.bsf", class_id);
    sections.push_back(read_section(section_dir / name, class_id, layout));
  }

  const std::filesystem::path temp_path = path.string() + ".tmp";
  struct TempCleanup {
    std::filesystem::path temp;
    std::filesystem::path final_path;
    bool armed = true;
    ~TempCleanup() {
      if (!armed)
        return;
      std::error_code ec;
      std::filesystem::remove(temp, ec);
      std::filesystem::remove(std::filesystem::path(temp.string() + ".sha256"), ec);
      std::filesystem::remove(final_path, ec);
      std::filesystem::remove(std::filesystem::path(final_path.string() + ".sha256"), ec);
    }
  } cleanup{temp_path, path};

  std::vector<std::uint8_t> header = build_header(game, class_ids.size(), layout, identity);
  const std::size_t offsets_pos = class_offsets_position(game);

  std::ofstream out(temp_path, std::ios::binary | std::ios::trunc);
  if (!out)
    throw std::runtime_error("assemble_binary_library: cannot open " + temp_path.string());
  out.write(reinterpret_cast<const char*>(header.data()),
            static_cast<std::streamsize>(header.size()));
  if (!out)
    throw std::runtime_error("assemble_binary_library: header write failed");

  std::vector<std::uint64_t> section_offsets(class_ids.size());
  for (std::size_t i = 0; i < sections.size(); ++i) {
    section_offsets[i] = static_cast<std::uint64_t>(out.tellp());
    out.write(reinterpret_cast<const char*>(sections[i].data()),
              static_cast<std::streamsize>(sections[i].size()));
    if (!out)
      throw std::runtime_error("assemble_binary_library: section write failed");
    const std::size_t current = static_cast<std::size_t>(out.tellp());
    const std::size_t aligned = align_up(current, kPageSize);
    if (aligned > current) {
      const std::vector<std::uint8_t> padding(aligned - current, 0);
      out.write(reinterpret_cast<const char*>(padding.data()),
                static_cast<std::streamsize>(padding.size()));
      if (!out)
        throw std::runtime_error("assemble_binary_library: padding write failed");
    }
  }
  out.seekp(static_cast<std::streamoff>(offsets_pos));
  for (std::uint64_t offset : section_offsets)
    write_u64_to(out, offset);
  if (!out)
    throw std::runtime_error("assemble_binary_library: class_offsets patch failed");
  out.flush();
  if (!out)
    throw std::runtime_error("assemble_binary_library: flush failed");
  out.close();

  // fsync the temp file, then publish atomically.
  {
    const int fd = ::open(temp_path.string().c_str(), O_RDONLY);
    if (fd < 0)
      throw std::runtime_error("assemble_binary_library: cannot reopen temp for fsync");
    if (::fsync(fd) != 0) {
      ::close(fd);
      throw std::runtime_error("assemble_binary_library: fsync failed");
    }
    ::close(fd);
  }
  std::filesystem::rename(temp_path, path);

  const std::string hex = artifacts::sha256_file_hex(path);
  const std::filesystem::path sidecar = path.string() + ".sha256";
  std::ofstream sidecar_out(sidecar, std::ios::binary | std::ios::trunc);
  if (!sidecar_out)
    throw std::runtime_error("assemble_binary_library: cannot open " + sidecar.string());
  sidecar_out << hex << "  " << path.filename().string() << "\n";
  if (!sidecar_out)
    throw std::runtime_error("assemble_binary_library: sidecar write failed");
  cleanup.armed = false;
}

void assemble_binary_library(const std::filesystem::path& path, const solver::UnifiedGame& game,
                             const LibraryIdentity& identity, ProbabilityLayout layout,
                             std::span<const ClassSectionInput> sections) {
  if (sections.empty())
    throw std::invalid_argument("assemble_binary_library: no classes to assemble");
  const std::filesystem::path staging =
      path.parent_path() / (".bs-assemble-" + std::to_string(::getpid()));
  struct StagingCleanup {
    std::filesystem::path dir;
    ~StagingCleanup() {
      std::error_code ec;
      std::filesystem::remove_all(dir, ec);
    }
  } staging_cleanup{staging};
  std::error_code ec;
  std::filesystem::create_directories(staging, ec);
  if (ec)
    throw std::runtime_error("assemble_binary_library: cannot create staging dir: " + ec.message());

  std::vector<std::uint32_t> class_ids;
  class_ids.reserve(sections.size());
  for (const ClassSectionInput& section : sections) {
    class_ids.push_back(section.class_id);
    char name[32];
    std::snprintf(name, sizeof(name), "class-%04u.bsf", section.class_id);
    std::ofstream out(staging / name, std::ios::binary | std::ios::trunc);
    if (!out)
      throw std::runtime_error("assemble_binary_library: cannot stage section");
    out.write(reinterpret_cast<const char*>(section.bytes.data()),
              static_cast<std::streamsize>(section.bytes.size()));
    if (!out)
      throw std::runtime_error("assemble_binary_library: staging write failed");
  }
  std::sort(class_ids.begin(), class_ids.end());
  assemble_binary_library(path, game, identity, layout, staging, class_ids);
}

void pack_binary_library_streaming(const std::filesystem::path& path,
                                   const solver::UnifiedGame& game, std::size_t class_count,
                                   const PackClassSupplier& supplier) {
  if (class_count == 0)
    throw std::invalid_argument("pack_binary_library_streaming: no classes to pack");
  if (game.def.player_count < 2 || game.def.player_count > kMaxSeats)
    throw std::invalid_argument("pack_binary_library_streaming: player_count out of range");

  // One pass under `layout`: serialize every class into a section temp while
  // reconciling the shared digests, then assemble. A class that exceeds the
  // layout's codebook cap aborts the pass so the caller can rerun wider.
  auto run_pass = [&](ProbabilityLayout layout) -> bool {
    const std::filesystem::path staging =
        path.parent_path() / (".bs-pack-" + std::to_string(::getpid()));
    struct StagingCleanup {
      std::filesystem::path dir;
      ~StagingCleanup() {
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
      }
    } staging_cleanup{staging};
    std::error_code ec;
    std::filesystem::create_directories(staging, ec);
    if (ec)
      throw std::runtime_error("pack_binary_library_streaming: cannot create staging dir");

    LibraryIdentity identity;
    std::vector<std::uint32_t> class_ids;
    class_ids.reserve(class_count);
    for (std::size_t i = 0; i < class_count; ++i) {
      const PackClassInput input = supplier(i);
      try {
        std::vector<std::uint8_t> section = serialize_class_section(input, layout);
        char name[32];
        std::snprintf(name, sizeof(name), "class-%04u.bsf", input.class_id);
        std::ofstream out(staging / name, std::ios::binary | std::ios::trunc);
        if (!out)
          throw std::runtime_error("pack_binary_library_streaming: cannot stage section");
        out.write(reinterpret_cast<const char*>(section.data()),
                  static_cast<std::streamsize>(section.size()));
        if (!out)
          throw std::runtime_error("pack_binary_library_streaming: staging write failed");
        class_ids.push_back(input.class_id);
      } catch (const CodebookTooLarge&) {
        return false;  // rerun under a wider layout
      }
      // Reconcile digests exactly as before (once any class supplies them, all
      // must supply identical values).
      bool have = identity.card_abstraction_digest.has_value();
      std::uint64_t card_digest = identity.card_abstraction_digest.value_or(0);
      std::uint64_t action_digest = identity.action_abstraction_digest.value_or(0);
      reconcile_digests(input, have, card_digest, action_digest);
      identity.card_abstraction_digest = card_digest;
      identity.action_abstraction_digest = action_digest;
      if (!have) {
        identity.card_abstraction_digest.reset();
        identity.action_abstraction_digest.reset();
      }
    }
    std::sort(class_ids.begin(), class_ids.end());
    if (std::adjacent_find(class_ids.begin(), class_ids.end()) != class_ids.end())
      throw std::runtime_error("pack_binary_library_streaming: duplicate class id");
    assemble_binary_library(path, game, identity, layout, staging, class_ids);
    return true;
  };

  // Single-pass default: Codebook16 indexes up to 65,535 distinct probability
  // values per class, which every flop class stays well under; this avoids the
  // old survey pass that read every artifact twice. Only a class exceeding the
  // cap triggers a second pass, under exact Doubles.
  if (!run_pass(ProbabilityLayout::Codebook16)) {
    std::fprintf(stderr, "pack: class exceeded the Codebook16 cap; rerunning with exact doubles\n");
    if (!run_pass(ProbabilityLayout::Doubles))
      throw std::runtime_error("pack_binary_library_streaming: Doubles pass unexpectedly failed");
  }
}

// ---------------------------------------------------------------------------
// MmapLibrary
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// MmapLibrary
// ---------------------------------------------------------------------------

struct MmapLibrary::Impl {
  int fd = -1;
  std::uint8_t* data = nullptr;
  std::size_t size = 0;

  // Parsed header.
  std::uint32_t class_count = 0;
  ProbabilityLayout codebook_type = ProbabilityLayout::Doubles;
  std::uint8_t card_bucket_kind = 0;
  std::uint64_t card_digest = 0;
  std::uint64_t action_digest = 0;
  solver::UnifiedGame game;
  std::string sha256_hex;

  // One bucket row: bucket id plus a section-relative byte offset into probs.
  // Matches the on-disk 8-byte BucketEntry (u16 bucket, u16 reserved, u32).
#pragma pack(push, 1)
  struct BucketRecord {
    std::uint16_t bucket = 0;
    std::uint16_t reserved = 0;
    std::uint32_t prob_offset = 0;
  };
#pragma pack(pop)
  static_assert(sizeof(BucketRecord) == 8);
  // Parsed class section. All column pointers reference the mmap region.
  struct ClassInfo {
    std::uint32_t class_id = 0;
    std::array<int, 3> canonical_board{};
    const std::uint8_t* section_base = nullptr;  // for section-relative offsets
    std::uint32_t node_count = 0;
    std::uint32_t total_menu_actions = 0;
    std::uint32_t total_bucket_rows = 0;
    std::uint32_t total_prob_entries = 0;
    std::uint32_t codebook_size = 0;
    const std::uint64_t* node_hash = nullptr;
    const std::uint32_t* node_actions_offset = nullptr;
    const std::uint16_t* node_actions_count = nullptr;
    const std::uint32_t* node_bucket_start = nullptr;
    const std::uint16_t* node_bucket_count = nullptr;
    const BucketRecord* bucket_rows = nullptr;  // total_bucket_rows records
    const CompactAction* actions = nullptr;     // actions_blob base
    const std::uint8_t* prob_indices_8 = nullptr;
    const std::uint16_t* prob_indices_16 = nullptr;
    const double* probabilities = nullptr;
    const double* codebook = nullptr;
    std::vector<solver::WeightedHand> range;
  };
  std::vector<ClassInfo> classes;
  // class_id → index into classes.
  std::unordered_map<std::uint32_t, std::size_t> class_id_map;
  // board key → class_id.
  std::unordered_map<std::uint64_t, std::uint32_t> board_map;

  ~Impl() { unmap(); }

  void unmap() {
    if (data) {
      ::munmap(data, size);
      data = nullptr;
    }
    if (fd >= 0) {
      ::close(fd);
      fd = -1;
    }
    size = 0;
  }

  const ClassInfo& find_class(std::uint32_t class_id) const {
    const auto it = class_id_map.find(class_id);
    if (it == class_id_map.end())
      throw std::out_of_range("mmap library: unknown class_id " + std::to_string(class_id));
    return classes[it->second];
  }
};

MmapLibrary::MmapLibrary() : impl_(std::make_unique<Impl>()) {}

MmapLibrary::~MmapLibrary() {
  // close() is called by the destructor; Impl's destructor also unmaps.
  close();
}

MmapLibrary::MmapLibrary(MmapLibrary&&) noexcept = default;
MmapLibrary& MmapLibrary::operator=(MmapLibrary&&) noexcept = default;

void MmapLibrary::close() {
  if (impl_) {
    impl_->unmap();
    impl_->classes.clear();
    impl_->class_id_map.clear();
    impl_->board_map.clear();
    impl_->class_count = 0;
    impl_->sha256_hex.clear();
  }
}

void MmapLibrary::open(const std::filesystem::path& path, bool verify_sha256) {
  close();

  // Read the SHA-256 sidecar (always, so sha256_hex() is populated even
  // when full verification is skipped).
  const std::filesystem::path sidecar = path.string() + ".sha256";
  std::string expected_hex;
  {
    std::ifstream in(sidecar);
    if (!in)
      throw std::runtime_error("mmap library: cannot open sidecar " + sidecar.string());
    in >> expected_hex;
    if (expected_hex.size() != 64)
      throw std::runtime_error("mmap library: malformed sidecar " + sidecar.string());
  }

  // Optionally verify the SHA-256 of the binary file. Full verification
  // reads every byte and is unsuitable for large libraries on the startup
  // path; the sidecar remains available for offline verification.
  if (verify_sha256) {
    const std::string actual_hex = artifacts::sha256_file_hex(path);
    if (actual_hex != expected_hex)
      throw std::runtime_error("mmap library: SHA-256 mismatch for " + path.string() +
                               " (expected " + expected_hex + ", got " + actual_hex + ")");
    impl_->sha256_hex = actual_hex;
  } else {
    impl_->sha256_hex = expected_hex;
  }

  // Open and mmap the file.
  const int fd = ::open(path.c_str(), O_RDONLY);
  if (fd < 0)
    throw std::runtime_error("mmap library: cannot open " + path.string() + " (errno " +
                             std::to_string(errno) + ")");

  struct stat st = {};
  if (::fstat(fd, &st) != 0) {
    ::close(fd);
    throw std::runtime_error("mmap library: fstat failed for " + path.string());
  }
  if (st.st_size < static_cast<off_t>(kPageSize)) {
    ::close(fd);
    throw std::runtime_error("mmap library: file too small " + path.string());
  }

  void* mapped =
      ::mmap(nullptr, static_cast<std::size_t>(st.st_size), PROT_READ, MAP_PRIVATE, fd, 0);
  if (mapped == MAP_FAILED) {
    ::close(fd);
    throw std::runtime_error("mmap library: mmap failed for " + path.string() + " (errno " +
                             std::to_string(errno) + ")");
  }
  // The mapping remains valid after close().
  ::close(fd);

  impl_->fd = -1;  // already closed
  impl_->data = static_cast<std::uint8_t*>(mapped);
  impl_->size = static_cast<std::size_t>(st.st_size);

  // Parse the header.
  const std::uint8_t* base = impl_->data;
  if (std::memcmp(base, kMagic, 8) != 0)
    throw std::runtime_error("mmap library: bad magic in " + path.string());
  const std::uint32_t version = read_u32(base + 8);
  if (version != kVersion)
    throw std::runtime_error("mmap library: unsupported version " + std::to_string(version) +
                             " in " + path.string());
  impl_->class_count = read_u32(base + 12);
  if (impl_->class_count == 0)
    throw std::runtime_error("mmap library: zero classes in " + path.string());
  impl_->codebook_type = codebook_type_from_u8(read_u8(base + 16));
  const std::uint8_t player_count = read_u8(base + 17);
  const std::uint8_t button = read_u8(base + 18);
  const std::uint8_t terminal_depth = read_u8(base + 19);
  impl_->card_bucket_kind = read_u8(base + 20);
  if (impl_->card_bucket_kind !=
      static_cast<std::uint8_t>(
          static_cast<std::underlying_type_t<abstraction::CardBucketKind>>(solver::kNSeatCardKind)))
    throw std::runtime_error(
        "mmap library: card bucket kind does not match the compiled engine in " + path.string());
  impl_->card_digest = read_u64(base + 200);
  impl_->action_digest = read_u64(base + 208);
  // Cross-check against the resident's loaded policy identity at the build
  // site via card_abstraction_digest()/action_abstraction_digest() accessors;
  // open() itself validates the compiled bucket kind.

  // Build the shared game identity.
  impl_->game.def.player_count = player_count;
  impl_->game.def.button = button;
  impl_->game.def.big_blind = read_u64(base + 24);
  impl_->game.def.pot = read_u64(base + 32);
  for (std::size_t i = 0; i < kMaxSeats; ++i)
    impl_->game.def.stacks[i] = read_u64(base + 40 + i * 8);
  for (std::size_t i = 0; i < kMaxSeats; ++i)
    impl_->game.def.contributions[i] = read_u64(base + 120 + i * 8);
  impl_->game.def.terminal =
      terminal_depth == 0 ? poker::TerminalDepth::Flop : poker::TerminalDepth::River;
  impl_->game.def.board = {0, 0, 0, 0, 0};
  impl_->game.def.board_size = 0;

  // Parse sizes (v2 header: sizes begin at offset 216 after the two digests).
  {
    const std::uint8_t* p = base + 216;
    for (std::size_t street = 0; street < 4; ++street) {
      const std::uint8_t bets_count = read_u8(p);
      const std::uint8_t raises_count = read_u8(p + 1);
      p += 2;
      auto& street_sizes = impl_->game.sizes[street];
      street_sizes.bets.clear();
      street_sizes.raises.clear();
      for (std::uint8_t i = 0; i < bets_count; ++i) {
        street_sizes.bets.push_back(read_fraction(p));
        p += 16;
      }
      for (std::uint8_t i = 0; i < raises_count; ++i) {
        street_sizes.raises.push_back(read_fraction(p));
        p += 16;
      }
    }
  }

  // Parse class offsets and class sections. The sizes section starts at
  // offset 216; its total byte length is recomputed by walking the four
  // street entries (2-byte header + 16 bytes per fraction).
  std::size_t sizes_bytes = 0;
  {
    const std::uint8_t* p = base + 216;
    for (std::size_t street = 0; street < 4; ++street) {
      const std::uint8_t bets_count = read_u8(p);
      const std::uint8_t raises_count = read_u8(p + 1);
      sizes_bytes += 2 + static_cast<std::size_t>(bets_count) * 16 +
                     static_cast<std::size_t>(raises_count) * 16;
      p += 2 + static_cast<std::size_t>(bets_count) * 16 +
           static_cast<std::size_t>(raises_count) * 16;
    }
  }

  const std::size_t class_offsets_pos = 216 + sizes_bytes;
  const std::uint8_t* offsets_ptr = base + class_offsets_pos;

  impl_->classes.reserve(impl_->class_count);
  for (std::uint32_t i = 0; i < impl_->class_count; ++i) {
    const std::uint64_t section_offset = read_u64(offsets_ptr + i * 8);
    if (section_offset >= impl_->size)
      throw std::runtime_error("mmap library: class section offset out of bounds");
    // Sections are page-aligned and ordered; bound each parse by the next
    // section start (or the file end) so every offset is range-checked.
    const std::uint64_t next_offset = (i + 1 < impl_->class_count)
                                          ? read_u64(offsets_ptr + (i + 1) * 8)
                                          : static_cast<std::uint64_t>(impl_->size);
    if (next_offset <= section_offset || next_offset > impl_->size)
      throw std::runtime_error("mmap library: class section bounds invalid");
    const std::uint8_t* sec = base + section_offset;
    const std::uint8_t* sec_end = base + next_offset;
    auto in_bounds = [&](const std::uint8_t* p, std::size_t bytes) {
      return p >= sec && p <= sec_end && p + bytes <= sec_end;
    };

    Impl::ClassInfo info;
    info.section_base = sec;
    info.class_id = read_u32(sec);
    info.canonical_board = {static_cast<int>(read_u8(sec + 4)), static_cast<int>(read_u8(sec + 5)),
                            static_cast<int>(read_u8(sec + 6))};
    info.node_count = read_u32(sec + 8);
    const std::uint16_t range_count = read_u16(sec + 12);
    info.total_menu_actions = read_u32(sec + 16);
    info.total_bucket_rows = read_u32(sec + 20);
    info.total_prob_entries = read_u32(sec + 24);
    info.codebook_size = read_u32(sec + 28);

    // Walk a section-relative byte cursor; section starts are 4096-aligned,
    // so relative and absolute alignments agree.
    std::size_t o = 40;
    auto at = [&](std::size_t rel) -> const std::uint8_t* {
      if (rel >= static_cast<std::size_t>(sec_end - sec))
        throw std::runtime_error(
            "mmap library: section offset out of bounds (rel=" + std::to_string(rel) +
            " sec_len=" + std::to_string(sec_end - sec) + " class=" + std::to_string(i) + ")");
      return sec + rel;
    };
    info.range.reserve(range_count);
    for (std::uint16_t r = 0; r < range_count; ++r) {
      const std::uint64_t combo_idx = read_u64(at(o));
      const double weight = read_f64(at(o) + 8);
      info.range.push_back(
          solver::WeightedHand{bs::comboCards(static_cast<int>(combo_idx)), weight});
      o += 16;
    }
    o = (o + 7) & ~std::size_t{7};  // pad to codebook f64 region

    if (info.codebook_size > 0) {
      info.codebook = reinterpret_cast<const double*>(at(o));
      o += static_cast<std::size_t>(info.codebook_size) * 8;
    }

    info.node_hash = reinterpret_cast<const std::uint64_t*>(at(o));
    o += static_cast<std::size_t>(info.node_count) * 8;
    info.node_actions_offset = reinterpret_cast<const std::uint32_t*>(at(o));
    o += static_cast<std::size_t>(info.node_count) * 4;
    info.node_actions_count = reinterpret_cast<const std::uint16_t*>(at(o));
    o += static_cast<std::size_t>(info.node_count) * 2;
    o = (o + 3) & ~std::size_t{3};  // pad to the u32 bucket-start column
    info.node_bucket_start = reinterpret_cast<const std::uint32_t*>(at(o));
    o += static_cast<std::size_t>(info.node_count) * 4;
    info.node_bucket_count = reinterpret_cast<const std::uint16_t*>(at(o));
    const std::uint16_t* node_bucket_count = info.node_bucket_count;
    o += static_cast<std::size_t>(info.node_count) * 2;
    o = (o + 7) & ~std::size_t{7};  // pad to bucket rows (8-byte records)

    info.bucket_rows = reinterpret_cast<const Impl::BucketRecord*>(at(o));
    o += static_cast<std::size_t>(info.total_bucket_rows) * 8;
    info.actions = reinterpret_cast<const CompactAction*>(at(o));
    o += static_cast<std::size_t>(info.total_menu_actions) * 3;

    // Cross-check the declared counters before trusting any offset.
    std::uint32_t menu_sum = 0;
    std::uint32_t bucket_sum = 0;
    std::uint32_t prob_sum = 0;
    for (std::uint32_t n = 0; n < info.node_count; ++n) {
      menu_sum += info.node_actions_count[n];
      bucket_sum += node_bucket_count[n];
      prob_sum += static_cast<std::uint32_t>(info.node_actions_count[n]) * node_bucket_count[n];
      if (info.node_bucket_start[n] > info.total_bucket_rows ||
          static_cast<std::uint64_t>(info.node_bucket_start[n]) + node_bucket_count[n] >
              info.total_bucket_rows)
        throw std::runtime_error("mmap library: node bucket slice out of bounds");
    }
    if (menu_sum != info.total_menu_actions || bucket_sum != info.total_bucket_rows ||
        prob_sum != info.total_prob_entries)
      throw std::runtime_error("mmap library: section counter cross-check failed");

    const std::size_t width = prob_entry_width(impl_->codebook_type);
    o = (width == 1) ? o : (o + width - 1) & ~(width - 1);
    if (o + static_cast<std::size_t>(info.total_prob_entries) * width >
        static_cast<std::size_t>(sec_end - sec))
      throw std::runtime_error("mmap library: probability region out of bounds");
    const std::uint8_t* probs = sec + o;
    info.prob_indices_8 = (impl_->codebook_type == ProbabilityLayout::Codebook8) ? probs : nullptr;
    info.prob_indices_16 = (impl_->codebook_type == ProbabilityLayout::Codebook16)
                               ? reinterpret_cast<const std::uint16_t*>(probs)
                               : nullptr;
    info.probabilities = (impl_->codebook_type == ProbabilityLayout::Doubles)
                             ? reinterpret_cast<const double*>(probs)
                             : nullptr;

    impl_->classes.push_back(std::move(info));
  }

  // Build lookup maps.
  for (std::size_t i = 0; i < impl_->classes.size(); ++i) {
    const auto& info = impl_->classes[i];
    impl_->class_id_map[info.class_id] = i;
    impl_->board_map[board_key(info.canonical_board)] = info.class_id;
  }
}

bool MmapLibrary::find_bucket(std::uint32_t class_id,
                              std::span<const std::uint8_t> public_compact_key,
                              std::uint16_t bucket, CompactRowView& out) const {
  const auto it = impl_->class_id_map.find(class_id);
  if (it == impl_->class_id_map.end())
    return false;
  const Impl::ClassInfo& info = impl_->classes[it->second];

  const std::uint64_t target_hash = hash_key_64(public_compact_key);

  // First binary search: sorted node hashes.
  std::size_t lo = 0;
  std::size_t hi = info.node_count;
  while (lo < hi) {
    const std::size_t mid = lo + (hi - lo) / 2;
    const std::uint64_t mid_hash = info.node_hash[mid];
    if (mid_hash < target_hash)
      lo = mid + 1;
    else
      hi = mid;
  }
  if (lo >= info.node_count || info.node_hash[lo] != target_hash)
    return false;  // unknown public node

  const std::uint32_t bucket_start = info.node_bucket_start[lo];
  const std::uint16_t bucket_count = info.node_bucket_count[lo];
  if (bucket_count == 0)
    return false;

  // Second binary search: bucket id within the node's contiguous slice.
  const Impl::BucketRecord* rows = info.bucket_rows + bucket_start;
  std::size_t blo = 0;
  std::size_t bhi = bucket_count;
  while (blo < bhi) {
    const std::size_t mid = blo + (bhi - blo) / 2;
    if (rows[mid].bucket < bucket)
      blo = mid + 1;
    else
      bhi = mid;
  }
  if (blo >= bucket_count || rows[blo].bucket != bucket)
    return false;  // node exists but this holding's bucket was never trained

  const std::uint32_t menu_byte_offset = info.node_actions_offset[lo];
  const std::uint16_t action_count = info.node_actions_count[lo];
  const std::uint32_t prob_byte_offset = rows[blo].prob_offset;

  out.count = action_count;
  out.actions = reinterpret_cast<const CompactAction*>(info.section_base + menu_byte_offset);
  out.prob_layout = impl_->codebook_type;
  const std::uint8_t* prob_ptr = info.section_base + prob_byte_offset;
  switch (impl_->codebook_type) {
    case ProbabilityLayout::Codebook8:
      out.codebook = info.codebook;
      out.probability_indices = prob_ptr;
      out.probability_indices_16 = nullptr;
      out.probabilities = nullptr;
      break;
    case ProbabilityLayout::Codebook16:
      out.codebook = info.codebook;
      out.probability_indices = nullptr;
      out.probability_indices_16 = reinterpret_cast<const std::uint16_t*>(prob_ptr);
      out.probabilities = nullptr;
      break;
    case ProbabilityLayout::Doubles:
      out.codebook = nullptr;
      out.probability_indices = nullptr;
      out.probability_indices_16 = nullptr;
      out.probabilities = reinterpret_cast<const double*>(prob_ptr);
      break;
  }
  return true;
}

std::int32_t MmapLibrary::class_id_for_board(const std::array<int, 3>& canonical_board) const {
  const auto it = impl_->board_map.find(board_key(canonical_board));
  if (it == impl_->board_map.end())
    return -1;
  return static_cast<std::int32_t>(it->second);
}

std::size_t MmapLibrary::class_count() const noexcept {
  return impl_->class_count;
}

std::uint32_t MmapLibrary::class_id_at(std::size_t index) const {
  return impl_->classes.at(index).class_id;
}

const std::array<int, 3>& MmapLibrary::canonical_board(std::uint32_t class_id) const {
  return impl_->find_class(class_id).canonical_board;
}

std::size_t MmapLibrary::node_count(std::uint32_t class_id) const {
  return impl_->find_class(class_id).node_count;
}

std::size_t MmapLibrary::bucket_row_count(std::uint32_t class_id) const {
  return impl_->find_class(class_id).total_bucket_rows;
}

std::size_t MmapLibrary::menu_action_count(std::uint32_t class_id) const {
  return impl_->find_class(class_id).total_menu_actions;
}

std::uint8_t MmapLibrary::card_bucket_kind() const noexcept {
  return impl_->card_bucket_kind;
}

std::uint64_t MmapLibrary::card_abstraction_digest() const noexcept {
  return impl_->card_digest;
}

std::uint64_t MmapLibrary::action_abstraction_digest() const noexcept {
  return impl_->action_digest;
}

const solver::UnifiedGame& MmapLibrary::game() const noexcept {
  return impl_->game;
}

const std::vector<solver::WeightedHand>& MmapLibrary::class_range(std::uint32_t class_id) const {
  return impl_->find_class(class_id).range;
}

const std::string& MmapLibrary::sha256_hex() const noexcept {
  return impl_->sha256_hex;
}

}  // namespace bs::resident
