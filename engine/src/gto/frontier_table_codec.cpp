// RFC 0013 Stage 2: binary serialization for NSeatDeclaredFrontierTable.
//
// Implements the BSF1 format documented in postflop_frontier.hpp. The codec
// uses only standard library headers so bigshark_solver needs no new link
// dependencies. SHA-256 is computed externally by callers.

#include <algorithm>
#include <bs/postflop_frontier.hpp>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>

namespace bs::gto {

namespace {

// On-disk header layout (128 bytes, packed).
struct [[gnu::packed]] Header {
  char magic[4];
  std::uint32_t version;
  std::uint8_t seat_count;
  std::uint8_t flags;
  std::uint16_t reserved;
  std::uint32_t entry_count;
  double representative_pot[SprBucketConfig::kBucketCount];
  char build_identity[56];
  std::uint8_t padding[8];
};

static_assert(sizeof(Header) == 128, "BSF1 header must be exactly 128 bytes");

// On-disk per-entry prefix (16 bytes, packed). The hands and values follow
// and have seat_count-dependent size.
struct [[gnu::packed]] EntryPrefix {
  std::int32_t canonical_flop[3];
  std::uint8_t spr_bucket;
  std::uint8_t pad[3];
};

static_assert(sizeof(EntryPrefix) == 16, "BSF1 entry prefix must be 16 bytes");

constexpr char kMagic[4] = {'B', 'S', 'F', '1'};
constexpr std::uint32_t kVersion = 1;

}  // namespace

void NSeatDeclaredFrontierTable::save(const std::filesystem::path& path,
                                      std::string_view build_identity) const {
  if (table_.empty())
    throw std::invalid_argument("NSeatDeclaredFrontierTable::save: table is empty");
  if (build_identity.size() > 55)
    throw std::invalid_argument(
        "NSeatDeclaredFrontierTable::save: build_identity exceeds 55 chars");

  // Determine the uniform seat count from the first entry.
  const std::size_t seat_count = table_.begin()->first.seat_count;
  if (seat_count < 2 || seat_count > poker::kMaxUnifiedSeats)
    throw std::invalid_argument("NSeatDeclaredFrontierTable::save: seat count out of range");

  // Validate uniform seat count across all entries.
  for (const auto& [key, values] : table_) {
    if (key.seat_count != seat_count)
      throw std::invalid_argument("NSeatDeclaredFrontierTable::save: mixed seat counts");
    if (values.size() != seat_count)
      throw std::invalid_argument("NSeatDeclaredFrontierTable::save: values/hands size mismatch");
  }

  // Build the header.
  Header header{};
  std::memcpy(header.magic, kMagic, 4);
  header.version = kVersion;
  header.seat_count = static_cast<std::uint8_t>(seat_count);
  header.flags = 0;
  header.reserved = 0;
  header.entry_count = static_cast<std::uint32_t>(table_.size());
  for (std::size_t b = 0; b < SprBucketConfig::kBucketCount; ++b)
    header.representative_pot[b] = metadata_.representative_pot[b];
  std::memcpy(header.build_identity, build_identity.data(), build_identity.size());
  // build_identity is zero-filled by the Header{} initialization; the null
  // terminator is implicit in the zero padding.

  // Compute the record size and expected file size.
  const std::size_t record_size = 16 + 16 * seat_count;
  const std::uint64_t expected_size =
      sizeof(Header) + static_cast<std::uint64_t>(header.entry_count) * record_size;

  // Write the file.
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out)
    throw std::runtime_error("NSeatDeclaredFrontierTable::save: cannot open file: " +
                             path.string());

  out.write(reinterpret_cast<const char*>(&header), sizeof(header));
  if (!out)
    throw std::runtime_error("NSeatDeclaredFrontierTable::save: header write failed");

  // Write each entry. Iterate in a deterministic order (sorted by key bytes)
  // so that two saves of the same table produce byte-identical files.
  std::vector<const std::pair<const Key, std::vector<double>>*> sorted;
  sorted.reserve(table_.size());
  for (const auto& kv : table_)
    sorted.push_back(&kv);
  std::sort(sorted.begin(), sorted.end(), [](const auto* a, const auto* b) {
    return std::memcmp(&a->first, &b->first, sizeof(Key)) < 0;
  });

  std::vector<char> record(record_size);
  for (const auto* kv : sorted) {
    const Key& key = kv->first;
    const std::vector<double>& values = kv->second;

    EntryPrefix prefix{};
    prefix.canonical_flop[0] = key.canonical_flop[0];
    prefix.canonical_flop[1] = key.canonical_flop[1];
    prefix.canonical_flop[2] = key.canonical_flop[2];
    prefix.spr_bucket = key.spr_bucket;
    // pad is zero-initialized.

    std::memcpy(record.data(), &prefix, sizeof(prefix));
    for (std::size_t s = 0; s < seat_count; ++s) {
      const std::int32_t cards[2] = {key.hands[s][0], key.hands[s][1]};
      std::memcpy(record.data() + 16 + s * 8, cards, 8);
    }
    for (std::size_t s = 0; s < seat_count; ++s) {
      const double v = values[s];
      std::memcpy(record.data() + 16 + seat_count * 8 + s * 8, &v, 8);
    }

    out.write(record.data(), record_size);
    if (!out)
      throw std::runtime_error("NSeatDeclaredFrontierTable::save: entry write failed");
  }

  // Flush the stream buffer so the filesystem reflects all writes before
  // checking the file size.
  out.flush();
  if (!out)
    throw std::runtime_error("NSeatDeclaredFrontierTable::save: flush failed");

