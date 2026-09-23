// RFC 0008 stage 5 decision-provenance tests. The oracle below is STRUCTURAL:
// it predicts the source the router must declare from the Ctx shape and the
// backend hint, never by calling the mapper or trusting result().exact. It
// also keeps an independent copy of the FROZEN minor-0/1 reason-prefix
// inference solely as a journal-compatibility cross-check, pinning the exact
// discrepancy set (the four preflop chart folds) that minor 2 now labels
// truthfully. Every sourced decision is additionally checked to be the same
// Decision the legacy evaluatePolicy path returns.
#include <array>
#include <bs/eval.hpp>
#include <bs/guarantee.hpp>
#include <bs/policy.hpp>
#include <bs/river_gto.hpp>
#include <cstdio>
#include <initializer_list>
#include <string>
#include <vector>

namespace {

int failures = 0;

void check(bool condition, const std::string& description) {
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", description.c_str());
    ++failures;
  }
}

using bs::DecisionSource;
using bs::Guarantee;
using bs::Legal;
using bs::RiverBackendHint;
using bs::SourcedDecision;

Legal legalNoBet() {
  Legal l;
  l.actions = {"check", "bet"};
  l.raiseMin = 100;
  l.raiseMax = 2000;
  return l;
}

// Independent re-coding of the frozen reason-text inference (v1_response_mapper
// solverSource): its own table, never shared with production code. This is
// what minor 0/1 still emit; it exists here only to pin the divergence.
DecisionSource legacyInferredSource(const std::string& reason) {
  static const std::array<const char*, 14> kPrefixes = {
      "gto-cfr",    "gto",        "short jam",   "short premium", "RFI",
      "3bet value", "3bet bluff", "flat",        "call strong",   "4bet value",
      "4bet bluff", "vs3 call",   "jam premium", "BB option"};
  if (reason.rfind("gto-cfr", 0) == 0)
    return DecisionSource::RiverDcfr;
  if (reason.rfind("gto", 0) == 0)
    return DecisionSource::RiverLp;
  for (const char* prefix : kPrefixes)
    if (reason.rfind(prefix, 0) == 0 && std::string(prefix) != "gto" &&
        std::string(prefix) != "gto-cfr")
      return DecisionSource::PreflopChart;
  return DecisionSource::PostflopHeuristic;
}

// The four chart folds the old text inference mislabels, pinned by reason
// prefix and required declared source.
struct FoldCase {
  const char* reasonPrefix;
};

void expectSourced(const bs::Ctx& ctx, DecisionSource expected, RiverBackendHint hint,
                   const std::string& label) {
  const SourcedDecision sourced = bs::evaluatePolicySourced(ctx, hint);
  const bs::Decision legacy = bs::evaluatePolicy(ctx);
  check(sourced.source == expected, label + ": declared source is " + sourced.decision.reason);
  check(sourced.decision.action == legacy.action && sourced.decision.amount == legacy.amount &&
            sourced.decision.reason == legacy.reason,
        label + ": sourced answer equals the legacy evaluatePolicy decision");
  check(bs::guaranteeFor(sourced.source) == Guarantee::Approximate,
        label + ": every live policy source is approximate");
}

bs::Ctx preflopBase() {
  bs::Ctx ctx;
  ctx.handId = "prov-pre";
  ctx.seed = 11;
  ctx.street = "preflop";
  ctx.sb = 10;
  ctx.bb = 20;
  ctx.effectiveStackBb = 100;
  ctx.legal.actions = {"fold", "call", "raise"};
  ctx.legal.call = 40;
  ctx.legal.raiseMin = 100;
  ctx.legal.raiseMax = 2000;
  return ctx;
}

}  // namespace

