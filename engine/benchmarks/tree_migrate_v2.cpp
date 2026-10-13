// RFC 0014 Phase 0: migrate .atf tree files from v1 (DFS terminal recompute)
// to v2 (serialized terminal payloads).
//
// Usage: bigshark-tree-migrate-v2 <dir> [threads]
//
// Scans <dir> for *.atf files, loads each (v1 or v2), re-saves in v2 format
// (atomic write via .tmp + rename). Files that are already v2 are skipped.
//
// This is an offline tool, NOT a CTest. The full migration of 10,530 files
// takes ~10-30 minutes depending on tree sizes and thread count.

#include <algorithm>
#include <array>
#include <atomic>
#include <bs/abstract_tree.hpp>
#include <bs/abstraction.hpp>
#include <bs/game_definition.hpp>
#include <bs/postflop_config.hpp>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using namespace bs::abstraction;
using namespace bs::poker;
using namespace bs::tree;
using namespace bs::gto;

namespace {

struct FileEntry {
  fs::path path;
  std::array<int, 3> flop_class;
  std::uint8_t spr_bucket;
};

// Parse the flop class and SPR bucket from the filename.
// Format: tree_{c0}_{c1}_{c2}_b{bucket}.atf
bool parse_filename(const fs::path& path, FileEntry& entry) {
  const std::string name = path.stem().string();  // e.g. "tree_0_1_10_b0"
  if (name.rfind("tree_", 0) != 0)
    return false;

  // Parse: tree_{c0}_{c1}_{c2}_b{bucket}
  int c0, c1, c2, bucket;
  char b;
  if (std::sscanf(name.c_str(), "tree_%d_%d_%d_%c%d", &c0, &c1, &c2, &b, &bucket) != 5)
    return false;
  if (b != 'b')
    return false;
  if (bucket < 0 || bucket > 5)
    return false;

  entry.path = path;
  entry.flop_class = {c0, c1, c2};
  entry.spr_bucket = static_cast<std::uint8_t>(bucket);
  return true;
}

// Check the ATFZ version in the file header. Returns -1 on error.
int read_atfz_version(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in)
    return -1;
  char magic[4];
  in.read(magic, 4);
  if (!in || std::memcmp(magic, "ATFZ", 4) != 0)
    return -1;
  std::uint32_t version;
  in.read(reinterpret_cast<char*>(&version), 4);
  if (!in)
    return -1;
  return static_cast<int>(version);
}

// Migrate a single file: load (v1 or v2) and re-save as v2.
// Returns: 0 = migrated, 1 = skipped (already v2), 2 = error.
int migrate_file(const FileEntry& entry) {
  const int version = read_atfz_version(entry.path);
  if (version < 0) {
    std::fprintf(stderr, "ERROR: cannot read version from %s\n", entry.path.c_str());
    return 2;
  }
  if (version >= 2)
    return 1;  // Already v2

  try {
    const GameDef def = make_postflop_def(entry.flop_class, entry.spr_bucket);
    const ActionAbstraction action = make_postflop_abstraction(entry.spr_bucket);
    TreeLimits limits;
    limits.max_bytes = std::size_t{4} << 30;
    limits.max_nodes = 5'000'000;

    AbstractTree tree = AbstractTree::load(entry.path, def, action, limits);
    tree.save(entry.path);  // Atomic write (tmp + rename)
    return 0;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "ERROR: %s: %s\n", entry.path.c_str(), e.what());
    return 2;
  }
}

}  // namespace

int main(int argc, char* argv[]) {
  if (argc < 2) {
    std::fprintf(stderr,
                 "usage: %s <dir> [threads]\n"
                 "  Migrates .atf tree files from v1 to v2 format.\n"
                 "  v2 files include serialized terminal payloads,\n"
                 "  eliminating the DFS terminal recompute on load.\n",
                 argv[0]);
    return 1;
  }

  const fs::path dir = argv[1];
  if (!fs::is_directory(dir)) {
    std::fprintf(stderr, "ERROR: %s is not a directory\n", dir.c_str());
    return 1;
  }

  unsigned num_threads = std::thread::hardware_concurrency();
  if (num_threads == 0)
    num_threads = 4;
  if (argc >= 3) {
    num_threads = static_cast<unsigned>(std::stoul(argv[2]));
    if (num_threads == 0)
      num_threads = 1;
  }

  // Collect files.
  std::vector<FileEntry> files;
  for (const auto& entry : fs::directory_iterator(dir)) {
    if (!entry.is_regular_file())
      continue;
    if (entry.path().extension() != ".atf")
      continue;
    FileEntry fe;
    if (parse_filename(entry.path(), fe))
      files.push_back(fe);
  }

  if (files.empty()) {
    std::printf("No .atf files found in %s\n", dir.c_str());
    return 0;
  }

  std::printf(
      "Tree v2 migration tool\n"
      "  dir:     %s\n"
      "  files:   %zu\n"
      "  threads: %u\n",
      dir.c_str(), files.size(), num_threads);

  // Parallel migration.
  std::atomic<std::size_t> next_index{0};
  std::atomic<std::size_t> migrated{0};
  std::atomic<std::size_t> skipped{0};
  std::atomic<std::size_t> failed{0};

  auto worker = [&]() {
    while (true) {
      const std::size_t idx = next_index.fetch_add(1, std::memory_order_relaxed);
      if (idx >= files.size())
        break;
      const int result = migrate_file(files[idx]);
      if (result == 0) {
        const auto m = migrated.fetch_add(1, std::memory_order_relaxed) + 1;
        if (m % 100 == 0)
          std::printf("  ... %zu/%zu migrated\n", m, files.size());
      } else if (result == 1) {
        skipped.fetch_add(1, std::memory_order_relaxed);
      } else {
        failed.fetch_add(1, std::memory_order_relaxed);
      }
    }
  };

  std::vector<std::thread> threads;
  threads.reserve(num_threads);
  for (unsigned i = 0; i < num_threads; ++i)
    threads.emplace_back(worker);
  for (auto& t : threads)
    t.join();

  std::printf(
      "\n--- Done ---\n"
      "  migrated: %zu\n"
      "  skipped:  %zu\n"
      "  failed:   %zu\n",
      migrated.load(), skipped.load(), failed.load());

  return failed.load() > 0 ? 1 : 0;
}
