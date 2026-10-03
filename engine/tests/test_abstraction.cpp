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
#include <map>
#include <numeric>
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

  // Card bucketing accepts an empty board (preflop, RFC 0007) and rejects
  // over-long boards rather than reading out of bounds.
  {
    const std::array<int, 2> hole{card("Ah"), card("Ad")};
    // Empty board: the two hole cards score as a pair or high card.
    const std::uint32_t preflop_score = strength_bucket(hole, {});
    CHECK(preflop_score != 0);
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

// The 3..10-seat menu caps to the DEEPEST cover over all other live, non-folded
// seats, not the next actor's. A short seat B (cover 10) and a deep seat C
// (cover 100) both face actor A's raise: clamping to B would silently delete the
// legal targets that C can call (11, 15); the multiway rule keeps them.
int test_multiway_cover_cap() {
  // Actor A faces a raise: owes 3 to call, legal raise interval [6, 100],
  // pot 6. The NEXT seat B is short (committed 6 + 4 behind = cover 10); a
  // third seat C is deep (committed 6 + 94 behind = cover 100). Default flop
  // raises 1/2, 1/1 of the pot after the call (9) ceil to 5, 9, added to base
  // 6 -> targets 11, 15. Cover must be 100, so both survive alongside the
  // all-in seed.
  poker::LegalActions legal = aggressive_menu(poker::ActionType::Raise, 6, 100);
  legal.call_amount = 3;
  MultiwayMenuContext ctx;
  ctx.street = poker::Street::Flop;
  ctx.pot = 6;
  ctx.actor_committed = 3;
  ctx.cover = 100;  // max over live opponents {B:10, C:100}
  const auto deep = build_multiway_action_menu(legal, default_size_schedule()[0], ctx);
  const std::vector<poker::Action> want_deep{
      {poker::ActionType::Fold},      {poker::ActionType::Call},
      {poker::ActionType::Raise, 6},  {poker::ActionType::Raise, 11},
      {poker::ActionType::Raise, 15}, {poker::ActionType::Raise, 100}};
  CHECK(deep == want_deep);

  // If the deepest opponent were actually the short seat (cover 10), the same
  // fractions clamp to 10 and the deep targets are gone -- proving the builder
  // honors the supplied cover rather than always taking the legal maximum.
  ctx.cover = 10;
  const auto short_cover = build_multiway_action_menu(legal, default_size_schedule()[0], ctx);
  const std::vector<poker::Action> want_short{{poker::ActionType::Fold},
                                              {poker::ActionType::Call},
                                              {poker::ActionType::Raise, 6},
                                              {poker::ActionType::Raise, 10}};
  CHECK(short_cover == want_short);

  // The single-opponent heads-up menu and the multiway menu agree when the
  // multiway cover equals that one opponent's total (the identity path is
  // unchanged at two seats).
  MenuContext hu_ctx;
  hu_ctx.street = poker::Street::Flop;
  hu_ctx.pot = 6;
  hu_ctx.actor_committed = 3;
  hu_ctx.opponent_committed = 6;
  hu_ctx.opponent_stack = 94;
  CHECK(build_action_menu(legal, default_size_schedule()[0], hu_ctx) == want_deep);
  return 0;
}

// Reduced form is part of declared identity: a schedule constructed with an
// unreduced fraction (2/6 for the flop 1/3 opener) is normalized at declaration
// so the stored schedule is reduced AND mints the identity id. A zero
// component is rejected at construction (not later, inside the trainer).
int test_declared_schedule_is_reduced() {
  SizeSchedule unreduced = default_size_schedule();
  unreduced[0].bets = {{2, 6}, {3, 4}, {3, 2}};  // 2/6 == 1/3
  ActionAbstraction a(unreduced);
  CHECK(a.id() == identity_action_id());
  CHECK((a.schedule()[0].bets == default_size_schedule()[0].bets));
  for (const StreetSizes& street : a.schedule())
    for (const std::vector<Fraction>* group : {&street.bets, &street.raises})
      for (const Fraction& f : *group)
        CHECK(std::gcd(f.numerator, f.denominator) == 1);

  SizeSchedule zero = default_size_schedule();
  zero[1].raises = {{0, 1}, {1, 1}};
  bool invalid = false;
  try {
    ActionAbstraction bad(zero);
    (void)bad;
  } catch (const std::invalid_argument&) {
    invalid = true;
  }
  CHECK(invalid);
  return 0;
}

}  // namespace

