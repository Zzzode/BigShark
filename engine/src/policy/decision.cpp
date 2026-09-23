#include <algorithm>
#include <array>
#include <bs/charts.hpp>
#include <bs/decision.hpp>
#include <bs/equity.hpp>
#include <bs/eval.hpp>
#include <bs/policy.hpp>
#include <bs/range.hpp>
#include <bs/river_gto.hpp>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace bs {

// Preflop BB-rounded sizes were historically clamped to the raw legal bounds
// (raiseMin can be 0), not to max(1, raiseMin); preserve that literal range
// so in-range results are bit-identical to the v0 path.
static int clampRawLegal(std::int64_t v, const Legal& L) {
  return static_cast<int>(
      std::clamp(v, static_cast<std::int64_t>(L.raiseMin), static_cast<std::int64_t>(L.raiseMax)));
}

// Round a double chip amount to a big-blind multiple in int64. The multiply
// is int64 (not int*int): call-derived sizes can exceed INT_MAX before the
// legal-range clamp. Callers must clamp to the int legal range via clampTarget
// before narrowing.
static std::int64_t roundBB(double v, int bb) {
  return std::llround(v / static_cast<double>(bb)) * static_cast<std::int64_t>(bb);
}

// ---------- helpers ----------
static std::string rfiBucket(const std::string& pos, int n) {
  if (n <= 3)
    return "BTN";
  if (pos == "MP")
    return "HJ";
  if (pos == "UTG")
    return n >= 8 ? "UTG" : "HJ";
  if (pos == "BB")
    return "BB";
  return pos;
}
static std::string openerBucket(const std::string& pos, int n) {
  if (n <= 3)
    return "BTN";
  if (pos == "UTG" || pos == "MP")
    return n <= 6 ? "HJ" : "EP";
  return pos;
}
// Open bet/lead: total = call + round-BB of frac*(pot + call), clamped to
// legal. All chip arithmetic is widened to int64 before adding: pot and call
// each fit int, but their sum can overflow a signed int. The clamped result
// always lies inside the int legal range, so the narrowing cast is safe.
static int targetTotal(const Legal& L, int pot, double frac, int bb) {
  const std::int64_t open = static_cast<std::int64_t>(pot) + L.call;
  const std::int64_t rounded =
      static_cast<std::int64_t>(std::llround(frac * static_cast<double>(open) / bb)) * bb;
  const std::int64_t target = static_cast<std::int64_t>(L.call) + rounded;
  const std::int64_t low = std::max<std::int64_t>(1, L.raiseMin);
  const std::int64_t high = std::max<std::int64_t>(1, L.raiseMax);
  return static_cast<int>(std::clamp(target, low, high));
}

