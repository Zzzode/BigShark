// RFC 0014: pre-build tool for the postflop tree cache.
//
// Builds and serializes AbstractTree instances for every canonical flop class
// x SPR bucket config, so the on-the-fly evaluator can load them from disk
// (~100 ms) instead of building from scratch (~5 s). This is the difference
// between an 11-day and a 12-hour preflop blueprint run.
//
// Usage: bigshark-postflop-tree-cache <output-dir> [classes] [buckets]
//        [class-start] [threads]
//   output-dir:  directory to write .atf tree files
//   classes:     limit canonical classes (0 = all remaining from class-start)
//   buckets:     limit SPR buckets (0 = all 6)
//   class-start: first canonical class index (default 0, for sharding)
//   threads:     parallel worker count (default = hardware concurrency)
//
// This is an offline tool, NOT a CTest. The full build is a long-running
// background compute task.
#include <algorithm>
#include <array>
#include <atomic>
#include <bs/abstract_tree.hpp>
#include <bs/abstraction.hpp>
#include <bs/game_definition.hpp>
#include <bs/postflop_config.hpp>
#include <bs/postflop_frontier.hpp>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace bs::abstraction;
using namespace bs::poker;
using namespace bs::tree;
using namespace bs::gto;

namespace {

struct Config {
  std::array<int, 3> flop_class;
  std::size_t bucket;
};

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr,
                 "usage: %s <output-dir> [classes] [buckets] [class-start] [threads]\n"
                 "  output-dir:  directory to write .atf tree files\n"
                 "  classes:     limit canonical classes (0 = all)\n"
                 "  buckets:     limit SPR buckets (0 = all 6)\n"
                 "  class-start: first canonical class index (default 0)\n"
                 "  threads:     parallel workers (default = hardware concurrency)\n",
                 argv[0]);
    return 1;
  }

  const std::filesystem::path output_dir = argv[1];
  const int class_limit = argc > 2 ? std::atoi(argv[2]) : 0;
  const int bucket_limit = argc > 3 ? std::atoi(argv[3]) : 0;
  const std::size_t class_start = argc > 4 ? static_cast<std::size_t>(std::atoll(argv[4])) : 0;
  const unsigned num_threads = argc > 5 ? static_cast<unsigned>(std::atoi(argv[5]))
                                        : std::max(1u, std::thread::hardware_concurrency());

  const std::size_t num_buckets =
      bucket_limit > 0 ? static_cast<std::size_t>(bucket_limit) : SprBucketConfig::kBucketCount;

  std::error_code ec;
  std::filesystem::create_directories(output_dir, ec);
  if (ec) {
    std::fprintf(stderr, "cannot create output dir %s: %s\n", output_dir.string().c_str(),
                 ec.message().c_str());
    return 1;
  }

  const auto all_classes = bs::abstraction::enumerate_canonical_flop_classes();
  if (class_start >= all_classes.size()) {
    std::fprintf(stderr, "class-start %zu exceeds total classes %zu\n", class_start,
                 all_classes.size());
    return 1;
  }
  const std::size_t remaining = all_classes.size() - class_start;
  const std::size_t num_classes =
      class_limit > 0 ? std::min(static_cast<std::size_t>(class_limit), remaining) : remaining;

  // Build the work list.
  std::vector<Config> configs;
  configs.reserve(num_classes * num_buckets);
  for (std::size_t ci = class_start; ci < class_start + num_classes; ++ci) {
    for (std::size_t b = 0; b < num_buckets; ++b)
      configs.push_back({all_classes[ci], b});
  }

  std::printf("RFC 0014: Postflop tree cache builder\n");
  std::printf("  output dir:  %s\n", output_dir.string().c_str());
  std::printf("  classes:     %zu [%zu, %zu)\n", num_classes, class_start,
              class_start + num_classes);
  std::printf("  buckets:     %zu\n", num_buckets);
  std::printf("  configs:     %zu\n", configs.size());
  std::printf("  threads:     %u\n", num_threads);
  std::fflush(stdout);

  // Tree limits matching the oracle.
  TreeLimits tree_limits;
  tree_limits.max_bytes = std::size_t{4} << 30;  // 4 GiB
  tree_limits.max_nodes = 5'000'000;

  std::atomic<std::size_t> next_config{0};
  std::atomic<std::size_t> completed{0};
  std::atomic<std::size_t> skipped{0};
  std::atomic<std::size_t> failed{0};
  std::mutex print_mutex;

  const auto start_time = std::chrono::steady_clock::now();

  auto worker = [&]() {
    while (true) {
      const std::size_t idx = next_config.fetch_add(1, std::memory_order_relaxed);
      if (idx >= configs.size())
        break;

      const Config& cfg = configs[idx];
      const std::string fname =
          tree_file_name(cfg.flop_class, static_cast<std::uint8_t>(cfg.bucket));
      const std::filesystem::path path = output_dir / fname;

      // Skip if already cached.
      if (std::filesystem::exists(path)) {
        skipped.fetch_add(1, std::memory_order_relaxed);
        continue;
      }

      try {
        const GameDef def = make_postflop_def(cfg.flop_class, cfg.bucket);
        const ActionAbstraction abstraction = make_postflop_abstraction(cfg.bucket);
        AbstractTree tree(def, abstraction, tree_limits);
        tree.save(path);

        const std::size_t done = completed.fetch_add(1, std::memory_order_relaxed) + 1;
        if (done % 100 == 0 || done == configs.size()) {
          const auto elapsed = std::chrono::steady_clock::now() - start_time;
          const double secs = std::chrono::duration<double>(elapsed).count();
          const double rate = done / secs;
          const double eta = (configs.size() - done) / rate;
          std::lock_guard<std::mutex> lock(print_mutex);
          std::printf("  %zu/%zu done (%.0f/s, ETA %.0f s)\n", done, configs.size(), rate, eta);
          std::fflush(stdout);
        }
      } catch (const std::exception& e) {
        failed.fetch_add(1, std::memory_order_relaxed);
        std::lock_guard<std::mutex> lock(print_mutex);
        std::fprintf(stderr, "  FAILED %s: %s\n", fname.c_str(), e.what());
      }
    }
  };

  std::vector<std::thread> threads;
  threads.reserve(num_threads);
  for (unsigned i = 0; i < num_threads; ++i)
    threads.emplace_back(worker);
  for (auto& t : threads)
    t.join();

  const auto elapsed = std::chrono::steady_clock::now() - start_time;
  const double secs = std::chrono::duration<double>(elapsed).count();

  std::printf("\n--- Done ---\n");
  std::printf("  completed: %zu\n", completed.load());
  std::printf("  skipped:   %zu\n", skipped.load());
  std::printf("  failed:    %zu\n", failed.load());
  std::printf("  wall time: %.1f s\n", secs);

  // Report total disk usage.
  std::uintmax_t total_bytes = 0;
  for (const auto& entry : std::filesystem::directory_iterator(output_dir)) {
    if (entry.is_regular_file() && entry.path().extension() == ".atf")
      total_bytes += entry.file_size();
  }
  std::printf(
      "  disk:      %.1f MB (%zu files)\n", static_cast<double>(total_bytes) / (1024.0 * 1024.0),
      static_cast<std::size_t>(std::count_if(
          std::filesystem::directory_iterator(output_dir), std::filesystem::directory_iterator(),
          [](const auto& e) { return e.is_regular_file() && e.path().extension() == ".atf"; })));

  return failed.load() > 0 ? 1 : 0;
}
