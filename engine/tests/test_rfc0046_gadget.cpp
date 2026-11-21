// RFC 0046 Stage-1 end-to-end smoke tests for the turn-boundary gadget, the
// multi-street certifier, and the per-infoset partition walker, on a small
// cap-1 turn-rooted two-seat tree with reduced ranges.
//
// Pinned properties:
//   1. gadget solves are bitwise deterministic under a fixed seed;
//   2. the partition telescope identity closes within 1e-9 root-pot,
//      T_trunk is exactly 0 (no pre-turn node), and extension-region mass is
//      exactly 0 on a cap-1-only walk;
//   3. a solved concrete profile drives chance-free NashConv down versus a
//      short run (gadget mechanics are not grossly wrong);
//   4. the certifier enumerates every positive-mass responder combo and
//      certifies when the locked margin equals the measured BR continuation.
#include <algorithm>
#include <array>
#include <bit>
#include <bs/abstract_tree.hpp>
#include <bs/abstraction.hpp>
#include <bs/eval.hpp>
#include <bs/game_definition.hpp>
#include <bs/heads_up_solver.hpp>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <map>
#include <span>
#include <string>
#include <vector>

#include "../benchmarks/rfc0046/certifier.hpp"
#include "../benchmarks/rfc0046/concrete_keyer.hpp"
#include "../benchmarks/rfc0046/margin_extract.hpp"
#include "../benchmarks/rfc0046/partition_walker.hpp"
#include "../benchmarks/rfc0046/turn_gadget.hpp"

