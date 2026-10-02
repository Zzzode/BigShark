#include "frame_stream.hpp"

#include <poll.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <string>

namespace bs::engine_client {

namespace {

constexpr unsigned char kContinuation = 0x80;
constexpr unsigned char kPayloadMask = 0x7f;

enum class ReadReady {
  Readable,
  Timeout,
  EndOfStream,
  Error,
};

// Waits up to `timeout_ms` for `fd` to become readable. Distinguishes a clean
// hangup (POLLHUP with no data) from a timeout so the caller can map them to
// EndOfStream vs Timeout.
ReadReady wait_readable(int fd, int timeout_ms) {
  struct pollfd pfd{};
  pfd.fd = fd;
  pfd.events = POLLIN;
  for (;;) {
    const int rc = ::poll(&pfd, 1, timeout_ms);
    if (rc > 0) {
      if ((pfd.revents & (POLLIN | POLLERR)) != 0)
        return ReadReady::Readable;
      if ((pfd.revents & POLLHUP) != 0)
        return ReadReady::EndOfStream;
      return ReadReady::Error;
    }
    if (rc == 0)
      return ReadReady::Timeout;
    if (errno == EINTR)
      continue;
    return ReadReady::Error;
  }
}

// Reads one byte from `fd`, waiting up to `timeout_ms`. Returns the byte as an
// unsigned char in [0,255], or -1 on EOF, -2 on timeout, -3 on error.
int read_byte(int fd, int timeout_ms) {
  const ReadReady ready = wait_readable(fd, timeout_ms);
  if (ready == ReadReady::Timeout)
    return -2;
  if (ready != ReadReady::Readable)
    return -1;
  unsigned char byte = 0;
  for (;;) {
    const ssize_t rc = ::read(fd, &byte, 1);
    if (rc == 1)
      return byte;
    if (rc == 0)
      return -1;  // EOF
    if (rc < 0 && errno == EINTR)
      continue;
    return -3;
  }
}

}  // namespace

bool encode_frame(std::string& output, const std::string& payload) {
  if (payload.size() > kMaxFrameBytes)
    return false;
  std::size_t length = payload.size();
  do {
    unsigned char byte = static_cast<unsigned char>(length & kPayloadMask);
    length >>= 7;
    if (length != 0)
      byte |= kContinuation;
    output.push_back(static_cast<char>(byte));
  } while (length != 0);
  output.append(payload);
  return true;
}

FrameStatus read_frame(int fd, std::string& frame, int timeout_ms) {
  // Decode the ULEB128 length into the fixed five-byte header. The running
  // value is range-checked as it accumulates, so an oversize length is
  // rejected before any payload allocation happens. Mirrors the server's
  // FrameReader exactly, including the overlong-encoding rejection.
  std::uint64_t length = 0;
  int prefixBytes = 0;
  while (true) {
    const int raw = read_byte(fd, timeout_ms);
    if (raw == -2)
      return FrameStatus::Timeout;
    if (raw < 0)
      return prefixBytes == 0 ? FrameStatus::EndOfStream : FrameStatus::MalformedFrame;
    const auto byte = static_cast<unsigned char>(raw);

    if (prefixBytes >= 5)
      return FrameStatus::MalformedFrame;  // ULEB128 uint32 cannot use 6+ bytes
    const std::uint64_t group = byte & kPayloadMask;
    length |= group << (7 * prefixBytes);
    ++prefixBytes;
    if (length > kMaxFrameBytes)
      return FrameStatus::OversizeFrame;
    if ((byte & kContinuation) == 0) {
      // A terminating zero group (e.g. 0x81 0x00 for value 1) is overlong.
      // Interior zero groups are legitimate canonical encoding (2^20 is
      // 0x80 0x80 0x40).
      if (prefixBytes > 1 && group == 0)
        return FrameStatus::MalformedFrame;
      break;
    }
  }

  if (length == 0)
    return FrameStatus::MalformedFrame;

  // The declared length is fully known and bounded before the single
  // allocation; the payload buffer never grows incrementally.
  frame.assign(static_cast<std::size_t>(length), '\0');
  std::size_t offset = 0;
  while (offset < frame.size()) {
    const ReadReady ready = wait_readable(fd, timeout_ms);
    if (ready == ReadReady::Timeout)
      return FrameStatus::Timeout;
    if (ready != ReadReady::Readable)
      return FrameStatus::MalformedFrame;  // EOF or error mid-payload
    const ssize_t rc = ::read(fd, frame.data() + offset, frame.size() - offset);
    if (rc > 0) {
      offset += static_cast<std::size_t>(rc);
      continue;
    }
    if (rc == 0)
      return FrameStatus::MalformedFrame;  // EOF mid-payload
    if (rc < 0 && errno == EINTR)
      continue;
    return FrameStatus::MalformedFrame;
  }
  return FrameStatus::Complete;
}

bool write_frame(int fd, const std::string& frame) {
  std::size_t offset = 0;
  while (offset < frame.size()) {
    const ssize_t rc = ::write(fd, frame.data() + offset, frame.size() - offset);
    if (rc > 0) {
      offset += static_cast<std::size_t>(rc);
      continue;
    }
    if (rc < 0 && errno == EINTR)
      continue;
    return false;  // EPIPE or another hard error
  }
  return true;
}

}  // namespace bs::engine_client
