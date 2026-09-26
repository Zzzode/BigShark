// RFC 0008 stage 6 R12 step 6: the deterministic exact<->coarse translator
// (R7). Every projection is validated against the REAL LegalActions interval
// of a GameState built from game_definition.hpp; pins:
//   * fold/check/call identity passthrough (Call normalized to target 0);
//   * nearest integer snap with ties to the smaller total;
//   * aggressive type rewrite to the legal Bet/Raise kind;
//   * mass merge when two coarse entries project to one concrete action;
//   * the declared no-aggressive fallback call -> check -> fold, including a
//     real call-only node reached after a short all-in and a real check-only
//     node whose only opponents are all in;
//   * fail-closed throws instead of clamping an illegal passive action or a
//     malformed distribution;
//   * exact->coarse nearest-index mapping with -1 on passive mismatch/absence;
//   * determinism and the frozen "rfc0008-coarse-to-exact:v1" identity.
#include <algorithm>
#include <array>
#include <bs/abstraction.hpp>
#include <bs/behavior_policy.hpp>
#include <bs/eval.hpp>
#include <bs/game_definition.hpp>
#include <bs/heads_up.hpp>
#include <bs/stage6/translator.hpp>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <limits>
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

GameDef three_seat_preflop(Chips stack) {
  GameDef def{};
  def.player_count = 3;
  def.button = 0;
  def.big_blind = 2;
  def.preflop = true;
  for (std::size_t i = 0; i < 3; ++i)
    def.stacks[i] = stack;
  def.blinds_posted = {0, 1, 2, 0, 0, 0, 0, 0, 0, 0};
  def.pot = 3;
  def.board = {-1, -1, -1, -1, -1};
  return def;
}

GameDef three_seat_preflop_short_bb() {
  GameDef def = three_seat_preflop(200);
  def.stacks[2] = 9;  // big blind posts 2 and has 7 behind
  return def;
}

// Rooted 3-seat flop 2c3d7h. `contributions` is equal dead money per seat and
// `stacks` is the chips left behind at the root.
GameDef three_seat_rooted_flop(std::array<Chips, 3> stacks, Chips each) {
  GameDef d{};
  d.player_count = 3;
  d.button = 0;
  d.big_blind = 2;
  for (std::size_t i = 0; i < 3; ++i)
    d.stacks[i] = stacks[i];
  d.contributions = {each, each, each, 0, 0, 0, 0, 0, 0, 0};
  d.pot = 3 * each;
  d.board = {card("2c"), card("3d"), card("7h"), 0, 0};
  d.board_size = 3;
  return d;
}

bool is_finite_distribution(const std::vector<PolicyAction>& dist, const LegalActions& legal) {
  double total = 0.0;
  for (const PolicyAction& pa : dist) {
    if (!std::isfinite(pa.probability) || pa.probability < 0.0)
      return false;
    if (!legal.contains(pa.action))
      return false;
    total += pa.probability;
  }
  return std::fabs(total - 1.0) <= 1e-9 && !dist.empty();
}

const PolicyAction* find_action(const std::vector<PolicyAction>& dist, poker::Action action) {
  for (const PolicyAction& pa : dist)
    if (pa.action == action)
      return &pa;
  return nullptr;
}

bool throws_runtime(const std::string& description, const auto& fn) {
  try {
    fn();
  } catch (const std::exception&) {
    // The translator fails closed via std::runtime_error and its
    // std::invalid_argument subclass-of-logic-error argument errors; both are
    // exception-family refusals rather than silent clamps.
    return true;
  }
  check(false, description);
  return false;
}

