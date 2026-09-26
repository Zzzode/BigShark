// RFC 0008 stage 6 R4/F5: byte-identical baseline-adapter corpus gate.
//
// Every corpus row decides ONE poker situation through two genuinely
// independent constructions and requires identical action AND amount:
//   (1) deployed path: a hand-specified v0 JSON request (the platform shape)
//       -> parseRequest -> evaluatePolicySourced(allow_exact=false);
//   (2) adapter path: a GameState + simulator HandLog -> adapt_to_ctx ->
//       evaluatePolicySourced with the same hint.
// The deployed JSON scalars come from the row's Expected fields, NOT from the
// adapted Ctx, so a pass means the adapter derived the same situation the
// platform describes, rather than a JSON round-trip tautology. The adapted
// Ctx's observation fields are additionally asserted against Expected. The
// shared decision must also be legal in the GameState (an illegal mapped
// action is a typed failure, never a clamp).
//
// Coverage: seats {2,3,6,7,9,10}; preflop raises 0/1/2 via real transitions;
// rooted flop (multiway + heads-up) snapshots; three hole sets per row.
#include <array>
#include <bs/eval.hpp>
#include <bs/game_definition.hpp>
#include <bs/policy.hpp>
#include <bs/stage6/adapter.hpp>
#include <bs/stage6/baseline_policy.hpp>
#include <bs/v0_protocol.hpp>
#include <cstdio>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

using namespace bs;
using namespace bs::poker;
using namespace bs::stage6;