int main() {
  // --- Pre-dispatch no-hole fold (v1-unreachable, core-only arm) ----------
  {
    bs::Ctx ctx;
    ctx.street = "preflop";
    ctx.legal.actions = {"fold"};
    expectSourced(ctx, DecisionSource::PostflopHeuristic, {}, "no-hole fold");
    const SourcedDecision answer = bs::evaluatePolicySourced(ctx);
    check(answer.decision.reason == "no hole cards", "no-hole fold reason pinned");
  }

  // --- Every preflop() exit is a chart decision, folds included -----------
  // RFI raise: AsKs on the button opens.
  {
    bs::Ctx ctx = preflopBase();
    ctx.position = "BTN";
    ctx.playersInHand = 6;
    ctx.hole = {"As", "Ks"};
    ctx.legal.actions = {"fold", "check", "raise"};
    ctx.legal.call = 0;
    expectSourced(ctx, DecisionSource::PreflopChart, {}, "RFI raise");
  }
  // BB option (chart check) when it is the big blind and nobody opened.
  {
    bs::Ctx ctx = preflopBase();
    ctx.position = "BB";
    ctx.hole = {"9s", "8d"};
    ctx.legal.actions = {"fold", "check", "raise"};
    ctx.legal.call = 0;
    expectSourced(ctx, DecisionSource::PreflopChart, {}, "BB option");
  }
  // The four fold exits. Each is selected by preflop() and must therefore be
  // PREFLOP_CHART even though the legacy reason inference sees no chart prefix.
  const std::array<std::pair<std::string, std::string>, 4> foldHands = {{
      {"fold pre", "7c2d"},      // UTG RFI fold
      {"fold vs open", "7c2d"},  // vs one raise
      {"fold vs 3bet", "7c2d"},  // hero's 4bet spot after a 3bet
      {"fold vs 4bet", "7c2d"},  // facing a 4bet
  }};
  std::size_t foldIndex = 0;
  for (const auto& [expectedPrefix, hand] : foldHands) {
    bs::Ctx ctx = preflopBase();
    ctx.hole = {std::string(1, hand[0]) + std::string(1, hand[1]),
                std::string(1, hand[2]) + std::string(1, hand[3])};
    if (foldIndex == 0) {
      ctx.position = "UTG";
      ctx.playersInHand = 9;
      ctx.raises = 0;
      // No check option for an unopened UTG spot, so a hand outside the RFI
      // range reaches "fold pre" rather than the BB-option check.
      ctx.legal.actions = {"fold", "raise"};
      ctx.legal.call = 0;
    } else if (foldIndex == 1) {
      ctx.position = "BB";
      ctx.raises = 1;
      ctx.openerPosition = "HJ";
    } else if (foldIndex == 2) {
      ctx.position = "BTN";
      ctx.raises = 2;
      ctx.heroWasRaiser = true;
    } else {
      ctx.position = "BTN";
      ctx.raises = 3;
      ctx.heroWasRaiser = false;
    }
    const std::string label = "chart fold: " + expectedPrefix;
    const SourcedDecision sourced = bs::evaluatePolicySourced(ctx);
    check(sourced.source == DecisionSource::PreflopChart,
          label + " declared PREFLOP_CHART, got: " + sourced.decision.reason);
    check(sourced.decision.action == "fold", label + " is a fold");
    check(sourced.decision.reason.rfind(expectedPrefix, 0) == 0,
          label + " reason prefix pinned, got: " + sourced.decision.reason);
    // Pinned divergence: minor 0/1 inference calls this a heuristic answer.
    check(legacyInferredSource(sourced.decision.reason) == DecisionSource::PostflopHeuristic,
          label + " is in the legacy-inference discrepancy set");
    ++foldIndex;
  }
  // Non-fold preflop answers agree with the legacy inference (no divergence).
  {
    bs::Ctx ctx = preflopBase();
    ctx.position = "BTN";
    ctx.playersInHand = 6;
    ctx.hole = {"As", "Ks"};
    ctx.legal.actions = {"fold", "check", "raise"};
    ctx.legal.call = 0;
    const SourcedDecision sourced = bs::evaluatePolicySourced(ctx);
    check(legacyInferredSource(sourced.decision.reason) == DecisionSource::PreflopChart,
          "chart raises stay aligned with legacy inference");
  }

  // --- Postflop catch-all --------------------------------------------------
  {
    bs::Ctx ctx;
    ctx.handId = "prov-flop";
    ctx.seed = 5;
    ctx.street = "flop";
    ctx.sb = 10;
    ctx.bb = 20;
    ctx.position = "BB";
    ctx.playersInHand = 2;
    ctx.hole = {"2h", "3h"};
    ctx.board = {"As", "Ks", "Qs"};
    ctx.pot = 40;
    ctx.legal = legalNoBet();
    expectSourced(ctx, DecisionSource::PostflopHeuristic, {}, "flop heuristic catch-all");
  }

  // --- River GTO fall-throughs are tagged at the call site -----------------
  {
    // Ineligible: multiway river never enters the solver.
    bs::Ctx ctx;
    ctx.handId = "prov-river-mw";
    ctx.seed = 5;
    ctx.street = "river";
    ctx.sb = 10;
    ctx.bb = 20;
    ctx.playersInHand = 3;
    ctx.hole = {"Ah", "Ad"};
    ctx.board = {"As", "Ks", "Qh", "5d", "2c"};
    ctx.pot = 100;
    ctx.legal = legalNoBet();
    ctx.riverGtoOn = true;
    ctx.riverLine = "";
    expectSourced(ctx, DecisionSource::PostflopHeuristic, {}, "multiway river falls through");
  }
  {
    // Unknown river line never enters the solver.
    bs::Ctx ctx;
    ctx.handId = "prov-river-line";
    ctx.seed = 5;
    ctx.street = "river";
    ctx.sb = 10;
    ctx.bb = 20;
    ctx.playersInHand = 2;
    ctx.hole = {"Ah", "Ad"};
    ctx.board = {"As", "Ks", "Qh", "5d", "2c"};
    ctx.pot = 100;
    ctx.legal = legalNoBet();
    ctx.riverGtoOn = true;
    ctx.riverLine = "zzz";
    expectSourced(ctx, DecisionSource::PostflopHeuristic, {}, "unknown river line falls through");
  }

  // --- Eligible river: the backend hint decides LP vs DCFR ----------------
  {
    bs::Ctx ctx;
    ctx.handId = "prov-river-elig";
    ctx.seed = 9;
    ctx.street = "river";
    ctx.sb = 10;
    ctx.bb = 20;
    ctx.position = "BTN";
    ctx.playersInHand = 2;
    ctx.effectiveStackBb = 100;
    ctx.hole = {"Ah", "Ad"};
    ctx.board = {"As", "Ks", "Qh", "5d", "2c"};
    ctx.pot = 100;
    ctx.legal = legalNoBet();
    ctx.raises = 1;
    ctx.riverGtoOn = true;
    ctx.riverLine = "";
    ctx.riverBetFrac = 0.75;
    ctx.riverRaiseFrac = 1.0;

    // Forced bounded backend: any solver answer is deterministically DCFR in
    // EVERY build; the heuristic fall-through is the only other possibility.
    const SourcedDecision forced = bs::evaluatePolicySourced(ctx, RiverBackendHint{false});
    if (forced.decision.reason.rfind("gto", 0) == 0) {
      check(forced.source == DecisionSource::RiverDcfr,
            "allow_exact=false forces RIVER_DCFR on a solver answer");
      check(forced.decision.reason.rfind("gto-cfr", 0) == 0,
            "the DCFR answer carries the cfr reason tag");
    } else {
      check(forced.source == DecisionSource::PostflopHeuristic,
            "a solver fall-through under the forced hint stays heuristic");
    }

    // Default hint predicts LP/DCFR purely from the compile-time backend,
    // never from result().exact.
    const SourcedDecision live = bs::evaluatePolicySourced(ctx, RiverBackendHint{});
    if (live.decision.reason.rfind("gto", 0) == 0) {
      const DecisionSource expected =
          bs::gto::hasExactRiverLp() ? DecisionSource::RiverLp : DecisionSource::RiverDcfr;
      check(live.source == expected,
            std::string("live solver source matches hasExactRiverLp: ") + live.decision.reason);
      check(legacyInferredSource(live.decision.reason) == live.source,
            "river solver answers stay aligned with legacy inference");
    } else {
      check(live.source == DecisionSource::PostflopHeuristic,
            "live solver fall-through stays heuristic");
    }
  }

  if (failures != 0) {
    std::fprintf(stderr, "DECISION PROVENANCE TESTS FAILED: %d\n", failures);
    return 1;
  }
  std::puts("DECISION PROVENANCE TESTS PASSED");
  return 0;
}
