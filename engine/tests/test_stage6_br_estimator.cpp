// RFC 0008 stage 6 R12 step 5: two-phase (learn/confirm) Monte Carlo
// deviation-gain estimator gates. Pins:
//   * the exact-freeze table reproduces exact_best_response_utility through
//     exact_oracle_utility (single source of pooling truth) and stays STRICTLY
//     below the omniscient bound on the stacks-10 splitting fixture;
//   * MC gains match the exact pooled reference on the enumerable R9 fixtures
//     ({1,1,1} passive, {1,2,2} jam, stochastic menu-contained opponents),
//     with the residual 95% CI including zero;
//   * the MC estimator never reaches the omniscient value;
//   * infoset pooling, disjoint seed lists, the miss policy, frozen-action
//     legality, bootstrap/stabilization behavior, CRN shared-spine pairing and
//     bitwise repeatability;
//   * the sum of the two seats' gains agrees with HeadsUpTrain::evaluate's
//     exact ExactEvaluation::nash_conv on an aligned-menu heads-up game;
//   * scalar NashConv statistics and the off-menu profile label.
#include <algorithm>
#include <array>
#include <bs/abstract_tree.hpp>
#include <bs/abstraction.hpp>
#include <bs/behavior_policy.hpp>
#include <bs/eval.hpp>
#include <bs/game_definition.hpp>
#include <bs/heads_up.hpp>
#include <bs/heads_up_solver.hpp>
#include <bs/multiway_sampler.hpp>
#include <bs/prng.hpp>
#include <bs/stage6/br_estimator.hpp>
#include <bs/stage6/crn_streams.hpp>
#include <bs/stage6/exact_oracle.hpp>
#include <bs/stage6/infoset_key.hpp>
#include <bs/stage6/statistics.hpp>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <utility>
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

using bs::solver::MultiwayWeightedHand;

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

// Turn/river-rooted variants for the MC gates: with the public board fixed the
// learn phase SATURATES every reachable information set (no 43x42 runout key
// space), so small seed lists give tight, unbiased estimates against the exact
// oracle. The fixed cards 4d/5h are absent from three_seat_ranges.
GameDef three_seat_turn_rooted(std::array<Chips, 3> stacks) {
  GameDef d = three_seat_rooted(stacks);
  d.board = {card("2c"), card("3d"), card("7h"), card("4d"), 0};
  d.board_size = 4;
  return d;
}

GameDef three_seat_river_rooted(std::array<Chips, 3> stacks) {
  GameDef d = three_seat_rooted(stacks);
  d.board = {card("2c"), card("3d"), card("7h"), card("4d"), card("5h")};
  d.board_size = 5;
  return d;
}

std::vector<std::vector<MultiwayWeightedHand>> three_seat_ranges() {
  auto hand = [](const char* a, const char* b) {
    return MultiwayWeightedHand{{card(a), card(b)}, 1.0};
  };
  return {
      {hand("As", "Ks"), hand("Ah", "Kh")},
      {hand("Ad", "Kd"), hand("As", "Qh")},  // AsQh overlaps seat0's AsKs
      {hand("Ac", "Kc"), hand("Qc", "Jc")},
  };
}

std::vector<std::vector<MultiwayWeightedHand>> stacks10_ranges() {
  auto hand = [](const char* a, const char* b) {
    return MultiwayWeightedHand{{card(a), card(b)}, 1.0};
  };
  return {
      {hand("Qs", "Js"), hand("Qh", "Jh"), hand("Qd", "Jd")},
      {hand("As", "Ks")},
      {hand("Ts", "9s"), hand("Th", "9h"), hand("Td", "9d")},
  };
}

std::vector<std::uint64_t> seed_list(std::uint64_t base, std::size_t count) {
  std::vector<std::uint64_t> seeds;
  for (std::size_t i = 0; i < count; ++i)
    seeds.push_back(base + static_cast<std::uint64_t>(i) * 7919 + 17);
  return seeds;
}

// Pinned confirm seeds always include 1, 17 and 43.
std::vector<std::uint64_t> confirm_seeds(std::size_t count) {
  std::vector<std::uint64_t> seeds{1, 17, 43};
  for (std::size_t i = 3; i < count; ++i)
    seeds.push_back(5000 + static_cast<std::uint64_t>(i) * 6151 + 29);
  return seeds;
}

// --- Reference policies (also used by the simulator/oracle tests) ----------

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
    return {{a, 1.0}};
  }
};

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
    return {{a, 1.0}};
  }
};

// Menu-contained stochastic opponent: 0.6 check / 0.4 the minimum declared
// bet when it is check-to, otherwise a deterministic call/check. Both actions
// are declared-menu actions at every node this policy is used.
class StochasticPolicy final : public BehaviorPolicy {
 public:
  std::vector<PolicyAction> distribution(const GameState& state, std::size_t, HoleCards,
                                         const PolicyContext&) const override {
    const LegalActions legal = state.legal();
    if (legal.check && legal.aggressive)
      return {{{poker::ActionType::Check, 0}, 0.6},
              {{legal.aggressive->type, legal.aggressive->minimum}, 0.4}};
    poker::Action a;
    if (legal.check)
      a = {poker::ActionType::Check, 0};
    else if (legal.call)
      a = {poker::ActionType::Call, 0};
    else
      a = {poker::ActionType::Fold, 0};
    return {{a, 1.0}};
  }
};

// A deterministic LEGAL profile that deliberately emits an off-menu total
// (bet 5 into pot 3 on stacks-6 geometry where the declared menu is
// {check, 2, 3, 4, 6}). The declared-menu BR is allowed to lose to it, so the
// measured gain may be negative; that is an honest property, never a failure.
class OffMenuPolicy final : public BehaviorPolicy {
 public:
  std::vector<PolicyAction> distribution(const GameState& state, std::size_t, HoleCards,
                                         const PolicyContext&) const override {
    const LegalActions legal = state.legal();
    if (legal.check && legal.aggressive) {
      const Chips wanted = 5;
      if (wanted >= legal.aggressive->minimum && wanted <= legal.aggressive->maximum)
        return {{{poker::ActionType::Bet, wanted}, 1.0}};
      return {{{poker::ActionType::Check, 0}, 1.0}};
    }
    poker::Action a;
    if (legal.check)
      a = {poker::ActionType::Check, 0};
    else if (legal.call)
      a = {poker::ActionType::Call, 0};
    else
      a = {poker::ActionType::Fold, 0};
    return {{a, 1.0}};
  }
};

// ---------------------------------------------------------------------------
// Exact freeze vs the pooled oracle
// ---------------------------------------------------------------------------

