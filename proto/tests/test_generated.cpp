#include <bigshark/engine/v1/engine.pb.h>
#include <google/protobuf/util/json_util.h>

#include <array>
#include <cstdio>
#include <fstream>
#include <ios>
#include <iterator>
#include <string>

namespace {

std::string readFile(const std::string& path) {
  std::ifstream input(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

int fail(const char* message) {
  std::fputs(message, stderr);
  std::fputc('\n', stderr);
  return 1;
}

}  // namespace

int main() {
  const std::string fixture_dir = BIGSHARK_PROTO_FIXTURE_DIR;
  constexpr std::array<const char*, 5> fixture_names = {"capabilities-request",
                                                        "capabilities-response", "decision-request",
                                                        "envelope", "error-response"};
  for (const char* name : fixture_names) {
    const std::string binary = readFile(fixture_dir + "/" + name + ".binpb");
    const std::string json = readFile(fixture_dir + "/" + name + ".json");
    bigshark::engine::v1::Envelope from_binary;
    bigshark::engine::v1::Envelope from_json;
    if (!from_binary.ParseFromString(binary))
      return fail("BINARY GOLDEN PARSE FAILED");
    const auto json_status = google::protobuf::util::JsonStringToMessage(json, &from_json);
    if (!json_status.ok())
      return fail("PROTOJSON GOLDEN PARSE FAILED");
    if (from_json.SerializeAsString() != binary)
      return fail("PROTOJSON AND BINARY GOLDENS DIVERGED");
  }

  const std::string binary = readFile(fixture_dir + "/envelope.binpb");
  bigshark::engine::v1::Envelope from_binary;
  if (!from_binary.ParseFromString(binary))
    return fail("BINARY GOLDEN PARSE FAILED");
  if (!from_binary.has_decision_response() || !from_binary.decision_response().has_strategy())
    return fail("BINARY GOLDEN PAYLOAD FAILED");

  const auto& action = from_binary.decision_response().strategy().actions(0);
  if (action.type() != bigshark::engine::v1::ACTION_TYPE_RAISE ||
      action.target_total() != 9007199254740993ULL || action.expected_value() != -42)
    return fail("BINARY GOLDEN UINT64 OR ACTION FAILED");

  bigshark::engine::v1::Envelope request;
  if (!request.ParseFromString(readFile(fixture_dir + "/decision-request.binpb")) ||
      !request.has_decision_request())
    return fail("DECISION REQUEST GOLDEN PAYLOAD FAILED");
  const auto& state = request.decision_request().state();
  const auto& options = request.decision_request().options();
  if (!state.game().has_rake() || !state.game().has_straddle() || state.players_size() != 3 ||
      state.hero_hole_cards_size() != 2 || state.board_size() != 4 ||
      state.pot().side_pots_size() != 1 || state.forced_contributions_size() != 6 ||
      state.action_history_size() != 6 || state.legal_actions_size() != 3 ||
      state.players(0).stack() != 9007199254740993ULL ||
      options.seed() != 18446744073709551615ULL ||
      options.solver_mode() != bigshark::engine::v1::SOLVER_MODE_AUTOMATIC)
    return fail("DECISION REQUEST NESTED FIELD COVERAGE FAILED");

  bigshark::engine::v1::Envelope capabilities;
  if (!capabilities.ParseFromString(readFile(fixture_dir + "/capabilities-response.binpb")) ||
      !capabilities.has_get_capabilities_response() ||
      capabilities.get_capabilities_response().supported_streets_size() != 5 ||
      capabilities.get_capabilities_response().supported_actions_size() != 5 ||
      capabilities.get_capabilities_response().maximum_request_bytes() != 1048576)
    return fail("CAPABILITIES RESPONSE FIELD COVERAGE FAILED");

  bigshark::engine::v1::Envelope error;
  if (!error.ParseFromString(readFile(fixture_dir + "/error-response.binpb")) ||
      !error.has_decision_response() || !error.decision_response().has_error() ||
      error.decision_response().error().violations_size() != 1)
    return fail("ERROR RESPONSE FIELD COVERAGE FAILED");

  std::string with_unknown = binary;
  with_unknown.append("\xA0\x06\x01", 3);
  bigshark::engine::v1::Envelope unknown_field;
  if (!unknown_field.ParseFromString(with_unknown) ||
      unknown_field.SerializeAsString() != with_unknown)
    return fail("UNKNOWN FIELD PRESERVATION FAILED");

  bigshark::engine::v1::Card unknown_enum;
  const std::string card_binary = readFile(fixture_dir + "/card-unknown-enum.binpb");
  if (!unknown_enum.ParseFromString(card_binary) || static_cast<int>(unknown_enum.rank()) != 99 ||
      unknown_enum.suit() != bigshark::engine::v1::SUIT_SPADES ||
      unknown_enum.SerializeAsString() != card_binary)
    return fail("UNKNOWN ENUM PRESERVATION FAILED");

  std::puts("PROTOBUF GENERATED ROUND-TRIP TESTS PASSED");
  return 0;
}
