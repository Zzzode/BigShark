// RFC 0012 Phase 2: consolidated mmap-friendly flop class library format.
//
// This file implements the pack function (offline tool path) and the
// MmapLibrary class (read-only mmap view). The binary layout is documented
// in engine/include/bs/mmap_library.hpp.
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <bs/abstraction.hpp>
#include <bs/mmap_library.hpp>
#include <bs/range.hpp>        // comboIndex
#include <bs/seat_policy.hpp>  // SeatPolicyRow (full definition for packing)
#include <bs/strategy_artifact.hpp>
#include <bs/unified_game.hpp>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <map>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include "resident_index.hpp"  // CompactRowView, CompactAction, ProbabilityLayout, ResidentIndex

namespace bs::resident {
namespace {

constexpr char kMagic[8] = {'B', 'S', 'F', 'L', 'I', 'B', '0', '1'};
constexpr std::uint32_t kVersion = 1;
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

// Per-class intermediate data built during the first pass of pack.
struct ClassBuildData {
  std::uint32_t class_id = 0;
  std::array<int, 3> canonical_board{};
  struct RowEntry {
    std::uint64_t hash = 0;
    std::array<std::uint8_t, 512> compact_key{};
    std::size_t compact_size = 0;
    const solver::SeatPolicyRow* row = nullptr;
  };
  std::vector<RowEntry> rows;
  // Distinct probability bit patterns → codebook index (sorted by bits).
  std::map<std::uint64_t, std::uint16_t> codebook_index;
  ProbabilityLayout layout = ProbabilityLayout::Doubles;
  const std::vector<solver::WeightedHand>* range = nullptr;
};

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

  // Phase 1: compute per-class data (compact keys, hashes, codebooks).
  std::vector<ClassBuildData> build_data;
  build_data.reserve(classes.size());

  for (const PackClassInput& input : classes) {
    if (!input.rows)
      throw std::invalid_argument("pack_binary_library: class " + std::to_string(input.class_id) +
                                  " has null rows");
    if (input.rows->empty())
      throw std::invalid_argument("pack_binary_library: class " + std::to_string(input.class_id) +
                                  " has no rows");

    ClassBuildData data;
    data.class_id = input.class_id;
    data.canonical_board = input.canonical_board;
    data.range = input.range;

    for (const auto& [key, row] : *input.rows) {
      if (row.actions.size() != row.probabilities.size() ||
          row.actions.size() > kMaxResidentActions)
        throw std::invalid_argument("pack_binary_library: malformed row in class " +
                                    std::to_string(input.class_id));

      ClassBuildData::RowEntry entry;
      entry.row = &row;
      entry.compact_size = ResidentIndex::compact_information_key(
          std::span<const std::uint64_t>(key.data(), key.size()),
          std::span<std::uint8_t>(entry.compact_key.data(), entry.compact_key.size()));
      if (entry.compact_size == 0)
        throw std::invalid_argument("pack_binary_library: key cannot be compacted in class " +
                                    std::to_string(input.class_id));
      entry.hash =
          hash_key_64(std::span<const std::uint8_t>(entry.compact_key.data(), entry.compact_size));
      data.rows.push_back(std::move(entry));

      for (double probability : row.probabilities) {
        std::uint64_t bits;
        std::memcpy(&bits, &probability, sizeof(bits));
        data.codebook_index.emplace(bits, 0);
      }
    }

    // Sort rows by hash for binary search.
    std::sort(data.rows.begin(), data.rows.end(),
              [](const ClassBuildData::RowEntry& a, const ClassBuildData::RowEntry& b) {
                return a.hash < b.hash;
              });

    // Choose the class's probability layout.
    data.layout = data.codebook_index.size() <= 256     ? ProbabilityLayout::Codebook8
                  : data.codebook_index.size() <= 65536 ? ProbabilityLayout::Codebook16
                                                        : ProbabilityLayout::Doubles;

    build_data.push_back(std::move(data));
  }