void test_exact_freeze_equals_oracle() {
  GameDef def = three_seat_rooted({1, 1, 1});
  auto ranges = three_seat_ranges();
  CheckCallPolicy passive;
  std::vector<const BehaviorPolicy*> pp{&passive, &passive, &passive};

  const std::array<double, 10> profile = exact_oracle_utility(def, ranges, pp, nullptr);
  for (std::size_t traverser = 0; traverser < 3; ++traverser) {
    FrozenBestResponseData frozen = learn_exact_frozen_best_response(def, ranges, pp, traverser);
    check(frozen.stabilized && frozen.epochs_run == 1,
          "the exact freeze is reported as a one-pass stabilized table");
    check(frozen.actions.size() == frozen.menus.size(),
          "every frozen key carries its declared menu");
    check(frozen.menu_identity_hash == declared_menu_identity_hash(),
          "the frozen table names the published menu identity");
    FrozenBestResponsePolicy policy(std::move(frozen));
    std::vector<const BehaviorPolicy*> joint{pp[0], pp[1], pp[2]};
    joint[traverser] = &policy;
    const std::array<double, 10> via_frozen = exact_oracle_utility(def, ranges, joint, nullptr);
    const std::array<double, 10> br =
        exact_best_response_utility(def, ranges, pp, traverser, nullptr);
    for (std::size_t s = 0; s < 3; ++s)
      check(std::abs(via_frozen[s] - br[s]) < 1e-9,
            "the frozen table evaluated exactly equals the pooled best response");
    check(br[traverser] + 1e-9 >= profile[traverser],
          "menu-contained profile: exact BR gain is non-negative");
  }
}

void test_exact_freeze_strict_pooling() {
  GameDef def = three_seat_rooted({10, 10, 10});
  auto ranges = stacks10_ranges();
  CheckCallPolicy passive;
  std::vector<const BehaviorPolicy*> pp{&passive, &passive, &passive};
  double max_gap = 0.0;
  for (std::size_t traverser = 0; traverser < 3; ++traverser) {
    FrozenBestResponseData frozen = learn_exact_frozen_best_response(def, ranges, pp, traverser);
    FrozenBestResponsePolicy policy(std::move(frozen));
    std::vector<const BehaviorPolicy*> joint{pp[0], pp[1], pp[2]};
    joint[traverser] = &policy;
    const std::array<double, 10> pooled = exact_oracle_utility(def, ranges, joint, nullptr);
    const std::array<double, 10> omni =
        exact_omniscient_deviation_utility(def, ranges, pp, traverser, nullptr);
    check(omni[traverser] + 1e-9 >= pooled[traverser],
          "exact freeze value never exceeds the omniscient upper bound");
    max_gap = std::max(max_gap, omni[traverser] - pooled[traverser]);
  }
  std::printf("[br-exact] stacks10 omniscient-minus-frozen gap = %.6f\n", max_gap);
  check(std::abs(max_gap - 0.371601) < 1e-6,
        "the exact freeze reproduces the pinned strict pooling gap 0.371601");
}

// ---------------------------------------------------------------------------
// MC vs exact on the enumerable fixtures
// ---------------------------------------------------------------------------

struct McGate {
  double exact_profile = 0.0;
  double exact_br = 0.0;
  double mc_mean = 0.0;
  ConfidenceInterval residual_ci;
};

McGate run_mc_gate(const GameDef& def, const std::vector<std::vector<MultiwayWeightedHand>>& ranges,
                   const std::vector<const BehaviorPolicy*>& policies, std::size_t traverser,
                   std::size_t learn_count, std::size_t confirm_count, double tolerance) {
  std::vector<int> board(def.board.begin(), def.board.begin() + def.board_size);
  const bs::solver::JointDealTable table = bs::solver::enumerate_joint_deals(ranges, board);
  const std::vector<std::uint64_t> learn = seed_list(1000 + 101 * traverser, learn_count);
  const std::vector<std::uint64_t> confirm = confirm_seeds(confirm_count);
  BrEstimatorConfig config;
  config.require_stabilization = true;
  BrEstimatorResult result =
      estimate_deviation_gains(def, ranges, policies, learn, confirm, &table, config);

  const std::array<double, 10> profile = exact_oracle_utility(def, ranges, policies, nullptr);
  const std::array<double, 10> br =
      exact_best_response_utility(def, ranges, policies, traverser, nullptr);
  const double exact_gain = br[traverser] - profile[traverser];
  std::vector<double> residual;
  residual.reserve(confirm.size());
  for (double g : result.gain[traverser])
    residual.push_back(g - exact_gain);
  const ConfidenceInterval ci = confidence_interval_95(residual);
  check(std::abs(result.mean_gain[traverser] - exact_gain) <= tolerance,
        "MC mean gain matches the exact pooled gain within tolerance");
  check(ci.lower <= 0.0 && ci.upper >= 0.0,
        "the residual 95% CI includes zero (no measured MC bias)");
  for (double g : result.gain[traverser])
    check(std::isfinite(g), "every per-seed gain is finite");
  return {profile[traverser], br[traverser], result.mean_gain[traverser], ci};
}

void test_mc_gain_passive() {
  // River-rooted: no chance branching, so the MC learn saturates every
  // reachable information set and the frozen table is the exact pooled BR.
  GameDef def = three_seat_river_rooted({1, 1, 1});
  CheckCallPolicy passive;
  std::vector<const BehaviorPolicy*> pp{&passive, &passive, &passive};
  for (std::size_t t = 0; t < 3; ++t) {
    const McGate gate = run_mc_gate(def, three_seat_ranges(), pp, t, 200, 2000, 0.08);
    check(gate.exact_br + 1e-9 >= gate.exact_profile,
          "menu-contained passive profile: exact gain non-negative");
    std::printf("[br-mc passive] seat=%zu exact=%.5f mc=%.5f ci=[%+.4f,%+.4f]\n", t,
                gate.exact_br - gate.exact_profile, gate.mc_mean, gate.residual_ci.lower,
                gate.residual_ci.upper);
  }
}

void test_mc_gain_sidepots() {
  GameDef def = three_seat_river_rooted({1, 2, 2});
  JamPolicy jam;
  std::vector<const BehaviorPolicy*> jp{&jam, &jam, &jam};
  for (std::size_t t = 0; t < 3; ++t) {
    const McGate gate = run_mc_gate(def, three_seat_ranges(), jp, t, 200, 2000, 0.10);
    std::printf("[br-mc jam] seat=%zu exact=%.5f mc=%.5f ci=[%+.4f,%+.4f]\n", t,
                gate.exact_br - gate.exact_profile, gate.mc_mean, gate.residual_ci.lower,
                gate.residual_ci.upper);
  }
}

void test_mc_gain_stochastic() {
  // Stacks of 2: check-to nodes offer bet 2 = cap, so the 0.6/0.4 mix is
  // fully menu-contained. Opponent randomness is external-sampled through the
  // OpponentAction CRN stream, exercising stochastic-policy branching without
  // any chance-card branching.
  GameDef def = three_seat_river_rooted({2, 2, 2});
  StochasticPolicy mix;
  CheckCallPolicy passive;
  for (std::size_t t = 0; t < 3; ++t) {
    std::vector<const BehaviorPolicy*> policies{&mix, &mix, &mix};
    policies[t] = &passive;
    const McGate gate = run_mc_gate(def, three_seat_ranges(), policies, t, 300, 3000, 0.12);
    check(gate.exact_br + 1e-9 >= gate.exact_profile,
          "stochastic menu-contained opponents: traverser gain non-negative");
    std::printf("[br-mc stoch] seat=%zu exact=%.5f mc=%.5f ci=[%+.4f,%+.4f]\n", t,
                gate.exact_br - gate.exact_profile, gate.mc_mean, gate.residual_ci.lower,
                gate.residual_ci.upper);
  }
}

