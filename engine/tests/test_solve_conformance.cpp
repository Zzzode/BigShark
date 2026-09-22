// RFC 0008 stage 4 L4 conformance.
//
// The unified solve() must reproduce the heads-up multistreet CFR's previous
// results BIT-FOR-BIT under the identity abstraction. This is NOT a
// same-call-twice comparison: the direct-call side builds its HeadsUpGame from
// an INDEPENDENT root literal (asserted same_game_def-equal to the tree's def,
// never pointer-identical), and solve() reconstructs the game from the tree +
// request. It covers both drivers (full + sampled), all fixed-runout variants,
// and negative typed refusals, plus the {2,10} all-in-blind deal-out.
#include <bs/detail/solve_projection.hpp>
#include <bs/eval.hpp>
#include <bs/game_definition.hpp>
#include <bs/solve.hpp>
#include <cstdio>
#include <optional>
#include <string>
#include <vector>

#define CHECK(cond)                                                         \
  do {                                                                      \
    if (!(cond)) {                                                          \
      std::printf("CHECK failed: %s (%s:%d)\n", #cond, __FILE__, __LINE__); \
      return 1;                                                             \
    }                                                                       \
  } while (0)

namespace {

using namespace bs;
using namespace bs::poker;
using namespace bs::solver;
using namespace bs::tree;

int card(const char* n) {
  return cardId(std::string(n));
}

bool rows_equal(const HeadsUpPolicy& a, const HeadsUpPolicy& b) {
  if (a.rows().size() != b.rows().size())
    return false;
  auto jt = b.rows().begin();
  for (const auto& [key, row] : a.rows()) {
    if (key != jt->first || row.actions != jt->second.actions)
      return false;
    if (row.probabilities.size() != jt->second.probabilities.size())
      return false;
    for (std::size_t i = 0; i < row.probabilities.size(); ++i)
      if (row.probabilities[i] != jt->second.probabilities[i])  // raw ==
        return false;
    ++jt;
  }
  return true;
}

// Every reported field must agree, not just the policy rows.
bool result_equal(const TrainingResult& a, const TrainingResult& b) {
  return a.status == b.status && a.completed_iterations == b.completed_iterations &&
         a.nodes == b.nodes && a.information_sets == b.information_sets &&
         a.accounted_bytes == b.accounted_bytes && a.seed == b.seed &&
         a.prng_state == b.prng_state && rows_equal(a.policy, b.policy);
}

// The golden fixture (test_heads_up_solver.cpp:58), rebuilt here independently.
// Rooted flop 2c3d7h, 1/1 stacks, contributions {1,1}, pot 2, bb 1, button 1.
HeadsUpGame direct_game(std::optional<int> turn, std::optional<int> river) {
  HeadsUpGame game;
  game.root = {{card("2c"), card("3d"), card("7h")}, {1, 1}, {1, 1}, 2, 1, 1};
  game.ranges[0] = {{{card("Ac"), card("Ad")}, 2}, {{card("8c"), card("8d")}, 3}};
  game.ranges[1] = {{{card("Ac"), card("As")}, 5}, {{card("Qc"), card("Qd")}, 7}};
  game.fixed_runout = {turn, river};
  return game;
}

// The same game as an independent GameDef literal (the L3 side). Asserted
// same_game_def-equal rather than constructed from the trainer's root.
GameDef golden_def() {
  GameDef def{};
  def.player_count = 2;
  def.button = 1;
  def.big_blind = 1;
  def.stacks = {1, 1, 0, 0, 0, 0, 0, 0, 0, 0};
  def.contributions = {1, 1, 0, 0, 0, 0, 0, 0, 0, 0};
  def.pot = 2;
  def.board = {card("2c"), card("3d"), card("7h"), 0, 0};
  def.board_size = 3;
  return def;
}

std::vector<WeightedHand> range0() {
  return {{{card("Ac"), card("Ad")}, 2}, {{card("8c"), card("8d")}, 3}};
}
std::vector<WeightedHand> range1() {
  return {{{card("Ac"), card("As")}, 5}, {{card("Qc"), card("Qd")}, 7}};
}

SolveRequest make_request(const AbstractTree& tree, SolveMode mode, std::uint64_t iterations,
                          std::uint64_t seed, std::optional<int> turn, std::optional<int> river) {
  SolveRequest req;
  req.tree = &tree;
  req.ranges = {range0(), range1()};
  req.mode = mode;
  req.iterations = iterations;
  req.seed = seed;
  req.runout = {turn, river};
  return req;
}

// Full-traversal bit-for-bit over all three runout variants.
int test_full_conformance(const AbstractTree& tree) {
  const struct Variant {
    std::optional<int> turn, river;
  } variants[] = {{card("Js"), card("9c")}, {card("Js"), std::nullopt}, {std::nullopt, card("9c")}};
  for (const Variant& v : variants) {
    HeadsUpTrainer direct(direct_game(v.turn, v.river));
    TrainingResult want = direct.train(1);

    SolveResult got = solve(make_request(tree, SolveMode::FullTraversal, 1, 0, v.turn, v.river));
    const TrainingResult& r = got.training();
    CHECK(r.status == TrainingStatus::Complete);
    CHECK(r.completed_iterations == 1);
    CHECK(r.information_sets == want.information_sets);
    CHECK(r.nodes == want.nodes);
    CHECK(r.accounted_bytes == want.accounted_bytes);
    CHECK(rows_equal(r.policy, want.policy));
    CHECK(got.action_id() == abstraction::identity_action_id());
  }
  return 0;
}

// External-sampling bit-for-bit over EVERY runout variant (the chance
// conditioning consumes the SplitMix64 stream differently for free vs fixed
// streets), every reported field, and seam repeatability under a fixed seed.
int test_sampled_conformance(const AbstractTree& tree) {
  const std::uint64_t seed = 17;
  const std::uint64_t iterations = 200;  // small; equality, not convergence, is the gate
  const struct Variant {
    std::optional<int> turn, river;
  } variants[] = {{card("Js"), card("9c")}, {card("Js"), std::nullopt}, {std::nullopt, card("9c")}};
  for (const Variant& v : variants) {
    HeadsUpTrainer direct(direct_game(v.turn, v.river));
    TrainingResult want = direct.train_sampled(iterations, seed);

    SolveResult got =
        solve(make_request(tree, SolveMode::ExternalSampling, iterations, seed, v.turn, v.river));
    CHECK(result_equal(got.training(), want));

    // The seam must be deterministic: a second identical solve reproduces the
    // first, including the terminal PRNG state.
    SolveResult again =
        solve(make_request(tree, SolveMode::ExternalSampling, iterations, seed, v.turn, v.river));
    CHECK(result_equal(again.training(), got.training()));
  }
  return 0;
}

// Negative: every unsupported shape throws unsupported_tree_shape, which is NOT
// an invalid_argument.
int test_refusals() {
  // True iff solve(req) throws unsupported_tree_shape and nothing in the
  // invalid_argument family.
  auto expect_shape = [](const SolveRequest& req) {
    bool typed = false, invalid = false;
    try {
      solve(req);
    } catch (const unsupported_tree_shape&) {
      typed = true;
    } catch (const std::invalid_argument&) {
      invalid = true;
    }
    return typed && !invalid;
  };

  // 3-seat tree refuses through the heads-up solver.
  {
    GameDef def{};
    def.player_count = 3;
    def.button = 0;
    def.big_blind = 2;
    def.stacks = {1, 1, 1, 0, 0, 0, 0, 0, 0, 0};
    def.contributions = {1, 1, 1, 0, 0, 0, 0, 0, 0, 0};
    def.pot = 3;
    def.board = {card("2c"), card("3d"), card("7h"), 0, 0};
    def.board_size = 3;
    AbstractTree tree(def, abstraction::ActionAbstraction::identity());
    SolveRequest req =
        make_request(tree, SolveMode::FullTraversal, 1, 0, std::nullopt, std::nullopt);
    req.ranges.push_back({{{card("Kh"), card("Qh")}, 1}});
    bool typed = false, invalid = false;
    try {
      solve(req);
    } catch (const unsupported_tree_shape&) {
      typed = true;
    } catch (const std::invalid_argument&) {
      invalid = true;
    }
    CHECK(typed);
    CHECK(!invalid);
  }

  // Non-identity action abstraction is refused (construct a tree on a custom
  // schedule whose id differs).
  {
    abstraction::SizeSchedule custom = abstraction::default_size_schedule();
    custom[0].bets = {{1, 2}, {3, 4}, {3, 2}};  // differs from identity
    abstraction::ActionAbstraction nonidentity(custom);
    CHECK(nonidentity.id() != abstraction::identity_action_id());
    GameDef def = golden_def();
    AbstractTree tree(def, nonidentity);
    SolveRequest req =
        make_request(tree, SolveMode::FullTraversal, 1, 0, std::nullopt, std::nullopt);
    bool typed = false;
    try {
      solve(req);
    } catch (const unsupported_tree_shape&) {
      typed = true;
    }
    CHECK(typed);
  }

  // Null tree is invalid_argument (malformed request, not a shape refusal).
  {
    SolveRequest req;
    bool invalid = false;
    try {
      solve(req);
    } catch (const std::invalid_argument&) {
      invalid = true;
    }
    CHECK(invalid);
  }

  // Zero iterations is invalid_argument.
  {
    GameDef def = golden_def();
    AbstractTree tree(def, abstraction::ActionAbstraction::identity());
    SolveRequest req =
        make_request(tree, SolveMode::FullTraversal, 0, 0, std::nullopt, std::nullopt);
    bool invalid = false;
    try {
      solve(req);
    } catch (const std::invalid_argument&) {
      invalid = true;
    }
    CHECK(invalid);
  }

  // Every non-L1 RFC-named solver is reachable through solve() and refuses with
  // the typed exception (never silently approximates or returns a policy).
  {
    GameDef def = golden_def();
    AbstractTree tree(def, abstraction::ActionAbstraction::identity());
    const SolverKind kinds[] = {SolverKind::RiverLp, SolverKind::RiverDcfr,
                                SolverKind::MultistreetCfr};
    for (SolverKind kind : kinds) {
      SolveRequest req =
          make_request(tree, SolveMode::FullTraversal, 1, 0, std::nullopt, std::nullopt);
      req.solver = kind;
      bool typed = false, invalid = false;
      try {
        solve(req);
      } catch (const unsupported_tree_shape&) {
        typed = true;
      } catch (const std::invalid_argument&) {
        invalid = true;
      }
      CHECK(typed);
      CHECK(!invalid);
    }
  }

  // Explicit HeadsUpCfr selection on a 3-seat tree refuses with the typed
  // error (the explicit branch in solve(), distinct from Auto).
  {
    GameDef def{};
    def.player_count = 3;
    def.button = 0;
    def.big_blind = 2;
    def.stacks = {1, 1, 1, 0, 0, 0, 0, 0, 0, 0};
    def.contributions = {1, 1, 1, 0, 0, 0, 0, 0, 0, 0};
    def.pot = 3;
    def.board = {card("2c"), card("3d"), card("7h"), 0, 0};
    def.board_size = 3;
    AbstractTree tree(def, abstraction::ActionAbstraction::identity());
    SolveRequest req =
        make_request(tree, SolveMode::FullTraversal, 1, 0, std::nullopt, std::nullopt);
    req.solver = SolverKind::HeadsUpCfr;
    CHECK(expect_shape(req));
  }

  // A two-seat tree with one or three seat ranges is a shape refusal (not
  // reached earlier via the player_count guard).
  {
    GameDef def = golden_def();
    AbstractTree tree(def, abstraction::ActionAbstraction::identity());
    for (int extra : {-1, 1}) {
      SolveRequest req =
          make_request(tree, SolveMode::FullTraversal, 1, 0, std::nullopt, std::nullopt);
      if (extra < 0)
        req.ranges.pop_back();
      else
        req.ranges.push_back(range1());
      CHECK(expect_shape(req));
    }
  }

  // Fixed conditioning that is illegal for the rooted game must refuse with the
  // typed error, never leak the trainer's invalid_argument: card on the board,
  // turn==river duplicate, card colliding with a dealt hole card, out of range
  // above 51, and a PRESENT negative id (which must not be mistaken for the
  // absent/nullopt free runout). Preflop conditioning is refused in solve() but
  // cannot be driven end-to-end here because a preflop tree is unmaterializable;
  // it is documented defense-in-depth.
  {
    GameDef def = golden_def();
    AbstractTree tree(def, abstraction::ActionAbstraction::identity());
    const struct Bad {
      std::optional<int> turn, river;
    } bad[] = {{card("7h"), std::nullopt}, {card("Js"), card("Js")},
               {card("Ac"), std::nullopt},  // collides a seat-0 hole card
               {card("As"), std::nullopt},  // collides a seat-1 hole card only
               {52, std::nullopt},         {-1, std::nullopt},
               {std::nullopt, -5}};
    for (const Bad& b : bad)
      CHECK(expect_shape(make_request(tree, SolveMode::FullTraversal, 1, 0, b.turn, b.river)));
    // Absent conditioning (nullopt in either slot) remains valid and solves.
    SolveRequest ok =
        make_request(tree, SolveMode::FullTraversal, 1, 0, std::nullopt, std::nullopt);
    SolveResult r = solve(ok);
    CHECK(r.training().status == TrainingStatus::Complete);
  }
  return 0;
}

// The two-seat GameDef -> HeadsUpRoot projection, verified for BOTH profiles.
// A full unconditioned preflop public tree is not materializable under the L3
// node cap, so the preflop arm cannot be driven end-to-end through solve();
// the projection is a pure function and is unit-tested here directly against
// the legacy HeadsUpState, while the rooted-flop arm is additionally covered
// bit-for-bit by test_full/sampled_conformance.
int test_projection() {
  // Preflop profile: the {2,10} short-big-blind all-in-blind deal-out. The
  // projected root must carry the -1 flop sentinels and the posted blinds, and
  // the legacy state it builds must match the unified GameState deal-out.
  {
    GameDef def{};
    def.player_count = 2;
    def.button = 1;
    def.big_blind = 2;
    def.stacks = {2, 10, 0, 0, 0, 0, 0, 0, 0, 0};
    def.contributions = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    def.pot = 2;
    def.board = {-1, -1, -1, 0, 0};
    def.board_size = 0;
    def.preflop = true;
    def.blinds_posted = {2, 1, 0, 0, 0, 0, 0, 0, 0, 0};

    GameState state(def);                                         // unified profile
    const HeadsUpRoot root = detail::project_heads_up_root(def);  // the seam projection

    // Every numeric field is copied verbatim.
    CHECK(root.preflop);
    CHECK(root.flop[0] == -1 && root.flop[1] == -1 && root.flop[2] == -1);
    CHECK(root.stacks[0] == 2 && root.stacks[1] == 10);
    CHECK(root.contributions[0] == 0 && root.contributions[1] == 0);
    CHECK(root.pot == 2);
    CHECK(root.big_blind == 2);
    CHECK(root.button == 1);
    CHECK(root.blinds_posted[0] == 2 && root.blinds_posted[1] == 1);

    // The legacy state built from the projection behaves identically to the
    // unified GameState: straight to Deal, seat 0 all in, one chip refunded.
    HeadsUpState legacy(root);
    CHECK(legacy.phase() == state.phase());
    CHECK(legacy.phase() == Phase::Deal);
    CHECK(!state.actor().has_value());
    CHECK(state.players()[0].all_in);
    CHECK(state.players()[0].refunded == 1);
    CHECK(state.players()[0].street_committed == 1);
    CHECK(legacy.players()[0].refunded == 1);
    CHECK(legacy.players()[0].street_committed == 1);
  }

  // Rooted-flop profile: the golden fixture. The projection must carry the
  // three board cards, zero posted blinds, and build a legacy state whose first
  // actor matches the unified GameState's.
  {
    GameDef def = golden_def();
    const HeadsUpRoot root = detail::project_heads_up_root(def);
    CHECK(!root.preflop);
    CHECK(root.flop[0] == card("2c") && root.flop[1] == card("3d") && root.flop[2] == card("7h"));
    CHECK(root.stacks[0] == 1 && root.stacks[1] == 1);
    CHECK(root.contributions[0] == 1 && root.contributions[1] == 1);
    CHECK(root.pot == 2);
    CHECK(root.big_blind == 1);
    CHECK(root.button == 1);
    CHECK(root.blinds_posted[0] == 0 && root.blinds_posted[1] == 0);

    HeadsUpState legacy(root);
    GameState state(def);
    CHECK(legacy.phase() == state.phase());
    if (legacy.phase() == Phase::Action && state.phase() == Phase::Action)
      CHECK(*legacy.actor() == *state.actor());
  }
  return 0;
}

}  // namespace

int main() {
  // The free (unconditioned) identity tree; runout arrives via the request.
  // Stack-1 rooted-flop tree is small enough to fully materialize.
  GameDef def = golden_def();
  // The conformance asserts the direct literal's game identity agrees with the
  // tree's def (non-pointer, field-wise).
  HeadsUpRoot direct_root = direct_game(card("Js"), card("9c")).root;
  GameDef direct_as_def{};
  direct_as_def.player_count = 2;
  direct_as_def.button = direct_root.button;
  direct_as_def.big_blind = direct_root.big_blind;
  direct_as_def.stacks = {direct_root.stacks[0], direct_root.stacks[1]};
  direct_as_def.contributions = {direct_root.contributions[0], direct_root.contributions[1]};
  direct_as_def.pot = direct_root.pot;
  direct_as_def.board = {direct_root.flop[0], direct_root.flop[1], direct_root.flop[2], 0, 0};
  direct_as_def.board_size = 3;
  CHECK(same_game_def(def, direct_as_def));

  AbstractTree tree(def, abstraction::ActionAbstraction::identity());

  if (test_full_conformance(tree) != 0)
    return 1;
  if (test_sampled_conformance(tree) != 0)
    return 1;
  if (test_refusals() != 0)
    return 1;
  if (test_projection() != 0)
    return 1;

  std::printf("test_solve_conformance PASS\n");
  return 0;
}