void test_id_identity() {
  TranslatorId filled = nearest_target_translator_id();
  check(filled.name == "rfc0008-coarse-to-exact", "frozen translator name");
  check(filled.version == 1, "frozen translator version");
  check(filled.to_string() == "rfc0008-coarse-to-exact:v1", "stable id to_string");
  check(filled.digest != 0, "the eval-side translator identity carries a nonzero rule digest");
  check(declared_translator_id() == filled,
        "the declared constant equals the filled nearest-target identity");
  // A default-constructed id names the same rule (name+version) and projects.
  TranslatorId id;
  check(id.name == filled.name && id.version == filled.version,
        "a default-constructed translator id names the frozen v1 rule");
  check(declared_translator_id().to_string() == id.to_string(),
        "declared id string equals the default name/version");
  TranslatorId other;
  other.name = "something-else";
  throws_runtime("an unknown translator id is rejected, not projected under v1 semantics", [&] {
    GameState state(three_seat_preflop(200));
    const std::vector<PolicyAction> coarse{{{poker::ActionType::Fold, 0}, 1.0}};
    (void)translate_coarse_to_exact(state, *state.actor(), coarse, other);
  });
}

void test_passive_passthrough() {
  // Preflop UTG (seat 0) may fold or call; both pass through unchanged, and a
  // Call carrying a stray target total is normalized to 0.
  GameState state(three_seat_preflop(200));
  const std::size_t seat = *state.actor();
  check(seat == 0, "UTG acts first preflop");
  const LegalActions legal = state.legal();
  check(legal.fold && legal.call && legal.aggressive, "UTG can fold, call, and raise");
  {
    const std::vector<PolicyAction> coarse{
        {{poker::ActionType::Fold, 0}, 0.3},
        {{poker::ActionType::Call, 5}, 0.7},  // stray total on a call
    };
    const std::vector<PolicyAction> out = translate_coarse_to_exact(state, seat, coarse);
    check(is_finite_distribution(out, legal), "fold/call output is a legal distribution");
    check(out.size() == 2, "fold/call stay separate actions");
    const PolicyAction* fold = find_action(out, {poker::ActionType::Fold, 0});
    const PolicyAction* call = find_action(out, {poker::ActionType::Call, 0});
    check(fold && std::fabs(fold->probability - 0.3) < 1e-12, "fold mass passes through");
    check(call && call->action.target_total == 0 && std::fabs(call->probability - 0.7) < 1e-12,
          "call passes through with target_total 0");
  }

  // A rooted flop opens check-to: Check passes through identically.
  GameState flop(three_seat_rooted_flop({200, 200, 200}, 1));
  const std::size_t fseat = *flop.actor();
  check(fseat == 1, "SB acts first on the rooted flop");
  const LegalActions flegal = flop.legal();
  check(flegal.check && flegal.aggressive && !flegal.fold && !flegal.call,
        "flop opener can check and bet, not fold or call");
  {
    const std::vector<PolicyAction> coarse{{{poker::ActionType::Check, 0}, 1.0}};
    const std::vector<PolicyAction> out = translate_coarse_to_exact(flop, fseat, coarse);
    check(out.size() == 1 && out[0].action == poker::Action{poker::ActionType::Check, 0} &&
              out[0].probability == 1.0,
          "check identity passthrough");
    check(is_finite_distribution(out, flegal), "the checked output is legal");
  }
}

