// RFC 0035 multi-root bucket-deduplicated mmap flop class library.
//
// A packed library is a single binary file that replaces a directory of
// schema-v3 SQLite class artifacts with a bucket-keyed format. RFC 0027: the
// builder writes it directly during training (one section per (root, class)),
// so it is the library artifact rather than a derived cache.
// N-seat training seals strategy rows under (card bucket, tree node); the
// SQLite publish path copies a sealed row onto every concrete holding in the
// bucket. The packed file stores only the distinct (public node, bucket)
// rows, and the reader projects a concrete holding to its trained bucket at
// lookup time. Strategy output is bitwise identical and size is independent
// of the training range width.
//
// Binary layout "BSFLIB03" (all integers little-endian; arrays are naturally
// aligned relative to the file start with explicit padding bytes):
//
//   Fixed header:
//     magic            char[8]  "BSFLIB03"
//     version          u32      3
//     root_count       u32
//     codebook_type    u8       0=Doubles, 8=Codebook8, 16=Codebook16
//     player_count     u8
//     button           u8
//     terminal_depth   u8       0=Flop, 1=River
//     card_bucket_kind u8       abstraction::CardBucketKind ordinal
//     reserved         u8[3]
//     big_blind        u64
//     card_abstraction_digest   u64  (FNV-1a 64-bit; suit-canonical identity)
//     action_abstraction_digest u64  (FNV-1a 64-bit)
//     action_name_len  u32
//     action_version   u32
//     action_parameters_len u32
//     reserved2        u32
//     action_name      char[action_name_len], padded to an 8-byte boundary
//     action_parameters char[action_parameters_len], padded to 8 bytes
//                      (the canonical RFC 0030 parameters, including the
//                      trailing `|max-raises=<N>` cap token and fraction sizes)
//     sizes            4 streets: u8 bets_count, u8 raises_count, then
//                      u64 numerator + u64 denominator per fraction
//
//   Root directory (immediately after the sizes):
//     root[root_count], each 192 bytes:
//       root_id           u32          // 0..root_count-1, ascending
//       line_tag          u8           // 1=limped, 2=SRP, 3=3bet
//       reserved          u8[3]
//       pot               u64
//       stacks            u64[10]
//       contributions     u64[10]
//       folded            u8[10]
//       live_count        u8
//       range_profile     u8           // 1=live-uniform (the only phase-1 value)
//       class_count       u32          // present classes in this root's index row
//     index             u64[root_count][1755], DENSE by GLOBAL class id:
//                       0 = absent, otherwise the section's absolute file
//                       offset. The fixed header, directory, and index are
//                       together padded to a 4096-byte boundary.
//
//   Per-(root,class) section (4096-aligned); uniqueness is (root_id, class_id):
//     root_id              u32        // NEW in BSFLIB03, offset 0
//     class_id             u32
//     canonical_board      u8[3]
//     padding              u8          (aligns node_count to offset 12)
//     node_count           u32
//     range_count          u16
//     padding              u16
//     total_menu_actions   u32
//     total_bucket_rows    u32
//     total_prob_entries   u32
//     codebook_size        u32
//     reserved2            u8[4]
//     padding              u8[4]       // logical header is 44 bytes; the four
//                                      // padding bytes make the effective
//                                      // class header 48 bytes (8-aligned)
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
// The reader mounts the root directory and refuses at open on any structural
// inconsistency: bad/old magic, truncation, duplicate root ledgers (ambiguous),
// an index offset outside the file or off the section alignment, a section
// whose root_id/class_id disagrees with the index path that addressed it,
// nonzero class-header padding, an unknown range profile, or an action
// identity whose digest/token fails RFC 0030 strict parsing. There is no
// BSFLIB02 reader: the old magic is foreign.
//
// A missing bucket at a present node is a miss with the same meaning as the
// exporter skipping a bucket the training range never reached. RFC 0027:
// training serializes each class straight into its section, so the packed
// file is the library artifact and no per-class SQLite copy is required.
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
#include <stdexcept>
#include <string>
#include <vector>

namespace bs::solver {
// Forward declaration; the full definition lives in bs/seat_policy.hpp.
struct SeatPolicyRow;
}  // namespace bs::solver