  // Phase 2: choose the file-level layout (worst case across all classes).
  ProbabilityLayout file_layout = ProbabilityLayout::Codebook8;
  for (const ClassBuildData& data : build_data) {
    if (data.layout == ProbabilityLayout::Doubles) {
      file_layout = ProbabilityLayout::Doubles;
      break;
    }
    if (data.layout == ProbabilityLayout::Codebook16)
      file_layout = ProbabilityLayout::Codebook16;
  }

  // Assign codebook indices (sorted bit-pattern order).
  for (ClassBuildData& data : build_data) {
    if (data.layout != ProbabilityLayout::Doubles) {
      std::uint16_t index = 0;
      for (auto& [bits, slot] : data.codebook_index)
        slot = index++;
    }
  }

  // Phase 3: build the file image.
  std::vector<std::uint8_t> buf;

  // Compute the sizes section size.
  std::size_t sizes_bytes = 0;
  for (const auto& street : game.sizes) {
    sizes_bytes += 2;  // bets_count + raises_count
    sizes_bytes += street.bets.size() * 16;
    sizes_bytes += street.raises.size() * 16;
  }

  // Header layout:
  //   0:  magic char[8]
  //   8:  version u32
  //   12: class_count u32
  //   16: codebook_type u8
  //   17: player_count u8
  //   18: button u8
  //   19: terminal_depth u8
  //   20: reserved u8[4]
  //   24: big_blind u64
  //   32: pot u64
  //   40: stacks u64[10]  (80 bytes)
  //   120: contributions u64[10]  (80 bytes)
  //   200: sizes (variable)
  //   after sizes: class_offsets u64[class_count]
  const std::size_t header_fixed = 200 + sizes_bytes;
  const std::size_t class_offsets_pos = header_fixed;
  const std::size_t header_total =
      align_up(header_fixed + build_data.size() * sizeof(std::uint64_t), kPageSize);

  buf.reserve(header_total + build_data.size() * kPageSize);

  // Write the header (class_offsets are patched later).
  buf.insert(buf.end(), kMagic, kMagic + 8);
  write_u32(buf, kVersion);
  write_u32(buf, static_cast<std::uint32_t>(build_data.size()));
  write_u8(buf, codebook_type_to_u8(file_layout));
  write_u8(buf, static_cast<std::uint8_t>(game.def.player_count));
  write_u8(buf, static_cast<std::uint8_t>(game.def.button));
  write_u8(buf, game.def.terminal == poker::TerminalDepth::Flop ? 0 : 1);
  // reserved u8[4]
  for (int i = 0; i < 4; ++i)
    write_u8(buf, 0);
  write_u64(buf, game.def.big_blind);
  write_u64(buf, game.def.pot);
  for (std::size_t i = 0; i < kMaxSeats; ++i)
    write_u64(buf, game.def.stacks[i]);
  for (std::size_t i = 0; i < kMaxSeats; ++i)
    write_u64(buf, game.def.contributions[i]);
  // Sizes.
  for (const auto& street : game.sizes) {
    write_u8(buf, static_cast<std::uint8_t>(street.bets.size()));
    write_u8(buf, static_cast<std::uint8_t>(street.raises.size()));
    for (const auto& frac : street.bets)
      write_fraction(buf, frac);
    for (const auto& frac : street.raises)
      write_fraction(buf, frac);
  }
  // Placeholder class_offsets (patched after sections are laid out).
  const std::size_t class_offsets_start = buf.size();
  for (std::size_t i = 0; i < build_data.size(); ++i)
    write_u64(buf, 0);
  // Pad to the first page boundary.
  buf.resize(header_total, 0);

  // Write each class section.
  std::vector<std::uint64_t> section_offsets;
  section_offsets.reserve(build_data.size());

