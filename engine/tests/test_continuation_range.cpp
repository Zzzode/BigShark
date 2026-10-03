// RFC 0007 (W4b): continuation-range export tests.
//
// The correctness test trains a small preflop flop-terminal policy, exports
// the continuation ranges, and hand-verifies the marginal at a specific flop
// against an independent tree walk. It also checks card removal (a combo
// blocking the flop receives zero reach), normalization, and multi-line
// accumulation (the independent walk sums over every preflop line that
// reaches the flop).
//
// The training-evidence test trains the declared small profile (6 combos,
// 3 BB, flop-terminal — the largest stack within the default 1 GiB byte cap)
// and reports the measured metrics.
#include <array>
#include <bs/abstract_tree.hpp>
#include <bs/abstraction.hpp>
#include <bs/continuation_range.hpp>
#include <bs/eval.hpp>
#include <bs/frontier.hpp>
#include <bs/game_definition.hpp>
#include <bs/nseat_trainer.hpp>
#include <bs/range.hpp>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <set>
#include <vector>

using namespace bs::poker;
using namespace bs::solver;
using namespace bs::tree;

namespace {

#define CHECK(cond)                                                         \
  do {                                                                      \
    if (!(cond)) {                                                          \
      std::printf("CHECK failed: %s (%s:%d)\n", #cond, __FILE__, __LINE__); \
      return false;                                                         \
    }                                                                       \
  } while (0)

bool near(double a, double b, double tol) {
  return std::fabs(a - b) <= tol;
}

int card(const char* name) {
  return bs::cardId(std::string(name));
}

// A trivial frontier evaluator: every frontier leaf has zero chip utility.
class ZeroFrontierEvaluator : public bs::gto::FrontierEvaluator {
 public:
  std::vector<double> evaluate(std::span<const int> flop, std::span<const std::array<int, 2>> hands,
                               const TerminalPayload& ledger) const override {
    (void)flop;
    (void)hands;
    (void)ledger;
    return {0.0, 0.0};
  }
};

// A two-seat preflop flop-terminal game: 3 BB stacks, standard heads-up blinds.
GameDef two_seat_preflop_def() {
  GameDef def{};
  def.player_count = 2;
  def.button = 0;
  def.big_blind = 2;
  def.stacks = {6, 6, 0, 0, 0, 0, 0, 0, 0, 0};
  def.contributions = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
  def.pot = 3;
  def.board = {-1, -1, -1, 0, 0};
  def.board_size = 0;
  def.preflop = true;
  def.blinds_posted = {1, 2, 0, 0, 0, 0, 0, 0, 0, 0};
  def.terminal = TerminalDepth::Flop;
  return def;
}

// Two combos per seat with DIFFERENT buckets (pair vs. high card) so the
// policy distinguishes them. All combos are mutually card-distinct.
std::vector<std::vector<WeightedHand>> two_seat_preflop_ranges() {
  return {
      {{{card("Ks"), card("Kd")}, 3}, {{card("Qd"), card("As")}, 2}},
      {{{card("Jh"), card("Jc")}, 7}, {{card("9h"), card("Tc")}, 5}},
  };
}

// Independent tree walk: compute R(h, o) by walking the preflop tree and
// summing the reach at every flop chance node. This is a separate
// implementation of the export's core reach computation; if the export's
// walk or multi-line accumulation is wrong, the hand-derived marginal below
// disagrees.
double independent_flop_reach(const NSeatPolicy& policy, const AbstractTree& tree,
                              std::size_t node_index, const std::array<int, 2>& hero,
                              const std::array<int, 2>& opp, double reach) {
  const TreeNode& node = tree.node(node_index);
  if (node.is_terminal())
    return 0.0;
  if (node.is_flop_deal())
    return reach;

  const std::size_t actor = node.actor;
  const std::array<int, 2>& own = (actor == 0) ? hero : opp;
  const std::uint32_t bucket =
      static_cast<std::uint32_t>(bs::abstraction::card_bucket(kNSeatCardKind, own, {}));
  const NSeatInformationKey key{bucket, static_cast<std::uint64_t>(node_index)};
  const auto it = policy.rows().find(key);

  std::vector<double> probs;
  if (it != policy.rows().end()) {
    probs = it->second.probabilities;
  } else {
    probs.assign(node.actions.size(), 1.0 / static_cast<double>(node.actions.size()));
  }

  double total = 0.0;
  for (std::size_t a = 0; a < node.actions.size(); ++a) {
    if (probs[a] <= 0.0)
      continue;
    total += independent_flop_reach(policy, tree, node.children[a], hero, opp, reach * probs[a]);
  }
  return total;
}

// ---------------------------------------------------------------------------
// Correctness: hand-derived marginal, card removal, normalization, multi-line.
// ---------------------------------------------------------------------------

bool test_continuation_range() {
  const GameDef def = two_seat_preflop_def();
  const auto ranges = two_seat_preflop_ranges();
  const AbstractTree tree(def, bs::abstraction::ActionAbstraction::identity());
  const ZeroFrontierEvaluator frontier;
  const NSeatTrainingResult trained =
      train_nseat(tree, ranges, 500, 20261003, NSeatTrainerLimits{}, &frontier);
  CHECK(trained.termination == NSeatTerminationPhase::Complete);

  const std::vector<PolicyReachRangePair> exported =
      export_continuation_ranges(trained.policy, tree, ranges);
  CHECK(!exported.empty());

  // Every flop's ranges normalize to one.
  for (const auto& pair : exported) {
    for (std::size_t seat = 0; seat < 2; ++seat) {
      double sum = 0.0;
      for (const auto& [combo, prob] : pair.ranges[seat])
        sum += prob;
      CHECK(near(sum, 1.0, 1e-9));
    }
  }

  // No duplicate flops.
  for (std::size_t i = 1; i < exported.size(); ++i) {
    const auto& prev = exported[i - 1].flop;
    const auto& curr = exported[i].flop;
    CHECK(prev[0] < curr[0] || (prev[0] == curr[0] && prev[1] < curr[1]) ||
          (prev[0] == curr[0] && prev[1] == curr[1] && prev[2] < curr[2]));
  }

  // Hand-derived marginal at a specific flop. Use 2c 3d 7h (doesn't block
  // any combo). Compute R(h,o) independently for all four joint deals, then
  // the expected marginal, and compare against the export.
  const std::array<int, 3> probe_flop{card("2c"), card("3d"), card("7h")};
  const PolicyReachRangePair* probe = nullptr;
  for (const auto& pair : exported) {
    if (pair.flop == probe_flop) {
      probe = &pair;
      break;
    }
  }
  CHECK(probe != nullptr);

  // Normalized priors.
  const double prior_h0 = 3.0 / 5.0;   // KsKd
  const double prior_h1 = 2.0 / 5.0;   // QdAs
  const double prior_o0 = 7.0 / 12.0;  // JhJc
  const double prior_o1 = 5.0 / 12.0;  // 9hTc

  const std::array<int, 2> h0{card("Ks"), card("Kd")};
  const std::array<int, 2> h1{card("Qd"), card("As")};
  const std::array<int, 2> o0{card("Jh"), card("Jc")};
  const std::array<int, 2> o1{card("9h"), card("Tc")};

  const double r00 = independent_flop_reach(trained.policy, tree, tree.root_index(), h0, o0, 1.0);
  const double r01 = independent_flop_reach(trained.policy, tree, tree.root_index(), h0, o1, 1.0);
  const double r10 = independent_flop_reach(trained.policy, tree, tree.root_index(), h1, o0, 1.0);
  const double r11 = independent_flop_reach(trained.policy, tree, tree.root_index(), h1, o1, 1.0);

  // Expected hero marginal for h0 (KsKd).
  const double z_h0 = prior_h0 * prior_o0 * r00 + prior_h0 * prior_o1 * r01;
  const double z_h1 = prior_h1 * prior_o0 * r10 + prior_h1 * prior_o1 * r11;
  const double expected_h0 = z_h0 / (z_h0 + z_h1);

  const int idx_h0 = bs::comboIndex(h0[0], h0[1]);
  const auto it = probe->ranges[0].find(idx_h0);
  CHECK(it != probe->ranges[0].end());
  CHECK(near(it->second, expected_h0, 1e-9));

  // Card removal: a flop containing Ks blocks the KsKd combo, so its reach
  // at that flop is zero (absent from the map).
  const std::array<int, 3> blocking_flop{card("Ks"), card("2c"), card("3d")};
  const PolicyReachRangePair* blocked = nullptr;
  for (const auto& pair : exported) {
    if (pair.flop == blocking_flop) {
      blocked = &pair;
      break;
    }
  }
  // The flop might have zero total reach (if the policy always folds), but
  // if it appears, KsKd must be absent.
  if (blocked) {
    CHECK(blocked->ranges[0].find(idx_h0) == blocked->ranges[0].end());
  }

  return true;
}

// ---------------------------------------------------------------------------
// Training evidence: the declared small profile (6 combos, flop-terminal)
// at 25 BB — a realistic stack depth.
//
// The virtual flop deal (FlopDeal leaf) replaces the 3-level chance subtree
// (52*51*50 = 132,600 frontier leaves per preflop line) with a single leaf,
// so the tree has only hundreds of nodes at any stack depth. Before this
// change the tree exceeded an 8 GiB cap at 25 BB; now it fits in under 1 MB.
// The RFC 0007 measured 606 info sets is the ABSTRACTED count (conditioned on
// hole-card buckets), not the raw tree node count.
// ---------------------------------------------------------------------------

bool test_preflop_training_evidence() {
  GameDef def{};
  def.player_count = 2;
  def.button = 0;
  def.big_blind = 2;
  def.stacks = {50, 50, 0, 0, 0, 0, 0, 0, 0, 0};  // 25 BB
  def.contributions = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
  def.pot = 3;
  def.board = {-1, -1, -1, 0, 0};
  def.board_size = 0;
  def.preflop = true;
  def.blinds_posted = {1, 2, 0, 0, 0, 0, 0, 0, 0, 0};
  def.terminal = TerminalDepth::Flop;

  // 6 combos per seat: AA, KK, QQ, JJ, AKs, AKo. Cards must be sorted
  // (cards[0] < cards[1]) per the WeightedHand contract.
  const std::vector<std::vector<WeightedHand>> ranges = {
      {{{card("As"), card("Ad")}, 1},
       {{card("Ks"), card("Kd")}, 1},
       {{card("Qs"), card("Qd")}, 1},
       {{card("Js"), card("Jd")}, 1},
       {{card("Ks"), card("As")}, 1},
       {{card("Kc"), card("Ad")}, 1}},
      {{{card("Ah"), card("Ac")}, 1},
       {{card("Kh"), card("Kc")}, 1},
       {{card("Qh"), card("Qc")}, 1},
       {{card("Jh"), card("Jc")}, 1},
       {{card("Kh"), card("Ah")}, 1},
       {{card("Kd"), card("Ac")}, 1}},
  };

  const AbstractTree tree(def, bs::abstraction::ActionAbstraction::identity());
  const ZeroFrontierEvaluator frontier;

  const auto start = std::chrono::steady_clock::now();
  const NSeatTrainingResult trained =
      train_nseat(tree, ranges, 1000, 20261003, NSeatTrainerLimits{}, &frontier);
  const auto elapsed = std::chrono::steady_clock::now() - start;
  const double wall_s = std::chrono::duration<double>(elapsed).count();

  std::printf("[evidence] declared small profile (6 combos, 25 BB, flop-terminal)\n");
  std::printf("[evidence]   termination:     %s\n",
              trained.termination == NSeatTerminationPhase::Complete ? "Complete" : "INCOMPLETE");
  std::printf("[evidence]   iterations:      %llu\n",
              static_cast<unsigned long long>(trained.completed_iterations));
  std::printf("[evidence]   tree nodes:      %zu\n", trained.nodes);
  std::printf("[evidence]   information sets: %zu\n", trained.information_sets);
  std::printf("[evidence]   accounted bytes: %zu\n", trained.accounted_bytes);
  std::printf("[evidence]   wall time:       %.2f s\n", wall_s);

  CHECK(trained.termination == NSeatTerminationPhase::Complete);
  CHECK(trained.information_sets > 0);
  CHECK(trained.nodes > 0);

  // The continuation-range export also runs on the declared profile.
  const std::vector<PolicyReachRangePair> exported =
      export_continuation_ranges(trained.policy, tree, ranges);
  std::printf("[evidence]   flops exported:  %zu\n", exported.size());
  CHECK(!exported.empty());

  return true;
}

// ---------------------------------------------------------------------------
// Full-range 100 BB measurement: all 1,326 combos per seat at 100 BB.
//
// This tests whether the virtual flop deal + Preflop169 card abstraction
// enables training at a realistic stack depth with the complete preflop
// range. The key measurement is the information-set count, which depends on
// the card bucket system: Preflop169 produces 169 distinct preflop buckets
// (13 pairs + 78 suited + 78 offsuit), versus CategoryTiersV1's 2 (pair vs.
// high card).
// ---------------------------------------------------------------------------

bool test_preflop_full_range_100bb() {
  GameDef def{};
  def.player_count = 2;
  def.button = 0;
  def.big_blind = 2;
  def.stacks = {200, 200, 0, 0, 0, 0, 0, 0, 0, 0};  // 100 BB
  def.contributions = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
  def.pot = 3;
  def.board = {-1, -1, -1, 0, 0};
  def.board_size = 0;
  def.preflop = true;
  def.blinds_posted = {1, 2, 0, 0, 0, 0, 0, 0, 0, 0};
  def.terminal = TerminalDepth::Flop;

  // All 1,326 combos per seat (C(52,2) = 1,326).
  std::vector<std::vector<WeightedHand>> ranges(2);
  ranges[0].reserve(1326);
  ranges[1].reserve(1326);
  for (int c0 = 0; c0 < 52; ++c0) {
    for (int c1 = c0 + 1; c1 < 52; ++c1) {
      ranges[0].push_back({{c0, c1}, 1.0});
      ranges[1].push_back({{c0, c1}, 1.0});
    }
  }

  const AbstractTree tree(def, bs::abstraction::ActionAbstraction::identity());

  // Count action nodes.
  std::size_t action_nodes = 0;
  for (const auto& n : tree.nodes()) {
    if (n.is_action())
      ++action_nodes;
  }

  // Count distinct preflop buckets under CategoryTiersV1 and Identity.
  std::set<std::uint32_t> tiers_buckets;
  std::set<std::uint32_t> identity_buckets;
  for (const auto& wh : ranges[0]) {
    tiers_buckets.insert(bs::abstraction::card_bucket(kNSeatCardKind, wh.cards, {}));
    identity_buckets.insert(
        bs::abstraction::card_bucket(bs::abstraction::CardBucketKind::Identity, wh.cards, {}));
  }

  std::printf("[full-range] 100 BB, 1326 combos/seat\n");
  std::printf("[full-range]   tree nodes:       %zu (%zu action)\n", tree.size(), action_nodes);
  std::printf("[full-range]   accounted bytes:  %zu\n", tree.accounted_bytes());
  std::printf("[full-range]   kNSeatCardKind buckets: %zu", tiers_buckets.size());
  std::printf(" (values:");
  for (auto b : tiers_buckets)
    std::printf(" %u", b);
  std::printf(")\n");
  std::printf("[full-range]   Identity buckets:      %zu\n", identity_buckets.size());

  // Train with a 5-minute wall clock.
  const ZeroFrontierEvaluator frontier;
  NSeatTrainerLimits limits;
  limits.wall = std::chrono::minutes(5);

  const auto start = std::chrono::steady_clock::now();
  const NSeatTrainingResult trained = train_nseat(tree, ranges, 10000, 20261003, limits, &frontier);
  const auto elapsed = std::chrono::steady_clock::now() - start;
  const double wall_s = std::chrono::duration<double>(elapsed).count();

  std::printf("[full-range]   termination:      %s\n",
              trained.termination == NSeatTerminationPhase::Complete    ? "Complete"
              : trained.termination == NSeatTerminationPhase::WallClock ? "WallClock"
                                                                        : "ResourceLimit");
  std::printf("[full-range]   iterations:       %llu\n",
              static_cast<unsigned long long>(trained.completed_iterations));
  std::printf("[full-range]   information sets: %zu\n", trained.information_sets);
  std::printf("[full-range]   accounted bytes:  %zu\n", trained.accounted_bytes);
  std::printf("[full-range]   wall time:        %.2f s\n", wall_s);

  CHECK(trained.termination != NSeatTerminationPhase::ResourceLimit);
  CHECK(trained.information_sets > 0);
  CHECK(trained.nodes > 0);

  return true;
}

}  // namespace

int main() {
  if (!test_continuation_range()) {
    std::printf("test_continuation_range FAILED\n");
    return 1;
  }
  std::printf("[continuation-range] correctness passed\n");

  if (!test_preflop_training_evidence()) {
    std::printf("test_preflop_training_evidence FAILED\n");
    return 1;
  }
  std::printf("[continuation-range] training evidence passed\n");

  if (!test_preflop_full_range_100bb()) {
    std::printf("test_preflop_full_range_100bb FAILED\n");
    return 1;
  }
  std::printf("[continuation-range] full-range 100 BB passed\n");

  std::printf("continuation-range: all tests passed\n");
  return 0;
}
