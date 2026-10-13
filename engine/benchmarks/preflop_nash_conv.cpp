// RFC 0015: learn information-set responses and confirm their signed gains on
// independent uniform joint deals. This is not an exact NashConv certificate.
// Usage: bigshark-preflop-nash-conv [artifact-path] [deals-per-phase]
// [flops-per-world] [seed] [frontier-table-path] [policy-directory]
#include <bs/equity_frontier.hpp>
#include <bs/postflop_frontier.hpp>
#include <bs/strategy_artifact.hpp>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>

#include "preflop_nash_conv_detail.hpp"

namespace {

std::uint64_t unsigned_argument(const char* value, const char* name) {
  const std::string text(value);
  if (text.empty() || text.find_first_not_of("0123456789") != std::string::npos)
    throw std::invalid_argument(std::string(name) + " must be an unsigned integer");
  std::size_t consumed = 0;
  const auto parsed = std::stoull(text, &consumed);
  if (consumed != text.size())
    throw std::invalid_argument(std::string(name) + " contains trailing characters");
  return parsed;
}

int run(int argc, char** argv) {
  namespace measurement = bs::benchmarks::preflop;
  if (argc > 7)
    throw std::invalid_argument("too many positional arguments");
  const std::string artifact_path =
      argc > 1 ? argv[1] : "artifacts/preflop-profile/preflop-profile.db";
  const std::size_t deals = argc > 2 ? unsigned_argument(argv[2], "deals") : 1000;
  const std::size_t flops = argc > 3 ? unsigned_argument(argv[3], "flops") : 1;
  const std::uint64_t seed = argc > 4 ? unsigned_argument(argv[4], "seed") : 42;
  const std::string frontier_table_path = argc > 5 ? argv[5] : "";
  const std::string policy_dir = argc > 6 ? argv[6] : "";
  if (deals == 0 || flops == 0)
    throw std::invalid_argument("deals and flops must be positive");
  if (!policy_dir.empty() && frontier_table_path.empty())
    throw std::invalid_argument("a policy directory requires a frontier table");

  bs::artifacts::LoadOptions opts;
  opts.max_file_bytes = 1ULL << 33;
  const auto loaded = bs::artifacts::load_artifact(artifact_path, opts);
  if (!loaded.bundle.nseat)
    throw std::invalid_argument("artifact is not a v2/v3 nseat bundle");
  const auto& policy = loaded.bundle.nseat->policy;
  const auto& def = policy.game();
  if (def.terminal != bs::poker::TerminalDepth::Flop || def.board_size != 0)
    throw std::invalid_argument("artifact must be preflop-rooted and flop-terminal");
  // Reconstruct the stored menu contract, not a default identity tree.
  auto action = bs::abstraction::ActionAbstraction(policy.sizes());
  if (action.id() != policy.action_id()) {
    action = bs::abstraction::ActionAbstraction::declared(
        policy.sizes(), bs::abstraction::CoverSeeds::DeclaredOnly);
    if (action.id() != policy.action_id())
      action = bs::abstraction::ActionAbstraction::declared(policy.sizes(),
                                                            bs::abstraction::CoverSeeds::MinAndCap);
  }
  bs::abstraction::require_same_abstraction(action.id(), policy.action_id());
  const bs::tree::AbstractTree tree(def, action);

  bs::gto::EquityFrontierEvaluator equity_frontier;
  std::unique_ptr<bs::gto::NSeatDeclaredFrontierTable> declared_table;
  std::unique_ptr<bs::gto::PostflopFrontierEvaluator> postflop_evaluator;
  std::unique_ptr<bs::gto::OnTheFlyPostflopEvaluator> onthefly_evaluator;
  std::string table_identity;
  const bs::gto::FrontierEvaluator* frontier = &equity_frontier;
  const char* frontier_tag = "exact-equity-all-in-flop";
  if (!frontier_table_path.empty()) {
    declared_table = std::make_unique<bs::gto::NSeatDeclaredFrontierTable>(
        bs::gto::NSeatDeclaredFrontierTable::load(frontier_table_path, &table_identity));
    if (!policy_dir.empty()) {
      onthefly_evaluator = std::make_unique<bs::gto::OnTheFlyPostflopEvaluator>(
          *declared_table, std::filesystem::path(policy_dir));
      frontier = onthefly_evaluator.get();
      frontier_tag = "postflop-onthefly-v1";
    } else {
      postflop_evaluator = std::make_unique<bs::gto::PostflopFrontierEvaluator>(*declared_table);
      frontier = postflop_evaluator.get();
      frontier_tag = "postflop-declared-table-v1";
    }
  }

  std::printf("Preflop frozen deviation-gain estimate (not exact NashConv)\n");
  std::printf("  artifact: %s\n", artifact_path.c_str());
  std::printf("  deals per phase: %zu; flops per world: %zu; seed: %llu\n", deals, flops,
              static_cast<unsigned long long>(seed));
  std::printf("  sampling: uniform joint holes, independent learn/confirmation worlds\n");
  std::printf("  tree nodes: %zu; seats: %zu; profile information sets: %zu\n", tree.size(),
              def.player_count, policy.rows().size());
  std::printf("  frontier: %s\n", frontier_tag);
  if (!frontier_table_path.empty()) {
    std::printf("  table: %s (%zu entries, identity: %s)\n", frontier_table_path.c_str(),
                declared_table->size(), table_identity.c_str());
    std::printf("  table sha256: %s\n",
                bs::artifacts::sha256_file_hex(frontier_table_path).c_str());
  }
  if (!policy_dir.empty())
    std::printf("  fixed policy directory: %s\n", policy_dir.c_str());

  const measurement::ProfileQuery query =
      [&](const bs::solver::InformationKey& key,
          std::span<const bs::poker::Action>) -> std::optional<bs::solver::SeatPolicyRow> {
    const auto found = policy.rows().find(key);
    if (found == policy.rows().end())
      return std::nullopt;
    return found->second;
  };
  const auto start = std::chrono::steady_clock::now();
  const auto result = measurement::measure(tree, query, *frontier, deals, flops, seed);
  const double elapsed =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();

  std::printf("\nConfirmed results (%zu worlds per seat, no dropped samples):\n", deals);
  std::printf("  learn seed: %llu; confirmation seed: %llu\n",
              static_cast<unsigned long long>(result.learn_seed),
              static_cast<unsigned long long>(result.confirm_seed));
  for (std::size_t seat = 0; seat < result.seats.size(); ++seat) {
    const auto& learned = result.learned[seat];
    const auto& confirmed = result.seats[seat];
    const auto& counts = confirmed.counts;
    const double coverage =
        counts.response_queries
            ? 100.0 * static_cast<double>(counts.response_queries - counts.response_misses) /
                  static_cast<double>(counts.response_queries)
            : 100.0;
    std::printf("  seat %zu: profile=%+.6f; frozen deviation=%+.6f; signed gain=%+.6f chips\n",
                seat, confirmed.profile_value, confirmed.deviation_value, confirmed.gain.mean);
    std::printf("    paired SE=%.6f; approximate normal 95%% CI=[%+.6f, %+.6f]\n",
                confirmed.gain.standard_error, confirmed.gain.ci95_low, confirmed.gain.ci95_high);
    std::printf(
        "    learned information sets=%zu; response coverage=%.2f%% (%zu misses/%zu queries)\n",
        learned.actions.size(), coverage, counts.response_misses, counts.response_queries);
    std::printf("    profile fallbacks: learn=%zu/%zu; confirmation paired legs=%zu/%zu\n",
                learned.counts.profile_fallbacks, learned.counts.profile_queries,
                counts.profile_fallbacks, counts.profile_queries);
  }
  const auto& total = result.summed_gain;
  const double seats = static_cast<double>(def.player_count);
  const double bb = static_cast<double>(def.big_blind);
  std::printf("  summed signed deviation gain: %+.6f chips; SE=%.6f; 95%% CI=[%+.6f, %+.6f]\n",
              total.mean, total.standard_error, total.ci95_low, total.ci95_high);
  std::printf("  average deviation gain per seat: %+.6f chips (%+.6f BB); SE=%.6f chips\n",
              total.mean / seats, total.mean / seats / bb, total.standard_error / seats);
  std::printf(
      "  CI measures confirmation sampling uncertainty only; missing responses follow the\n"
      "  original profile. Low gain does not certify equilibrium or response optimality.\n");
  std::printf("  wall time: %.3f s\n", elapsed);
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  try {
    return run(argc, argv);
  } catch (const std::exception& error) {
    std::fprintf(stderr, "measurement failed (no estimate published): %s\n", error.what());
    return 1;
  }
}