namespace bs::resident {

struct CompactRowView;

// The pinned global canonical flop class universe (W4a golden digest
// 0xaf83bd016a92d2d6). The dense per-root index has exactly this stride.
inline constexpr std::uint32_t kFlopClassUniverse = 1755;

// Packed 3-byte action: type (0-4, matching poker::ActionType) and target
// total (0-65535, enough for any trained artifact's chip values). RFC 0027
// moved this from the private resident index so the offline serializer and the
// resident reader name one encoding; RFC 0028 reuses it from the sealed
// checkpoint codec for the same reason.
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

// Probability storage layout for a packed library. RFC 0027 moved this from
// the private resident index so the offline serializer and the resident
// reader name one enum.
//   - Codebook8:  <= 256 distinct values (uint8 index per action)
//   - Codebook16: <= 65536 distinct values (uint16 index per action)
//   - Doubles:    more than 65536 distinct values (one exact double per action)
enum class ProbabilityLayout : std::uint8_t { Doubles, Codebook8, Codebook16 };

// RFC 0035: the preflop family a root was reached through. The tag is a
// diagnostic label; exact root matching uses the ledger tuple, never the tag.
enum class RootLineTag : std::uint8_t {
  Limped = 1,
  SingleRaisedPot = 2,
  ThreeBetPot = 3,
};

// RFC 0035: the per-root range selector. Live-uniform gives every seat (folded
// seats included, as the joint-deal removal distribution) the same
// board-filtered full range. The line-conditioned profile (value 2) is
// reserved for a future section revision and is refused on read and write:
// a BSFLIB03 section stores exactly one range vector.
enum class RootRangeProfile : std::uint8_t {
  LiveUniform = 1,
};

const char* to_string(RootLineTag tag) noexcept;

// One root ledger in the BSFLIB03 directory: the exact tuple same_root
// matches (pot, per-seat stacks/net contributions/folded mask) plus its
// diagnostic labels and trained-class count. `class_count` is filled by the
// builder/reader from the dense index row; callers declaring a root table
// leave it zero.
struct RootDirectoryEntry {
  std::uint32_t root_id = 0;
  RootLineTag line_tag = RootLineTag::Limped;
  std::uint64_t pot = 0;
  std::array<std::uint64_t, 10> stacks{};
  std::array<std::uint64_t, 10> contributions{};
  std::array<std::uint8_t, 10> folded{};
  std::uint8_t live_count = 0;
  RootRangeProfile range_profile = RootRangeProfile::LiveUniform;
  std::uint32_t class_count = 0;
};

// Thrown by serialize_class_section when a class has more distinct
// probability values than the requested layout can index. The caller either
// reruns under a wider layout (packer) or fails closed (trainer).
class CodebookCapacityError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

// The two abstraction identities stored in the file header. A class section
// does not carry them. Both present or both absent; a one-sided identity is
// rejected, matching the packer's reconcile rule. RFC 0035 also pins the full
// action identity (name/version/parameters) in the header, but the digests
// stay the field the assembler reconcile compares across sections.
struct LibraryIdentity {
  std::optional<std::uint64_t> card_abstraction_digest;
  std::optional<std::uint64_t> action_abstraction_digest;
};

// One class's rows for the pack function. The rows and range are borrowed;
// the pack function reads them but does not retain them after returning.
struct PackClassInput {
  std::uint32_t root_id = 0;  // RFC 0035: directory root this section belongs to
  std::uint32_t class_id = 0;
  std::array<int, 3> canonical_board{};
  const std::map<solver::InformationKey, solver::SeatPolicyRow>* rows = nullptr;
  // Per-seat declared range for this class (board-overlapping combos already
  // removed by the trainer). Every seat shares this range in a BSFLIB03 root.
  // The serializer canonicalizes it to ascending comboIndex order; callers may
  // pass any order. A duplicate combo is an error.
  const std::vector<solver::WeightedHand>* range = nullptr;
  // Shared abstraction digests (FNV-1a 64-bit, as rendered in the artifact
  // manifest). Optional for synthetic test inputs; real class artifacts
  // supply both, and the packer requires every class of a library to agree.
  std::optional<std::uint64_t> card_abstraction_digest;
  std::optional<std::uint64_t> action_abstraction_digest;
};

// RFC 0027: serialize one class into its BSFLIB03 section bytes (unpadded,
// section-relative offsets). Runs the RFC 0026 losslessness assertions: one
// action menu per public node, and bitwise-identical probability runs for
// every concrete row sealing the same (node, bucket). Throws
// CodebookCapacityError when the class cannot be indexed under `layout`.
std::vector<std::uint8_t> serialize_class_section(const PackClassInput& input,
                                                  ProbabilityLayout layout);

// One pre-serialized section for the in-memory assembler.
struct RootClassSection {
  std::uint32_t root_id = 0;
  std::uint32_t class_id = 0;
  std::span<const std::uint8_t> bytes;
};

// RFC 0035: on-disk per-(root,class) section file name:
// "root-NNNN-class-NNNN.bsf", shared by the builder, the assembler, and every
// resume scan so the stride identity cannot drift.
std::string root_class_section_name(std::uint32_t root_id, std::uint32_t class_id);

// Write a complete BSFLIB03 file and its .sha256 sidecar from per-(root,class)
// section files. Streams to `path + ".tmp"`, fsyncs, and atomically renames.
// `section_dir` holds one root_class_section_name(root, class) file per
// trained pair; class_ids_by_root[root] is the ascending class id list for
// that root and must agree with roots[root].class_count. Every section is
// validated before the first output byte is written.
void assemble_binary_library(const std::filesystem::path& path, const solver::UnifiedGame& game,
                             const LibraryIdentity& identity, ProbabilityLayout layout,
                             std::span<const RootDirectoryEntry> roots,
                             const std::filesystem::path& section_dir,
                             std::span<const std::vector<std::uint32_t>> class_ids_by_root);

// In-memory convenience adapter for tests: stages the sections in a private
// `.bs-assemble-*` directory under `path.parent_path()`, calls the
// file-backed assembler, and removes the directory on success or failure.
void assemble_binary_library(const std::filesystem::path& path, const solver::UnifiedGame& game,
                             const LibraryIdentity& identity, ProbabilityLayout layout,
                             std::span<const RootDirectoryEntry> roots,
                             std::span<const RootClassSection> sections);

// Pack a set of class policies into the consolidated binary format at `path`
// and write the <path>.sha256 sidecar. Delegates to the streaming writer.
void pack_binary_library(const std::filesystem::path& path, const solver::UnifiedGame& game,
                         std::span<const RootDirectoryEntry> roots,
                         std::span<const PackClassInput> classes);

// Streaming variant: the supplier is called once per class index in a single
// pass (serialize, stage, assemble); only one class's rows are resident at a
// time, so peak memory is bounded by the largest single class. Each
// PackClassInput names its root_id. The probability region uses Codebook16 by
// default; if a class ever exceeds its 65,535 distinct-value cap the whole
// pass is rerun under exact Doubles. The supplier must return a
// PackClassInput whose borrowed pointers remain valid until the next call.
using PackClassSupplier = std::function<PackClassInput(std::size_t index)>;

void pack_binary_library_streaming(const std::filesystem::path& path,
                                   const solver::UnifiedGame& game,
                                   std::span<const RootDirectoryEntry> roots,
                                   std::size_t class_count, const PackClassSupplier& supplier);

// Read-only memory-mapped view of a packed multi-root flop class library.
// Immutable and thread-safe after open().
class MmapLibrary {
 public:
  MmapLibrary();
  ~MmapLibrary();
  MmapLibrary(const MmapLibrary&) = delete;
  MmapLibrary& operator=(const MmapLibrary&) = delete;
  MmapLibrary(MmapLibrary&&) noexcept;
  MmapLibrary& operator=(MmapLibrary&&) noexcept;

