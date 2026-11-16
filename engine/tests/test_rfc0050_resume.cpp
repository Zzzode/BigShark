// RFC 0050 D307 native test: resumable river checkpoint numeric
// neutrality and fail-close validation. Real behavioral assertions
// through the kernel and the file codec, no source-text matching. A
// split solve (capture, write, read, resume) must reproduce the
// uninterrupted solve's NashConv, reach-bound e_embed/pure-menu, and
// average-strategy hash bit for bit; every form of checkpoint
// corruption or identity mismatch must throw rather than load.
#include <array>
#include <bs/abstraction.hpp>
#include <bs/eval.hpp>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <stdexcept>
#include <vector>

#include "../benchmarks/rfc0047/cap_embedding.hpp"
#include "../benchmarks/rfc0047/offline_census.hpp"
#include "../benchmarks/rfc0047/policy_hash.hpp"
#include "../benchmarks/rfc0047/reach_bound.hpp"
#include "../benchmarks/rfc0047/rfc0050_stage1.hpp"
#include "../benchmarks/rfc0047/river_solver.hpp"
#include "../benchmarks/rfc0047/river_state_codec.hpp"

#define CHECK(cond)                                                         \
  do {                                                                      \
    if (!(cond)) {                                                          \
      std::printf("CHECK failed: %s (%s:%d)\n", #cond, __FILE__, __LINE__); \
      return false;                                                         \
    }                                                                       \
  } while (0)

using namespace bs::rfc0047;
using bs::abstraction::ActionAbstraction;

namespace {

bs::poker::GameDef shallow_river() {
  bs::poker::GameDef def{};
  def.player_count = 2;
  def.button = 0;
  def.big_blind = 2;
  def.ante = 0;
  def.stacks = {8, 8, 0, 0, 0, 0, 0, 0, 0, 0};
  def.contributions = {2, 2, 0, 0, 0, 0, 0, 0, 0, 0};
  def.pot = 4;
  const std::array<int, 5> board = {bs::cardId("2c"), bs::cardId("3d"), bs::cardId("7h"),
                                    bs::cardId("Js"), bs::cardId("9c")};
  def.board = board;
  def.board_size = 5;
  def.preflop = false;
  def.terminal = bs::poker::TerminalDepth::River;
  return def;
}

RiverSolveConfig config_for(const bs::poker::GameDef& def, std::uint64_t iterations) {
  RiverSolveConfig cfg;
  cfg.def = def;
  cfg.iterations = iterations;
  cfg.threads = 1;
  cfg.linear_averaging = true;
  cfg.rm_plus = true;
  cfg.dcfr = false;
  cfg.action_abstraction = ActionAbstraction::identity(0);
  cfg.tree_limits = offline_census_limits();
  return cfg;
}

bool expect_throw(const std::function<void()>& fn) {
  try {
    fn();
  } catch (const std::runtime_error&) {
    return true;
  }
  return false;
}

bool test_resume_is_bitwise_neutral() {
  const auto def = shallow_river();
  Rfc0050Stage1Domain domain = build_stage1_domain(def, offline_census_limits());
  constexpr std::uint64_t kTotal = 12;
  constexpr std::uint64_t kSplit = 5;
  const std::filesystem::path path =
      std::filesystem::temp_directory_path() / "rfc0050_resume_neutral.bsrc";

  // Uninterrupted reference solve.
  const std::vector<std::uint64_t> cps{kTotal};
  const RiverSolveResult full = solve_river_subgame(config_for(def, kTotal), cps);

  // Split path: solve to the split, checkpoint, reload, resume the remainder.
  RiverResumableState captured = capture_river_state(config_for(def, kSplit));
  CHECK(captured.completed_iterations == kSplit);
  save_river_checkpoint(path, captured, /*rm_plus=*/true, /*linear=*/true, /*dcfr=*/false);
  const std::uint64_t digest = ActionAbstraction::identity(0).id().digest;
  RiverResumableState reloaded = load_river_checkpoint(path, def, digest);
  const RiverSolveResult resumed = resume_river_solve(config_for(def, kTotal), cps, reloaded);

  CHECK(!resumed.stopped_early);
  CHECK(resumed.iterations_completed == kTotal);
  // Hard numeric-neutrality gate: bitwise equality, not a tolerance.
  CHECK(resumed.nash_conv_bb == full.nash_conv_bb);
  CHECK(hash_average_snapshot(resumed.avg_sigma) == hash_average_snapshot(full.avg_sigma));

  RiverSolveResult snap_full = full;
  RiverSolveResult snap_resumed = resumed;
  const ReachBoundResult rb_full =
      reach_bound_from_snapshot(def, snap_full, domain.reference_tree, domain.bounded_tree,
                                domain.subset, CapExtensionCompletion::Uniform);
  const ReachBoundResult rb_resumed =
      reach_bound_from_snapshot(def, snap_resumed, domain.reference_tree, domain.bounded_tree,
                                domain.subset, CapExtensionCompletion::Uniform);
  CHECK(rb_resumed.bounded_nash_conv_bb == rb_full.bounded_nash_conv_bb);
  CHECK(rb_resumed.exact_exploitability_bb == rb_full.exact_exploitability_bb);
  CHECK(pure_menu_reads_zero(rb_resumed.exact_exploitability_bb - rb_resumed.bounded_nash_conv_bb));
  std::filesystem::remove(path);
  return true;
}

bool test_checkpoint_fail_close() {
  const auto def = shallow_river();
  const std::filesystem::path path =
      std::filesystem::temp_directory_path() / "rfc0050_resume_fail.bsrc";
  RiverResumableState captured = capture_river_state(config_for(def, 4));
  save_river_checkpoint(path, captured, true, true, false);
  const std::uint64_t digest = ActionAbstraction::identity(0).id().digest;

  // The well-formed file loads.
  RiverResumableState good = load_river_checkpoint(path, def, digest);
  CHECK(good.completed_iterations == 4);

  auto read_bytes = [&] {
    std::ifstream f(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  };
  auto write_bytes = [&](const std::string& s) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f.write(s.data(), static_cast<std::streamsize>(s.size()));
  };

  // Tamper a payload byte (well inside the file) -> checksum mismatch.
  std::string tampered = read_bytes();
  const std::size_t payload_ish = tampered.size() - 16;
  tampered[payload_ish] =
      static_cast<char>(static_cast<unsigned char>(tampered[payload_ish]) ^ 0xFF);
  write_bytes(tampered);
  CHECK(expect_throw([&] { (void)load_river_checkpoint(path, def, digest); }));

  // Bad magic.
  std::string bad_magic = read_bytes();
  bad_magic[0] = static_cast<char>(static_cast<unsigned char>(bad_magic[0]) ^ 0x01);
  write_bytes(bad_magic);
  CHECK(expect_throw([&] { (void)load_river_checkpoint(path, def, digest); }));

  // Truncation.
  std::string truncated = read_bytes();
  truncated.resize(truncated.size() / 2);
  write_bytes(truncated);
  CHECK(expect_throw([&] { (void)load_river_checkpoint(path, def, digest); }));

  // Wrong expected action digest and wrong GameDef both fail close.
  save_river_checkpoint(path, captured, true, true, false);
  CHECK(expect_throw([&] { (void)load_river_checkpoint(path, def, digest ^ 0xDEADBEEF); }));
  auto other_def = def;
  other_def.pot = def.pot + 2;
  CHECK(expect_throw([&] { (void)load_river_checkpoint(path, other_def, digest); }));
  std::filesystem::remove(path);
  return true;
}

}  // namespace

int main() {
  if (!test_resume_is_bitwise_neutral() || !test_checkpoint_fail_close())
    return 1;
  std::printf("rfc0050_resume: all checks passed\n");
  return 0;
}
