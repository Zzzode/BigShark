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
#include <openssl/evp.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <bs/flop_library.hpp>
#include <bs/heads_up.hpp>
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

// SHA-256 of a file, lowercase hex (the test links libcrypto through the
// engine's own digest helper by shelling out is avoided: use openssl's EVP).
std::string sha256_of(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  unsigned char digest[EVP_MAX_MD_SIZE];
  unsigned int len = 0;
  EVP_MD_CTX* ctx = EVP_MD_CTX_new();
  EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr);
  EVP_DigestUpdate(ctx, data.data(), data.size());
  EVP_DigestFinal_ex(ctx, digest, &len);
  EVP_MD_CTX_free(ctx);
  std::string hex;
  hex.reserve(64);
  static const char* kHex = "0123456789abcdef";
  for (unsigned int i = 0; i < len; ++i) {
    hex.push_back(kHex[digest[i] >> 4]);
    hex.push_back(kHex[digest[i] & 0xf]);
  }
  return hex;
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

// RFC 0027 AC6: a directory manifest naming library.bin resolves to the packed
// file, is content-verified, and serves. A digest disagreement and a mixed
// binary/resident-root launch both exit 2.
std::filesystem::path make_binary_library(const std::string& sha_hex, bool write_sidecar) {
  const auto dir = make_temp_dir();
  if (dir.empty())
    return {};
  // Minimal valid BSFLIB02 header is not needed: the digest check runs before
  // the mmap parse, so a small file suffices for the pin tests.
  write_file(dir / "library.bin", "BSFLIB02-test-bytes");
  if (write_sidecar)
    write_file(dir / "library.bin.sha256", sha_hex + "  library.bin\n");
  const std::string manifest =
      R"({"library_bin":"library.bin","library_bin_sha256":")" + sha_hex + R"(","classes":[]})";
  write_file(dir / "manifest.json", manifest);
  return dir;
}

void test_binary_manifest_resolution() {
  // Build a real tiny library with the real builder's output contract by
  // copying nothing: only the resolution and pin path is under test here, so
  // the exact bytes are irrelevant as long as the digest matches.
  const std::string dummy = std::string(64, 'a');
  const auto dir = make_binary_library(dummy, true);
  CHECK(!dir.empty());
  // Compute the true digest of the dummy file and pin it.
  const std::string actual = sha256_of(dir / "library.bin");
  write_file(dir / "library.bin.sha256", actual + "  library.bin\n");
  write_file(dir / "manifest.json", R"({"library_bin":"library.bin","library_bin_sha256":")" +
                                        actual + R"(","classes":[]})");
  const RunResult result = run_engine({"--flop-library", dir.string(), "--serve-proto"});
  // The digest matches, so the host reaches the mmap open. The dummy bytes are
  // not a valid BSFLIB02 file, so opening fails and no root is advertised;
  // what matters here is that the error is the mmap error, not a config error.
  CHECK(result.exit_code == 0);
  CHECK(result.stderr_output.find("flop-library:") == std::string::npos);
  std::filesystem::remove_all(dir);
}

void test_binary_manifest_digest_mismatch() {
  const std::string dummy = std::string(64, 'b');
  const auto dir = make_binary_library(dummy, true);
  CHECK(!dir.empty());
  const RunResult result = run_engine({"--flop-library", dir.string(), "--serve-proto"});
  CHECK(result.exit_code == 2);
  CHECK(result.stderr_output.find("disagrees with manifest") != std::string::npos);
  std::filesystem::remove_all(dir);
}

void test_binary_cannot_mix_with_resident_root() {
  const std::string dummy = std::string(64, 'c');
  const auto dir = make_binary_library(dummy, true);
  CHECK(!dir.empty());
  const std::string pin = dummy + "=" + dummy;
  const RunResult result =
      run_engine({"--flop-library", dir.string(), "--resident-root", pin, "--serve-proto"});
  CHECK(result.exit_code == 2);
  CHECK(result.stderr_output.find("cannot be combined") != std::string::npos);
  std::filesystem::remove_all(dir);
}

}  // namespace

