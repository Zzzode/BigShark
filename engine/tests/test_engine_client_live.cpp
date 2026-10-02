// test_engine_client_live.cpp — RFC 0009 W4e live integration test.
//
// Spawns a real bigshark-engine --serve-proto subprocess and exercises the
// full client stack: minor-2 handshake (with minor-0 fallback), one heads-up
// preflop decision, and the fallback path against a bogus engine path. This
// test depends on the bigshark-engine target and is slower than the unit
// tests; it is registered as a CTest integration test.

#include <bs/behavior_policy.hpp>
#include <bs/engine_client/engine_client.hpp>
#include <bs/game_definition.hpp>
#include <bs/heads_up.hpp>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#ifndef ENGINE_PATH
#error "ENGINE_PATH must be defined by CMake"
#endif

namespace bs::engine_client {
namespace {

using bs::poker::Action;
using bs::poker::ActionType;
using bs::poker::GameDef;
using bs::poker::GameState;
using bs::poker::LegalActions;
using bs::stage6::HandLog;
using bs::stage6::HoleCards;
using bs::stage6::PolicyContext;

int failures = 0;

#define CHECK(cond)                                                        \
  do {                                                                     \
    if (!(cond)) {                                                         \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      ++failures;                                                          \
    }                                                                      \
  } while (0)

GameState heads_up_preflop_root() {
  GameDef def{};
  def.player_count = 2;
  def.button = 0;
  def.big_blind = 2;
  def.stacks[0] = 200;
  def.stacks[1] = 200;
  def.blinds_posted[0] = 1;
  def.blinds_posted[1] = 2;
  def.pot = 3;
  def.preflop = true;
  def.board = {-1, -1, -1, -1, -1};
  def.board_size = 0;
  return GameState(def);
}

// ---- Real handshake and decision ---------------------------------------------------

void test_real_handshake_and_decision() {
  EngineClientConfig cfg;
  cfg.engine_path = ENGINE_PATH;
  cfg.timeout_ms = 30000;
  cfg.solve_budget_ms = 1000;
  EngineServedPolicy policy(cfg);

  // The engine must start and complete the capabilities handshake.
  CHECK(policy.start());
  const std::uint32_t minor = policy.negotiated_minor();
  CHECK(minor == 0u || minor == 2u);
  CHECK(!policy.engine_version().empty());

  // One heads-up preflop decision.
  GameState state = heads_up_preflop_root();
  const std::size_t seat = *state.actor();
  HandLog log;
  PolicyContext ctx{&log, 42};
  const std::vector<bs::stage6::PolicyAction> dist =
      policy.distribution(state, seat, HoleCards{0, 1}, ctx);
  CHECK(dist.size() == 1);
  CHECK(dist[0].probability == 1.0);

  // The returned action must be legal for the state. Whether it was served or
  // fell back depends on the engine's resident configuration, but the action
  // must always be legal.
  const LegalActions legal = state.legal();
  CHECK(legal.contains(dist[0].action));

  const EngineServedStats s = policy.stats();
  CHECK(s.decisions == 1);
  CHECK(s.served + s.fallbacks == 1);
  // If served, latency must be recorded.
  if (s.served > 0) {
    CHECK(s.total_latency_us > 0);
    CHECK(s.min_latency_us > 0);
    CHECK(s.max_latency_us > 0);
  }
}

// ---- Bogus engine path falls back ----------------------------------------------------

void test_bogus_engine_path_falls_back() {
  EngineClientConfig cfg;
  cfg.engine_path = "/nonexistent/bigshark-engine";
  cfg.timeout_ms = 5000;
  EngineServedPolicy policy(cfg);

  CHECK(!policy.start());

  GameState state = heads_up_preflop_root();
  const std::size_t seat = *state.actor();
  HandLog log;
  PolicyContext ctx{&log, 42};
  const std::vector<bs::stage6::PolicyAction> dist =
      policy.distribution(state, seat, HoleCards{0, 1}, ctx);
  CHECK(dist.size() == 1);
  CHECK(dist[0].probability == 1.0);

  // Fallback: check not legal (SB faces BB), call legal -> call.
  CHECK(dist[0].action == (Action{ActionType::Call}));

  const EngineServedStats s = policy.stats();
  CHECK(s.decisions == 1);
  CHECK(s.served == 0);
  CHECK(s.fallbacks == 1);
  CHECK(s.protocol_errors == 1);
}

}  // namespace
}  // namespace bs::engine_client

int main() {
  using namespace bs::engine_client;
  test_real_handshake_and_decision();
  test_bogus_engine_path_falls_back();
  if (failures > 0) {
    std::fprintf(stderr, "%d check(s) failed\n", failures);
    return 1;
  }
  std::printf("all live integration tests passed\n");
  return 0;
}
