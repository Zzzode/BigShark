// v1_mappers.hpp — internal RFC 0002 v1 Protobuf <-> domain boundary.
//
// This header includes generated Protobuf types and is therefore confined to
// engine/src/protocol/v1* translation units, their tests, and the fuzz target.
// Domain headers under engine/include/bs never see generated messages.
#pragma once

#include <bigshark/engine/v1/engine.pb.h>

#include <bs/decision.hpp>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace bs::v1 {

namespace pv = ::bigshark::engine::v1;

// Largest chip value accepted on the minor-0 integer chip profile. All chip
// fields stay within this bound; fields consumed by the integer heuristic
// additionally require int fit (validated centrally).
constexpr std::uint64_t kMaxChipAmount = (1ull << 53) - 1;

// proto3 enums are open on the wire: unknown numeric values (e.g. 99 or
// 4294967295, which int-reads as -1) parse successfully. Every wire-received
// enum passes through one closed-set predicate before indexing, mapping, or
// influencing a decision. Response-only enums (SolverSource, FeatureSupport,
// AmountSemantics, ErrorCode) are produced internally from constants, never
// read from a request, so they intentionally have no predicate here.
bool isKnownActionType(pv::ActionType value);              // FOLD..RAISE
bool isKnownRank(pv::Rank value);                          // TWO..ACE
bool isKnownSuit(pv::Suit value);                          // SPADES..CLUBS
bool isKnownDecisionStreet(pv::Street value);              // PREFLOP..RIVER
bool isKnownHistoryStreet(pv::Street value);               // PREFLOP..RIVER
bool isKnownPlayerStatus(pv::PlayerStatus value);          // ACTIVE..ALL_IN
bool isKnownForcedType(pv::ForcedContributionType value);  // SMALL_BLIND..STRADDLE
bool isKnownGameVariant(pv::GameVariant value);
bool isKnownBettingStructure(pv::BettingStructure value);
bool isKnownGameType(pv::GameType value);
// Decision options solver modes are a capability check, not a plain closed
// set: AUTOMATIC/HEURISTIC are accepted, the selectable river/multistreet
// backends are UNSUPPORTED_FEATURE in minor 0, anything else is invalid.
bool isKnownSolverMode(pv::SolverMode value);

// Hard cap on reported FieldViolations so a sub-1 MiB request with hundreds of
// thousands of bad entries cannot produce a multi-hundred-MB error response
// that kills the persistent host. The last entry marks truncation.
constexpr int kMaxFieldViolations = 32;

// Exception used strictly inside the v1 boundary. handleEnvelope converts it
// into a DecisionResponse.error; it must never reach the frame loop.
class MappingError : public std::runtime_error {
 public:
  MappingError(pv::ErrorCode code, std::string message, bool retryable,
               std::vector<pv::FieldViolation> violations = {});

  pv::ErrorCode code() const { return code_; }
  bool retryable() const { return retryable_; }
  const std::vector<pv::FieldViolation>& violations() const { return violations_; }

 private:
  pv::ErrorCode code_;
  bool retryable_;
  std::vector<pv::FieldViolation> violations_;
};

struct ValidationReport {
  std::vector<pv::FieldViolation> violations;

  void add(const std::string& fieldPath, const std::string& description);
  bool ok() const { return violations.empty(); }
};

// RFC 3629 UTF-8 validation for proto3 string fields. The C++ runtime parses
// invalid UTF-8 leniently (logging to stderr), while Protobuf-ES rejects it;
// validating here keeps both decoders consistent and fails malformed strings
// closed before they are echoed.
bool isValidUtf8(std::string_view value);

// Full semantic validation (protovalidate annotations plus poker invariants).
// Returns ERROR_CODE_UNSPECIFIED when the request is valid, otherwise the
// applicable failure class (INVALID_REQUEST or UNSUPPORTED_*). All discovered
// violations are appended to the report regardless of the returned code.
pv::ErrorCode validateDecisionRequest(const pv::DecisionRequest& request, ValidationReport& report);

// Maps a semantically valid DecisionRequest into the v0-equivalent heuristic
// Ctx. Any unrepresentable knob is reported as a MappingError rather than
// silently defaulted.
pv::ErrorCode validateAndMap(const pv::DecisionRequest& request, ValidationReport& report,
                             bs::Ctx& context);

// Maps a heuristic decision into a minor-0 degenerate Strategy (one action at
// probability 1). Includes the deterministic selected action only when
// requested and validates it against the request's legal actions by kind and
// exact target. Throws MappingError(INTERNAL) if the engine returned an
// action that is not a member of the requested legal set.
pv::DecisionResponse mapDecisionResponse(const pv::DecisionRequest& request,
                                         const bs::Decision& decision);

// Builds the host capability advertisement. Minor 0 only; minor 1 is owned by
// a later stage.
pv::GetCapabilitiesResponse buildCapabilities();

// Constructs a DecisionResponse carrying an EngineError.
pv::DecisionResponse errorResponse(pv::ErrorCode code, const std::string& message, bool retryable,
                                   std::vector<pv::FieldViolation> violations = {});

}  // namespace bs::v1
