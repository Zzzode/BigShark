// v1_protocol.hpp — RFC 0002 framed Protobuf engine protocol boundary.
//
// This public header is deliberately free of generated Protobuf types: the
// wire messages terminate inside engine/src/protocol and never cross into
// domain, solver, policy, or service headers. The host only needs the
// length-delimited frame codec and the byte-level envelope entry point.
#pragma once

#include <array>
#include <bs/heads_up.hpp>
#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <string>
#include <string_view>

namespace bs::v1 {

// RFC 0002 transport limit. Declared frame lengths at or below this value are
// accepted; larger values are rejected before the payload buffer is allocated.
constexpr std::size_t kMaxFrameBytes = 1u << 20;  // 1 MiB

// Appends a canonical ULEB128 varint followed by the payload. Payloads larger
// than kMaxFrameBytes are rejected by readers, so the encoder returns false
// rather than emitting an unacceptable frame.
bool writeFrame(std::string& output, const std::string& payload);

// Reads one length-delimited frame from a blocking byte stream.
//
// The length prefix is decoded incrementally into a fixed five-byte header
// buffer; the payload is allocated once, after the declared length is known
// to satisfy 1..kMaxFrameBytes. A clean end of stream while waiting for the
// next frame is a normal shutdown; any partial frame at end of stream is a
// protocol error.
class FrameReader {
 public:
  enum class Status {
    Complete,        // a frame is available in `frame`
    EndOfStream,     // clean close between frames
    OversizeFrame,   // declared length exceeds kMaxFrameBytes
    MalformedFrame,  // overlong/non-canonical varint or stream I/O failure
  };

  explicit FrameReader(std::istream& input);

  Status read(std::string& frame);

 private:
  std::istream& input_;
};

// Outcome of feeding one frame to the protocol engine.
enum class EnvelopeOutcome {
  Respond,      // `response` holds a serialized Envelope frame payload
  CloseStream,  // the connection must be closed without a response
};

struct EnvelopeResult {
  EnvelopeOutcome outcome = EnvelopeOutcome::CloseStream;
  std::string response;
};

// Why a lookup is not covered, mirrored 1:1 from bs::resident::MissReason for
// the resident-owned misses plus UnsupportedHandState for a HandState the
// postflop heads-up reconstructor refuses (non-postflop, multiway, antes,
// unequal closed-street contributions, unparseable history, an illegal
// transition, or a card collision). Names intentionally reuse the resident
// vocabulary so diagnostics stay stable across the seam; the enum itself is
// protobuf- and resident-independent.
enum class V1BlueprintMiss : std::uint8_t {
  None,
  UnsupportedHandState,
  RootNotSupported,
  RootIdentityMismatch,
  OverBudgetNotAdvertised,
  MissingHistory,
  OffTree,
  OffTreeAmount,
  ZeroProbabilityObservedAction,
  EmptyJointRange,
  UntrainedCombo,
  ComboBlockedByBoard,
  RunoutDivergence,
  OpponentRangeFullyBlocked,
  ZeroProbabilityHeroCombination,
};

const char* to_string(V1BlueprintMiss miss) noexcept;

// Immutable resident policy row in poker-domain terms. The pointers reference
// process-lifetime resident storage and stay valid until the services object
// is destroyed.
struct V1BlueprintRow {
  std::size_t size = 0;
  const bs::poker::Action* actions = nullptr;
  const double* probabilities = nullptr;
  std::string_view artifact_sha256;
};

struct V1BlueprintResult {
  bool hit = false;
  V1BlueprintMiss miss = V1BlueprintMiss::None;
  V1BlueprintRow row;
};

// Injectable protobuf-free host services. The default (noResidentServices)
// advertises no blueprint roots and every blueprint lookup misses, which keeps
// the published default binary on the minor-0/heuristic paths. The framed host
// composition root supplies an implementation backed by a
// bs::resident::ResidentPolicySet; generated Protobuf and resident types stay
// confined to the engine/src/protocol translation units and the host TU.
//
// All calls happen on the single frame-loop thread.
class V1HostServices {
 public:
  virtual ~V1HostServices() = default;

  // True when at least one resident root was advertised at startup.
  virtual bool blueprintAdvertised() const noexcept = 0;

  // Hero decision lookup at a reconstructed postflop heads-up node. The state
  // and hero cards are poker-domain values owned by the caller for the call
  // duration; the implementation reuses its own per-process scratch. The pin
  // is empty on the wire today (no request field names an artifact yet) and
  // selects among roots when present.
  virtual V1BlueprintResult blueprintHeroDecision(
      const bs::poker::HeadsUpState& state, const std::array<int, 2>& hero_cards,
      std::string_view pinned_sha256) const noexcept = 0;
};

// Shared services instance meaning "no resident roots configured".
const V1HostServices& noResidentServices() noexcept;

// Parses one serialized Envelope, dispatches capabilities or a validated
// decision, and returns a serialized Envelope. Never throws: parser failures
// that still identify a frame are reported as INVALID_REQUEST responses,
// while transport-level corruption asks the host to close the stream.
EnvelopeResult handleEnvelope(const std::string& frame);

// Same entry point with explicit host services. Minor 0 never touches them:
// its call graph and response bytes are identical to the one-argument form.
EnvelopeResult handleEnvelope(const std::string& frame, const V1HostServices& services);

}  // namespace bs::v1