  for (const ClassBuildData& data : build_data) {
    const std::size_t section_start = buf.size();
    section_offsets.push_back(section_start);

    const std::uint32_t info_set_count = static_cast<std::uint32_t>(data.rows.size());
    std::uint32_t action_count = 0;
    for (const auto& entry : data.rows)
      action_count += static_cast<std::uint32_t>(entry.row->actions.size());

    const std::uint32_t codebook_size =
        (file_layout == ProbabilityLayout::Doubles)
            ? 0u
            : static_cast<std::uint32_t>(data.codebook_index.size());
    const std::uint16_t range_count =
        data.range ? static_cast<std::uint16_t>(data.range->size()) : 0;

    // Fixed section header (24 bytes).
    write_u32(buf, data.class_id);
    write_u8(buf, static_cast<std::uint8_t>(data.canonical_board[0]));
    write_u8(buf, static_cast<std::uint8_t>(data.canonical_board[1]));
    write_u8(buf, static_cast<std::uint8_t>(data.canonical_board[2]));
    write_u8(buf, 0);  // reserved
    write_u32(buf, info_set_count);
    write_u32(buf, action_count);
    write_u32(buf, codebook_size);
    write_u16(buf, range_count);
    write_u8(buf, 0);  // reserved2
    write_u8(buf, 0);

    // Range entries: [u64 combo_index, f64 weight] * range_count.
    if (data.range) {
      for (const solver::WeightedHand& hand : *data.range) {
        write_u64(buf, static_cast<std::uint64_t>(bs::comboIndex(hand.cards[0], hand.cards[1])));
        write_f64(buf, hand.weight);
      }
    }

    // Codebook: f64[codebook_size] (sorted bit-pattern order).
    if (codebook_size > 0) {
      for (const auto& [bits, slot] : data.codebook_index) {
        double probability;
        std::memcpy(&probability, &bits, sizeof(probability));
        write_f64(buf, probability);
      }
    }

    // Hashes: u64[info_set_count] (sorted ascending).
    for (const auto& entry : data.rows)
      write_u64(buf, entry.hash);

    // Row offsets: u32[info_set_count] (prefix sum of action counts).
    {
      std::uint32_t offset = 0;
      for (const auto& entry : data.rows) {
        write_u32(buf, offset);
        offset += static_cast<std::uint32_t>(entry.row->actions.size());
      }
    }

    // Action counts: u16[info_set_count].
    for (const auto& entry : data.rows)
      write_u16(buf, static_cast<std::uint16_t>(entry.row->actions.size()));

    // Actions: CompactAction[action_count] (3 bytes each, packed).
    for (const auto& entry : data.rows) {
      for (const poker::Action& action : entry.row->actions) {
        if (action.target_total > 0xffff)
          throw std::invalid_argument("pack_binary_library: action target exceeds uint16 range");
        write_u8(buf, static_cast<std::uint8_t>(action.type));
        write_u16(buf, static_cast<std::uint16_t>(action.target_total));
      }
    }

    // Probability storage.
    if (file_layout == ProbabilityLayout::Codebook8) {
      for (const auto& entry : data.rows)
        for (double probability : entry.row->probabilities) {
          std::uint64_t bits;
          std::memcpy(&bits, &probability, sizeof(bits));
          write_u8(buf, data.codebook_index.at(bits));
        }
    } else if (file_layout == ProbabilityLayout::Codebook16) {
      // Align to 2 bytes for u16 access.
      if (buf.size() % 2 != 0)
        write_u8(buf, 0);
      for (const auto& entry : data.rows)
        for (double probability : entry.row->probabilities) {
          std::uint64_t bits;
          std::memcpy(&bits, &probability, sizeof(bits));
          write_u16(buf, data.codebook_index.at(bits));
        }
    } else {
      // Doubles: align to 8 bytes for f64 access.
      while (buf.size() % 8 != 0)
        write_u8(buf, 0);
      for (const auto& entry : data.rows)
        for (double probability : entry.row->probabilities)
          write_f64(buf, probability);
    }

    // Pad to the next page boundary.
    buf.resize(align_up(buf.size(), kPageSize), 0);
  }

  // Patch class_offsets in the header.
  for (std::size_t i = 0; i < section_offsets.size(); ++i) {
    const std::size_t pos = class_offsets_pos + i * sizeof(std::uint64_t);
    const std::uint64_t offset = section_offsets[i];
    for (int b = 0; b < 8; ++b)
      buf[pos + b] = static_cast<std::uint8_t>((offset >> (b * 8)) & 0xffu);
  }

