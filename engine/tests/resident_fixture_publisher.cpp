// resident_fixture_publisher.cpp — offline tool for the RFC 0002 Stage 8
// real-binary matrix tests. Trains a tiny deterministic complete postflop
// game whose root has SIX abstract actions (check plus five bet sizes),
// publishes one complete immutable blueprint artifact into the target
// directory, and prints a single JSON line on stdout:
//
//   {"path":"...","sha256":"...","root":{...},"ranges":[[...],[...]]}
//
// The TypeScript differential test parses that line, spawns the real engine
// with --resident-root path=sha256, and builds requests matching the printed
// root. The tool is test infrastructure; production never ships blueprints.
#include <algorithm>
#include <array>
#include <bs/eval.hpp>
#include <bs/heads_up.hpp>
#include <bs/heads_up_solver.hpp>
#include <bs/strategy_artifact.hpp>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>

#include "artifacts/artifact_internal.hpp"
#include "gto/heads_up_solver_debug.hpp"

namespace {

using bs::artifacts::detail::PolicyAssembler;
using namespace bs::poker;
using namespace bs::solver;
using namespace bs::artifacts;

int card(const char* name) {
  return bs::cardId(std::string(name));
}

TrainingLimits fastLimits() {
  TrainingLimits limits;
  limits.max_nodes = 500000000;
  limits.max_information_sets = 5000000;
  limits.max_bytes = std::size_t{4} << 30;
  limits.time = std::chrono::minutes{10};
  return limits;
}

// Deeper SPR root chosen so the default sizing schedule produces six root
// actions (check plus bet totals 5,7,15,30,40), which proves the minor-1
// distribution is not truncated to the minor-0 five-entry cap.
HeadsUpGame fixtureGame() {
  HeadsUpGame game;
  game.root = {{card("2c"), card("3d"), card("7h")}, {40, 40}, {10, 10}, 20, 5, 1};
  game.ranges[0] = {{{card("As"), card("Ks")}, 2}, {{card("Qh"), card("Jh")}, 3}};
  game.ranges[1] = {{{card("Ac"), card("Kc")}, 5}, {{card("Qd"), card("Jd")}, 7}};
  game.fixed_runout = {card("9h"), card("8s")};
  for (auto& range : game.ranges)
    for (auto& hand : range)
      std::sort(hand.cards.begin(), hand.cards.end());
  return game;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    std::fprintf(stderr, "usage: bs-resident-fixture <output-directory>\n");
    return 2;
  }
  const std::filesystem::path dir(argv[1]);
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  const std::filesystem::path policyPath = dir / "resident-fixture-policy.db";
  std::filesystem::remove(policyPath, ec);
  std::filesystem::remove(dir / "resident-fixture-checkpoint.db", ec);

  const HeadsUpGame game = fixtureGame();

  // Sanity: the root really offers six abstract actions, so a hit exercises
  // the >5 expanded distribution on the wire.
  const std::vector<Action> rootActions = abstract_actions(HeadsUpState(game.root), game.sizes);
  if (rootActions.size() <= 5) {
    std::fprintf(stderr, "fixture root offers only %zu actions; need more than five\n",
                 rootActions.size());
    return 1;
  }

  const DebugTrainingOutput trained = HeadsUpSolverDebug::train_full(game, 1, fastLimits());
  if (trained.result.status != TrainingStatus::Complete) {
    std::fprintf(stderr, "fixture training did not complete\n");
    return 1;
  }

  HeadsUpPolicy policy;
  PolicyAssembler::set_game(policy, game);
  TrainingRows rawRows;
  for (const auto& [key, trainedRow] : trained.result.policy.rows()) {
    PolicyAssembler::add_row(policy, key, PolicyRow{trainedRow.actions, trainedRow.probabilities});
    if (const auto rawIt = trained.rows.find(key); rawIt != trained.rows.end())
      rawRows.emplace(
          key, TrainingRow{rawIt->second.actions, rawIt->second.regrets, rawIt->second.sums});
  }

  TrainingResult result = trained.result;
  result.policy = std::move(policy);
  const std::filesystem::path checkpoint = dir / "resident-fixture-checkpoint.db";
  CheckpointProvenance provenance;
  provenance.prng_identifier = kFullTraversalPrngIdentifier;
  provenance.engine_revision = "stage8-fixture-engine-1";
  create_checkpoint(checkpoint, result, rawRows, provenance);
  const PublishedPolicy published = publish_policy(checkpoint, policyPath, "stage8-fixture-1");

  std::printf(
      "{\"path\":\"%s\",\"sha256\":\"%s\","
      "\"root\":{\"flop\":[\"2c\",\"3d\",\"7h\"],\"stacks\":[40,40],"
      "\"contributions\":[10,10],\"pot\":20,\"bigBlind\":5,\"button\":1},"
      "\"ranges\":[[[\"As\",\"Ks\"],[\"Qh\",\"Jh\"]],"
      "[[\"Ac\",\"Kc\"],[\"Qd\",\"Jd\"]]],"
      "\"runout\":[\"9h\",\"8s\"],\"rootActionCount\":%zu}\n",
      policyPath.string().c_str(), published.sha256_hex.c_str(), rootActions.size());
  return 0;
}
