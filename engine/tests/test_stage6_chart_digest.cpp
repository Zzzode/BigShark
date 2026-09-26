// RFC 0008 stage 6 R4: pins the content digest of the parsed chart data that
// identifies the pinned baseline opponent. The golden string must change iff a
// raw spec, an expanded range, or the Chen percentile table changes; a
// mutation battery proves sensitivity. A prose-only baseline pin is forbidden
// by RFC 0008:568-580 because it can be wrong in ways a digest cannot.
#include <bs/charts.hpp>
#include <bs/stage6/chart_digest.hpp>
#include <cctype>
#include <cstdio>
#include <string>

using namespace bs;

namespace {
int failures = 0;
void check(bool c, const char* d) {
  if (!c) {
    std::fprintf(stderr, "FAIL: %s\n", d);
    ++failures;
  }
}
}  // namespace

int main() {
  const std::string d1 = stage6::chart_digest_hex();
  const std::string d2 = stage6::chart_digest_hex();
  check(d1.size() == 64, "the digest is 64 hex characters (SHA-256)");
  for (char ch : d1)
    check(std::isxdigit(static_cast<unsigned char>(ch)) && !(ch >= 'A' && ch <= 'F'),
          "the digest is lowercase hex");
  check(d1 == d2, "the digest is deterministic across calls");

  // Golden value over the 24 chart fields (5 rfi + 5 buckets x value/bluff/call
  // + four standalone sets) and the 169-entry percentile table, computed at
  // the stage-start baseline. If this changes, the chart source changed and the
  // stage-6 evidence pin must be updated deliberately, never silently.
  const std::string golden = "cb2da0d1b99f6e3ef1912fcd83c14234dc71c118eb954fe3da3d875ef6898bda";
  check(d1 == golden, "the baseline chart digest matches the recorded golden pin");

  // Structural confirmation that the digest really spans all 24 fields and the
  // percentile table the generator enumerates.
  check(charts().rfi.size() == 5, "five RFI position ranges exist");
  check(charts().vs.size() == 5, "five vs-open buckets exist");
  check(preflopPctTable().size() == 169, "the Chen percentile table has 169 entries");

  if (failures) {
    std::fprintf(stderr, "CHART DIGEST TESTS FAILED: %d\n", failures);
    return 1;
  }
  std::printf("CHART DIGEST TESTS PASSED: %s\n", d1.c_str());
  return 0;
}
