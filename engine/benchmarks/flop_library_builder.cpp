// flop_library_builder.cpp — RFC 0009 W4c-iii: offline flop class library builder.
//
// Builds the first trained class library: per-class flop-rooted schema-v3
// artifacts for the first N canonical classes (by board-id order), plus a
// manifest recording the honest coverage and storage measurements. Offline
// only: no service, host, protocol, network, or credentials.
//
// Usage: bigshark-flop-library-builder [options] <output-dir> [class-count]
//                                      [iterations] [player-count] [stack]
//                                      [contribution] [abstraction]
//   class-count   number of canonical classes to train (default 8)
//   iterations    MCCFR iterations per class      (default 1000)
//   player-count  seats in the game, 2..10         (default 2)
//   stack         starting stack in chips          (default 6 = 3 BB)
//   contribution  preflop contribution in chips    (default 2 = 1 BB)
//                 Chips behind at the root are stack - contribution, so the
//                 default root is 4 behind with SPR 1 (RFC 0033).
//   abstraction   'identity' (default) or 'coarse' (RFC 0008 DeclaredOnly)
//
// Options:
//   --parallel N  train classes with N forked worker processes (default 1 =
//                 serial). Each worker trains a strided class range and writes
//                 a manifest fragment; the parent merges them after all workers
//                 exit. Refused if N x the per-worker RSS estimate would exceed
//                 75% of physical RAM.
//   --resume      warm-start each class from its class-NNNN-checkpoint.bswarm
//                 if present (RFC 0028). A checkpoint is written every
//                 256 iterations so a crashed build can resume.
//   --raise-cap N cap the number of full raises per street (RFC 0024). Zero
//                 means no cap (the default). The cap is recorded in the
//                 library manifest.
//
// Worker mode (internal, spawned by the parent via fork/exec):
//   --worker-id W --parallel N  (then the same positional args)
#include <sys/wait.h>
#include <unistd.h>

#include <bs/abstraction.hpp>
#include <bs/flop_library.hpp>
#include <bs/heads_up.hpp>  // poker::Chips
#include <bs/sealed_checkpoint.hpp>
#include <bs/strategy_artifact.hpp>
#include <cerrno>
#include <chrono>
#include <climits>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace {

// A conservative per-worker peak-RSS estimate for the offline training caps:
// the tree's 1 GiB byte cap, the trainer's row store, the frontier evaluator,
// and process overhead. Used only for the --parallel memory gate.
constexpr std::uint64_t kPeakRssEstimateBytes = 2ULL * 1024 * 1024 * 1024;  // 2 GiB

struct Args {
  std::filesystem::path out_dir;
  std::size_t class_count = 8;
  std::uint64_t iterations = 1000;
  std::size_t player_count = 2;
  // RFC 0033: a starting stack, not chips behind. 6 - 2 = 4 behind, SPR 1.
  bs::poker::Chips stack = 6;
  bs::poker::Chips contribution = 2;
  bool coarse = false;
  bool full_range = false;  // 1,326 combos/seat instead of the 40-combo premium range
  bool doubles = false;     // RFC 0027: exact-doubles probability layout
  std::size_t parallel = 1;
  bool resume = false;
  std::uint8_t raise_cap = 0;
  // Worker mode (internal).
  bool worker = false;
  std::size_t worker_id = 0;
};

[[noreturn]] void usage(const char* prog) {
  std::fprintf(stderr,
               "usage: %s [options] <output-dir> [class-count] [iterations] [player-count] "
               "[stack] [contribution] [abstraction]\n"
               "  class-count   number of canonical classes to train (default 8)\n"
               "  iterations    MCCFR iterations per class      (default 1000)\n"
               "  player-count  seats in the game, 2..10         (default 2)\n"
               "  stack         starting stack in chips          (default 6 = 3 BB; chips behind = "
               "stack - contribution)\n"
               "  contribution  preflop contribution in chips    (default 2 = 1 BB)\n"
               "  abstraction   'identity' (default) or 'coarse' (RFC 0008 DeclaredOnly)\n"
               "options:\n"
               "  --parallel N  train with N forked workers      (default 1)\n"
               "  --resume      warm-start from per-class checkpoints\n"
               "  --raise-cap N cap raises per street, 0 = no cap (default 0)\n"
               "  --range S     'premium' 40 combos/seat (default) or 'full' 1,326\n"
               "  --codebook C  '16' (default) or 'doubles' probability layout\n",
               prog);
  std::exit(2);
}