// ---------- preflop ----------
static Decision preflop(const Ctx& c, const std::string& k) {
  const Charts& C = charts();
  const int n = c.playersInHand;
  const Legal& L = c.legal;

  // short-stack jam/fold (<=12bb)
  if (c.effectiveStackBb <= 12 && L.has("raise")) {
    const double pct = preflopPct(k);
    const double thr =
        (c.position == "BTN" || c.position == "CO" || c.position == "SB") ? 0.35 : 0.55;
    if (c.raises == 0 && pct >= thr)
      return {"raise", L.raiseMax, "short jam " + k, -1, -1};
    if (c.raises >= 1 && C.vs4Continue.count(k))
      return {L.has("call") ? "call" : "raise", L.raiseMax, "short premium " + k, -1, -1};
  }

  if (c.raises == 0) {
    const std::string b = rfiBucket(c.position, n);
    if (b == "BB")
      return {L.has("check") ? "check" : "fold", 0, "BB option", -1, -1};
    auto it = C.rfi.find(b);
    if (it != C.rfi.end() && it->second.count(k) && L.has("raise")) {
      // The old path truncated the double to int before BB rounding; for
      // in-range values static_cast<int64_t> truncates identically, while for
      // over-large chip counts the result is clamped to the legal range below
      // instead of overflowing.
      const std::int64_t truncated =
          static_cast<std::int64_t>((2.5 + c.limpers * 1.5) * static_cast<double>(c.bb));
      return {"raise", clampRawLegal(roundBB(static_cast<double>(truncated), c.bb), L),
              "RFI " + b + " " + k, -1, -1};
    }
    if (L.call == 0 && L.has("check"))
      return {"check", 0, "BB option", -1, -1};
    return {"fold", 0, "fold pre " + b + " " + k, -1, -1};
  }

  if (c.raises == 1) {
    const std::string ob = openerBucket(c.openerPosition.empty() ? "HJ" : c.openerPosition, n);
    const VsOpen& v = C.vs.count(ob) ? C.vs.at(ob) : C.vs.at("HJ");
    const double mult = ob == "EP" ? 3.0 : 2.8;  // 3bet target = multiplier of opener total
    if (v.value.count(k)) {
      if (L.has("raise"))
        return {"raise", clampRawLegal(roundBB(mult * static_cast<double>(L.call), c.bb), L),
                "3bet value " + k, -1, -1};
      return {"call", 0, "call strong " + k, -1, -1};
    }
    if (v.bluff.count(k) && L.has("raise"))
      return {"raise", clampRawLegal(roundBB(mult * static_cast<double>(L.call), c.bb), L),
              "3bet bluff " + k, -1, -1};
    if (v.call.count(k))
      return {"call", 0, "flat " + k, -1, -1};
    return {"fold", 0, "fold vs open " + k, -1, -1};
  }

  if (c.raises == 2 && c.heroWasRaiser) {
    if (C.fourValue.count(k) && L.has("raise"))
      return {"raise", L.raiseMax, "4bet value jam " + k, -1, -1};
    if (C.fourBluff.count(k) && L.has("raise"))
      return {"raise", clampRawLegal(roundBB(2.2 * static_cast<double>(L.call), c.bb), L),
              "4bet bluff " + k, -1, -1};
    if (C.vs3Call.count(k))
      return {"call", 0, "vs3 call " + k, -1, -1};
    return {"fold", 0, "fold vs 3bet " + k, -1, -1};
  }

  if (C.vs4Continue.count(k))
    return {L.has("raise") ? "raise" : "call", L.raiseMax, "jam premium " + k, -1, -1};
  return {"fold", 0, "fold vs 4bet " + k, -1, -1};
}

// ---------- postflop ----------
struct BoardTex {
  bool wet, paired, high;
};
static BoardTex texture(const std::vector<int>& b) {
  if (b.size() < 3)
    return {false, false, true};
  int suits[4] = {}, ranks[13] = {};
  for (int c : b) {
    suits[c & 3]++;
    ranks[c >> 2]++;
  }
  int maxSuit = *std::max_element(suits, suits + 4);
  bool paired = false;
  for (int x : ranks)
    if (x >= 2)
      paired = true;
  int hi = 0;
  for (int r = 12; r >= 0; r--)
    if (ranks[r]) {
      hi = r;
      break;
    }
  bool wet = maxSuit >= 2;
  std::vector<int> rr;
  for (int r = 0; r < 13; r++)
    if (ranks[r])
      rr.push_back(r);
  int close = 0;
  for (size_t i = 1; i < rr.size(); i++)
    if (rr[i] - rr[i - 1] <= 2)
      close++;
  if (close >= 2)
    wet = true;
  return {wet, paired, hi >= 10};
}