void test_mc_gain_with_runout_chance() {
  // Turn-rooted: one river card is sampled through the RunoutCard CRN stream.
  // Learn saturation over the 42-card river fan is not attainable cheaply, so
  // the frozen table comes from the EXACT oracle (every information set
  // covered) and phase 2 alone is Monte Carlo. Its paired gain series must
  // reproduce the exact pooled gain with a residual CI including zero —
  // validating the runout CRN leg end to end.
  GameDef def = three_seat_turn_rooted({1, 1, 1});
  auto ranges = three_seat_ranges();
  std::vector<int> board(def.board.begin(), def.board.begin() + def.board_size);
  const bs::solver::JointDealTable table = bs::solver::enumerate_joint_deals(ranges, board);
  CheckCallPolicy passive;
  std::vector<const BehaviorPolicy*> pp{&passive, &passive, &passive};
  const std::vector<std::uint64_t> confirm = confirm_seeds(4000);
  BrEstimatorConfig config;
  for (std::size_t t = 0; t < 3; ++t) {
    FrozenBestResponseData frozen = learn_exact_frozen_best_response(def, ranges, pp, t);
    const std::array<double, 10> profile = exact_oracle_utility(def, ranges, pp, nullptr);
    const std::array<double, 10> br = exact_best_response_utility(def, ranges, pp, t, nullptr);
    const double exact_gain = br[t] - profile[t];
    std::vector<double> gains;
    gains.reserve(confirm.size());
    for (std::uint64_t seed : confirm) {
      ConfirmPairOutput pair =
          mc_confirm_pair(def, ranges, pp, t, seed, frozen, &table, config, nullptr);
      gains.push_back(pair.best_utility[t] - pair.profile_utility[t]);
    }
    std::vector<double> residual;
    for (double g : gains)
      residual.push_back(g - exact_gain);
    const ConfidenceInterval ci = confidence_interval_95(residual);
    check(ci.lower <= 0.0 && ci.upper >= 0.0, "turn-rooted runout MC residual CI includes zero");
    std::printf("[br-mc chance] seat=%zu exact=%.5f mc=%.5f ci=[%+.4f,%+.4f]\n", t, exact_gain,
                confidence_interval_95(gains).mean, ci.lower, ci.upper);
  }
}

// MC LEARN with a chance branch above a descendant traverser decision. The
// chance-free gates cannot regress the learn-side interaction of full-menu
// fan-out + sampled runout + a later own decision; the exact-freeze gates
// bypass learn entirely. This gate runs the full Monte Carlo LEARN on a
// turn-rooted game (one 42-card river draw) over several INDEPENDENT learn
// lists, so a persistent (not seed-noise) learn bias would shift every
// residual mean the same way and fail.
void test_mc_learn_with_chance_branching() {
  GameDef def = three_seat_turn_rooted({4, 4, 4});
  auto ranges = three_seat_ranges();
  std::vector<int> board(def.board.begin(), def.board.begin() + def.board_size);
  const bs::solver::JointDealTable table = bs::solver::enumerate_joint_deals(ranges, board);
  CheckCallPolicy passive;
  std::vector<const BehaviorPolicy*> pp{&passive, &passive, &passive};
  const std::array<double, 10> profile = exact_oracle_utility(def, ranges, pp, nullptr);
  const std::vector<std::uint64_t> confirm = confirm_seeds(1500);

  for (std::uint64_t base : {std::uint64_t{20000}, std::uint64_t{40000}, std::uint64_t{60000}}) {
    const std::vector<std::uint64_t> learn = seed_list(base, 6000);
    BrEstimatorConfig config;
    config.on_confirm_miss = FrozenMissPolicy::RecordMiss;
    BrEstimatorResult result =
        estimate_deviation_gains(def, ranges, pp, learn, confirm, &table, config);
    for (std::size_t t = 0; t < 3; ++t) {
      check(result.counts.confirm_misses_per_traverser[t] == 0,
            "6000 turn-rooted learn seeds saturate the 42-card river information sets");
      const std::array<double, 10> br = exact_best_response_utility(def, ranges, pp, t, nullptr);
      const double exact_gain = br[t] - profile[t];
      std::vector<double> residual;
      for (double g : result.gain[t])
        residual.push_back(g - exact_gain);
      const ConfidenceInterval ci = confidence_interval_95(residual);
      check(ci.lower <= 0.0 && ci.upper >= 0.0,
            "MC learn through a sampled chance node: residual CI includes the exact pooled gain "
            "on every independent learn list");
      check(result.frozen[t].stabilized, "the turn-rooted learn table stabilizes");
    }
  }
  std::printf("[br-mc learn+chance] three independent learn lists all match exact pooled BR\n");
}

