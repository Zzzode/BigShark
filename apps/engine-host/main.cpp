// main.cpp — engine host entry point.
//   default:        one decision, JSON context on stdin -> JSON decision on stdout
//   --serve:        persistent line-delimited coprocess: read one JSON object per
//                   stdin line, print one JSON decision per stdout line (flush each).
//   --proto:        one RFC 0002 framed Envelope on stdin -> one framed Envelope
//   --serve-proto:  persistent framed Protobuf coprocess, one frame in/out per
//                   decision. Diagnostics go to stderr; stdout carries frames only.
#include <bs/decision.hpp>
#include <bs/service.hpp>
#include <bs/v0_protocol.hpp>
#include <bs/v1_protocol.hpp>
#include <exception>
#include <iostream>
#include <sstream>
#include <string>

static std::string decideJson(const std::string& raw) {
  bs::Decision d;
  try {
    bs::Ctx ctx = bs::v0::parseRequest(raw);
    d = bs::decide(ctx);
  } catch (const std::exception&) {
    return "{\"action\":\"fold\",\"amount\":0,\"reason\":\"parse-error\"}";
  }
  return bs::v0::serializeResponse(d);
}

// Handles one frame at a time and stops on a framing-level protocol error.
static int runProto(std::istream& input, std::ostream& output, bool persistent) {
  bs::v1::FrameReader reader(input);
  std::string frame;
  while (true) {
    frame.clear();
    const bs::v1::FrameReader::Status status = reader.read(frame);
    if (status == bs::v1::FrameReader::Status::EndOfStream)
      return 0;
    if (status != bs::v1::FrameReader::Status::Complete) {
      std::cerr << "v1 frame protocol error\n";
      return 2;
    }

    const bs::v1::EnvelopeResult result = bs::v1::handleEnvelope(frame);
    if (result.outcome != bs::v1::EnvelopeOutcome::Respond)
      return 2;
    std::string encoded;
    if (!bs::v1::writeFrame(encoded, result.response)) {
      // An over-large engine response is an engine fault, never a reason to
      // kill a persistent coprocess: report on stderr and keep serving.
      std::cerr << "v1 response exceeded frame limit\n";
      continue;
    }
    output.write(encoded.data(), static_cast<std::streamsize>(encoded.size()));
    output.flush();

    if (!persistent)
      return 0;
  }
}

int main(int argc, char** argv) {
  std::ios::sync_with_stdio(false);
  std::cin.tie(nullptr);

  bool serve = false;
  bool proto = false;
  bool serveProto = false;
  for (int i = 1; i < argc; i++) {
    const std::string argument = argv[i];
    if (argument == "--serve")
      serve = true;
    else if (argument == "--proto")
      proto = true;
    else if (argument == "--serve-proto")
      serveProto = true;
  }

  if (proto || serveProto)
    return runProto(std::cin, std::cout, serveProto);

  if (serve) {
    std::string line;
    while (std::getline(std::cin, line)) {
      if (line.empty())
        continue;
      std::cout << decideJson(line) << '\n' << std::flush;
    }
    return 0;
  }

  std::ostringstream ss;
  ss << std::cin.rdbuf();
  std::cout << decideJson(ss.str()) << '\n';
  return 0;
}
