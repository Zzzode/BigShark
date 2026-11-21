// RFC 0008 stage 6 R12 step 4: the full-hand N-seat simulator and the exact
// small-game oracle. Pins:
//   * exact terminal utility conservation (zero-sum) on fold and showdown
//     hands for 2 and 3 seats;
//   * determinism from seed + hand_id + predetermined board;
//   * TOTAL typed geometry coverage (an uncovered candidate flop throws
//     stage6_geometry_uncovered; a covered one does not);
//   * the exact 3p oracle at three_seat_rooted({1,1,1}) and {1,2,2}: expected
//     utility is zero-sum, the joint table rejects overlapping combos, and an
//     all-three-to-river line exists;
//   * illegal-policy rejection.
#include <bs/behavior_policy.hpp>
#include <bs/eval.hpp>
#include <bs/game_definition.hpp>
#include <bs/multiway_sampler.hpp>
#include <bs/prng.hpp>
#include <bs/stage6/exact_oracle.hpp>
#include <bs/stage6/geometry.hpp>
#include <bs/stage6/geometry_enumerator.hpp>
#include <bs/stage6/simulator.hpp>
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

using namespace bs::stage6;
using bs::poker::Chips;
using bs::poker::GameDef;
using bs::poker::GameState;
using bs::poker::LegalActions;
namespace poker = bs::poker;