  // Write the file.
  {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out)
      throw std::runtime_error("pack_binary_library: cannot open " + path.string() + " for write");
    out.write(reinterpret_cast<const char*>(buf.data()), static_cast<std::streamsize>(buf.size()));
    if (!out)
      throw std::runtime_error("pack_binary_library: write failed for " + path.string());
  }

  // Compute and write the SHA-256 sidecar.
  const std::string hex = artifacts::sha256_file_hex(path);
  const std::filesystem::path sidecar = path.string() + ".sha256";
  {
    std::ofstream out(sidecar, std::ios::binary | std::ios::trunc);
    if (!out)
      throw std::runtime_error("pack_binary_library: cannot open " + sidecar.string() +
                               " for write");
    out << hex << "  " << path.filename().string() << "\n";
    if (!out)
      throw std::runtime_error("pack_binary_library: write failed for " + sidecar.string());
  }
}

// ---------------------------------------------------------------------------
// Streaming pack (RFC 0012 Phase 2 follow-up: OOM fix for large libraries)
// ---------------------------------------------------------------------------

namespace {

// Survey pass: count distinct probability bit patterns for one class. This is
// much lighter than build_class_data — no compact keys, hashes, or sorting.
std::size_t survey_codebook_size(const PackClassInput& input) {
  if (!input.rows || input.rows->empty())
    throw std::invalid_argument("pack_binary_library_streaming: class " +
                                std::to_string(input.class_id) + " has no rows");
  std::map<std::uint64_t, std::uint16_t> bits;
  for (const auto& [key, row] : *input.rows) {
    if (row.actions.size() != row.probabilities.size() || row.actions.size() > kMaxResidentActions)
      throw std::invalid_argument("pack_binary_library_streaming: malformed row in class " +
                                  std::to_string(input.class_id));
    for (double probability : row.probabilities) {
      std::uint64_t bit_pattern;
      std::memcpy(&bit_pattern, &probability, sizeof(bit_pattern));
      bits.emplace(bit_pattern, 0);
    }
  }
  return bits.size();
}

// Full build for one class: compact keys, hashes, sorted rows, codebook.
ClassBuildData build_class_data(const PackClassInput& input) {
  ClassBuildData data;
  data.class_id = input.class_id;
  data.canonical_board = input.canonical_board;
  data.range = input.range;

  for (const auto& [key, row] : *input.rows) {
    ClassBuildData::RowEntry entry;
    entry.row = &row;
    entry.compact_size = ResidentIndex::compact_information_key(
        std::span<const std::uint64_t>(key.data(), key.size()),
        std::span<std::uint8_t>(entry.compact_key.data(), entry.compact_key.size()));
    if (entry.compact_size == 0)
      throw std::invalid_argument("pack_binary_library_streaming: key cannot be compacted in " +
                                  std::to_string(input.class_id));
    entry.hash =
        hash_key_64(std::span<const std::uint8_t>(entry.compact_key.data(), entry.compact_size));
    data.rows.push_back(std::move(entry));

    for (double probability : row.probabilities) {
      std::uint64_t bits;
      std::memcpy(&bits, &probability, sizeof(bits));
      data.codebook_index.emplace(bits, 0);
    }
  }

  std::sort(data.rows.begin(), data.rows.end(),
            [](const ClassBuildData::RowEntry& a, const ClassBuildData::RowEntry& b) {
              return a.hash < b.hash;
            });

  data.layout = data.codebook_index.size() <= 256     ? ProbabilityLayout::Codebook8
                : data.codebook_index.size() <= 65536 ? ProbabilityLayout::Codebook16
                                                      : ProbabilityLayout::Doubles;
  if (data.layout != ProbabilityLayout::Doubles) {
    std::uint16_t index = 0;
    for (auto& [bits, slot] : data.codebook_index)
      slot = index++;
  }
  return data;
}

// Write one class section into buf (does not pad — caller pads).
void write_class_section(std::vector<std::uint8_t>& buf, const ClassBuildData& data,
                         ProbabilityLayout file_layout) {
  const std::uint32_t info_set_count = static_cast<std::uint32_t>(data.rows.size());
  std::uint32_t action_count = 0;
  for (const auto& entry : data.rows)
    action_count += static_cast<std::uint32_t>(entry.row->actions.size());

  const std::uint32_t codebook_size = (file_layout == ProbabilityLayout::Doubles)
                                          ? 0u
                                          : static_cast<std::uint32_t>(data.codebook_index.size());
  const std::uint16_t range_count = data.range ? static_cast<std::uint16_t>(data.range->size()) : 0;

  write_u32(buf, data.class_id);
  write_u8(buf, static_cast<std::uint8_t>(data.canonical_board[0]));
  write_u8(buf, static_cast<std::uint8_t>(data.canonical_board[1]));
  write_u8(buf, static_cast<std::uint8_t>(data.canonical_board[2]));
  write_u8(buf, 0);  // reserved
  write_u32(buf, info_set_count);
  write_u32(buf, action_count);
  write_u32(buf, codebook_size);
  write_u16(buf, range_count);
  write_u8(buf, 0);  // reserved2
  write_u8(buf, 0);

  if (data.range) {
    for (const solver::WeightedHand& hand : *data.range) {
      write_u64(buf, static_cast<std::uint64_t>(bs::comboIndex(hand.cards[0], hand.cards[1])));
      write_f64(buf, hand.weight);
    }
  }

  if (codebook_size > 0) {
    for (const auto& [bits, slot] : data.codebook_index) {
      double probability;
      std::memcpy(&probability, &bits, sizeof(probability));
      write_f64(buf, probability);
    }
  }

  for (const auto& entry : data.rows)
    write_u64(buf, entry.hash);

  {
    std::uint32_t offset = 0;
    for (const auto& entry : data.rows) {
      write_u32(buf, offset);
      offset += static_cast<std::uint32_t>(entry.row->actions.size());
    }
  }

  for (const auto& entry : data.rows)
    write_u16(buf, static_cast<std::uint16_t>(entry.row->actions.size()));

  for (const auto& entry : data.rows) {
    for (const poker::Action& action : entry.row->actions) {
      if (action.target_total > 0xffff)
        throw std::invalid_argument(
            "pack_binary_library_streaming: action target exceeds uint16 range");
      write_u8(buf, static_cast<std::uint8_t>(action.type));
      write_u16(buf, static_cast<std::uint16_t>(action.target_total));
    }
  }

  if (file_layout == ProbabilityLayout::Codebook8) {
    for (const auto& entry : data.rows)
      for (double probability : entry.row->probabilities) {
        std::uint64_t bits;
        std::memcpy(&bits, &probability, sizeof(bits));
        write_u8(buf, data.codebook_index.at(bits));
      }
  } else if (file_layout == ProbabilityLayout::Codebook16) {
    if (buf.size() % 2 != 0)
      write_u8(buf, 0);
    for (const auto& entry : data.rows)
      for (double probability : entry.row->probabilities) {
        std::uint64_t bits;
        std::memcpy(&bits, &probability, sizeof(bits));
        write_u16(buf, data.codebook_index.at(bits));
      }
  } else {
    while (buf.size() % 8 != 0)
      write_u8(buf, 0);
    for (const auto& entry : data.rows)
      for (double probability : entry.row->probabilities)
        write_f64(buf, probability);
  }
}

}  // namespace

