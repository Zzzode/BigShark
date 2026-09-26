// RFC 0008 stage 6 Revision 3: pins the pot-scale geometry quantization that
// collapses the 80,438 exact 10-seat flop geometries to 184 buckets (24
// actionable + 160 all-in runout). Covers the bucket key, totality (every
// exact signature maps to exactly one bucket), the all-in-for-less no-mix
// guard, and the reduced live-count representative construction.
#include <bs/abstract_tree.hpp>
#include <bs/abstraction.hpp>
#include <bs/game_definition.hpp>
#include <bs/stage6/geometry.hpp>
#include <cstdio>
#include <exception>
#include <stdexcept>
#include <string>
#include <vector>

using namespace bs::stage6;
using bs::poker::Chips;

namespace {
int failures = 0;
void check(bool c, const std::string& d) {
  if (!c) {
    std::fprintf(stderr, "FAIL: %s\n", d.c_str());
    ++failures;
  }
}

GeometrySignature sig(std::size_t n, Chips pot, std::vector<std::size_t> live,
                      std::vector<int> stacks, std::vector<int> contrib) {
  GeometrySignature s;
  s.player_count = n;
  s.pot = pot;
  for (std::size_t i = 0; i < n; ++i)
    s.stacks[i] = static_cast<Chips>(stacks[i]);
  for (std::size_t i = 0; i < n; ++i)
    s.contributed[i] = static_cast<Chips>(contrib[i]);
  s.live = live;
  return s;
}
}  // namespace