namespace {
int failures = 0;
void check(bool c, const std::string& d) {
  if (!c) {
    std::fprintf(stderr, "FAIL: %s\n", d.c_str());
    ++failures;
  }
}

int card(const char* name) {
  return bs::cardId(std::string(name));
}

// Deterministic passive policy: check, else call (Call carries target_total
// 0), else fold. Never raises. Distribution is a single legal action with
// probability one.
class CheckCallPolicy final : public BehaviorPolicy {
 public:
  std::vector<PolicyAction> distribution(const GameState& state, std::size_t, HoleCards,
                                         const PolicyContext&) const override {
    const LegalActions legal = state.legal();
    poker::Action a;
    if (legal.check)
      a = {poker::ActionType::Check, 0};
    else if (legal.call)
      a = {poker::ActionType::Call, 0};
    else
      a = {poker::ActionType::Fold, 0};
    if (!legal.contains(a))
      throw std::runtime_error("check/call policy produced an illegal action");
    return {{a, 1.0}};
  }
};

// Deterministic aggressive policy: open/raise to the legal maximum when an
// aggressive interval exists, otherwise check/call. Exercises multi-raise and
// (with unequal stacks) side-pot geometry.
class JamPolicy final : public BehaviorPolicy {
 public:
  std::vector<PolicyAction> distribution(const GameState& state, std::size_t, HoleCards,
                                         const PolicyContext&) const override {
    const LegalActions legal = state.legal();
    poker::Action a;
    if (legal.aggressive)
      a = {legal.aggressive->type, legal.aggressive->maximum};
    else if (legal.check)
      a = {poker::ActionType::Check, 0};
    else if (legal.call)
      a = {poker::ActionType::Call, 0};
    else
      a = {poker::ActionType::Fold, 0};
    if (!legal.contains(a))
      throw std::runtime_error("jam policy produced an illegal action");
    return {{a, 1.0}};
  }
};

// A policy that returns an illegal action, to prove the simulator rejects it.
class IllegalPolicy final : public BehaviorPolicy {
 public:
  std::vector<PolicyAction> distribution(const GameState&, std::size_t, HoleCards,
                                         const PolicyContext&) const override {
    return {{{poker::ActionType::Raise, 999999}, 1.0}};
  }
};

// Deterministic fold policy: fold when facing a wager, check otherwise. The
// "always-fold baseline" of the information-restriction negative control.
class FoldPolicy final : public BehaviorPolicy {
 public:
  std::vector<PolicyAction> distribution(const GameState& state, std::size_t, HoleCards,
                                         const PolicyContext&) const override {
    const LegalActions legal = state.legal();
    poker::Action a;
    if (legal.check)
      a = {poker::ActionType::Check, 0};
    else
      a = {poker::ActionType::Fold, 0};
    if (!legal.contains(a))
      throw std::runtime_error("fold policy produced an illegal action");
    return {{a, 1.0}};
  }
};

GameDef preflop_def(std::size_t n, Chips bb, Chips stack) {
  GameDef def{};
  def.player_count = n;
  def.button = 0;
  def.big_blind = bb;
  def.preflop = true;
  for (std::size_t i = 0; i < n; ++i)
    def.stacks[i] = stack;
  std::array<Chips, 10> blinds{};
  if (n == 2) {
    blinds[0] = bb / 2;
    blinds[1] = bb;
  } else {
    blinds[(0 + 1) % n] = bb / 2;
    blinds[(0 + 2) % n] = bb;
  }
  def.blinds_posted = blinds;
  Chips pot = 0;
  for (Chips b : blinds)
    pot += b;
  def.pot = pot;
  def.board = {-1, -1, -1, -1, -1};
  return def;
}

GameDef three_seat_rooted(std::array<Chips, 3> stacks) {
  GameDef d{};
  d.player_count = 3;
  d.button = 0;
  d.big_blind = 2;
  for (std::size_t i = 0; i < 3; ++i)
    d.stacks[i] = stacks[i];
  d.contributions = {1, 1, 1, 0, 0, 0, 0, 0, 0, 0};
  d.pot = 3;
  d.board = {card("2c"), card("3d"), card("7h"), 0, 0};
  d.board_size = 3;
  return d;
}

double util_sum(const std::array<double, 10>& u, std::size_t n) {
  double s = 0;
  for (std::size_t i = 0; i < n; ++i)
    s += u[i];
  return s;
}

void test_simulator_three_seat_showdown() {
  CheckCallPolicy policy;
  std::vector<const BehaviorPolicy*> policies{&policy, &policy, &policy};
  bs::SplitMix64 rng(12345);
  HandSimulator sim(policies, nullptr, rng);

  GameDef def = preflop_def(3, 2, 200);
  // Board 2c3d7h4d5h: seat0's A makes a wheel; the other two hold K/Q high, so
  // there is a single winner (the three-way passive hand does not chop).
  std::vector<HoleCards> holes{
      {{card("As"), card("Ks")}}, {{card("Kd"), card("Qd")}}, {{card("Kh"), card("Qh")}}};
  std::array<int, 5> board{card("2c"), card("3d"), card("7h"), card("4d"), card("5h")};

  const SimResult r = sim.run(def, holes, board, 0);
  check(!r.folded, "three passive seats limp and reach showdown");
  check(std::abs(util_sum(r.utility, 3)) < 1e-9, "showdown utility is zero-sum");
  // The wheel wins the 6-chip limped pot: seat0 gains 4 (won 6, put in 2) and
  // each blind loses 2.
  check(std::abs(r.utility[0] - 4.0) < 1e-9, "seat0's wheel wins the limped pot");
  check(std::abs(r.utility[1] + 2.0) < 1e-9 && std::abs(r.utility[2] + 2.0) < 1e-9,
        "the two blinds lose their called chips");
}

void test_simulator_determinism() {
  JamPolicy policy;
  std::vector<const BehaviorPolicy*> policies{&policy, &policy, &policy};
  GameDef def = preflop_def(3, 2, 200);
  std::vector<HoleCards> holes{
      {{card("As"), card("Ks")}}, {{card("Qd"), card("Jd")}}, {{card("Tc"), card("9c")}}};
  std::array<int, 5> board{card("2c"), card("3d"), card("7h"), card("4s"), card("5s")};

  bs::SplitMix64 a(777), b(777);
  HandSimulator sa(policies, nullptr, a);
  HandSimulator sb(policies, nullptr, b);
  const SimResult ra = sa.run(def, holes, board, 42);
  const SimResult rb = sb.run(def, holes, board, 42);
  bool same = ra.folded == rb.folded;
  for (std::size_t i = 0; i < 3; ++i)
    same = same && ra.utility[i] == rb.utility[i];
  check(same, "same seed/hand_id/board gives identical utility");
  check(std::abs(util_sum(ra.utility, 3)) < 1e-9, "aggressive hand settles zero-sum");
}

void test_simulator_coverage_total() {
  CheckCallPolicy policy;
  std::vector<const BehaviorPolicy*> policies{&policy, &policy, &policy};
  GameDef def = preflop_def(3, 2, 200);
  std::vector<HoleCards> holes{
      {{card("As"), card("Ks")}}, {{card("Ad"), card("Kd")}}, {{card("Ac"), card("Kc")}}};
  std::array<int, 5> board{card("2c"), card("3d"), card("7h"), card("4s"), card("5s")};

  // Empty coverage: the reached limped flop is uncovered and must throw the
  // typed refusal (never a clamp or skip).
  {
    GeometryCoverage empty(std::vector<GeometrySignature>{});
    bs::SplitMix64 rng(1);
    HandSimulator sim(policies, &empty, rng);
    bool threw = false;
    try {
      sim.run(def, holes, board, 0);
    } catch (const stage6_geometry_uncovered&) {
      threw = true;
    }
    check(threw, "an uncovered candidate flop throws stage6_geometry_uncovered");
  }
  // Coverage that contains the exact reached limped-flop signature: after a
  // three-way limp each seat has 198 behind, contributed 2, pot 6, all live.
  // (The chart/deviation matrix never limps, so total lookup is tested against
  // the concrete reached signature rather than chart reach.)
  {
    GeometrySignature limp;
    limp.player_count = 3;
    limp.pot = 6;
    limp.stacks = {198, 198, 198, 0, 0, 0, 0, 0, 0, 0};
    limp.contributed = {2, 2, 2, 0, 0, 0, 0, 0, 0, 0};
    limp.live = {0, 1, 2};
    GeometryCoverage covered(std::vector<GeometrySignature>{limp});
    bs::SplitMix64 rng(1);
    HandSimulator sim(policies, &covered, rng);
    bool ok = true;
    try {
      sim.run(def, holes, board, 0);
    } catch (const stage6_geometry_uncovered&) {
      ok = false;
    }
    check(ok, "a reached flop whose signature is covered does not throw");
  }
}

void test_simulator_two_seat_fold() {
  // One aggressive seat opens; a policy that always folds yields a fold
  // terminal and zero-sum.
  JamPolicy jam;
  class AlwaysFold final : public BehaviorPolicy {
   public:
    std::vector<PolicyAction> distribution(const GameState& state, std::size_t, HoleCards,
                                           const PolicyContext&) const override {
      poker::Action a{poker::ActionType::Fold, 0};
      if (!state.legal().contains(a))
        a = {poker::ActionType::Check, 0};
      return {{a, 1.0}};
    }
  } fold_policy;
  std::vector<const BehaviorPolicy*> policies{&jam, &fold_policy};
  GameDef def = preflop_def(2, 2, 200);
  std::vector<HoleCards> holes{{{card("As"), card("Ks")}}, {{card("2d"), card("7d")}}};
  std::array<int, 5> board{card("2c"), card("3d"), card("7h"), card("4s"), card("5s")};
  bs::SplitMix64 rng(9);
  HandSimulator sim(policies, nullptr, rng);
  const SimResult r = sim.run(def, holes, board, 0);
  check(r.folded, "2-seat hand ends by a fold");
  check(std::abs(util_sum(r.utility, 2)) < 1e-9, "fold utility is zero-sum");
}

void test_simulator_rejects_illegal_policy() {
  IllegalPolicy bad;
  CheckCallPolicy good;
  std::vector<const BehaviorPolicy*> policies{&good, &bad};
  GameDef def = preflop_def(2, 2, 200);
  std::vector<HoleCards> holes{{{card("As"), card("Ks")}}, {{card("Ad"), card("Kd")}}};
  std::array<int, 5> board{card("2c"), card("3d"), card("7h"), card("4s"), card("5s")};
  bs::SplitMix64 rng(3);
  HandSimulator sim(policies, nullptr, rng);
  bool threw = false;
  try {
    sim.run(def, holes, board, 0);
  } catch (const stage6_sim_error&) {
    threw = true;
  }
  check(threw, "an illegal policy action is a typed simulator error, not a clamp");
}

// RFC 0029 AC2: a PREFLOP-ROOTED flop-terminal definition must throw the named
// simulator error instead of spinning, because the simulator has no frontier
// evaluator. Before the case existed the loop had no arm for the phase, so this
// hand spun forever; the definition below is the one test_game_definition.cpp
// already proves reaches `Phase::Frontier`.
void test_simulator_flop_terminal_throws() {
  CheckCallPolicy policy;
  std::vector<const BehaviorPolicy*> policies{&policy, &policy};

  // Heads-up preflop root with flop-terminal depth, matching the construction
  // in test_game_definition.cpp's case_flop_terminal_frontier().
  GameDef def = preflop_def(2, 2, 50);
  def.button = 1;
  def.blinds_posted = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
  def.blinds_posted[def.button] = 1;      // SB on the button
  def.blinds_posted[1 - def.button] = 2;  // BB
  def.pot = 3;
  def.terminal = bs::poker::TerminalDepth::Flop;

  std::vector<HoleCards> holes{{{card("As"), card("Ks")}}, {{card("2d"), card("7d")}}};
  std::array<int, 5> board{card("2c"), card("3d"), card("7h"), card("4s"), card("5s")};
  bs::SplitMix64 rng(5);
  HandSimulator sim(policies, nullptr, rng);

  bool threw = false;
  std::string message;
  try {
    (void)sim.run(def, holes, board, 0);
  } catch (const stage6_sim_error& e) {
    threw = true;
    message = e.what();
  } catch (const std::exception&) {
    // Any other exception type is a failure of the named-error contract.
    threw = false;
  }
  check(threw, "a preflop-rooted flop-terminal hand throws stage6_sim_error");
  check(message.find("frontier") != std::string::npos,
        "the simulator's frontier error names the missing evaluator");
}

void test_coverage_hash_and_membership() {
  GeometrySignature a;
  a.player_count = 3;
  a.pot = 18;
  a.stacks = {194, 194, 194, 0, 0, 0, 0, 0, 0, 0};
  a.contributed = {6, 6, 6, 0, 0, 0, 0, 0, 0, 0};
  a.live = {0, 1, 2};
  GeometryCoverage cov({a, a});  // duplicates collapse
  check(cov.size() == 1, "coverage deduplicates signatures");
  check(cov.covers(a), "coverage contains its member");
  check(cov.require_covered(a) == a, "require_covered returns the covered signature");
  GeometrySignature other = a;
  other.pot = 20;
  check(!cov.covers(other), "coverage rejects a non-member");
  bool threw = false;
  try {
    cov.require_covered(other);
  } catch (const stage6_geometry_uncovered&) {
    threw = true;
  }
  check(threw, "require_covered throws the typed refusal on a miss");
  const std::uint64_t h1 = cov.content_hash();
  GeometryCoverage cov2({a});
  check(h1 == cov2.content_hash(), "content hash is order/duplicate independent");
}

// Small joint ranges: some combos across seats share a card (so the joint
// table enumerates conflicts and rejects them), while a mutually compatible
// subset exists and lets all three reach the river under the passive policy.
std::vector<std::vector<bs::solver::MultiwayWeightedHand>> three_seat_ranges() {
  using bs::solver::MultiwayWeightedHand;
  auto hand = [](const char* a, const char* b) {
    return MultiwayWeightedHand{{card(a), card(b)}, 1.0};
  };
  return {
      {hand("As", "Ks"), hand("Ah", "Kh")},
      {hand("Ad", "Kd"), hand("As", "Qh")},  // AsQh overlaps seat0's AsKs
      {hand("Ac", "Kc"), hand("Qc", "Jc")},
  };
}

void run_oracle_fixture(const char* label, std::array<Chips, 3> stacks) {
  GameDef def = three_seat_rooted(stacks);
  CheckCallPolicy passive;
  std::vector<const BehaviorPolicy*> policies{&passive, &passive, &passive};
  auto ranges = three_seat_ranges();
  const bs::solver::JointDealTable table =
      bs::solver::enumerate_joint_deals(ranges, {card("2c"), card("3d"), card("7h")});
  check(table.deals.size() >= 1, "oracle joint table has compatible deals");
  check(table.rejected_conflicts > 0, "oracle ranges force card-overlap rejection");
  OracleCounts counts;
  const std::array<double, 10> u =
      exact_oracle_utility(def, ranges, policies, bs::gto::NoFrontier{}, &counts);
  check(counts.joint_deals == table.deals.size(), "oracle walks every joint deal");
  check(counts.showdown_leaves > 0, "passive profiles reach showdown leaves");
  // Exact walk counts, independently re-derived per deal: 6 compatible deals;
  // per deal chance = 1 turn + 43 river = 44, showdowns = 43*42 = 1,806,
  // actions = 3 + 43*3 + 43*42*3 = 5,550.
  check(counts.joint_deals == 6, "exactly six compatible joint deals");
  check(counts.chance_nodes == 264, "pinned oracle chance-node count");
  check(counts.showdown_leaves == 10836, "pinned oracle showdown-leaf count");
  check(counts.action_nodes == 33300, "pinned oracle action-node count");
  check(std::abs(util_sum(u, 3)) < 1e-7, "exact 3p expected utility is zero-sum");
  std::printf(
      "oracle %s: deals=%llu action=%llu chance=%llu fold=%llu showdown=%llu "
      "u=[%.6f %.6f %.6f]\n",
      label, (unsigned long long)counts.joint_deals, (unsigned long long)counts.action_nodes,
      (unsigned long long)counts.chance_nodes, (unsigned long long)counts.fold_leaves,
      (unsigned long long)counts.showdown_leaves, u[0], u[1], u[2]);
}

void test_oracle_aggressive_sidepots() {
  // Unequal stacks with an aggressive policy exercise the short-stack all-in
  // and side-pot layers; the aggressive walk must differ from the check-down
  // walk and stay zero-sum.
  GameDef def = three_seat_rooted({1, 2, 2});
  auto ranges = three_seat_ranges();

  CheckCallPolicy passive;
  std::vector<const BehaviorPolicy*> pp{&passive, &passive, &passive};
  OracleCounts pc;
  (void)exact_oracle_utility(def, ranges, pp, bs::gto::NoFrontier{}, &pc);

  JamPolicy jam;
  std::vector<const BehaviorPolicy*> jp{&jam, &jam, &jam};
  OracleCounts jc;
  const std::array<double, 10> ju =
      exact_oracle_utility(def, ranges, jp, bs::gto::NoFrontier{}, &jc);
  check(std::abs(util_sum(ju, 3)) < 1e-7, "aggressive unequal-stack oracle stays zero-sum");
  check(jc.action_nodes != pc.action_nodes,
        "the aggressive policy explores a different tree than a check-down");
  check(jc.showdown_leaves + jc.fold_leaves > 0, "oracle reaches terminals under jams");

  // One fixed compatible deal, evaluated directly, is also exactly zero-sum.
  const std::vector<HoleCards> one{
      {{card("Ah"), card("Kh")}}, {{card("Ad"), card("Kd")}}, {{card("Ac"), card("Qc")}}};
  OracleCounts sc;
  const std::array<double, 10> su = exact_deal_utility(def, one, jp, bs::gto::NoFrontier{}, &sc);
  check(std::abs(util_sum(su, 3)) < 1e-9, "a single fixed deal settles zero-sum");
  check(sc.chance_nodes > 0, "the single deal averages over runout chance");
}

// A policy that returns a NaN probability, to prove the exact oracle (the R9
// gate the step-5 estimator is validated against) fails closed rather than
// propagating NaN expected utility.
class NanPolicy final : public BehaviorPolicy {
 public:
  std::vector<PolicyAction> distribution(const GameState& state, std::size_t, HoleCards,
                                         const PolicyContext&) const override {
    poker::Action a = state.legal().check ? poker::Action{poker::ActionType::Check, 0}
                                          : poker::Action{poker::ActionType::Fold, 0};
    return {{a, std::nan("")}};
  }
};
// Builds the uniform all-1326-combo range (weight 1 each) the PRIMARY dealing
// distribution uses.
std::vector<bs::solver::MultiwayWeightedHand> all_combos() {
  using bs::solver::MultiwayWeightedHand;
  std::vector<MultiwayWeightedHand> combos;
  combos.reserve(1326);
  for (int a = 0; a < 52; ++a)
    for (int b = a + 1; b < 52; ++b)
      combos.push_back({{a, b}, 1.0});
  return combos;
}

// The exact oracle must fail closed on a NaN probability instead of returning
// NaN expected utility (the R9 gate the step-5 estimator is validated against).
void test_oracle_rejects_nan() {
  GameDef def = three_seat_rooted({1, 1, 1});
  NanPolicy nan;
  CheckCallPolicy good;
  std::vector<const BehaviorPolicy*> policies{&nan, &good, &good};
  const std::vector<HoleCards> one{
      {{card("Ah"), card("Kh")}}, {{card("Ad"), card("Kd")}}, {{card("Ac"), card("Qc")}}};
  bool threw = false;
  try {
    (void)exact_deal_utility(def, one, policies, bs::gto::NoFrontier{}, nullptr);
  } catch (const std::runtime_error&) {
    threw = true;
  }
  check(threw, "a NaN oracle probability throws rather than poisoning the exact value");
}

// The exact best response is the R9 reference the Monte Carlo estimator must
// match. The true best response pools opponent holdings at each information
// set (max_a E_z[Q]); exact_best_response_utility does that. The per-deal
// "omniscient" deviation E_z[max_a Q] is only an upper bound. On the tiny
// {1,1,1} fixture the test policies are menu-contained, so each traverser's
// best-WITHIN-menu value is >= its profile value.
void test_exact_best_response_gain() {
  GameDef def = three_seat_rooted({1, 1, 1});
  auto ranges = three_seat_ranges();
  CheckCallPolicy passive;
  JamPolicy jam;

  // The passive profile (check/call) and the jam profile (cap, which the
  // deviation menu always contains) are menu-contained, so the unilateral
  // best-WITHIN-menu gain is non-negative for every seat.
  std::vector<const BehaviorPolicy*> pp{&passive, &passive, &passive};
  OracleCounts pc;
  const std::array<double, 10> profile =
      exact_oracle_utility(def, ranges, pp, bs::gto::NoFrontier{}, &pc);
  for (std::size_t seat = 0; seat < 3; ++seat) {
    OracleCounts bc;
    const std::array<double, 10> br =
        exact_best_response_utility(def, ranges, pp, seat, bs::gto::NoFrontier{}, &bc);
    check(br[seat] + 1e-9 >= profile[seat],
          "menu-contained profile: pooled BR utility >= profile utility");

    // The omniscient per-deal deviation is an UPPER BOUND on the true BR:
    // E_z[max_a Q] >= max_a E_z[Q] (the max-expectation inequality).
    OracleCounts oc;
    const std::array<double, 10> omni =
        exact_omniscient_deviation_utility(def, ranges, pp, seat, bs::gto::NoFrontier{}, &oc);
    check(omni[seat] + 1e-9 >= br[seat],
          "omniscient per-deal deviation upper-bounds the pooled best response");
  }

  // Against two jam policies, a traverser's pooled best response is a
  // well-defined finite value and the whole vector stays zero-sum (the other
  // seats keep their fixed policy; only the traverser deviates).
  std::vector<const BehaviorPolicy*> jp{&jam, &jam, &jam};
  OracleCounts bc0;
  const std::array<double, 10> br0 =
      exact_best_response_utility(def, ranges, jp, 0, bs::gto::NoFrontier{}, &bc0);
  check(std::isfinite(br0[0]) && std::isfinite(br0[1]) && std::isfinite(br0[2]),
        "best response utility is finite");
  check(std::abs(util_sum(br0, 3)) < 1e-7, "best-response outcome vector is zero-sum");
}

// The pooled best response never exceeds the omniscient bound, and on a game
// where the information-set argmax disagrees across deals the two are STRICTLY
// ordered — proving the estimator reference pools opponent uncertainty rather
// than peeking. Even if this particular fixture does not force a strict gap on
// every build, the weak inequality always holds; a strict gap is asserted only
// when the measured data separates.
void test_pooled_br_does_not_peek() {
  GameDef def = three_seat_rooted({10, 10, 10});
  using bs::solver::MultiwayWeightedHand;
  auto h = [&](const char* a, const char* b) {
    return MultiwayWeightedHand{{card(a), card(b)}, 1.0};
  };
  // Traverser (seat1) holds a single fixed hand; opponents each range over
  // several holdings so one shared information set is reached with many
  // opponent deal combinations.
  std::vector<std::vector<MultiwayWeightedHand>> ranges{
      {h("Qs", "Js"), h("Qh", "Jh"), h("Qd", "Jd")},
      {h("As", "Ks")},
      {h("Ts", "9s"), h("Th", "9h"), h("Td", "9d")},
  };
  CheckCallPolicy passive;
  std::vector<const BehaviorPolicy*> pp{&passive, &passive, &passive};
  double max_gap = 0.0;
  for (std::size_t seat = 0; seat < 3; ++seat) {
    OracleCounts bc, oc;
    const std::array<double, 10> br =
        exact_best_response_utility(def, ranges, pp, seat, bs::gto::NoFrontier{}, &bc);
    const std::array<double, 10> omni =
        exact_omniscient_deviation_utility(def, ranges, pp, seat, bs::gto::NoFrontier{}, &oc);
    check(omni[seat] + 1e-9 >= br[seat],
          "multi-holding game: omniscient bound >= pooled best response for every seat");
    check(std::abs(util_sum(br, 3)) < 1e-7, "pooled best response stays zero-sum");
    max_gap = std::max(max_gap, omni[seat] - br[seat]);
  }
  std::printf("[pooling] stacks10 multi-holding max omniscient-minus-pooled gap = %.6f\n", max_gap);
  check(max_gap > 1e-6,
        "on this multi-holding fixture the information-set argmax splits, so the "
        "omniscient bound is STRICTLY above the pooled best response (no card peeking)");
}

// Qualification proposal required information-restriction negative control.
// The responder (responder_seat) holds one FIXED hand on a fully dealt river
// board and faces an all-in fold/call decision; the opponent is equally likely
// to hold a hand that wins or a hand that loses, indistinguishable from the
// responder's seat.  Chip accounting is the proposal's fixture scaled by 2 so
// the river lead (2) is a legal full raise at big_blind 2:
//
//                        fold      call
//   strong opponent      -2        -4   (opponent's trip nines win)
//   weak opponent        -2        +4   (responder's pair of threes wins)
//
// A legal information-set response is ONE action for both worlds.  Folding is
// worth -2; calling pools to (-4 + 4)/2 = 0, so the pooled best response calls
// with value 0.  A hidden-information-dependent response would fold in the
// strong world and call in the weak one, averaging (-2 + 4)/2 = 1 — illegal at
// this information set, exactly what exact_best_response_utility must refuse.
// Against the always-fold baseline (-2) the legal gain is 2; the omniscient
// (world-wise) calculation reports 3.  Divide by 2 to recover the proposal's
// exact figures (-1/-2 vs -1/+2; BR 0; world-wise 0.5; gains 1 vs 1.5).
void run_information_restriction_fixture(std::size_t responder_seat) {
  using bs::solver::MultiwayWeightedHand;
  GameDef def{};
  def.player_count = 2;
  def.button = 0;
  def.big_blind = 2;
  // Rainbow unpaired board: no flush, and no hand below a pair improves.
  def.board = {card("2c"), card("5h"), card("9d"), card("Kc"), card("4s")};
  def.board_size = 5;
  const std::size_t opp = 1 - responder_seat;
  // Before the river both seats had matched 2; the opponent led for 2 more on
  // the river (raise rights exhausted, so the responder's menu is fold/call),
  // and the responder faces calling 2.
  def.contributions[responder_seat] = 2;
  def.contributions[opp] = 4;
  def.stacks[responder_seat] = 100;
  def.stacks[opp] = 100;
  def.pot = 6;
  def.street_committed[responder_seat] = 0;
  def.street_committed[opp] = 2;
  def.last_full_raise = 2;
  def.raise_rights[responder_seat] = false;
  def.raise_rights[opp] = false;

  // Sanity: the responder is the root actor with exactly fold/call.
  GameState root(def);
  check(*root.actor() == responder_seat, "negative control: responder is root actor");
  const LegalActions legal = root.legal();
  check(!legal.aggressive.has_value(), "negative control: all-in lead offers no raise");
  check(legal.call, "negative control: calling is legal");

  auto h = [&](const char* a, const char* b) {
    return MultiwayWeightedHand{{card(a), card(b)}, 1.0};
  };
  std::vector<std::vector<MultiwayWeightedHand>> ranges(2);
  // The responder's information is IDENTICAL in both worlds: one fixed hand.
  ranges[responder_seat] = {h("3h", "3d")};  // pair of threes
  // Two equally likely, indistinguishable opponent worlds.
  ranges[opp] = {
      h("9h", "9s"),  // strong: trip nines (board 9d) beats pair of threes
      h("Ah", "Qh"),  // weak: ace high loses to pair of threes
  };

  FoldPolicy fold;
  CheckCallPolicy check_call;
  std::vector<const BehaviorPolicy*> profile(2);
  profile[responder_seat] = &fold;
  profile[opp] = &check_call;

  // Always-fold baseline: -2 in every world.
  OracleCounts pc;
  const auto baseline = exact_oracle_utility(def, ranges, profile, bs::gto::NoFrontier{}, &pc);
  check(std::abs(baseline[responder_seat] - (-2.0)) < 1e-9,
        "negative control: always-fold baseline is -2");

  // The legal pooled best response must call: pooled value 0, strictly above
  // folding's -2.  A pooled oracle that peeked would report 1 instead.
  OracleCounts bc;
  const auto br =
      exact_best_response_utility(def, ranges, profile, responder_seat, bs::gto::NoFrontier{}, &bc);
  check(std::abs(br[responder_seat] - 0.0) < 1e-9,
        "negative control: pooled BR calls, value 0 (worlds pooled, not peeked)");

  // The omniscient per-deal deviation is the named illegal answer, 1.
  OracleCounts oc;
  const auto omni = exact_omniscient_deviation_utility(def, ranges, profile, responder_seat,
                                                       bs::gto::NoFrontier{}, &oc);
  check(std::abs(omni[responder_seat] - 1.0) < 1e-9,
        "negative control: world-wise (omniscient) value is 1");
  check(omni[responder_seat] - br[responder_seat] > 0.5 - 1e-9,
        "negative control: omniscient 1 exceeds pooled 0 by the pinned 0.5 units");

  // Gains against the -2 baseline, in 2x chip units: legal 2 vs erroneous 3
  // (= proposal units 1 vs 1.5).
  const double legal_gain = br[responder_seat] - baseline[responder_seat];
  const double worldwise_gain = omni[responder_seat] - baseline[responder_seat];
  check(std::abs(legal_gain - 2.0) < 1e-9, "negative control: legal gain is 2 chips (1 unit)");
  check(std::abs(worldwise_gain - 3.0) < 1e-9,
        "negative control: world-wise gain is 3 chips (1.5 units)");
  check(std::abs(util_sum(br, 2)) < 1e-7, "negative control: BR vector stays zero-sum");

  std::printf(
      "[negative-control] responder=seat%zu baseline=%.1f pooledBR=%.1f "
      "omniscient=%.1f legalGain=%.1f worldwiseGain=%.1f (units: /2)\n",
      responder_seat, baseline[responder_seat], br[responder_seat], omni[responder_seat],
      legal_gain, worldwise_gain);
}

void test_information_restriction_negative_control() {
  // Both seat orientations must produce the identical pinned answer (the
  // restriction is on the responder's information, not its seat label).
  run_information_restriction_fixture(0);
  run_information_restriction_fixture(1);
}

}  // namespace

int main() {
  test_simulator_three_seat_showdown();
  test_simulator_determinism();
  test_simulator_coverage_total();
  test_simulator_two_seat_fold();
  test_simulator_rejects_illegal_policy();
  test_simulator_flop_terminal_throws();
  test_coverage_hash_and_membership();
  run_oracle_fixture("{1,1,1}", {1, 1, 1});
  run_oracle_fixture("{1,2,2}", {1, 2, 2});
  test_oracle_aggressive_sidepots();
  test_oracle_rejects_nan();
  test_exact_best_response_gain();
  test_pooled_br_does_not_peek();
  test_information_restriction_negative_control();
  if (failures) {
    std::fprintf(stderr, "STAGE6 SIMULATOR/ORACLE TESTS FAILED: %d\n", failures);
    return 1;
  }
  std::puts("STAGE6 SIMULATOR/ORACLE TESTS PASSED");
  return 0;
}
