// RFC 0026 bucket-deduplicated mmap flop class library.
//
// A packed library is a single binary file that replaces a directory of
// schema-v3 SQLite class artifacts with a bucket-keyed acceleration format.
// N-seat training seals strategy rows under (card bucket, tree node); the
// SQLite publish path copies a sealed row onto every concrete holding in the
// bucket. The packed file stores only the distinct (public node, bucket)
// rows, and the reader projects a concrete holding to its trained bucket at
// lookup time. Strategy output is bitwise identical and size is independent
// of the training range width.
//
// Binary layout "BSFLIB02" (all integers little-endian; arrays are naturally
// aligned relative to the file start with explicit padding bytes):
//
//   Header (padded to a 4096 boundary after class_offsets):
//     magic            char[8]  "BSFLIB02"
//     version          u32      2
//     class_count      u32
//     codebook_type    u8       0=Doubles, 8=Codebook8, 16=Codebook16
//     player_count     u8
//     button           u8
//     terminal_depth   u8       0=Flop, 1=River
//     card_bucket_kind u8       abstraction::CardBucketKind ordinal
//     reserved         u8[3]
//     big_blind        u64
//     pot              u64
//     stacks           u64[10]
//     contributions    u64[10]
//     card_abstraction_digest   u64  (FNV-1a 64-bit; suit-canonical identity)
//     action_abstraction_digest u64  (FNV-1a 64-bit)
//     sizes            4 streets: u8 bets_count, u8 raises_count, then
//                      u64 numerator + u64 denominator per fraction
//     class_offsets    u64[class_count]   absolute section file offsets
//
//   Per-class section (4096-aligned):
//     class_id             u32
//     canonical_board      u8[3]
//     padding              u8     (aligns node_count to offset 8)
//     node_count           u32
//     range_count          u16
//     padding              u16
//     total_menu_actions   u32   entries in actions_blob (sum of menu sizes)
//     total_bucket_rows    u32
//     total_prob_entries   u32   sum of menu_size * node_bucket_count
//     codebook_size        u32
//     reserved2            u8[4]
//     padding to 8 (offset 40)
//     range_entries        [u64 combo_index, f64 weight] * range_count
//     padding to 8
//     codebook             f64[codebook_size]
//     node_hash            u64[node_count]             sorted ascending
//     node_actions_offset  u32[node_count]             section-relative bytes
//     node_actions_count   u16[node_count]
//     padding to 4 when node_count is odd
//     node_bucket_start    u32[node_count]             first bucket-row index
//     node_bucket_count    u16[node_count]
//     padding to 8
//     bucket_rows          BucketEntry[total_bucket_rows] (8 bytes each)
//     actions_blob         CompactAction[total_menu_actions] (3 bytes each)
//     padding to prob alignment (1/2/8 by codebook type)
//     probs                one menu-length run per bucket row
//
//   BucketEntry: u16 bucket_id, u16 reserved, u32 prob_offset (section-
//   relative bytes into probs). A node's buckets are a contiguous,
//   bucket-id-sorted slice [node_bucket_start, +node_bucket_count).
//
// A missing bucket at a present node is a miss with the same meaning as the
// exporter skipping a bucket the training range never reached. The SQLite
// artifacts remain the canonical interchange format; the .bin is derived,
// cacheable, and regenerable at any time.
#pragma once

#include <array>
#include <bs/abstraction.hpp>
#include <bs/heads_up_solver.hpp>  // WeightedHand, InformationKey
#include <bs/unified_game.hpp>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace bs::solver {
// Forward declaration; the full definition lives in bs/seat_policy.hpp.
struct SeatPolicyRow;
}  // namespace bs::solver