  // Open the .bin file read-only and parse its header, root directory, and
  // every section header. When verify_sha256 is true the full-file hash is
  // checked against the <path>.sha256 sidecar (reads every byte; off the
  // startup path). Throws on hash mismatch, bad/old magic/version, a bucket
  // kind that differs from the compiled engine, an action identity that fails
  // RFC 0030 strict validation, a truncated file, an ambiguous duplicate root
  // ledger, or any structural inconsistency.
  void open(const std::filesystem::path& path, bool verify_sha256 = true);
  void close();

  // --- RFC 0035 root directory ---------------------------------------------
  std::size_t root_count() const noexcept;
  const RootDirectoryEntry& root(std::uint32_t root_id) const;
  // The sorted global class ids trained for this root (the nonzero entries of
  // its dense index row, ascending).
  std::span<const std::uint32_t> root_class_ids(std::uint32_t root_id) const;
  bool has_class(std::uint32_t root_id, std::uint32_t class_id) const;
  // Map a canonical flop board to its class id within one root; -1 absent.
  std::int32_t class_id_for_root_board(std::uint32_t root_id,
                                       const std::array<int, 3>& canonical_board) const;

  // Bucket-keyed row lookup scoped to one (root, class). Returns false when
  // the pair is absent, the node is unknown, or the bucket was never trained.
  // On success, `out` references the mmap region and stays valid until close()
  // or destruction; no allocation occurs.
  bool find_bucket(std::uint32_t root_id, std::uint32_t class_id,
                   std::span<const std::uint8_t> public_compact_key, std::uint16_t bucket,
                   CompactRowView& out) const;

