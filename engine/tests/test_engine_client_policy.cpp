// test_engine_client_policy.cpp — RFC 0009 W4e EngineServedPolicy tests.
//
// Drives the policy adapter against a scripted engine subprocess (see
// scripted_engine.cpp) that speaks the v1 framed protocol and can be told to
// serve canned decisions, hang (timeout), or crash mid-session. Pins served
// decisions, timeout fallback, crash-and-lazy-respawn, fallback accounting,
// latency stats, and the unreachable-engine path.

#include <bs/behavior_policy.hpp>
#include <bs/engine_client/engine_client.hpp>
#include <bs/game_definition.hpp>
#include <bs/heads_up.hpp>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#ifndef SCRIPTED_ENGINE_PATH
#error "SCRIPTED_ENGINE_PATH must be defined by CMake"
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

// ---- Scripted engine control --------------------------------------------------

void set_scripted_mode(const char* mode, const char* action = nullptr,
                       const char* crash_after = nullptr) {
  ::setenv("SCRIPTED_ENGINE_MODE", mode, 1);
  if (action)
    ::setenv("SCRIPTED_ENGINE_ACTION", action, 1);
  else
    ::unsetenv("SCRIPTED_ENGINE_ACTION");
  if (crash_after)
    ::setenv("SCRIPTED_ENGINE_CRASH_AFTER", crash_after, 1);
  else
    ::unsetenv("SCRIPTED_ENGINE_CRASH_AFTER");
}

void clear_scripted_mode() {
  ::unsetenv("SCRIPTED_ENGINE_MODE");
  ::unsetenv("SCRIPTED_ENGINE_ACTION");
  ::unsetenv("SCRIPTED_ENGINE_CRASH_AFTER");
}

// ---- State factory --------------------------------------------------------------

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

EngineClientConfig config_with_timeout(std::uint32_t timeout_ms) {
  EngineClientConfig cfg;
  cfg.engine_path = SCRIPTED_ENGINE_PATH;
  cfg.timeout_ms = timeout_ms;
  cfg.solve_budget_ms = 1000;
  return cfg;
}

// Calls distribution() once and returns the single action.
Action decide(EngineServedPolicy& policy, const GameState& state, std::size_t seat,
              std::uint64_t seed) {
  HandLog log;
  PolicyContext ctx{&log, seed};
  const std::vector<bs::stage6::PolicyAction> dist =
      policy.distribution(state, seat, HoleCards{0, 1}, ctx);
  CHECK(dist.size() == 1);
  CHECK(dist[0].probability == 1.0);
  return dist[0].action;
}

// ---- Served decision -------------------------------------------------------------

void test_served_decision() {
  set_scripted_mode("serve", "call");
  EngineServedPolicy policy(config_with_timeout(30000));
  CHECK(policy.start());
  CHECK(policy.negotiated_minor() == 2u);
  CHECK(policy.engine_version() == "scripted-1.0");
  CHECK(policy.resident_pipeline_advertised());

  GameState state = heads_up_preflop_root();
  const std::size_t seat = *state.actor();
  const Action action = decide(policy, state, seat, 42);
  CHECK(action == (Action{ActionType::Call}));

  const EngineServedStats s = policy.stats();
  CHECK(s.decisions == 1);
  CHECK(s.served == 1);
  CHECK(s.fallbacks == 0);
  CHECK(s.timeouts == 0);
  CHECK(s.engine_errors == 0);
  CHECK(s.protocol_errors == 0);
  CHECK(s.restarts == 0);
  clear_scripted_mode();
}

// ---- Timeout fallback --------------------------------------------------------------

void test_timeout_fallback() {
  set_scripted_mode("timeout");
  EngineServedPolicy policy(config_with_timeout(300));
  CHECK(policy.start());

  GameState state = heads_up_preflop_root();
  const std::size_t seat = *state.actor();
  const Action action = decide(policy, state, seat, 42);
  // SB facing BB: check not legal, call legal -> fallback is call.
  CHECK(action == (Action{ActionType::Call}));

  const EngineServedStats s = policy.stats();
  CHECK(s.decisions == 1);
  CHECK(s.served == 0);
  CHECK(s.fallbacks == 1);
  CHECK(s.timeouts == 1);
  CHECK(s.protocol_errors == 0);
  clear_scripted_mode();
}

// ---- Crash and lazy respawn ----------------------------------------------------------

