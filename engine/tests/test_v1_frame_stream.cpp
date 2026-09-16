#include <bs/v1_protocol.hpp>
#include <cstdio>
#include <sstream>
#include <string>

namespace {

int failures = 0;

void check(bool condition, const char* description) {
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", description);
    ++failures;
  }
}

std::string bytes(std::initializer_list<unsigned char> values) {
  std::string result;
  for (unsigned char value : values)
    result.push_back(static_cast<char>(value));
  return result;
}

}  // namespace

int main() {
  using bs::v1::FrameReader;
  using bs::v1::kMaxFrameBytes;

  // Round trip: small frame.
  {
    std::string encoded;
    check(bs::v1::writeFrame(encoded, "hello"), "writeFrame accepts small payload");
    check(encoded == bytes({0x05, 'h', 'e', 'l', 'l', 'o'}), "small frame varint prefix");
    std::istringstream input(encoded);
    FrameReader reader(input);
    std::string frame;
    check(reader.read(frame) == FrameReader::Status::Complete, "read small frame");
    check(frame == "hello", "small frame payload");
    check(reader.read(frame) == FrameReader::Status::EndOfStream, "clean end of stream");
  }

  // Round trip: maximum-size frame uses a three-byte canonical prefix.
  {
    const std::string payload(kMaxFrameBytes, 'x');
    std::string encoded;
    check(bs::v1::writeFrame(encoded, payload), "writeFrame accepts 1 MiB payload");
    check(static_cast<unsigned char>(encoded[0]) == 0x80 &&
              static_cast<unsigned char>(encoded[1]) == 0x80 &&
              static_cast<unsigned char>(encoded[2]) == 0x40,
          "1 MiB prefix is canonical");
    std::istringstream input(encoded);
    FrameReader reader(input);
    std::string frame;
    check(reader.read(frame) == FrameReader::Status::Complete, "read 1 MiB frame");
    check(frame.size() == kMaxFrameBytes, "1 MiB payload length");
  }

  // One byte over the limit is rejected without a payload allocation.
  {
    // Varint encoding of 1048577 = 0x100001.
    std::string encoded = bytes({0x81, 0x80, 0x40, 'a'});
    std::istringstream input(encoded);
    FrameReader reader(input);
    std::string frame;
    check(reader.read(frame) == FrameReader::Status::OversizeFrame,
          "declared length above 1 MiB is rejected");
  }

  // Zero-length frames are malformed.
  {
    std::istringstream input(bytes({0x00}));
    FrameReader reader(input);
    std::string frame;
    check(reader.read(frame) == FrameReader::Status::MalformedFrame, "zero-length frame rejected");
  }

  // Overlong (non-canonical) varints are rejected.
  {
    // Value 1 encoded with a trailing zero group instead of the one-byte 0x01.
    std::istringstream input(bytes({0x81, 0x00, 'a'}));
    FrameReader reader(input);
    std::string frame;
    check(reader.read(frame) == FrameReader::Status::MalformedFrame,
          "non-canonical trailing zero group rejected");
  }
  {
    std::istringstream input(bytes({0x81, 0x80, 0x80, 0x80, 0x00, 'a'}));
    FrameReader reader(input);
    std::string frame;
    check(reader.read(frame) == FrameReader::Status::MalformedFrame,
          "non-canonical trailing zero group rejected");
  }

  // More than five prefix bytes are rejected.
  {
    std::istringstream input(bytes({0x80, 0x80, 0x80, 0x80, 0x80, 0x01}));
    FrameReader reader(input);
    std::string frame;
    check(reader.read(frame) == FrameReader::Status::MalformedFrame, "six-byte prefix rejected");
  }

  // A truncated payload at end of stream is a protocol error, not clean EOF.
  {
    std::istringstream input(bytes({0x05, 'a', 'b'}));
    FrameReader reader(input);
    std::string frame;
    check(reader.read(frame) == FrameReader::Status::MalformedFrame,
          "incomplete frame at EOF rejected");
  }
  {
    std::istringstream input(bytes({0x81, 0x01}));
    FrameReader reader(input);
    std::string frame;
    check(reader.read(frame) == FrameReader::Status::MalformedFrame,
          "prefix without body at EOF rejected");
  }

  // Multiple frames are read in order from one stream.
  {
    std::string encoded;
    bs::v1::writeFrame(encoded, "one");
    bs::v1::writeFrame(encoded, "two");
    std::istringstream input(encoded);
    FrameReader reader(input);
    std::string frame;
    check(reader.read(frame) == FrameReader::Status::Complete && frame == "one", "first frame");
    check(reader.read(frame) == FrameReader::Status::Complete && frame == "two", "second frame");
    check(reader.read(frame) == FrameReader::Status::EndOfStream, "final clean EOF");
  }

  // The writer refuses to emit an oversize frame.
  {
    std::string encoded;
    const std::string tooLarge(kMaxFrameBytes + 1, 'x');
    check(!bs::v1::writeFrame(encoded, tooLarge), "writer rejects above-limit payload");
  }

  if (failures != 0) {
    std::fprintf(stderr, "V1 FRAME STREAM TESTS FAILED: %d\n", failures);
    return 1;
  }
  std::puts("V1 FRAME STREAM TESTS PASSED");
  return 0;
}
