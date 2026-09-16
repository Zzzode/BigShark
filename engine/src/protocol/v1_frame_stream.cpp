#include <bs/v1_protocol.hpp>
#include <istream>
#include <string>

namespace bs::v1 {
namespace {

constexpr unsigned char kContinuation = 0x80;
constexpr unsigned char kPayloadMask = 0x7f;

}  // namespace

bool writeFrame(std::string& output, const std::string& payload) {
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

FrameReader::FrameReader(std::istream& input) : input_(input) {}

FrameReader::Status FrameReader::read(std::string& frame) {
  // Decode the ULEB128 length into the fixed five-byte header buffer. The
  // running value is range-checked as it accumulates, so an oversize length
  // is rejected before any payload allocation happens.
  std::uint64_t length = 0;
  int prefixBytes = 0;
  while (true) {
    const int raw = input_.get();
    if (raw == std::char_traits<char>::eof())
      return prefixBytes == 0 ? Status::EndOfStream : Status::MalformedFrame;
    if (!input_.good())
      return Status::MalformedFrame;
    const auto byte = static_cast<unsigned char>(raw);

    if (prefixBytes >= 5)
      return Status::MalformedFrame;  // ULEB128 uint32 cannot use 6+ bytes
    const std::uint64_t group = byte & kPayloadMask;
    length |= group << (7 * prefixBytes);
    ++prefixBytes;
    if (length > kMaxFrameBytes)
      return Status::OversizeFrame;
    if ((byte & kContinuation) == 0) {
      // A terminating zero group (e.g. 0x81 0x00 for value 1) is overlong.
      // Interior zero groups are legitimate canonical encoding (2^20 is
      // 0x80 0x80 0x40).
      if (prefixBytes > 1 && group == 0)
        return Status::MalformedFrame;
      break;
    }
  }

  if (length == 0)
    return Status::MalformedFrame;

  // The declared length is fully known and bounded before the single
  // allocation; the payload buffer never grows incrementally.
  frame.assign(static_cast<std::size_t>(length), '\0');
  input_.read(frame.data(), static_cast<std::streamsize>(length));
  if (input_.gcount() != static_cast<std::streamsize>(length))
    return Status::MalformedFrame;  // incomplete frame at end of stream
  return Status::Complete;
}

}  // namespace bs::v1
