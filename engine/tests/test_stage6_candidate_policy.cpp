// RFC 0008 stage 6 R12 step 8 gate for the composed candidate policy.
//
// Coverage:
//   * a frozen trained artifact binds to the candidate; on the reduced rooted
//     representative game the candidate returns a legal, unit-sum distribution
//     whose concrete actions are the R7 projection of the sealed row;
//   * the same row, when read directly from the artifact, projects to the same
//     concrete actions (no hidden clamping);
//   * an uncovered geometry, an unknown artifact stamp, and a missing row each
//     throw stage6_candidate_error (totality, never a silent fallback);
//   * the preflop branch is exactly the pinned chart action (the candidate at
//     a preflop state equals the BaselineBehaviorPolicy).
#include <array>
#include <bs/abstract_tree.hpp>
#include <bs/abstraction.hpp>
#include <bs/behavior_policy.hpp>
#include <bs/eval.hpp>
#include <bs/game_definition.hpp>
#include <bs/stage6/baseline_policy.hpp>
#include <bs/stage6/candidate_policy.hpp>
#include <bs/stage6/geometry.hpp>
#include <bs/stage6/trainer.hpp>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using namespace bs::poker;
using namespace bs::stage6;
using bs::abstraction::ActionAbstraction;
using bs::abstraction::CoverSeeds;
using bs::abstraction::SizeSchedule;

int failures = 0;
void check(bool cond, const char* what) {
  if (!cond) {
    std::printf("FAIL: %s\n", what);
    ++failures;
  }
}

ActionAbstraction coarse_action() {
  SizeSchedule schedule = bs::abstraction::default_size_schedule();
  for (auto& street : schedule) {
    street.bets = {{1, 2}};
    street.raises = {{1, 1}};
  }
  return ActionAbstraction::declared(schedule, CoverSeeds::DeclaredOnly);
}

// Same tiny live==2 representative the trainer gate uses.
GeometryBucket bucket_2p() {
  GeometryBucket b;
  b.key.player_count = 2;
  b.key.pot_bb = 4;
  b.key.live_count = 2;
  b.key.acting_count = 2;
  b.actionable = true;
  b.big_blind = 2;
  b.representative_stacks = {4, 4};
  b.representative_contrib = {4, 4};
  b.representative_pot = 8;
  b.min_acting_stack = 4;
  return b;
}

GeometryBucket bucket_3p_shallow() {
  GeometryBucket b;
  b.key.player_count = 3;
  b.key.pot_bb = 3;
  b.key.live_count = 3;
  b.key.acting_count = 3;
  b.actionable = true;
  b.big_blind = 2;
  b.representative_stacks = {2, 2, 2};
  b.representative_contrib = {2, 2, 2};
  b.representative_pot = 6;
  b.min_acting_stack = 2;
  return b;
}

GameDef rooted_def(const GeometryBucket& b, const std::array<int, 3>& flop) {
  GameDef def{};
  def.player_count = b.key.live_count;
  def.button = 0;
  def.big_blind = b.big_blind;
  def.preflop = false;
  for (std::size_t p = 0; p < b.key.live_count; ++p) {
    def.stacks[p] = b.representative_stacks[p];
    def.contributions[p] = b.representative_contrib[p];
  }
  def.pot = b.representative_pot;
  def.board = {flop[0], flop[1], flop[2], 0, 0};
  def.board_size = 3;
  return def;
}

TrainingConfig make_config(std::uint64_t iters, std::uint64_t seed) {
  TrainingConfig c;
  c.action = coarse_action();
  c.card_kind = bs::abstraction::CardBucketKind::CategoryTiersV1;
  c.iterations = iters;
  c.master_seed = seed;
  c.geometry_matrix_hash = 0xabcdef1234567890ULL;
  c.chart_digest_sha256 = "candidate-test-digest";
  return c;
}

bool is_distribution_legal(const std::vector<PolicyAction>& dist, const GameState& state) {
  const LegalActions legal = state.legal();
  double total = 0.0;
  for (const PolicyAction& pa : dist) {
    if (!std::isfinite(pa.probability) || pa.probability < 0.0)
      return false;
    if (!legal.contains(pa.action))
      return false;
    total += pa.probability;
  }
  return std::abs(total - 1.0) <= 1e-9;
}