void test_mc_never_reaches_omniscient() {
  // Freeze the EXACT pooled table, then confirm it with Monte Carlo: the
  // paired rollouts are single-spine (no learn bush cost), and their gain
  // series must reproduce the exact pooled gain with a residual CI that
  // includes zero. The STRICT separation below the omniscient bound is
  // demanded only at the seat whose pooled gap is the pinned 0.371601: a
  // point estimate at a sub-0.1-chip gap seat can cross by noise, but the
  // wide-gap seat cannot once its CI is tight enough — that is the empirical
  // proof the sampled pipeline sits on the pooled side.
  GameDef def = three_seat_rooted({10, 10, 10});
  auto ranges = stacks10_ranges();
  CheckCallPolicy passive;
  std::vector<const BehaviorPolicy*> pp{&passive, &passive, &passive};
  const bs::solver::JointDealTable table =
      bs::solver::enumerate_joint_deals(ranges, {card("2c"), card("3d"), card("7h")});
  // Gains swing +-10 chips and only six compatible joint deals share the
  // seeds, so a wide confirm list is needed for the residual CI to sit around
  // zero with margin. The pinned list makes the CI bitwise reproducible
  // forever; it is not a freshly drawn 95% test.
  const std::vector<std::uint64_t> confirm = confirm_seeds(40000);
  double max_gap = -1.0;
  std::size_t wide_gap_seat = 0;
  for (std::size_t t = 0; t < 3; ++t) {
    const std::array<double, 10> br = exact_best_response_utility(def, ranges, pp, t, nullptr);
    const std::array<double, 10> omni =
        exact_omniscient_deviation_utility(def, ranges, pp, t, nullptr);
    const double gap = omni[t] - br[t];
    if (gap > max_gap) {
      max_gap = gap;
      wide_gap_seat = t;
    }
  }
  check(std::abs(max_gap - 0.371601) < 1e-6, "the pinned 0.371601 pooled gap exists");
  for (std::size_t t = 0; t < 3; ++t) {
    FrozenBestResponseData frozen = learn_exact_frozen_best_response(def, ranges, pp, t);
    BrEstimatorConfig config;
    std::vector<double> gains;
    gains.reserve(confirm.size());
    for (std::uint64_t seed : confirm) {
      ConfirmPairOutput pair =
          mc_confirm_pair(def, ranges, pp, t, seed, frozen, &table, config, nullptr);
      gains.push_back(pair.best_utility[t] - pair.profile_utility[t]);
    }
    const ConfidenceInterval gain_ci = confidence_interval_95(gains);
    const std::array<double, 10> profile = exact_oracle_utility(def, ranges, pp, nullptr);
    const std::array<double, 10> br = exact_best_response_utility(def, ranges, pp, t, nullptr);
    const std::array<double, 10> omni =
        exact_omniscient_deviation_utility(def, ranges, pp, t, nullptr);
    const double exact_gain = br[t] - profile[t];
    std::vector<double> residual;
    for (double g : gains)
      residual.push_back(g - exact_gain);
    const ConfidenceInterval residual_ci = confidence_interval_95(residual);
    check(residual_ci.lower <= 0.0 && residual_ci.upper >= 0.0,
          "the exact-frozen MC residual CI includes the exact pooled gain");
    std::printf("[br-mc pool] seat=%zu mc=%.5f pooled=%.5f omni-gain=%.5f gap=%.5f hw=%.5f\n", t,
                gain_ci.mean, exact_gain, omni[t] - profile[t], omni[t] - br[t],
                gain_ci.half_width);
    if (t == wide_gap_seat) {
      check(gain_ci.upper <= omni[t] - profile[t] - 0.5 * max_gap + 1e-9,
            "at the 0.371601-gap seat the MC gain CI stays strictly below the omniscient "
            "value by at least half the gap");
    }
  }
}

// ---------------------------------------------------------------------------
// Structural gates
// ---------------------------------------------------------------------------

void test_disjoint_seed_lists() {
  GameDef def = three_seat_river_rooted({1, 1, 1});
  CheckCallPolicy passive;
  std::vector<const BehaviorPolicy*> pp{&passive, &passive, &passive};
  auto ranges = three_seat_ranges();
  std::vector<int> board(def.board.begin(), def.board.begin() + def.board_size);
  const bs::solver::JointDealTable table = bs::solver::enumerate_joint_deals(ranges, board);
  BrEstimatorConfig config;
  config.max_epochs = 4;
  auto expect_throw = [&](const std::vector<std::uint64_t>& learn,
                          const std::vector<std::uint64_t>& confirm, const std::string& label) {
    bool threw = false;
    try {
      (void)estimate_deviation_gains(def, ranges, pp, learn, confirm, &table, config);
    } catch (const std::invalid_argument&) {
      threw = true;
    }
    check(threw, label);
  };
  expect_throw({}, {1, 2}, "an empty learn list is rejected");
  expect_throw({1, 2}, {}, "an empty confirm list is rejected");
  expect_throw({1, 2, 3}, {4, 2}, "an overlapping confirm seed is rejected");
  expect_throw({1, 17, 43}, {43, 44}, "pinned seed 43 cannot be in both lists");
  expect_throw({1, 2, 2}, {3, 4}, "a duplicate learn seed is rejected");
  expect_throw({1, 2}, {3, 3}, "a duplicate confirm seed is rejected");

  const std::vector<std::uint64_t> a{1, 2, 3};
  const std::vector<std::uint64_t> b{4, 5, 6};
  check(seed_list_hash(a) != seed_list_hash(b), "distinct seed lists hash differently");
  check(seed_list_hash(a) == seed_list_hash(std::vector<std::uint64_t>{1, 2, 3}),
        "the seed-list hash is deterministic");
  check(seed_list_hash(a) != seed_list_hash(std::vector<std::uint64_t>{3, 2, 1}),
        "the seed-list hash is order dependent");
}

void test_infoset_miss_policy() {
  GameDef def = three_seat_river_rooted({1, 1, 1});
  CheckCallPolicy passive;
  std::vector<const BehaviorPolicy*> pp{&passive, &passive, &passive};
  auto ranges = three_seat_ranges();
  std::vector<int> board(def.board.begin(), def.board.begin() + def.board_size);
  const bs::solver::JointDealTable table = bs::solver::enumerate_joint_deals(ranges, board);
  // One learn seed freezes only the few own-holdings that deal reaches.
  const std::vector<std::uint64_t> learn{100001};
  const std::vector<std::uint64_t> confirm = confirm_seeds(60);

  BrEstimatorConfig throwing;
  throwing.max_epochs = 8;
  bool missed = false;
  try {
    (void)estimate_deviation_gains(def, ranges, pp, learn, confirm, &table, throwing);
  } catch (const stage6_br_infoset_miss&) {
    missed = true;
  }
  check(missed, "an unfrozen confirm information set throws stage6_br_infoset_miss");

  BrEstimatorConfig recording = throwing;
  recording.on_confirm_miss = FrozenMissPolicy::RecordMiss;
  BrEstimatorResult result =
      estimate_deviation_gains(def, ranges, pp, learn, confirm, &table, recording);
  std::size_t total_misses = 0;
  for (std::size_t m : result.counts.confirm_misses_per_traverser)
    total_misses += m;
  check(total_misses > 0, "RecordMiss completes and counts the misses");
}

void test_frozen_policy_legality() {
  GameDef def = three_seat_rooted({1, 1, 1});
  CheckCallPolicy passive;
  std::vector<const BehaviorPolicy*> pp{&passive, &passive, &passive};

  // On a flop-rooted 3-seat game the opener is the first seat clockwise of
  // the button (seat 1); its exact freeze is the one that owns a root
  // information set and must serve a unit point mass there.
  constexpr std::size_t opener = 1;
  FrozenBestResponseData good_data =
      learn_exact_frozen_best_response(def, three_seat_ranges(), pp, opener);
  GameState root_state(def);
  HandLog root_log;
  const InfosetKey* root_key = nullptr;
  HoleCards root_own{};
  for (const auto& [key, action] : good_data.actions) {
    const HoleCards holding{key.own[0], key.own[1]};
    const InfosetKey candidate = make_infoset_key(root_state, root_log, opener, holding);
    if (candidate == key) {
      root_key = &key;
      root_own = holding;
      break;
    }
  }
  check(root_key != nullptr, "the opener's exact freeze has a root information set");
  FrozenBestResponsePolicy good(std::move(good_data));
  const std::vector<PolicyAction> point =
      good.distribution(root_state, opener, root_own, PolicyContext{&root_log, 1});
  check(point.size() == 1 && point[0].probability == 1.0 &&
            root_state.legal().contains(point[0].action),
        "the frozen policy serves one legal unit point mass at the root");

  // A corrupted table (target outside the legal interval) is rejected.
  FrozenBestResponseData corrupted =
      learn_exact_frozen_best_response(def, three_seat_ranges(), pp, opener);
  corrupted.actions.begin()->second = poker::Action{poker::ActionType::Raise, 999999};
  FrozenBestResponsePolicy bad(std::move(corrupted));
  std::vector<const BehaviorPolicy*> joint{&passive, &passive, &passive};
  joint[opener] = &bad;
  bool threw = false;
  try {
    (void)exact_oracle_utility(def, three_seat_ranges(), joint, nullptr);
  } catch (const stage6_br_error&) {
    threw = true;
  }
  check(threw, "a corrupted frozen action is rejected at query time, never clamped");

  // A query at the wrong seat throws.
  const FrozenBestResponseData clean =
      learn_exact_frozen_best_response(def, three_seat_ranges(), pp, opener);
  FrozenBestResponsePolicy clean_policy(clean);
  bool wrong_seat = false;
  try {
    (void)clean_policy.distribution(root_state, opener + 1, HoleCards{0, 1},
                                    PolicyContext{&root_log, 1});
  } catch (const stage6_br_error&) {
    wrong_seat = true;
  }
  check(wrong_seat, "an opener frozen table refuses the other seat's queries");
}