static Decision postflop(const Ctx& c) {
  const Legal& L = c.legal;
  const int bb = c.bb;
  const bool multi = c.playersInHand >= 3;

  const std::array<int, 2> hole{cardId(c.hole[0]), cardId(c.hole[1])};
  std::vector<int> board;
  for (const auto& s : c.board)
    board.push_back(cardId(s));
  int all[7] = {hole[0], hole[1]};
  int nAll = 2;
  for (int x : board)
    all[nAll++] = x;
  const int cat = evaluate(all, nAll).cat;
  const BoardTex tex = texture(board);

  // equity vs opponent action range
  const int nOpp = c.playersInHand - 1;
  EquityOpts eo;
  uint64_t baseSeed =
      c.seed ? c.seed
             : (uint64_t)(std::hash<std::string>{}(c.handId) ^ (uint64_t)c.revision * 2654435761u);
  eo.seed = baseSeed;
  eo.iterations = L.call > 0 ? 6000 : 3000;
  if (!c.opponentPcts.empty())
    eo.minPct = c.opponentPcts;
  else
    eo.minPct = std::vector<double>(nOpp, L.call > 0 ? 0.55 : 0.25);
  eo.postflopContinue = true;
  const double eq = equityVsAll(hole, board, eo).equity;

  XorShift64 rng(baseSeed ^ 0xABCDEF);
  const double bluffScale = c.style == "station-hunter" ? 0.35 : c.style == "lag" ? 1.4 : 1.0;
  const double valueThresh = c.style == "station-hunter" ? 0.52 : 0.58;
  // Widen before adding: pot and call each fit int, but their signed sum can
  // overflow (UB). Both operands are exactly representable as double, and any
  // exact integer sum here is below 2^53, so the widened double sum is
  // bit-for-bit the same value for every non-overflow input.
  const double potPlusCall = static_cast<double>(c.pot) + static_cast<double>(L.call);
  const double mdf = potPlusCall > 0.0 ? static_cast<double>(c.pot) / potPlusCall : 1.0;

  // draw classification
  bool anyFd = false, nutFd = false, oesd = false;
  {
    int suitCount[4] = {};
    for (int i = 0; i < nAll; i++)
      suitCount[all[i] & 3]++;
    for (int s = 0; s < 4; s++)
      if (suitCount[s] >= 4) {
        anyFd = true;
        if ((hole[0] & 3) == s && (hole[1] & 3) == s)
          nutFd = true;
      }
    // open-ended: 8+ straight outs approximated by isPotentialDraw with 4-flush excluded
    oesd = !anyFd && isPotentialDraw(all, nAll);
  }
  const bool nutDraw = nutFd || (anyFd && oesd) || (oesd && cat >= 2);
  auto bluffShare = [](double x) { return x / (1 + 2 * x); };

  // ---------- facing a bet ----------
  if (L.call > 0) {
    if (cat >= 4 && L.has("raise")) {
      double frac = (cat >= 5 || tex.wet) ? 0.75 : 0.55;
      return {"raise", targetTotal(L, c.pot, frac, bb), "value raise cat" + std::to_string(cat), eq,
              mdf};
    }
    // strong one pair heads-up (TPTK / overpair ~ eq>=0.68)
    if (cat == 2 && !multi && L.has("raise") && eq >= 0.68)
      return {"raise", targetTotal(L, c.pot, tex.wet ? 0.7 : 0.5, bb), "value raise strong pair",
              eq, mdf};
    // nut/combo draw semibluff on flop/turn
    if (c.street != "river" && L.has("raise") && (nutFd || (anyFd && oesd)) &&
        (eq >= 0.30 || rng.unit() < 0.5 * bluffScale))
      return {"raise", targetTotal(L, c.pot, 0.6, bb), "semibluff nut draw", eq, mdf};
    // call by equity vs price (river draw equity is 0 by MC). Require an edge
    // margin for no-pair hands so Monte Carlo noise doesn't justify bluffcatch.
    double edge = cat >= 2 ? 0.0 : 0.05;
    if (eq >= L.potOdds + edge) {
      if (L.has("call"))
        return {"call", 0, "call eq" + std::to_string((int)(eq * 100)), eq, mdf};
      if (L.has("check"))
        return {"check", 0, "check eq", eq, mdf};
    }
    // MDF-leaning mixed defense with a made hand on dry boards
    if (cat >= 2 && !tex.wet && eq >= L.potOdds * 0.85 &&
        rng.unit() < std::min(1.0, mdf * bluffScale * 0.5))
      return {"call", 0, "MDF defend pair", eq, mdf};
    return {
        "fold", 0,
        "fold eq" + std::to_string((int)(eq * 100)) + "<" + std::to_string((int)(L.potOdds * 100)),
        eq, mdf};
  }

  // ---------- no bet ahead ----------
  if (cat >= 4 && L.has("bet"))
    return {"bet",
            targetTotal(L, c.pot,
                        cat >= 5  ? 0.7
                        : tex.wet ? 0.7
                                  : 0.5,
                        bb),
            "value bet cat" + std::to_string(cat), eq, mdf};

  if (c.street != "river" && L.has("bet") && (anyFd || oesd) && (!multi || nutDraw)) {
    double x = tex.wet ? 0.6 : 0.5;
    double share = bluffShare(x) * bluffScale;
    if (nutDraw)
      share = std::min(0.85, share + 0.35);
    if (rng.unit() < share)
      return {"bet", targetTotal(L, c.pot, x, bb),
              nutFd   ? "semibluff nutFD"
              : anyFd ? "semibluff FD"
                      : "semibluff OESD",
              eq, mdf};
  }

  if (cat == 2 && L.has("bet") && eq >= valueThresh && (!multi || eq >= 0.66)) {
    double frac = c.style == "station-hunter" ? 0.5 : 0.4;
    if (rng.unit() < (c.style == "station-hunter" ? 0.85 : 0.6))
      return {"bet", targetTotal(L, c.pot, tex.wet ? 0.55 : frac, bb), "thin value pair", eq, mdf};
  }

  // air / no SDV: balanced c-bet only as the preflop aggressor; multiway nearly give up
  if (cat <= 1 && c.street != "river" && L.has("bet") && c.heroPreflopAggressor &&
      (!multi || !tex.wet)) {
    double base = c.style == "lag" ? 0.7 : c.style == "station-hunter" ? 0.3 : 0.55;
    double freq = (tex.wet ? base * 0.45 : base) * (multi ? 0.4 : 1.0);
    double x = tex.wet ? 0.5 : 0.33;
    if (rng.unit() < freq * bluffScale * 0.8)
      return {"bet", targetTotal(L, c.pot, x, bb), "c-bet air", eq, mdf};
  }

  if (L.has("check"))
    return {"check", 0, "check SDV/giveup", eq, mdf};
  return {"fold", 0, "check-unavailable fold", eq, mdf};
}

