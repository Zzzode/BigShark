// Integration test: the --raise-cap flag is wired in both offline builders
// and the cap value is recorded in the published manifest.
//
// Spawns the real bigshark-preflop-profile-builder and
// bigshark-flop-library-builder binaries with --raise-cap 1 and minimal
// iterations, then reads the manifest JSON to verify the cap is recorded.
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#ifndef PREFLOP_BUILDER_PATH
#error "PREFLOP_BUILDER_PATH must be defined by CMake"
#endif

#ifndef FLOP_BUILDER_PATH
#error "FLOP_BUILDER_PATH must be defined by CMake"
#endif

namespace {

namespace fs = std::filesystem;

int failures = 0;

#define CHECK(cond)                                                        \
  do {                                                                     \
    if (!(cond)) {                                                         \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      ++failures;                                                          \
    }                                                                      \
  } while (0)

struct RunResult {
  int exit_code = -1;
  std::string stdout_output;
};

// Runs a binary with the given args, captures stdout.
RunResult run_binary(const char* path, const std::vector<std::string>& args) {
  int stdout_pipe[2];
  if (::pipe(stdout_pipe) != 0)
    return {-1, "pipe failed"};

  const pid_t pid = ::fork();
  if (pid < 0)
    return {-1, "fork failed"};

  if (pid == 0) {
    // Child: redirect stdout to the pipe, stderr to /dev/null.
    ::dup2(stdout_pipe[1], STDOUT_FILENO);
    ::close(stdout_pipe[0]);
    ::close(stdout_pipe[1]);
    const int devnull = ::open("/dev/null", O_WRONLY);
    if (devnull >= 0) {
      ::dup2(devnull, STDERR_FILENO);
      ::close(devnull);
    }
    std::vector<char*> argv;
    argv.push_back(const_cast<char*>(path));
    for (const auto& arg : args)
      argv.push_back(const_cast<char*>(arg.c_str()));
    argv.push_back(nullptr);
    ::execv(path, argv.data());
    _exit(127);
  }

  // Parent: read stdout, wait for the child.
  ::close(stdout_pipe[1]);
  std::string output;
  char buf[4096];
  ssize_t n;
  while ((n = ::read(stdout_pipe[0], buf, sizeof(buf))) > 0)
    output.append(buf, static_cast<std::size_t>(n));
  ::close(stdout_pipe[0]);

  int status = 0;
  if (::waitpid(pid, &status, 0) < 0)
    return {-1, "waitpid failed"};
  if (!WIFEXITED(status))
    return {-1, "child did not exit normally"};
  return {WEXITSTATUS(status), std::move(output)};
}

std::string read_file(const fs::path& path) {
  std::ifstream f(path);
  if (!f.good())
    return "";
  return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

bool test_preflop_builder_raise_cap(const fs::path& dir) {
  // 1 iteration, 10 BB, 2 seats, identity abstraction, raise-cap 1.
  const RunResult result = run_binary(
      PREFLOP_BUILDER_PATH, {dir.string(), "1", "10", "2", "identity", "--raise-cap", "1"});
  CHECK(result.exit_code == 0);

  const std::string manifest = read_file(dir / "manifest.json");
  CHECK(!manifest.empty());
  CHECK(manifest.find("\"max_raises_per_street\": 1") != std::string::npos);
  return failures == 0;
}

bool test_flop_builder_raise_cap(const fs::path& dir) {
  // 1 class, 10 iterations, 2 seats, raise-cap 1.
  const RunResult result = run_binary(
      FLOP_BUILDER_PATH, {dir.string(), "1", "10", "2", "4", "2", "identity", "--raise-cap", "1"});
  CHECK(result.exit_code == 0);

  const std::string manifest = read_file(dir / "manifest.json");
  CHECK(!manifest.empty());
  CHECK(manifest.find("\"max_raises_per_street\": 1") != std::string::npos);
  return failures == 0;
}

}  // namespace

int main() {
  const fs::path dir = fs::temp_directory_path() / "bs-builder-raise-cap-test";
  std::error_code ec;
  fs::remove_all(dir, ec);
  fs::create_directories(dir, ec);

  test_preflop_builder_raise_cap(dir / "preflop");
  test_flop_builder_raise_cap(dir / "flop");

  fs::remove_all(dir, ec);
  if (failures > 0) {
    std::printf("builder_raise_cap: %d check(s) FAILED\n", failures);
    return 1;
  }
  std::printf("builder_raise_cap: all tests passed\n");
  return 0;
}