void pack_binary_library_streaming(const std::filesystem::path& path,
                                   const solver::UnifiedGame& game, std::size_t class_count,
                                   const PackClassSupplier& supplier) {
  if (class_count == 0)
    throw std::invalid_argument("pack_binary_library_streaming: no classes to pack");
  if (game.def.player_count < 2 || game.def.player_count > kMaxSeats)
    throw std::invalid_argument("pack_binary_library_streaming: player_count out of range");

  // Pass 1: survey codebook sizes to choose the file-level layout.
  std::vector<std::size_t> codebook_sizes(class_count);
  for (std::size_t i = 0; i < class_count; ++i)
    codebook_sizes[i] = survey_codebook_size(supplier(i));

  ProbabilityLayout file_layout = ProbabilityLayout::Codebook8;
  for (std::size_t size : codebook_sizes) {
    if (size > 65536) {
      file_layout = ProbabilityLayout::Doubles;
      break;
    }
    if (size > 256)
      file_layout = ProbabilityLayout::Codebook16;
  }

  // Compute header layout.
  std::size_t sizes_bytes = 0;
  for (const auto& street : game.sizes) {
    sizes_bytes += 2;
    sizes_bytes += street.bets.size() * 16;
    sizes_bytes += street.raises.size() * 16;
  }
  const std::size_t header_fixed = 200 + sizes_bytes;
  const std::size_t class_offsets_pos = header_fixed;
  const std::size_t header_total =
      align_up(header_fixed + class_count * sizeof(std::uint64_t), kPageSize);

  // Open the file and write the placeholder header.
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out)
    throw std::runtime_error("pack_binary_library_streaming: cannot open " + path.string() +
                             " for write");

  {
    std::vector<std::uint8_t> header;
    header.reserve(header_total);
    header.insert(header.end(), kMagic, kMagic + 8);
    write_u32(header, kVersion);
    write_u32(header, static_cast<std::uint32_t>(class_count));
    write_u8(header, codebook_type_to_u8(file_layout));
    write_u8(header, static_cast<std::uint8_t>(game.def.player_count));
    write_u8(header, static_cast<std::uint8_t>(game.def.button));
    write_u8(header, game.def.terminal == poker::TerminalDepth::Flop ? 0 : 1);
    for (int i = 0; i < 4; ++i)
      write_u8(header, 0);
    write_u64(header, game.def.big_blind);
    write_u64(header, game.def.pot);
    for (std::size_t i = 0; i < kMaxSeats; ++i)
      write_u64(header, game.def.stacks[i]);
    for (std::size_t i = 0; i < kMaxSeats; ++i)
      write_u64(header, game.def.contributions[i]);
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
    out.write(reinterpret_cast<const char*>(header.data()),
              static_cast<std::streamsize>(header.size()));
    if (!out)
      throw std::runtime_error("pack_binary_library_streaming: header write failed for " +
                               path.string());
  }

  // Pass 2: build and write each class section.
  std::vector<std::uint64_t> section_offsets(class_count);
  for (std::size_t i = 0; i < class_count; ++i) {
    ClassBuildData data = build_class_data(supplier(i));
    section_offsets[i] = static_cast<std::uint64_t>(out.tellp());

    std::vector<std::uint8_t> section;
    write_class_section(section, data, file_layout);
    out.write(reinterpret_cast<const char*>(section.data()),
              static_cast<std::streamsize>(section.size()));
    if (!out)
      throw std::runtime_error("pack_binary_library_streaming: section write failed for " +
                               path.string());

    // Pad to the next page boundary.
    const std::size_t current = static_cast<std::size_t>(out.tellp());
    const std::size_t aligned = align_up(current, kPageSize);
    if (aligned > current) {
      const std::vector<std::uint8_t> padding(aligned - current, 0);
      out.write(reinterpret_cast<const char*>(padding.data()),
                static_cast<std::streamsize>(padding.size()));
      if (!out)
        throw std::runtime_error("pack_binary_library_streaming: padding write failed for " +
                                 path.string());
    }
  }

  // Patch class_offsets in the header.
  out.seekp(static_cast<std::streamoff>(class_offsets_pos));
  for (std::uint64_t offset : section_offsets) {
    std::uint8_t bytes[8];
    for (int b = 0; b < 8; ++b)
      bytes[b] = static_cast<std::uint8_t>((offset >> (b * 8)) & 0xffu);
    out.write(reinterpret_cast<const char*>(bytes), 8);
  }
  if (!out)
    throw std::runtime_error("pack_binary_library_streaming: header patch failed for " +
                             path.string());
  out.close();

  // Compute and write the SHA-256 sidecar.
  const std::string hex = artifacts::sha256_file_hex(path);
  const std::filesystem::path sidecar = path.string() + ".sha256";
  {
    std::ofstream sidecar_out(sidecar, std::ios::binary | std::ios::trunc);
    if (!sidecar_out)
      throw std::runtime_error("pack_binary_library_streaming: cannot open " + sidecar.string() +
                               " for write");
    sidecar_out << hex << "  " << path.filename().string() << "\n";
    if (!sidecar_out)
      throw std::runtime_error("pack_binary_library_streaming: write failed for " +
                               sidecar.string());
  }
}

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
  solver::UnifiedGame game;
  std::string sha256_hex;

  // Parsed class sections.
  struct ClassInfo {
    std::uint32_t class_id = 0;
    std::array<int, 3> canonical_board{};
    std::uint32_t info_set_count = 0;
    std::uint32_t action_count = 0;
    // Pointers into the mmap region.
    const std::uint64_t* hashes = nullptr;
    const std::uint32_t* row_offsets = nullptr;
    const std::uint16_t* action_counts = nullptr;
    const CompactAction* actions = nullptr;
    const std::uint8_t* prob_indices_8 = nullptr;
    const std::uint16_t* prob_indices_16 = nullptr;
    const double* probabilities = nullptr;
    const double* codebook = nullptr;
    // Copied from mmap (small: 40 combos × 16 bytes).
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

  // Parse sizes.
  {
    const std::uint8_t* p = base + 200;
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
  // offset 200; its total byte length is recomputed by walking the four
  // street entries (2-byte header + 16 bytes per fraction).
  std::size_t sizes_bytes = 0;
  {
    const std::uint8_t* p = base + 200;
    for (std::size_t street = 0; street < 4; ++street) {
      const std::uint8_t bets_count = read_u8(p);
      const std::uint8_t raises_count = read_u8(p + 1);
      sizes_bytes += 2 + static_cast<std::size_t>(bets_count) * 16 +
                     static_cast<std::size_t>(raises_count) * 16;
      p += 2 + static_cast<std::size_t>(bets_count) * 16 +
           static_cast<std::size_t>(raises_count) * 16;
    }
  }

  const std::size_t class_offsets_pos = 200 + sizes_bytes;
  const std::uint8_t* offsets_ptr = base + class_offsets_pos;

  impl_->classes.reserve(impl_->class_count);
  for (std::uint32_t i = 0; i < impl_->class_count; ++i) {
    const std::uint64_t section_offset = read_u64(offsets_ptr + i * 8);
    if (section_offset >= impl_->size)
      throw std::runtime_error("mmap library: class section offset out of bounds");
    const std::uint8_t* sec = base + section_offset;

    Impl::ClassInfo info;
    info.class_id = read_u32(sec);
    info.canonical_board = {static_cast<int>(read_u8(sec + 4)), static_cast<int>(read_u8(sec + 5)),
                            static_cast<int>(read_u8(sec + 6))};
    info.info_set_count = read_u32(sec + 8);
    info.action_count = read_u32(sec + 12);
    const std::uint32_t codebook_size = read_u32(sec + 16);
    const std::uint16_t range_count = read_u16(sec + 20);

    // Range entries start at offset 24.
    const std::uint8_t* p = sec + 24;
    info.range.reserve(range_count);
    for (std::uint16_t r = 0; r < range_count; ++r) {
      const std::uint64_t combo_idx = read_u64(p);
      const double weight = read_f64(p + 8);
      const auto cards = bs::comboCards(static_cast<int>(combo_idx));
      info.range.push_back(solver::WeightedHand{cards, weight});
      p += 16;
    }

    // Codebook.
    if (codebook_size > 0) {
      info.codebook = reinterpret_cast<const double*>(p);
      p += codebook_size * 8;
    }

    // Hashes.
    info.hashes = reinterpret_cast<const std::uint64_t*>(p);
    p += info.info_set_count * 8;

    // Row offsets.
    info.row_offsets = reinterpret_cast<const std::uint32_t*>(p);
    p += info.info_set_count * 4;

    // Action counts.
    info.action_counts = reinterpret_cast<const std::uint16_t*>(p);
    p += info.info_set_count * 2;

    // Actions (CompactAction is packed, 1-byte alignment).
    info.actions = reinterpret_cast<const CompactAction*>(p);
    p += info.action_count * 3;

    // Probability storage.
    if (impl_->codebook_type == ProbabilityLayout::Codebook8) {
      info.prob_indices_8 = p;
    } else if (impl_->codebook_type == ProbabilityLayout::Codebook16) {
      // Align to 2 bytes.
      const auto addr = reinterpret_cast<std::uintptr_t>(p);
      if (addr % 2 != 0)
        ++p;
      info.prob_indices_16 = reinterpret_cast<const std::uint16_t*>(p);
    } else {
      // Doubles: align to 8 bytes.
      const auto addr = reinterpret_cast<std::uintptr_t>(p);
      const std::size_t misalign = addr % 8;
      if (misalign != 0)
        p += (8 - misalign);
      info.probabilities = reinterpret_cast<const double*>(p);
    }

    impl_->classes.push_back(std::move(info));
  }

  // Build lookup maps.
  for (std::size_t i = 0; i < impl_->classes.size(); ++i) {
    const auto& info = impl_->classes[i];
    impl_->class_id_map[info.class_id] = i;
    impl_->board_map[board_key(info.canonical_board)] = info.class_id;
  }
}

bool MmapLibrary::find(std::uint32_t class_id, std::span<const std::uint8_t> compact_key,
                       CompactRowView& out) const {
  const auto it = impl_->class_id_map.find(class_id);
  if (it == impl_->class_id_map.end())
    return false;
  const Impl::ClassInfo& info = impl_->classes[it->second];

  const std::uint64_t target_hash = hash_key_64(compact_key);

  // Binary search over the sorted hashes.
  std::size_t lo = 0;
  std::size_t hi = info.info_set_count;
  while (lo < hi) {
    const std::size_t mid = lo + (hi - lo) / 2;
    const std::uint64_t mid_hash = info.hashes[mid];
    if (mid_hash < target_hash)
      lo = mid + 1;
    else
      hi = mid;
  }
  if (lo >= info.info_set_count || info.hashes[lo] != target_hash)
    return false;

  // Verify the full compact key (hash collisions are possible).
  // The compact key is not stored in the binary format (only the hash), so
  // a hash collision would return the wrong row. The 64-bit hash makes this
  // negligibly unlikely, but the ResidentIndex path verifies the full key.
  // For the mmap path, we rely on the 64-bit hash's collision resistance.
  // TODO: store compact keys in the binary format for full-key verification.

  const std::uint32_t row_offset = info.row_offsets[lo];
  const std::uint16_t action_count = info.action_counts[lo];

  out.count = action_count;
  out.actions = info.actions + row_offset;
  out.prob_layout = impl_->codebook_type;
  switch (impl_->codebook_type) {
    case ProbabilityLayout::Codebook8:
      out.codebook = info.codebook;
      out.probability_indices = info.prob_indices_8 + row_offset;
      out.probability_indices_16 = nullptr;
      out.probabilities = nullptr;
      break;
    case ProbabilityLayout::Codebook16:
      out.codebook = info.codebook;
      out.probability_indices = nullptr;
      out.probability_indices_16 = info.prob_indices_16 + row_offset;
      out.probabilities = nullptr;
      break;
    case ProbabilityLayout::Doubles:
      out.codebook = nullptr;
      out.probability_indices = nullptr;
      out.probability_indices_16 = nullptr;
      out.probabilities = info.probabilities + row_offset;
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

std::size_t MmapLibrary::info_set_count(std::uint32_t class_id) const {
  return impl_->find_class(class_id).info_set_count;
}

std::size_t MmapLibrary::action_count(std::uint32_t class_id) const {
  return impl_->find_class(class_id).action_count;
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