void test_stabilization_guard() {
  // Against two jams the bootstrap (jam at every own infoset) is systematically
  // wrong (folding dominates at many holdings), so one epoch cannot be a fixed
  // point.
  GameDef def = three_seat_river_rooted({1, 1, 1});
  JamPolicy jam;
  std::vector<const BehaviorPolicy*> jp{&jam, &jam, &jam};
  auto ranges = three_seat_ranges();
  std::vector<int> board(def.board.begin(), def.board.begin() + def.board_size);
  const bs::solver::JointDealTable table = bs::solver::enumerate_joint_deals(ranges, board);
  const std::vector<std::uint64_t> learn = seed_list(4000, 200);
  const std::vector<std::uint64_t> confirm = confirm_seeds(200);
  {
    BrEstimatorConfig config;
    config.max_epochs = 1;
    config.require_stabilization = true;
    bool threw = false;
    try {
      (void)estimate_deviation_gains(def, ranges, jp, learn, confirm, &table, config);
    } catch (const stage6_br_not_stabilized&) {
      threw = true;
    }
    check(threw, "max_epochs=1 with required stabilization throws when not at a fixed point");
  }
  {
    BrEstimatorConfig config;
    config.max_epochs = 1;
    config.require_stabilization = false;
    BrEstimatorResult result =
        estimate_deviation_gains(def, ranges, jp, learn, confirm, &table, config);
    bool any_unstabilized = false;
    for (const FrozenBestResponseData& f : result.frozen)
      if (!f.stabilized)
        any_unstabilized = true;
    check(any_unstabilized, "the non-required path reports stabilized == false");
    for (double y : result.nashconv_replicates)
      check(std::isfinite(y), "an unstabilized table still yields finite unbiased gains");
  }
}

void test_off_menu_profile_labeled() {
  // The profile bets 5, an off-grid total on stacks-6 geometry (menu ends in
  // 4 and the cap 6). The best-WITHIN-menu gain is allowed to be negative;
  // the run completes and every figure is finite.
  GameDef def = three_seat_river_rooted({6, 6, 6});
  OffMenuPolicy off;
  std::vector<const BehaviorPolicy*> pp{&off, &off, &off};
  auto ranges = three_seat_ranges();
  std::vector<int> board(def.board.begin(), def.board.begin() + def.board_size);
  const bs::solver::JointDealTable table = bs::solver::enumerate_joint_deals(ranges, board);
  BrEstimatorConfig config;
  config.max_epochs = 8;
  const std::vector<std::uint64_t> learn = seed_list(9000, 200);
  const std::vector<std::uint64_t> confirm = confirm_seeds(400);
  BrEstimatorResult result =
      estimate_deviation_gains(def, ranges, pp, learn, confirm, &table, config);
  for (std::size_t t = 0; t < 3; ++t) {
    for (double g : result.gain[t])
      check(std::isfinite(g), "off-menu profile gains are finite (of either sign)");
    std::printf("[br-offmenu] seat=%zu mean declared-menu gain=%.5f\n", t, result.mean_gain[t]);
  }
}

// ---------------------------------------------------------------------------
// CRN pairing, repeatability, dealing
// ---------------------------------------------------------------------------

void test_paired_rollout_conservation_and_crn() {
  // Flop-rooted with the EXACT freeze: every information set is covered, so
  // the paired phase explores the full 43x42 runout universe through the
  // RunoutCard CRN stream while staying on the legally frozen table. Stacks
  // {1,2,2} make jams legal for the two deep seats, so the pooled BR deviates
  // from the jam profile and best/profile divergence is actually reached.
  GameDef def = three_seat_rooted({1, 2, 2});
  JamPolicy jam;
  std::vector<const BehaviorPolicy*> jp{&jam, &jam, &jam};
  auto ranges = three_seat_ranges();
  std::vector<int> board(def.board.begin(), def.board.begin() + def.board_size);
  const bs::solver::JointDealTable table = bs::solver::enumerate_joint_deals(ranges, board);
  const std::vector<std::uint64_t> confirm = confirm_seeds(200);
  BrEstimatorConfig config;
  config.max_epochs = 8;
  const std::size_t traverser = 2;  // acts last, so a shared prefix always exists
  const FrozenBestResponseData frozen =
      learn_exact_frozen_best_response(def, ranges, jp, traverser);

  std::size_t observed_divergence = 0;
  std::size_t runout_events = 0;
  for (std::uint64_t seed : confirm) {
    ConfirmPairOutput pair =
        mc_confirm_pair(def, ranges, jp, traverser, seed, frozen, &table, config, nullptr);
    double sb = 0.0, sp = 0.0;
    for (std::size_t i = 0; i < 3; ++i) {
      sb += pair.best_utility[i];
      sp += pair.profile_utility[i];
    }
    check(std::abs(sb) < 1e-9 && std::abs(sp) < 1e-9, "each paired leg settles zero-sum");
    if (pair.diverged)
      ++observed_divergence;
    runout_events += pair.best_draw_log.size() + pair.profile_draw_log.size();
    // Global pairing invariant, not just the positional prefix: index every
    // best-leg draw by its full coordinates and require each profile-leg draw
    // at the same coordinate (node, purpose, seat, counter) to carry the same
    // value. CRN content addressing means equal coordinates MUST mean one
    // value, anywhere in the two logs.
    for (const CrnDrawEvent& b : pair.best_draw_log) {
      for (const CrnDrawEvent& p : pair.profile_draw_log) {
        if (b.node_token == p.node_token && b.purpose == p.purpose && b.seat == p.seat &&
            b.counter == p.counter)
          check(b.value == p.value,
                "equal CRN coordinates anywhere in the two legs map to one value");
      }
    }
  }
  check(runout_events > 0, "the flop-rooted rollouts recorded runout-card draws");
  // Same deal fingerprint across the two per-seat runs at one shared seed.
  ConfirmPairOutput seat2 =
      mc_confirm_pair(def, ranges, jp, 2, 17, frozen, &table, config, nullptr);
  const FrozenBestResponseData frozen0 = learn_exact_frozen_best_response(def, ranges, jp, 0);
  ConfirmPairOutput seat0 =
      mc_confirm_pair(def, ranges, jp, 0, 17, frozen0, &table, config, nullptr);
  check(seat2.deal_fingerprint == seat0.deal_fingerprint && seat2.deal_fingerprint != 0,
        "per-traverser runs at one seed share the same joint deal");
  check(observed_divergence > 0, "the jam fixture forces best/profile divergence");

  // On a saturated river-rooted fixture the full estimator merges legs at
  // nodes where the frozen action equals the profile draw.
  GameDef rdef = three_seat_river_rooted({1, 1, 1});
  std::vector<int> rboard(rdef.board.begin(), rdef.board.begin() + 5);
  const bs::solver::JointDealTable rtable =
      bs::solver::enumerate_joint_deals(three_seat_ranges(), rboard);
  CheckCallPolicy passive;
  std::vector<const BehaviorPolicy*> pp{&passive, &passive, &passive};
  const std::vector<std::uint64_t> learn = seed_list(4000, 200);
  BrEstimatorResult full =
      estimate_deviation_gains(rdef, three_seat_ranges(), pp, learn, confirm, &rtable, config);
  check(full.counts.paired_same_action_merges > 0,
        "some confirm nodes merge the frozen and profile action into one walk");
}

