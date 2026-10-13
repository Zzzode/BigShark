// RFC 0014 optimization: one-shot migration tool that recompresses .atf and
// .nsp files from raw ATF1/NSP1 to zstd-compressed ATFZ/NSPZ.
//
// Usage: bigshark-tree-recompress <dir> [threads]
//
// Scans <dir> for *.atf and *.nsp files, checks the magic bytes, and
// recompresses raw files in place (atomic write via .tmp + rename). Files
// that are already compressed (ATFZ/NSPZ magic) are skipped.

#include <zstd.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

namespace {

constexpr char kAtf1Magic[4] = {'A', 'T', 'F', '1'};
constexpr char kAtfZMagic[4] = {'A', 'T', 'F', 'Z'};
constexpr char kNsp1Magic[4] = {'N', 'S', 'P', '1'};
constexpr char kNspZMagic[4] = {'N', 'S', 'P', 'Z'};
constexpr std::uint32_t kVersion = 1;
constexpr int kZstdLevel = 3;

struct FileEntry {
  fs::path path;
  bool is_policy;  // true = .nsp, false = .atf
};

// Read the entire file into a vector. Returns false on error.
bool read_file(const fs::path& path, std::vector<char>& out) {
  std::ifstream in(path, std::ios::binary | std::ios::ate);
  if (!in)
    return false;
  const auto size = in.tellg();
  if (size <= 0)
    return false;
  in.seekg(0);
  out.resize(static_cast<std::size_t>(size));
  in.read(out.data(), size);
  return static_cast<bool>(in);
}

// Write data atomically: write to <path>.tmp then rename.
bool write_atomic(const fs::path& path, const char* data, std::size_t size) {
  const fs::path tmp = std::string(path.string()) + ".tmp";
  std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
  if (!out)
    return false;
  out.write(data, static_cast<std::streamsize>(size));
  if (!out)
    return false;
  out.close();
  std::error_code ec;
  fs::rename(tmp, path, ec);
  return !ec;
}

// Recompress a single file. Returns: 0 = compressed, 1 = skipped (already
// compressed), 2 = error.
int recompress_file(const FileEntry& entry) {
  // Read the first 4 bytes to check magic.
  std::ifstream in(entry.path, std::ios::binary);
  if (!in) {
    std::cerr << "ERROR: cannot open " << entry.path << "\n";
    return 2;
  }
  char magic[4];
  in.read(magic, 4);
  if (!in) {
    std::cerr << "ERROR: cannot read magic from " << entry.path << "\n";
    return 2;
  }
  in.close();

  const char* raw_magic = entry.is_policy ? kNsp1Magic : kAtf1Magic;
  const char* comp_magic = entry.is_policy ? kNspZMagic : kAtfZMagic;

  // Already compressed?
  if (std::memcmp(magic, comp_magic, 4) == 0)
    return 1;

  // Not raw?
  if (std::memcmp(magic, raw_magic, 4) != 0) {
    std::cerr << "ERROR: unknown magic in " << entry.path << "\n";
    return 2;
  }

  // Read the entire raw file.
  std::vector<char> raw;
  if (!read_file(entry.path, raw)) {
    std::cerr << "ERROR: cannot read " << entry.path << "\n";
    return 2;
  }

  // Compress.
  const std::size_t bound = ZSTD_compressBound(raw.size());
  std::vector<char> compressed(bound);
  const std::size_t comp_size =
      ZSTD_compress(compressed.data(), bound, raw.data(), raw.size(), kZstdLevel);
  if (ZSTD_isError(comp_size)) {
    std::cerr << "ERROR: zstd compress failed for " << entry.path << ": "
              << ZSTD_getErrorName(comp_size) << "\n";
    return 2;
  }

  // Build output: magic(4) + version(4) + uncompressed_size(8) + compressed.
  const std::size_t out_size = 16 + comp_size;
  std::vector<char> output(out_size);
  std::memcpy(output.data(), comp_magic, 4);
  const std::uint32_t ver = kVersion;
  std::memcpy(output.data() + 4, &ver, 4);
  const std::uint64_t usize = static_cast<std::uint64_t>(raw.size());
  std::memcpy(output.data() + 8, &usize, 8);
  std::memcpy(output.data() + 16, compressed.data(), comp_size);

  if (!write_atomic(entry.path, output.data(), out_size)) {
    std::cerr << "ERROR: cannot write " << entry.path << "\n";
    return 2;
  }

  return 0;
}

}  // namespace

int main(int argc, char* argv[]) {
  if (argc < 2) {
    std::cerr << "Usage: " << argv[0] << " <dir> [threads]\n";
    return 1;
  }

  const fs::path dir = argv[1];
  if (!fs::is_directory(dir)) {
    std::cerr << "ERROR: " << dir << " is not a directory\n";
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
    const auto& path = entry.path();
    const auto ext = path.extension().string();
    if (ext == ".atf")
      files.push_back({path, false});
    else if (ext == ".nsp")
      files.push_back({path, true});
  }

  if (files.empty()) {
    std::cout << "No .atf or .nsp files found in " << dir << "\n";
    return 0;
  }

  std::cout << "Recompress tool\n"
            << "  dir:     " << dir << "\n"
            << "  files:   " << files.size() << "\n"
            << "  threads: " << num_threads << "\n";

  // Parallel recompression.
  std::atomic<std::size_t> next_index{0};
  std::atomic<std::size_t> compressed{0};
  std::atomic<std::size_t> skipped{0};
  std::atomic<std::size_t> failed{0};

  auto worker = [&]() {
    while (true) {
      const std::size_t idx = next_index.fetch_add(1, std::memory_order_relaxed);
      if (idx >= files.size())
        break;
      const int result = recompress_file(files[idx]);
      if (result == 0)
        compressed.fetch_add(1, std::memory_order_relaxed);
      else if (result == 1)
        skipped.fetch_add(1, std::memory_order_relaxed);
      else
        failed.fetch_add(1, std::memory_order_relaxed);
    }
  };

  std::vector<std::thread> threads;
  threads.reserve(num_threads);
  for (unsigned i = 0; i < num_threads; ++i)
    threads.emplace_back(worker);
  for (auto& t : threads)
    t.join();

  // Report disk savings.
  std::uintmax_t total_size = 0;
  for (const auto& f : files) {
    std::error_code ec;
    total_size += fs::file_size(f.path, ec);
  }

  std::cout << "\n--- Done ---\n"
            << "  compressed: " << compressed.load() << "\n"
            << "  skipped:    " << skipped.load() << "\n"
            << "  failed:     " << failed.load() << "\n"
            << "  total size: " << (total_size / (1024.0 * 1024.0)) << " MB\n";

  return failed.load() > 0 ? 1 : 0;
}