namespace bs::resident {

struct CompactRowView;

// One class's rows for the pack function. The rows and range are borrowed;
// the pack function reads them but does not retain them after returning.
struct PackClassInput {
  std::uint32_t class_id = 0;
  std::array<int, 3> canonical_board{};
  const std::map<solver::InformationKey, solver::SeatPolicyRow>* rows = nullptr;
  // Per-seat declared range for this class (board-overlapping combos already
  // removed by the trainer). All seats share this range in a flop library.
  const std::vector<solver::WeightedHand>* range = nullptr;
  // Shared abstraction digests (FNV-1a 64-bit, as rendered in the artifact
  // manifest). Optional for synthetic test inputs; real class artifacts
  // supply both, and the packer requires every class of a library to agree.
  std::optional<std::uint64_t> card_abstraction_digest;
  std::optional<std::uint64_t> action_abstraction_digest;
};

// Pack a set of class policies into the consolidated binary format at `path`
// and write the <path>.sha256 sidecar. Delegates to the streaming writer.
void pack_binary_library(const std::filesystem::path& path, const solver::UnifiedGame& game,
                         std::span<const PackClassInput> classes);

// Streaming variant: the supplier is called once per class index in a single
// pass (group, write); only one class's artifact is resident at a time, so
// peak memory is bounded by the largest single class. The probability region
// uses Codebook16 by default; if a class ever exceeds its 65,535 distinct-
// value cap the library is automatically rewritten in a second pass under
// exact Doubles. The supplier must return a PackClassInput whose borrowed
// pointers remain valid until the next call.
using PackClassSupplier = std::function<PackClassInput(std::size_t index)>;

void pack_binary_library_streaming(const std::filesystem::path& path,
                                   const solver::UnifiedGame& game, std::size_t class_count,
                                   const PackClassSupplier& supplier);

// Read-only memory-mapped view of a packed bucket-deduplicated flop class
// library. Immutable and thread-safe after open().
class MmapLibrary {
 public:
  MmapLibrary();
  ~MmapLibrary();
  MmapLibrary(const MmapLibrary&) = delete;
  MmapLibrary& operator=(const MmapLibrary&) = delete;
  MmapLibrary(MmapLibrary&&) noexcept;
  MmapLibrary& operator=(MmapLibrary&&) noexcept;

  // Open the .bin file read-only and parse its header. When verify_sha256 is
  // true the full-file hash is checked against the <path>.sha256 sidecar
  // (reads every byte; off the startup path). Throws on hash mismatch, bad
  // magic/version, a bucket kind that differs from the compiled engine, a
  // truncated file, or any structural inconsistency.
  void open(const std::filesystem::path& path, bool verify_sha256 = true);
  void close();

  // Bucket-keyed row lookup. `public_compact_key` is the compact information
  // key WITHOUT the two own-card bytes
  // ([seat][board_size][board...][5 bytes per public action]); the caller
  // computes `bucket = card_bucket(kNSeatCardKind, hero_cards, board)`.
  // Returns false when the class/node is unknown or the bucket was never
  // trained at that node. On success, `out` references the mmap region and
  // stays valid until close() or destruction; no allocation occurs.
  bool find_bucket(std::uint32_t class_id, std::span<const std::uint8_t> public_compact_key,
                   std::uint16_t bucket, CompactRowView& out) const;

  // Map a canonical flop board to its class id; -1 when absent.
  std::int32_t class_id_for_board(const std::array<int, 3>& canonical_board) const;

  std::size_t class_count() const noexcept;
  std::uint32_t class_id_at(std::size_t index) const;
  const std::array<int, 3>& canonical_board(std::uint32_t class_id) const;
  // Distinct public-node count and bucket-row/menu-action totals for a class.
  std::size_t node_count(std::uint32_t class_id) const;
  std::size_t bucket_row_count(std::uint32_t class_id) const;
  std::size_t menu_action_count(std::uint32_t class_id) const;

  // Header identity fields for caller-side cross-checks against the loaded
  // policy identity (the build site compares these with the artifact
  // manifests before serving).
  std::uint8_t card_bucket_kind() const noexcept;
  std::uint64_t card_abstraction_digest() const noexcept;
  std::uint64_t action_abstraction_digest() const noexcept;

  const solver::UnifiedGame& game() const noexcept;
  const std::vector<solver::WeightedHand>& class_range(std::uint32_t class_id) const;
  const std::string& sha256_hex() const noexcept;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace bs::resident