Args parse_args(int argc, char** argv) {
  Args args;
  std::vector<std::string> positional;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--parallel") {
      if (i + 1 >= argc)
        usage(argv[0]);
      args.parallel = static_cast<std::size_t>(std::strtoull(argv[++i], nullptr, 10));
      if (args.parallel == 0) {
        std::fprintf(stderr, "--parallel must be positive\n");
        std::exit(2);
      }
    } else if (arg == "--resume") {
      args.resume = true;
    } else if (arg == "--raise-cap") {
      if (i + 1 >= argc)
        usage(argv[0]);
      const unsigned long long cap = std::strtoull(argv[++i], nullptr, 10);
      if (cap > 255) {
        std::fprintf(stderr, "--raise-cap must be in 0..255\n");
        std::exit(2);
      }
      args.raise_cap = static_cast<std::uint8_t>(cap);
    } else if (arg == "--range") {
      if (i + 1 >= argc)
        usage(argv[0]);
      const std::string value = argv[++i];
      if (value == "full")
        args.full_range = true;
      else if (value == "premium")
        args.full_range = false;
      else {
        std::fprintf(stderr, "--range must be 'premium' or 'full'\n");
        std::exit(2);
      }
    } else if (arg == "--worker-id") {
      if (i + 1 >= argc)
        usage(argv[0]);
      args.worker = true;
      args.worker_id = static_cast<std::size_t>(std::strtoull(argv[++i], nullptr, 10));
    } else if (!arg.empty() && arg[0] == '-') {
      std::fprintf(stderr, "unknown option: %s\n", arg.c_str());
      usage(argv[0]);
    } else {
      positional.push_back(arg);
    }
  }
  if (positional.empty())
    usage(argv[0]);
  if (positional.size() > 7)
    usage(argv[0]);
  args.out_dir = positional[0];
  if (positional.size() > 1)
    args.class_count = std::stoull(positional[1]);
  if (positional.size() > 2)
    args.iterations = std::strtoull(positional[2].c_str(), nullptr, 10);
  if (positional.size() > 3)
    args.player_count = std::stoull(positional[3]);
  if (positional.size() > 4)
    args.stack = static_cast<bs::poker::Chips>(std::strtoull(positional[4].c_str(), nullptr, 10));
  if (positional.size() > 5)
    args.contribution =
        static_cast<bs::poker::Chips>(std::strtoull(positional[5].c_str(), nullptr, 10));
  if (positional.size() > 6) {
    if (positional[6] == "coarse")
      args.coarse = true;
    else if (positional[6] != "identity") {
      std::fprintf(stderr, "abstraction must be 'identity' or 'coarse' (got '%s')\n",
                   positional[6].c_str());
      std::exit(2);
    }
  }
  return args;
}

bs::flop_library::BuildParams make_params(const Args& args) {
  bs::flop_library::BuildParams params;
  params.class_count = args.class_count;
  params.iterations_per_class = args.iterations;
  params.seed = 20261002;
  params.starting_stack = args.stack;
  params.contribution = args.contribution;
  params.range = args.full_range ? bs::flop_library::declared_full_library_range()
                                 : bs::flop_library::declared_library_range();
  params.player_count = args.player_count;
  params.action = args.coarse ? bs::abstraction::ActionAbstraction::declared(
                                    bs::abstraction::default_size_schedule(),
                                    bs::abstraction::CoverSeeds::DeclaredOnly, args.raise_cap)
                              : bs::abstraction::ActionAbstraction::identity(args.raise_cap);
  params.resume = args.resume;
  params.range_name = args.full_range ? "full" : "premium";
  params.layout = args.doubles ? bs::resident::ProbabilityLayout::Doubles
                               : bs::resident::ProbabilityLayout::Codebook16;
  return params;
}

// Return the path to the running executable (for fork/exec).
std::string executable_path() {
  char buf[4096];
  const ssize_t len = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
  if (len <= 0) {
    std::perror("readlink /proc/self/exe");
    std::exit(1);
  }
  buf[len] = '\0';
  return buf;
}

// RFC 0012 Phase 1: refuse --parallel N if N x the per-worker RSS estimate
// would exceed 75% of physical RAM.
void check_memory_budget(std::size_t parallel) {
  const long page_count = ::sysconf(_SC_PHYS_PAGES);
  const long page_size = ::sysconf(_SC_PAGE_SIZE);
  if (page_count <= 0 || page_size <= 0) {
    std::fprintf(stderr, "warning: cannot determine physical RAM; skipping memory gate\n");
    return;
  }
  const std::uint64_t total_ram =
      static_cast<std::uint64_t>(page_count) * static_cast<std::uint64_t>(page_size);
  const std::uint64_t budget = (total_ram * 3) / 4;  // 75%
  const std::uint64_t estimated = static_cast<std::uint64_t>(parallel) * kPeakRssEstimateBytes;
  if (estimated > budget) {
    std::fprintf(stderr,
                 "refusing --parallel %zu: estimated RSS %llu MiB exceeds the 75%% memory "
                 "budget of %llu MiB (%llu MiB physical RAM, %llu MiB per worker)\n",
                 parallel, static_cast<unsigned long long>(estimated / (1024 * 1024)),
                 static_cast<unsigned long long>(budget / (1024 * 1024)),
                 static_cast<unsigned long long>(total_ram / (1024 * 1024)),
                 static_cast<unsigned long long>(kPeakRssEstimateBytes / (1024 * 1024)));
    std::exit(2);
  }
}

