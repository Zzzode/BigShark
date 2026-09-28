// RFC 0008 stage 6 R11 item 2 (chunk A): chart-only reach carries the
// reaching preflop log, and per-live-seat chart-reach-restricted hole ranges
// are built by intersecting, per seat, the holdings whose pinned chart action
// matches that seat's logged actions. The combo counts here are PINNED
// empirical facts (do not "soften" them): HU s0:R6 s1:C gives 526/84 combos,
// the 3-seat single-raised pot gives 526/84/84, and every actionable
// chart-reachable bucket at n in {2,3,6} has a non-empty range per live seat.
#include <algorithm>
#include <array>
#include <bs/multiway_sampler.hpp>
#include <bs/prng.hpp>
#include <bs/stage6/chart_reach.hpp>
#include <bs/stage6/geometry.hpp>
#include <bs/stage6/geometry_enumerator.hpp>
#include <cstdio>
#include <exception>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <unordered_set>
#include <vector>

using namespace bs::stage6;
using bs::poker::Chips;
using bs::solver::MultiwayWeightedHand;

namespace {

int failures = 0;
void check(bool c, const std::string& d) {
  if (!c) {
    std::fprintf(stderr, "FAIL: %s\n", d.c_str());
    ++failures;
  }
}

// Card id = rank*4+suit; ranks "23456789TJQKA", suits s/h/d/c.
int card_id(const std::string& name) {
  static const std::string ranks = "23456789TJQKA";
  static const std::string suits = "shdc";
  const std::size_t r = ranks.find(name[0]);
  const std::size_t s = suits.find(name[1]);
  check(r != std::string::npos && s != std::string::npos && name.size() == 2,
        "card name helper got a malformed name");
  return static_cast<int>(r * 4 + s);
}

std::array<int, 2> combo(const std::string& hi, const std::string& lo) {
  std::array<int, 2> c{card_id(hi), card_id(lo)};
  std::sort(c.begin(), c.end());
  return c;
}

// Canonical origin line, byte-for-byte the encoding geometry_enumerator emits
// ("s<seat>:<int(action.type)>=<target_total> " joined).
std::string canonical_line(const HandLog& log) {
  std::string line;
  for (const LoggedAction& a : log.preflop) {
    line += "s" + std::to_string(a.seat) + ":" + std::to_string(static_cast<int>(a.action.type)) +
            "=" + std::to_string(a.action.target_total) + " ";
  }
  return line;
}

const ReachableGeometry& find_by_line(const std::vector<ReachableGeometry>& reach,
                                      const std::string& line) {
  for (const ReachableGeometry& rec : reach)
    if (canonical_line(rec.preflop_log) == line)
      return rec;
  check(false, "expected chart-reachable line not found: " + line);
  return reach.front();
}

using ComboSet = std::unordered_set<int>;  // key = low*52+high

int combo_key(const std::array<int, 2>& c) {
  return c[0] * 52 + c[1];
}

ComboSet to_set(const std::vector<MultiwayWeightedHand>& range) {
  ComboSet set;
  for (const MultiwayWeightedHand& h : range)
    set.insert(combo_key(h.cards));
  return set;
}

void check_contains(const ComboSet& set, std::array<int, 2> c, const std::string& what) {
  check(set.contains(combo_key(c)),
        what + " must be kept (" + std::to_string(c[0]) + "," + std::to_string(c[1]) + ")");
}

void check_excludes(const ComboSet& set, std::array<int, 2> c, const std::string& what) {
  check(!set.contains(combo_key(c)),
        what + " must be excluded (" + std::to_string(c[0]) + "," + std::to_string(c[1]) + ")");
}

// (1)+(2): the pinned single-raised-pot surviving sets on a concrete line.
void test_single_raised_pot(std::size_t n, const std::string& line) {
  const Chips bb = 2;
  const std::vector<ReachableGeometry> reach = enumerate_chart_flop_reach(n, bb);
  const ReachableGeometry& rec = find_by_line(reach, line);
  const std::vector<std::vector<MultiwayWeightedHand>> ranges = chart_reach_ranges(rec, bb);
  check(ranges.size() == rec.sig.live.size(),
        "one range per live seat on the single-raised pot n=" + std::to_string(n));
  check(ranges[0].size() == 526,
        "opener surviving range is exactly 526 combos (n=" + std::to_string(n) + ")");
  for (std::size_t s = 1; s < ranges.size(); ++s)
    check(ranges[s].size() == 84,
          "caller surviving range is exactly 84 combos (n=" + std::to_string(n) + ")");

  const ComboSet opener = to_set(ranges[0]);
  check_contains(opener, combo("As", "Ah"), "opener AsAh");
  check_contains(opener, combo("As", "Ks"), "opener AsKs");
  check_excludes(opener, combo("7s", "2s"), "opener 7s2s");

  for (std::size_t s = 1; s < ranges.size(); ++s) {
    const ComboSet caller = to_set(ranges[s]);
    const std::string who = "caller seat " + std::to_string(rec.sig.live[s]);
    check_contains(caller, combo("2s", "2h"), who + " 2s2h (pocket pair)");
    check_excludes(caller, combo("As", "Ks"), who + " AsKs");
    check_excludes(caller, combo("Ks", "Js"), who + " KsJs");
    check_excludes(caller, combo("Js", "Ts"), who + " JsTs");
    check_excludes(caller, combo("Qs", "Js"), who + " QsJs");
    check_excludes(caller, combo("As", "2s"), who + " As2s");
    check_excludes(caller, combo("Ks", "2s"), who + " Ks2s");
    check_excludes(caller, combo("7s", "2c"), who + " 7s2c");
  }
}

}  // namespace

