// frame_stream.hpp — RFC 0009 W4e client-side framed v1 transport.
//
// Re-implements the server's canonical ULEB128 length-prefixed codec (see
// engine/src/protocol/v1_frame_stream.cpp, which the client must not link)
// over raw pipe file descriptors with poll()-based deadlines. The wire format
// is byte-identical to the server's, so the two can interoperate.
//
// Frame layout: a ULEB128 varint payload length (1..5 bytes, canonical: no
// overlong terminating zero group, no zero length) followed by the payload.
// The maximum frame size is 1 MiB.
#pragma once

#include <sys/types.h>

#include <cstddef>
#include <string>

namespace bs::engine_client {

// Maximum frame payload size in bytes. Mirrors bs::v1::kMaxFrameBytes.
constexpr std::size_t kMaxFrameBytes = 1u << 20;

// Encodes `payload` into `output` as one ULEB128-framed message. Returns false
// if the payload exceeds kMaxFrameBytes.
bool encode_frame(std::string& output, const std::string& payload);

// Outcome of a read_frame call.
enum class FrameStatus {
  Complete,        // a full frame was read into `frame`
  EndOfStream,     // clean EOF before any prefix byte
  Timeout,         // deadline expired with no data
  MalformedFrame,  // bad varint, overlong encoding, zero length, or short read
  OversizeFrame,   // declared length exceeds kMaxFrameBytes
};

// Reads one framed message from `fd` into `frame`, waiting at most
// `timeout_ms` for the first byte. Once the first byte arrives the deadline
// is refreshed per subsequent read so a slow but live engine is not killed
// mid-frame. Retries EINTR. Returns FrameStatus::Complete on success.
FrameStatus read_frame(int fd, std::string& frame, int timeout_ms);

// Writes the full `frame` bytes to `fd`, retrying EINTR and partial writes.
// Returns false on EPIPE or another hard error.
bool write_frame(int fd, const std::string& frame);

}  // namespace bs::engine_client