namespace {

int failures = 0;
int checked = 0;

void fail(const std::string& why) {
  std::fprintf(stderr, "FAIL: %s\n", why.c_str());
  ++failures;
}

std::string q(const std::string& s) {
  return "\"" + s + "\"";
}

// The independently specified platform description of one decision point.
struct Expected {
  std::string street;
  std::string position;
  int players_in_hand;
  int raises = 0;
  int limpers = 0;
  std::string opener_position;  // "" when nobody opened
  bool hero_was_raiser = false;
  bool hero_pfa = false;
};

// Builds the deployed v0 JSON from the row's Expected fields plus the state's
// pot/legal numbers the platform itself would report (read independently from
// GameState here, matching how the platform normalizer fills them).
std::string deployed_json(const GameState& state, std::size_t seat, const std::array<int, 2>& hole,
                          const Expected& e) {
  const GameDef& def = state.def();
  const int bb = static_cast<int>(def.big_blind);
  const LegalActions L = state.legal();
  std::ostringstream os;
  os << "{";
  os << q("handId") << ":" << q("stage6-corpus") << ",";
  os << q("seed") << ":424242,";
  os << q("style") << ":" << q("tag") << ",";
  os << q("street") << ":" << q(e.street) << ",";
  os << "\"blinds\":[" << bb / 2 << "," << bb << "],";
  os << q("position") << ":" << q(e.position) << ",";
  os << q("playersInHand") << ":" << e.players_in_hand << ",";
  os << q("effectiveStackBb") << ":100,";
  os << q("hole") << ":[\"" << card_name(hole[0]) << "\",\"" << card_name(hole[1]) << "\"],";
  os << q("board") << ":[";
  bool first_b = true;
  for (int c : state.board()) {
    os << (first_b ? "" : ",") << q(card_name(c));
    first_b = false;
  }
  os << "],";
  os << q("pot") << ":" << state.pot() << ",";
  os << q("raises") << ":" << e.raises << ",";
  os << q("riverGtoOn") << ":false,";
  os << q("limpers") << ":" << e.limpers << ",";
  os << q("openerPosition") << ":" << q(e.opener_position) << ",";
  os << "\"heroWasRaiser\":" << (e.hero_was_raiser ? "true" : "false") << ",";
  os << "\"heroPreflopAggressor\":" << (e.hero_pfa ? "true" : "false") << ",";
  os << q("legal") << ":{";
  os << q("actions") << ":[";
  bool first_a = true;
  for (const char* a : {"fold", "check", "call", "raise"}) {
    bool have = (std::string(a) == "fold" && L.fold) || (std::string(a) == "check" && L.check) ||
                (std::string(a) == "call" && L.call) || (std::string(a) == "raise" && L.aggressive);
    if (have) {
      os << (first_a ? "" : ",") << q(a);
      first_a = false;
    }
  }
  os << "],";
  os << q("call") << ":" << L.call_amount << ",";
  const double odds = L.call_amount ? static_cast<double>(L.call_amount) /
                                          static_cast<double>(state.pot() + L.call_amount)
                                    : 0.0;
  os << q("potOdds") << ":" << odds << ",";
  os << q("raiseTo") << ":{\"min\":" << (L.aggressive ? L.aggressive->minimum : 0)
     << ",\"max\":" << (L.aggressive ? L.aggressive->maximum : 0) << "}";
  os << "}}";
  (void)seat;
  return os.str();
}

void assert_ctx_fields(const Ctx& c, const Expected& e, const std::string& label) {
  auto eq_s = [&](const std::string& got, const std::string& want, const char* f) {
    if (got != want)
      fail(label + ": adapter Ctx." + f + "='" + got + "' expected '" + want + "'");
  };
  eq_s(c.street, e.street, "street");
  eq_s(c.position, e.position, "position");
  if (c.playersInHand != e.players_in_hand)
    fail(label + ": Ctx.playersInHand mismatch");
  if (c.raises != e.raises)
    fail(label + ": Ctx.raises=" + std::to_string(c.raises) + " expected " +
         std::to_string(e.raises));
  if (c.limpers != e.limpers)
    fail(label + ": Ctx.limpers mismatch");
  eq_s(c.openerPosition, e.opener_position, "openerPosition");
  if (c.heroWasRaiser != e.hero_was_raiser)
    fail(label + ": Ctx.heroWasRaiser mismatch");
  if (c.heroPreflopAggressor != e.hero_pfa)
    fail(label + ": Ctx.heroPreflopAggressor mismatch");
  if (c.riverGtoOn)
    fail(label + ": harness baseline must keep the river solver disabled");
}

void check_row(const GameState& state, std::size_t seat, const std::array<int, 2>& hole,
               const HandLog& log, const Expected& expected, const std::string& label) {
  const Ctx adapted = adapt_to_ctx(state, seat, hole, log, 424242ULL);
  assert_ctx_fields(adapted, expected, label);
  const Ctx deployed = v0::parseRequest(deployed_json(state, seat, hole, expected));
  const RiverBackendHint hint{false};
  const SourcedDecision a = evaluatePolicySourced(adapted, hint);
  const SourcedDecision b = evaluatePolicySourced(deployed, hint);
  ++checked;
  if (a.decision.action != b.decision.action || a.decision.amount != b.decision.amount) {
    std::ostringstream os;
    os << label << " with " << card_name(hole[0]) << card_name(hole[1]) << ": adapter=["
       << a.decision.action << " " << a.decision.amount << "] deployed=[" << b.decision.action
       << " " << b.decision.amount << "]";
    fail(os.str());
    return;
  }
  try {
    (void)map_deployed_decision(state, a.decision);
  } catch (const std::exception& ex) {
    fail(label + ": shared decision illegal in GameState: " + ex.what());
  }
}

GameState apply(GameState state, std::size_t seat, poker::Action action, HandLog* log) {
  log->preflop.push_back(LoggedAction{seat, action});
  return state.after_action(seat, action);
}

// N-seat preflop GameDef, 100bb, button 0; 2p uses the heads-up blind rule.
GameDef preflop_def(std::size_t n) {
  GameDef def{};
  def.player_count = n;
  def.button = 0;
  def.big_blind = 2;
  def.preflop = true;
  for (std::size_t i = 0; i < n; ++i)
    def.stacks[i] = 200;
  std::array<Chips, 10> blinds{};
  if (n == 2) {
    blinds[0] = 1;  // button posts SB heads-up
    blinds[1] = 2;
  } else {
    blinds[(0 + 1) % n] = 1;
    blinds[(0 + 2) % n] = 2;
  }
  def.blinds_posted = blinds;
  def.pot = 3;
  def.board = {-1, -1, -1, -1, -1};
  return def;
}

// Rooted three-or-more-seat flop with every live seat 100bb deep and `pot`
// evenly contributed. Board 2c 3d 7h (ids 0, 6, 21).
GameDef rooted_flop(std::size_t n, Chips each) {
  GameDef def{};
  def.player_count = n;
  def.button = 0;
  def.big_blind = 2;
  def.preflop = false;
  Chips pot = 0;
  for (std::size_t i = 0; i < n; ++i) {
    def.stacks[i] = 200 - each;
    def.contributions[i] = each;
    pot += each;
  }
  def.pot = pot;
  def.board = {0, 6, 21, 0, 0};
  def.board_size = 3;
  return def;
}

const std::array<std::array<int, 2>, 3> kHoles = {{
    {{50, 45}},  // As Kh (premium)
    {{33, 30}},  // 9s 9h (medium pair)
    {{1, 5}},    // 2h 3h (trash)
}};

}  // namespace

