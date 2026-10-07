// RFC 0012 Phase 2: consolidated mmap-friendly flop class library format.
//
// A packed library is a single binary file that replaces a directory of
// schema-v3 SQLite class artifacts. The layout is designed for read-only
// memory mapping: a small self-describing header holds the shared game
// identity and the per-class offset table, and each class section is
// 4096-byte aligned so an mmap page fault is class-granular. Rows are found
// by binary search over a sorted 64-bit hash array (no slot table), and the
// CompactRowView pointers reference the mapped region directly, so a warm
// lookup allocates nothing and copies nothing.
//
// The format is a derived acceleration layer: the SQLite artifacts remain
// the canonical interchange and debugging format, and the pack tool never
// deletes them. The file-level SHA-256 sidecar (<path>.sha256) supports
// both offline verification and optional open-time verification; the
// default open path skips the full-file hash so large libraries meet the
// AC2 load-time target.
//
// Binary layout (all multi-byte values little-endian):
//
//   Header (padded to 4096 bytes):
//     magic            char[8]   "BSFLIB01"
//     version          u32       1
//     class_count      u32
//     codebook_type    u8        0=Doubles, 8=Codebook8, 16=Codebook16
//     player_count     u8
//     button           u8
//     terminal_depth   u8        0=Flop, 1=River
//     reserved         u8[5]
//     big_blind        u64
//     pot              u64
//     stacks           u64[10]   (first player_count valid)
//     contributions    u64[10]   (first player_count valid)
//     sizes            4 streets, each: u8 bets_count, u8 raises_count,
//                      then per fraction u64 numerator + u64 denominator
//     class_offsets    u64[class_count]
//
//   Per-class section (4096-byte aligned):
//     class_id         u32
//     canonical_board  u8[3]
//     reserved         u8[1]
//     info_set_count   u32
//     action_count     u32
//     codebook_size    u32       (0 for Doubles)
//     range_count      u16
//     reserved2        u8[2]
//     range_entries    [u64 combo_index, f64 weight] * range_count
//     codebook         f64[codebook_size]
//     hashes           u64[info_set_count]  (sorted ascending)
//     row_offsets      u32[info_set_count]
//     action_counts    u16[info_set_count]
//     actions          CompactAction[action_count]  (3 bytes each, packed)
//     prob_indices     u8[action_count]  (Codebook8)
//                      u16[action_count] (Codebook16)
//                      absent            (Doubles)
//     probabilities    f64[action_count] (Doubles only)
//
// The codebook_type is file-level: the pack tool picks the worst-case layout
// across all classes so every class section uses the same probability
// encoding. The codebook itself is per-class (distinct probability values).
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
#include <span>
#include <string>
#include <vector>

namespace bs::solver {
// Forward declaration; the full definition lives in bs/seat_policy.hpp.
// The public header only needs a pointer-to-map, so a forward declaration
// suffices and avoids pulling in the nseat_trainer dependency chain.
struct SeatPolicyRow;
}  // namespace bs::solver

namespace bs::resident {

// Forward declaration; the full definition lives in the resident-internal
// header src/resident/resident_index.hpp. Callers that access the view's
// fields include that header; this public header only needs the incomplete
// type to declare find() by reference.
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
};

// Pack a set of class policies into the consolidated binary format at `path`
// and write the <path>.sha256 sidecar. `game` supplies the shared root
// identity (player count, blinds, stacks, contributions, pot, button,
// terminal depth, and sizing schedule); each class's per-seat range is taken
// from its SeatPolicy rows. The file-level codebook layout is the worst case
// across all classes. Throws std::runtime_error on any failure.
void pack_binary_library(const std::filesystem::path& path, const solver::UnifiedGame& game,
                         std::span<const PackClassInput> classes);

// Streaming variant for large libraries that cannot fit in memory. The
// supplier is called for each class index in two passes: a lightweight survey
// pass that counts distinct probability bit patterns (to choose the file-level
// codebook layout), and a write pass that builds the full class data and
// appends the section to the file. Only one class's artifact is resident at a
// time, so peak memory is bounded by the largest single class rather than the
// entire library. The supplier must return a PackClassInput whose borrowed
// pointers remain valid until the next call.
using PackClassSupplier = std::function<PackClassInput(std::size_t index)>;

void pack_binary_library_streaming(const std::filesystem::path& path,
                                   const solver::UnifiedGame& game, std::size_t class_count,
                                   const PackClassSupplier& supplier);

// Read-only memory-mapped view of a packed flop class library. Immutable and
// thread-safe after open(): multiple threads may call find() concurrently,
// and multiple processes may mmap the same file and share physical pages.
class MmapLibrary {
 public:
  MmapLibrary();
  ~MmapLibrary();
  MmapLibrary(const MmapLibrary&) = delete;
  MmapLibrary& operator=(const MmapLibrary&) = delete;
  MmapLibrary(MmapLibrary&&) noexcept;
  MmapLibrary& operator=(MmapLibrary&&) noexcept;

  // Open the .bin file, mmap it read-only (PROT_READ, MAP_PRIVATE), and
  // parse the header. When verify_sha256 is true (the default), the full
  // file SHA-256 is computed and compared against the <path>.sha256
  // sidecar before the mapping is used; this reads every byte and is
  // unsuitable for large libraries on the startup path. When false, only
  // the header structural checks (magic, version, class count, offset
  // bounds) run, and the sidecar is left for offline verification.
  // Throws std::runtime_error on checksum mismatch (when verified), a bad
  // magic or version, a truncated file, or any structural inconsistency.
  void open(const std::filesystem::path& path, bool verify_sha256 = true);

  // Unmap and close. Safe to call on a default-constructed instance.
  void close();

  // Find a row by class id and compact key (produced by
  // ResidentIndex::compact_information_key). Fills `out` with pointers into
  // the mmap region that stay valid until close() or destruction. Returns
  // false on a miss (unknown class or absent key).
  bool find(std::uint32_t class_id, std::span<const std::uint8_t> compact_key,
            CompactRowView& out) const;

  // Map a canonical flop board to its class id. Returns -1 when the board's
  // class is not in this library.
  std::int32_t class_id_for_board(const std::array<int, 3>& canonical_board) const;

  std::size_t class_count() const noexcept;
  std::uint32_t class_id_at(std::size_t index) const;
  const std::array<int, 3>& canonical_board(std::uint32_t class_id) const;
  std::size_t info_set_count(std::uint32_t class_id) const;
  std::size_t action_count(std::uint32_t class_id) const;

  // The shared root identity (player count, stacks, pot, sizes, ...). The
  // board fields are zeroed; each class supplies its own canonical board.
  const solver::UnifiedGame& game() const noexcept;

  // The per-seat declared range for a class (board-overlapping combos
  // already removed by the trainer).
  const std::vector<solver::WeightedHand>& class_range(std::uint32_t class_id) const;

  // Hex-encoded SHA-256 from the sidecar. When open() was called with
  // verify_sha256=true this is also the computed file hash; otherwise it
  // is the expected value recorded by the pack tool.
  const std::string& sha256_hex() const noexcept;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace bs::resident
