// engine_client.hpp — RFC 0009 W4e engine-served practice tier.
//
// A BehaviorPolicy that forwards each decision to a separate
// bigshark-engine --serve-proto subprocess over the v1 framed Protobuf
// contract. This gives the offline practice simulator access to the resident
// blueprint+resolver pipeline (W4d), which is strictly better than the Medium
// bot's heuristic-only policy. On any error (timeout, crash, malformed
// response, illegal action) the policy falls back to check/call/fold and
// records the fallback class.
//
// The client links only bigshark_behavior (the policy interface),
// bigshark_poker (domain types), and bigshark_protocol (generated protobuf).
// It must never link the service, either wire protocol's server side, the
// policy, the solver, or the resident layer. The stage6_offline_guard
// enforces this by construction.
//
// Usage:
//   EngineClientConfig config;
//   config.engine_path = "bin/bigshark-engine";
//   EngineServedPolicy policy(config);
//   if (!policy.start()) { /* engine unreachable; every decision falls back */ }
//   table.set_bot(bot_seat, std::make_unique<EngineServedPolicy>(std::move(policy)));
//
// Single-threaded synchronous use only: the transport and stats are mutable
// members accessed from the const distribution() override.
#pragma once

#include <bs/behavior_policy.hpp>
#include <bs/game_definition.hpp>  // poker::GameState
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace bs::engine_client {

// Configuration for the engine-served policy.
struct EngineClientConfig {
  // Path to the bigshark-engine binary.
  std::string engine_path = "bin/bigshark-engine";
  // Resident root specification: "<path>=<sha256>". Empty means no resident
  // roots are advertised (the engine serves the labeled operational fallback
  // at minor 2).
  std::string resident_root;
  // Per-decision IPC timeout in milliseconds. A timeout kills the engine
  // process; the next decision lazily respawns it.
  std::uint32_t timeout_ms = 30000;
  // Engine solve time budget in milliseconds. The loop is synchronous, so
  // this should be small (default 1000).
  std::uint32_t solve_budget_ms = 1000;
  // Strategy profile name sent to the engine.
  std::string strategy_profile = "tag";
};

// Per-decision latency and fallback accounting. All counters are cumulative
// over the lifetime of the policy.
struct EngineServedStats {
  std::uint64_t decisions = 0;
  std::uint64_t served = 0;
  std::uint64_t fallbacks = 0;
  std::uint64_t timeouts = 0;
  std::uint64_t engine_errors = 0;
  std::uint64_t protocol_errors = 0;
  std::uint64_t restarts = 0;
  std::uint64_t total_latency_us = 0;
  std::uint64_t min_latency_us = 0;
  std::uint64_t max_latency_us = 0;
};

// A BehaviorPolicy that forwards decisions to a bigshark-engine subprocess.
// Falls back to check/call/fold on any error. The process is spawned eagerly
// by start() and lazily respawned after a crash or timeout.
class EngineServedPolicy : public bs::stage6::BehaviorPolicy {
 public:
  explicit EngineServedPolicy(EngineClientConfig config);
  ~EngineServedPolicy() override;

  // Eagerly spawns the engine process and performs the capabilities
  // handshake. Returns false if the engine could not be started or the
  // handshake failed; in that case every decision falls back until a lazy
  // respawn succeeds.
  bool start();

  // BehaviorPolicy interface.
  std::vector<bs::stage6::PolicyAction> distribution(
      const bs::poker::GameState& state, std::size_t seat, bs::stage6::HoleCards hole,
      const bs::stage6::PolicyContext& context) const override;

  // Accessors for the CLI startup/shutdown report.
  EngineServedStats stats() const;
  std::uint32_t negotiated_minor() const;
  std::string engine_version() const;
  bool resident_pipeline_advertised() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace bs::engine_client
