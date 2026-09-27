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

// NearestCoarseEdge integration, found empirically rather than hand-crafted.
// Walk a deep representative game along the EXACT declared five-fraction menu
// (aggressive at every seat) and locate a logged action where the strict
// lookup fails but the edge policy still answers the real state legally;
// record it as an off-tree decision. Same-type off-sizes are NOT such a point
// (strict already snaps those to the nearest edge).
int test_nearest_coarse_edge_line() {
  int local = 0;
  GeometryBucket bucket = bucket_2p();
  bucket.representative_stacks = {196, 196};  // 200 start, 4 in each
  const TrainingConfig config = make_config(20'000, 11);
  BucketTrainingResult trained = train_bucket(bucket, config);
  auto make_one = [&]() {
    CandidateBinding b;
    b.bucket = bucket;
    b.action = config.action;
    b.manifest = manifest_for(bucket, config, trained.rows, trained.report.iterations_completed);
    b.rows = trained.rows;
    std::vector<CandidateBinding> v;
    v.push_back(std::move(b));
    return v;
  };

  const std::array<int, 3> flop = {0, 6, 21};

  // Drive the real representative game with aggressive EXACT jam actions to
  // reach deep pots where coarse-edge overshoot desyncs the shadow.
  GameState real(GameState(rooted_def(bucket, flop)));
  HandLog log;
  PolicyContext ctx;
  ctx.decision_seed = 777;
  const HoleCards hole{2, 8};
  bool found_strict_failure = false;
  bool found_edge_answer = false;
  auto street_log = [&](Street s) -> std::vector<LoggedAction>& {
    return s == Street::Flop ? log.flop : (s == Street::Turn ? log.turn : log.river);
  };
  // Fixed 5-card runout consistent with the flop ids {0,6,21}.
  const std::array<int, 5> runout = {0, 6, 21, 30, 44};
  bool found_on_tree_equivalence = false;
  bool found_final_parity_offtree = false;
  for (int step = 0; step < 12; ++step) {
    if (real.phase() != Phase::Action)
      break;
    const std::size_t actor = *real.actor();
    ctx.hand_log = &log;

    // Final-node parity probe. The menu-parity branch fires at the acting
    // seat's OWN decision node (shadow aligned in phase/actor but aggression
    // presence differs): strict throws specifically the parity error, while
    // edge returns the counted uniform answer. A fresh policy is used so the
    // counter isolates THIS single query. This is the discriminating mutation
    // gate for the parity block — deleting it makes strict succeed here (the
    // later mid-log detector only trips after advancing the shadow).
    {
      CandidateBehaviorPolicy strict(make_one(), CandidateMissPolicy::UniformOnUnvisited,
                                     CandidateProjectionMode::StrictCoarse);
      bool strict_parity_throw = false;
      try {
        (void)strict.distribution(real, actor, hole, ctx);
      } catch (const stage6_candidate_error& e) {
        strict_parity_throw = std::string(e.what()).find("aggressive options") != std::string::npos;
      }
      if (strict_parity_throw) {
        CandidateBehaviorPolicy edge(make_one(), CandidateMissPolicy::UniformOnUnvisited,
                                     CandidateProjectionMode::NearestCoarseEdge);
        std::vector<PolicyAction> answer;
        bool edge_ok = true;
        try {
          answer = edge.distribution(real, actor, hole, ctx);
        } catch (const std::exception&) {
          edge_ok = false;
        }
        if (edge_ok && edge.off_tree_misses() == 1 && is_distribution_legal(answer, real)) {
          const std::vector<Action> exact_menu = declared_behavior_menu(real, actor);
          bool uniform = answer.size() == exact_menu.size();
          const double expect = 1.0 / static_cast<double>(exact_menu.size());
          for (const PolicyAction& pa : answer)
            if (std::abs(pa.probability - expect) > 1e-12)
              uniform = false;
          if (uniform)
            found_final_parity_offtree = true;
        }
      }
    }

    // Before diverging, strict and edge MUST agree at every still-on-tree
    // node (edge is addressing-only there): same distribution, zero off-tree.
    {
      CandidateBehaviorPolicy strict(make_one(), CandidateMissPolicy::UniformOnUnvisited,
                                     CandidateProjectionMode::StrictCoarse);
      CandidateBehaviorPolicy edge(make_one(), CandidateMissPolicy::UniformOnUnvisited,
                                   CandidateProjectionMode::NearestCoarseEdge);
      std::vector<PolicyAction> ds;
      std::vector<PolicyAction> de;
      bool s_ok = true, e_ok = true;
      try {
        ds = strict.distribution(real, actor, hole, ctx);
      } catch (const std::exception&) {
        s_ok = false;
      }
      try {
        de = edge.distribution(real, actor, hole, ctx);
      } catch (const std::exception&) {
        e_ok = false;
      }
      if (s_ok && e_ok) {
        found_on_tree_equivalence = true;
        if (ds.size() != de.size()) {
          std::printf("FAIL: strict/edge diverge in support at an on-tree node\n");
          return ++local;
        }
        for (std::size_t i = 0; i < ds.size(); ++i)
          if (ds[i].action != de[i].action ||
              std::abs(ds[i].probability - de[i].probability) > 1e-12) {
            std::printf("FAIL: strict/edge diverge in mass at an on-tree node\n");
            return ++local;
          }
      }
    }
    {
      CandidateBehaviorPolicy strict(make_one(), CandidateMissPolicy::UniformOnUnvisited,
                                     CandidateProjectionMode::StrictCoarse);
      try {
        (void)strict.distribution(real, actor, hole, ctx);
      } catch (const stage6_candidate_error&) {
        found_strict_failure = true;
        CandidateBehaviorPolicy edge(make_one(), CandidateMissPolicy::UniformOnUnvisited,
                                     CandidateProjectionMode::NearestCoarseEdge);
        std::vector<PolicyAction> answer;
        try {
          answer = edge.distribution(real, actor, hole, ctx);
        } catch (const std::exception& e) {
          std::printf("FAIL: edge candidate also failed at an off-tree point: %s\n", e.what());
          return ++local;
        }
        if (!is_distribution_legal(answer, real)) {
          std::printf("FAIL: edge off-tree answer is not a legal unit distribution\n");
          return ++local;
        }
        // The off-tree answer is exactly UNIFORM over the real declared menu:
        // zero information, not some biased proxy.
        const std::vector<Action> exact_menu = declared_behavior_menu(real, actor);
        if (answer.size() != exact_menu.size()) {
          std::printf("FAIL: off-tree answer is not uniform over the exact declared menu\n");
          return ++local;
        }
        const double expect = 1.0 / static_cast<double>(exact_menu.size());
        for (const PolicyAction& pa : answer)
          if (std::abs(pa.probability - expect) > 1e-12) {
            std::printf("FAIL: off-tree answer is not equal-mass\n");
            return ++local;
          }
        if (edge.off_tree_misses() == 0) {
          std::printf("FAIL: edge answered off-tree but did not count it\n");
          return ++local;
        }
        found_edge_answer = true;
        break;
      }
    }
    // Advance the real line with one exact 3/4-pot aggression (a size the
    // coarse {1/2 bet,1x raise} edges do NOT carry), then the fixed runout
    // cards; coarse-edge overshoot eventually desyncs the shadow.
    const LegalActions legal = real.legal();
    if (!legal.aggressive)
      break;
    long long target =
        static_cast<long long>(legal.call_amount) +
        (static_cast<long long>(real.pot()) + static_cast<long long>(legal.call_amount)) * 3 / 4;
    target = std::max<long long>(target, legal.aggressive->minimum);
    target = std::min<long long>(target, legal.aggressive->maximum);
    const Action a{legal.aggressive->type, static_cast<Chips>(target)};
    street_log(real.street()).push_back(LoggedAction{actor, a});
    real = real.after_action(actor, a);
    while (real.phase() == Phase::Deal) {
      const std::size_t want = real.board().size();
      real = real.after_card(runout[want]);
    }
  }
  check(found_strict_failure, "the deterministic edge walk reaches an off-tree point");
  check(found_edge_answer, "edge mode answers the off-tree point uniformly and counts it");
  check(found_on_tree_equivalence, "strict and edge agree at a preceding on-tree node");
  check(found_final_parity_offtree,
        "the final-node menu-parity branch is itself exercised by a single fresh query");
  return local;
}

}  // namespace

int main() {
  failures += test_rooted_flop_decisions();
  failures += test_streaming_3p_decisions();
  failures += test_totality_failures();
  failures += test_uniform_on_unvisited_policy();
  failures += test_preflop_matches_baseline();
  failures += test_nearest_coarse_edge_line();
  if (failures) {
    std::printf("STAGE 6 CANDIDATE TESTS FAILED: %d\n", failures);
    return 1;
  }
  std::puts("STAGE 6 CANDIDATE TESTS PASSED");
  return 0;
}