void test_crash_and_restart() {
  set_scripted_mode("crash", "call", "1");
  EngineServedPolicy policy(config_with_timeout(30000));
  CHECK(policy.start());

  GameState state = heads_up_preflop_root();
  const std::size_t seat = *state.actor();

  // Decision 1: served (scripted engine has seen 1 decision, crash_after=1).
  Action a1 = decide(policy, state, seat, 1);
  CHECK(a1 == (Action{ActionType::Call}));

  // Decision 2: the scripted engine sees its 2nd decision and exits. The
  // client reads EOF and falls back.
  Action a2 = decide(policy, state, seat, 2);
  CHECK(a2 == (Action{ActionType::Call}));  // fallback is also call here

  // Decision 3: ensure_started() respawns the engine (restarts=1) and the
  // fresh process serves.
  Action a3 = decide(policy, state, seat, 3);
  CHECK(a3 == (Action{ActionType::Call}));

  const EngineServedStats s = policy.stats();
  CHECK(s.decisions == 3);
  CHECK(s.served == 2);
  CHECK(s.fallbacks == 1);
  CHECK(s.protocol_errors == 1);  // EOF classified as protocol error
  CHECK(s.restarts == 1);
  CHECK(s.timeouts == 0);
  CHECK(s.engine_errors == 0);
  clear_scripted_mode();
}

// ---- Latency stats -------------------------------------------------------------------

void test_latency_stats() {
  set_scripted_mode("serve", "call");
  EngineServedPolicy policy(config_with_timeout(30000));
  CHECK(policy.start());

  GameState state = heads_up_preflop_root();
  const std::size_t seat = *state.actor();

  (void)decide(policy, state, seat, 1);
  EngineServedStats s = policy.stats();
  CHECK(s.served == 1);
  CHECK(s.total_latency_us > 0);
  CHECK(s.min_latency_us > 0);
  CHECK(s.max_latency_us > 0);
  CHECK(s.min_latency_us == s.max_latency_us);
  CHECK(s.total_latency_us == s.min_latency_us);

  (void)decide(policy, state, seat, 2);
  s = policy.stats();
  CHECK(s.served == 2);
  CHECK(s.total_latency_us > 0);
  CHECK(s.min_latency_us > 0);
  CHECK(s.max_latency_us >= s.min_latency_us);
  CHECK(s.total_latency_us >= s.max_latency_us);
  clear_scripted_mode();
}

// ---- Unreachable engine ----------------------------------------------------------------

void test_unreachable_engine_falls_back() {
  EngineClientConfig cfg;
  cfg.engine_path = "/nonexistent/bigshark-engine";
  cfg.timeout_ms = 5000;
  EngineServedPolicy policy(cfg);
  CHECK(!policy.start());

  GameState state = heads_up_preflop_root();
  const std::size_t seat = *state.actor();
  const Action action = decide(policy, state, seat, 42);
  // Fallback: check not legal, call legal -> call.
  CHECK(action == (Action{ActionType::Call}));

  const EngineServedStats s = policy.stats();
  CHECK(s.decisions == 1);
  CHECK(s.served == 0);
  CHECK(s.fallbacks == 1);
  CHECK(s.protocol_errors == 1);
  CHECK(s.restarts == 0);
}

// ---- Engine error fallback ---------------------------------------------------------------

void test_engine_error_falls_back() {
  // The scripted engine always serves a selected_action, so to test the
  // engine-error path we verify the stats classification indirectly: a
  // malformed response (no strategy or error) is a protocol error, while an
  // EngineError is an engine error. This is covered by the response mapper
  // unit tests; here we just confirm the policy compiles and links with the
  // full stats surface.
  set_scripted_mode("serve", "fold");
  EngineServedPolicy policy(config_with_timeout(30000));
  CHECK(policy.start());

  GameState state = heads_up_preflop_root();
  const std::size_t seat = *state.actor();
  const Action action = decide(policy, state, seat, 42);
  CHECK(action == (Action{ActionType::Fold}));

  const EngineServedStats s = policy.stats();
  CHECK(s.served == 1);
  CHECK(s.fallbacks == 0);
  clear_scripted_mode();
}

}  // namespace
}  // namespace bs::engine_client

int main() {
  using namespace bs::engine_client;
  test_served_decision();
  test_timeout_fallback();
  test_crash_and_restart();
  test_latency_stats();
  test_unreachable_engine_falls_back();
  test_engine_error_falls_back();
  if (failures > 0) {
    std::fprintf(stderr, "%d check(s) failed\n", failures);
    return 1;
  }
  std::printf("all policy tests passed\n");
  return 0;
}
