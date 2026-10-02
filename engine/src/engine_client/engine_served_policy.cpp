// engine_served_policy.cpp — RFC 0009 W4e BehaviorPolicy adapter.
//
// Forwards each practice-simulator decision to a long-lived
// bigshark-engine --serve-proto subprocess over the v1 framed Protobuf
// contract. On any error (timeout, crash, malformed response, illegal action)
// falls back to check/call/fold and records the fallback class in
// EngineServedStats. Single-threaded synchronous use only.

#include <algorithm>
#include <bs/engine_client/engine_client.hpp>
#include <chrono>
#include <cstdint>
#include <exception>
#include <string>
#include <utility>
#include <vector>

#include "engine_process.hpp"
#include "hand_state_builder.hpp"
#include "response_mapper.hpp"

namespace bs::engine_client {

namespace {

namespace pv = ::bigshark::engine::v1;

// The operational fallback, matching the host's operationalFallbackDecision:
// check if legal, else call if legal, else fold.
bs::poker::Action operational_fallback(const bs::poker::LegalActions& legal) {
  if (legal.check)
    return {bs::poker::ActionType::Check};
  if (legal.call)
    return {bs::poker::ActionType::Call};
  return {bs::poker::ActionType::Fold};
}

}  // namespace

struct EngineServedPolicy::Impl {
  explicit Impl(EngineClientConfig cfg) : config(std::move(cfg)), process(config) {}

  EngineClientConfig config;
  mutable EngineProcess process;
  mutable EngineServedStats stats;
  mutable bool started = false;

  // Lazily (re)spawns the engine process. Returns true if the process is
  // alive and ready. A failed start is retried on the next decision.
  bool ensure_started() const {
    if (started && process.alive())
      return true;
    const bool was_started = started;
    started = process.start();
    if (started && was_started)
      ++stats.restarts;
    return started;
  }
};

EngineServedPolicy::EngineServedPolicy(EngineClientConfig config)
    : impl_(std::make_unique<Impl>(std::move(config))) {}

EngineServedPolicy::~EngineServedPolicy() = default;

bool EngineServedPolicy::start() {
  impl_->started = impl_->process.start();
  return impl_->started;
}

std::vector<bs::stage6::PolicyAction> EngineServedPolicy::distribution(
    const bs::poker::GameState& state, std::size_t seat, bs::stage6::HoleCards hole,
    const bs::stage6::PolicyContext& context) const {
  using clock = std::chrono::steady_clock;
  const auto t0 = clock::now();
  ++impl_->stats.decisions;

  const bs::poker::LegalActions legal = state.legal();

  // Lazily (re)spawn the engine if needed.
  if (!impl_->ensure_started()) {
    ++impl_->stats.protocol_errors;
    ++impl_->stats.fallbacks;
    return {{operational_fallback(legal), 1.0}};
  }

  try {
    const std::uint64_t seed = context.decision_seed;

    // Build the decision request.
    pv::DecisionRequest request = build_decision_request(
        state, seat, hole, context.hand_log, seed, impl_->config, impl_->process.capabilities());

    // Wrap in an envelope and send.
    pv::Envelope envelope;
    envelope.set_protocol_minor(impl_->process.negotiated_minor());
    envelope.set_request_id("d-" + std::to_string(impl_->stats.decisions));
    *envelope.mutable_decision_request() = std::move(request);

    auto response_envelope = impl_->process.round_trip(envelope);
    if (!response_envelope) {
      switch (impl_->process.last_error()) {
        case TransactError::Timeout:
          ++impl_->stats.timeouts;
          break;
        default:
          ++impl_->stats.protocol_errors;
          break;
      }
      ++impl_->stats.fallbacks;
      return {{operational_fallback(legal), 1.0}};
    }

    if (!response_envelope->has_decision_response()) {
      ++impl_->stats.protocol_errors;
      ++impl_->stats.fallbacks;
      return {{operational_fallback(legal), 1.0}};
    }

    // Classify before mapping: has_error() is the engine-error signal.
    const bool is_engine_error = response_envelope->decision_response().has_error();

    MappedDecision mapped =
        map_decision_response(response_envelope->decision_response(), legal, seed);

    if (mapped.fell_back) {
      if (is_engine_error)
        ++impl_->stats.engine_errors;
      else
        ++impl_->stats.protocol_errors;
      ++impl_->stats.fallbacks;
      return {{mapped.action, 1.0}};
    }

    // Served: record latency.
    const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(clock::now() - t0);
    const std::uint64_t latency_us = static_cast<std::uint64_t>(elapsed.count());
    ++impl_->stats.served;
    impl_->stats.total_latency_us += latency_us;
    if (impl_->stats.served == 1 || latency_us < impl_->stats.min_latency_us)
      impl_->stats.min_latency_us = latency_us;
    if (latency_us > impl_->stats.max_latency_us)
      impl_->stats.max_latency_us = latency_us;

    return {{mapped.action, 1.0}};

  } catch (const std::exception&) {
    ++impl_->stats.protocol_errors;
    ++impl_->stats.fallbacks;
    return {{operational_fallback(legal), 1.0}};
  }
}

EngineServedStats EngineServedPolicy::stats() const {
  return impl_->stats;
}

std::uint32_t EngineServedPolicy::negotiated_minor() const {
  return impl_->process.negotiated_minor();
}

std::string EngineServedPolicy::engine_version() const {
  return impl_->process.capabilities().engine_build_version();
}

bool EngineServedPolicy::resident_pipeline_advertised() const {
  for (int mode : impl_->process.capabilities().solver_modes()) {
    if (mode == pv::SOLVER_MODE_BLUEPRINT)
      return true;
  }
  return false;
}

}  // namespace bs::engine_client