void test_nearest_snap_against_real_interval() {
  // The real UTG raise interval is [4, 200] (min one full big blind over the
  // 2-chip high, cap at the 200 stack).
  GameState state(three_seat_preflop(200));
  const std::size_t seat = *state.actor();
  const LegalActions legal = state.legal();
  check(legal.aggressive->type == poker::ActionType::Raise, "UTG faces a Raise interval");
  const Chips minimum = legal.aggressive->minimum;
  const Chips maximum = legal.aggressive->maximum;
  check(minimum == 4 && maximum == 200, "real UTG raise interval is [4,200]");

  // Tie-to-smaller exercised with half-integers measured AGAINST that real
  // interval: k+0.5 snaps down to k, inside, below, and above.
  check(nearest_legal_target(static_cast<double>(minimum) + 0.5, minimum, maximum) == minimum,
        "half-integer just above the minimum ties down to the minimum");
  check(nearest_legal_target(static_cast<double>(minimum) + 1.5, minimum, maximum) == minimum + 1,
        "half-integer midpoint ties to the smaller total");
  check(nearest_legal_target(static_cast<double>(minimum) + 0.4, minimum, maximum) == minimum,
        "sub-half rounds down");
  check(nearest_legal_target(static_cast<double>(minimum) + 0.6, minimum, maximum) == minimum + 1,
        "above-half rounds up");
  check(nearest_legal_target(0.0, minimum, maximum) == minimum, "below the interval clamps to it");
  check(nearest_legal_target(1.0e18, minimum, maximum) == maximum,
        "above the interval clamps to the cap");

  // End-to-end projection: integer totals map to themselves inside the
  // interval and clamp at either end.
  const std::vector<PolicyAction> coarse{
      {{poker::ActionType::Raise, 5}, 0.25},    // identity inside
      {{poker::ActionType::Raise, 3}, 0.25},    // below minimum -> 4
      {{poker::ActionType::Raise, 999}, 0.25},  // above maximum -> 200
      {{poker::ActionType::Call, 0}, 0.25},
  };
  const std::vector<PolicyAction> out = translate_coarse_to_exact(state, seat, coarse);
  check(is_finite_distribution(out, legal), "snapped distribution is legal and sums to 1");
  const PolicyAction* r5 = find_action(out, {poker::ActionType::Raise, 5});
  const PolicyAction* r4 = find_action(out, {poker::ActionType::Raise, 4});
  const PolicyAction* r200 = find_action(out, {poker::ActionType::Raise, 200});
  check(r5 && std::fabs(r5->probability - 0.25) < 1e-12, "legal integer total is identity");
  check(r4 && std::fabs(r4->probability - 0.25) < 1e-12, "sub-minimum target snaps to minimum");
  check(r200 && std::fabs(r200->probability - 0.25) < 1e-12, "over-cap target snaps to cap");
  check(!find_action(out, {poker::ActionType::Raise, 3}), "the illegal total never appears");

  throws_runtime("nearest snap rejects a NaN wanted total",
                 [&] { (void)nearest_legal_target(std::nan(""), minimum, maximum); });
  throws_runtime("nearest snap rejects an inverted interval",
                 [&] { (void)nearest_legal_target(10.0, maximum, minimum); });
}

void test_aggressive_type_rewrite_and_merge() {
  // On a rooted check-to flop the legal aggressive kind is Bet. A coarse RAISE
  // must be emitted as the legal Bet type, and two same-total aggressive
  // entries of different coarse kinds merge.
  GameState flop(three_seat_rooted_flop({200, 200, 200}, 1));
  const std::size_t seat = *flop.actor();
  const LegalActions legal = flop.legal();
  check(legal.aggressive->type == poker::ActionType::Bet, "flop opener faces a Bet interval");
  check(legal.aggressive->minimum == 2, "rooted flop bet minimum is the 2-chip big blind");
  {
    const std::vector<PolicyAction> coarse{
        {{poker::ActionType::Raise, 10}, 0.2},  // wrong kind, legal total
        {{poker::ActionType::Bet, 10}, 0.2},    // same projection -> merges
        {{poker::ActionType::Bet, 1}, 0.2},     // below minimum -> 2
        {{poker::ActionType::Check, 0}, 0.4},
    };
    const std::vector<PolicyAction> out = translate_coarse_to_exact(flop, seat, coarse);
    check(is_finite_distribution(out, legal), "type-rewritten output is a legal distribution");
    check(out.size() == 3, "three distinct concrete projections");
    const PolicyAction* b10 = find_action(out, {poker::ActionType::Bet, 10});
    const PolicyAction* b2 = find_action(out, {poker::ActionType::Bet, 2});
    const PolicyAction* check_pa = find_action(out, {poker::ActionType::Check, 0});
    check(b10 && std::fabs(b10->probability - 0.4) < 1e-12,
          "coarse Bet/Raise on one total merge their masses");
    check(b2 && std::fabs(b2->probability - 0.2) < 1e-12, "sub-minimum bet snaps to minimum");
    check(check_pa && std::fabs(check_pa->probability - 0.4) < 1e-12, "check mass is untouched");
    check(!find_action(out, {poker::ActionType::Raise, 10}),
          "the illegal Raise kind never appears on a Bet node");
  }

  // Stable first-seen merge order: a sub-minimum raise (4) and the minimum
  // raise (4) collapse at UTG; the merged entry keeps the FIRST position and a
  // later Call keeps its relative order.
  GameState pre(three_seat_preflop(200));
  const std::size_t pseat = *pre.actor();
  const std::vector<PolicyAction> coarse{
      {{poker::ActionType::Raise, 4}, 0.3},
      {{poker::ActionType::Call, 0}, 0.5},
      {{poker::ActionType::Raise, 3}, 0.2},
  };
  const std::vector<PolicyAction> out = translate_coarse_to_exact(pre, pseat, coarse);
  check(out.size() == 2, "two raises collapse onto one legal total");
  check(out[0].action == poker::Action{poker::ActionType::Raise, 4} &&
            std::fabs(out[0].probability - 0.5) < 1e-12,
        "merged raise keeps first-seen position and summed mass");
  check(out[1].action == poker::Action{poker::ActionType::Call, 0} &&
            std::fabs(out[1].probability - 0.5) < 1e-12,
        "call keeps its later stable position");
}