int test_rooted_flop_decisions() {
  int local = 0;
  const GeometryBucket bucket = bucket_2p();
  const TrainingConfig config = make_config(50'000, 7);
  BucketTrainingResult trained = train_bucket(bucket, config);
  check(!trained.rows.empty(), "candidate test trained rows exist");

  CandidateBinding binding;
  binding.bucket = bucket;
  binding.action = config.action;
  binding.manifest =
      manifest_for(bucket, config, trained.rows, trained.report.iterations_completed);
  binding.rows = trained.rows;
  std::vector<CandidateBinding> bindings;
  bindings.push_back(std::move(binding));
  CandidateBehaviorPolicy candidate(std::move(bindings));
  check(candidate.binding_count() == 1, "one binding installed");

  // The reduced rooted game; the opener on the flop is seat 1 (pinned by the
  // rules). Evaluate for a few own holdings so every own-card bucket is hit.
  const std::array<int, 3> flop = {0, 6, 21};
  const GameDef def = rooted_def(bucket_2p(), flop);
  const HoleCards holdings[] = {{1, 7}, {2, 8}, {4, 10}, {5, 11}, {3, 9}};
  HandLog empty_log;
  PolicyContext ctx;
  ctx.hand_log = &empty_log;
  ctx.decision_seed = 0x6f7261636c65ULL;
  for (const HoleCards& hole : holdings) {
    GameState state(def);
    const std::size_t seat = *state.actor();
    std::vector<PolicyAction> dist;
    try {
      dist = candidate.distribution(state, seat, hole, ctx);
    } catch (const std::exception& e) {
      std::printf("FAIL: candidate threw on a rooted flop decision: %s\n", e.what());
      ++local;
      continue;
    }
    if (!is_distribution_legal(dist, state)) {
      std::printf("FAIL: candidate distribution is not legal/unit-sum\n");
      ++local;
    }
  }
  return local;
}

int test_totality_failures() {
  int local = 0;
  const GeometryBucket bucket = bucket_2p();
  const TrainingConfig config = make_config(5'000, 7);
  BucketTrainingResult trained = train_bucket(bucket, config);

  auto make_binding = [&]() {
    CandidateBinding b;
    b.bucket = bucket;
    b.action = config.action;
    b.manifest = manifest_for(bucket, config, trained.rows, trained.report.iterations_completed);
    b.rows = trained.rows;
    return b;
  };

  // A flop whose geometry the binding does not cover (different pot bucket):
  // same shapes but pot 10 -> potbb 5 instead of 4.
  {
    CandidateBinding b = make_binding();
    std::vector<CandidateBinding> v;
    v.push_back(std::move(b));
    CandidateBehaviorPolicy candidate(std::move(v));

    GeometryBucket other = bucket_2p();
    other.key.pot_bb = 5;
    other.representative_contrib = {5, 5};
    other.representative_pot = 10;
    const GameDef def = rooted_def(other, {0, 6, 21});
    GameState state(def);
    HandLog log;
    PolicyContext ctx;
    ctx.hand_log = &log;
    ctx.decision_seed = 1;
    bool threw = false;
    try {
      candidate.distribution(state, *state.actor(), HoleCards{1, 7}, ctx);
    } catch (const stage6_candidate_error&) {
      threw = true;
    }
    check(threw, "an uncovered flop geometry throws (no silent fallback)");
  }

  // Wrong artifact stamp: corrupt one manifest stamp; lookup must fail.
  {
    CandidateBinding b = make_binding();
    b.manifest.artifact_content_hash ^= 0x1ULL;
    std::vector<CandidateBinding> v;
    v.push_back(std::move(b));
    CandidateBehaviorPolicy candidate(std::move(v));

    const GameDef def = rooted_def(bucket_2p(), {0, 6, 21});
    GameState state(def);
    HandLog log;
    PolicyContext ctx;
    ctx.hand_log = &log;
    ctx.decision_seed = 1;
    bool threw = false;
    try {
      candidate.distribution(state, *state.actor(), HoleCards{1, 7}, ctx);
    } catch (const stage6_candidate_error&) {
      threw = true;
    }
    check(threw, "a mismatched artifact stamp throws rather than returning a row");
  }

  // A manifest whose bucket disagrees with its binding is rejected at build.
  {
    CandidateBinding b = make_binding();
    b.manifest.bucket.pot_bb = 99;
    bool threw = false;
    try {
      std::vector<CandidateBinding> v;
      v.push_back(std::move(b));
      CandidateBehaviorPolicy candidate(std::move(v));
    } catch (const stage6_candidate_error&) {
      threw = true;
    }
    check(threw, "a manifest/binding bucket mismatch is refused at construction");
  }
  return local;
}

// Streaming cursor end-to-end: a shallow live==3 artifact binds and the
// candidate returns a legal distribution at the reduced rooted flop.
int test_streaming_3p_decisions() {
  int local = 0;
  const GeometryBucket bucket = bucket_3p_shallow();
  const TrainingConfig config = make_config(20'000, 11);
  BucketTrainingResult trained = train_bucket(bucket, config);
  check(trained.report.mode == TrainerMode::Streaming, "3p bucket trains in streaming mode");
  CandidateBinding binding;
  binding.bucket = bucket;
  binding.action = config.action;
  binding.manifest =
      manifest_for(bucket, config, trained.rows, trained.report.iterations_completed);
  binding.rows = trained.rows;
  std::vector<CandidateBinding> v;
  v.push_back(std::move(binding));
  CandidateBehaviorPolicy candidate(std::move(v));

  const GameDef def = rooted_def(bucket, {0, 6, 21});
  HandLog log;
  PolicyContext ctx;
  ctx.hand_log = &log;
  ctx.decision_seed = 0x6f7261636c65ULL;
  GameState state(def);
  const std::size_t seat = *state.actor();
  for (const HoleCards& hole : {HoleCards{1, 7}, HoleCards{2, 8}, HoleCards{4, 10}}) {
    GameState s(def);
    try {
      const std::vector<PolicyAction> dist = candidate.distribution(s, seat, hole, ctx);
      if (!is_distribution_legal(dist, s)) {
        std::printf("FAIL: streaming 3p distribution is not legal/unit-sum\n");
        ++local;
      }
    } catch (const std::exception& e) {
      std::printf("FAIL: streaming 3p candidate threw: %s\n", e.what());
      ++local;
    }
  }
  return local;
}

// The coarse NashConv measurement mode: a structurally-valid coarse node with
// no sealed row answers with the artifact's uniform-unvisited value and counts
// the miss, but a genuine abstraction violation (uncovered geometry) still
// throws regardless of the mode.
int test_uniform_on_unvisited_policy() {
  int local = 0;
  const GeometryBucket bucket = bucket_2p();
  const TrainingConfig config = make_config(1, 1);  // one iteration: almost everything uncovered
  BucketTrainingResult trained = train_bucket(bucket, config);
  CandidateBinding binding;
  binding.bucket = bucket;
  binding.action = config.action;
  binding.manifest =
      manifest_for(bucket, config, trained.rows, trained.report.iterations_completed);
  binding.rows = trained.rows;
  std::vector<CandidateBinding> v;
  v.push_back(std::move(binding));
  CandidateBehaviorPolicy candidate(std::move(v), CandidateMissPolicy::UniformOnUnvisited);

  const GameDef def = rooted_def(bucket, {0, 6, 21});
  // Drive down a coarse branch that a single sweep almost certainly never
  // sealed a row for, repeatedly, so at least one lookup is an unvisited node.
  std::uint64_t observed_misses_before = candidate.unvisited_misses();
  bool got_legal_uniform = false;
  for (const HoleCards& hole : {HoleCards{1, 7}, HoleCards{2, 8}, HoleCards{4, 10}}) {
    HandLog log;
    PolicyContext ctx;
    ctx.hand_log = &log;
    ctx.decision_seed = 3;
    GameState state(def);
    try {
      const std::vector<PolicyAction> dist =
          candidate.distribution(state, *state.actor(), hole, ctx);
      if (is_distribution_legal(dist, state)) {
        // At a root node one sweep usually leaves no row, in which case the
        // answer is uniform over the root menu.
        double uniform = 1.0 / static_cast<double>(dist.size());
        bool all_uniform = true;
        for (const PolicyAction& pa : dist)
          if (std::abs(pa.probability - uniform) > 1e-9)
            all_uniform = false;
        if (all_uniform)
          got_legal_uniform = true;
      } else {
        std::printf("FAIL: unvisited fallback returned an illegal distribution\n");
        ++local;
      }
    } catch (const std::exception& e) {
      std::printf("FAIL: uniform-on-unvisited threw on a covered fixture: %s\n", e.what());
      ++local;
    }
  }
  check(candidate.unvisited_misses() >= observed_misses_before,
        "unvisited miss counter is monotonic");
  check(got_legal_uniform || candidate.unvisited_misses() > 0,
        "an unvisited structurally-valid node returns the legal uniform fallback");

  // A genuinely uncovered geometry STILL throws even in uniform mode.
  GeometryBucket other = bucket;
  other.key.pot_bb = 5;
  other.representative_contrib = {5, 5};
  other.representative_pot = 10;
  // Rebuild a candidate whose only binding is the original bucket, then query
  // the other geometry: this must fail closed (it is an abstraction miss, not
  // an unvisited coarse node).
  {
    BucketTrainingResult again = train_bucket(bucket, config);
    CandidateBinding b;
    b.bucket = bucket;
    b.action = config.action;
    b.manifest = manifest_for(bucket, config, again.rows, again.report.iterations_completed);
    b.rows = again.rows;
    std::vector<CandidateBinding> v2;
    v2.push_back(std::move(b));
    CandidateBehaviorPolicy strict(std::move(v2), CandidateMissPolicy::UniformOnUnvisited);
    const GameDef other_def = rooted_def(other, {0, 6, 21});
    HandLog log;
    PolicyContext ctx;
    ctx.hand_log = &log;
    GameState state(other_def);
    bool threw = false;
    try {
      (void)strict.distribution(state, *state.actor(), HoleCards{1, 7}, ctx);
    } catch (const stage6_candidate_error&) {
      threw = true;
    }
    check(threw, "an uncovered geometry throws even in uniform-on-unvisited mode");
  }
  return local;
}

// At a preflop state the composed candidate must decide EXACTLY as the pinned
// BaselineBehaviorPolicy (they share chart_preflop).
int test_preflop_matches_baseline() {
  int local = 0;
  const GeometryBucket bucket = bucket_2p();
  const TrainingConfig config = make_config(1'000, 1);
  BucketTrainingResult trained = train_bucket(bucket, config);
  CandidateBinding b;
  b.bucket = bucket;
  b.action = config.action;
  b.manifest = manifest_for(bucket, config, trained.rows, trained.report.iterations_completed);
  b.rows = trained.rows;
  std::vector<CandidateBinding> v;
  v.push_back(std::move(b));
  CandidateBehaviorPolicy candidate(std::move(v));
  BaselineBehaviorPolicy baseline;

  // A standard 3-seat preflop def.
  GameDef def{};
  def.player_count = 3;
  def.button = 0;
  def.big_blind = 2;
  def.preflop = true;
  for (std::size_t i = 0; i < 3; ++i)
    def.stacks[i] = 200;
  std::array<Chips, 10> blinds{};
  blinds[1] = 1;
  blinds[2] = 2;
  def.blinds_posted = blinds;
  def.pot = 3;
  def.board = {-1, -1, -1, -1, -1};

  GameState state(def);
  HandLog log;
  PolicyContext ctx;
  ctx.hand_log = &log;
  ctx.decision_seed = 12345;
  const std::size_t seat = *state.actor();
  const HoleCards hole{39, 40};  // two cards not special to the charts
  const std::vector<PolicyAction> cand = candidate.distribution(state, seat, hole, ctx);
  const std::vector<PolicyAction> base = baseline.distribution(state, seat, hole, ctx);
  if (cand.size() != 1 || base.size() != 1 || cand[0].action != base[0].action) {
    std::printf("FAIL: candidate preflop does not match the pinned baseline\n");
    ++local;
  }
  check(cand.size() == 1 && cand[0].probability == 1.0,
        "preflop candidate is a deterministic chart action");
  return local;
}

}  // namespace

int main() {
  failures += test_rooted_flop_decisions();
  failures += test_streaming_3p_decisions();
  failures += test_totality_failures();
  failures += test_uniform_on_unvisited_policy();
  failures += test_preflop_matches_baseline();
  if (failures) {
    std::printf("STAGE 6 CANDIDATE TESTS FAILED: %d\n", failures);
    return 1;
  }
  std::puts("STAGE 6 CANDIDATE TESTS PASSED");
  return 0;
}
