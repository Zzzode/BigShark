// RFC 0046 Stage-1 single-cell measurement runner (manual benchmark target,
// NOT a CTest). Drives ONE frozen cell end to end:
//
//   bigshark-rfc0046-cell --stratum <name> --seed <n> --iters <n> --cap <0|1>
//                          [--extract-margins --margin-iters <n>] [--quiet]
//
// Locked margin precomputation (optional) runs OUTSIDE the resolve wall; the
// receipt-to-result cold-resolve wall starts before the online subtree model
// build and ends after certification (the offline chance-free NashConv walk
// is outside the wall). Emits one exact-schema CSV row. Benchmark-only.
#include <sys/resource.h>

#include <array>
#include <bs/abstract_tree.hpp>
#include <bs/abstraction.hpp>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "certifier.hpp"
#include "concrete_keyer.hpp"
#include "margin_extract.hpp"
#include "partition_walker.hpp"
#include "strata.hpp"
#include "turn_gadget.hpp"

using namespace bs;
using namespace bs::rfc0046;

namespace {

struct Args {
  std::string stratum = "monotone";
  std::uint64_t seed = 4320;
  std::uint64_t iters = 10000;
  std::uint64_t margin_iters = 20000;
  std::uint64_t margin_wall_ms = 3600000;  // locked-reference train wall (outside resolve wall)
  std::uint64_t wall_ms = 10000;           // frozen Gate-2 wall; research runs override
  bool extract_margins = false;
  bool quiet = false;
  bool probe_nc = false;  // research: compute+emit partition even if not certified
  std::uint8_t cap = 1;
};

std::int64_t peak_rss_bytes() {
  struct rusage usage = {};
  if (getrusage(RUSAGE_SELF, &usage) != 0)
    return -1;
  return static_cast<std::int64_t>(usage.ru_maxrss) * 1024;  // Linux KiB -> bytes
}

Args parse(int argc, char** argv) {
  Args a;
  for (int i = 1; i < argc; ++i) {
    const std::string_view flag = argv[i];
    auto next = [&]() -> std::string {
      if (i + 1 >= argc)
        throw std::runtime_error("missing value for flag");
      return argv[++i];
    };
    if (flag == "--stratum")
      a.stratum = next();
    else if (flag == "--seed")
      a.seed = std::stoull(next());
    else if (flag == "--iters")
      a.iters = std::stoull(next());
    else if (flag == "--margin-iters")
      a.margin_iters = std::stoull(next());
    else if (flag == "--margin-wall-ms")
      a.margin_wall_ms = std::stoull(next());
    else if (flag == "--wall-ms")
      a.wall_ms = std::stoull(next());
    else if (flag == "--extract-margins")
      a.extract_margins = true;
    else if (flag == "--quiet")
      a.quiet = true;
    else if (flag == "--probe-nc")
      a.probe_nc = true;
    else if (flag == "--cap")
      a.cap = static_cast<std::uint8_t>(std::stoi(next()));
    else
      throw std::runtime_error("unknown flag: " + std::string(flag));
  }
  return a;
}

// Gate-grade concrete profile over the partition tree. A missing average row
// at a reached infoset is a coverage/convergence miss: fail closed rather than
// silently substitute uniform (which would bias Total/B0).
class StrictPolicy final : public PartitionPolicy {
 public:
  const ConcreteCardKeyer* keyer = nullptr;
  const std::map<solver::NSeatInformationKey, GadgetRow>* rows = nullptr;
  void probabilities(std::size_t, std::size_t node0, std::int64_t, const std::array<int, 2>& own,
                     const std::vector<poker::Action>& menu, std::span<double> out) const override {
    const std::uint32_t bucket = keyer->dense_for_live_pair(own);
    const auto it =
        rows->find(solver::NSeatInformationKey{bucket, static_cast<std::uint64_t>(node0)});
    if (it == rows->end() || it->second.actions != menu ||
        it->second.probabilities.size() != menu.size())
      throw std::runtime_error("rfc0046-cell: missing/non-matching solved row at a reached node");
    for (std::size_t a = 0; a < menu.size(); ++a)
      out[a] = it->second.probabilities[a];
  }
};

std::string csv_header() {
  return "stratum,root,turn_board,river_card,range,scheme,trunk,cap,seed,resolve_id,policy,"
         "trunk_rows,trunk_bytes,infosets,nodes,peak_bytes,train_s,certify_s,resolve_wall_s,"
         "walk_s,termination,coldcold_bitwise,warmhint_bitwise,max_margin_diff,max_cert_slack,"
         "total_nc,t_trunk,t_boundary,t_river,t_action,extension_mass,telescope_resid,"
         "nc_endgame_cap1,nc_endgame_cap0,river_argmax_diff,note";
}

}  // namespace

