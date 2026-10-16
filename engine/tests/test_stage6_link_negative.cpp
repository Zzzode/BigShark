// RFC 0008 stage 6 offline link-negative guard.
//
// This binary links the live decision surface -- decision service and the
// framed protocol -- and exercises one service call path, so the linker pulls
// every object those libraries actually need. A companion CTest (registered
// in CMakeLists.txt) runs `nm` over this executable and FAILS if any symbol
// from the bs::stage6 namespace (or the bigshark_behavior library) is present.
// The configure-time transitive-link guard catches the dependency declaration;
// this binary proves the final link really carries no offline object code.
#include <bs/protocol.hpp>
#include <string>

int main() {
  // Drive one frame encode/decode path so protocol objects are pulled.
  std::string frame;
  if (!bs::writeFrame(frame, "test"))
    return 1;
  const bs::EnvelopeResult result = bs::handleEnvelope(frame);
  return result.outcome == bs::EnvelopeOutcome::Respond ? 0 : 1;
}