void test_no_aggressive_fallback_to_call() {
  // Real call-only node: UTG opens to 8, SB calls, the short BB (9 starting,
  // 7 behind after posting) shoves 9 -- a SHORT all-in that does not reopen
  // raise rights. Action returns to UTG facing 1 more chip with no legal raise.
  GameState state(three_seat_preflop_short_bb());
  GameState opened = state.after_action(0, {poker::ActionType::Raise, 8});
  GameState called = opened.after_action(1, {poker::ActionType::Call, 0});
  check(called.actor() == 2, "the short big blind acts after the SB calls");
  GameState shoved = called.after_action(2, {poker::ActionType::Raise, 9});
  check(shoved.actor() == 0, "action returns to UTG after the short jam");
  check(shoved.players()[2].all_in, "the short BB is all in for 9");
  const LegalActions legal = shoved.legal();
  check(legal.fold && legal.call && !legal.aggressive,
        "UTG faces fold/call only: the short jam did not reopen raising");
  check(legal.call_amount == 1, "UTG owes one chip");

  const std::vector<PolicyAction> coarse{
      {{poker::ActionType::Call, 0}, 0.2},
      {{poker::ActionType::Raise, 999}, 0.5},  // aggressive mass with nowhere to go
      {{poker::ActionType::Fold, 0}, 0.3},
  };
  const std::vector<PolicyAction> out = translate_coarse_to_exact(shoved, 0, coarse);
  check(is_finite_distribution(out, legal), "fallback output is a legal distribution summing to 1");
  for (const PolicyAction& pa : out)
    check(pa.action.type != poker::ActionType::Bet && pa.action.type != poker::ActionType::Raise,
          "no aggressive action survives at a non-aggressive node");
  const PolicyAction* call = find_action(out, {poker::ActionType::Call, 0});
  const PolicyAction* fold = find_action(out, {poker::ActionType::Fold, 0});
  check(call && std::fabs(call->probability - 0.7) < 1e-12,
        "aggressive mass shifts to CALL and merges with the call mass");
  check(fold && std::fabs(fold->probability - 0.3) < 1e-12, "fold mass stays fold");

  // The short big blind's OWN decision at that node faces the degenerate
  // all-in-only aggressive interval [9,9]. Three distinct coarse aggressive
  // entries (one below the minimum with the wrong named kind, one exact, one
  // above the cap) all snap to the single legal Raise 9 and merge to 0.6.
  check(called.actor() == 2, "the short BB decides at the call node");
  const LegalActions bb_legal = called.legal();
  check(
      bb_legal.aggressive && bb_legal.aggressive->minimum == 9 && bb_legal.aggressive->maximum == 9,
      "the short BB faces exactly the all-in-only interval [9,9]");
  const std::vector<PolicyAction> bb_coarse{
      {{poker::ActionType::Raise, 4}, 0.2},  // below minimum, correct kind
      {{poker::ActionType::Bet, 999}, 0.2},  // over cap, wrong kind
      {{poker::ActionType::Bet, 9}, 0.2},    // exact, wrong kind
      {{poker::ActionType::Call, 0}, 0.2},  {{poker::ActionType::Fold, 0}, 0.2},
  };
  const std::vector<PolicyAction> bb_out = translate_coarse_to_exact(called, 2, bb_coarse);
  check(is_finite_distribution(bb_out, bb_legal),
        "the all-in-only projection is a legal distribution summing to 1");
  const PolicyAction* jam9 = find_action(bb_out, {poker::ActionType::Raise, 9});
  check(jam9 && std::fabs(jam9->probability - 0.6) < 1e-12,
        "all three coarse aggressive entries collapse to Raise 9 with merged mass 0.6");
  check(bb_out.end() == std::find_if(bb_out.begin(), bb_out.end(),
                                     [](const PolicyAction& pa) {
                                       return pa.action.type == poker::ActionType::Bet;
                                     }),
        "a coarse Bet kind never survives onto a Raise interval");
}

