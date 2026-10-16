// v2_protocol.hpp — RFC 0022 v2 framed Protobuf engine protocol boundary.
//
// This public header is deliberately free of generated Protobuf types: the
// wire messages terminate inside engine/src/protocol and never cross into
// domain, solver, policy, or service headers. The host only needs the
// length-delimited frame codec and the byte-level envelope entry point.
//
// The v2 protocol reuses the v1 frame codec, envelope result types, and host
// services interface: the services seam is protobuf-free and operates on
// native poker types (GameState, PublicAction, Action), so a v2 handler
// produces the same native types from a different wire format.
#pragma once

#include <bs/v1_protocol.hpp>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace bs::v2 {

// Reuse the v1 frame codec and envelope result types. The ULEB128 framing is
// version-agnostic; the version discriminator lives inside the envelope.
using bs::v1::EnvelopeOutcome;
using bs::v1::EnvelopeResult;
using bs::v1::FrameReader;
using bs::v1::kMaxFrameBytes;
using bs::v1::writeFrame;

// Reuse the v1 host services interface. The v2 handler reconstructs a native
// GameState from the v2 wire format and delegates to the same blueprint and
// resolving lookup seams.
using bs::v1::noResidentServices;
using bs::v1::V1BlueprintMiss;
using bs::v1::V1BlueprintResult;
using bs::v1::V1BlueprintRow;
using bs::v1::V1HostServices;
using bs::v1::V1ResolveOutcome;
using bs::v1::V1ResolveResult;

// Parses one serialized v2 Envelope (protocol_version == 3), dispatches
// capabilities or a validated decision, and returns a serialized Envelope.
// Never throws: parser failures that still identify a frame are reported as
// INVALID_REQUEST responses, while transport-level corruption asks the host
// to close the stream.
EnvelopeResult handleEnvelope(const std::string& frame);

// Same entry point with explicit host services. The no-resident default
// advertises no blueprint roots and every lookup misses.
EnvelopeResult handleEnvelope(const std::string& frame, const V1HostServices& services);

// Peeks the protocol_version (field 1) from a serialized envelope without
// parsing the payload. Field 1 is a uint32 varint at wire tag 0x08 in both
// v1 and v2 envelopes. Returns nullopt on malformation. The host uses this
// to route: 0..2 -> v1 (frozen), 3 -> v2, >3 -> v2 (rejects with
// UNSUPPORTED_PROTOCOL), nullopt -> v1 (preserves v1's INVALID_REQUEST).
std::optional<std::uint32_t> peekProtocolVersion(std::string_view frame) noexcept;

}  // namespace bs::v2