int main() {
  const Chips bb = 2;

  // (1) Heads-up BTN open to 6, BB call.
  test_single_raised_pot(2, "s0:4=6 s1:2=0 ");

  // (2) 3-seat UTN open to 6, SB call, BB call.
  test_single_raised_pot(3, "s0:4=6 s1:2=0 s2:2=0 ");

  // (3) Seat mapping: the opener folds, the two later seats reach the flop.
  //     Reduced-seat order must follow the ascending live list {1,2}.
  {
    const std::vector<ReachableGeometry> reach = enumerate_chart_flop_reach(3, bb);
    const ReachableGeometry& rec = find_by_line(reach, "s0:0=0 s1:4=6 s2:2=0 ");
    check(rec.sig.live == std::vector<std::size_t>({1, 2}),
          "folded-opener line leaves exactly live seats {1,2}");
    const std::vector<std::vector<MultiwayWeightedHand>> ranges = chart_reach_ranges(rec, bb);
    check(ranges.size() == 2, "folded seat gets no range vector");
    for (std::size_t i = 0; i < rec.sig.live.size(); ++i) {
      check(!ranges[i].empty(), "reduced seat range is non-empty");
      for (const MultiwayWeightedHand& h : ranges[i])
        check(h.cards[0] >= 0 && h.cards[1] <= 51 && h.cards[0] < h.cards[1],
              "reduced range holds sorted in-range card pairs");
    }
    // The SB (reduced seat 0, full seat 1) is the raiser; it must keep AsKs,
    // while the BB caller (reduced seat 1) must not — proving the index maps
    // to the full-table live seat rather than to an enumeration rank.
    check_contains(to_set(ranges[0]), combo("As", "Ks"), "reduced seat 0 is the SB raiser");
    check_excludes(to_set(ranges[1]), combo("As", "Ks"), "reduced seat 1 is the BB caller");
  }

  // Cache the larger enumerations for (5),(6),(9).
  const std::vector<ReachableGeometry> reach2 = enumerate_chart_flop_reach(2, bb);
  const std::vector<ReachableGeometry> reach3 = enumerate_chart_flop_reach(3, bb);
  const std::vector<ReachableGeometry> reach6 = enumerate_chart_flop_reach(6, bb);

  // (4) Nonemptiness sweep plus the n6 potbb10/live3 rotation-union pin.
  std::printf("[chart-reach] actionable bucket counts and per-seat union sizes:\n");
  std::optional<GeometryBucket> n6_live3_pot10;
  std::optional<GeometryBucket> n6_all_live;
  for (const std::size_t n : {std::size_t{2}, std::size_t{3}, std::size_t{6}}) {
    const std::vector<ReachableGeometry>& reach = (n == 2 ? reach2 : n == 3 ? reach3 : reach6);
    std::vector<GeometrySignature> sigs;
    sigs.reserve(reach.size());
    for (const ReachableGeometry& rec : reach)
      sigs.push_back(rec.sig);
    const std::vector<GeometryBucket> buckets = bucket_geometries(n, bb, sigs);
    std::size_t actionable = 0;
    for (const GeometryBucket& bucket : buckets) {
      if (!bucket.actionable)
        continue;
      ++actionable;
      const std::vector<std::vector<MultiwayWeightedHand>> ranges =
          chart_reach_bucket_ranges(bucket, reach, bb);
      check(ranges.size() == bucket.key.live_count,
            "bucket union returns one range per live seat (n=" + std::to_string(n) + ")");
      for (const std::vector<MultiwayWeightedHand>& range : ranges)
        check(!range.empty(),
              "actionable bucket has no empty per-seat range (n=" + std::to_string(n) + ")");
      if (n == 6 && bucket.key.pot_bb == 10 && bucket.key.live_count == 3)
        n6_live3_pot10 = bucket;
      if (n == 6 && bucket.key.live_count == 6)
        n6_all_live = bucket;
    }
    static const std::size_t pinned[] = {0, 0, 1, 3, 0, 0, 12};
    check(actionable == pinned[n],
          "pinned actionable chart-reachable bucket count for n=" + std::to_string(n) + " (got " +
              std::to_string(actionable) + ")");
    std::printf("  n=%zu actionable_buckets=%zu\n", n, actionable);
  }

  check(n6_live3_pot10.has_value(), "an n=6 potbb10 live3 bucket exists under chart reach");
  {
    const GeometryBucket& bucket = *n6_live3_pot10;
    check(bucket.key.acting_count == 3, "n6 potbb10 live3 bucket has three acting seats");
    const std::vector<std::vector<MultiwayWeightedHand>> ranges =
        chart_reach_bucket_ranges(bucket, reach6, bb);

    // Independently recompute the indicator union: dedupe member combos per
    // reduced seat, and demand it equals the production result exactly.
    std::vector<ComboSet> independent(bucket.key.live_count);
    for (const GeometrySignature& member : bucket.members) {
      const ReachableGeometry* mrec = nullptr;
      for (const ReachableGeometry& rec : reach6)
        if (rec.sig == member)
          mrec = &rec;
      check(mrec != nullptr, "bucket member is present in the chart-only reach");
      const std::vector<std::vector<MultiwayWeightedHand>> mranges = chart_reach_ranges(*mrec, bb);
      // Same live set within one member.
      check(mrec->sig.live.size() == mranges.size(),
            "member range count matches its live-seat count");
      for (std::size_t seat = 0; seat < mranges.size(); ++seat)
        for (const MultiwayWeightedHand& h : mranges[seat])
          independent[seat].insert(combo_key(h.cards));
    }
    std::printf("  n6 potbb10 live3 members=%zu union sizes:", bucket.members.size());
    for (std::size_t seat = 0; seat < ranges.size(); ++seat) {
      std::printf(" %zu", ranges[seat].size());
      check(ranges[seat].size() == independent[seat].size(),
            "union size equals indicator dedupe (n6 potbb10 live3)");
      const ComboSet produced = to_set(ranges[seat]);
      check(produced == independent[seat],
            "union content equals indicator dedupe (n6 potbb10 live3)");
      for (const MultiwayWeightedHand& h : ranges[seat]) {
        check(h.weight == 1.0, "indicator-unioned combo carries weight exactly 1.0");
        check(h.cards[0] < h.cards[1], "union holds a sorted card pair");
      }
    }
    std::printf("\n");
  }

  // (4b) Raw-walk audit: every ACTIONABLE signature is reached by a single
  //      preflop line. The deduped enumeration keeps only the lexicographically
  //      smallest line per sig; that is lossless only while no two distinct
  //      lines to one actionable sig imply different surviving ranges. Replay
  //      EVERY raw hit (pre-dedupe) and compare per-seat card sets per sig.
  for (const std::size_t n : {std::size_t{2}, std::size_t{3}, std::size_t{6}}) {
    const std::vector<ReachableGeometry>& deduped = (n == 2 ? reach2 : n == 3 ? reach3 : reach6);
    std::set<std::string> actionable_sigs;
    {
      std::vector<GeometrySignature> sigs;
      for (const ReachableGeometry& rec : deduped)
        sigs.push_back(rec.sig);
      for (const GeometryBucket& bucket : bucket_geometries(n, bb, sigs))
        if (bucket.actionable)
          for (const GeometrySignature& member : bucket.members)
            actionable_sigs.insert(member.to_string());
    }
    std::map<std::string, std::vector<std::vector<ComboSet>>> by_sig;
    for (const ReachableGeometry& hit : enumerate_chart_flop_reach_hits(n, bb)) {
      if (!actionable_sigs.contains(hit.sig.to_string()))
        continue;
      const auto member_ranges = chart_reach_ranges(hit, bb);
      std::vector<ComboSet> sets;
      sets.reserve(member_ranges.size());
      for (const auto& r : member_ranges)
        sets.push_back(to_set(r));
      by_sig[hit.sig.to_string()].push_back(std::move(sets));
    }
    std::size_t multi_line = 0;
    for (const auto& [label, variants] : by_sig) {
      check(!variants.empty(), "actionable sig has at least one raw hit");
      if (variants.size() == 1)
        continue;
      ++multi_line;
      for (std::size_t v = 1; v < variants.size(); ++v)
        check(variants[v] == variants[0],
              "actionable sig " + label +
                  " is reached by two lines with different chart-conditioned ranges");
    }
    std::printf("  n=%zu actionable sigs reached by >1 distinct line: %zu (all range-identical)\n",
                n, multi_line);
  }

  // (5) Determinism: enumeration order/dedupe and range card sets are stable
  //     across independent runs.
  {
    const std::vector<ReachableGeometry> again = enumerate_chart_flop_reach(3, bb);
    check(again.size() == reach3.size(), "n=3 reach enumeration count is stable");
    for (std::size_t i = 0; i < reach3.size(); ++i) {
      check(reach3[i].sig == again[i].sig, "n=3 reach signature order is stable");
      check(reach3[i].sig.to_string() <=
                (i + 1 < reach3.size() ? reach3[i + 1].sig.to_string() : std::string("~")),
            "n=3 reach is sorted by signature string");
      check(canonical_line(reach3[i].preflop_log) == canonical_line(again[i].preflop_log),
            "n=3 retained canonical line is stable");
    }
    // All-in-at-flop geometries can legitimately carry an empty chart range;
    // that typed throw must itself be deterministic across the two runs.
    for (const ReachableGeometry& rec : reach3) {
      std::vector<std::vector<MultiwayWeightedHand>> a;
      std::vector<std::vector<MultiwayWeightedHand>> b;
      bool a_empty = false;
      bool b_empty = false;
      try {
        a = chart_reach_ranges(rec, bb);
      } catch (const stage6_chart_reach_error&) {
        a_empty = true;
      }
      try {
        b = chart_reach_ranges(rec, bb);
      } catch (const stage6_chart_reach_error&) {
        b_empty = true;
      }
      check(a_empty == b_empty, "empty-range classification is deterministic");
      if (a_empty)
        continue;
      check(a.size() == b.size(), "range seat count is deterministic");
      for (std::size_t s = 0; s < a.size(); ++s) {
        check(a[s].size() == b[s].size(), "range combo count is deterministic");
        for (std::size_t k = 0; k < a[s].size(); ++k)
          check(a[s][k].cards == b[s][k].cards, "range card sets are deterministic");
      }
    }
  }

  // (6) Joint compatibility: 10000 scalable joint deals per bucket against a
  //     fixed board must always be card-valid, in-range, and within ranges.
  //     The empirical pin is a tight ATTEMPT TAIL (p99 far under 20; average
  //     1.4 at n6 live3 and 3.3 at all-live 6 with no board): over 10000
  //     draws the sample maximum can still graze past 20 (~0.2% tail with a
  //     board), so the hard assertion pins the p99/average rather than the
  //     10000-draw maximum, while printing the maximum for information.
  {
    check(n6_all_live.has_value(), "an n=6 all-live (six live seats) bucket exists");
    const std::vector<int> board = {0, 6, 21};  // 2s, 3d, 7h
    for (const GeometryBucket* bucket : {&*n6_live3_pot10, &*n6_all_live}) {
      const std::vector<std::vector<MultiwayWeightedHand>> ranges =
          chart_reach_bucket_ranges(*bucket, reach6, bb);
      std::vector<ComboSet> sets;
      sets.reserve(ranges.size());
      for (const auto& range : ranges)
        sets.push_back(to_set(range));
      bs::SplitMix64 rng(0x51112026deadbeefULL);
      std::vector<std::size_t> attempt_samples;
      attempt_samples.reserve(10000);
      std::size_t max_attempts = 0;
      std::uint64_t total_attempts = 0;
      for (int draw = 0; draw < 10000; ++draw) {
        std::size_t attempts = 0;
        const bs::solver::MultiwayDeal deal =
            bs::solver::sample_scalable_joint_deal(ranges, board, rng, 100000, &attempts);
        attempt_samples.push_back(attempts);
        max_attempts = std::max(max_attempts, attempts);
        total_attempts += attempts;
        check(deal.hands.size() == ranges.size(), "joint deal deals one hand per live seat");
        std::array<bool, 52> used{};
        for (int c : board)
          used[c] = true;
        for (std::size_t s = 0; s < deal.hands.size(); ++s) {
          const std::array<int, 2>& h = deal.hands[s];
          check(h[0] >= 0 && h[1] < 52 && h[0] < h[1], "dealt hole pair is sorted and in range");
          check(!used[h[0]] && !used[h[1]], "joint deal has no duplicate or board card");
          used[h[0]] = used[h[1]] = true;
          check(sets[s].contains(combo_key(h)),
                "dealt pair belongs to that seat's chart-reach range");
        }
      }
      std::sort(attempt_samples.begin(), attempt_samples.end());
      const double avg = static_cast<double>(total_attempts) / 10000.0;
      const std::size_t p99 = attempt_samples[9900];
      std::printf(
          "[chart-reach] n6 live%zu joint deals (board 2s,3d,7h): avg_attempts=%.2f "
          "p99=%zu max_attempts=%zu\n",
          ranges.size(), avg, p99, max_attempts);
      // The average is the robust canary: it is low-variance and would catch
      // any real doubling of the restart-proposal cost. p99 and the sample
      // maximum are seed-pinned tail canaries (the fixed RNG makes them
      // deterministic, not statistical confidence bounds), hence their looser
      // margins over the observed p99=16 / max=36 on the all-live-6 range.
      check(p99 < 20, "joint-deal p99 proposal attempts stay far under 20");
      check(avg < 6.0, "joint-deal average proposal attempts stay tight");
      check(max_attempts < 100, "joint-deal maximum proposal attempts stay bounded");
    }
  }

  // (7) The fail-closed same-liveset invariant: fabricated member sets that
  //     disagree must throw; agreeing members union normally.
  {
    const GeometryBucketKey key{3, 10, 3, 3};
    ChartReachMemberSets a;
    a.label = "synthetic-A";
    a.live = {0, 1, 2};
    a.card_sets = {{combo("2s", "2h")}, {combo("As", "Ks")}, {combo("7s", "2c")}};
    ChartReachMemberSets b;
    b.label = "synthetic-B";
    b.live = {0, 1, 2};  // identical live list
    b.card_sets = {
        {combo("2s", "2h")}, {combo("As", "Qs"), combo("As", "Ks")}, {combo("7s", "2c")}};
    bool threw = false;
    try {
      (void)chart_reach_union_member_ranges(key, {a, b});
    } catch (const stage6_chart_reach_error& e) {
      threw = true;
      std::printf("[chart-reach] same-liveset conflict refused: %s\n", e.what());
    } catch (const std::exception& e) {
      check(false, std::string("same-liveset conflict must throw the typed chart-reach error, "
                               "got: ") +
                       e.what());
    }
    check(threw, "same live list with different surviving card sets throws fail-closed");

    // Positive control: equal sets for equal live lists union without throw.
    ChartReachMemberSets c = a;
    c.label = "synthetic-C";
    bool threw_good = false;
    try {
      const auto ranges = chart_reach_union_member_ranges(key, {a, c});
      check(ranges.size() == 3 && ranges[0].size() == 1 && ranges[1].size() == 1 &&
                ranges[2].size() == 1,
            "agreeing same-liveset members union to the shared sets");
    } catch (const std::exception& e) {
      threw_good = true;
      std::fprintf(stderr, "FAIL: agreeing members unexpectedly threw: %s\n", e.what());
    }
    check(!threw_good, "agreeing same-liveset members do not throw");
  }

  // (8) Empty-range throw on a natural all-in-at-flop chart-reachable
  //     geometry (empties occur only when acting_count == 0). Search
  //     n=6,9,10 and pin the typed throw on the first hit.
  {
    std::string empty_sig;
    std::string empty_line;
    std::size_t empty_n = 0;
    for (const std::size_t n : {std::size_t{6}, std::size_t{9}, std::size_t{10}}) {
      const std::vector<ReachableGeometry> reach = enumerate_chart_flop_reach(n, bb);
      for (const ReachableGeometry& rec : reach) {
        if (bucket_key_for(rec.sig, bb).acting_count != 0)
          continue;
        try {
          (void)chart_reach_ranges(rec, bb);
        } catch (const stage6_chart_reach_error&) {
          empty_sig = rec.sig.to_string();
          empty_line = canonical_line(rec.preflop_log);
          empty_n = n;
          break;
        } catch (const std::exception&) {
          // Any other replay problem on this log: keep searching.
        }
      }
      if (!empty_sig.empty())
        break;
    }
    if (!empty_sig.empty()) {
      std::printf("[chart-reach] natural empty-range all-in-at-flop sig at n=%zu: %s line=%s\n",
                  empty_n, empty_sig.c_str(), empty_line.c_str());
      // Re-enumerate and re-assert by exact signature, independent of the
      // search loop's vector lifetime.
      const std::vector<ReachableGeometry> reach = enumerate_chart_flop_reach(empty_n, bb);
      const ReachableGeometry* hit = nullptr;
      for (const ReachableGeometry& rec : reach)
        if (rec.sig.to_string() == empty_sig)
          hit = &rec;
      check(hit != nullptr, "the all-in-at-flop empty-range signature is reproducibly reachable");
      bool threw_empty = false;
      try {
        (void)chart_reach_ranges(*hit, bb);
      } catch (const stage6_chart_reach_error&) {
        threw_empty = true;
      } catch (const std::exception& e) {
        check(false, std::string("expected the typed empty-range error, got: ") + e.what());
      }
      check(threw_empty, "all-in-at-flop geometry throws stage6_chart_reach_error");
    } else {
      // Fallback (only if no natural all-in empty range exists up to n=10):
      // mutate one logged action to an illegal total and demand a typed
      // std::exception on replay.
      ReachableGeometry mutated = reach3.front();
      mutated.preflop_log.preflop.back().action.target_total = 999999;
      bool threw = false;
      try {
        (void)chart_reach_ranges(mutated, bb);
      } catch (const std::exception& e) {
        threw = true;
        std::printf("[chart-reach] mutated-log replay refused: %s\n", e.what());
      }
      check(threw, "a mutated illegal logged action refuses replay with a std::exception");
    }
  }

  // (9) Wrapper equivalence: the projecting wrapper reproduces the old
  //     enumerator output order-for-order and line-for-line.
  for (const std::size_t n : {std::size_t{2}, std::size_t{3}, std::size_t{6}}) {
    std::vector<std::string> wrapper_lines;
    const std::vector<GeometrySignature> wrapper_sigs =
        enumerate_chart_flop_geometries(n, bb, &wrapper_lines);
    const std::vector<ReachableGeometry> reach = enumerate_chart_flop_reach(n, bb);
    check(wrapper_sigs.size() == reach.size(),
          "wrapper and reach enumerate the same signature count (n=" + std::to_string(n) + ")");
    for (std::size_t i = 0; i < wrapper_sigs.size(); ++i)
      check(wrapper_sigs[i] == reach[i].sig,
            "wrapper signature order matches the reach order (n=" + std::to_string(n) + ")");
    std::vector<std::string> projected_lines;
    for (const ReachableGeometry& rec : reach)
      projected_lines.push_back(canonical_line(rec.preflop_log));
    std::sort(projected_lines.begin(), projected_lines.end());
    projected_lines.erase(std::unique(projected_lines.begin(), projected_lines.end()),
                          projected_lines.end());
    check(projected_lines == wrapper_lines,
          "wrapper origin lines match the projected canonical lines (n=" + std::to_string(n) + ")");
  }

  // Final empirical count table for the full pinned table (n7/n9/n10 are
  // expensive; failures above already pin n=2,3,6).
  std::printf("[chart-reach] full actionable chart-reachable bucket table:\n");
  for (const std::size_t n : {std::size_t{2}, std::size_t{3}, std::size_t{6}, std::size_t{7},
                              std::size_t{9}, std::size_t{10}}) {
    const std::vector<ReachableGeometry> reach = enumerate_chart_flop_reach(n, bb);
    std::vector<GeometrySignature> sigs;
    sigs.reserve(reach.size());
    for (const ReachableGeometry& rec : reach)
      sigs.push_back(rec.sig);
    const std::vector<GeometryBucket> buckets = bucket_geometries(n, bb, sigs);
    std::size_t actionable = 0;
    for (const GeometryBucket& bkt : buckets)
      if (bkt.actionable)
        ++actionable;
    static const std::size_t pinned_full[11] = {0, 0, 1, 3, 0, 0, 12, 15, 0, 21, 24};
    check(actionable == pinned_full[n],
          "pinned full-table actionable count n=" + std::to_string(n));
    std::printf("  n=%zu exact_sigs=%zu actionable_buckets=%zu\n", n, reach.size(), actionable);
  }

  if (failures == 0) {
    std::printf("stage6_chart_reach: all checks passed\n");
    return 0;
  }
  std::fprintf(stderr, "stage6_chart_reach: %d check(s) failed\n", failures);
  return 1;
}