// ---------- river equilibrium (heads-up, one bet/one raise) ----------
// Returns an equilibrium action for hero's exact combo plus the solver source
// that produced it (exact LP vs bounded DCFR), or an absent optional when the
// spot is outside the solver model / the chosen action is not legal, in which
// case the CALL SITE tags the heuristic baseline. The backend hint is the
// policy-level test seam: allow_exact=false forces the bounded-CFR backend
// deterministically; production routing passes the default.
static std::optional<SourcedDecision> riverAnswer(const Ctx& c, const std::array<int, 2>& hole,
                                                  RiverBackendHint backend) {
  if (c.playersInHand != 2 || c.board.size() != 5)
    return std::nullopt;
  // node + which solver side hero occupies, derived from the public line
  gto::RiverNode node;
  int heroSide;  // 0 = solver first actor, 1 = responder
  if (c.riverLine == "") {
    node = gto::RiverNode::kRoot;
    heroSide = 0;
  } else if (c.riverLine == "cb") {
    node = gto::RiverNode::kFaceLead;
    heroSide = 0;
  } else if (c.riverLine == "br") {
    node = gto::RiverNode::kFaceRaiseI;
    heroSide = 0;
  } else if (c.riverLine == "c") {
    node = gto::RiverNode::kAfterC;
    heroSide = 1;
  } else if (c.riverLine == "b") {
    node = gto::RiverNode::kFaceBet;
    heroSide = 1;
  } else if (c.riverLine == "cbr") {
    node = gto::RiverNode::kFaceRaiseO;
    heroSide = 1;
  } else
    return std::nullopt;

  const Legal& L = c.legal;
  std::vector<int> board;
  for (const auto& s : c.board)
    board.push_back(cardId(s));
  const int combo = comboIndex(hole[0], hole[1]);

  // action-line-tracked, differentiated weighted ranges for both sides
  constexpr int kCap = 36;
  gto::TrackedRangesOptions tro;
  tro.cap = kCap;
  tro.preflop_raises = c.raises;
  tro.hero_was_aggressor = c.heroPreflopAggressor;
  gto::TrackedRanges ranges = gto::TrackRiverRanges(board, combo, /*hero_is_ip=*/heroSide == 0,
                                                    c.flopLine, c.turnLine, tro);

  gto::RiverSolveOptions opt;
  opt.max_combos_per_side = kCap;
  opt.time_budget_s = 1.5;
  opt.cfr_iterations = 100000;
  opt.allow_exact = backend.allow_exact;
  auto sol = gto::SolveRiver(board, c.pot, (float)c.riverBetFrac, (float)c.riverRaiseFrac,
                             ranges.ip, ranges.oop, opt);
  if (!sol->result().ok)
    return std::nullopt;
  std::array<double, 3> p{};
  if (!sol->ProbsFor(combo, node, p.data()))
    return std::nullopt;

  const uint64_t seed =
      c.seed ? c.seed
             : (uint64_t)(std::hash<std::string>{}(c.handId) ^ (uint64_t)c.revision * 2654435761u);
  const int ai = sol->ChooseAction(combo, node, seed);
  const int na = gto::RiverSolution::NumActions(node);

  // Widen before adding: pot and call each fit int, but their signed sum can
  // overflow (UB). Both operands are exactly representable as double, and any
  // exact integer sum here is below 2^53, so the widened double sum is
  // bit-for-bit the same value for every non-overflow input.
  const double potPlusCall = static_cast<double>(c.pot) + static_cast<double>(L.call);
  const double mdf = potPlusCall > 0.0 ? static_cast<double>(c.pot) / potPlusCall : 1.0;
  const char* tag = sol->result().exact ? "gto" : "gto-cfr";

  // translate action index -> protocol action, with hard legality validation
  auto checkToken = [&]() -> std::string {
    if (L.has("check"))
      return "check";
    if (L.call == 0 && L.has("call"))
      return "call";
    return "";
  };
  auto callToken = [&]() -> std::string {
    if (L.has("call"))
      return "call";
    if (L.has("check"))
      return "check";
    return "";
  };
  // Open bet/lead: total = frac * (pot + call), clamped to legal.
  auto raiseToken = [&](double frac) -> std::pair<std::string, int> {
    if (!L.has("raise") && !L.has("bet"))
      return {"", 0};
    int amount = targetTotal(L, c.pot, frac, c.bb);
    if (amount < std::max(1, L.raiseMin) || amount > std::max(1, L.raiseMax))
      return {"", 0};
    return {L.has("bet") ? "bet" : "raise", amount};
  };
  // Re-raise: match the solver geometry R = rf*(pot + 2B), i.e. the total
  // chips we commit after calling the bet B, rounded to BB and clamped legal.
  auto reraiseto = [&]() -> std::pair<std::string, int> {
    if (!L.has("raise"))
      return {"", 0};
    // Widen the pot + 2*call geometry to int64 before adding or doubling;
    // the rounded value is clamped back into the int legal range.
    const std::int64_t geometry =
        static_cast<std::int64_t>(c.pot) + 2 * static_cast<std::int64_t>(L.call);
    const std::int64_t raw = static_cast<std::int64_t>(std::llround(
                                 c.riverRaiseFrac * static_cast<double>(geometry) / c.bb)) *
                             c.bb;
    const std::int64_t low = std::max<std::int64_t>(1, L.raiseMin);
    const std::int64_t high = std::max<std::int64_t>(1, L.raiseMax);
    const int amount = static_cast<int>(std::clamp(raw, low, high));
    return {"raise", amount};
  };

  std::string act;
  int amount = 0;
  if (na == 2 && (node == gto::RiverNode::kRoot || node == gto::RiverNode::kAfterC)) {
    if (ai == 0)
      act = checkToken();
    else {
      auto t = raiseToken(c.riverBetFrac);
      act = t.first;
      amount = t.second;
    }
  } else if (na == 3) {
    if (ai == 0)
      act = L.has("fold") ? "fold" : "";
    else if (ai == 1)
      act = callToken();
    else {
      auto t = reraiseto();
      act = t.first;
      amount = t.second;
    }
  } else {  // face a raise: fold / call
    if (ai == 0)
      act = L.has("fold") ? "fold" : "";
    else
      act = callToken();
  }
  if (act.empty())
    return std::nullopt;  // let the heuristic stay legal
  Decision d{act, amount, std::string(tag) + " " + c.riverLine, -1.0, mdf};
  // Tag from the SAME exact read that built the reason tag, once.
  return SourcedDecision{std::move(d),
                         sol->result().exact ? DecisionSource::RiverLp : DecisionSource::RiverDcfr};
}