  // Verify the file size.
  const auto actual_size = std::filesystem::file_size(path);
  if (actual_size != expected_size)
    throw std::runtime_error("NSeatDeclaredFrontierTable::save: file size mismatch after write");
}

NSeatDeclaredFrontierTable NSeatDeclaredFrontierTable::load(const std::filesystem::path& path,
                                                            std::string* build_identity_out) {
  // Read the entire file into memory.
  std::ifstream in(path, std::ios::binary | std::ios::ate);
  if (!in)
    throw std::runtime_error("NSeatDeclaredFrontierTable::load: cannot open file: " +
                             path.string());

  const std::uint64_t file_size = static_cast<std::uint64_t>(in.tellg());
  if (file_size < sizeof(Header))
    throw std::runtime_error("NSeatDeclaredFrontierTable::load: file too small for header");

  in.seekg(0);
  std::vector<char> data(static_cast<std::size_t>(file_size));
  in.read(data.data(), static_cast<std::streamsize>(file_size));
  if (!in)
    throw std::runtime_error("NSeatDeclaredFrontierTable::load: read failed");

  // Parse and validate the header.
  Header header{};
  std::memcpy(&header, data.data(), sizeof(header));

  if (std::memcmp(header.magic, kMagic, 4) != 0)
    throw std::runtime_error("NSeatDeclaredFrontierTable::load: bad magic");
  if (header.version != kVersion)
    throw std::runtime_error("NSeatDeclaredFrontierTable::load: unsupported version");
  if (header.seat_count < 2 || header.seat_count > poker::kMaxUnifiedSeats)
    throw std::runtime_error("NSeatDeclaredFrontierTable::load: seat count out of range");
  if (header.flags != 0)
    throw std::runtime_error("NSeatDeclaredFrontierTable::load: unsupported flags");
  if (header.reserved != 0)
    throw std::runtime_error("NSeatDeclaredFrontierTable::load: reserved field nonzero");

  const std::size_t seat_count = header.seat_count;
  const std::size_t record_size = 16 + 16 * seat_count;
  const std::uint64_t expected_size =
      sizeof(Header) + static_cast<std::uint64_t>(header.entry_count) * record_size;
  if (file_size != expected_size)
    throw std::runtime_error("NSeatDeclaredFrontierTable::load: file size mismatch (expected " +
                             std::to_string(expected_size) + ", got " + std::to_string(file_size) +
                             ")");

  // Validate representative pots.
  Metadata metadata{};
  for (std::size_t b = 0; b < SprBucketConfig::kBucketCount; ++b) {
    if (!std::isfinite(header.representative_pot[b]) || header.representative_pot[b] <= 0.0)
      throw std::runtime_error("NSeatDeclaredFrontierTable::load: representative_pot invalid");
    metadata.representative_pot[b] = header.representative_pot[b];
  }

  // Extract build identity (null-terminated within the 56-byte field).
  std::string build_identity;
  if (build_identity_out != nullptr) {
    const char* id = header.build_identity;
    const std::size_t len = strnlen(id, 56);
    build_identity.assign(id, len);
    *build_identity_out = build_identity;
  }

  // Parse entries.
  std::vector<Entry> entries;
  entries.reserve(header.entry_count);

  const char* ptr = data.data() + sizeof(Header);
  for (std::uint32_t i = 0; i < header.entry_count; ++i) {
    EntryPrefix prefix{};
    std::memcpy(&prefix, ptr, sizeof(prefix));

    if (prefix.spr_bucket >= SprBucketConfig::kBucketCount)
      throw std::runtime_error("NSeatDeclaredFrontierTable::load: SPR bucket out of range");

    Entry entry;
    entry.canonical_flop = {prefix.canonical_flop[0], prefix.canonical_flop[1],
                            prefix.canonical_flop[2]};
    entry.spr_bucket = prefix.spr_bucket;
    entry.hands.resize(seat_count);
    entry.values.resize(seat_count);

    const char* hands_ptr = ptr + 16;
    for (std::size_t s = 0; s < seat_count; ++s) {
      std::int32_t cards[2];
      std::memcpy(cards, hands_ptr + s * 8, 8);
      if (cards[0] < 0 || cards[0] > 51 || cards[1] < 0 || cards[1] > 51)
        throw std::runtime_error("NSeatDeclaredFrontierTable::load: card id out of range");
      entry.hands[s] = {cards[0], cards[1]};
    }

    const char* values_ptr = ptr + 16 + seat_count * 8;
    for (std::size_t s = 0; s < seat_count; ++s) {
      double v;
      std::memcpy(&v, values_ptr + s * 8, 8);
      if (!std::isfinite(v))
        throw std::runtime_error("NSeatDeclaredFrontierTable::load: non-finite value");
      entry.values[s] = v;
    }

    entries.push_back(std::move(entry));
    ptr += record_size;
  }

  return NSeatDeclaredFrontierTable(std::move(entries), std::move(metadata));
}

std::vector<NSeatDeclaredFrontierTable::Entry> NSeatDeclaredFrontierTable::export_entries() const {
  std::vector<Entry> entries;
  entries.reserve(table_.size());
  for (const auto& [key, values] : table_) {
    Entry entry;
    entry.canonical_flop = key.canonical_flop;
    entry.spr_bucket = key.spr_bucket;
    entry.hands.resize(key.seat_count);
    for (std::size_t s = 0; s < key.seat_count; ++s)
      entry.hands[s] = key.hands[s];
    entry.values = values;
    entries.push_back(std::move(entry));
  }
  return entries;
}

}  // namespace bs::gto