void test_no_aggressive_fallback_to_check_and_only_fold_arm() {
  // Real check-only node: rooted flop where the opener's two opponents are
  // already all in. It can check (no bet ahead) but cannot wager against
  // anyone, so aggressive mass must shift to check.
  GameDef d = three_seat_rooted_flop({0, 10, 0}, 0);
  d.contributions = {10, 5, 5, 0, 0, 0, 0, 0, 0, 0};
  d.pot = 20;
  GameState state(d);
  check(state.actor() == 1, "the one seat with chips behind acts");
  const LegalActions legal = state.legal();
  check(legal.check && !legal.fold && !legal.call && !legal.aggressive,
        "the lone actionable seat can only check");
  {
    const std::vector<PolicyAction> coarse{
        {{poker::ActionType::Check, 0}, 0.4},
        {{poker::ActionType::Bet, 50}, 0.6},
    };
    const std::vector<PolicyAction> out = translate_coarse_to_exact(state, 1, coarse);
    check(is_finite_distribution(out, legal), "check-fallback output is legal and sums to 1");
    check(out.size() == 1 && out[0].action == poker::Action{poker::ActionType::Check, 0} &&
              std::fabs(out[0].probability - 1.0) < 1e-12,
          "aggressive mass shifts to CHECK and merges to probability one");
  }
  // Fold mass is never invented away: a coarse fold at this node is illegal
  // and must fail closed rather than be clamped.
  throws_runtime("coarse fold mass at a check-only node throws instead of clamping", [&] {
    const std::vector<PolicyAction> coarse{
        {{poker::ActionType::Fold, 0}, 0.1},
        {{poker::ActionType::Check, 0}, 0.9},
    };
    (void)translate_coarse_to_exact(state, 1, coarse);
  });

  // The L1 machine never offers fold as the sole passive action, so the final
  // fold arm of the declared chain is pinned on the helper directly, along
  // with the total-rule throw when no passive action exists at all.
  LegalActions only_fold;
  only_fold.fold = true;
  check(aggressive_fallback_action(only_fold) == poker::Action{poker::ActionType::Fold, 0},
        "with no call/check available aggressive mass falls through to fold");
  LegalActions none;
  throws_runtime("a node with no passive action is unrecoverable",
                 [&] { (void)aggressive_fallback_action(none); });
}