SourcedDecision evaluatePolicySourced(const Ctx& c, RiverBackendHint backend) {
  if (c.hole.size() != 2)
    // Defensive pre-dispatch fold. Unreachable on a validated v1 request
    // (the validator requires exactly two hole cards); present on the v0/JSON
    // path, which keeps its historical classification.
    return {{"fold", 0, "no hole cards", -1, -1}, DecisionSource::PostflopHeuristic};
  const std::string k = key169(cardId(c.hole[0]), cardId(c.hole[1]));
  if (c.street == "preflop")
    // Every exit of preflop() — including its four chart folds — is a chart
    // decision: the branch that selected it is the chart.
    return {preflop(c, k), DecisionSource::PreflopChart};
  if (c.street == "river" && c.riverGtoOn) {
    std::array<int, 2> hole{cardId(c.hole[0]), cardId(c.hole[1])};
    if (auto a = riverAnswer(c, hole, backend))
      return std::move(*a);
    // All riverGto fall-throughs (ineligible spot, unknown line, solver not
    // ok, combo absent, illegal translated action) use the heuristic.
  }
  return {postflop(c), DecisionSource::PostflopHeuristic};
}

Decision evaluatePolicy(const Ctx& c) {
  return evaluatePolicySourced(c, RiverBackendHint{}).decision;
}

}  // namespace bs
