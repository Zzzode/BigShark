// Unit tests for the RFC 0008 stage 3 L2 abstraction component. This binary
// links bigshark_abstraction, which itself depends on only the rules layer, so
// the component is proven usable and testable without any solver, storage, or
// IO dependency (a stray symbol out of L2 would be an unresolved reference).
#include <algorithm>
#include <array>
#include <bs/abstraction.hpp>
#include <bs/eval.hpp>
#include <bs/range.hpp>
#include <cstdint>
#include <cstdio>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

using namespace bs;
using namespace bs::abstraction;

#define CHECK(cond)                                                         \
  do {                                                                      \
    if (!(cond)) {                                                          \
      std::printf("CHECK failed: %s (%s:%d)\n", #cond, __FILE__, __LINE__); \
      return 1;                                                             \
    }                                                                       \
  } while (0)

namespace {

int card(const char* name) {
  return cardId(std::string(name));
}

poker::LegalActions aggressive_menu(poker::ActionType type, poker::Chips minimum,
                                    poker::Chips maximum) {
  poker::LegalActions legal;
  const bool facing_wager = type == poker::ActionType::Raise;
  legal.fold = facing_wager;
  legal.check = !facing_wager;
  legal.call = facing_wager;
  legal.call_amount = facing_wager ? minimum : 0;
  legal.aggressive = poker::TargetRange{type, minimum, maximum, minimum == maximum};
  return legal;
}

// Hand-derived expectations for the menu rule, independent of any state type.
int test_action_menu_math() {
  // A bet facing no wager: pot 6 after a zero call, default flop bet fractions
  // 1/3, 3/4, 3/2 -> ceil targets 2, 5, 9, plus the legal minimum 1 and the
  // 100 cap.
  {
    poker::LegalActions legal = aggressive_menu(poker::ActionType::Bet, 1, 100);
    MenuContext ctx;
    ctx.street = poker::Street::Flop;
    ctx.pot = 6;
    ctx.opponent_stack = 100;
    const auto menu = build_action_menu(legal, default_size_schedule()[0], ctx);
    const std::vector<poker::Action> want{
        {poker::ActionType::Check},  {poker::ActionType::Bet, 1}, {poker::ActionType::Bet, 2},
        {poker::ActionType::Bet, 5}, {poker::ActionType::Bet, 9}, {poker::ActionType::Bet, 100}};
    CHECK(menu == want);
  }
  // A raise facing a wager: base includes what the actor already committed and
  // owes, and the cap is the opponent's matching total. Raise fractions 1/2,1/1
  // of the pot after the call.
  {
    poker::LegalActions legal = aggressive_menu(poker::ActionType::Raise, 6, 100);
    legal.call_amount = 3;  // owed to call, distinct from the 6 raise minimum
    MenuContext ctx;
    ctx.street = poker::Street::Flop;
    ctx.pot = 6;
    ctx.actor_committed = 3;
    ctx.opponent_committed = 6;
    ctx.opponent_stack = 94;  // effective opponent total 6 + 94 = 100
    // base 6; pot after call 9; raises ceil 4.5=5, 9 -> targets 11, 15 on top
    // of base; with min 6 and cap 100 -> {6, 11, 15, 100}.
    const auto menu = build_action_menu(legal, default_size_schedule()[0], ctx);
    const std::vector<poker::Action> want{
        {poker::ActionType::Fold},      {poker::ActionType::Call},
        {poker::ActionType::Raise, 6},  {poker::ActionType::Raise, 11},
        {poker::ActionType::Raise, 15}, {poker::ActionType::Raise, 100}};
    CHECK(menu == want);
  }
  // No aggressive legal: only the passive actions survive and ordering is
  // fold, check/call.
  {
    poker::LegalActions legal;
    legal.fold = true;
    legal.call = true;
    legal.call_amount = 4;
    MenuContext ctx;
    const auto menu = build_action_menu(legal, default_size_schedule()[0], ctx);
    CHECK(menu.size() == 2);
    CHECK(menu[0].type == poker::ActionType::Fold);
    CHECK(menu[1].type == poker::ActionType::Call);
  }
  // An all-in-only interval collapses to a single aggressive target; fraction
  // targets clamp to it and de-duplicate.
  {
    poker::LegalActions legal = aggressive_menu(poker::ActionType::Bet, 2, 2);
    MenuContext ctx;
    ctx.street = poker::Street::River;
    ctx.pot = 100;
    ctx.opponent_stack = 2;
    const auto menu = build_action_menu(legal, default_size_schedule()[2], ctx);
    CHECK(menu.front().type == poker::ActionType::Check);
    CHECK(menu.size() == 2);
    CHECK(menu.back() == poker::Action(poker::ActionType::Bet, 2));
  }
  // The cap is the opponent's MATCHING total, which can sit below the legal
  // maximum when the opponent is short: the menu must not offer a target the
  // opponent can cover only by the later unmatched refund. Actor has committed
  // 3, owes 3 more to call; the opponent has committed 6 with only 4 behind, so
  // the effective cap is 10 even though the legal interval maximum is 100.
  {
    poker::LegalActions legal = aggressive_menu(poker::ActionType::Raise, 6, 100);
    legal.call_amount = 3;
    MenuContext ctx;
    ctx.street = poker::Street::Flop;
    ctx.pot = 6;
    ctx.actor_committed = 3;
    ctx.opponent_committed = 6;
    ctx.opponent_stack = 4;  // matching total 10 < legal maximum 100
    StreetSizes only_pot = default_size_schedule()[0];
    only_pot.raises = {{100, 1}};  // fractions all clamp to the effective cap
    const auto menu = build_action_menu(legal, only_pot, ctx);
    const std::vector<poker::Action> want{{poker::ActionType::Fold},
                                          {poker::ActionType::Call},
                                          {poker::ActionType::Raise, 6},
                                          {poker::ActionType::Raise, 10}};
    CHECK(menu == want);  // no 100 target: the short opponent cannot match it
  }
  // The preflop opener must use the Preflop=3 blind-relative fractions, not a
  // postflop street's. SB-open at stack 20, blinds 1/2: owes 1 to call, the
  // legal raise interval is [4,20], pot after the call is 4. Preflop RAISES
  // 3/2,2/1,3/1 of 4 (ceil 6,8,12) added to base 2 yield interior targets
  // 8,10,14 between the minimum 4 and the all-in 20. These interior targets are
  // what makes the preflop fraction list load-bearing; a shallow all-in-only
  // root clamps them away and could never catch a wrong default.
  {
    poker::LegalActions legal;
    legal.fold = true;
    legal.call = true;
    legal.call_amount = 1;  // SB posted 1, owes 1 more to reach the BB's 2
    legal.aggressive = poker::TargetRange{poker::ActionType::Raise, 4, 20, false};
    MenuContext ctx;
    ctx.street = poker::Street::Preflop;
    ctx.pot = 3;
    ctx.actor_committed = 1;
    ctx.opponent_committed = 2;
    ctx.opponent_stack = 18;  // BB total that can be matched: 2 + 18 = 20
    const auto menu = build_action_menu(legal, default_size_schedule()[3], ctx);
    const std::vector<poker::Action> want{
        {poker::ActionType::Fold},      {poker::ActionType::Call},
        {poker::ActionType::Raise, 4},  {poker::ActionType::Raise, 8},
        {poker::ActionType::Raise, 10}, {poker::ActionType::Raise, 14},
        {poker::ActionType::Raise, 20}};
    CHECK(menu == want);
  }
  return 0;
}

// Fraction validation: non-positive or unreduced fractions are rejected, and a
// product that overflows the chip type is an overflow_error.
int test_fraction_validation() {
  MenuContext ctx;
  ctx.pot = 6;
  ctx.opponent_stack = 100;
  auto rejects = [&](Fraction f) {
    StreetSizes street = default_size_schedule()[0];
    street.bets = {f};
    poker::LegalActions legal = aggressive_menu(poker::ActionType::Bet, 1, 100);
    try {
      (void)build_action_menu(legal, street, ctx);
    } catch (const std::invalid_argument&) {
      return true;
    }
    return false;
  };
  for (Fraction bad : {Fraction{0, 1}, Fraction{1, 0}, Fraction{2, 4}, Fraction{2, 2}})
    CHECK(rejects(bad));
  StreetSizes huge = default_size_schedule()[0];
  huge.bets = {{std::numeric_limits<std::uint64_t>::max(), 1}};
  poker::LegalActions legal = aggressive_menu(poker::ActionType::Bet, 1, 100);
  bool overflowed = false;
  try {
    (void)build_action_menu(legal, huge, ctx);
  } catch (const std::overflow_error&) {
    overflowed = true;
  }
  CHECK(overflowed);
  return 0;
}

// AbstractionId is deterministic, distinguishes a changed parameter, and the
// typed cross-check fails closed with abstraction_mismatch (the RFC 0008
// refusal) rather than silently serving the wrong policy.
int test_identity_and_typed_refusal() {
  const AbstractionId a = identity_action_id();
  const AbstractionId a2 = identity_action_id();
  CHECK(a == a2);
  CHECK(a.digest != 0);
  CHECK(a.name == "rfc0007-pot-fractions");
  CHECK(a.version == 1);
  // Same inputs always hash the same.
  CHECK(a.digest == abstraction_digest(a.name, a.version, a.parameters));
  // Golden identity digest. The equivalence harness hands both menu
  // implementations the SAME schedule, so by construction it cannot detect a
  // wrong shipped DEFAULT; this constant can. Any fraction (including the
  // preflop 3/2,2/1,3/1 opener) drifting silently changes the canonical text
  // and thus the digest.
  CHECK(a.digest == 0x422c245239c7a527ULL);

  // Pin the shipped preflop default explicitly (the entry a flop-rooted game
  // never reads): blind-relative openers, distinct from the postflop default.
  const SizeSchedule shipped = default_size_schedule();
  const StreetSizes& preflop = shipped[static_cast<std::size_t>(poker::Street::Preflop)];
  CHECK((preflop.bets == std::vector<Fraction>{{3, 2}, {2, 1}, {3, 1}}));
  CHECK((preflop.raises == std::vector<Fraction>{{3, 2}, {2, 1}, {3, 1}}));
  // Postflop streets keep their own default, proving the preflop override is
  // scoped to index 3.
  CHECK((shipped[static_cast<std::size_t>(poker::Street::Flop)].bets ==
         std::vector<Fraction>{{1, 3}, {3, 4}, {3, 2}}));

  // Changing one fraction changes the canonical parameters and the digest.
  SizeSchedule changed = default_size_schedule();
  changed[0].bets = {{1, 2}, {3, 4}, {3, 2}};
  const ActionAbstraction other(changed);
  CHECK(other.id() != a);

  require_same_abstraction(a, a2);  // equal: passes
  bool refused = false;
  try {
    require_same_abstraction(a, other.id());
  } catch (const abstraction_mismatch& e) {
    refused = true;
    CHECK(e.requested() == a.to_string());
    CHECK(e.trained() == other.id().to_string());
  }
  CHECK(refused);

  // The two card families have distinct identities.
  CHECK(card_abstraction_id(CardBucketKind::Identity) !=
        card_abstraction_id(CardBucketKind::CategoryTiersV1));

  // Reduced form is part of declared identity: a schedule written with an
  // unreduced fraction (2/4) mints the SAME id as its reduced form (1/2).
  {
    SizeSchedule unreduced = default_size_schedule();
    SizeSchedule reduced = default_size_schedule();
    unreduced[0].bets = {{2, 4}};
    reduced[0].bets = {{1, 2}};
    CHECK(ActionAbstraction(unreduced).id() == ActionAbstraction(reduced).id());
    // ...and a genuinely different fraction still differs.
    SizeSchedule other = default_size_schedule();
    other[0].bets = {{1, 3}};
    CHECK(ActionAbstraction(other).id() != ActionAbstraction(reduced).id());
  }

  // Declaration must fail closed for a non-positive fraction, not hit integer
  // division by zero while reducing gcd(0,0)==0 in the canonical id. This is
  // the same invalid_argument the menu-build path raises, reached earlier (in
  // ActionAbstraction's constructor).
  for (Fraction bad : {Fraction{0, 0}, Fraction{0, 1}, Fraction{1, 0}}) {
    SizeSchedule bad_schedule = default_size_schedule();
    bad_schedule[1].raises = {bad};
    bool rejected = false;
    try {
      (void)ActionAbstraction(bad_schedule).id();
    } catch (const std::invalid_argument&) {
      rejected = true;
    }
    CHECK(rejected);
  }

  // Card bucketing requires a 3/4/5-card board; preflop and over-long boards
  // fail closed rather than reading out of bounds.
  {
    const std::array<int, 2> hole{card("Ah"), card("Ad")};
    bool rejected_empty = false;
    try {
      (void)strength_bucket(hole, {});
    } catch (const std::invalid_argument&) {
      rejected_empty = true;
    }
    CHECK(rejected_empty);
    bool rejected_long = false;
    try {
      (void)strength_bucket(hole, {1, 2, 3, 4, 5, 6});
    } catch (const std::invalid_argument&) {
      rejected_long = true;
    }
    CHECK(rejected_long);
  }
  return 0;
}

// Identity card bucket is the seven-card evaluator score, so it is an exact,
// order-preserving map: hands of different strength get different buckets.
int test_identity_card_bucket() {
  const std::vector<int> board{card("2c"), card("3d"), card("7h"), card("9s"), card("Jd")};
  const std::array<std::array<int, 2>, 3> holes{{
      {{card("Ah"), card("Ad")}},
      {{card("Kh"), card("Kd")}},
      {{card("Qh"), card("Qd")}},
  }};
  std::array<std::uint32_t, 3> buckets{};
  for (std::size_t i = 0; i < 3; ++i) {
    buckets[i] = card_bucket(CardBucketKind::Identity, holes[i], board);
    std::array<int, 7> cards{};
    std::copy(board.begin(), board.end(), cards.begin());
    cards[5] = holes[i][0];
    cards[6] = holes[i][1];
    CHECK(buckets[i] == evaluate(cards.data(), 7).score);
  }
  // Strictly ordered: aces beat kings beat queens.
  CHECK(buckets[0] > buckets[1]);
  CHECK(buckets[1] > buckets[2]);

  // Across the full non-blocked combo bank of one fixed flop the identity
  // bucket equals the evaluator score and takes an exact, pinned number of
  // values. The exact count is asserted (not just printed) so a layout or
  // scoring regression cannot quietly change it.
  const std::vector<int> flop{card("As"), card("Kc"), card("2h")};
  std::set<std::uint32_t> scores;
  long flop_holdings = 0;
  for (int combo = 0; combo < N_COMBOS; ++combo) {
    const auto h = comboCards(combo);
    bool blocked = false;
    for (int pc : flop)
      if (h[0] == pc || h[1] == pc)
        blocked = true;
    if (blocked)
      continue;
    const std::array<int, 2> hole{h[0], h[1]};
    ++flop_holdings;
    const auto s = strength_bucket(hole, flop);
    scores.insert(s);
    CHECK(card_bucket(CardBucketKind::Identity, hole, flop) == s);
  }
  // 1326 combos minus the 150 sharing one of the three board cards.
  CHECK(flop_holdings == 1176);
  CHECK(scores.size() == 91);  // pinned identity-bucket cardinality on this flop
  return 0;
}

// The first lossy family collapses hands within one hand category while still
// separating categories. It must extract the evaluator's category nibble for
// EVERY category 1..9, not merely the four a single rainbow flop happens to
// reach -- otherwise a wrong mask/shift could pass on a narrow board.
int test_lossy_tier_bucket_merges() {
  // One (board, hole) pair per hand category 1..9, built on the river so the
  // made hand is final. Each asserts the exact evaluator category, which is
  // the value CategoryTiersV1 must return: a wrong mask (e.g. & 0x7) or shift
  // fails here.
  struct CaseC {
    std::array<int, 5> board;
    std::array<int, 2> hole;
    int want_cat;
  };
  // board, hole use distinct cards.
  const std::array<CaseC, 9> cases{{
      {{{card("2c"), card("5d"), card("8h"), card("Ts"), card("Kd")}},
       {{card("Qh"), card("6s")}},
       1},  // high card (queen high)
      {{{card("2c"), card("5d"), card("8h"), card("Ts"), card("Kd")}},
       {{card("Qh"), card("Qd")}},
       2},  // one pair (queens; no queen on board)
      {{{card("2c"), card("5d"), card("8h"), card("Ks"), card("Ad")}},
       {{card("5s"), card("Ah")}},
       3},  // two pair (aces and fives)
      {{{card("2c"), card("5d"), card("8h"), card("Ks"), card("Ad")}},
       {{card("Kc"), card("Kh")}},
       4},  // trips (kings)
      {{{card("2c"), card("5d"), card("8h"), card("9s"), card("Kd")}},
       {{card("6h"), card("7c")}},
       5},  // straight (5..9)
      {{{card("2c"), card("5c"), card("8c"), card("Ks"), card("Ad")}},
       {{card("Jc"), card("Tc")}},
       6},  // flush (clubs)
      {{{card("2c"), card("2d"), card("8h"), card("8s"), card("Ad")}},
       {{card("2h"), card("Ac")}},
       7},  // full house (twos full of aces)
      {{{card("2c"), card("2d"), card("8h"), card("Ks"), card("Ad")}},
       {{card("2s"), card("2h")}},
       8},  // four of a kind (deuces)
      {{{card("2c"), card("3c"), card("4c"), card("5c"), card("Kd")}},
       {{card("Ac"), card("Qh")}},
       9},  // straight flush (wheel A-2-3-4-5 clubs)
  }};
  for (const CaseC& c : cases) {
    const std::vector<int> board{c.board.begin(), c.board.end()};
    std::array<int, 7> seven{};
    std::copy(c.board.begin(), c.board.end(), seven.begin());
    seven[5] = c.hole[0];
    seven[6] = c.hole[1];
    const std::uint32_t score = strength_bucket(c.hole, board);
    const int cat = evaluate(seven.data(), 7).cat;
    CHECK(cat == c.want_cat);
    CHECK((score >> 20) == static_cast<std::uint32_t>(c.want_cat));
    CHECK(card_bucket(CardBucketKind::CategoryTiersV1, c.hole, board) ==
          static_cast<std::uint32_t>(c.want_cat));
  }

  // Within a category, distinct made strengths collide: two different high-card
  // holdings on the same board share the tier though their scores differ.
  const std::vector<int> board{card("2c"), card("5d"), card("8h"), card("Ts"), card("Kd")};
  const std::array<int, 2> weak1{card("Qh"), card("6s")};
  const std::array<int, 2> weak2{card("Qc"), card("7s")};
  CHECK(strength_bucket(weak1, board) != strength_bucket(weak2, board));
  CHECK(card_bucket(CardBucketKind::CategoryTiersV1, weak1, board) ==
        card_bucket(CardBucketKind::CategoryTiersV1, weak2, board));

  // Exact structural merge statistic over the non-blocked combos of one board:
  // both the holding count and the identity/tier cardinalities are pinned.
  const std::vector<int> flop{card("As"), card("Kc"), card("2h")};
  long holdings = 0;
  std::set<std::uint32_t> identity_buckets;
  std::set<std::uint32_t> tier_buckets;
  for (int combo = 0; combo < N_COMBOS; ++combo) {
    const auto h = comboCards(combo);
    bool blocked = false;
    for (int cc : flop)
      if (h[0] == cc || h[1] == cc)
        blocked = true;
    if (blocked)
      continue;
    const std::array<int, 2> hole{h[0], h[1]};
    ++holdings;
    identity_buckets.insert(card_bucket(CardBucketKind::Identity, hole, flop));
    tier_buckets.insert(card_bucket(CardBucketKind::CategoryTiersV1, hole, flop));
  }
  CHECK(holdings == 1176);
  CHECK(identity_buckets.size() == 91);
  // On a rainbow unpaired flop with two hole cards only categories 1..4 are
  // reachable (no straight/flush/full house/quads), so the lossy map has four
  // distinct values here; the nine-category witness above is what pins 5..9.
  CHECK(tier_buckets.size() == 4);
  return 0;
}

}  // namespace

int main() {
  struct Case {
    const char* name;
    int (*fn)();
  };
  const std::array<Case, 5> cases{{
      {"action_menu_math", test_action_menu_math},
      {"fraction_validation", test_fraction_validation},
      {"identity_and_typed_refusal", test_identity_and_typed_refusal},
      {"identity_card_bucket", test_identity_card_bucket},
      {"lossy_tier_bucket_merges", test_lossy_tier_bucket_merges},
  }};
  int failures = 0;
  for (const Case& item : cases) {
    std::printf("case %s ...\n", item.name);
    const int result = item.fn();
    std::printf("  %s\n", result == 0 ? "ok" : "FAILED");
    failures += result;
  }
  if (failures != 0) {
    std::printf("test_abstraction FAILED\n");
    return 1;
  }
  std::printf("test_abstraction PASS\n");
  return 0;
}