void test_fail_closed_inputs() {
  GameState state(three_seat_preflop(200));
  throws_runtime("translation outside the acting seat's action phase is rejected", [&] {
    const std::vector<PolicyAction> coarse{{{poker::ActionType::Fold, 0}, 1.0}};
    (void)translate_coarse_to_exact(state, 1, coarse);
  });
  throws_runtime("an empty coarse distribution is rejected",
                 [&] { (void)translate_coarse_to_exact(state, 0, std::vector<PolicyAction>{}); });
  throws_runtime("coarse masses must sum to 1", [&] {
    const std::vector<PolicyAction> coarse{
        {{poker::ActionType::Fold, 0}, 0.4},
        {{poker::ActionType::Call, 0}, 0.4},
    };
    (void)translate_coarse_to_exact(state, 0, coarse);
  });
  throws_runtime("a NaN coarse mass is rejected", [&] {
    const std::vector<PolicyAction> coarse{
        {{poker::ActionType::Fold, 0}, std::nan("")},
        {{poker::ActionType::Call, 0}, 1.0},
    };
    (void)translate_coarse_to_exact(state, 0, coarse);
  });
  throws_runtime("a negative coarse mass is rejected", [&] {
    const std::vector<PolicyAction> coarse{
        {{poker::ActionType::Fold, 0}, -0.1},
        {{poker::ActionType::Call, 0}, 1.1},
    };
    (void)translate_coarse_to_exact(state, 0, coarse);
  });
  throws_runtime("a positive-infinity coarse mass is rejected", [&] {
    const std::vector<PolicyAction> coarse{
        {{poker::ActionType::Fold, 0}, std::numeric_limits<double>::infinity()},
    };
    (void)translate_coarse_to_exact(state, 0, coarse);
  });
  // Illegal passive actions fail closed even when their mass is positive:
  // checking is illegal while a bet is owed, and calling is illegal at a
  // check-to node. Neither is silently clamped to the legal passive.
  throws_runtime("a coarse CHECK at a node that owes a bet is rejected, not clamped to call", [&] {
    const std::vector<PolicyAction> coarse{
        {{poker::ActionType::Check, 0}, 0.5},
        {{poker::ActionType::Call, 0}, 0.5},
    };
    (void)translate_coarse_to_exact(state, 0, coarse);
  });
  {
    GameState flop(three_seat_rooted_flop({200, 200, 200}, 1));
    const std::size_t fseat = *flop.actor();
    throws_runtime("a coarse CALL at a check-to flop node is rejected, not clamped to check", [&] {
      const std::vector<PolicyAction> coarse{
          {{poker::ActionType::Call, 0}, 0.5},
          {{poker::ActionType::Check, 0}, 0.5},
      };
      (void)translate_coarse_to_exact(flop, fseat, coarse);
    });
  }
}

void test_translation_over_declared_coarse_menu() {
  // Build the coarse distribution FROM the declared ActionAbstraction menu
  // (DeclaredOnly): every projected action must be legal and the total stays
  // a distribution, exercising merges between clamped fraction targets.
  GameState flop(three_seat_rooted_flop({200, 200, 200}, 1));
  const std::size_t seat = *flop.actor();
  const LegalActions legal = flop.legal();
  bs::abstraction::MultiwayMenuContext ctx;
  ctx.street = poker::Street::Flop;
  ctx.pot = flop.pot();
  ctx.actor_committed = 0;
  ctx.cover = 200;
  const bs::abstraction::ActionAbstraction abstract = bs::abstraction::ActionAbstraction::declared(
      bs::abstraction::default_size_schedule(), bs::abstraction::CoverSeeds::DeclaredOnly);
  const std::vector<poker::Action> menu = abstract.multiway_menu(legal, ctx);
  check(!menu.empty(), "the declared coarse menu is non-empty at a flop bet node");
  const double mass = 1.0 / static_cast<double>(menu.size());
  std::vector<PolicyAction> coarse;
  for (const poker::Action& a : menu)
    coarse.push_back({a, mass});
  const std::vector<PolicyAction> out = translate_coarse_to_exact(flop, seat, coarse);
  check(is_finite_distribution(out, legal),
        "a uniform distribution over the declared coarse menu projects to a legal distribution");
  check(out.size() <= menu.size(), "projection never creates actions");
}

