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
#include <bs/abstraction.hpp>
#include <bs/eval.hpp>
#include <bs/heads_up.hpp>
#include <bs/heads_up_solver.hpp>
#include <bs/strategy_artifact.hpp>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>

#include "artifacts/rekey_fixture.hpp"
#include "gto/heads_up_solver_debug.hpp"

namespace {

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
// actions (check plus bet totals 5,7,15,30,40), which proves the full menu
// is published rather than a truncated five-entry cap.
//
// Button is 0 to match the resident mapper convention: the mapper rotates
// actor indices so the button is always at index 0 before root matching.
// Actor 0 is the button (SB postflop OOP); actor 1 is the BB (postflop IP).
// The ranges are swapped relative to the old button=1 layout so the game
// is strategy-equivalent under the actor rotation.
HeadsUpGame fixtureGame() {
  HeadsUpGame game;
  game.root = {{card("2c"), card("3d"), card("7h")}, {40, 40}, {10, 10}, 20, 5, 0};
  game.ranges[0] = {{{card("Ac"), card("Kc")}, 5}, {{card("Qd"), card("Jd")}, 7}};
  game.ranges[1] = {{{card("As"), card("Ks")}, 2}, {{card("Qh"), card("Jh")}, 3}};
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

  // RFC 0038: the fixed-runout in-memory training is rekeyed into a
  // flop-rooted river-terminal schema-v2 policy (no PolicyAssembler, no v1
  // writer). The information keys are byte-identical between the two forms, so
  // the published rows answer the same hands; the rekey helper stamps a
  // genuine revision-two identity.
  const SeatTrainingResult rekeyed =
      bs::artifacts::detail::rekey_heads_up_result({game, trained.result});

  const std::filesystem::path checkpoint = dir / "resident-fixture-checkpoint.db";
  create_checkpoint(checkpoint, rekeyed,
                    bs::artifacts::detail::rekey_provenance("stage8-fixture-engine-1"));
  const PublishedPolicy published = publish_policy(checkpoint, policyPath, "stage8-fixture-1");

  std::printf(
      "{\"path\":\"%s\",\"sha256\":\"%s\","
      "\"root\":{\"flop\":[\"2c\",\"3d\",\"7h\"],\"stacks\":[40,40],"
      "\"contributions\":[10,10],\"pot\":20,\"bigBlind\":5,\"button\":0},"
      "\"ranges\":[[[\"Ac\",\"Kc\"],[\"Qd\",\"Jd\"]],"
      "[[\"As\",\"Ks\"],[\"Qh\",\"Jh\"]]],"
      "\"runout\":[\"9h\",\"8s\"],\"rootActionCount\":%zu}\n",
      policyPath.string().c_str(), published.sha256_hex.c_str(), rootActions.size());
  return 0;
}
