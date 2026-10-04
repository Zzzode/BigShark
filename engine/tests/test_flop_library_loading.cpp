// test_flop_library_loading.cpp — integration test for --flop-library.
//
// Spawns the real bigshark-engine binary with --flop-library and verifies:
// 1. A valid manifest with dummy .db files starts the engine (the roots fail
//    to load, but the manifest parsing and spec expansion succeed and the
//    engine enters its serve loop).
// 2. A nonexistent directory exits with code 2.
// 3. A missing manifest exits with code 2.
// 4. Invalid JSON exits with code 2.
//
// This test depends on the bigshark-engine target and is registered as a
// CTest integration test.

#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#ifndef ENGINE_PATH
#error "ENGINE_PATH must be defined by CMake"
#endif

namespace {

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
  std::string stderr_output;
};

// Runs the engine with the given args, stdin from /dev/null, captures stderr.
RunResult run_engine(const std::vector<std::string>& args) {
  int stderr_pipe[2];
  if (::pipe(stderr_pipe) != 0)
    return {-1, "pipe failed"};

  const pid_t pid = ::fork();
  if (pid < 0)
    return {-1, "fork failed"};

  if (pid == 0) {
    // Child: redirect stderr to the pipe, stdin from /dev/null, exec.
    ::dup2(stderr_pipe[1], STDERR_FILENO);
    ::close(stderr_pipe[0]);
    ::close(stderr_pipe[1]);
    const int devnull = ::open("/dev/null", O_RDONLY);
    if (devnull >= 0) {
      ::dup2(devnull, STDIN_FILENO);
      ::close(devnull);
    }
    std::vector<char*> argv;
    argv.push_back(const_cast<char*>(ENGINE_PATH));
    for (const auto& arg : args)
      argv.push_back(const_cast<char*>(arg.c_str()));
    argv.push_back(nullptr);
    ::execv(ENGINE_PATH, argv.data());
    _exit(127);
  }

  // Parent: read stderr, wait for the child.
  ::close(stderr_pipe[1]);
  std::string output;
  char buf[4096];
  ssize_t n;
  while ((n = ::read(stderr_pipe[0], buf, sizeof(buf))) > 0)
    output.append(buf, static_cast<std::size_t>(n));
  ::close(stderr_pipe[0]);

  int status = 0;
  ::waitpid(pid, &status, 0);
  const int exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
  return {exit_code, output};
}

std::filesystem::path make_temp_dir() {
  const auto base = std::filesystem::temp_directory_path() / "bs-flop-lib-test-XXXXXX";
  std::string tmpl = base.string();
  char* result = ::mkdtemp(tmpl.data());
  if (!result)
    return {};
  return {result};
}

void write_file(const std::filesystem::path& path, const std::string& content) {
  std::ofstream f(path, std::ios::binary);
  f << content;
}

// A 64-lowercase-hex SHA-256 digest (all zeros — valid format, dummy value).
constexpr const char* kDummySha =
    "0000000000000000000000000000000000000000000000000000000000000000";

// Writes a minimal valid manifest with `count` dummy class entries and
// creates empty .db files for each. Returns the directory path.
std::filesystem::path make_dummy_library(std::size_t count) {
  const auto dir = make_temp_dir();
  if (dir.empty())
    return {};
  std::string manifest = R"({"classes":[)";
  for (std::size_t i = 0; i < count; ++i) {
    if (i > 0)
      manifest += ',';
    const std::string name = "class-" + std::to_string(i) + ".db";
    manifest += R"({"artifact_name":")" + name + R"(","sha256_hex":")" + kDummySha + R"("})";
    // Create an empty dummy .db file so the path exists (the load will fail
    // because it is not a valid artifact, but the manifest expansion succeeds).
    write_file(dir / name, "");
  }
  manifest += "]}";
  write_file(dir / "manifest.json", manifest);
  return dir;
}

// ---- Valid manifest: engine starts, roots reported, exits on EOF ----------

void test_valid_manifest_starts_engine() {
  const auto dir = make_dummy_library(2);
  CHECK(!dir.empty());

  const RunResult result = run_engine({
      "--flop-library",
      dir.string(),
      "--serve-proto",
  });

  // The engine must start successfully (exit 0 on stdin EOF).
  CHECK(result.exit_code == 0);

  // Both dummy roots must be reported on stderr with their expanded paths.
  // The path may be quoted by the platform's path operator<<, so search for
  // the path string itself rather than the full line prefix.
  const std::string path0 = (dir / "class-0.db").string();
  const std::string path1 = (dir / "class-1.db").string();
  CHECK(result.stderr_output.find(path0) != std::string::npos);
  CHECK(result.stderr_output.find(path1) != std::string::npos);
  CHECK(result.stderr_output.find("resident-root") != std::string::npos);

  std::filesystem::remove_all(dir);
}