// The stage-6 coarse abstraction: a distinct declared identity whose menus
// offer only the clamped fraction targets, suppressing the identity rule's
// forced minimum/cap. This is the capability the independent wall review
// mandated (abstraction.cpp forced {minimum, cap} even with empty fractions,
// multiplying the coarse tree). Identity schedules and the golden digest are
// untouched.
namespace {

int test_declared_only_coarse_menu() {
  // Fresh identity: distinct name, version 1, digest different from the
  // identity id even when handed the same schedule.
  {
    ActionAbstraction coarse =
        ActionAbstraction::declared(default_size_schedule(), CoverSeeds::DeclaredOnly);
    CHECK(coarse.id() != identity_action_id());
    CHECK(coarse.id().name == "rfc0008-declared-coarse");
    CHECK(coarse.id().version == 1);
    CHECK(coarse.cover_seeds() == CoverSeeds::DeclaredOnly);
    // Same fractions, different seed rule -> different digest.
    ActionAbstraction seeded =
        ActionAbstraction::declared(default_size_schedule(), CoverSeeds::MinAndCap);
    CHECK(seeded.id() != coarse.id());
    // And neither collides with the RFC 0007 identity despite equal fractions.
    CHECK(seeded.id() != identity_action_id());
  }

  // Identity is byte-identical: same schedule, MinAndCap, golden digest.
  {
    ActionAbstraction idn = ActionAbstraction::identity();
    CHECK(idn.cover_seeds() == CoverSeeds::MinAndCap);
    CHECK(idn.schedule() == default_size_schedule());
    CHECK(idn.id() == identity_action_id());
    CHECK(idn.id().digest == 0x422c245239c7a527ULL);
  }

  // Empty fractions + DeclaredOnly => purely passive menu. Facing no wager the
  // menu is check only; the minimum bet and the jam are gone.
  {
    StreetSizes passive;
    passive.bets = {};
    passive.raises = {};
    ActionAbstraction coarse = ActionAbstraction::declared(
        SizeSchedule{passive, passive, passive, passive}, CoverSeeds::DeclaredOnly);
    poker::LegalActions legal = aggressive_menu(poker::ActionType::Bet, 1, 100);
    MenuContext ctx;
    ctx.street = poker::Street::Flop;
    ctx.pot = 6;
    ctx.opponent_stack = 100;
    const auto menu = coarse.menu(legal, ctx);
    CHECK(menu.size() == 1);
    CHECK(menu[0].type == poker::ActionType::Check);
  }

  // Empty fractions facing a wager => fold/call, no raise target at all.
  {
    StreetSizes passive;
    passive.bets = {};
    passive.raises = {};
    ActionAbstraction coarse = ActionAbstraction::declared(
        SizeSchedule{passive, passive, passive, passive}, CoverSeeds::DeclaredOnly);
    poker::LegalActions legal = aggressive_menu(poker::ActionType::Raise, 6, 100);
    legal.call_amount = 3;
    MenuContext ctx;
    ctx.street = poker::Street::Flop;
    ctx.pot = 6;
    ctx.actor_committed = 3;
    ctx.opponent_committed = 6;
    ctx.opponent_stack = 94;
    const auto menu = coarse.menu(legal, ctx);
    CHECK(menu.size() == 2);
    CHECK(menu[0].type == poker::ActionType::Fold);
    CHECK(menu[1].type == poker::ActionType::Call);
  }

  // Declared fractions {1/2,1/1} facing no wager, pot 6: targets ceil 3 and 6
  // only -- the legal minimum 1 and cap 100 are NOT offered (not coincident).
  {
    StreetSizes onew;
    onew.bets = {{1, 2}, {1, 1}};
    onew.raises = {};
    ActionAbstraction coarse =
        ActionAbstraction::declared(SizeSchedule{onew, onew, onew, onew}, CoverSeeds::DeclaredOnly);
    poker::LegalActions legal = aggressive_menu(poker::ActionType::Bet, 1, 100);
    MenuContext ctx;
    ctx.street = poker::Street::Flop;
    ctx.pot = 6;
    ctx.opponent_stack = 100;
    const auto menu = coarse.menu(legal, ctx);
    const std::vector<poker::Action> want{
        {poker::ActionType::Check}, {poker::ActionType::Bet, 3}, {poker::ActionType::Bet, 6}};
    CHECK(menu == want);
  }

  // A fraction that clamps to the effective cap DOES surface that cap under
  // DeclaredOnly -- the rule suppresses the unconditional SEED, not a target a
  // declared fraction actually reaches.
  {
    StreetSizes onew;
    onew.bets = {{100, 1}};  // clamps to cap 2
    onew.raises = {};
    ActionAbstraction coarse =
        ActionAbstraction::declared(SizeSchedule{onew, onew, onew, onew}, CoverSeeds::DeclaredOnly);
    poker::LegalActions legal = aggressive_menu(poker::ActionType::Bet, 2, 2);
    MenuContext ctx;
    ctx.street = poker::Street::River;
    ctx.pot = 100;
    ctx.opponent_stack = 2;
    const auto menu = coarse.menu(legal, ctx);
    CHECK(menu.size() == 2);
    CHECK(menu.front().type == poker::ActionType::Check);
    CHECK(menu.back() == poker::Action(poker::ActionType::Bet, 2));
  }

  // Heads-up and multiway declared-only menus agree when the multiway cover is
  // the single opponent's total (no seat-count divergence in the new mode).
  {
    StreetSizes onew;
    onew.bets = {};
    onew.raises = {{1, 2}, {1, 1}};
    ActionAbstraction coarse =
        ActionAbstraction::declared(SizeSchedule{onew, onew, onew, onew}, CoverSeeds::DeclaredOnly);
    poker::LegalActions legal = aggressive_menu(poker::ActionType::Raise, 6, 100);
    legal.call_amount = 3;
    MenuContext hu;
    hu.street = poker::Street::Flop;
    hu.pot = 6;
    hu.actor_committed = 3;
    hu.opponent_committed = 6;
    hu.opponent_stack = 94;
    MultiwayMenuContext mw;
    mw.street = poker::Street::Flop;
    mw.pot = 6;
    mw.actor_committed = 3;
    mw.cover = 100;
    // base 6, pot-after-call 9: ceil 4.5=5 and 9 -> 11 and 15; min 6 and jam
    // 100 are suppressed in both.
    const std::vector<poker::Action> want{{poker::ActionType::Fold},
                                          {poker::ActionType::Call},
                                          {poker::ActionType::Raise, 11},
                                          {poker::ActionType::Raise, 15}};
    CHECK(coarse.menu(legal, hu) == want);
    CHECK(coarse.multiway_menu(legal, mw) == want);
  }
  return 0;
}

}  // namespace

