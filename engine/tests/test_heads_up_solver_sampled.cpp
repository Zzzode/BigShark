// Independent regressions for RFC 0004 Stage 3 sampled traversal.
//
// Coverage:
//  - pinned SplitMix64 revision-1 vectors;
//  - enumerated expected one-sweep sampled updates: raw regrets equal the
//    full-traversal sweep and kSimple averages equal it only after behavior
//    normalization, including non-uniform joint weights and a non-fixed
//    public card;
//  - hand-derived exact full-sweep literals from chip accounting and hand
//    evaluation, with no shared strategy code;
//  - repeatability, seed behavior, and iteration/PRNG rollback under node,
//    information-set, byte, and depth caps;
//  - fixed (<=0.002 pot) and sampled-chance (<=0.02 pot) convergence gates;
//  - an independent backward-induction best response over a tree whose public
//    card is not fixed, counter-checking the modeled response() aggregation
//    (the Stage 2 P2 review finding).
#include <algorithm>
#include <array>
#include <bs/eval.hpp>
#include <bs/heads_up.hpp>
#include <bs/heads_up_solver.hpp>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <functional>
#include <map>
#include <numeric>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "gto/heads_up_solver_debug.hpp"

using namespace bs::poker;
using namespace bs::solver;

#define CHECK(cond)                                                         \
  do {                                                                      \
    if (!(cond)) {                                                          \
      std::printf("CHECK failed: %s (%s:%d)\n", #cond, __FILE__, __LINE__); \
      return 1;                                                             \
    }                                                                       \
  } while (0)

namespace {

using Hand = std::array<int, 2>;
using Holes = std::array<Hand, 2>;

int card(const char* name) {
  return bs::cardId(std::string(name));
}

bool near(double a, double b, double tolerance = 1e-12) {
  return std::isfinite(a) && std::isfinite(b) && std::abs(a - b) <= tolerance;
}

template <typename Exception, typename F>
bool throws_as(F&& operation) {
  try {
    operation();
  } catch (const Exception&) {
    return true;
  } catch (...) {
    return false;
  }
  return false;
}

TrainingLimits large_budget() {
  TrainingLimits limits;
  limits.max_nodes = 200000000;
  limits.max_information_sets = 20000000;
  limits.time = std::chrono::minutes{10};
  return limits;
}

// Convergence smoke tests must COMPLETE rather than hit a wall-clock budget.
// The ASan/UBSan build instruments every map node visit and can run one to two
// orders of magnitude slower than Release on a loaded machine, so the deadline
// is deliberately loose; deterministic interruption is covered separately by
// test_resource_rollback.
TrainingLimits completion_budget() {
  TrainingLimits limits = large_budget();
  limits.time = std::chrono::minutes{30};
  return limits;
}

// Two combos per side, asymmetric weights, fixed turn Js and river 9c.
HeadsUpGame fixed_fixture() {
  HeadsUpGame game;
  game.root = {{card("2c"), card("3d"), card("7h")}, {1, 1}, {1, 1}, 2, 1, 1};
  game.ranges[0] = {{{card("Ac"), card("Ad")}, 2}, {{card("8c"), card("8d")}, 3}};
  game.ranges[1] = {{{card("Ac"), card("As")}, 5}, {{card("Qc"), card("Qd")}, 7}};
  game.fixed_runout = {card("Js"), card("9c")};
  return game;
}

// Same weighted ranges; fixed turn Js, free river. Chance is below every
// decision and the decision tree stays bounded for sampled coverage.
HeadsUpGame free_river_fixture() {
  HeadsUpGame game = fixed_fixture();
  game.fixed_runout = {card("Js"), std::nullopt};
  return game;
}

// One combo per side, free turn, fixed river 9c: chance (44 cards) is above
// later decisions. Used by the independent chance-conditioned BR counter-check.
HeadsUpGame free_turn_fixture() {
  HeadsUpGame game;
  game.root = {{card("2c"), card("3d"), card("7h")}, {1, 1}, {1, 1}, 2, 1, 1};
  game.ranges[0] = {{{card("Ac"), card("Kd")}, 1}};
  game.ranges[1] = {{{card("Qh"), card("Jh")}, 1}};
  game.fixed_runout = {std::nullopt, card("9c")};
  return game;
}

// Two weighted combos per side with TWO chips behind (matched one-chip
// contributions into a pot of two, big blind one) and both runouts fixed.
// Every one-chip fixture elsewhere collapses all aggressive fractions onto a
// single all-in target, so every decision node has exactly two actions; with
// two chips behind the minimum bet and the jam are distinct, leaving three
// abstract actions at every decision (check/min-bet/jam out of position and
// fold/call/jam facing a bet). Three chips or more would be closer to a six to
// twelve chip profile, but the min-raise reopen chains across three streets
// grow the complete tree past both the enumerated draw-vector cap and the
// exact evaluator budget; two behind is the deepest profile that exercises
// three-or-more-action sampled nodes with a tractable complete tree.
HeadsUpGame multi_size_fixture() {
  HeadsUpGame game;
  game.root = {{card("2c"), card("3d"), card("7h")}, {2, 2}, {1, 1}, 2, 1, 1};
  game.ranges[0] = {{{card("As"), card("Ks")}, 2}, {{card("Qh"), card("Jh")}, 3}};
  game.ranges[1] = {{{card("Ac"), card("Kc")}, 5}, {{card("Qd"), card("Jd")}, 7}};
  game.fixed_runout = {card("9h"), card("8s")};
  return game;
}

struct JointDeal {
  Holes holes;
  double probability;
};

std::vector<JointDeal> enumerate_deals(const HeadsUpGame& game) {
  std::vector<JointDeal> result;
  long double total = 0;
  for (const auto& first : game.ranges[0]) {
    for (const auto& second : game.ranges[1]) {
      std::set<int> used(game.root.flop.begin(), game.root.flop.end());
      for (auto fixed : game.fixed_runout)
        if (fixed)
          used.insert(*fixed);
      bool compatible = true;
      for (int c : first.cards)
        compatible = used.insert(c).second && compatible;
      for (int c : second.cards)
        compatible = used.insert(c).second && compatible;
      if (!compatible)
        continue;
      const long double mass = static_cast<long double>(first.weight) * second.weight;
      total += mass;
      result.push_back({{first.cards, second.cards}, static_cast<double>(mass)});
    }
  }
  if (total <= 0)
    throw std::invalid_argument("oracle has no compatible deals");
  for (auto& deal : result)
    deal.probability = static_cast<double>(static_cast<long double>(deal.probability) / total);
  return result;
}

std::vector<int> enumerate_cards(const HeadsUpGame& game, const HeadsUpState& state,
                                 const Holes& holes) {
  const auto fixed = game.fixed_runout[state.board().size() - 3];
  if (fixed)
    return {*fixed};
  std::set<int> unavailable(state.board().begin(), state.board().end());
  for (const auto& hand : holes)
    unavailable.insert(hand.begin(), hand.end());
  for (auto reserved : game.fixed_runout)
    if (reserved)
      unavailable.insert(*reserved);
  std::vector<int> result;
  for (int c = 0; c < 52; ++c)
    if (!unavailable.contains(c))
      result.push_back(c);
  return result;
}

bool same_actions(const std::vector<Action>& a, const std::vector<Action>& b) {
  return a == b;
}

// --- Pinned PRNG -----------------------------------------------------------

int test_prng_pinned() {
  const auto zero = HeadsUpSolverDebug::splitmix64(0, 4);
  CHECK((zero == std::vector<std::uint64_t>{0xe220a8397b1dcdafULL, 0x6e789e6aa1b965f4ULL,
                                            0x06c45d188009454fULL, 0xf88bb8a8724c81ecULL}));
  CHECK(HeadsUpSolverDebug::splitmix64(1, 1)[0] == 0x910a2dec89025cc1ULL);
  CHECK(HeadsUpSolverDebug::splitmix64(43, 1)[0] == 0xba69ec90eb4fef88ULL);
  return 0;
}

// --- Enumerated sampled-update expectation ---------------------------------

struct DrawVector {
  std::vector<std::uint64_t> draws;
  double probability = 1;
};

// A 64-bit draw whose top 53 bits place the unit double inside the closed-open
// CDF bin containing point. Points are interior bin midpoints.
std::uint64_t unit_draw_for(double point) {
  return static_cast<std::uint64_t>(point * static_cast<double>(1ULL << 53)) << 11;
}

// A bounded draw that survives rejection and resolves to index. The rejection
// threshold is smaller than the bound, so index + bound always qualifies and
// reduces to index.
std::uint64_t card_draw_for(std::size_t index, std::size_t bound) {
  if (bound == 1)
    return 0;
  return static_cast<std::uint64_t>(index) + static_cast<std::uint64_t>(bound);
}

using VectorTable = std::map<InformationKey, DebugRow>;

// Accumulates one observed episode row, weighted by the episode probability.
void add_weighted(VectorTable& table, const InformationKey& key, const DebugRow& row,
                  double weight) {
  auto it = table.find(key);
  if (it == table.end()) {
    DebugRow scaled = row;
    for (double& v : scaled.regrets)
      v *= weight;
    for (double& v : scaled.sums)
      v *= weight;
    table.emplace(key, std::move(scaled));
    return;
  }
  if (!(it->second.actions == row.actions) || it->second.regrets.size() != row.regrets.size())
    throw std::runtime_error("enumerated update action mismatch");
  for (std::size_t a = 0; a < row.regrets.size(); ++a)
    it->second.regrets[a] += weight * row.regrets[a];
  for (std::size_t a = 0; a < row.sums.size(); ++a)
    it->second.sums[a] += weight * row.sums[a];
}

// Complete draw-vector suffixes below `state`. Traverser actions are all
// walked within one episode, so suffixes combine as a cartesian product;
// chance and opponent actions are sampled alternatives.
std::vector<DrawVector> enumerate_suffixes(const HeadsUpGame& game, const HeadsUpState& state,
                                           const Holes& holes, std::size_t traverser) {
  if (state.phase() == Phase::Folded || state.phase() == Phase::Showdown)
    return {{}};
  if (state.phase() == Phase::Deal) {
    const auto cards = enumerate_cards(game, state, holes);
    std::vector<DrawVector> result;
    const double p = 1.0 / static_cast<double>(cards.size());
    for (std::size_t i = 0; i < cards.size(); ++i) {
      for (auto& suffix : enumerate_suffixes(game, state.after_card(cards[i]), holes, traverser)) {
        suffix.draws.insert(suffix.draws.begin(), card_draw_for(i, cards.size()));
        suffix.probability *= p;
        result.push_back(std::move(suffix));
      }
    }
    return result;
  }
  const std::size_t actor = *state.actor();
  const auto actions = abstract_actions(state, game.sizes);
  std::vector<std::vector<DrawVector>> per_child;
  per_child.reserve(actions.size());
  for (const Action& action : actions)
    per_child.push_back(
        enumerate_suffixes(game, state.after_action(actor, action), holes, traverser));
  if (actor == traverser) {
    std::vector<DrawVector> combined{{}};
    for (const auto& child_vectors : per_child) {
      std::vector<DrawVector> next;
      next.reserve(combined.size() * child_vectors.size());
      for (const auto& prefix : combined)
        for (const auto& suffix : child_vectors) {
          DrawVector joined = prefix;
          joined.draws.insert(joined.draws.end(), suffix.draws.begin(), suffix.draws.end());
          joined.probability *= suffix.probability;
          next.push_back(std::move(joined));
        }
      combined = std::move(next);
    }
    return combined;
  }
  std::vector<DrawVector> result;
  const double p = 1.0 / static_cast<double>(actions.size());
  for (std::size_t a = 0; a < actions.size(); ++a) {
    for (auto& suffix : per_child[a]) {
      suffix.draws.insert(suffix.draws.begin(), unit_draw_for((static_cast<double>(a) + 0.5) * p));
      suffix.probability *= p;
      result.push_back(std::move(suffix));
    }
  }
  return result;
}

// Prescribed-policy suffix enumeration. Identical to enumerate_suffixes
// except opponent outcomes are sampled from the prescribed per-node vector
// (cumulative positive-weight CDF bins) instead of uniform 1/n. A zero entry
// emits no vectors at all, which is how the zero branch is proven absent;
// traverser actions still combine as a cartesian product, so a zero-sigma
// traverser action remains enumerated.
std::vector<DrawVector> enumerate_suffixes_seeded(const HeadsUpGame& game,
                                                  const HeadsUpState& state, const Holes& holes,
                                                  std::size_t traverser,
                                                  const PrescribedPolicy& prescribed) {
  if (state.phase() == Phase::Folded || state.phase() == Phase::Showdown)
    return {{}};
  if (state.phase() == Phase::Deal) {
    const auto cards = enumerate_cards(game, state, holes);
    std::vector<DrawVector> result;
    const double p = 1.0 / static_cast<double>(cards.size());
    for (std::size_t i = 0; i < cards.size(); ++i) {
      for (auto& suffix : enumerate_suffixes_seeded(game, state.after_card(cards[i]), holes,
                                                    traverser, prescribed)) {
        suffix.draws.insert(suffix.draws.begin(), card_draw_for(i, cards.size()));
        suffix.probability *= p;
        result.push_back(std::move(suffix));
      }
    }
    return result;
  }
  const std::size_t actor = *state.actor();
  const auto actions = abstract_actions(state, game.sizes);
  std::vector<std::vector<DrawVector>> per_child;
  per_child.reserve(actions.size());
  for (const Action& action : actions)
    per_child.push_back(enumerate_suffixes_seeded(game, state.after_action(actor, action), holes,
                                                  traverser, prescribed));
  if (actor == traverser) {
    std::vector<DrawVector> combined{{}};
    for (const auto& child_vectors : per_child) {
      std::vector<DrawVector> next;
      next.reserve(combined.size() * child_vectors.size());
      for (const auto& prefix : combined)
        for (const auto& suffix : child_vectors) {
          DrawVector joined = prefix;
          joined.draws.insert(joined.draws.end(), suffix.draws.begin(), suffix.draws.end());
          joined.probability *= suffix.probability;
          next.push_back(std::move(joined));
        }
      combined = std::move(next);
    }
    return combined;
  }
  const auto found = prescribed.find(information_key(state, holes[actor]));
  const std::vector<double> uniform(actions.size(), 1.0 / static_cast<double>(actions.size()));
  const std::vector<double>& weights = found == prescribed.end() ? uniform : found->second;
  std::vector<DrawVector> result;
  double cumulative = 0;
  for (std::size_t a = 0; a < actions.size(); ++a) {
    if (weights[a] == 0.0)
      continue;  // Zero-probability branch: zero draw-space width, no vectors.
    const double low = cumulative;
    cumulative += weights[a];
    for (auto& suffix : per_child[a]) {
      suffix.draws.insert(suffix.draws.begin(), unit_draw_for(low + weights[a] * 0.5));
      suffix.probability *= weights[a];
      result.push_back(std::move(suffix));
    }
  }
  return result;
}

// Enumerates every complete sampled episode across weighted joint deals and
// aggregates the exact expected raw rows for one traverser. Regret rows land
// on the traverser's own information sets; kSimple sum rows land on the
// opponent information sets of this sweep.
VectorTable enumerate_expected_updates(const HeadsUpGame& game, std::size_t traverser,
                                       std::size_t vector_cap) {
  const auto deals = enumerate_deals(game);
  double cumulative = 0;
  double total_mass = 0;
  std::size_t total_vectors = 0;
  VectorTable expected;
  for (const JointDeal& deal : deals) {
    const double low = cumulative;
    cumulative += deal.probability;
    const double midpoint = (low + cumulative) * 0.5;
    auto suffixes = enumerate_suffixes(game, HeadsUpState(game.root), deal.holes, traverser);
    total_vectors += suffixes.size();
    if (total_vectors > vector_cap)
      throw std::runtime_error("enumerated vector cap exceeded");
    for (const DrawVector& suffix : suffixes) {
      const double probability = deal.probability * suffix.probability;
      total_mass += probability;
      DrawVector full;
      full.draws.push_back(unit_draw_for(midpoint));
      full.draws.insert(full.draws.end(), suffix.draws.begin(), suffix.draws.end());
      const auto observed =
          HeadsUpSolverDebug::run_episode_scripted(game, full.draws, traverser, large_budget());
      if (observed.entropy_consumed != full.draws.size())
        throw std::runtime_error("scripted entropy mismatch");
      for (const auto& [key, row] : observed.rows)
        add_weighted(expected, key, row, probability);
    }
  }
  if (!near(total_mass, 1, 1e-9))
    throw std::runtime_error("enumerated probability mass mismatch");
  return expected;
}

std::vector<double> normalized(const std::vector<double>& row) {
  double sum = 0;
  for (double v : row)
    sum += v;
  if (!(sum > 0))
    throw std::runtime_error("cannot normalize a zero average row");
  std::vector<double> out(row.size());
  for (std::size_t a = 0; a < row.size(); ++a)
    out[a] = row[a] / sum;
  return out;
}

// Enumerates every complete sampled episode across weighted joint deals and
// compares the exact expected raw regrets (against the traverser full sweep)
// and the expected kSimple behavior (against the opponent full sweep) to the
// full-traversal table. Regrets are an unbiased raw identity. The average
// identity holds only after behavior normalization: per RFC 0004 lines
// 194-195, "sampled unnormalized averages may differ by a constant chance
// factor; compare normalized expected averages."
int expected_iteration_updates(const HeadsUpGame& game, std::size_t traverser,
                               const VectorTable& regret_sweep, const VectorTable& average_sweep,
                               double tolerance, std::size_t vector_cap) {
  const VectorTable expected = enumerate_expected_updates(game, traverser, vector_cap);

  for (const auto& [key, observed] : expected) {
    const auto regret_it = regret_sweep.find(key);
    const auto average_it = average_sweep.find(key);
    // The information key prefix is {actor, own0, own1, board size, ...}.
    const bool traverser_owned = key.front() == traverser;
    if (regret_it == regret_sweep.end() || average_it == average_sweep.end())
      return 1;
    if (!(regret_it->second.actions == observed.actions))
      return 1;
    if (traverser_owned) {
      // Raw regrets: unbiased external-sampling estimate at traverser nodes;
      // the traverser's own nodes never receive a kSimple sum in this sweep.
      for (std::size_t a = 0; a < observed.regrets.size(); ++a) {
        CHECK(near(observed.regrets[a], regret_it->second.regrets[a], tolerance));
        CHECK(observed.sums[a] == 0.0);
      }
    } else {
      // Opponent nodes receive the kSimple visit average but no regret. The
      // raw sums differ by a constant chance factor, so compare the summed-to-
      // one behavior vectors elementwise rather than the unnormalized rows.
      const std::vector<double> sampled_behavior = normalized(observed.sums);
      const std::vector<double> full_behavior = normalized(average_it->second.sums);
      for (std::size_t a = 0; a < observed.sums.size(); ++a) {
        CHECK(observed.regrets[a] == 0.0);
        CHECK(near(sampled_behavior[a], full_behavior[a], tolerance));
      }
    }
  }
  return 0;
}

// Aggregates every scripted seeded episode for one traverser, sampling
// opponent outcomes from the prescribed per-node policy. Returns the exact
// expected raw table and reports the complete draw-vector count.
VectorTable enumerate_expected_seeded_updates(const HeadsUpGame& game, std::size_t traverser,
                                              const PrescribedPolicy& prescribed,
                                              std::size_t& vector_count) {
  const auto deals = enumerate_deals(game);
  double cumulative = 0;
  double total_mass = 0;
  VectorTable expected;
  vector_count = 0;
  for (const JointDeal& deal : deals) {
    const double low = cumulative;
    cumulative += deal.probability;
    const double midpoint = (low + cumulative) * 0.5;
    auto suffixes =
        enumerate_suffixes_seeded(game, HeadsUpState(game.root), deal.holes, traverser, prescribed);
    vector_count += suffixes.size();
    for (const DrawVector& suffix : suffixes) {
      const double probability = deal.probability * suffix.probability;
      total_mass += probability;
      DrawVector full;
      full.draws.push_back(unit_draw_for(midpoint));
      full.draws.insert(full.draws.end(), suffix.draws.begin(), suffix.draws.end());
      const auto observed = HeadsUpSolverDebug::run_episode_scripted_seeded(
          game, full.draws, traverser, prescribed, large_budget());
      if (observed.entropy_consumed != full.draws.size())
        throw std::runtime_error("seeded scripted entropy mismatch");
      for (const auto& [key, row] : observed.rows)
        add_weighted(expected, key, row, probability);
    }
  }
  if (!near(total_mass, 1, 1e-9))
    throw std::runtime_error("seeded enumerated probability mass mismatch");
  return expected;
}

// Fixed-policy enumeration with exact zero-policy actions (RFC 0004 lines
// 317-321). One deal, fixed runout, one chip behind. The prescribed policy
// pins: player 0 root check = 1, bet = 0 (a TRAVERSER zero action, still
// enumerated with its raw regret update and zero weight in the node value),
// and player 1 after the root check check = 0, bet = 1 (an OPPONENT zero
// branch that must never be drawn).
int test_seeded_zero_policy() {
  HeadsUpGame game;
  game.root = {{card("2c"), card("3d"), card("7h")}, {1, 1}, {1, 1}, 2, 1, 1};
  game.ranges[0] = {{{card("Ac"), card("Ad")}, 1}};
  game.ranges[1] = {{{card("Qh"), card("Th")}, 1}};
  game.fixed_runout = {card("Js"), card("9c")};
  const Hand h0 = game.ranges[0][0].cards;
  const Hand h1 = game.ranges[1][0].cards;

  const HeadsUpState root(game.root);
  const auto after_check = root.after_action(0, {ActionType::Check});
  const auto ip_bets = after_check.after_action(1, {ActionType::Bet, 1});
  const auto oop_bets = root.after_action(0, {ActionType::Bet, 1});
  // States that exist only below the zero-probability opponent check branch.
  const auto turn_checked = after_check.after_action(1, {ActionType::Check}).after_card(card("Js"));
  const auto turn_ip_lead = turn_checked.after_action(0, {ActionType::Check});

  PrescribedPolicy prescribed;
  prescribed.emplace(information_key(root, h0), std::vector<double>{1.0, 0.0});
  prescribed.emplace(information_key(after_check, h1), std::vector<double>{0.0, 1.0});

  // Opponent zero-branch proof: the check branch exists in the game tree but
  // the seeded enumerator gives it zero draw-space width. From the IP node
  // exactly one vector remains (bet only), drawn at the sole CDF bin
  // midpoint 0.5 and carrying total probability one; the uniform enumerator
  // additionally enters the check branch.
  const Holes holes = {h0, h1};
  const auto seeded_ip = enumerate_suffixes_seeded(game, after_check, holes, 0, prescribed);
  const auto uniform_ip = enumerate_suffixes(game, after_check, holes, 0);
  CHECK(seeded_ip.size() == 1);
  CHECK(seeded_ip[0].probability == 1.0);
  // The opponent draw (bet bin) followed by the two fixed runout cards dealt
  // after the forced all-in call; the zero check branch contributes nothing.
  CHECK(seeded_ip[0].draws ==
        (std::vector<std::uint64_t>{unit_draw_for(0.5), card_draw_for(0, 1), card_draw_for(0, 1)}));
  CHECK(uniform_ip.size() > seeded_ip.size());

  // Exact enumerated expectation against identically seeded full sweeps.
  const auto sweep0 =
      HeadsUpSolverDebug::train_full_sweep_seeded(game, 0, prescribed, large_budget());
  const auto sweep1 =
      HeadsUpSolverDebug::train_full_sweep_seeded(game, 1, prescribed, large_budget());
  CHECK(sweep0.result.status == TrainingStatus::Complete);
  CHECK(sweep1.result.status == TrainingStatus::Complete);
  std::size_t vector_count = 0;
  const VectorTable expected = enumerate_expected_seeded_updates(game, 0, prescribed, vector_count);
  // Two complete draw vectors cover every enumerated traverser action under
  // the forced opponent bet; the total draw-space mass is asserted as one in
  // the enumerator, and every scripted run consumes its draws exactly.
  CHECK(vector_count == 2);
  for (const auto& [key, observed] : expected) {
    const auto regret_it = sweep0.rows.find(key);
    const auto average_it = sweep1.rows.find(key);
    CHECK(regret_it != sweep0.rows.end() && average_it != sweep1.rows.end());
    if (key.front() == 0) {
      for (std::size_t a = 0; a < observed.regrets.size(); ++a) {
        CHECK(near(observed.regrets[a], regret_it->second.regrets[a], 1e-9));
        CHECK(observed.sums[a] == 0.0);
      }
    } else {
      const std::vector<double> sampled_behavior = normalized(observed.sums);
      const std::vector<double> full_behavior = normalized(average_it->second.sums);
      for (std::size_t a = 0; a < observed.sums.size(); ++a)
        CHECK(near(sampled_behavior[a], full_behavior[a], 1e-9));
    }
  }

  // Hand-derived raw UPDATE literals (the seeded starting regrets are added
  // to the table before the walk, so subtract them to expose the raw delta).
  // Folding loses the single root chip (-1); calling and winning the all-in is
  // +2; winning when IP folds is +1.
  // Root: v(check) = 0.5*(-1) + 0.5*(+2) = 0.5,
  //       v(bet)   = 0.5*(+1) + 0.5*(+2) = 1.5,
  // v = 1*0.5 + 0*1.5 = 0.5, so the raw regret update is exactly {0, +1}.
  const auto& root_seed = prescribed.at(information_key(root, h0));
  const auto& root_row = expected.at(information_key(root, h0));
  CHECK(near(root_row.regrets[0] - root_seed[0], 0.0, 1e-9));
  CHECK(near(root_row.regrets[1] - root_seed[1], 1.0, 1e-9));
  // Facing the forced IP bet this node starts uniform: raw regrets
  // {-1 - 0.5, +2 - 0.5} = {-1.5, +1.5}.
  const auto& facing_row = expected.at(information_key(ip_bets, h0));
  CHECK(near(facing_row.regrets[0], -1.5, 1e-9));
  CHECK(near(facing_row.regrets[1], 1.5, 1e-9));
  // Opponent kSimple sums: IP after the root check always bets {0, 1}; IP
  // facing a root bet (the enumerated zero-sigma branch) stays {0.5, 0.5}.
  const auto& ip_lead_row = expected.at(information_key(after_check, h1));
  CHECK(near(ip_lead_row.sums[0], 0.0, 1e-9));
  CHECK(near(ip_lead_row.sums[1], 1.0, 1e-9));
  const auto& ip_face_row = expected.at(information_key(oop_bets, h1));
  CHECK(near(ip_face_row.sums[0], 0.5, 1e-9));
  CHECK(near(ip_face_row.sums[1], 0.5, 1e-9));

  // No zero-branch information set appears anywhere in the expectation.
  CHECK(expected.find(information_key(turn_checked, h0)) == expected.end());
  CHECK(expected.find(information_key(turn_ip_lead, h1)) == expected.end());
  return 0;
}

// Hand-derived literals for the one-combo nuts (AcAd) vs air (QhTh) fixed
// runout game with one chip behind. Folding the flop/turn/river bet loses the
// single root chip (-1); calling an all-in bet and winning is +2; calling and
// losing is -2. These values come from chip accounting and hand evaluation,
// not from the trainer under test.
int test_hand_derived_and_enumerated_fixed() {
  HeadsUpGame game;
  game.root = {{card("2c"), card("3d"), card("7h")}, {1, 1}, {1, 1}, 2, 1, 1};
  game.ranges[0] = {{{card("Ac"), card("Ad")}, 1}};
  game.ranges[1] = {{{card("Qh"), card("Th")}, 1}};
  game.fixed_runout = {card("Js"), card("9c")};
  const Hand h0 = game.ranges[0][0].cards;
  const Hand h1 = game.ranges[1][0].cards;

  const HeadsUpState root(game.root);
  const auto turn = root.after_action(0, {ActionType::Check})
                        .after_action(1, {ActionType::Check})
                        .after_card(card("Js"));
  const auto river = turn.after_action(0, {ActionType::Check})
                         .after_action(1, {ActionType::Check})
                         .after_card(card("9c"));
  auto facing = [](const HeadsUpState& street) {
    return street.after_action(0, {ActionType::Check}).after_action(1, {ActionType::Bet, 1});
  };

  const auto sweep0 = HeadsUpSolverDebug::train_full_sweep(game, 0, large_budget());
  const auto sweep1 = HeadsUpSolverDebug::train_full_sweep(game, 1, large_budget());
  CHECK(sweep0.result.status == TrainingStatus::Complete);
  CHECK(sweep1.result.status == TrainingStatus::Complete);

  // Traverser 0 (OOP, winning) regrets while facing an IP all-in bet:
  // call beats fold by three chips; the full sweep weights by opponent reach.
  const double d_weight = 1.5;  // 0.5*(call +2 - fold -1)
  CHECK((near(sweep0.rows.at(information_key(facing(root), h0)).regrets[0], -0.5 * d_weight)));
  CHECK((near(sweep0.rows.at(information_key(facing(root), h0)).regrets[1], 0.5 * d_weight)));
  CHECK((near(sweep0.rows.at(information_key(facing(turn), h0)).regrets[0], -0.25 * d_weight)));
  CHECK((near(sweep0.rows.at(information_key(facing(turn), h0)).regrets[1], 0.25 * d_weight)));
  CHECK((near(sweep0.rows.at(information_key(facing(river), h0)).regrets[0], -0.125 * d_weight)));
  CHECK((near(sweep0.rows.at(information_key(facing(river), h0)).regrets[1], 0.125 * d_weight)));

  // Traverser 0 full average sums at OOP nodes: own reach 1, 1/2, 1/4 at the
  // root/turn/river check lines times the uniform policy mass of one half.
  const std::array<std::pair<HeadsUpState, double>, 3> own_average = {{
      {root, 0.5},
      {turn, 0.25},
      {river, 0.125},
  }};
  for (const auto& [state, expected] : own_average) {
    const auto& sums = sweep0.rows.at(information_key(state, h0)).sums;
    CHECK(near(sums[0], expected));
    CHECK(near(sums[1], expected));
  }

  // Traverser 1 (IP, losing) regrets while facing an OOP all-in bet: folding
  // (-1) beats calling and losing (-2) by one chip.
  const double c_weight = 0.5;  // 0.5*(fold -1 - call -2 magnitude)
  const auto oop_bet_root = root.after_action(0, {ActionType::Bet, 1});
  const auto oop_bet_turn = turn.after_action(0, {ActionType::Bet, 1});
  const auto oop_bet_river = river.after_action(0, {ActionType::Bet, 1});
  CHECK((near(sweep1.rows.at(information_key(oop_bet_root, h1)).regrets[0], 0.5 * c_weight)));
  CHECK((near(sweep1.rows.at(information_key(oop_bet_root, h1)).regrets[1], -0.5 * c_weight)));
  CHECK((near(sweep1.rows.at(information_key(oop_bet_turn, h1)).regrets[0], 0.25 * c_weight)));
  CHECK((near(sweep1.rows.at(information_key(oop_bet_turn, h1)).regrets[1], -0.25 * c_weight)));
  CHECK((near(sweep1.rows.at(information_key(oop_bet_river, h1)).regrets[0], 0.125 * c_weight)));
  CHECK((near(sweep1.rows.at(information_key(oop_bet_river, h1)).regrets[1], -0.125 * c_weight)));
  // IP river node after OOP checks: bet wins one on a fold but loses two on a
  // call, so its uniform-policy regret is +/- 0.25 weighted by OOP reach 1/8.
  const auto ip_river = river.after_action(0, {ActionType::Check});
  CHECK((near(sweep1.rows.at(information_key(ip_river, h1)).regrets[0], -0.03125)));
  CHECK((near(sweep1.rows.at(information_key(ip_river, h1)).regrets[1], 0.03125)));

  // Full-traversal averages accumulate only at the traverser's OWN nodes, so
  // OOP (h0) sums are carried solely by sweep0 and IP (h1) sums solely by
  // sweep1. kSimple sampled traverser-t episodes instead carry the opponent
  // sums; the enumerated identity below equates the two conventions.
  const double oop_face_reach[3] = {0.25, 0.125, 0.0625};
  CHECK((near(sweep0.rows.at(information_key(facing(root), h0)).sums[0], oop_face_reach[0])));
  CHECK((near(sweep0.rows.at(information_key(facing(root), h0)).sums[1], oop_face_reach[0])));
  CHECK((near(sweep0.rows.at(information_key(facing(turn), h0)).sums[0], oop_face_reach[1])));
  CHECK((near(sweep0.rows.at(information_key(facing(turn), h0)).sums[1], oop_face_reach[1])));
  CHECK((near(sweep0.rows.at(information_key(facing(river), h0)).sums[0], oop_face_reach[2])));
  CHECK((near(sweep0.rows.at(information_key(facing(river), h0)).sums[1], oop_face_reach[2])));

  // IP full average sums in the traverser-1 sweep: 0.5, 0.25, 0.125 along the
  // check-down line for both the lead and the face-bet decision families.
  const auto ip_after_check_root = root.after_action(0, {ActionType::Check});
  const auto ip_after_check_turn = turn.after_action(0, {ActionType::Check});
  const auto ip_after_check_river = river.after_action(0, {ActionType::Check});
  const HeadsUpState ip_lead[3] = {ip_after_check_root, ip_after_check_turn, ip_after_check_river};
  const HeadsUpState ip_face[3] = {oop_bet_root, oop_bet_turn, oop_bet_river};
  const double ip_average[3] = {0.5, 0.25, 0.125};
  for (std::size_t i = 0; i < 3; ++i) {
    CHECK(near(sweep1.rows.at(information_key(ip_lead[i], h1)).sums[0], ip_average[i]));
    CHECK(near(sweep1.rows.at(information_key(ip_lead[i], h1)).sums[1], ip_average[i]));
    CHECK(near(sweep1.rows.at(information_key(ip_face[i], h1)).sums[0], ip_average[i]));
    CHECK(near(sweep1.rows.at(information_key(ip_face[i], h1)).sums[1], ip_average[i]));
  }
  // The opposite sweep never contributes a node's own average.
  CHECK(near(sweep1.rows.at(information_key(root, h0)).sums[0], 0));
  CHECK(near(sweep0.rows.at(information_key(ip_lead[0], h1)).sums[0], 0));

  // Exact enumerated sampled expectation for both traversers (22 and 15
  // complete draw vectors). Sampled regrets match the traverser full sweep
  // as raw values; sampled kSimple behavior matches the opponent full sweep
  // after row normalization (the pinned fixed game has a single deal and no
  // chance factor, so the raw sums coincide here as well).
  CHECK(expected_iteration_updates(game, 0, sweep0.rows, sweep1.rows, 1e-12, 100000) == 0);
  CHECK(expected_iteration_updates(game, 1, sweep1.rows, sweep0.rows, 1e-12, 100000) == 0);
  return 0;
}

// The reviewed accumulation concern under NON-UNIFORM range weights and a
// NON-FIXED public card. Two distinct conventions are pinned by RFC 0004 and
// both are enumerated here:
//
//  * The full traversal averages ONCE PER INFORMATION SET by own reach alone,
//    so its raw sums are independent of range weights and chance mass: 0.5 at
//    the root and 0.25 on the check-checked turn line, per action.
//  * The sampled kSimple sum is a per-visit raw add. Its expectation over the
//    weighted joint deals and the free turn card equals the MARGINALIZED
//    numerators 0.14/0.36 at the root and 7/4400, 9/2200, 21/8800 at specific
//    turn cards (enumerated, not converged). These rows agree with the full
//    traversal only after behavior normalization.
int test_weighted_free_chance_averages() {
  HeadsUpGame game;
  game.root = {{card("2c"), card("3d"), card("7h")}, {1, 1}, {1, 1}, 2, 1, 1};
  game.ranges[0] = {{{card("Ac"), card("Ad")}, 2}, {{card("8c"), card("8d")}, 3}};
  game.ranges[1] = {{{card("Ac"), card("As")}, 5}, {{card("Qc"), card("Qd")}, 7}};
  game.fixed_runout = {std::nullopt, card("9c")};

  const HeadsUpState root(game.root);
  const auto checked =
      root.after_action(0, {ActionType::Check}).after_action(1, {ActionType::Check});
  const auto turn_ks = checked.after_card(card("Ks"));
  const auto turn_ac = checked.after_card(card("Ac"));
  const Hand aces = game.ranges[0][0].cards;
  const Hand eights = game.ranges[0][1].cards;

  const auto full = HeadsUpSolverDebug::train_full(game, 1, large_budget());
  CHECK(full.result.status == TrainingStatus::Complete);
  // Pinned full raw sums: one average per information set per sweep, weighted
  // by own reach and policy only, with no joint-deal or chance factor. Own
  // reach is one at the root and one quarter on the check-checked turn line
  // (own check probability one half times the uniform policy mass one half).
  auto pinned_sums = [&](const HeadsUpState& state, Hand own) {
    return full.rows.at(information_key(state, own)).sums;
  };
  for (const Hand own : {aces, eights}) {
    const auto& root_sums = pinned_sums(root, own);
    CHECK(near(root_sums[0], 0.5, 1e-12));
    CHECK(near(root_sums[1], 0.5, 1e-12));
  }
  CHECK(near(pinned_sums(turn_ks, aces)[0], 0.25, 1e-12));
  CHECK(near(pinned_sums(turn_ks, eights)[0], 0.25, 1e-12));
  CHECK(near(pinned_sums(turn_ac, eights)[0], 0.25, 1e-12));
  for (const Hand own : {aces, eights}) {
    const auto& sums = pinned_sums(turn_ks, own);
    CHECK(near(sums[0], sums[1], 1e-12));
  }

  // Enumerated sampled expectation for traverser 1: player-0 information sets
  // are opponent nodes of this sweep and receive the kSimple per-visit adds.
  // Aggregating every weighted deal and free-card episode yields the
  // marginalized numerators (the values the old full-sweep literals encoded).
  const VectorTable sampled = enumerate_expected_updates(game, 1, 100000);
  auto sampled_sums = [&](const HeadsUpState& state, Hand own) -> const std::vector<double>& {
    return sampled.at(information_key(state, own)).sums;
  };
  // Root: the AcAd compatible joint mass is 14/50 and the 8c8d mass is 36/50
  // (15/50 plus 21/50); the uniform policy contributes one half.
  CHECK(near(sampled_sums(root, aces)[0], 0.14, 1e-9));
  CHECK(near(sampled_sums(root, eights)[0], 0.36, 1e-9));
  // Per-turn-card sums add the conditional 1/44 public-card probability and
  // exclude incompatible joint deals. Ks is compatible with all three OOP
  // deals; Ac is compatible only with the two deals not holding an ace.
  CHECK(near(sampled_sums(turn_ks, aces)[0], 7.0 / 4400.0, 1e-9));
  CHECK(near(sampled_sums(turn_ks, eights)[0], 9.0 / 2200.0, 1e-9));
  CHECK(near(sampled_sums(turn_ac, eights)[0], 21.0 / 8800.0, 1e-9));

  // Normalized sampled behavior equals the pinned full behavior at each key.
  for (const Hand own : {aces, eights}) {
    const std::vector<double> sampled_behavior = normalized(sampled_sums(root, own));
    const std::vector<double> full_behavior = normalized(pinned_sums(root, own));
    for (std::size_t a = 0; a < sampled_behavior.size(); ++a)
      CHECK(near(sampled_behavior[a], full_behavior[a], 1e-9));
  }

  // Exact enumerated sampled expectation for one weighted two-versus-one
  // free-turn variant stays tractable and matches the full sweeps exactly
  // (raw regrets; normalized averages), covering weighted joint deals, the
  // 44-card chance, repeated hidden histories, and kSimple averages together.
  HeadsUpGame weighted;
  weighted.root = game.root;
  weighted.fixed_runout = game.fixed_runout;
  weighted.ranges[0] = game.ranges[0];
  weighted.ranges[1] = {{{card("Qh"), card("Th")}, 4}};
  const auto w_sweep0 = HeadsUpSolverDebug::train_full_sweep(weighted, 0, large_budget());
  const auto w_sweep1 = HeadsUpSolverDebug::train_full_sweep(weighted, 1, large_budget());
  CHECK(w_sweep0.result.status == TrainingStatus::Complete);
  CHECK(w_sweep1.result.status == TrainingStatus::Complete);
  CHECK(expected_iteration_updates(weighted, 0, w_sweep0.rows, w_sweep1.rows, 1e-9, 100000) == 0);
  CHECK(expected_iteration_updates(weighted, 1, w_sweep1.rows, w_sweep0.rows, 1e-9, 100000) == 0);
  return 0;
}

// --- Multi-size (three-or-more-action) sampled coverage --------------------

// Every one-chip fixture clamps each fractional size onto the single all-in
// target, so the sampled traverser multi-action enumeration and the opponent
// weighted sampling over more than two outcomes never run. This fixture keeps
// three distinct abstract actions at the decision nodes; a future clamping or
// dedup regression shrinks the widths and fails the explicit assertions.
int test_multi_size_sampled_coverage() {
  const HeadsUpGame game = multi_size_fixture();
  const HeadsUpState root(game.root);
  const auto checked = root.after_action(0, {ActionType::Check});
  const auto facing_bet = checked.after_action(1, {ActionType::Bet, 1});

  const std::size_t root_width = abstract_actions(root, game.sizes).size();
  const std::size_t checked_width = abstract_actions(checked, game.sizes).size();
  const std::size_t facing_width = abstract_actions(facing_bet, game.sizes).size();
  std::printf("multi-size action widths: root=%zu checked=%zu facing=%zu\n", root_width,
              checked_width, facing_width);
  CHECK(root_width >= 3);
  CHECK(checked_width >= 3);
  CHECK(facing_width >= 3);
  CHECK(abstract_actions(root, game.sizes) ==
        (std::vector<Action>{{ActionType::Check}, {ActionType::Bet, 1}, {ActionType::Bet, 2}}));

  // Exact enumerated sampled-update identity for both traversers over all
  // four weighted joint deals (47,076 complete draw vectors total). Traverser
  // episodes enumerate three actions at their own nodes and opponent draws
  // weight three outcomes, so the previously uncovered sampled code paths run.
  const auto sweep0 = HeadsUpSolverDebug::train_full_sweep(game, 0, large_budget());
  const auto sweep1 = HeadsUpSolverDebug::train_full_sweep(game, 1, large_budget());
  CHECK(sweep0.result.status == TrainingStatus::Complete);
  CHECK(sweep1.result.status == TrainingStatus::Complete);
  CHECK(expected_iteration_updates(game, 0, sweep0.rows, sweep1.rows, 1e-9, 100000) == 0);
  CHECK(expected_iteration_updates(game, 1, sweep1.rows, sweep0.rows, 1e-9, 100000) == 0);

  // Hand-derived later-street facing lists and key multiplicity, so an
  // abstraction or perfect-recall key-merge regression cannot make the
  // identity pass silently. After X-B1-C one chip remains behind on the
  // turn, so lead nodes are {Check, Bet1} and facing nodes {Fold, Call}.
  const auto after_xbc = root.after_action(0, {ActionType::Check})
                             .after_action(1, {ActionType::Bet, 1})
                             .after_action(0, {ActionType::Call});
  const auto turn0 = after_xbc.after_card(card("9h"));
  const auto turn_face =
      turn0.after_action(0, {ActionType::Check}).after_action(1, {ActionType::Bet, 1});
  const auto river0 = turn0.after_action(0, {ActionType::Check})
                          .after_action(1, {ActionType::Check})
                          .after_card(card("8s"));
  const auto river_face =
      river0.after_action(0, {ActionType::Check}).after_action(1, {ActionType::Bet, 1});
  CHECK(abstract_actions(turn0, game.sizes) ==
        (std::vector<Action>{{ActionType::Check}, {ActionType::Bet, 1}}));
  CHECK(abstract_actions(turn_face, game.sizes) ==
        (std::vector<Action>{{ActionType::Fold}, {ActionType::Call}}));
  CHECK(abstract_actions(river0, game.sizes) ==
        (std::vector<Action>{{ActionType::Check}, {ActionType::Bet, 1}}));
  CHECK(abstract_actions(river_face, game.sizes) ==
        (std::vector<Action>{{ActionType::Fold}, {ActionType::Call}}));
  // Player-0 own keys per sweep: 16 on the turn (2 combos x 8 histories) and
  // 24 on the river (2 x 12). Four keys per street are flop-checked-through
  // lines where both chips remain behind and the lead node keeps three
  // abstract actions; X-B1-C lines have one chip behind and two actions.
  // Each constructed facing key is present.
  std::size_t turn_keys = 0, river_keys = 0, turn_wide = 0, river_wide = 0;
  for (const auto& [key, row] : sweep0.rows)
    if (key[0] == 0 && key[3] == 4) {
      ++turn_keys;
      turn_wide += row.actions.size() == 3;
    } else if (key[0] == 0 && key[3] == 5) {
      ++river_keys;
      river_wide += row.actions.size() == 3;
    }
  std::printf("multi-size player-0 keys: turn=%zu (wide %zu) river=%zu (wide %zu)\n", turn_keys,
              turn_wide, river_keys, river_wide);
  CHECK(turn_keys == 16 && turn_wide == 4);
  CHECK(river_keys == 24 && river_wide == 4);
  for (const Hand own : {game.ranges[0][0].cards, game.ranges[0][1].cards}) {
    CHECK(sweep0.rows.contains(information_key(turn_face, own)));
    CHECK(sweep0.rows.contains(information_key(river_face, own)));
  }

  // Real SplitMix64 convergence smoke across all three pinned seeds: loose
  // gate with margin and monotone improvement over a checkpoint, evaluated
  // exactly. Also record the widest published behavior vector so a clamp
  // regression cannot silently collapse the abstraction back to two actions.
  const HeadsUpTrainer trainer(game);
  for (std::uint64_t seed : {1ULL, 17ULL, 43ULL}) {
    const auto checkpoint = trainer.train_sampled(5000, seed, large_budget());
    CHECK(checkpoint.status == TrainingStatus::Complete);
    const double early = trainer.evaluate(checkpoint.policy, large_budget()).exploitability_pot;
    const auto trained = trainer.train_sampled(30000, seed, large_budget());
    CHECK(trained.status == TrainingStatus::Complete);
    const double final = trainer.evaluate(trained.policy, large_budget()).exploitability_pot;
    std::size_t widest = 0;
    for (const auto& [key, row] : trained.policy.rows())
      widest = std::max(widest, row.actions.size());
    std::printf("multi-size seed %llu: 5k=%.6f 30k=%.6f widest_policy_row=%zu\n",
                static_cast<unsigned long long>(seed), early, final, widest);
    CHECK(final < early);
    CHECK(final <= 0.025);
    CHECK(widest >= 3);
  }
  return 0;
}

// --- Repeatability and seed behavior ---------------------------------------

bool policies_differ(const HeadsUpPolicy& a, const HeadsUpPolicy& b) {
  if (a.rows().size() != b.rows().size())
    return true;
  auto j = b.rows().begin();
  for (auto i = a.rows().begin(); i != a.rows().end(); ++i, ++j)
    if (i->first != j->first || i->second.probabilities != j->second.probabilities)
      return true;
  return false;
}

bool policies_equal(const HeadsUpPolicy& a, const HeadsUpPolicy& b) {
  return !policies_differ(a, b);
}

int test_repeatability_and_seeds() {
  const auto game = fixed_fixture();
  const HeadsUpTrainer trainer(game);
  const std::uint64_t iterations = 20000;
  const auto a = trainer.train_sampled(iterations, 17, large_budget());
  const auto b = trainer.train_sampled(iterations, 17, large_budget());
  CHECK(a.status == TrainingStatus::Complete);
  CHECK(b.status == TrainingStatus::Complete);
  CHECK(a.nodes == b.nodes);
  CHECK(a.prng_state == b.prng_state);
  CHECK(policies_equal(a.policy, b.policy));
  // Same-build repeats match within 1e-12; on this deterministic path the raw
  // published probabilities compare equal.
  const auto c = trainer.train_sampled(iterations, 43, large_budget());
  CHECK(c.status == TrainingStatus::Complete);
  CHECK(c.prng_state != a.prng_state);
  CHECK(policies_differ(a.policy, c.policy));
  return 0;
}

// --- Resource interruption, rollback, and PRNG restore ---------------------

int test_resource_rollback() {
  const auto game = fixed_fixture();
  const HeadsUpTrainer trainer(game);
  const auto one = trainer.train_sampled(1, 99, large_budget());
  const auto two = trainer.train_sampled(2, 99, large_budget());
  CHECK(one.status == TrainingStatus::Complete);
  CHECK(two.status == TrainingStatus::Complete);
  CHECK(one.prng_state != two.prng_state);

  // A cap that allows the first iteration but stops inside the second rolls
  // the table and PRNG back to the one-iteration committed state.
  for (std::size_t cap : {one.nodes, one.nodes + 1, two.nodes - 1}) {
    auto limits = large_budget();
    limits.max_nodes = cap;
    const auto partial = trainer.train_sampled(3, 99, limits);
    CHECK(partial.status == TrainingStatus::ResourceLimit);
    CHECK(partial.completed_iterations == 1);
    CHECK(partial.nodes == cap);
    CHECK(partial.prng_state == one.prng_state);
    CHECK(policies_equal(partial.policy, one.policy));
  }
  // Caps that fail before any iteration commits publish nothing.
  for (int resource = 0; resource < 3; ++resource) {
    auto limits = large_budget();
    if (resource == 0)
      limits.max_nodes = one.nodes - 1;
    else if (resource == 1)
      limits.max_information_sets = 1;
    else
      limits.max_bytes = 1;
    const auto failed = trainer.train_sampled(3, 99, limits);
    CHECK(failed.status == TrainingStatus::ResourceLimit);
    CHECK(failed.completed_iterations == 0);
    CHECK(failed.information_sets == 0);
    CHECK(failed.policy.rows().empty());
    CHECK(failed.prng_state == 99);
  }
  auto depth = large_budget();
  depth.max_depth = 1;
  const auto shallow = trainer.train_sampled(2, 99, depth);
  CHECK(shallow.status == TrainingStatus::ResourceLimit);
  CHECK(shallow.completed_iterations == 0);
  CHECK(shallow.policy.rows().empty());
  CHECK(shallow.prng_state == 99);

  // Deadline interruption: a one-millisecond budget stops a long run at some
  // committed iteration N. Whatever N is, the interrupted result must be
  // identical to a clean N-iteration run (same policy and PRNG state), which
  // is the interruption-and-resume transactional guarantee.
  auto deadline = large_budget();
  deadline.time = std::chrono::milliseconds{1};
  const auto timed = trainer.train_sampled(100000000, 7, deadline);
  CHECK(timed.status == TrainingStatus::ResourceLimit);
  CHECK(timed.completed_iterations < 100000000);
  const auto clean = trainer.train_sampled(timed.completed_iterations, 7, large_budget());
  CHECK(clean.status == TrainingStatus::Complete);
  CHECK(clean.prng_state == timed.prng_state);
  CHECK(policies_equal(clean.policy, timed.policy));
  return 0;
}

// --- Convergence quality gates ---------------------------------------------

// Fast, bounded convergence smoke checks for the default CTest suite. The
// pinned release gates are normalized NashConv at most 0.002 (fixed small
// game) and 0.02 (sampled small game) measured within the one-million
// compute budget in the version-2 heads-up blueprint benchmarks: the default
// CTest benchmark spends the free-river case at a bounded 100,000-iteration
// checkpoint (which already passes the 0.02 gate inside that budget), while
// the manual capacity runner spends every sampled case at the full
// 1,000,000-iteration checkpoint. Here we assert monotone improvement over a
// checkpoint and a loose bound reached with margin, across all three pinned
// seeds, so a broken update rule fails quickly.
int test_quality_smoke() {
  const auto fixed = fixed_fixture();
  const HeadsUpTrainer fixed_trainer(fixed);
  for (std::uint64_t seed : {1ULL, 17ULL, 43ULL}) {
    const auto checkpoint = fixed_trainer.train_sampled(10000, seed, completion_budget());
    CHECK(checkpoint.status == TrainingStatus::Complete);
    const double early =
        fixed_trainer.evaluate(checkpoint.policy, completion_budget()).exploitability_pot;
    const auto trained = fixed_trainer.train_sampled(100000, seed, completion_budget());
    CHECK(trained.status == TrainingStatus::Complete);
    const double final =
        fixed_trainer.evaluate(trained.policy, completion_budget()).exploitability_pot;
    CHECK(final < early);
    CHECK(final <= 0.006);
  }

  const auto chance = free_river_fixture();
  const HeadsUpTrainer chance_trainer(chance);
  for (std::uint64_t seed : {1ULL, 17ULL, 43ULL}) {
    const auto checkpoint = chance_trainer.train_sampled(10000, seed, completion_budget());
    CHECK(checkpoint.status == TrainingStatus::Complete);
    const double early =
        chance_trainer.evaluate(checkpoint.policy, completion_budget()).exploitability_pot;
    const auto trained = chance_trainer.train_sampled(50000, seed, completion_budget());
    CHECK(trained.status == TrainingStatus::Complete);
    CHECK(trained.policy.rows().size() == checkpoint.policy.rows().size());
    const double final =
        chance_trainer.evaluate(trained.policy, completion_budget()).exploitability_pot;
    CHECK(final < early);
    CHECK(final <= 0.02);
  }
  return 0;
}

// --- Independent best response over a non-fixed chance card (P2 finding) ----
//
// Backward-induction best response written entirely against the public poker
// transition and settlement API. It groups compatible hidden joint deals by
// the responder's own observation, averages uniform public chance, and picks
// one action per observation. It never calls the modeled response() recursion
// or information_key(). It counter-checks response()'s argmax and value where
// chance nodes sit above later responder decisions.
class GreedyChanceOracle {
 public:
  using Reach = std::vector<double>;

  GreedyChanceOracle(const HeadsUpGame& game, const HeadsUpPolicy& policy)
      : game_(game), policy_(policy), deals_(enumerate_deals(game)) {}

  double best_response_value(std::size_t player) {
    double total = 0;
    for (const auto& hand : game_.ranges[player])
      total += walk(HeadsUpState(game_.root), player, hand.cards, root_reach(player, hand.cards));
    return total;
  }

  // Per-action responder values at an arbitrary descendant state with an
  // explicit reach vector.
  std::vector<double> action_values_at(const HeadsUpState& state, std::size_t player, Hand own,
                                       const Reach& reach) {
    const auto actions = abstract_actions(state, game_.sizes);
    std::vector<double> values;
    values.reserve(actions.size());
    for (const Action& action : actions)
      values.push_back(walk(state.after_action(*state.actor(), action), player, own, reach));
    return values;
  }

  Reach root_reach(std::size_t player, Hand own) {
    Reach reach(deals_.size(), 0.0);
    for (std::size_t d = 0; d < deals_.size(); ++d)
      if (deals_[d].holes[player] == own)
        reach[d] = deals_[d].probability;
    return reach;
  }

 private:
  std::string observation(const HeadsUpState& state, Hand own) const {
    Hand sorted = own;
    std::sort(sorted.begin(), sorted.end());
    std::string label = std::to_string(*state.actor()) + ":" + std::to_string(sorted[0]) + "," +
                        std::to_string(sorted[1]) + ";";
    for (int c : state.board())
      label += std::to_string(c) + ",";
    label += ";";
    for (const auto& event : state.history()) {
      label += std::to_string(static_cast<int>(event.street)) + ":" + std::to_string(event.actor) +
               ":" + std::to_string(static_cast<int>(event.action.type)) + ":" +
               std::to_string(event.action.target_total) + ";";
    }
    return label;
  }

  double terminal(const HeadsUpState& state, std::size_t player, const Reach& reach) const {
    double value = 0;
    for (std::size_t d = 0; d < deals_.size(); ++d) {
      if (reach[d] == 0)
        continue;
      const auto settlement = state.phase() == Phase::Folded
                                  ? state.settle_fold()
                                  : state.settle_showdown(deals_[d].holes);
      value += reach[d] * static_cast<double>(settlement.net_utility[player]);
    }
    return value;
  }

  double walk(const HeadsUpState& state, std::size_t player, Hand own, const Reach& reach) {
    if (std::accumulate(reach.begin(), reach.end(), 0.0) == 0)
      return 0;
    if (state.phase() == Phase::Folded || state.phase() == Phase::Showdown)
      return terminal(state, player, reach);
    if (state.phase() == Phase::Deal) {
      std::array<Reach, 52> card_reach{};
      for (std::size_t d = 0; d < deals_.size(); ++d) {
        if (reach[d] == 0)
          continue;
        const auto cards = enumerate_cards(game_, state, deals_[d].holes);
        for (int c : cards) {
          if (card_reach[c].empty())
            card_reach[c].assign(deals_.size(), 0.0);
          card_reach[c][d] = reach[d] / static_cast<double>(cards.size());
        }
      }
      double value = 0;
      for (int c = 0; c < 52; ++c)
        if (!card_reach[c].empty())
          value += walk(state.after_card(c), player, own, card_reach[c]);
      return value;
    }
    const std::size_t actor = *state.actor();
    const auto actions = abstract_actions(state, game_.sizes);
    std::vector<double> values(actions.size(), 0.0);
    for (std::size_t a = 0; a < actions.size(); ++a) {
      Reach child = reach;
      if (actor != player) {
        for (std::size_t d = 0; d < deals_.size(); ++d) {
          if (child[d] == 0)
            continue;
          const auto* row = policy_.lookup(state, deals_[d].holes[actor]);
          if (!row || !same_actions(row->actions, actions))
            throw std::runtime_error("greedy oracle missing policy row");
          child[d] *= row->probabilities[a];
        }
      }
      values[a] = walk(state.after_action(actor, actions[a]), player, own, child);
    }
    if (actor != player)
      return std::accumulate(values.begin(), values.end(), 0.0);
    // The responder takes one action for this exact observation, chosen after
    // aggregating every compatible hidden deal and every public card.
    best_action_.emplace(
        observation(state, own),
        static_cast<std::size_t>(std::max_element(values.begin(), values.end()) - values.begin()));
    return *std::max_element(values.begin(), values.end());
  }

  const HeadsUpGame& game_;
  const HeadsUpPolicy& policy_;
  std::vector<JointDeal> deals_;
  std::map<std::string, std::size_t> best_action_;
};

int test_chance_conditioned_best_response() {
  const auto game = free_turn_fixture();
  const HeadsUpTrainer trainer(game);
  // Full traversal provides a complete policy over every free turn card.
  const auto trained = trainer.train(2000, large_budget());
  CHECK(trained.status == TrainingStatus::Complete);
  const auto evaluation = trainer.evaluate(trained.policy, large_budget());

  for (std::size_t player = 0; player < 2; ++player) {
    const Hand own = game.ranges[player][0].cards;
    const double oracle_value = [&] {
      GreedyChanceOracle fresh(game, trained.policy);
      return fresh.best_response_value(player);
    }();
    CHECK(near(oracle_value, evaluation.best_response_value[player], 1e-9));

    // Vector-valued counter-check at the responder's first decision node.
    // Player 0 acts first at the root; player 1 acts after player 0 checks, so
    // its reach is advanced through the root OOP check policy per hidden deal.
    HeadsUpState decision(game.root);
    GreedyChanceOracle fresh(game, trained.policy);
    auto reach = fresh.root_reach(player, own);
    if (player == 1) {
      const auto root_actions = abstract_actions(HeadsUpState(game.root), game.sizes);
      const auto* oop_row = trained.policy.lookup(HeadsUpState(game.root), game.ranges[0][0].cards);
      CHECK(oop_row && oop_row->actions == root_actions);
      const std::size_t check_index = static_cast<std::size_t>(
          std::find(root_actions.begin(), root_actions.end(), Action{ActionType::Check}) -
          root_actions.begin());
      for (std::size_t d = 0; d < reach.size(); ++d)
        reach[d] *= oop_row->probabilities[check_index];
      decision = HeadsUpState(game.root).after_action(0, {ActionType::Check});
    }
    CHECK(*decision.actor() == player);
    const auto values = fresh.action_values_at(decision, player, own, reach);
    const auto modeled = HeadsUpSolverDebug::response_value_with_reach(
        game, trained.policy, decision, player, own, true, reach, large_budget());
    const std::size_t oracle_argmax =
        static_cast<std::size_t>(std::max_element(values.begin(), values.end()) - values.begin());
    // The modeled response() argmax over aggregated chance and hidden reach
    // matches the independent backward-induction per-action values exactly.
    CHECK(near(modeled, values[oracle_argmax], 1e-9));
    for (std::size_t a = 0; a < values.size(); ++a)
      CHECK(values[a] <= values[oracle_argmax] + 1e-9);
    // The non-best profile mixture from this node must not exceed the BR.
    const double profile = HeadsUpSolverDebug::response_value_with_reach(
        game, trained.policy, decision, player, own, false, reach, large_budget());
    CHECK(profile <= modeled + 1e-9);
  }
  return 0;
}

}  // namespace

int main() {
  const char* names[] = {"prng",         "enumerated",    "zero_policy", "weighted_chance",
                         "multi_size",   "repeatability", "rollback",    "chance_br",
                         "quality_smoke"};
  int (*tests[])() = {test_prng_pinned,
                      test_hand_derived_and_enumerated_fixed,
                      test_seeded_zero_policy,
                      test_weighted_free_chance_averages,
                      test_multi_size_sampled_coverage,
                      test_repeatability_and_seeds,
                      test_resource_rollback,
                      test_chance_conditioned_best_response,
                      test_quality_smoke};
  for (std::size_t i = 0; i < std::size(tests); ++i) {
    try {
      if (tests[i]() != 0) {
        std::printf("FAILED: %s\n", names[i]);
        return 1;
      }
    } catch (const std::exception& error) {
      std::printf("FAILED: %s: %s\n", names[i], error.what());
      return 1;
    }
  }
  std::printf("test_heads_up_solver_sampled PASS\n");
  return 0;
}