void test_exact_to_coarse_index() {
  const std::vector<poker::Action> menu{
      {poker::ActionType::Fold, 0},   {poker::ActionType::Call, 0},
      {poker::ActionType::Raise, 4},  {poker::ActionType::Raise, 8},
      {poker::ActionType::Raise, 20}, {poker::ActionType::Raise, 200},
  };
  // Passives match exactly.
  check(project_exact_to_coarse_index(menu, {poker::ActionType::Fold, 0}) == 0,
        "exact fold locates its coarse entry");
  check(project_exact_to_coarse_index(menu, {poker::ActionType::Call, 0}) == 1,
        "exact call locates its coarse entry");
  check(project_exact_to_coarse_index(menu, {poker::ActionType::Check, 0}) == -1,
        "a passive absent from the menu maps to -1");
  check(project_exact_to_coarse_index(menu, {poker::ActionType::Call, 5}) == -1,
        "a call with a stray target total is a passive mismatch -> -1");

  // Aggressive nearest-index with ties to the smaller target.
  check(project_exact_to_coarse_index(menu, {poker::ActionType::Raise, 5}) == 2,
        "off-menu raise 5 is nearest to coarse 4");
  check(project_exact_to_coarse_index(menu, {poker::ActionType::Raise, 6}) == 2,
        "raise 6 is equidistant between 4 and 8; tie goes to the smaller (4)");
  check(project_exact_to_coarse_index(menu, {poker::ActionType::Raise, 14}) == 3,
        "raise 14 is equidistant between 8 and 20; tie goes to the smaller (8)");
  check(project_exact_to_coarse_index(menu, {poker::ActionType::Raise, 999}) == 5,
        "an over-cap exact raise locates the coarse cap");
  check(project_exact_to_coarse_index(menu, {poker::ActionType::Bet, 8}) == -1,
        "a bet has no coarse entry on a raise-only menu -> -1");

  // Tie-to-smaller is by TARGET, not by menu position: put the smaller total
  // at a later index and verify it still wins an equidistant tie.
  const std::vector<poker::Action> unsorted{
      {poker::ActionType::Raise, 20},
      {poker::ActionType::Raise, 8},
  };
  check(project_exact_to_coarse_index(unsorted, {poker::ActionType::Raise, 14}) == 1,
        "an exact tie resolves to the smaller target even at a later index");

  // Empty menu: everything maps to -1.
  check(project_exact_to_coarse_index({}, {poker::ActionType::Raise, 10}) == -1,
        "an empty coarse menu yields -1");
}

// Heads-up preflop root: the button posts the small blind and acts first; the
// big blind holds the option after a limp. This is a distinct rules profile
// from the 3-seat fixtures, so one real HU node is pinned here.
GameDef heads_up_preflop(Chips stack) {
  GameDef def{};
  def.player_count = 2;
  def.button = 0;
  def.big_blind = 2;
  def.preflop = true;
  def.stacks = {stack, stack, 0, 0, 0, 0, 0, 0, 0, 0};
  def.blinds_posted = {1, 2, 0, 0, 0, 0, 0, 0, 0, 0};
  def.pot = 3;
  def.board = {-1, -1, -1, -1, -1};
  return def;
}