#define CHECK(cond)                                                         \
  do {                                                                      \
    if (!(cond)) {                                                          \
      std::printf("CHECK failed: %s (%s:%d)\n", #cond, __FILE__, __LINE__); \
      return false;                                                         \
    }                                                                       \
  } while (0)

namespace {
using namespace bs;
using namespace bs::poker;
using namespace bs::rfc0046;

int card(const char* name) {
  return cardId(std::string(name));
}

GameDef turn_def() {
  GameDef def{};
  def.player_count = 2;
  def.button = 0;
  def.big_blind = 2;
  def.stacks = {20, 20};
  def.contributions = {1, 1};
  def.pot = 2;
  def.board = {card("2c"), card("3d"), card("7h"), card("Js"), 0};
  def.board_size = 4;
  def.preflop = false;
  def.terminal = TerminalDepth::River;
  return def;
}

// A fixed small set of combos off the board, per seat. Deals are
// blocker-filtered by the gadget/walker.
std::vector<solver::WeightedHand> small_range() {
  static const char* names[][2] = {
      {"Qs", "Kh"}, {"As", "9d"}, {"Ts", "8c"}, {"5h", "5d"},
      {"Jc", "2s"}, {"Ad", "Kc"}, {"7s", "6h"}, {"Qd", "Tc"},
  };
  std::vector<solver::WeightedHand> out;
  for (auto& [a, b] : names) {
    int x = card(a);
    int y = card(b);
    if (x > y)
      std::swap(x, y);
    out.push_back(solver::WeightedHand{{x, y}, 1.0});
  }
  return out;
}

class GadgetPolicy final : public PartitionPolicy {
 public:
  const ConcreteCardKeyer* keyer = nullptr;
  const std::map<solver::NSeatInformationKey, GadgetRow>* rows = nullptr;

  void probabilities(std::size_t /*seat*/, std::size_t node0, std::int64_t node1,
                     const std::array<int, 2>& own, const std::vector<Action>& menu,
                     std::span<double> out) const override {
    const std::uint32_t bucket = keyer->dense_for_live_pair(own);
    const auto index =
        node1 < 0 ? static_cast<std::uint64_t>(node0) : static_cast<std::uint64_t>(node1);
    const auto it = rows->find(solver::NSeatInformationKey{bucket, index});
    if (it != rows->end() && it->second.actions == menu) {
      for (std::size_t a = 0; a < menu.size(); ++a)
        out[a] = it->second.probabilities[a];
    } else {
      for (std::size_t a = 0; a < menu.size(); ++a)
        out[a] = 1.0 / static_cast<double>(menu.size());
    }
  }
};

bool bitwise_equal(const std::map<solver::NSeatInformationKey, GadgetRow>& a,
                   const std::map<solver::NSeatInformationKey, GadgetRow>& b) {
  if (a.size() != b.size())
    return false;
  auto jb = b.begin();
  for (auto ia = a.begin(); ia != a.end(); ++ia, ++jb)
    if (ia->first != jb->first || ia->second.actions != jb->second.actions ||
        ia->second.probabilities.size() != jb->second.probabilities.size())
      return false;
    else
      for (std::size_t i = 0; i < ia->second.probabilities.size(); ++i)
        if (std::bit_cast<std::uint64_t>(ia->second.probabilities[i]) !=
            std::bit_cast<std::uint64_t>(jb->second.probabilities[i]))
          return false;
  return true;
}

bool test_determinism_and_convergence() {
  const GameDef def = turn_def();
  const tree::AbstractTree tree(def, abstraction::ActionAbstraction::identity(1));
  const std::array<int, 4> board{card("2c"), card("3d"), card("7h"), card("Js")};
  const ConcreteCardKeyer keyer(board);
  const auto range = small_range();
  MarginTable zero_margins;

  GadgetInput in;
  in.tree = &tree;
  in.keyer = &keyer;
  in.hero = 0;
  in.responder = 1;
  in.hero_range = range;
  in.responder_range = range;
  in.margins = zero_margins;
  in.seed = 4320;
  in.iterations = 5000;
  in.limits.wall = std::chrono::seconds(60);

  const GadgetResult a = run_turn_gadget(in);
  const GadgetResult b = run_turn_gadget(in);
  CHECK(a.status == GadgetStatus::Certified);
  CHECK(b.status == GadgetStatus::Certified);
  CHECK(bitwise_equal(a.rows, b.rows));
  CHECK(a.information_sets > 0);

  GadgetPolicy policy;
  policy.keyer = &keyer;
  policy.rows = &a.rows;
  PartitionInput pin;
  pin.tree0 = &tree;
  pin.keyer = &keyer;
  pin.ranges = {range, range};
  pin.policy = &policy;
  const PartitionResult part = partition_nash_conv(pin);
  CHECK(std::isfinite(part.nash_conv_bb));
  CHECK(part.nash_conv_bb >= 0.0);
  CHECK(part.terms_chip[static_cast<std::size_t>(PartitionTerm::Trunk)] == 0.0);
  CHECK(part.extension_mass_chip == 0.0);
  CHECK(part.residual_pot[0] <= 1e-9);
  CHECK(part.residual_pot[1] <= 1e-9);
  for (const double t : part.terms_bb)
    CHECK(std::isfinite(t) && t >= -1e-12);

  // B4: the frozen metric is SUMMED BB - chip quantity divided by BB only,
  // never by live-seat count.
  const double bb = static_cast<double>(def.big_blind);
  CHECK(std::abs(part.nash_conv_bb - part.nash_conv_chip / bb) < 1e-9);
  for (std::size_t i = 0; i < 4; ++i)
    CHECK(std::abs(part.terms_bb[i] - part.terms_chip[i] / bb) < 1e-9);

  // A longer solve must not raise NashConv; expect the gadget to reduce
  // exploitability meaningfully versus a 500-iteration run on this small game.
  GadgetInput short_in = in;
  short_in.iterations = 500;
  const GadgetResult s = run_turn_gadget(short_in);
  CHECK(s.status == GadgetStatus::Certified);
  GadgetPolicy short_policy;
  short_policy.keyer = &keyer;
  short_policy.rows = &s.rows;
  PartitionInput spin = pin;
  spin.policy = &short_policy;
  const PartitionResult short_part = partition_nash_conv(spin);
  CHECK(part.nash_conv_bb <= short_part.nash_conv_bb + 1e-9);
  return true;
}

bool test_certifier_mechanics() {
  const GameDef def = turn_def();
  const tree::AbstractTree tree(def, abstraction::ActionAbstraction::identity(1));
  const std::array<int, 4> board{card("2c"), card("3d"), card("7h"), card("Js")};
  const ConcreteCardKeyer keyer(board);
  const auto range = small_range();

  GadgetInput in;
  in.tree = &tree;
  in.keyer = &keyer;
  in.hero = 0;
  in.responder = 1;
  in.hero_range = range;
  in.responder_range = range;
  in.seed = 4321;
  in.iterations = 8000;
  in.limits.wall = std::chrono::seconds(60);
  const GadgetResult solved = run_turn_gadget(in);
  CHECK(solved.status == GadgetStatus::Certified);

  CertifierInput cin;
  cin.tree = &tree;
  cin.keyer = &keyer;
  cin.hero = 0;
  cin.responder = 1;
  cin.hero_range = range;
  cin.responder_range = range;
  cin.policy = &solved.rows;

  // Every enumerated combo that keeps a compatible hero hand is present.
  const CertifierResult raw = certify_turn_subgame(cin);
  CHECK(raw.positive_combos > 0);
  CHECK(std::isfinite(raw.max_slack));
  CHECK(raw.margins.size() == raw.positive_combos);

  // Lock each margin at the certifier's own measured BR continuation: the
  // responder inequality then certifies by construction.
  MarginTable br_margins;
  for (const auto& [bucket, pair] : raw.margins)
    br_margins[bucket] = pair[1];
  cin.margins = br_margins;
  const CertifierResult self = certify_turn_subgame(cin);
  CHECK(self.certified);
  CHECK(self.max_slack <= 1e-9 * static_cast<double>(def.pot) + 1e-12);
  return true;
}

// Locked-margin safety direction: margins extracted from a converged
// REFERENCE profile (production train_nseat through the ConcreteCards seam)
// are locked for ONE gadget solve (the BS safe-subgame flow; the margin is an
// external trunk value, never an extract/resolve fixed point). The certifier
// slack (responder BR continuation minus locked margin) must be finite and
// strictly smaller than the slack under zero margins, which grossly
// mis-specify the responder's terminate payoff.
bool test_locked_margin_safety_direction() {
  const GameDef def = turn_def();
  const tree::AbstractTree tree(def, abstraction::ActionAbstraction::identity(1));
  const std::array<int, 4> board{card("2c"), card("3d"), card("7h"), card("Js")};
  const ConcreteCardKeyer keyer(board);
  const auto range = small_range();
  std::vector<std::vector<solver::WeightedHand>> ranges{range, range};

  // Reference profile: production MCCFR through the exact-key seam.
  const std::uint64_t ref_iters =
      std::getenv("RFC0046_REF_ITERS") ? std::stoull(std::getenv("RFC0046_REF_ITERS")) : 20000;
  const std::uint64_t gadget_iters =
      std::getenv("RFC0046_G_ITERS") ? std::stoull(std::getenv("RFC0046_G_ITERS")) : 10000;
  solver::NSeatTrainerLimits tlim;
  tlim.wall = std::chrono::seconds(600);
  const auto reference =
      solver::train_nseat(tree, ranges, ref_iters, 4320, tlim, nullptr, nullptr, nullptr, &keyer);
  CHECK(reference.completed_iterations == ref_iters);

  // Adapt the raw average table to the PartitionPolicy/extraction interface.
  struct RefPolicy final : PartitionPolicy {
    const ConcreteCardKeyer* keyer = nullptr;
    const solver::NSeatTrainingResult* ref = nullptr;
    void probabilities(std::size_t, std::size_t node0, std::int64_t, const std::array<int, 2>& own,
                       const std::vector<Action>& menu, std::span<double> out) const override {
      const std::uint32_t bucket = keyer->dense_for_live_pair(own);
      const auto it = ref->raw_rows.find(
          solver::NSeatInformationKey{bucket, static_cast<std::uint64_t>(node0)});
      if (it == ref->raw_rows.end() || it->second.actions != menu) {
        for (std::size_t a = 0; a < menu.size(); ++a)
          out[a] = 1.0 / static_cast<double>(menu.size());
        return;
      }
      double total = 0.0;
      for (double s : it->second.sums)
        total += s;
      if (total > 0.0)
        for (std::size_t a = 0; a < menu.size(); ++a)
          out[a] = it->second.sums[a] / total;
      else
        for (std::size_t a = 0; a < menu.size(); ++a)
          out[a] = 1.0 / static_cast<double>(menu.size());
    }
  } ref_policy;
  ref_policy.keyer = &keyer;
  ref_policy.ref = &reference;

  MarginExtractionInput min;
  min.tree = &tree;
  min.keyer = &keyer;
  min.hero = 0;
  min.responder = 1;
  min.ranges = {range, range};
  min.reference = &ref_policy;
  const auto extracted = extract_per_combo_margins(min);
  CHECK(!extracted.empty());

  auto certify_with = [&](const MarginTable& margins, double& slack_out) -> bool {
    GadgetInput in;
    in.tree = &tree;
    in.keyer = &keyer;
    in.hero_range = range;
    in.responder_range = range;
    in.margins = margins;
    in.seed = 4322;
    in.iterations = gadget_iters;
    in.limits.wall = std::chrono::seconds(600);
    const GadgetResult solved = run_turn_gadget(in);
    if (solved.status != GadgetStatus::Certified)
      return false;
    CertifierInput cin;
    cin.tree = &tree;
    cin.keyer = &keyer;
    cin.hero_range = range;
    cin.responder_range = range;
    cin.policy = &solved.rows;
    cin.margins = margins;
    const CertifierResult cr = certify_turn_subgame(cin);
    slack_out = cr.max_slack;
    return true;
  };

  MarginTable locked;
  for (const auto& [bucket, pair] : extracted)
    locked[bucket] = pair[0];
  double locked_slack = 0.0;
  if (!certify_with(locked, locked_slack))
    return false;
  double zero_slack = 0.0;
  if (!certify_with(MarginTable{}, zero_slack))
    return false;
  std::printf("locked-margin slack=%g  zero-margin slack=%g\n", locked_slack, zero_slack);
  CHECK(std::isfinite(locked_slack));
  CHECK(locked_slack < zero_slack);
  return true;
}

}  // namespace

int main() {
  const char* only = std::getenv("RFC0046_ONLY");
  if (!only || std::string(only) != "locked") {
    if (!test_determinism_and_convergence()) {
      std::printf("rfc0046_gadget: FAIL determinism/convergence\n");
      return 1;
    }
    std::printf("rfc0046_gadget: pass determinism/convergence\n");
    if (!test_certifier_mechanics()) {
      std::printf("rfc0046_gadget: FAIL certifier mechanics\n");
      return 1;
    }
    std::printf("rfc0046_gadget: pass certifier mechanics\n");
  }
  if (!test_locked_margin_safety_direction()) {
    std::printf("rfc0046_gadget: FAIL locked-margin safety direction\n");
    return 1;
  }
  std::printf("rfc0046_gadget: pass locked-margin safety direction\n");
  std::printf("rfc0046_gadget: all tests passed\n");
  return 0;
}
