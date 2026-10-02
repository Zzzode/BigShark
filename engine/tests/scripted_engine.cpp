// scripted_engine.cpp — a minimal scripted engine for policy adapter tests.
//
// Speaks the v1 framed protobuf protocol on stdin/stdout. Behavior is
// controlled by environment variables so the test can point EngineClientConfig
// at this binary and drive specific failure modes:
//   SCRIPTED_ENGINE_MODE         "serve" (default) | "timeout" | "crash"
//   SCRIPTED_ENGINE_ACTION       "call" (default) | "fold" | "check"
//   SCRIPTED_ENGINE_CRASH_AFTER  int, decisions served before exit (default 1)
//
// This is a test helper, not a production engine. It ignores --serve-proto and
// --resident-root arguments that EngineProcess passes via execv.

#include <bigshark/engine/v1/engine.pb.h>
#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

#include "engine_client/frame_stream.hpp"

namespace pv = ::bigshark::engine::v1;
using bs::engine_client::encode_frame;
using bs::engine_client::FrameStatus;
using bs::engine_client::read_frame;
using bs::engine_client::write_frame;

namespace {

pv::GetCapabilitiesResponse make_caps() {
  pv::GetCapabilitiesResponse caps;
  caps.add_supported_protocol_minors(0);
  caps.add_supported_protocol_minors(1);
  caps.add_supported_protocol_minors(2);
  caps.set_engine_build_version("scripted-1.0");
  caps.add_supported_game_variants(pv::GAME_VARIANT_NLHE);
  caps.add_supported_betting_structures(pv::BETTING_STRUCTURE_NO_LIMIT);
  caps.set_minimum_players(2);
  caps.set_maximum_players(2);
  caps.add_supported_streets(pv::STREET_PREFLOP);
  caps.add_supported_streets(pv::STREET_FLOP);
  caps.add_supported_streets(pv::STREET_TURN);
  caps.add_supported_streets(pv::STREET_RIVER);
  caps.add_supported_actions(pv::ACTION_TYPE_FOLD);
  caps.add_supported_actions(pv::ACTION_TYPE_CHECK);
  caps.add_supported_actions(pv::ACTION_TYPE_CALL);
  caps.add_supported_actions(pv::ACTION_TYPE_BET);
  caps.add_supported_actions(pv::ACTION_TYPE_RAISE);
  caps.set_amount_semantics(pv::AMOUNT_SEMANTICS_TARGET_TOTAL_INCLUSIVE);
  caps.add_solver_modes(pv::SOLVER_MODE_BLUEPRINT);
  caps.add_solver_modes(pv::SOLVER_MODE_AUTOMATIC);
  caps.add_strategy_profiles("tag");
  caps.set_exact_lp(pv::FEATURE_SUPPORT_SUPPORTED);
  caps.set_dcfr(pv::FEATURE_SUPPORT_SUPPORTED);
  caps.set_multistreet(pv::FEATURE_SUPPORT_SUPPORTED);
  caps.set_side_pots(pv::FEATURE_SUPPORT_UNSUPPORTED);
  caps.set_rake(pv::FEATURE_SUPPORT_UNSUPPORTED);
  caps.set_tournament_icm(pv::FEATURE_SUPPORT_UNSUPPORTED);
  caps.set_maximum_request_bytes(1048576);
  caps.set_maximum_solve_time_ms(5000);
  return caps;
}

void write_envelope(const pv::Envelope& env) {
  std::string payload;
  if (!env.SerializeToString(&payload))
    std::exit(1);
  std::string frame;
  encode_frame(frame, payload);
  if (!write_frame(STDOUT_FILENO, frame))
    std::exit(1);
}

}  // namespace

int main() {
  const char* mode_env = std::getenv("SCRIPTED_ENGINE_MODE");
  const std::string mode = mode_env ? mode_env : "serve";
  const char* action_env = std::getenv("SCRIPTED_ENGINE_ACTION");
  const std::string action = action_env ? action_env : "call";
  const char* crash_env = std::getenv("SCRIPTED_ENGINE_CRASH_AFTER");
  const int crash_after = crash_env ? std::atoi(crash_env) : 1;

  std::uint64_t decisions_seen = 0;

  for (;;) {
    std::string frame;
    const FrameStatus status = read_frame(STDIN_FILENO, frame, 60000);
    if (status != FrameStatus::Complete)
      return 0;

    pv::Envelope envelope;
    if (!envelope.ParseFromString(frame))
      return 1;

    if (envelope.has_get_capabilities_request()) {
      pv::Envelope response;
      response.set_protocol_minor(envelope.protocol_minor());
      response.set_request_id("cap-response");
      *response.mutable_get_capabilities_response() = make_caps();
      write_envelope(response);
      continue;
    }

    if (envelope.has_decision_request()) {
      ++decisions_seen;

      if (mode == "timeout") {
        // Hang forever; the client times out and kills us.
        for (;;)
          std::this_thread::sleep_for(std::chrono::seconds(1));
      }

      if (mode == "crash" && decisions_seen > static_cast<std::uint64_t>(crash_after)) {
        // Exit abruptly; the client sees EOF on the next read.
        _exit(1);
      }

      pv::Envelope response;
      response.set_protocol_minor(envelope.protocol_minor());
      response.set_request_id("d-response");
      pv::DecisionResponse* dr = response.mutable_decision_response();
      pv::Strategy* strategy = dr->mutable_strategy();
      pv::SelectedAction* sel = strategy->mutable_selected_action();
      if (action == "fold")
        sel->set_type(pv::ACTION_TYPE_FOLD);
      else if (action == "check")
        sel->set_type(pv::ACTION_TYPE_CHECK);
      else
        sel->set_type(pv::ACTION_TYPE_CALL);
      strategy->mutable_solver()->set_source(pv::SOLVER_SOURCE_BLUEPRINT);
      write_envelope(response);
      continue;
    }
  }

  return 0;
}
