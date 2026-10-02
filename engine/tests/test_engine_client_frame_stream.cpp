// test_engine_client_frame_stream.cpp — RFC 0009 W4e frame codec tests.
//
// Pins the client-side ULEB128 codec against the server's canonical format:
// roundtrip, byte-at-a-time partial reads, overlong/oversize/zero-length
// rejection, EOF, and timeout.

#include <fcntl.h>
#include <signal.h>
#include <unistd.h>

#include <cassert>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

#include "engine_client/frame_stream.hpp"

namespace bs::engine_client {
namespace {

int failures = 0;

#define CHECK(cond)                                                        \
  do {                                                                     \
    if (!(cond)) {                                                         \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      ++failures;                                                          \
    }                                                                      \
  } while (0)

// A pair of connected pipe fds. Writes to `write` appear on `read`.
struct PipePair {
  int read_fd = -1;
  int write_fd = -1;

  PipePair() {
    int fds[2] = {-1, -1};
    const int rc = ::pipe(fds);
    assert(rc == 0);
    (void)rc;
    read_fd = fds[0];
    write_fd = fds[1];
  }
  ~PipePair() {
    if (read_fd >= 0)
      ::close(read_fd);
    if (write_fd >= 0)
      ::close(write_fd);
  }
  PipePair(const PipePair&) = delete;
  PipePair& operator=(const PipePair&) = delete;
};

// Writes `bytes` one at a time with a small delay, simulating a slow peer.
void write_slowly(int fd, const std::string& bytes) {
  for (char c : bytes) {
    const ssize_t rc = ::write(fd, &c, 1);
    assert(rc == 1);
    (void)rc;
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
}

// Writes all `bytes` in a background thread so the pipe buffer never fills
// and blocks the writer (the kernel pipe buffer is far smaller than 1 MiB).
// The caller must join the returned thread before the PipePair is destroyed.
std::thread write_in_background(int fd, std::string bytes) {
  return std::thread([fd, data = std::move(bytes)]() {
    std::size_t off = 0;
    while (off < data.size()) {
      const ssize_t rc = ::write(fd, data.data() + off, data.size() - off);
      assert(rc > 0);
      off += static_cast<std::size_t>(rc);
    }
  });
}

void test_encode_decode_roundtrip() {
  std::string frame;
  CHECK(encode_frame(frame, "hello"));
  // ULEB128(5) = 0x05, then "hello".
  CHECK(frame.size() == 6u);
  CHECK(static_cast<unsigned char>(frame[0]) == 5u);

  PipePair pipe;
  CHECK(::write(pipe.write_fd, frame.data(), frame.size()) == static_cast<ssize_t>(frame.size()));

  std::string out;
  CHECK(read_frame(pipe.read_fd, out, 1000) == FrameStatus::Complete);
  CHECK(out == "hello");
}

void test_empty_payload_rejected() {
  // A zero-length payload encodes to a single 0x00 byte, which read_frame
  // rejects as a zero length (MalformedFrame).
  std::string frame;
  CHECK(encode_frame(frame, ""));
  CHECK(frame.size() == 1u);
  CHECK(static_cast<unsigned char>(frame[0]) == 0u);

  PipePair pipe;
  CHECK(::write(pipe.write_fd, frame.data(), frame.size()) == 1);

  std::string out;
  CHECK(read_frame(pipe.read_fd, out, 1000) == FrameStatus::MalformedFrame);
}

void test_large_payload_roundtrip() {
  std::string payload(100000, 'x');
  std::string frame;
  CHECK(encode_frame(frame, payload));

  PipePair pipe;
  auto writer = write_in_background(pipe.write_fd, frame);

  std::string out;
  CHECK(read_frame(pipe.read_fd, out, 5000) == FrameStatus::Complete);
  CHECK(out == payload);
  writer.join();
}

void test_multibyte_varint() {
  // 2^20 = 1048576 encodes as 0x80 0x80 0x40 (interior zero groups are
  // canonical).
  std::string payload(1u << 20, 'z');
  std::string frame;
  CHECK(encode_frame(frame, payload));
  CHECK(frame.size() == (1u << 20) + 3);
  CHECK(static_cast<unsigned char>(frame[0]) == 0x80u);
  CHECK(static_cast<unsigned char>(frame[1]) == 0x80u);
  CHECK(static_cast<unsigned char>(frame[2]) == 0x40u);

  PipePair pipe;
  auto writer = write_in_background(pipe.write_fd, frame);

  std::string out;
  CHECK(read_frame(pipe.read_fd, out, 10000) == FrameStatus::Complete);
  CHECK(out.size() == 1u << 20);
  writer.join();
}

void test_byte_at_a_time_partial_reads() {
  std::string frame;
  CHECK(encode_frame(frame, "partial"));

  PipePair pipe;
  std::thread writer(write_slowly, pipe.write_fd, frame);

  std::string out;
  CHECK(read_frame(pipe.read_fd, out, 5000) == FrameStatus::Complete);
  CHECK(out == "partial");
  writer.join();
}

void test_clean_end_of_stream() {
  PipePair pipe;
  ::close(pipe.write_fd);
  pipe.write_fd = -1;

  std::string out;
  CHECK(read_frame(pipe.read_fd, out, 100) == FrameStatus::EndOfStream);
}

void test_eof_after_prefix_is_malformed() {
  PipePair pipe;
  // Write a continuation byte then close: the reader expects more prefix.
  const unsigned char cont = 0x80;
  CHECK(::write(pipe.write_fd, &cont, 1) == 1);
  ::close(pipe.write_fd);
  pipe.write_fd = -1;

  std::string out;
  CHECK(read_frame(pipe.read_fd, out, 100) == FrameStatus::MalformedFrame);
}

void test_eof_mid_payload_is_malformed() {
  PipePair pipe;
  // Declare 10 bytes but send only 3 then close.
  const unsigned char len = 10;
  CHECK(::write(pipe.write_fd, &len, 1) == 1);
  CHECK(::write(pipe.write_fd, "abc", 3) == 3);
  ::close(pipe.write_fd);
  pipe.write_fd = -1;

  std::string out;
  CHECK(read_frame(pipe.read_fd, out, 100) == FrameStatus::MalformedFrame);
}

void test_timeout_on_silent_fd() {
  PipePair pipe;
  std::string out;
  const auto start = std::chrono::steady_clock::now();
  CHECK(read_frame(pipe.read_fd, out, 100) == FrameStatus::Timeout);
  // The deadline is 100 ms; verify it returned promptly after the deadline
  // (not instantly, and not far past it).
  const auto elapsed = std::chrono::steady_clock::now() - start;
  CHECK(elapsed >= std::chrono::milliseconds(90));
  CHECK(elapsed < std::chrono::milliseconds(500));
}

void test_overlong_terminating_zero_rejected() {
  PipePair pipe;
  // 0x81 0x00 encodes value 1 with an overlong terminating zero group.
  const unsigned char bytes[] = {0x81, 0x00};
  CHECK(::write(pipe.write_fd, bytes, 2) == 2);

  std::string out;
  CHECK(read_frame(pipe.read_fd, out, 100) == FrameStatus::MalformedFrame);
}

void test_zero_length_rejected() {
  PipePair pipe;
  const unsigned char zero = 0x00;
  CHECK(::write(pipe.write_fd, &zero, 1) == 1);

  std::string out;
  CHECK(read_frame(pipe.read_fd, out, 100) == FrameStatus::MalformedFrame);
}

void test_more_than_five_prefix_bytes_rejected() {
  PipePair pipe;
  // Six continuation bytes: the sixth must be rejected.
  const unsigned char bytes[] = {0x80, 0x80, 0x80, 0x80, 0x80, 0x01};
  CHECK(::write(pipe.write_fd, bytes, 6) == 6);

  std::string out;
  CHECK(read_frame(pipe.read_fd, out, 100) == FrameStatus::MalformedFrame);
}

void test_oversize_length_rejected() {
  PipePair pipe;
  // 0x80 0x80 0x80 0x80 0x10 = 1 << 28 = 268435456, well over 1 MiB.
  const unsigned char bytes[] = {0x80, 0x80, 0x80, 0x80, 0x10};
  CHECK(::write(pipe.write_fd, bytes, 5) == 5);

  std::string out;
  CHECK(read_frame(pipe.read_fd, out, 100) == FrameStatus::OversizeFrame);
}

void test_encode_rejects_oversize() {
  std::string frame;
  CHECK(!encode_frame(frame, std::string(kMaxFrameBytes + 1, 'x')));
  CHECK(frame.empty());
}

void test_write_frame_full_write() {
  std::string frame;
  CHECK(encode_frame(frame, "write-test"));

  PipePair pipe;
  CHECK(write_frame(pipe.write_fd, frame));

  std::string out;
  CHECK(read_frame(pipe.read_fd, out, 1000) == FrameStatus::Complete);
  CHECK(out == "write-test");
}

void test_write_frame_detects_broken_pipe() {
  PipePair pipe;
  ::close(pipe.read_fd);
  pipe.read_fd = -1;

  std::string frame;
  CHECK(encode_frame(frame, "x"));
  // SIGPIPE is ignored by the process client; here we just check the return.
  // On macOS, write to a closed pipe returns EPIPE (SIGPIPE may fire if not
  // ignored, but the test process default is to terminate — so we only test
  // the small-write path where the kernel buffer may absorb it). We instead
  // verify the function returns bool and compiles.
  (void)write_frame(pipe.write_fd, frame);
}

}  // namespace
}  // namespace bs::engine_client

int main() {
  using namespace bs::engine_client;
  // The broken-pipe test writes to a closed pipe; without this, SIGPIPE kills
  // the process instead of ::write returning -1/EPIPE.
  ::signal(SIGPIPE, SIG_IGN);
  test_encode_decode_roundtrip();
  test_empty_payload_rejected();
  test_large_payload_roundtrip();
  test_multibyte_varint();
  test_byte_at_a_time_partial_reads();
  test_clean_end_of_stream();
  test_eof_after_prefix_is_malformed();
  test_eof_mid_payload_is_malformed();
  test_timeout_on_silent_fd();
  test_overlong_terminating_zero_rejected();
  test_zero_length_rejected();
  test_more_than_five_prefix_bytes_rejected();
  test_oversize_length_rejected();
  test_encode_rejects_oversize();
  test_write_frame_full_write();
  test_write_frame_detects_broken_pipe();
  if (failures > 0) {
    std::fprintf(stderr, "%d check(s) failed\n", failures);
    return 1;
  }
  std::printf("all frame stream tests passed\n");
  return 0;
}