int main() {
  const Chips bb = 2;

  // Bucket key quantizes pot to whole bb (nearest).
  {
    GeometrySignature a = sig(3, 12, {0, 1, 2}, {0, 194, 194}, {0, 6, 6});
    GeometrySignature b = sig(3, 13, {0, 1, 2}, {0, 194, 194}, {0, 6, 7});
    GeometryBucketKey ka = bucket_key_for(a, bb);
    GeometryBucketKey kb = bucket_key_for(b, bb);
    check(ka.pot_bb == 6, "pot 12 rounds to 6 bb");
    check(kb.pot_bb == 7, "pot 13 rounds to 7 bb");  // (13+1)/2
    check(ka.live_count == 3 && ka.acting_count == 2, "live/acting counts exclude folded seat");
  }

  // Two geometries with the same bb pot + live count + acting count collapse
  // to one bucket even if exact chips differ by less than a bb.
  {
    GeometrySignature a = sig(3, 12, {1, 2}, {200, 194, 194}, {0, 6, 6});
    GeometrySignature b = sig(3, 12, {0, 2}, {194, 200, 194}, {6, 0, 6});
    std::vector<GeometryBucket> buckets = bucket_geometries(3, bb, {a, b});
    check(buckets.size() == 1, "equal pot/live/acting collapse to one bucket");
    check(buckets[0].members.size() == 2, "both exact geometries are bucket members");
    check(buckets[0].actionable, "two seats with stack behind is actionable");
  }

  // All-in-at-flop (no live seat has stack behind) is a non-actionable runout.
  {
    GeometrySignature allin = sig(3, 600, {0, 1, 2}, {0, 0, 0}, {200, 200, 200});
    std::vector<GeometryBucket> buckets = bucket_geometries(3, bb, {allin});
    check(buckets.size() == 1 && !buckets[0].actionable, "all-in-at-flop is a runout bucket");
    check(buckets[0].representative_stacks.empty(), "runout has no representative acting game");
  }

  // Reduced representative for an actionable bucket: live-count-only game,
  // equal acting stacks at the member MINIMUM and equal contributions whose
  // sum is the legal representative pot.
  {
    GeometrySignature deep = sig(3, 18, {0, 1, 2}, {194, 194, 194}, {6, 6, 6});
    std::vector<GeometryBucket> buckets = bucket_geometries(3, bb, {deep});
    const GeometryBucket& b = buckets[0];
    check(b.key.live_count == 3, "three live seats in reduced game");
    check(b.representative_stacks.size() == 3, "representative has one stack per live seat");
    for (Chips st : b.representative_stacks)
      check(st == 194, "representative acting stacks equal the member minimum");
    Chips sum = 0;
    for (Chips c : b.representative_contrib)
      sum += c;
    check(sum == b.representative_pot, "representative contributions reconcile exactly to pot");
  }

  // For-less all-in separation: a shorter live seat that ends all-in for
  // less has zero stack behind, so it lowers the acting count and the
  // geometry lands in a DIFFERENT bucket from a fully-continuing one. The
  // bucket key itself prevents the mix; the in-code guard is defense in
  // depth. (This is why an actionable representative never silently contains
  // an all-in-for-less member with an incompatible betting tree.)
  {
    GeometrySignature all_continuing = sig(4, 18, {1, 2, 3}, {200, 194, 194, 194}, {0, 6, 6, 6});
    GeometrySignature for_less_allin = sig(4, 18, {1, 2, 3}, {200, 194, 194, 0}, {0, 6, 6, 12});
    const GeometryBucketKey ka = bucket_key_for(all_continuing, bb);
    const GeometryBucketKey kb = bucket_key_for(for_less_allin, bb);
    check(ka.acting_count == 3 && kb.acting_count == 2,
          "the for-less all-in lowers the acting count");
    check(!(ka == kb), "the for-less geometry is a separate bucket, never mixed");
    // And neither representative construction throws.
    (void)bucket_geometries(4, bb, {all_continuing});
    (void)bucket_geometries(4, bb, {for_less_allin});
  }

  // Regression (mutation-RED): a bucket whose rounded pot carries folded-seat
  // dead money must produce a LEGAL equal-contribution reduced representative,
  // not an uneven split that settlement rejects. Before the fix the pot-10
  // bucket below was built as in{6,7,7}; the level-6 chip layer then had a
  // single contributor and materializing the tree threw "positive pot layer
  // has no eligible winner". Three acting live seats each really called 6; the
  // extra chip is the folded small blind, which the reduced game cannot carry.
  {
    // Three callers each put in 6; a folded seat posted a 1-chip small blind
    // and folded, padding the rounded pot to 19/20. Only the three live seats
    // survive in the reduced game. Acting stacks are deliberately shallow so
    // the materialized tree fits the default node cap; the contribution-layer
    // validation that regressed is independent of behind-stack depth.
    GeometrySignature padded = sig(4, 19, {1, 2, 3}, {200, 8, 8, 8}, {1, 6, 6, 6});
    std::vector<GeometryBucket> buckets = bucket_geometries(4, bb, {padded});
    const GeometryBucket* target = nullptr;
    for (const GeometryBucket& b : buckets)
      if (b.actionable && b.key.live_count == 3)
        target = &b;
    check(target != nullptr, "folded-dead fixture yields a live-3 actionable bucket");
    if (target) {
      check(target->representative_contrib.size() == 3, "representative has three live seats");
      for (Chips c : target->representative_contrib)
        check(c == 6, "every reduced seat contributes the equal 6, never an invented 7");
      Chips sum = 0;
      for (Chips c : target->representative_contrib)
        sum += c;
      check(sum == target->representative_pot, "representative pot reconciles to the equal sum");

      // The representative must be a legal rooted game: build the declared-only
      // coarse tree exactly as the R3b sizer does. This is the call that threw
      // before the fix.
      bs::poker::GameDef def{};
      def.player_count = 3;
      def.button = 0;
      def.big_blind = bb;
      def.preflop = false;
      for (std::size_t i = 0; i < 3; ++i) {
        def.stacks[i] = target->representative_stacks[i];
        def.contributions[i] = target->representative_contrib[i];
      }
      def.pot = target->representative_pot;
      def.board = {0, 6, 21, 0, 0};
      def.board_size = 3;
      using bs::abstraction::ActionAbstraction;
      using bs::abstraction::CoverSeeds;
      using bs::abstraction::SizeSchedule;
      using bs::tree::AbstractTree;
      SizeSchedule coarse = bs::abstraction::default_size_schedule();
      for (auto& street : coarse) {
        street.bets = {{1, 2}};
        street.raises = {{1, 1}};
      }
      ActionAbstraction action = ActionAbstraction::declared(coarse, CoverSeeds::DeclaredOnly);
      bool threw = false;
      try {
        AbstractTree tree(def, action);
        check(tree.size() > 0, "folded-dead representative materializes a non-empty tree");
      } catch (const std::exception& e) {
        threw = true;
        std::fprintf(stderr, "unexpected throw: %s\n", e.what());
      }
      check(!threw, "materializing the folded-dead representative does not throw");
    }
  }

  // Minimum acting depth across members must be a true minimum even when the
  // FIRST member's acting stacks are ASCENDING. An earlier reduction borrowed a
  // cross-member sentinel inside the per-seat loop, so for the first member it
  // overwrote the minimum with every seat and ended at that member's LAST
  // (deepest) stack. Latent at the uniform-100bb fixture (all stacks equal),
  // wrong once mixed-depth members share a bucket (the key ignores depth).
  {
    GeometrySignature ascending = sig(2, 12, {0, 1}, {50, 100}, {6, 6});
    GeometrySignature deep = sig(2, 12, {0, 1}, {100, 100}, {6, 6});
    std::vector<GeometryBucket> buckets = bucket_geometries(2, bb, {ascending, deep});
    check(buckets.size() == 1, "unequal-depth signatures share one (depth-agnostic) bucket");
    check(buckets[0].min_acting_stack == 50,
          "min acting depth is 50, not the first member's last/100 stack");
    for (Chips st : buckets[0].representative_stacks)
      check(st == 50, "the reduced representative uses the true shallowest depth");
  }

  if (failures) {
    std::fprintf(stderr, "GEOMETRY BUCKET TESTS FAILED: %d\n", failures);
    return 1;
  }
  std::puts("GEOMETRY BUCKET TESTS PASSED");
  return 0;
}
