// engine_process.hpp — RFC 0009 W4e engine subprocess manager.
//
// Owns a long-lived bigshark-engine --serve-proto child process and speaks the
// v1 framed Protobuf contract over its stdin/stdout. The process is spawned
// eagerly by start() and lazily respawned after a crash or timeout.
//
// The client links only bigshark_protocol (generated protobuf); it must not
// link bigshark_v1_protocol (which drags in the service, policy, solver, and
// resident layers).
#pragma once

#include <bigshark/engine/v1/engine.pb.h>
#include <sys/types.h>

#include <bs/engine_client/engine_client.hpp>
#include <cstdint>
#include <memory>

namespace bs::engine_client {

namespace pv = ::bigshark::engine::v1;

// How the last round_trip failed, for fallback classification.
enum class TransactError {
  None,
  Timeout,      // read_frame timed out; process was killed
  EndOfStream,  // engine closed stdout (crash or exit)
  MalformedFrame,
  WriteFailed,  // broken pipe or write error (engine likely dead)
  NotRunning,   // round_trip called before start() or after stop()
};

// Manages one bigshark-engine --serve-proto subprocess. Single-threaded
// synchronous use only.
class EngineProcess {
 public:
  explicit EngineProcess(EngineClientConfig config);
  ~EngineProcess();

  EngineProcess(const EngineProcess&) = delete;
  EngineProcess& operator=(const EngineProcess&) = delete;

  // Spawns the engine and performs the capabilities handshake. Tries
  // protocol minor 2 first; on UNSUPPORTED_PROTOCOL falls back to minor 0.
  // Returns false if the engine could not be started or both handshake
  // attempts failed.
  bool start();

  // Sends SIGTERM, waits up to 2 seconds, then SIGKILL. Safe to call on a
  // dead or never-started process.
  void stop();

  // True when the child process is believed alive. A crashed process is
  // detected lazily on the next round_trip.
  bool alive() const noexcept;

  // Sends one envelope and reads the response. Returns nullptr on timeout,
  // EOF, malformed frame, or parse failure. A timeout kills the process (it
  // may be wedged in a solve); the next call to start() respawns it.
  std::unique_ptr<pv::Envelope> round_trip(const pv::Envelope& request);

  // How the last round_trip failed. Cleared to None on a successful call.
  TransactError last_error() const noexcept;

  // The negotiated protocol minor (0 or 2). Undefined before a successful
  // start().
  std::uint32_t negotiated_minor() const noexcept;

  // The capabilities captured during the handshake. Undefined before a
  // successful start().
  const pv::GetCapabilitiesResponse& capabilities() const noexcept;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace bs::engine_client