// Remove only incomplete artifacts (SQLite .tmp files and manifest fragments)
// after a worker failure. Completed sections and checkpoints are preserved so
// the build can resume with --resume. This is critical for long parallel builds
// where a single worker crash would otherwise destroy hours of progress.
void cleanup_incomplete_artifacts(const std::filesystem::path& out_dir) {
  std::error_code ec;
  if (!std::filesystem::is_directory(out_dir, ec))
    return;
  for (const auto& entry : std::filesystem::directory_iterator(out_dir, ec)) {
    const std::string name = entry.path().filename().string();
    // Remove SQLite temporary files (.bs-policy-*.tmp, *.tmp-journal).
    if (name.rfind(".bs-policy-", 0) == 0)
      std::filesystem::remove(entry.path(), ec);
    // RFC 0027: `.sections/` is durable progress and is kept; only its stale
    // `.tmp` files are removed.
    if (name.size() > 4 && name.substr(name.size() - 4) == ".tmp")
      std::filesystem::remove(entry.path(), ec);
  }
}

// Remove the mutable state of a build that could not start (fork failure):
// every checkpoint plus the section staging directory. RFC 0027 keeps
// `.sections/` only after training has begun, where it is durable progress;
// a spawn failure has no progress to keep.
void cleanup_failed_build(const std::filesystem::path& out_dir, std::size_t class_count) {
  std::error_code ec;
  // The CLI builds the single default limped root, so checkpoints use the
  // RFC 0035 root-0 naming. Multi-root builds go through the programmatic API.
  for (std::size_t i = 0; i < class_count; ++i) {
    char stem[32];
    std::snprintf(stem, sizeof(stem), "root-0000-class-%04zu", i);
    std::filesystem::remove(out_dir / (std::string(stem) + "-checkpoint.bswarm"), ec);
  }
  std::filesystem::remove_all(out_dir / ".sections", ec);
}

int run_worker(const Args& args) {
  const bs::flop_library::BuildParams params = make_params(args);
  bs::flop_library::build_library_worker(args.out_dir, args.worker_id, args.parallel, params);
  return 0;
}

