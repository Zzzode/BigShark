// mappers.hpp — internal RFC 0022 Protobuf <-> domain boundary.
//
// This header includes generated Protobuf types and is therefore confined to
// engine/src/protocol translation units and their tests. Domain headers
// under engine/include/bs never see generated messages.
//
// The protocol surface includes: G/F contribution ledger validation,
// phase-tagged pot layers, observation provenance, capability tuples,
// and the expanded error code set.
#pragma once

#include <bigshark/engine/v2/engine.pb.h>

#include <array>
#include <bs/decision.hpp>
#include <bs/game_definition.hpp>
#include <bs/guarantee.hpp>
#include <bs/policy.hpp>
#include <bs/protocol.hpp>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace bs {

namespace pv2 = ::bigshark::engine::v2;

// Largest chip value accepted on the integer chip profile.
constexpr std::uint64_t kMaxChipAmount = (1ull << 53) - 1;

// Hard cap on reported FieldViolations so a malformed request cannot produce
// an oversized error response. The last entry marks truncation.
constexpr int kMaxFieldViolations = 32;

// Exception used strictly inside the protocol boundary. handleEnvelope converts it
// into a DecisionResponse.error; it must never reach the frame loop.
class MappingError : public std::runtime_error {
 public:
  MappingError(pv2::ErrorCode code, std::string message, bool retryable,
               std::vector<pv2::FieldViolation> violations = {});

  pv2::ErrorCode code() const { return code_; }
  bool retryable() const { return retryable_; }
  const std::vector<pv2::FieldViolation>& violations() const { return violations_; }

 private:
  pv2::ErrorCode code_;
  bool retryable_;
  std::vector<pv2::FieldViolation> violations_;
};

struct ValidationReport {
  std::vector<pv2::FieldViolation> violations;

  void add(const std::string& fieldPath, const std::string& description);
  bool ok() const { return violations.empty(); }
};

// Reconstructed native state from a HandState. The GameState is the
// historyless transition machine; the PublicAction log carries the observed
// path from the hand-start root to the actionable node.
struct ReconstructedState {
  std::optional<bs::poker::GameState> state;
  std::vector<bs::poker::PublicAction> history;
  std::array<int, 2> hero_cards{-1, -1};
  std::size_t hero_actor = 0;
};

// Reconstruction miss reasons. Richer than BlueprintMiss:
// Distinguishes replay mismatch (replayed state != declared snapshot) from
// off-tree amounts and incompatible roots.
enum class ReplayMiss : std::uint8_t {
  None,
  HistoryIncomplete,
  ReplayMismatch,
  IncompatibleRoot,
  OffTreeAmount,
  RulesUnconfirmed,
  UnsupportedFeature,
};

// RFC 3629 UTF-8 validation for proto3 string fields. The implementation
// lives in semantic_validator.cpp.
bool isValidUtf8(std::string_view value);

// Full semantic validation (protovalidate annotations plus poker invariants).
// Returns ERROR_CODE_UNSPECIFIED when the request is valid, otherwise the
// applicable failure class. All discovered violations are appended to the
// report regardless of the returned code.
pv2::ErrorCode validateDecisionRequest(const pv2::DecisionRequest& request,
                                       ValidationReport& report);

// Validates the request and maps it into a heuristic bs::Ctx for the
// chart+heuristic cascade. Returns ERROR_CODE_UNSPECIFIED on success.
pv2::ErrorCode validateAndMap(const pv2::DecisionRequest& request, ValidationReport& report,
                              bs::Ctx& context);

// Reconstructs a native GameState from the HandState by replaying the
// TransitionEvent path from the true hand-start root. Does not
// rebase mid-street: the replay runs continuously from preflop to the
// actionable node, retaining refunds and folded seats. Returns false on a
// reconstruction miss (history incomplete, replay mismatch, incompatible
// root); the miss reason is set for diagnostic mapping.
bool reconstructHandState(const pv2::DecisionRequest& request, ReconstructedState& out,
                          ReplayMiss& miss);

// Maps a sourced heuristic decision into an ExpandedStrategy. The source is
// the declared policy source (no reason-text inference), the guarantee level
// comes from the normative guaranteeFor table, and the utility profile is
// CASH_CHIP. Legal membership is validated exactly as before.
pv2::DecisionResponse mapHeuristicResponse(const pv2::DecisionRequest& request,
                                           const bs::SourcedDecision& answer);

// Maps a resident blueprint hit into an ExpandedStrategy. Every row action
// becomes one ActionPolicy (1..32, verbatim probabilities, target total only
// on bet/raise, all_in derived from the legal maximum), the sampled action is
// selected by the pinned SplitMix64 convention, source is BLUEPRINT, and the
// guarantee is approximate.
pv2::DecisionResponse mapBlueprintResponse(const pv2::DecisionRequest& request,
                                           const BlueprintRow& row);

// Maps a resolved terminal-only row into an ExpandedStrategy. A certified
// resolve earns certified_bound; deadline baselines earn approximate.
pv2::DecisionResponse mapResolvedResponse(const pv2::DecisionRequest& request,
                                          const BlueprintRow& row, ResolveOutcome outcome);

// Maps the AUTOMATIC operational fallback (check, else call, else fold) into
// an ExpandedStrategy tagged operational_fallback with an OPERATIONAL_FALLBACK
// source.
pv2::DecisionResponse mapOperationalFallbackResponse(const pv2::DecisionRequest& request,
                                                     const bs::Decision& decision);

// Every resident row action must be a member of the request legal set by kind
// and exact target total.
bool blueprintRowIsLegal(const pv2::DecisionRequest& request, const BlueprintRow& row);

// Builds the host capability advertisement. Emits CapabilityTuple messages
// for every fully supported combination. Initially advertises hu-r1 (heads-up)
// only; sixmax-r1 is gated on T2-T5 completion.
pv2::GetCapabilitiesResponse buildCapabilities(bool blueprint_advertised = false,
                                               bool resolving_advertised = false);

// Constructs a DecisionResponse carrying an EngineError.
pv2::DecisionResponse errorResponse(pv2::ErrorCode code, const std::string& message, bool retryable,
                                    std::vector<pv2::FieldViolation> violations = {});

}  // namespace bs