void test_repeatability() {
  GameDef def = three_seat_river_rooted({1, 1, 1});
  CheckCallPolicy passive;
  std::vector<const BehaviorPolicy*> pp{&passive, &passive, &passive};
  auto ranges = three_seat_ranges();
  std::vector<int> board(def.board.begin(), def.board.begin() + def.board_size);
  const bs::solver::JointDealTable table = bs::solver::enumerate_joint_deals(ranges, board);
  const std::vector<std::uint64_t> learn = seed_list(7000, 300);
  const std::vector<std::uint64_t> confirm = confirm_seeds(1000);
  BrEstimatorConfig config;
  config.max_epochs = 8;
  const BrEstimatorResult a =
      estimate_deviation_gains(def, ranges, pp, learn, confirm, &table, config);
  const BrEstimatorResult b =
      estimate_deviation_gains(def, ranges, pp, learn, confirm, &table, config);
  check(a.learn_seed_list_hash == b.learn_seed_list_hash &&
            a.confirm_seed_list_hash == b.confirm_seed_list_hash,
        "repeat runs name the same seed lists");
  for (std::size_t t = 0; t < 3; ++t) {
    check(a.frozen[t].content_hash() == b.frozen[t].content_hash(),
          "the frozen table is bitwise reproducible");
    check(a.frozen[t].epochs_run == b.frozen[t].epochs_run &&
              a.frozen[t].stabilized == b.frozen[t].stabilized,
          "epoch counts and stabilization reproduce");
    for (std::size_t s = 0; s < confirm.size(); ++s)
      check(a.gain[t][s] == b.gain[t][s], "per-seed gains are bitwise reproducible");
  }
  for (std::size_t s = 0; s < confirm.size(); ++s)
    check(a.nashconv_replicates[s] == b.nashconv_replicates[s],
          "nashconv replicates are bitwise reproducible");
  // Traverser-loop order independence: learning seat 2 alone gives the same
  // frozen table as the full run's seat 2.
  const FrozenBestResponseData alone =
      mc_learn_frozen_best_response(def, ranges, pp, 2, learn, &table, config, nullptr);
  check(alone.content_hash() == a.frozen[2].content_hash(),
        "per-traverser learn output is independent of traverser loop order");
}

void test_deal_draw_matches_table() {
  auto ranges = three_seat_ranges();
  const bs::solver::JointDealTable table =
      bs::solver::enumerate_joint_deals(ranges, {card("2c"), card("3d"), card("7h")});
  check(table.deals.size() == 6, "the pinned joint table has six compatible deals");
  constexpr std::size_t draws = 60000;
  std::vector<std::size_t> hits(table.deals.size(), 0);
  std::size_t restarts_scalable = 0;
  for (std::size_t k = 0; k < draws; ++k) {
    bs::SplitMix64 rng = crn_deal_rng(20000 + static_cast<std::uint64_t>(k));
    const std::size_t index = bs::solver::sample_joint_deal(table, ranges, rng);
    ++hits[index];
  }
  for (std::size_t d = 0; d < table.deals.size(); ++d) {
    const double empirical = static_cast<double>(hits[d]) / static_cast<double>(draws);
    // Multinomial 5-sigma tolerance at 60k draws (~0.0022); pinned 5x loose.
    check(std::abs(empirical - table.deals[d].weight) < 0.01,
          "keyed joint-deal frequencies match the table measure");
    std::size_t attempts = 0;
    bs::SplitMix64 rng = crn_deal_rng(30000 + static_cast<std::uint64_t>(d));
    (void)bs::solver::sample_scalable_joint_deal(ranges, {card("2c"), card("3d"), card("7h")}, rng,
                                                 100000, &attempts);
    restarts_scalable += attempts;
  }
  std::printf("[br-deal] table draws=%zu scalable attempts(6)=%zu\n", draws, restarts_scalable);
}

// ---------------------------------------------------------------------------
// NashConv scalar statistics
// ---------------------------------------------------------------------------

void test_nashconv_scalar_statistics() {
  GameDef def = three_seat_river_rooted({1, 1, 1});
  CheckCallPolicy passive;
  std::vector<const BehaviorPolicy*> pp{&passive, &passive, &passive};
  auto ranges = three_seat_ranges();
  std::vector<int> board(def.board.begin(), def.board.begin() + def.board_size);
  const bs::solver::JointDealTable table = bs::solver::enumerate_joint_deals(ranges, board);
  const std::vector<std::uint64_t> learn = seed_list(8000, 300);
  const std::vector<std::uint64_t> confirm = confirm_seeds(1000);
  BrEstimatorConfig config;
  config.max_epochs = 8;
  const BrEstimatorResult r =
      estimate_deviation_gains(def, ranges, pp, learn, confirm, &table, config);
  for (std::size_t s = 0; s < confirm.size(); ++s) {
    double y = 0.0;
    for (std::size_t t = 0; t < 3; ++t)
      y += r.gain[t][s];
    check(y == r.nashconv_replicates[s], "Y_s is the sum of the per-seat gains");
  }
  const ConfidenceInterval direct = confidence_interval_95(r.nashconv_replicates);
  check(std::abs(direct.mean - r.mean_nashconv) < 1e-12, "mean Y matches the result field");
  // Paired difference on identical seeds equals the direct difference series.
  std::vector<double> shifted(r.nashconv_replicates.size());
  for (std::size_t s = 0; s < shifted.size(); ++s)
    shifted[s] = r.nashconv_replicates[s] + 0.07;
  const ConfidenceInterval paired = paired_difference_ci_95(shifted, r.nashconv_replicates);
  check(std::abs(paired.mean - 0.07) < 1e-9,
        "the paired difference CI is computed on the within-seed differences");
  std::printf("[br-stats] mean NashConv=%.5f CI=[%+.4f,%+.4f] chips/hand (estimate)\n",
              r.mean_nashconv, direct.lower, direct.upper);
}

