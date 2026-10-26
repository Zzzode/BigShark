// practice/engine_process.hpp - C++ framed-engine process client.
//
// Spawns the engine host (bigshark-engine --serve-proto) as a child process
// and communicates via ULEB128-framed Protobuf envelopes. One client
// instance serves all bot seats in a practice session: the same host process
// receives every DecisionRequest, so all five bot decisions come from the
// same loaded artifact library and resolver state.
//
// The client is offline: no network, no credentials, no River Club. It is a
// local training tool that exercises the full framed-protocol pipeline.
#pragma once

#include <sys/types.h>

#include <bs/protocol.hpp>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

// Forward-declare the engine protobuf types so the header stays protobuf-free.
namespace bigshark::engine {
class Envelope;
class GetCapabilitiesResponse;
}  // namespace bigshark::engine

// Short alias for the engine protobuf namespace, used throughout the practice
// engine client.
namespace pe = bigshark::engine;

namespace bs::practice {

// Configuration for spawning the engine host.
struct EngineProcessConfig {
  // Path to the bigshark-engine binary.
  std::string engine_path = "bin/bigshark-engine";
  // Per-decision solve budget in milliseconds.
  std::uint32_t solve_budget_ms = 1000;
  // Round-trip timeout in milliseconds.
  std::uint32_t timeout_ms = 30000;
  // Repeatable resident root specs ("path=sha256-hex").
  std::vector<std::string> resident_roots;
  // Optional flop library directory.
  std::string flop_library;
  // Enable lazy loading for flop class libraries.
  bool lazy = false;
};

// A running engine host child process.
//
// Lifecycle: construct (no process), start() spawns and handshakes, request()
// sends one envelope and returns the correlated response, stop() terminates.
// The destructor calls stop() if the process is still running.
class EngineProcess {
 public:
  explicit EngineProcess(EngineProcessConfig config);
  ~EngineProcess();

  EngineProcess(const EngineProcess&) = delete;
  EngineProcess& operator=(const EngineProcess&) = delete;

  // Spawns the engine host and performs the capabilities handshake.
  // Returns true on success. On failure, the error is available via
  // last_error() and the process is not running.
  bool start();

  // Sends one framed envelope and returns the correlated response envelope.
  // Returns nullptr on timeout, protocol error, or if the process died.
  std::unique_ptr<pe::Envelope> request(const pe::Envelope& request);

  // Terminates the engine host process.
  void stop();

  bool running() const noexcept { return pid_ > 0; }
  const std::string& last_error() const noexcept { return last_error_; }

  // Capabilities from the handshake (empty until start() succeeds).
  const pe::GetCapabilitiesResponse& capabilities() const noexcept;

 private:
  EngineProcessConfig config_;
  std::string last_error_;
  pid_t pid_ = -1;
  int stdin_fd_ = -1;
  int stdout_fd_ = -1;
  std::uint64_t request_counter_ = 0;
  std::unique_ptr<pe::GetCapabilitiesResponse> capabilities_;
};

}  // namespace bs::practice
