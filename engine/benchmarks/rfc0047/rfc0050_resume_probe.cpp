// RFC 0050 D307 manual resume-checkpoint cost and neutrality probe
// (NOT a CTest). On the real exact cap-0 rank-0 anchor it captures a
// short solve, times the checkpoint write and reload against the
// ~6 s/iteration solve cost, reports bytes and the iteration-cost
// ratio with the 5% pre-registered gate, then resumes to a longer total
// and asserts bitwise equality with the uninterrupted solve. No argv;
// the iteration counts and 5% threshold are hard-coded.
//
// Usage: bigshark-rfc0050-resume-probe
#include <chrono>
#include <cstdio>
#include <filesystem>

#include "ledger_census.hpp"
#include "offline_census.hpp"
#include "policy_hash.hpp"
#include "rfc0050_anchors.hpp"
#include "river_solver.hpp"
#include "river_state_codec.hpp"

using namespace bs::rfc0047;
using bs::abstraction::ActionAbstraction;

namespace {

constexpr std::uint64_t kSplit = 6;
constexpr std::uint64_t kTotal = 12;
constexpr double kWriteCostGateFraction = 0.05;  // D307: 5% of one iteration
constexpr double kReferenceIterationMs = 6000.0;

const LedgerAnchor& rank_zero_anchor(const std::vector<LedgerAnchor>& anchors) {
  const AnchoredLedger& wanted = rfc0050_six_anchors().front();
  for (const LedgerAnchor& a : anchors)
    if (a.family == std::string(wanted.family) && a.key == wanted.key)
      return a;
  throw std::runtime_error("rfc0050 resume: rank-0 anchor missing");
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

double now_ms() {
  return std::chrono::duration<double, std::milli>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

}  // namespace

int main() {
  const std::vector<LedgerAnchor> anchors = enumerate_ledger_anchors();
  const LedgerAnchor& anchor = rank_zero_anchor(anchors);
  const auto& def = anchor.representative.river_def;
  const std::filesystem::path path =
      std::filesystem::temp_directory_path() / "rfc0050_resume_probe.bsrc";

  const RiverResumableState captured = capture_river_state(config_for(def, kSplit));

  // Pure in-memory serialization (no I/O): the unavoidable copy floor.
  const double s0 = now_ms();
  const std::string blob = serialize_river_checkpoint(captured, true, true, false);
  const double serialize_ms = now_ms() - s0;

  // Disk write of the same bytes on top of serialization.
  const double t0 = now_ms();
  save_river_checkpoint(path, captured, true, true, false);
  const double write_ms = now_ms() - t0;
  const std::uintmax_t file_bytes = std::filesystem::file_size(path);
  const std::uint64_t payload_bytes = river_checkpoint_payload_bytes(captured);

  const double t1 = now_ms();
  const RiverResumableState reloaded =
      load_river_checkpoint(path, def, ActionAbstraction::identity(0).id().digest);
  const double load_ms = now_ms() - t1;

  const std::vector<std::uint64_t> checkpoints{kTotal};
  const RiverSolveResult full = solve_river_subgame(config_for(def, kTotal), checkpoints);
  const RiverSolveResult resumed =
      resume_river_solve(config_for(def, kTotal), checkpoints, reloaded);
  const bool neutral =
      resumed.nash_conv_bb == full.nash_conv_bb &&
      hash_average_snapshot(resumed.avg_sigma) == hash_average_snapshot(full.avg_sigma);
  if (!neutral) {
    std::fprintf(stderr, "neutrality failure: resumed NC %.10g vs full %.10g\n",
                 resumed.nash_conv_bb, full.nash_conv_bb);
    std::filesystem::remove(path);
    return 5;
  }

  const double write_fraction = write_ms / kReferenceIterationMs;
  const double serialize_fraction = serialize_ms / kReferenceIterationMs;
  const bool within_gate = write_fraction <= kWriteCostGateFraction;
  std::printf(
      "metric,value\n"
      "split_iterations,%llu\n"
      "total_iterations,%llu\n"
      "payload_bytes,%llu\n"
      "file_bytes,%llu\n"
      "serialize_ms,%.3f\n"
      "write_ms,%.3f\n"
      "load_ms,%.3f\n"
      "reference_iteration_ms,%.1f\n"
      "serialize_cost_fraction,%.6f\n"
      "write_cost_fraction,%.6f\n"
      "write_gate_fraction,%.2f\n"
      "within_write_gate,%d\n"
      "nash_conv_bb,%.8f\n"
      "resume_neutral_bitwise,%d\n",
      static_cast<unsigned long long>(kSplit), static_cast<unsigned long long>(kTotal),
      static_cast<unsigned long long>(payload_bytes), static_cast<unsigned long long>(file_bytes),
      serialize_ms, write_ms, load_ms, kReferenceIterationMs, serialize_fraction, write_fraction,
      kWriteCostGateFraction, within_gate ? 1 : 0, full.nash_conv_bb, neutral ? 1 : 0);
  std::filesystem::remove(path);
  return within_gate ? 0 : 4;  // distinct RC if the cost gate fails
}