// ---------------------------------------------------------------------------
// Heads-up cross-check against bs::solver::HeadsUpTrainer::evaluate
// ---------------------------------------------------------------------------

void test_two_seat_nash_conv_crosscheck() {
  // Two chips behind with a fixed pot: every aggressive node is all-in-only
  // ([2,2] over a check-to, no reopening after the jam), so the solver's
  // abstract menu and the declared deviation menu coincide EXACTLY and there
  // is no snapping delta. Free turn/river, two compatible combos per seat.
  using bs::solver::HeadsUpGame;
  using bs::solver::HeadsUpTrainer;
  using bs::solver::TrainingLimits;
  using bs::solver::TrainingResult;
  HeadsUpGame game;
  game.root = {{card("2c"), card("3d"), card("7h")}, {2, 2}, {1, 1}, 2, static_cast<Chips>(1),
               static_cast<std::size_t>(1)};
  game.ranges[0] = {{{card("As"), card("Ks")}, 1}, {{card("Ah"), card("Kh")}, 1}};
  game.ranges[1] = {{{card("Ac"), card("Kc")}, 1}, {{card("Qd"), card("Jd")}, 1}};

  TrainingLimits limits;
  limits.max_nodes = 200000000;
  limits.max_information_sets = 20000000;
  limits.time = std::chrono::minutes{5};
  TrainingResult trained = HeadsUpTrainer(game).train(8, limits);
  check(trained.status == bs::solver::TrainingStatus::Complete,
        "the full-traversal heads-up policy completes on the tiny fixture");
  const bs::solver::ExactEvaluation ev = HeadsUpTrainer(game).evaluate(trained.policy, limits);

  // Replay the trained abstract policy on the unified GameState tree. Every
  // reached solver row must equal the declared deviation menu; a mismatch is a
  // test failure (it would invalidate the menu-alignment premise).
  class TrainedBehavior final : public BehaviorPolicy {
   public:
    const bs::solver::HeadsUpPolicy* policy = nullptr;
    poker::HeadsUpRoot root{};
    std::vector<PolicyAction> distribution(const GameState& state, std::size_t seat, HoleCards hole,
                                           const PolicyContext& context) const override {
      poker::HeadsUpState hu(root);
      const HandLog& log = *context.hand_log;
      auto replay = [&](const std::vector<LoggedAction>& actions) {
        for (const LoggedAction& a : actions)
          hu = hu.after_action(a.seat, a.action);
      };
      replay(log.flop);
      if (state.board().size() >= 4)
        hu = hu.after_card(state.board()[3]);
      replay(log.turn);
      if (state.board().size() >= 5)
        hu = hu.after_card(state.board()[4]);
      replay(log.river);
      const std::array<int, 2> own{hole[0], hole[1]};
      const bs::solver::PolicyRow* row = policy->lookup(hu, own);
      if (!row)
        throw std::runtime_error("cross-check: trained policy missing a reached row");
      const std::vector<poker::Action> menu = declared_behavior_menu(state, seat);
      if (row->actions != menu)
        throw std::runtime_error("cross-check: abstract and declared menus differ");
      std::vector<PolicyAction> dist;
      for (std::size_t i = 0; i < row->actions.size(); ++i)
        dist.push_back({row->actions[i], row->probabilities[i]});
      return dist;
    }
  };

  GameDef def{};
  def.player_count = 2;
  def.button = 1;
  def.big_blind = 1;
  def.stacks = {2, 2, 0, 0, 0, 0, 0, 0, 0, 0};
  def.contributions = {1, 1, 0, 0, 0, 0, 0, 0, 0, 0};
  def.pot = 2;
  def.board = {card("2c"), card("3d"), card("7h"), 0, 0};
  def.board_size = 3;

  TrainedBehavior behavior;
  behavior.policy = &trained.policy;
  behavior.root = game.root;
  std::vector<const BehaviorPolicy*> pp{&behavior, &behavior};
  std::vector<std::vector<MultiwayWeightedHand>> ranges{
      {{{card("As"), card("Ks")}, 1.0}, {{card("Ah"), card("Kh")}, 1.0}},
      {{{card("Ac"), card("Kc")}, 1.0}, {{card("Qd"), card("Jd")}, 1.0}},
  };
  const bs::solver::JointDealTable table =
      bs::solver::enumerate_joint_deals(ranges, {card("2c"), card("3d"), card("7h")});
  const std::vector<std::uint64_t> confirm = confirm_seeds(4000);
  BrEstimatorConfig config;
  // This two-seat case is rooted at the flop, so the estimator side averages
  // over the free turn/river with the EXACT frozen BR (every runout
  // information set covered) and Monte Carlo confirm seeds; the phase-2
  // sample is unbiased for the solver's uniform runout average.
  std::vector<std::vector<double>> gains(2);
  std::array<double, 2> profile_leg_sum{0.0, 0.0};
  std::array<double, 2> best_leg_sum{0.0, 0.0};
  for (std::size_t t = 0; t < 2; ++t) {
    const FrozenBestResponseData frozen = learn_exact_frozen_best_response(def, ranges, pp, t);
    for (std::uint64_t seed : confirm) {
      ConfirmPairOutput pair =
          mc_confirm_pair(def, ranges, pp, t, seed, frozen, &table, config, nullptr);
      gains[t].push_back(pair.best_utility[t] - pair.profile_utility[t]);
      profile_leg_sum[t] += pair.profile_utility[t];
      best_leg_sum[t] += pair.best_utility[t];
    }
  }
  std::printf("[br-2p legs] profile means=[%.5f,%.5f] best means=[%.5f,%.5f]\n",
              profile_leg_sum[0] / confirm.size(), profile_leg_sum[1] / confirm.size(),
              best_leg_sum[0] / confirm.size(), best_leg_sum[1] / confirm.size());
  std::vector<double> y_series;
  y_series.reserve(confirm.size());
  for (std::size_t s = 0; s < confirm.size(); ++s)
    y_series.push_back(gains[0][s] + gains[1][s]);
  const ConfidenceInterval y_ci = confidence_interval_95(y_series);
  std::vector<double> residual;
  residual.reserve(y_series.size());
  for (double y : y_series)
    residual.push_back(y - ev.nash_conv);
  const ConfidenceInterval ci = confidence_interval_95(residual);
  std::printf(
      "[br-2p] solver nash_conv=%.5f mc NashConv=%.5f ci=[%+.4f,%+.4f] profile=[%.4f,%.4f]\n",
      ev.nash_conv, y_ci.mean, ci.lower, ci.upper, ev.profile_value[0], ev.profile_value[1]);
  check(std::abs(y_ci.mean - ev.nash_conv) < 0.12,
        "MC NashConv agrees with HeadsUpTrainer::evaluate within the pinned tolerance");
  check(ci.lower <= 0.0 && ci.upper >= 0.0,
        "the 2p residual CI includes the exact solver nash_conv");
}