int run_parallel(const Args& args) {
  check_memory_budget(args.parallel);
  const bs::flop_library::BuildParams params = make_params(args);
  // RFC 0033: one shared resume decision for every entry point. The inline
  // scan this replaces was the reason the parallel path drifted from the
  // serial one; the helper now owns torn/revision/stale/no-identity, reads
  // sealed headers only (RFC 0028), and runs before any worker is forked.
  bs::flop_library::preflight_resume(args.out_dir, params, params.resume);
  // RFC 0027: the parent owns the build identity. Write it before spawning so
  // every worker validates the same layout/identity and a resume can compare.
  bs::flop_library::prepare_build_identity(args.out_dir, params, params.resume);

  const std::string exe = executable_path();

  // Spawn N workers via fork/exec. Each worker trains a strided class range
  // and appends to its own JSONL fragment, so concurrent
  // workers never touch the same file.
  std::vector<pid_t> pids;
  for (std::size_t w = 0; w < args.parallel; ++w) {
    const pid_t pid = ::fork();
    if (pid < 0) {
      std::perror("fork");
      // Kill any children already spawned.
      for (pid_t p : pids)
        ::kill(p, SIGKILL);
      for (pid_t p : pids)
        ::waitpid(p, nullptr, 0);
      cleanup_failed_build(args.out_dir, args.class_count);
      return 1;
    }
    if (pid == 0) {
      // Child: exec the worker mode.
      const std::string wid = std::to_string(w);
      const std::string par = std::to_string(args.parallel);
      const std::string cc = std::to_string(args.class_count);
      const std::string it = std::to_string(args.iterations);
      const std::string pc = std::to_string(args.player_count);
      const std::string st = std::to_string(args.stack);
      const std::string ct = std::to_string(args.contribution);
      const char* abstraction = args.coarse ? "coarse" : "identity";
      const std::string cap = std::to_string(static_cast<int>(args.raise_cap));
      ::execl(exe.c_str(), exe.c_str(), "--worker-id", wid.c_str(), "--parallel", par.c_str(),
              args.out_dir.string().c_str(), cc.c_str(), it.c_str(), pc.c_str(), st.c_str(),
              ct.c_str(), abstraction, "--raise-cap", cap.c_str(),
              args.full_range ? "--range" : (const char*)nullptr,
              args.full_range ? "full" : (const char*)nullptr,
              args.doubles ? "--codebook" : (const char*)nullptr,
              args.doubles ? "doubles" : (const char*)nullptr, args.resume ? "--resume" : nullptr,
              nullptr);
      // exec failed.
      std::perror("execl");
      ::_exit(127);
    }
    pids.push_back(pid);
  }

  // Reap children. If any exits non-zero, kill the rest and clean up.
  bool any_failed = false;
  std::size_t reaped = 0;
  while (reaped < pids.size()) {
    int status = 0;
    const pid_t reaped_pid = ::waitpid(-1, &status, 0);
    if (reaped_pid < 0) {
      if (errno == EINTR)
        continue;
      std::perror("waitpid");
      any_failed = true;
      break;
    }
    ++reaped;
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
      any_failed = true;
      std::fprintf(stderr, "worker (pid %d) failed with status %d\n", static_cast<int>(reaped_pid),
                   WIFEXITED(status) ? WEXITSTATUS(status) : -1);
      // Kill every child still running.
      for (pid_t p : pids) {
        if (p != reaped_pid)
          ::kill(p, SIGKILL);
      }
    }
  }
  // Reap any children killed above.
  while (::waitpid(-1, nullptr, 0) > 0) {
  }

  if (any_failed) {
    // Preserve completed sections and checkpoint files so the build can resume
    // with --resume. Only remove incomplete artifacts (.tmp, fragments).
    cleanup_incomplete_artifacts(args.out_dir);
    std::fprintf(stderr,
                 "worker failure: completed class artifacts preserved. "
                 "Restart with --resume to continue.\n");
    return 1;
  }

  // All workers succeeded: assemble the sections into the final library.
  const auto assemble_start = std::chrono::steady_clock::now();
  const bs::flop_library::LibraryManifest manifest =
      bs::flop_library::assemble_library(args.out_dir, params);
  const auto assemble_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - assemble_start);
  std::printf("assembly: %.3f s for %zu classes\n",
              static_cast<double>(assemble_elapsed.count()) / 1000.0, manifest.class_count);
  const double coverage_pct = 100.0 * static_cast<double>(manifest.covered_flops) /
                              static_cast<double>(manifest.total_flops);
  std::printf(
      "flop library: %zu classes, %zu/%zu flops covered (%.4f%%), %llu bytes stored, %zu players, "
      "%s, %zu workers\n",
      manifest.class_count, manifest.covered_flops, manifest.total_flops, coverage_pct,
      static_cast<unsigned long long>(manifest.storage_bytes), manifest.player_count,
      manifest.action_id.name.c_str(), args.parallel);
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  const Args args = parse_args(argc, argv);
  if (args.worker)
    return run_worker(args);
  if (args.parallel > 1)
    return run_parallel(args);

  // Serial path.
  const bs::flop_library::BuildParams params = make_params(args);
  const auto serial_start = std::chrono::steady_clock::now();
  const bs::flop_library::LibraryManifest manifest = bs::flop_library::build_library(
      args.out_dir, params.class_count, params.iterations_per_class, params.seed,
      params.starting_stack, params.contribution, params.range, params.player_count, params.action,
      params.resume, params.layout, params.range_name);
  const auto serial_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - serial_start);
  std::printf("build: %.3f s total\n", static_cast<double>(serial_elapsed.count()) / 1000.0);

  const double coverage_pct = 100.0 * static_cast<double>(manifest.covered_flops) /
                              static_cast<double>(manifest.total_flops);
  std::printf(
      "flop library: %zu classes, %zu/%zu flops covered (%.4f%%), %llu bytes stored, %zu players, "
      "%s\n",
      manifest.class_count, manifest.covered_flops, manifest.total_flops, coverage_pct,
      static_cast<unsigned long long>(manifest.storage_bytes), manifest.player_count,
      manifest.action_id.name.c_str());
  for (const auto& c : manifest.classes) {
    std::printf("  class board {%d, %d, %d}: %llu rows, %llu iterations, %zu section bytes\n",
                c.canonical_board[0], c.canonical_board[1], c.canonical_board[2],
                static_cast<unsigned long long>(c.stored_rows),
                static_cast<unsigned long long>(c.completed_iterations), c.section_bytes);
  }
  return 0;
}