int run_cell(int argc, char** argv) {
  Args args;
  args = parse(argc, argv);
  const StratumSpec* spec = find_stratum(args.stratum);
  if (!spec) {
    std::fprintf(stderr, "rfc0046-cell: unknown stratum %s\n", args.stratum.c_str());
    return 2;
  }

  const poker::GameDef def = make_stratum_def(*spec, args.cap);
  const std::array<int, 4> board{
      cardId(std::string(spec->turn_board[0])), cardId(std::string(spec->turn_board[1])),
      cardId(std::string(spec->turn_board[2])), cardId(std::string(spec->turn_board[3]))};
  const ConcreteCardKeyer keyer(board);
  const std::vector<solver::WeightedHand> range = stratum_range(*spec);
  const char* range_token = std::string_view(spec->name) == "w1" ? "w1" : "full";

  // ---- Locked-input precomputation (outside the resolve wall).
  MarginTable locked_margins;
  std::uint64_t margin_completed = 0;
  if (args.extract_margins) {
    const tree::AbstractTree ref_tree(def, abstraction::ActionAbstraction::identity(args.cap));
    std::vector<std::vector<solver::WeightedHand>> ranges{range, range};
    solver::NSeatTrainerLimits tlim;
    tlim.wall = std::chrono::milliseconds(args.margin_wall_ms);
    const auto reference = solver::train_nseat(ref_tree, ranges, args.margin_iters, args.seed, tlim,
                                               nullptr, nullptr, nullptr, &keyer);
    margin_completed = reference.completed_iterations;
    struct RefPolicy final : PartitionPolicy {
      const ConcreteCardKeyer* keyer = nullptr;
      const solver::NSeatTrainingResult* ref = nullptr;
      void probabilities(std::size_t, std::size_t node0, std::int64_t,
                         const std::array<int, 2>& own, const std::vector<poker::Action>& menu,
                         std::span<double> out) const override {
        const std::uint32_t b = keyer->dense_for_live_pair(own);
        const auto it =
            ref->raw_rows.find(solver::NSeatInformationKey{b, static_cast<std::uint64_t>(node0)});
        const auto uniform = [&] {
          for (std::size_t a = 0; a < menu.size(); ++a)
            out[a] = 1.0 / static_cast<double>(menu.size());
        };
        // The margin REFERENCE is a finite-iteration MCCFR profile evaluated
        // by an exact all-action/all-river fan-out. Its CURRENT strategy at a
        // reached infoset is: average strategy if it accumulated, else RM+
        // over clipped regrets, else uniform for a never-visited infoset
        // (the standard MCCFR current strategy for zero information - not an
        // invented served policy). The SERVED gadget policy remains strict
        // (StrictPolicy throws) so this never masks a Gate-2 coverage miss.
        // Row absent: a never-visited infoset -> uniform current strategy.
        if (it == ref->raw_rows.end()) {
          uniform();
          return;
        }
        // Row present but structurally incompatible: a tree/key mismatch, not
        // a coverage gap - fail loudly (structurally impossible on the
        // same-tree same-key reference).
        if (it->second.actions != menu || it->second.regrets.size() != menu.size())
          throw std::runtime_error("rfc0046-cell: margin reference row/menu mismatch");
        double sum_total = 0.0;
        for (double s : it->second.sums)
          sum_total += s;
        if (sum_total > 0.0) {
          for (std::size_t a = 0; a < menu.size(); ++a)
            out[a] = it->second.sums[a] / sum_total;
          return;
        }
        double positive = 0.0;
        for (double r : it->second.regrets)
          positive += std::max(0.0, r);
        if (positive > 0.0)
          for (std::size_t a = 0; a < menu.size(); ++a)
            out[a] = std::max(0.0, it->second.regrets[a]) / positive;
        else
          uniform();
      }
    } ref_policy;
    ref_policy.keyer = &keyer;
    ref_policy.ref = &reference;
    MarginExtractionInput min;
    min.tree = &ref_tree;
    min.keyer = &keyer;
    min.ranges = {range, range};
    min.reference = &ref_policy;
    for (const auto& [bucket, pair] : extract_per_combo_margins(min))
      locked_margins[bucket] = pair[0];
  }

  // ---- Cold resolve: wall includes online model build + solve + certify.
  const auto wall_start = std::chrono::steady_clock::now();
  const tree::AbstractTree tree(def, abstraction::ActionAbstraction::identity(args.cap));
  const std::size_t tree_nodes = tree.size();

  GadgetInput gin;
  gin.tree = &tree;
  gin.keyer = &keyer;
  gin.hero_range = range;
  gin.responder_range = range;
  gin.margins = locked_margins;  // MUST feed the SAME locked margins into the
                                 // gadget that the certifier checks, else the
                                 // slack scores the wrong augmented game.
  gin.seed = args.seed;
  gin.iterations = args.iters;
  gin.limits.wall =
      std::chrono::milliseconds(args.wall_ms);  // frozen Gate-2 default 10000; --wall-ms for probes
  const GadgetResult solved = run_turn_gadget(gin);

  CertifierInput cin;
  cin.tree = &tree;
  cin.keyer = &keyer;
  cin.hero_range = range;
  cin.responder_range = range;
  cin.policy = &solved.rows;
  cin.margins = locked_margins;
  cin.require_all_margins = args.extract_margins;
  const CertifierResult cert = certify_turn_subgame(cin);
  const auto wall_end = std::chrono::steady_clock::now();
  const double resolve_wall_s = std::chrono::duration<double>(wall_end - wall_start).count();
  const std::int64_t peak_rss = peak_rss_bytes();

  // Typed termination mapping (addendum section 6.2). InvalidInput is runner
  // misconfiguration (never a measurement outcome): fail the process instead
  // of emitting a row the evaluator cannot parse (finding n1).
  if (solved.status == GadgetStatus::InvalidInput) {
    std::fprintf(stderr, "rfc0046-cell: gadget rejected the input configuration\n");
    return 3;
  }
  const char* termination = "Certified";
  const char* note = "final";
  if (solved.status == GadgetStatus::SolveDeadline ||
      solved.status == GadgetStatus::ResourceExhausted) {
    termination =
        solved.status == GadgetStatus::SolveDeadline ? "SolveDeadline" : "ResourceExhausted";
    note = "truncated";
  } else if (!cert.certified) {
    termination =
        cert.reason == CertifierFailReason::MissingRow ? "SolveDeadline" : "CertificationRejected";
    note = "truncated";
  }
  const bool certified_row = std::string_view(note) == "final";

  // Research diagnostic (not a gate column): report certifier progress even on
  // a truncated solve so convergence can be studied without changing the
  // fail-closed CSV semantics.
  std::fprintf(stderr,
               "rfc0046-cell-diag iters=%llu infosets=%zu status=%d cert=%d reason=%d "
               "positive_combos=%zu max_slack_chips=%g worst_bucket=%u wall_s=%.3f "
               "margin_req=%llu margin_done=%llu\n",
               static_cast<unsigned long long>(solved.completed_iterations),
               solved.information_sets, static_cast<int>(solved.status),
               static_cast<int>(cert.certified), static_cast<int>(cert.reason),
               cert.positive_combos, cert.max_slack, cert.worst_bucket, resolve_wall_s,
               static_cast<unsigned long long>(args.margin_iters),
               static_cast<unsigned long long>(margin_completed));

  // ---- Offline chance-free NashConv partition (outside the resolve wall).
  PartitionResult part;
  double walk_s = 0.0;
  const bool do_partition = certified_row || args.probe_nc;
  if (do_partition) {
    StrictPolicy policy;
    policy.keyer = &keyer;
    policy.rows = &solved.rows;
    PartitionInput pin;
    pin.tree0 = &tree;
    pin.keyer = &keyer;
    pin.ranges = {range, range};
    pin.policy = &policy;
    const auto walk_start = std::chrono::steady_clock::now();
    part = partition_nash_conv(pin);
    walk_s = std::chrono::duration<double>(std::chrono::steady_clock::now() - walk_start).count();
  }

  if (!args.quiet)
    std::printf("%s\n", csv_header().c_str());
  const std::string turn_board = std::string(spec->turn_board[0]) + spec->turn_board[1] +
                                 spec->turn_board[2] + spec->turn_board[3];
  const double pot = static_cast<double>(def.pot);
  const double residual_pot = std::max(part.residual_pot[0], part.residual_pot[1]);
  const char* row_note = certified_row ? "final" : (args.probe_nc ? "probe" : "truncated");

  if (do_partition) {
    // Columns 25-32: slack(pot frac), total_nc(BB), t_trunk..t_action(BB),
    // extension(pot frac), residual(pot frac) = 8 numeric fields.
    std::printf(
        "%s,%s,%s,%s,%s,measurement,turn-rows,%u,%llu,r0,hybrid,0,0,%zu,%zu,%lld,0,0,%.6f,%.6f,"
        "%s,,,,%+.9f,%.9f,%.9f,%.9f,%.9f,%.9f,%.9f,%.9f,,,,%s\n",
        spec->name, spec->root_token, turn_board.c_str(), spec->river, range_token,
        static_cast<unsigned>(args.cap), static_cast<unsigned long long>(args.seed),
        solved.information_sets, tree_nodes, static_cast<long long>(peak_rss), resolve_wall_s,
        walk_s, termination, cert.max_slack / pot, part.nash_conv_bb, part.terms_bb[0],
        part.terms_bb[1], part.terms_bb[2], part.terms_bb[3], part.extension_mass_chip / pot,
        residual_pot, row_note);
  } else {
    // Truncated timing-slot row: only structure/resource/timing fields.
    std::printf(
        "%s,%s,%s,%s,%s,measurement,turn-rows,%u,%llu,r0,hybrid,0,0,%zu,%zu,%lld,0,0,%.6f,0,"
        "%s,,,,,,,,,,,,,,,truncated\n",
        spec->name, spec->root_token, turn_board.c_str(), spec->river, range_token,
        static_cast<unsigned>(args.cap), static_cast<unsigned long long>(args.seed),
        solved.information_sets, tree_nodes, static_cast<long long>(peak_rss), resolve_wall_s,
        termination);
  }
  return 0;
}

int main(int argc, char** argv) {
  try {
    return run_cell(argc, argv);
  } catch (const std::exception& e) {
    // A measurement/coverage/config error fails the process with a typed
    // non-zero exit and no CSV row (findings n1/D1): never an abort.
    std::fprintf(stderr, "rfc0046-cell: %s\n", e.what());
    return 4;
  }
}