// ---------------------------------------------------------------------------
// Coarse deviation space (R11 coarse-game NashConv)
// ---------------------------------------------------------------------------

bs::abstraction::ActionAbstraction trainer_coarse_action() {
  bs::abstraction::SizeSchedule schedule = bs::abstraction::default_size_schedule();
  for (auto& street : schedule) {
    street.bets = {{1, 2}};
    street.raises = {{1, 1}};
  }
  return bs::abstraction::ActionAbstraction::declared(schedule,
                                                      bs::abstraction::CoverSeeds::DeclaredOnly);
}

void test_coarse_deviation_space() {
  const bs::abstraction::ActionAbstraction coarse = trainer_coarse_action();
  GameDef def = three_seat_river_rooted({4, 4, 4});
  auto ranges = three_seat_ranges();
  std::vector<int> board(def.board.begin(), def.board.begin() + def.board_size);
  const bs::solver::JointDealTable table = bs::solver::enumerate_joint_deals(ranges, board);
  CheckCallPolicy passive;
  std::vector<const BehaviorPolicy*> pp{&passive, &passive, &passive};
  const std::vector<std::uint64_t> confirm = confirm_seeds(400);

  // Config validation.
  {
    BrEstimatorConfig bad;
    bad.deviation_space = DeviationSpace::CoarseAbstraction;
    bad.coarse_action = nullptr;
    bool threw = false;
    try {
      (void)estimate_deviation_gains(def, ranges, pp, seed_list(9000, 64), confirm, &table, bad);
    } catch (const std::invalid_argument&) {
      threw = true;
    }
    check(threw, "coarse deviation space without a coarse_action is refused");
  }

  BrEstimatorConfig coarse_cfg;
  coarse_cfg.deviation_space = DeviationSpace::CoarseAbstraction;
  coarse_cfg.coarse_action = &coarse;
  coarse_cfg.on_confirm_miss = FrozenMissPolicy::RecordMiss;
  const std::vector<std::uint64_t> learn = seed_list(70000, 4000);
  BrEstimatorResult coarse_result =
      estimate_deviation_gains(def, ranges, pp, learn, confirm, &table, coarse_cfg);

  BrEstimatorConfig declared_cfg;
  declared_cfg.on_confirm_miss = FrozenMissPolicy::RecordMiss;
  BrEstimatorResult declared_result =
      estimate_deviation_gains(def, ranges, pp, learn, confirm, &table, declared_cfg);

  for (std::size_t t = 0; t < 3; ++t) {
    const FrozenBestResponseData& cf = coarse_result.frozen[t];
    check(cf.menu_identity_hash != declared_menu_identity_hash(),
          "the coarse frozen table carries a distinct, tagged menu identity");
    check(cf.stabilized, "the coarse learn table stabilizes");

    // Passive opponents never raise, so every traverser node is check-to: the
    // coarse menu is exactly {check, half-pot bet} at every information set.
    // Structural subset proof — a coarse frozen table can never contain an
    // R8-only 1/3, 3/4 or 3/2 fraction aggression.
    for (const auto& [key, menu] : cf.menus) {
      check(menu.size() == 2, "coarse menu at a check-to node has exactly two actions");
      check(menu[0].type == poker::ActionType::Check, "coarse check-to menu starts with check");
      check(menu[1].type == poker::ActionType::Bet, "coarse check-to menu offers one bet");
    }

    // The ROOT key's frozen menu is exactly the shared L3 coarse rule for the
    // root state, per traverser holding the learn seeds can reach.
    for (const auto& hand : ranges[t]) {
      const HoleCards hole{hand.cards[0], hand.cards[1]};
      const InfosetKey root_key = make_infoset_key(GameState(def), HandLog{}, t, hole);
      auto it = cf.menus.find(root_key);
      if (it == cf.menus.end())
        continue;  // deal-conflicted holding on this seed support
      const std::vector<poker::Action> expected =
          bs::tree::abstract_node_menu(GameState(def), coarse, GameState(def).legal());
      check(it->second == expected,
            "the coarse frozen root menu equals the trainer's L3 coarse menu");
    }

    // Monotonicity: the best response over the coarse SUBSET can never beat
    // the best response over the full declared menu in expectation. The two
    // runs share learn and confirm seed lists; both are unbiased MC estimates
    // and the theoretical gap is >= 0.
    check(declared_result.mean_gain[t] + 0.05 >= coarse_result.mean_gain[t],
          "coarse-subset BR gain never exceeds the full declared-menu BR gain");

    // A (learned) best response never loses against its opponents: the
    // estimated gain is non-negative within Monte-Carlo noise.
    check(coarse_result.mean_gain[t] >= -0.02,
          "coarse-space best-response gain against passive opponents is non-negative");
  }
  std::printf("[br-coarse] coarse NashConv=%.5f declared NashConv=%.5f\n",
              coarse_result.mean_nashconv, declared_result.mean_nashconv);
  check(coarse_result.mean_nashconv <= declared_result.mean_nashconv + 0.05,
        "coarse-game NashConv is bounded above by declared-menu NashConv");

  // A second coarse run stamps the same identity.
  BrEstimatorResult again =
      estimate_deviation_gains(def, ranges, pp, learn, confirm, &table, coarse_cfg);
  for (std::size_t t = 0; t < 3; ++t)
    check(again.frozen[t].menu_identity_hash == coarse_result.frozen[t].menu_identity_hash,
          "the coarse menu identity is deterministic across runs");
}

}  // namespace

int main() {
  test_exact_freeze_equals_oracle();
  test_exact_freeze_strict_pooling();
  test_mc_gain_passive();
  test_mc_gain_sidepots();
  test_mc_gain_stochastic();
  test_mc_gain_with_runout_chance();
  test_mc_learn_with_chance_branching();
  test_mc_never_reaches_omniscient();
  test_disjoint_seed_lists();
  test_infoset_miss_policy();
  test_frozen_policy_legality();
  test_stabilization_guard();
  test_off_menu_profile_labeled();
  test_paired_rollout_conservation_and_crn();
  test_repeatability();
  test_deal_draw_matches_table();
  test_nashconv_scalar_statistics();
  test_two_seat_nash_conv_crosscheck();
  test_coarse_deviation_space();
  if (failures) {
    std::fprintf(stderr, "STAGE6 BR ESTIMATOR TESTS FAILED: %d\n", failures);
    return 1;
  }
  std::puts("STAGE6 BR ESTIMATOR TESTS PASSED");
  return 0;
}