  // Per-(root,class) distinct public-node count and bucket-row/menu totals.
  std::size_t node_count(std::uint32_t root_id, std::uint32_t class_id) const;
  std::size_t bucket_row_count(std::uint32_t root_id, std::uint32_t class_id) const;
  std::size_t menu_action_count(std::uint32_t root_id, std::uint32_t class_id) const;
  const std::array<int, 3>& canonical_board(std::uint32_t root_id, std::uint32_t class_id) const;

  // RFC 0031 Stage 0: read-only per-node enumeration for offline segmentation
  // statistics, addressed by (root, class). Node indices run [0, node_count)
  // in the sorted-hash order find_bucket binary-searches. Throws
  // std::out_of_range on an absent pair or an out-of-range node/row index.
  std::uint64_t node_hash_at(std::uint32_t root_id, std::uint32_t class_id,
                             std::size_t node_index) const;
  std::size_t node_bucket_count_at(std::uint32_t root_id, std::uint32_t class_id,
                                   std::size_t node_index) const;
  std::uint16_t node_bucket_id_at(std::uint32_t root_id, std::uint32_t class_id,
                                  std::size_t node_index, std::size_t row_index) const;
  void node_bucket_row_at(std::uint32_t root_id, std::uint32_t class_id, std::size_t node_index,
                          std::size_t row_index, CompactRowView& out) const;

  // Single-root tooling convenience: these address root 0, which is every
  // one-root artifact. They throw std::invalid_argument on a multi-root file,
  // where class ids are unique only per (root, class) and root-scoped access
  // is required. Offline tools enumerate roots explicitly.
  std::size_t class_count() const;
  std::uint32_t class_id_at(std::size_t index) const;
  const std::array<int, 3>& canonical_board(std::uint32_t class_id) const;
  std::size_t node_count(std::uint32_t class_id) const;
  std::size_t bucket_row_count(std::uint32_t class_id) const;
  std::size_t menu_action_count(std::uint32_t class_id) const;
  std::uint64_t node_hash_at(std::uint32_t class_id, std::size_t node_index) const;
  std::size_t node_bucket_count_at(std::uint32_t class_id, std::size_t node_index) const;
  std::uint16_t node_bucket_id_at(std::uint32_t class_id, std::size_t node_index,
                                  std::size_t row_index) const;
  void node_bucket_row_at(std::uint32_t class_id, std::size_t node_index, std::size_t row_index,
                          CompactRowView& out) const;

  // Header identity fields for caller-side cross-checks.
  std::uint8_t card_bucket_kind() const noexcept;
  std::uint64_t card_abstraction_digest() const noexcept;
  std::uint64_t action_abstraction_digest() const noexcept;
  const abstraction::AbstractionId& action_identity() const noexcept;

  const solver::UnifiedGame& game() const noexcept;
  const std::vector<solver::WeightedHand>& class_range(std::uint32_t root_id,
                                                       std::uint32_t class_id) const;
  const std::vector<solver::WeightedHand>& class_range(std::uint32_t class_id) const;
  const std::string& sha256_hex() const noexcept;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;

  // Root-0 convenience accessors throw on a multi-root file through this.
  void require_single_root() const;
};

}  // namespace bs::resident
