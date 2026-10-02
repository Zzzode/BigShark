// test_engine_client_response.cpp — RFC 0009 W4e response mapper tests.
//
// Pins the DecisionResponse -> poker::Action mapping: selected_action direct
// mapping, sampler parity when no selected_action, expanded_strategy path,
// EngineError fallback, missing target_total fallback, preflop verb mapping,
// illegal-action defense, and the check/call/fold fallback ordering.

#include <bs/prng.hpp>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include "engine_client/response_mapper.hpp"

namespace bs::engine_client {
namespace {

namespace pv = ::bigshark::engine::v1;

using bs::poker::Action;
using bs::poker::ActionType;
using bs::poker::LegalActions;
using bs::poker::TargetRange;

int failures = 0;

#define CHECK(cond)                                                        \
  do {                                                                     \
    if (!(cond)) {                                                         \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      ++failures;                                                          \
    }                                                                      \
  } while (0)

// ---- LegalActions factories ------------------------------------------------

// Facing a bet: fold + call + raise (no check).
LegalActions facing_bet() {
  LegalActions legal;
  legal.fold = true;
  legal.call = true;
  legal.call_amount = 100;
  legal.aggressive = TargetRange{ActionType::Raise, 200, 1000, false};
  return legal;
}

// Big-blind option preflop: fold + check + bet (no call, to_call == 0).
LegalActions bb_option() {
  LegalActions legal;
  legal.fold = true;
  legal.check = true;
  legal.call_amount = 0;
  legal.aggressive = TargetRange{ActionType::Bet, 200, 1000, false};
  return legal;
}

// No aggression available: fold + check only.
LegalActions check_only() {
  LegalActions legal;
  legal.fold = true;
  legal.check = true;
  return legal;
}

// ---- Strategy builders -----------------------------------------------------

pv::DecisionResponse selected_response(pv::ActionType type, bool has_target = false,
                                       std::uint64_t target = 0) {
  pv::DecisionResponse response;
  pv::Strategy* strategy = response.mutable_strategy();
  pv::SelectedAction* sel = strategy->mutable_selected_action();
  sel->set_type(type);
  if (has_target)
    sel->set_target_total(target);
  strategy->mutable_solver()->set_source(pv::SOLVER_SOURCE_BLUEPRINT);
  return response;
}

pv::DecisionResponse sampled_response(std::vector<std::pair<pv::ActionType, double>> actions,
                                      std::uint64_t target = 0) {
  pv::DecisionResponse response;
  pv::Strategy* strategy = response.mutable_strategy();
  for (auto& [type, prob] : actions) {
    pv::ActionPolicy* policy = strategy->add_actions();
    policy->set_type(type);
    policy->set_probability(prob);
    if (type == pv::ACTION_TYPE_BET || type == pv::ACTION_TYPE_RAISE)
      policy->set_target_total(target);
  }
  strategy->mutable_solver()->set_source(pv::SOLVER_SOURCE_BLUEPRINT);
  return response;
}

// ---- selected_action direct mapping ----------------------------------------

void test_selected_fold() {
  auto result = map_decision_response(selected_response(pv::ACTION_TYPE_FOLD), facing_bet(), 42);
  CHECK(!result.fell_back);
  CHECK(result.action == (Action{ActionType::Fold}));
  CHECK(result.reason.empty());
}

void test_selected_check() {
  auto result = map_decision_response(selected_response(pv::ACTION_TYPE_CHECK), bb_option(), 42);
  CHECK(!result.fell_back);
  CHECK(result.action == (Action{ActionType::Check}));
}

void test_selected_call() {
  auto result = map_decision_response(selected_response(pv::ACTION_TYPE_CALL), facing_bet(), 42);
  CHECK(!result.fell_back);
  CHECK(result.action == (Action{ActionType::Call}));
}

void test_selected_bet_postflop() {
  // Postflop: engine returns BET, legal aggressive type is Raise.
  auto result =
      map_decision_response(selected_response(pv::ACTION_TYPE_BET, true, 300), facing_bet(), 42);
  CHECK(!result.fell_back);
  CHECK(result.action == (Action{ActionType::Raise, 300}));
}

void test_selected_raise_preflop() {
  // Preflop big-blind option: engine returns RAISE, but the state's aggressive
  // verb is Bet. The mapper must land on Bet.
  auto result =
      map_decision_response(selected_response(pv::ACTION_TYPE_RAISE, true, 250), bb_option(), 42);
  CHECK(!result.fell_back);
  CHECK(result.action == (Action{ActionType::Bet, 250}));
}

void test_selected_raise_facing_bet() {
  // Facing a bet: engine returns RAISE, legal aggressive type is Raise.
  auto result =
      map_decision_response(selected_response(pv::ACTION_TYPE_RAISE, true, 400), facing_bet(), 42);
  CHECK(!result.fell_back);
  CHECK(result.action == (Action{ActionType::Raise, 400}));
}

// ---- Aggressive without target_total -> fallback ----------------------------

void test_aggressive_missing_target_falls_back() {
  auto result =
      map_decision_response(selected_response(pv::ACTION_TYPE_BET, false, 0), facing_bet(), 42);
  CHECK(result.fell_back);
  CHECK(result.reason == "engine aggressive action missing target_total");
  // Fallback: check not legal, call legal -> call.
  CHECK(result.action == (Action{ActionType::Call}));
}

void test_aggressive_with_no_legal_aggressive_falls_back() {
  // Engine says BET but no aggressive interval is legal.
  auto result =
      map_decision_response(selected_response(pv::ACTION_TYPE_BET, true, 300), check_only(), 42);
  CHECK(result.fell_back);
  CHECK(result.reason == "engine aggressive action with no legal aggressive interval");
  // Fallback: check legal -> check.
  CHECK(result.action == (Action{ActionType::Check}));
}

// ---- Sampler parity --------------------------------------------------------

// Replicates the engine's sampleBucket to compute the expected bucket for a
// given seed and distribution. This pins the mapper's CDF walking and clamping
// to the same SplitMix64 protocol sampler the engine uses.
std::size_t expected_bucket(const std::vector<double>& probs, std::uint64_t seed) {
  double sum = 0.0;
  for (double p : probs)
    sum += p;
  bs::SplitMix64 rng = bs::SplitMix64::protocolSampler(seed);
  const std::uint64_t draw = rng.next_u64();
  const double unit = static_cast<double>(draw >> 11) * 0x1.0p-53;
  const double point = unit * sum;
  double prefix = 0.0;
  for (std::size_t i = 0; i < probs.size(); ++i) {
    prefix += probs[i];
    if (prefix > point)
      return i;
  }
  for (std::size_t i = probs.size(); i-- > 0;) {
    if (probs[i] > 0.0)
      return i;
  }
  return probs.size();
}

void test_sampler_parity_fold_call_raise() {
  const std::vector<double> probs = {0.3, 0.5, 0.2};
  auto response = sampled_response(
      {{pv::ACTION_TYPE_FOLD, 0.3}, {pv::ACTION_TYPE_CALL, 0.5}, {pv::ACTION_TYPE_RAISE, 0.2}},
      350);
  const std::size_t expected = expected_bucket(probs, 777);
  auto result = map_decision_response(response, facing_bet(), 777);
  CHECK(!result.fell_back);
  if (expected == 0)
    CHECK(result.action == (Action{ActionType::Fold}));
  else if (expected == 1)
    CHECK(result.action == (Action{ActionType::Call}));
  else
    CHECK(result.action == (Action{ActionType::Raise, 350}));
}

void test_sampler_parity_multiple_seeds() {
  // Pin the seed->bucket mapping across several seeds to catch any drift in
  // the CDF walking or clamping logic. All three actions must be legal in
  // facing_bet() (no CHECK when facing a bet).
  const std::vector<double> probs = {0.34, 0.33, 0.33};
  for (std::uint64_t seed : {1ULL, 42ULL, 999ULL, 123456789ULL}) {
    auto response = sampled_response(
        {{pv::ACTION_TYPE_FOLD, 0.34}, {pv::ACTION_TYPE_CALL, 0.33}, {pv::ACTION_TYPE_RAISE, 0.33}},
        500);
    const std::size_t expected = expected_bucket(probs, seed);
    auto result = map_decision_response(response, facing_bet(), seed);
    CHECK(!result.fell_back);
    if (expected == 0)
      CHECK(result.action == (Action{ActionType::Fold}));
    else if (expected == 1)
      CHECK(result.action == (Action{ActionType::Call}));
    else
      CHECK(result.action == (Action{ActionType::Raise, 500}));
  }
}

void test_sampler_clamps_to_last_positive() {
  // Distribution [0.0, 0.0, 1.0]: any draw lands in bucket 2 (clamp to last
  // positive bucket).
  auto response = sampled_response(
      {{pv::ACTION_TYPE_FOLD, 0.0}, {pv::ACTION_TYPE_CALL, 0.0}, {pv::ACTION_TYPE_RAISE, 1.0}},
      300);
  auto result = map_decision_response(response, facing_bet(), 55);
  CHECK(!result.fell_back);
  CHECK(result.action == (Action{ActionType::Raise, 300}));
}

void test_sampler_first_bucket_always_wins() {
  // Distribution [1.0, 0.0, 0.0]: any draw lands in bucket 0.
  auto response = sampled_response(
      {{pv::ACTION_TYPE_FOLD, 1.0}, {pv::ACTION_TYPE_CALL, 0.0}, {pv::ACTION_TYPE_RAISE, 0.0}},
      300);
  auto result = map_decision_response(response, facing_bet(), 55);
  CHECK(!result.fell_back);
  CHECK(result.action == (Action{ActionType::Fold}));
}

// Golden seed->bucket vectors computed by an independent Python SplitMix64
// implementation (not the codebase's bs::SplitMix64). If the protocol sampler
// drifts, these hard-coded expectations fail even when expected_bucket()
// shares the same bug.
void test_sampler_golden_vectors() {
  // Distribution: fold=0.3, call=0.5, raise=0.2 (sum=1.0).
  // Golden buckets: seed 1->1(call), 42->1(call), 777->1(call),
  //                 12345->2(raise), 0xDEADBEEF->1(call).
  struct Golden {
    std::uint64_t seed;
    ActionType type;
    std::uint64_t target;  // 0 for non-aggressive
  };
  const Golden goldens[] = {
      {1, ActionType::Call, 0},
      {42, ActionType::Call, 0},
      {777, ActionType::Call, 0},
      {12345, ActionType::Raise, 350},
      {0xDEADBEEFULL, ActionType::Call, 0},
  };
  for (const auto& g : goldens) {
    auto response = sampled_response(
        {{pv::ACTION_TYPE_FOLD, 0.3}, {pv::ACTION_TYPE_CALL, 0.5}, {pv::ACTION_TYPE_RAISE, 0.2}},
        350);
    auto result = map_decision_response(response, facing_bet(), g.seed);
    CHECK(!result.fell_back);
    if (g.target > 0)
      CHECK(result.action == (Action{g.type, g.target}));
    else
      CHECK(result.action == (Action{g.type}));
  }
}

// ---- expanded_strategy path (minor 1/2) ------------------------------------

void test_expanded_strategy_selected_action() {
  pv::DecisionResponse response;
  pv::ExpandedStrategy* expanded = response.mutable_expanded_strategy();
  pv::SelectedAction* sel = expanded->mutable_selected_action();
  sel->set_type(pv::ACTION_TYPE_CALL);
  expanded->mutable_solver()->set_source(pv::SOLVER_SOURCE_RESOLVING);
  expanded->mutable_solver()->set_guarantee_level("certified_bound");

  auto result = map_decision_response(response, facing_bet(), 42);
  CHECK(!result.fell_back);
  CHECK(result.action == (Action{ActionType::Call}));
  CHECK(result.guarantee_level == "certified_bound");
}

void test_expanded_strategy_sampled() {
  pv::DecisionResponse response;
  pv::ExpandedStrategy* expanded = response.mutable_expanded_strategy();
  pv::ActionPolicy* policy = expanded->add_actions();
  policy->set_type(pv::ACTION_TYPE_CHECK);
  policy->set_probability(1.0);
  expanded->mutable_solver()->set_source(pv::SOLVER_SOURCE_BLUEPRINT);

  auto result = map_decision_response(response, bb_option(), 42);
  CHECK(!result.fell_back);
  CHECK(result.action == (Action{ActionType::Check}));
}

// ---- EngineError -> fallback ------------------------------------------------

void test_engine_error_falls_back() {
  pv::DecisionResponse response;
  pv::EngineError* error = response.mutable_error();
  error->set_code(pv::ERROR_CODE_DEADLINE_EXCEEDED);
  error->set_message("solver timed out");

  auto result = map_decision_response(response, facing_bet(), 42);
  CHECK(result.fell_back);
  CHECK(result.reason == "engine error 6: solver timed out");
  // Fallback: check not legal, call legal -> call.
  CHECK(result.action == (Action{ActionType::Call}));
}

void test_engine_error_falls_back_to_check() {
  pv::DecisionResponse response;
  pv::EngineError* error = response.mutable_error();
  error->set_code(pv::ERROR_CODE_INTERNAL);
  error->set_message("boom");

  auto result = map_decision_response(response, bb_option(), 42);
  CHECK(result.fell_back);
  CHECK(result.action == (Action{ActionType::Check}));
}

// ---- No strategy or error -> fallback ---------------------------------------

void test_empty_response_falls_back() {
  pv::DecisionResponse response;  // no oneof set
  auto result = map_decision_response(response, facing_bet(), 42);
  CHECK(result.fell_back);
  CHECK(result.reason == "engine response has no strategy or error");
  CHECK(result.action == (Action{ActionType::Call}));
}

// ---- Illegal action defense -------------------------------------------------

void test_illegal_action_falls_back() {
  // Engine returns CHECK but check is not legal (facing a bet).
  auto result = map_decision_response(selected_response(pv::ACTION_TYPE_CHECK), facing_bet(), 42);
  CHECK(result.fell_back);
  CHECK(result.reason == "illegal engine action");
  CHECK(result.action == (Action{ActionType::Call}));
}

void test_illegal_aggressive_target_falls_back() {
  // Engine returns RAISE to 50 but the minimum is 200.
  auto result =
      map_decision_response(selected_response(pv::ACTION_TYPE_RAISE, true, 50), facing_bet(), 42);
  CHECK(result.fell_back);
  CHECK(result.reason == "illegal engine action");
  CHECK(result.action == (Action{ActionType::Call}));
}

// ---- Fallback ordering ------------------------------------------------------

void test_fallback_ordering_check_first() {
  // When check is legal, fallback is check (not call). Trigger the fallback
  // via an aggressive action missing target_total.
  auto result =
      map_decision_response(selected_response(pv::ACTION_TYPE_BET, false, 0), bb_option(), 42);
  CHECK(result.fell_back);
  CHECK(result.action == (Action{ActionType::Check}));
}

void test_fallback_ordering_call_when_no_check() {
  // When check is not legal but call is, fallback is call.
  LegalActions legal;
  legal.fold = true;
  legal.call = true;
  legal.call_amount = 50;
  auto result = map_decision_response(selected_response(pv::ACTION_TYPE_CHECK), legal, 42);
  CHECK(result.fell_back);
  CHECK(result.action == (Action{ActionType::Call}));
}

void test_fallback_ordering_fold_when_nothing_else() {
  // When neither check nor call is legal, fallback is fold.
  LegalActions legal;
  legal.fold = true;
  auto result = map_decision_response(selected_response(pv::ACTION_TYPE_CHECK), legal, 42);
  CHECK(result.fell_back);
  CHECK(result.action == (Action{ActionType::Fold}));
}

// ---- guarantee_level extraction ---------------------------------------------

void test_guarantee_level_extracted() {
  pv::DecisionResponse response;
  pv::Strategy* strategy = response.mutable_strategy();
  pv::SelectedAction* sel = strategy->mutable_selected_action();
  sel->set_type(pv::ACTION_TYPE_FOLD);
  strategy->mutable_solver()->set_source(pv::SOLVER_SOURCE_BLUEPRINT);
  strategy->mutable_solver()->set_guarantee_level("operational_fallback");

  auto result = map_decision_response(response, facing_bet(), 42);
  CHECK(!result.fell_back);
  CHECK(result.guarantee_level == "operational_fallback");
}

void test_guarantee_level_empty_when_absent() {
  auto result = map_decision_response(selected_response(pv::ACTION_TYPE_FOLD), facing_bet(), 42);
  CHECK(!result.fell_back);
  CHECK(result.guarantee_level.empty());
}

}  // namespace
}  // namespace bs::engine_client

int main() {
  using namespace bs::engine_client;
  test_selected_fold();
  test_selected_check();
  test_selected_call();
  test_selected_bet_postflop();
  test_selected_raise_preflop();
  test_selected_raise_facing_bet();
  test_aggressive_missing_target_falls_back();
  test_aggressive_with_no_legal_aggressive_falls_back();
  test_sampler_parity_fold_call_raise();
  test_sampler_parity_multiple_seeds();
  test_sampler_clamps_to_last_positive();
  test_sampler_first_bucket_always_wins();
  test_sampler_golden_vectors();
  test_expanded_strategy_selected_action();
  test_expanded_strategy_sampled();
  test_engine_error_falls_back();
  test_engine_error_falls_back_to_check();
  test_empty_response_falls_back();
  test_illegal_action_falls_back();
  test_illegal_aggressive_target_falls_back();
  test_fallback_ordering_check_first();
  test_fallback_ordering_call_when_no_check();
  test_fallback_ordering_fold_when_nothing_else();
  test_guarantee_level_extracted();
  test_guarantee_level_empty_when_absent();
  if (failures > 0) {
    std::fprintf(stderr, "%d check(s) failed\n", failures);
    return 1;
  }
  std::printf("all response mapper tests passed\n");
  return 0;
}