// ---- Nonexistent directory: exit 2 ----------------------------------------

void test_nonexistent_directory() {
  const RunResult result = run_engine({
      "--flop-library",
      "/nonexistent/path/that/does/not/exist",
      "--serve-proto",
  });
  CHECK(result.exit_code == 2);
  CHECK(result.stderr_output.find("not a directory") != std::string::npos);
}

// ---- Missing manifest: exit 2 ---------------------------------------------

void test_missing_manifest() {
  const auto dir = make_temp_dir();
  CHECK(!dir.empty());

  const RunResult result = run_engine({
      "--flop-library",
      dir.string(),
      "--serve-proto",
  });
  CHECK(result.exit_code == 2);
  CHECK(result.stderr_output.find("cannot open manifest") != std::string::npos);

  std::filesystem::remove_all(dir);
}

// ---- Invalid JSON: exit 2 -------------------------------------------------

void test_invalid_json() {
  const auto dir = make_temp_dir();
  CHECK(!dir.empty());
  write_file(dir / "manifest.json", "{not valid json");

  const RunResult result = run_engine({
      "--flop-library",
      dir.string(),
      "--serve-proto",
  });
  CHECK(result.exit_code == 2);
  CHECK(result.stderr_output.find("not valid JSON") != std::string::npos);

  std::filesystem::remove_all(dir);
}

// ---- Empty classes array: exit 2 ------------------------------------------

void test_empty_classes() {
  const auto dir = make_temp_dir();
  CHECK(!dir.empty());
  write_file(dir / "manifest.json", R"({"classes":[]})");

  const RunResult result = run_engine({
      "--flop-library",
      dir.string(),
      "--serve-proto",
  });
  CHECK(result.exit_code == 2);
  CHECK(result.stderr_output.find("no non-empty classes array") != std::string::npos);

  std::filesystem::remove_all(dir);
}

// ---- Bad sha256_hex: exit 2 ------------------------------------------------

void test_bad_sha256() {
  const auto dir = make_temp_dir();
  CHECK(!dir.empty());
  write_file(dir / "manifest.json", R"({"classes":[{"artifact_name":"x.db","sha256_hex":"zz"}]})");
  write_file(dir / "x.db", "");

  const RunResult result = run_engine({
      "--flop-library",
      dir.string(),
      "--serve-proto",
  });
  CHECK(result.exit_code == 2);
  CHECK(result.stderr_output.find("invalid sha256_hex") != std::string::npos);

  std::filesystem::remove_all(dir);
}

// ---- Resident budget: valid value starts engine, reports budget -------------

void test_resident_budget_starts_engine() {
  const auto dir = make_dummy_library(2);
  CHECK(!dir.empty());

  const RunResult result = run_engine({
      "--flop-library",
      dir.string(),
      "--resident-budget",
      "6144",
      "--serve-proto",
  });
  CHECK(result.exit_code == 0);
  CHECK(result.stderr_output.find("resident-budget 6144 MiB") != std::string::npos);

  std::filesystem::remove_all(dir);
}

// ---- Resident budget: invalid values exit 2 ---------------------------------

void test_resident_budget_invalid() {
  // Non-numeric value.
  CHECK(run_engine({"--resident-budget", "abc", "--serve-proto"}).exit_code == 2);
  // Zero.
  CHECK(run_engine({"--resident-budget", "0", "--serve-proto"}).exit_code == 2);
  // Missing value (must include --serve-proto so the proto branch parses it).
  CHECK(run_engine({"--resident-budget", "--serve-proto"}).exit_code == 2);
  // Overflow (far beyond size_t max in bytes).
  CHECK(run_engine({"--resident-budget", "99999999999999999999", "--serve-proto"}).exit_code == 2);
}

// ---- Resident budget: default (no flag) still works --------------------------

void test_resident_budget_default() {
  const auto dir = make_dummy_library(1);
  CHECK(!dir.empty());

  // No --resident-budget flag: the engine uses its 256 MiB default and still
  // starts. The budget line is always printed when roots are loaded.
  const RunResult result = run_engine({
      "--flop-library",
      dir.string(),
      "--serve-proto",
  });
  CHECK(result.exit_code == 0);
  CHECK(result.stderr_output.find("resident-budget 256 MiB") != std::string::npos);

  std::filesystem::remove_all(dir);
}

}  // namespace

int main() {
  test_valid_manifest_starts_engine();
  test_nonexistent_directory();
  test_missing_manifest();
  test_invalid_json();
  test_empty_classes();
  test_bad_sha256();
  test_resident_budget_starts_engine();
  test_resident_budget_invalid();
  test_resident_budget_default();

  if (failures > 0) {
    std::fprintf(stderr, "%d check(s) failed\n", failures);
    return 1;
  }
  std::fprintf(stderr, "all flop library loading tests passed\n");
  return 0;
}