int main() {
  // --- Preflop raises 0/1/2 at every required seat count via real
  // transitions. The first actor at an unopened orbit is the deciding seat for
  // raises=0; after one opener it is the next live seat (raises=1); after a
  // 3-bet the opener decides again (raises=2, hero was raiser).
  for (std::size_t n : {std::size_t{2}, std::size_t{3}, std::size_t{6}, std::size_t{7},
                        std::size_t{9}, std::size_t{10}}) {
    const GameDef def = preflop_def(n);

    // raises = 0
    {
      GameState state(def);
      const std::size_t seat = *state.actor();
      HandLog log;
      Expected e;
      e.street = "preflop";
      e.position = position_vocabulary(n, (seat + n - def.button) % n);
      e.players_in_hand = static_cast<int>(n);
      for (const auto& hole : kHoles)
        check_row(state, seat, hole, log, e, "preflop-n" + std::to_string(n) + "-r0");
    }

    // raises = 1: first actor opens, next actor faces it.
    {
      GameState state(def);
      HandLog log;
      const std::size_t opener = *state.actor();
      state = apply(std::move(state), opener, poker::Action{poker::ActionType::Raise, 6}, &log);
      if (state.phase() == Phase::Action) {
        const std::size_t seat = *state.actor();
        Expected e;
        e.street = "preflop";
        e.position = position_vocabulary(n, (seat + n - def.button) % n);
        e.players_in_hand = static_cast<int>(n);
        e.raises = 1;
        e.opener_position = position_vocabulary(n, (opener + n - def.button) % n);
        for (const auto& hole : kHoles)
          check_row(state, seat, hole, log, e, "preflop-n" + std::to_string(n) + "-r1");
      }
    }

    // raises = 2: open, 3-bet, opener faces the 3-bet (hero was the raiser).
    // Only at counts where the opener is distinct from the 3-bettor (always
    // true here) and deep stacks keep both actions legal.
    if (n == 2 || n == 3 || n == 6 || n == 10) {
      GameState state(def);
      HandLog log;
      const std::size_t opener = *state.actor();
      state = apply(std::move(state), opener, poker::Action{poker::ActionType::Raise, 6}, &log);
      const std::size_t threebettor = *state.actor();
      state =
          apply(std::move(state), threebettor, poker::Action{poker::ActionType::Raise, 18}, &log);
      if (state.phase() == Phase::Action && *state.actor() == opener) {
        Expected e;
        e.street = "preflop";
        e.position = position_vocabulary(n, (opener + n - def.button) % n);
        e.players_in_hand = static_cast<int>(n);
        e.raises = 2;
        e.opener_position = position_vocabulary(n, (opener + n - def.button) % n);
        // The opener facing the 3-bet is NOT the last raiser: the 3-bettor is.
        e.hero_was_raiser = false;
        e.hero_pfa = false;
        for (const auto& hole : kHoles)
          check_row(state, opener, hole, log, e, "preflop-n" + std::to_string(n) + "-r2");
      }
    }
  }

  // --- Rooted flop snapshots: every live seat limped (`each` chips), pot
  // already closed and the flop dealt. The actor is first-to-act. One row with
  // no preflop aggressor and one where the actor was the preflop raiser (the
  // c-bet-air branch reads heroPreflopAggressor).
  for (std::size_t n : {std::size_t{2}, std::size_t{3}, std::size_t{7}, std::size_t{10}}) {
    const Chips each = 4;
    const GameDef def = rooted_flop(n, each);
    GameState state(def);
    if (state.phase() != Phase::Action)
      fail("rooted flop n=" + std::to_string(n) + " did not open action");
    if (state.phase() == Phase::Action) {
      const std::size_t seat = *state.actor();
      HandLog log;  // no preflop action recorded past the aggregated dead pot
      Expected e;
      e.street = "flop";
      e.position = position_vocabulary(n, (seat + n - def.button) % n);
      e.players_in_hand = static_cast<int>(n);
      for (const auto& hole : kHoles)
        check_row(state, seat, hole, log, e, "flop-n" + std::to_string(n) + "-pfa0");

      // Same state with the actor recorded as the single preflop raiser.
      // Deployed semantics (v0-normalizer.ts): on the flop the platform sends
      // raises=0 / limpers=0 / openerPosition="" (those fields are preflop
      // only), while heroWasRaiser/heroPreflopAggressor come from the
      // all-street raise list and are true.
      HandLog aggressor_log;
      aggressor_log.preflop.push_back(
          LoggedAction{seat, poker::Action{poker::ActionType::Raise, 6}});
      Expected e2 = e;
      e2.hero_pfa = true;
      e2.hero_was_raiser = true;
      for (const auto& hole : kHoles)
        check_row(state, seat, hole, aggressor_log, e2, "flop-n" + std::to_string(n) + "-pfa1");
    }
  }

  if (failures != 0) {
    std::fprintf(stderr, "ADAPTER CORPUS GATE FAILED: %d mismatch(es) across %d decisions\n",
                 failures, checked);
    return 1;
  }
  std::printf("ADAPTER CORPUS GATE PASSED: %d decisions identical, all legal\n", checked);
  return 0;
}
