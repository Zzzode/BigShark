// D-382 manual single-ledger cap-k embedding probe (manual, NOT a CTest).
//
// The D164 cap-materiality probe can only measure the DFS-first frontier
// leaf of a frozen stratum. The D-382 other-river-class campaign needs the
// cap-4 structural term on a specific aggressive-line betting ledger
// selected from the RFC 0050 eleven-ledger enumeration, so this probe adds
// ledger selection only: it reuses the unchanged measure_cap_materiality
// instrument (same frozen T6 identity, same EmbeddedGame evaluator, same
// D164 CSV columns) and adds no solver, certifier, or production surface.
// Terminal all-in ledgers (zero action nodes) are rejected structurally.
//
// Usage: bigshark-rfc0047-ledger-embed-probe FAMILY FAMILY_INDEX ITERS THREADS CAP [ARM]
//        FAMILY is "limped" or "srp"; FAMILY_INDEX follows the sealed
//        stage0a eleven-ledger census ordering; ARM defaults to both and
//        accepts uniform|br_favorable|both for bounded-wall campaigns.
#include <sys/resource.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#include "cap_embedding.hpp"
#include "ledger_census.hpp"
#include "offline_census.hpp"

using namespace bs::rfc0047;

namespace {

constexpr unsigned long kMaxOnlineCap = 32;

double timeval_ms(const timeval& tv) {
  return static_cast<double>(tv.tv_sec) * 1000.0 + static_cast<double>(tv.tv_usec) / 1000.0;
}

long current_rss_kb() {
  std::ifstream status("/proc/self/statm");
  long resident_pages = 0;
  long total_pages = 0;
  status >> total_pages >> resident_pages;
  if (resident_pages <= 0)
    return -1;
  const long page_kb = sysconf(_SC_PAGESIZE) / 1024L;
  return resident_pages * (page_kb > 0 ? page_kb : 4L);
}

const char* arm_name(CapExtensionCompletion arm) {
  return arm == CapExtensionCompletion::Uniform ? "uniform" : "br_favorable";
}

const char* kEmbedHeader =
    "cap,completion,cap0_action_nodes,online_action_nodes,mapped_nodes,online_nash_conv_bb,"
    "e_embed_bb,pure_menu_term_bb,gate_m_bb,gate_pass,wall_ms,cpu_ms,rss_peak_kb,rss_current_kb\n";

void print_embed_row(const CapMateriality& m, CapExtensionCompletion arm, double wall_ms,
                     double cpu_ms, long rss_peak_kb) {
  std::printf("%u,%s,%zu,%zu,%zu,%.8f,%.8f,%.8f,%.4f,%d,%.3f,%.3f,%ld,%ld\n",
              static_cast<unsigned>(m.online_cap), arm_name(arm), m.cap0_action_nodes,
              m.online_action_nodes, m.mapped_nodes, m.online_nash_conv_bb,
              m.embedded_exploitability_bb, m.embedded_exploitability_bb - m.online_nash_conv_bb,
              kCapMaterialityGateBb, m.gate_pass ? 1 : 0, wall_ms, cpu_ms, rss_peak_kb,
              current_rss_kb());
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 6 && argc != 7) {
    std::fprintf(stderr, "usage: %s FAMILY(limped|srp) FAMILY_INDEX ITERS THREADS CAP [ARM]\n",
                 argv[0]);
    return 2;
  }
  const std::string family = argv[1];
  if (family != "limped" && family != "srp") {
    std::fprintf(stderr, "family must be 'limped' or 'srp', got '%s'\n", family.c_str());
    return 2;
  }
  std::size_t family_index = 0;
  try {
    family_index = std::stoull(argv[2]);
  } catch (const std::exception&) {
    std::fprintf(stderr, "family index must be an integer\n");
    return 2;
  }
  const std::uint64_t iters = std::stoull(argv[3]);
  const std::size_t threads = std::stoul(argv[4]);
  const unsigned long cap_raw = std::stoul(argv[5]);
  if (cap_raw < 1 || cap_raw > kMaxOnlineCap) {
    std::fprintf(stderr, "cap must be in [1, %u]\n", static_cast<unsigned>(kMaxOnlineCap));
    return 2;
  }
  const std::uint8_t cap = static_cast<std::uint8_t>(cap_raw);
  std::vector<CapExtensionCompletion> arms{CapExtensionCompletion::Uniform,
                                           CapExtensionCompletion::BrFavorable};
  if (argc == 7) {
    const std::string arm_arg = argv[6];
    if (arm_arg == "uniform")
      arms = {CapExtensionCompletion::Uniform};
    else if (arm_arg == "br_favorable")
      arms = {CapExtensionCompletion::BrFavorable};
    else if (arm_arg != "both") {
      std::fprintf(stderr, "arm must be uniform, br_favorable, or both; got '%s'\n",
                   arm_arg.c_str());
      return 2;
    }
  }

  const std::vector<LedgerAnchor> anchors = enumerate_ledger_anchors();
  const LedgerAnchor* target = nullptr;
  for (const LedgerAnchor& a : anchors) {
    if (a.family == family && a.family_index == family_index) {
      target = &a;
      break;
    }
  }
  if (target == nullptr) {
    std::fprintf(stderr, "no ledger for family=%s index=%zu\n", family.c_str(), family_index);
    return 2;
  }
  const OfflineCensusRow census = offline_census(
      target->representative.river_def, bs::abstraction::ActionAbstraction::identity(0),
      family + "-ledger-" + std::to_string(family_index), family, static_cast<int>(family_index));
  if (!census.fits || census.action_nodes == 0) {
    std::fprintf(stderr,
                 "ledger %s/%zu is terminal or outside the offline envelope; no menu to measure\n",
                 family.c_str(), family_index);
    return 2;
  }

  const bs::poker::GameDef& def = target->representative.river_def;
  std::printf(
      "# d382_ledger_embed family=%s family_index=%zu pot=%llu contributions=%llu/%llu "
      "leaf_count=%zu iters=%llu threads=%zu cap=%u\n",
      family.c_str(), family_index, static_cast<unsigned long long>(target->key.pot),
      static_cast<unsigned long long>(target->key.contribution0),
      static_cast<unsigned long long>(target->key.contribution1), target->leaf_count,
      static_cast<unsigned long long>(iters), threads, static_cast<unsigned>(cap));
  std::printf("%s", kEmbedHeader);
  std::fflush(stdout);
  for (const CapExtensionCompletion arm : arms) {
    rusage ru0{};
    getrusage(RUSAGE_SELF, &ru0);
    const auto t0 = std::chrono::steady_clock::now();
    const CapMateriality m = measure_cap_materiality(def, cap, iters, threads, arm);
    const auto t1 = std::chrono::steady_clock::now();
    rusage ru_now{};
    getrusage(RUSAGE_SELF, &ru_now);
    const double wall_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    const double cpu_ms = (timeval_ms(ru_now.ru_utime) - timeval_ms(ru0.ru_utime)) +
                          (timeval_ms(ru_now.ru_stime) - timeval_ms(ru0.ru_stime));
    print_embed_row(m, arm, wall_ms, cpu_ms, ru_now.ru_maxrss);
    std::fflush(stdout);
  }
  return 0;
}