// RFC 0035: the startup lines name every root shape the library answers, one
// per served_roots[] entry. An operator who sees a class count must not infer
// that every flop state is served, so the server says which ledgers it answers.
void test_served_root_reported() {
  const auto dir = make_temp_dir();
  CHECK(!dir.empty());
  write_file(dir / "library.bin", "BSFLIB03-test-bytes");
  const std::string actual = sha256_of(dir / "library.bin");
  write_file(dir / "library.bin.sha256", actual + "  library.bin\n");
  // The dummy .bin does not mount, so the startup report falls back to the
  // manifest arrays and still prints one line per entry.
  const std::string manifest = R"({"library_bin":"library.bin","library_bin_sha256":")" + actual +
                               R"(","classes":[],"served_roots":[)"
                               R"({"root_id":0,"line_tag":"limped","pot":12,)"
                               R"("stacks":[198,198,198,198,198,198],)"
                               R"("contributions":[2,2,2,2,2,2],)"
                               R"("folded":[false,false,false,false,false,false],)"
                               R"("live_count":6,"range_profile":1,"class_count":1755,)"
                               R"("covered_flops":22100,"total_flops":22100,"section_bytes":0},)"
                               R"({"root_id":1,"line_tag":"srp","pot":30,)"
                               R"("stacks":[170,170,170,170,170,170],)"
                               R"("contributions":[6,6,4,4,4,6],)"
                               R"("folded":[false,false,false,false,false,false],)"
                               R"("live_count":6,"range_profile":1,"class_count":1755,)"
                               R"("covered_flops":22100,"total_flops":22100,"section_bytes":0}]})";
  write_file(dir / "manifest.json", manifest);

  const RunResult result = run_engine({"--flop-library", dir.string(), "--serve-proto"});
  CHECK(result.exit_code == 0);
  CHECK(result.stderr_output.find("resident-served-root") != std::string::npos);
  // Exactly two lines, one per root, with the differing ledgers visible.
  CHECK(result.stderr_output.find("root=0 stacks=198,198,198,198,198,198") != std::string::npos);
  CHECK(result.stderr_output.find("contributions=2,2,2,2,2,2") != std::string::npos);
  CHECK(result.stderr_output.find("root=1 stacks=170,170,170,170,170,170") != std::string::npos);
  CHECK(result.stderr_output.find("contributions=6,6,4,4,4,6") != std::string::npos);
  CHECK(result.stderr_output.find("folded=0,0,0,0,0,0") != std::string::npos);

  std::filesystem::remove_all(dir);
}

// RFC 0035: build a REAL tiny BSFLIB03 through the builder, then verify the
// host reports the served root and that a manifest whose served_roots ledger
// disagrees with the mounted directory aborts startup.
void test_served_roots_reconcile_with_directory() {
  const auto dir = make_temp_dir();
  CHECK(!dir.empty());
  const auto lib_dir = dir / "lib";
  const auto range = bs::flop_library::declared_library_range();
  bs::flop_library::LibraryManifest manifest;
  try {
    manifest = bs::flop_library::build_library(lib_dir, /*class_count=*/1, /*iterations=*/8,
                                               /*seed=*/7, /*starting_stack=*/6,
                                               /*contribution=*/2, range, /*player_count=*/2);
  } catch (const std::exception& e) {
    std::fprintf(stderr, "real library build failed: %s\n", e.what());
    CHECK(false);
    std::filesystem::remove_all(dir);
    return;
  }
  CHECK(manifest.served_roots.size() == 1);

  // Matching manifest: the engine mounts the directory, reconciles it, prints
  // the one startup line, and exits cleanly on EOF.
  {
    const RunResult result = run_engine({"--flop-library", lib_dir.string(), "--serve-proto"});
    CHECK(result.exit_code == 0);
    CHECK(result.stderr_output.find("resident-served-root") != std::string::npos);
    CHECK(result.stderr_output.find("root=0") != std::string::npos);
    CHECK(result.stderr_output.find("stacks=4,4") != std::string::npos);
    CHECK(result.stderr_output.find("contributions=2,2") != std::string::npos);
  }

  // Tamper the manifest: claim a pot the directory does not carry. The host
  // mounts the real binary and must abort with the reconciliation message.
  {
    const std::filesystem::path manifest_path = lib_dir / "manifest.json";
    std::ifstream in(manifest_path, std::ios::binary);
    std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const std::string before = "\"root_id\": 0, \"line_tag\": \"limped\", \"pot\": 4";
    const std::string after = "\"root_id\": 0, \"line_tag\": \"limped\", \"pot\": 99";
    CHECK(content.find(before) != std::string::npos);
    const std::size_t pos = content.find(before);
    content.replace(pos, before.size(), after);
    write_file(manifest_path, content);

    const RunResult result = run_engine({"--flop-library", lib_dir.string(), "--serve-proto"});
    CHECK(result.exit_code == 2);
    CHECK(result.stderr_output.find("disagrees with the mounted BSFLIB03 root directory") !=
          std::string::npos);
  }

  std::filesystem::remove_all(dir);
}

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
  test_binary_manifest_resolution();
  test_binary_manifest_digest_mismatch();
  test_binary_cannot_mix_with_resident_root();
  test_served_root_reported();
  test_served_roots_reconcile_with_directory();

  if (failures > 0) {
    std::fprintf(stderr, "%d check(s) failed\n", failures);
    return 1;
  }
  std::fprintf(stderr, "all flop library loading tests passed\n");
  return 0;
}