// Suit-isomorphic flop canonicalization (RFC 0009 D5.1).
namespace {

// Apply a suit relabel to a card id, keeping its rank.
int relabel_card(int card_id, const std::array<int, 4>& relabel) {
  return (card_id / 4) * 4 + relabel[card_id % 4];
}

// All 22,100 flops fold onto 1,755 isomorphism classes. For every board the
// returned relabel is a permutation that achieves the component-wise minimum
// over all 24 suit permutations, the canonical board is invariant under every
// relabeling of the input (orbit invariance), and canonicalize is idempotent
// with the identity relabel.
int test_canonicalize_classes() {
  std::array<std::array<int, 4>, 24> perms{};
  {
    std::array<int, 4> p{0, 1, 2, 3};
    int n = 0;
    do {
      perms[n++] = p;
    } while (std::next_permutation(p.begin(), p.end()));
    CHECK(n == 24);
  }
  std::set<std::array<int, 3>> classes;
  long boards = 0;
  for (int a = 0; a < 52; ++a)
    for (int b = a + 1; b < 52; ++b)
      for (int c = b + 1; c < 52; ++c) {
        const std::array<int, 3> board{a, b, c};
        const CanonicalBoard canon = canonicalize(board);
        ++boards;
        classes.insert(canon.board);
        // The relabel is a permutation of suits 0..3.
        std::array<int, 4> sorted = canon.relabel;
        std::sort(sorted.begin(), sorted.end());
        CHECK((sorted == std::array<int, 4>{0, 1, 2, 3}));
        // It achieves the minimum: its own sorted key is the canonical board,
        // and no permutation produces a smaller key.
        std::array<int, 3> own{};
        for (int i = 0; i < 3; ++i)
          own[i] = relabel_card(board[i], canon.relabel);
        std::sort(own.begin(), own.end());
        CHECK(own == canon.board);
        for (const auto& p : perms) {
          std::array<int, 3> key{};
          for (int i = 0; i < 3; ++i)
            key[i] = relabel_card(board[i], p);
          std::sort(key.begin(), key.end());
          CHECK(canon.board <= key);
        }
        // Idempotence: a canonical board keeps itself with identity relabel.
        const CanonicalBoard again = canonicalize(canon.board);
        CHECK(again.board == canon.board);
        CHECK((again.relabel == std::array<int, 4>{0, 1, 2, 3}));
        // Orbit invariance: every suit relabeling of the input folds onto
        // the same class representative.
        for (const auto& p : perms) {
          std::array<int, 3> shifted{};
          for (int i = 0; i < 3; ++i)
            shifted[i] = relabel_card(board[i], p);
          CHECK(canonicalize(shifted).board == canon.board);
        }
      }
  CHECK(boards == 22100);
  CHECK(classes.size() == 1755);  // the pinned isomorphism-class count
  return 0;
}

// Degenerate boards pin the declared tie-break: rainbow (three distinct
// ranks in three suits), paired, monotone, and two-tone.
int test_canonicalize_fixtures() {
  struct Fixture {
    std::array<int, 3> board;
    std::array<int, 3> want_board;
    std::array<int, 4> want_relabel;
  };
  const std::array<Fixture, 4> fixtures{{
      {{{card("Ks"), card("Qd"), card("7c")}}, {{20, 41, 46}}, {{2, 3, 1, 0}}},
      {{{card("Ks"), card("Kd"), card("7c")}}, {{20, 45, 46}}, {{1, 3, 2, 0}}},
      {{{card("Ks"), card("Qs"), card("7s")}}, {{20, 40, 44}}, {{0, 1, 2, 3}}},
      {{{card("Ks"), card("Kd"), card("7d")}}, {{20, 44, 45}}, {{1, 2, 0, 3}}},
  }};
  for (const Fixture& f : fixtures) {
    const CanonicalBoard canon = canonicalize(f.board);
    CHECK(canon.board == f.want_board);
    CHECK(canon.relabel == f.want_relabel);
  }
  // Out-of-range and duplicate cards fail closed.
  for (const std::array<int, 3>& bad :
       {std::array<int, 3>{-1, 1, 2}, std::array<int, 3>{52, 1, 2}, std::array<int, 3>{1, 1, 2}}) {
    bool rejected = false;
    try {
      (void)canonicalize(bad);
    } catch (const std::invalid_argument&) {
      rejected = true;
    }
    CHECK(rejected);
  }
  return 0;
}

// The own-card token round-trips over every enumerated (board, holding) pair:
// the token computed from the concrete holding equals the token of the
// relabeled holding under the identity, and inverting the relabel recovers
// the holding. Canonicalization preserves made-hand strength: the relabeled
// holding on the canonical board scores exactly what the concrete holding
// scores on the concrete board (a suit permutation renames both together).
// The token's merge behavior is pinned exactly: per-board distinct-token
// counts and the per-class union size, which is the flop-library row count.
int test_own_card_token_roundtrip() {
  const std::array<int, 4> identity{0, 1, 2, 3};
  const auto token_less = [](const OwnCardToken& x, const OwnCardToken& y) {
    if (x.ranks != y.ranks)
      return x.ranks < y.ranks;
    return x.suit_mult < y.suit_mult;
  };
  // Group the 22,100 boards by their canonical class.
  std::map<std::array<int, 3>, std::vector<std::array<int, 3>>> by_class;
  for (int a = 0; a < 52; ++a)
    for (int b = a + 1; b < 52; ++b)
      for (int c = b + 1; c < 52; ++c) {
        const std::array<int, 3> board{a, b, c};
        by_class[canonicalize(board).board].push_back(board);
      }
  CHECK(by_class.size() == 1755);

  long long total_pairs = 0;
  long long total_rows = 0;
  std::map<int, long> per_board_dist;
  for (const auto& [cboard, boards] : by_class) {
    const std::vector<int> cboard_vec{cboard.begin(), cboard.end()};
    std::vector<OwnCardToken> class_tokens;
    for (const std::array<int, 3>& board : boards) {
      const CanonicalBoard canon = canonicalize(board);
      CHECK(canon.board == cboard);
      std::array<int, 4> inverse{};
      for (int s = 0; s < 4; ++s)
        inverse[canon.relabel[s]] = s;
      const std::vector<int> board_vec{board.begin(), board.end()};
      std::vector<OwnCardToken> board_tokens;
      for (int h0 = 0; h0 < 52; ++h0) {
        if (h0 == board[0] || h0 == board[1] || h0 == board[2])
          continue;
        for (int h1 = h0 + 1; h1 < 52; ++h1) {
          if (h1 == board[0] || h1 == board[1] || h1 == board[2])
            continue;
          const std::array<int, 2> holding{h0, h1};
          const OwnCardToken token = own_card_token(holding, canon.relabel);
          ++total_pairs;
          // The multiplicity vector always sums to two.
          CHECK(token.suit_mult[0] + token.suit_mult[1] + token.suit_mult[2] + token.suit_mult[3] ==
                2);
          // Round-trip: the token is a function of the canonical holding, so
          // the concrete holding under the class relabel and the canonical
          // holding under the identity mint the same token.
          const std::array<int, 2> canonical_holding{relabel_card(h0, canon.relabel),
                                                     relabel_card(h1, canon.relabel)};
          CHECK(own_card_token(canonical_holding, identity) == token);
          // Invert: the inverse relabel recovers the concrete holding.
          std::array<int, 2> recovered{relabel_card(canonical_holding[0], inverse),
                                       relabel_card(canonical_holding[1], inverse)};
          std::sort(recovered.begin(), recovered.end());
          CHECK(recovered == holding);
          // Strength preservation: relabeling board and holding together
          // cannot change made-hand strength.
          CHECK(strength_bucket(holding, board_vec) ==
                strength_bucket(canonical_holding, cboard_vec));
          board_tokens.push_back(token);
          class_tokens.push_back(token);
        }
      }
      std::sort(board_tokens.begin(), board_tokens.end(), token_less);
      board_tokens.erase(std::unique(board_tokens.begin(), board_tokens.end()), board_tokens.end());
      per_board_dist[static_cast<int>(board_tokens.size())]++;
    }
    std::sort(class_tokens.begin(), class_tokens.end(), token_less);
    class_tokens.erase(std::unique(class_tokens.begin(), class_tokens.end()), class_tokens.end());
    total_rows += static_cast<long long>(class_tokens.size());
  }
  CHECK(total_pairs == 25989600);
  // Pinned merge behavior: per-board distinct-token counts. Trips boards
  // (52) merge the most; rainbow unpaired boards (6,864) the least.
  const std::map<int, long> want_dist{{780, 52},   {801, 1872},  {802, 1872},
                                      {807, 1144}, {811, 10296}, {813, 6864}};
  CHECK(per_board_dist == want_dist);
  // Pinned per-class union: the flop-library row count over all 1,755
  // classes, measured now so W4c's storage budget starts from a known number.
  // Boards in the same class share most tokens, so the union is far below the
  // 17,895,072 distinct (concrete-board, token) rows.
  CHECK(total_rows == 1419366);
  return 0;
}

// Pinned own-card tokens on the fixture boards, including the tie-break
// interaction: on the paired board the two kings' suits tie on the minimum,
// and holdings in the two tied suits land in different slots.
int test_own_card_token_fixtures() {
  const std::array<int, 4> identity{0, 1, 2, 3};
  struct Case {
    std::array<int, 2> holding;
    std::array<int, 2> ranks;
    std::array<int, 4> mult;
  };
  // Rainbow Ks Qd 7c, relabel [2,3,1,0]: s->2, h->3, d->1, c->0.
  const CanonicalBoard rainbow = canonicalize({card("Ks"), card("Qd"), card("7c")});
  CHECK((rainbow.relabel == std::array<int, 4>{2, 3, 1, 0}));
  const std::array<Case, 4> rainbow_cases{{
      {{{card("As"), card("Jh")}}, {{9, 12}}, {{0, 0, 1, 1}}},
      {{{card("As"), card("Js")}}, {{9, 12}}, {{0, 0, 2, 0}}},
      {{{card("Ad"), card("Jc")}}, {{9, 12}}, {{1, 1, 0, 0}}},
      {{{card("Ah"), card("Jc")}}, {{9, 12}}, {{1, 0, 0, 1}}},
  }};
  for (const Case& c : rainbow_cases) {
    const OwnCardToken token = own_card_token(c.holding, rainbow.relabel);
    CHECK(token.ranks == c.ranks);
    CHECK(token.suit_mult == c.mult);
    const std::array<int, 2> canonical_holding{relabel_card(c.holding[0], rainbow.relabel),
                                               relabel_card(c.holding[1], rainbow.relabel)};
    CHECK(own_card_token(canonical_holding, identity) == token);
  }
  // Paired Ks Kd 7c, relabel [1,3,2,0]: s->1, h->3, d->2, c->0.
  const CanonicalBoard paired = canonicalize({card("Ks"), card("Kd"), card("7c")});
  CHECK((paired.relabel == std::array<int, 4>{1, 3, 2, 0}));
  const std::array<Case, 2> paired_cases{{
      {{{card("As"), card("Js")}}, {{9, 12}}, {{0, 2, 0, 0}}},
      {{{card("Ad"), card("Jd")}}, {{9, 12}}, {{0, 0, 2, 0}}},
  }};
  for (const Case& c : paired_cases) {
    const OwnCardToken token = own_card_token(c.holding, paired.relabel);
    CHECK(token.ranks == c.ranks);
    CHECK(token.suit_mult == c.mult);
  }
  // Bad holdings and malformed relabels fail closed.
  for (const std::array<int, 2>& bad : {std::array<int, 2>{52, 1}, std::array<int, 2>{1, 1}}) {
    bool rejected = false;
    try {
      (void)own_card_token(bad, identity);
    } catch (const std::invalid_argument&) {
      rejected = true;
    }
    CHECK(rejected);
  }
  bool rejected_relabel = false;
  try {
    (void)own_card_token({{1, 2}}, {{0, 0, 1, 2}});
  } catch (const std::invalid_argument&) {
    rejected_relabel = true;
  }
  CHECK(rejected_relabel);
  return 0;
}

// The declared identity: distinct name, golden digest, and no collision with
// the other abstraction ids.
int test_suit_canonicalization_id() {
  const AbstractionId id = suit_canonicalization_id();
  CHECK(id.name == "suit-canonical-v1");
  CHECK(id.version == 1);
  CHECK(id.digest == abstraction_digest(id.name, id.version, id.parameters));
  // Golden digest: any change to the comparison order, tie-break, or token
  // shape changes the canonical parameters and thus the digest.
  CHECK(id.digest == 0xaf83bd016a92d2d6ULL);
  CHECK(id.digest != identity_action_id().digest);
  CHECK(id.digest != card_abstraction_id(CardBucketKind::Identity).digest);
  CHECK(id.digest != card_abstraction_id(CardBucketKind::CategoryTiersV1).digest);
  return 0;
}

}  // namespace

int main() {
  struct Case {
    const char* name;
    int (*fn)();
  };
  const std::array<Case, 13> cases{{
      {"action_menu_math", test_action_menu_math},
      {"fraction_validation", test_fraction_validation},
      {"identity_and_typed_refusal", test_identity_and_typed_refusal},
      {"identity_card_bucket", test_identity_card_bucket},
      {"lossy_tier_bucket_merges", test_lossy_tier_bucket_merges},
      {"multiway_cover_cap", test_multiway_cover_cap},
      {"declared_schedule_is_reduced", test_declared_schedule_is_reduced},
      {"declared_only_coarse_menu", test_declared_only_coarse_menu},
      {"canonicalize_classes", test_canonicalize_classes},
      {"canonicalize_fixtures", test_canonicalize_fixtures},
      {"own_card_token_roundtrip", test_own_card_token_roundtrip},
      {"own_card_token_fixtures", test_own_card_token_fixtures},
      {"suit_canonicalization_id", test_suit_canonicalization_id},
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
