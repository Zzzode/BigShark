// v1_protocol.hpp — RFC 0002 framed Protobuf engine protocol boundary.
//
// This public header is deliberately free of generated Protobuf types: the
// wire messages terminate inside engine/src/protocol and never cross into
// domain, solver, policy, or service headers. The host only needs the
// length-delimited frame codec and the byte-level envelope entry point.
#pragma once

#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <string>

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

// Parses one serialized Envelope, dispatches capabilities or a validated
// decision, and returns a serialized Envelope. Never throws: parser failures
// that still identify a frame are reported as INVALID_REQUEST responses,
// while transport-level corruption asks the host to close the stream.
EnvelopeResult handleEnvelope(const std::string& frame);

}  // namespace bs::v1