void test_heads_up_real_nodes() {
  GameState sb(heads_up_preflop(200));
  check(sb.actor() == 0, "heads-up the button/small blind acts first preflop");
  const LegalActions sb_legal = sb.legal();
  check(sb_legal.fold && sb_legal.call && sb_legal.aggressive &&
            sb_legal.aggressive->type == poker::ActionType::Raise,
        "the HU small blind can fold, call the 1 extra, or raise");
  {
    // A coarse BET is retyped to the legal Raise; an over-cap total snaps to
    // the 200 cap.
    const std::vector<PolicyAction> coarse{{{poker::ActionType::Fold, 0}, 0.2},
                                           {{poker::ActionType::Call, 0}, 0.2},
                                           {{poker::ActionType::Raise, 4}, 0.2},
                                           {{poker::ActionType::Bet, 6}, 0.2},
                                           {{poker::ActionType::Raise, 999}, 0.2}};
    const std::vector<PolicyAction> out = translate_coarse_to_exact(sb, 0, coarse);
    check(is_finite_distribution(out, sb_legal),
          "the HU small-blind projection is a legal distribution summing to 1");
    const PolicyAction* r6 = find_action(out, {poker::ActionType::Raise, 6});
    const PolicyAction* r200 = find_action(out, {poker::ActionType::Raise, 200});
    check(r6 && std::fabs(r6->probability - 0.2) < 1e-12,
          "a coarse Bet 6 at the HU SB node retypes to Raise 6");
    check(r200 && std::fabs(r200->probability - 0.2) < 1e-12,
          "an over-cap HU raise snaps to the cap");
  }

  // The big blind OPTION after a limp: no bet is owed, so the BB can check or
  // raise but not call or fold.
  GameState bb = sb.after_action(0, {poker::ActionType::Call, 0});
  check(bb.actor() == 1, "a HU limp passes the option to the big blind");
  const LegalActions bb_legal = bb.legal();
  check(bb_legal.check && bb_legal.aggressive && !bb_legal.fold && !bb_legal.call,
        "the HU big-blind option is check or raise, never call or fold");
  {
    const std::vector<PolicyAction> coarse{
        {{poker::ActionType::Check, 0}, 0.3},
        {{poker::ActionType::Raise, 6}, 0.3},
        {{poker::ActionType::Raise, 999}, 0.4},
    };
    const std::vector<PolicyAction> out = translate_coarse_to_exact(bb, 1, coarse);
    check(is_finite_distribution(out, bb_legal),
          "the big-blind option projection is legal and sums to 1");
    check(out.size() == 3, "the three coarse entries stay distinct");
  }
  throws_runtime("a coarse CALL at the HU big-blind option node is rejected", [&] {
    const std::vector<PolicyAction> coarse{
        {{poker::ActionType::Call, 0}, 0.5},
        {{poker::ActionType::Check, 0}, 0.5},
    };
    (void)translate_coarse_to_exact(bb, 1, coarse);
  });
}

void test_determinism() {
  GameState state(three_seat_preflop(200));
  const std::vector<PolicyAction> coarse{
      {{poker::ActionType::Fold, 0}, 0.1},
      {{poker::ActionType::Call, 0}, 0.2},
      {{poker::ActionType::Raise, 4}, 0.3},
      {{poker::ActionType::Raise, 3}, 0.1},  // merges into 4
      {{poker::ActionType::Raise, 200}, 0.3},
  };
  const std::vector<PolicyAction> a = translate_coarse_to_exact(state, 0, coarse);
  const std::vector<PolicyAction> b = translate_coarse_to_exact(state, 0, coarse);
  const std::vector<PolicyAction> c =
      translate_coarse_to_exact(state, 0, coarse, declared_translator_id());
  check(a.size() == b.size(), "deterministic output size");
  bool same = a.size() == c.size();
  for (std::size_t i = 0; i < a.size(); ++i)
    same = same && a[i].action == b[i].action && a[i].probability == b[i].probability &&
           a[i].action == c[i].action && a[i].probability == c[i].probability;
  check(same, "repeated projections are identical including order and merged masses");
}

}  // namespace

int main() {
  test_id_identity();
  test_passive_passthrough();
  test_nearest_snap_against_real_interval();
  test_aggressive_type_rewrite_and_merge();
  test_no_aggressive_fallback_to_call();
  test_no_aggressive_fallback_to_check_and_only_fold_arm();
  test_fail_closed_inputs();
  test_translation_over_declared_coarse_menu();
  test_exact_to_coarse_index();
  test_heads_up_real_nodes();
  test_determinism();
  if (failures) {
    std::fprintf(stderr, "STAGE6 TRANSLATOR TESTS FAILED: %d\n", failures);
    return 1;
  }
  std::puts("STAGE6 TRANSLATOR TESTS PASSED");
  return 0;
}
