// stage6/chart_reach.cpp — build chart-reach-restricted preflop hole ranges
// by replaying a reaching preflop HandLog through the pinned chart path.
//
// The replay is deliberately the SAME pinned path geometry_enumerator.cpp's
// chart_action_support walks at every branch node: adapt_to_ctx(state, seat,
// hole, prefix_log, seed) with the seed counter starting at
// 0x9e3779b97f4a7c15 and incremented per holding, then
// evaluatePolicySourced(ctx, RiverBackendHint{false}) and
// map_deployed_decision. The chart decision itself never reads the seed
// preflop; the counter is kept purely for byte-for-byte path parity.
#include <algorithm>
#include <array>
#include <bs/policy.hpp>
#include <bs/stage6/adapter.hpp>
#include <bs/stage6/baseline_policy.hpp>
#include <bs/stage6/chart_reach.hpp>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace bs::stage6 {
namespace {

namespace poker = bs::poker;

// The uniform 100 BB, button-at-0 preflop fixture. Must stay identical to the
// private fixture in geometry_enumerator.cpp: a replayed log only stays legal
// against the exact chip frame the walk enumerated it from.
poker::GameDef make_preflop_def(std::size_t n, poker::Chips bb) {
  poker::GameDef def{};
  def.player_count = n;
  def.button = 0;
  def.big_blind = bb;
  def.preflop = true;
  for (std::size_t i = 0; i < n; ++i)
    def.stacks[i] = 100 * bb;
  std::array<poker::Chips, 10> blinds{};
  if (n == 2) {
    blinds[0] = bb / 2;
    blinds[1] = bb;
  } else {
    blinds[(0 + 1) % n] = bb / 2;
    blinds[(0 + 2) % n] = bb;
  }
  def.blinds_posted = blinds;
  poker::Chips pot = 0;
  for (poker::Chips b : blinds)
    pot += b;
  def.pot = pot;
  def.board = {-1, -1, -1, -1, -1};
  return def;
}

// All 1326 distinct holds as card-id pairs, in the same stable a<b order the
// enumerator uses.
std::vector<std::array<int, 2>> all_holdings() {
  std::vector<std::array<int, 2>> out;
  out.reserve(1326);
  for (int a = 0; a < 52; ++a)
    for (int b = a + 1; b < 52; ++b)
      out.push_back({a, b});
  return out;
}

// Builds the flop signature of a state that has just closed preflop. Same
// public-chip construction as the enumerator's signature_from_state.
GeometrySignature signature_from_state(const poker::GameState& state) {
  GeometrySignature sig;
  sig.player_count = state.def().player_count;
  sig.pot = state.pot();
  for (std::size_t p = 0; p < sig.player_count; ++p) {
    sig.stacks[p] = state.players()[p].stack;
    sig.contributed[p] = state.players()[p].contributed;
    if (!state.players()[p].folded)
      sig.live.push_back(p);
  }
  return sig;
}

bool combo_less(const std::array<int, 2>& a, const std::array<int, 2>& b) {
  return a[0] != b[0] ? a[0] < b[0] : a[1] < b[1];
}

}  // namespace

std::vector<std::vector<bs::solver::MultiwayWeightedHand>> chart_reach_ranges(
    const ReachableGeometry& rec, poker::Chips big_blind) {
  const std::vector<std::array<int, 2>> holdings = all_holdings();
  const poker::GameDef def = make_preflop_def(rec.sig.player_count, big_blind);
  poker::GameState state(def);

  // Ranges are conditioned only for seats that reach the flop; folded seats
  // never get a vector, so their (possibly folded) decisions are not intersected.
  std::array<bool, 10> is_live{};
  for (std::size_t s : rec.sig.live) {
    if (s >= 10)
      throw std::invalid_argument("chart reach replay: live seat index out of range");
    is_live[s] = true;
  }
  // surviving[seat][holding index], starting at the full 1326 and intersected
  // at every one of that seat's own decisions.
  std::array<std::vector<bool>, 10> surviving;
  for (std::size_t s : rec.sig.live)
    surviving[s].assign(holdings.size(), true);

  // The prefix log is the actions already applied to `state`, exactly as the
  // enumerator's walk passes its accumulated log into chart_action_support.
  HandLog prefix;
  for (const LoggedAction& logged : rec.preflop_log.preflop) {
    if (state.phase() != poker::Phase::Action)
      throw std::invalid_argument(
          "chart reach replay desync: expected an action phase before replaying logged action");
    if (state.street() != poker::Street::Preflop)
      throw std::invalid_argument(
          "chart reach replay desync: logged action does not sit on a preflop state");
    const std::optional<std::size_t> actor = state.actor();
    if (!actor || *actor != logged.seat)
      throw std::invalid_argument(
          "chart reach replay desync: logged seat is not the state's acting seat");

    // Same seed convention as chart_action_support: reset at every decision
    // node and increment per holding.
    std::uint64_t per_decision_seed = 0x9e3779b97f4a7c15ULL;
    std::vector<unsigned char> matches(holdings.size(), 0);
    bool any_holding_matches = false;
    for (std::size_t hi = 0; hi < holdings.size(); ++hi) {
      const Ctx ctx = adapt_to_ctx(state, logged.seat, holdings[hi], prefix, per_decision_seed++);
      if (ctx.street != "preflop")
        throw std::invalid_argument(
            "chart reach replay desync: adapter did not classify the state as preflop");
      const SourcedDecision sd = evaluatePolicySourced(ctx, RiverBackendHint{false});
      const poker::Action chart_action = map_deployed_decision(state, sd.decision);
      if (chart_action == logged.action) {
        matches[hi] = 1;
        any_holding_matches = true;
      }
    }
    if (!any_holding_matches)
      throw std::invalid_argument(
          "chart reach replay: logged action is outside the chart support for all 1326 "
          "holdings at its state");

    // Condition ONLY this seat on its own decision. Other seats' surviving
    // sets are untouched; inter-seat card compatibility belongs to the joint
    // dealer.
    if (is_live[logged.seat]) {
      for (std::size_t hi = 0; hi < holdings.size(); ++hi)
        if (!matches[hi])
          surviving[logged.seat][hi] = false;
    }

    state = state.after_action(logged.seat, logged.action);
    prefix.preflop.push_back(logged);
  }

  // The walk records a geometry exactly at the Deal phase that closes
  // preflop, before any flop card is dealt.
  if (state.phase() != poker::Phase::Deal || state.board().size() != 0)
    throw std::invalid_argument(
        "chart reach replay desync: the log does not close preflop into the flop deal");
  if (state.live_players().size() < 2)
    throw std::invalid_argument(
        "chart reach replay desync: fewer than two live seats at the flop deal");

  const GeometrySignature reached = signature_from_state(state);
  if (!(reached == rec.sig))
    throw std::invalid_argument(
        "chart reach replay desync: replayed flop signature does not match the recorded one");

  std::vector<std::vector<bs::solver::MultiwayWeightedHand>> ranges;
  ranges.reserve(rec.sig.live.size());
  for (std::size_t s : rec.sig.live) {
    std::vector<bs::solver::MultiwayWeightedHand> range;
    for (std::size_t hi = 0; hi < holdings.size(); ++hi)
      if (surviving[s][hi])
        range.push_back(bs::solver::MultiwayWeightedHand{holdings[hi], 1.0});
    if (range.empty())
      throw stage6_chart_reach_error("chart reach: live seat " + std::to_string(s) +
                                     " has an empty chart-conditioned range at geometry " +
                                     rec.sig.to_string());
    ranges.push_back(std::move(range));
  }
  return ranges;
}

std::vector<std::vector<bs::solver::MultiwayWeightedHand>> chart_reach_union_member_ranges(
    const GeometryBucketKey& key, const std::vector<ChartReachMemberSets>& members) {
  if (members.empty())
    throw std::invalid_argument("chart reach union: bucket " + key.to_string() + " has no members");
  for (const ChartReachMemberSets& m : members) {
    if (m.live.size() != key.live_count)
      throw std::invalid_argument("chart reach union: member " + m.label + " live-list size (" +
                                  std::to_string(m.live.size()) +
                                  ") does not match bucket live_count " +
                                  std::to_string(key.live_count));
    if (m.card_sets.size() != m.live.size())
      throw std::invalid_argument("chart reach union: member " + m.label +
                                  " has fewer per-seat card sets than live seats");
  }

  // Fail-closed invariant: members that share an identical live list are the
  // SAME reduced game reached through the SAME position layout, so their
  // per-reduced-seat surviving card sets must agree exactly. A disagreement
  // would mean silently averaging differently conditioned ranges.
  for (std::size_t i = 0; i < members.size(); ++i) {
    for (std::size_t j = i + 1; j < members.size(); ++j) {
      if (members[i].live != members[j].live)
        continue;
      if (members[i].card_sets.size() != members[j].card_sets.size())
        throw stage6_chart_reach_error(
            "chart reach bucket " + key.to_string() + " members " + members[i].label + " and " +
            members[j].label + " share a live set but disagree on surviving card-set sizes");
      for (std::size_t seat = 0; seat < members[i].card_sets.size(); ++seat) {
        const std::vector<std::array<int, 2>>& a = members[i].card_sets[seat];
        const std::vector<std::array<int, 2>>& b = members[j].card_sets[seat];
        if (a != b)
          throw stage6_chart_reach_error(
              "chart reach bucket " + key.to_string() + " members " + members[i].label + " and " +
              members[j].label +
              " share a live set but disagree on the surviving card set of reduced seat " +
              std::to_string(seat));
      }
    }
  }

  std::vector<std::vector<std::array<int, 2>>> merged(key.live_count);
  for (const ChartReachMemberSets& m : members)
    for (std::size_t seat = 0; seat < key.live_count; ++seat)
      for (const std::array<int, 2>& combo : m.card_sets[seat])
        merged[seat].push_back(combo);

  std::vector<std::vector<bs::solver::MultiwayWeightedHand>> ranges;
  ranges.reserve(key.live_count);
  for (std::size_t seat = 0; seat < key.live_count; ++seat) {
    std::vector<std::array<int, 2>>& combos = merged[seat];
    std::sort(combos.begin(), combos.end(), combo_less);
    combos.erase(std::unique(combos.begin(), combos.end()), combos.end());
    if (combos.empty())
      throw stage6_chart_reach_error("chart reach bucket " + key.to_string() +
                                     " unions to an empty range at reduced seat " +
                                     std::to_string(seat));
    std::vector<bs::solver::MultiwayWeightedHand> range;
    range.reserve(combos.size());
    for (const std::array<int, 2>& combo : combos)
      range.push_back(bs::solver::MultiwayWeightedHand{combo, 1.0});
    ranges.push_back(std::move(range));
  }
  return ranges;
}

std::vector<std::vector<bs::solver::MultiwayWeightedHand>> chart_reach_bucket_ranges(
    const GeometryBucket& bucket, const std::vector<ReachableGeometry>& reach,
    poker::Chips big_blind) {
  std::unordered_map<std::string, const ReachableGeometry*> index;
  index.reserve(reach.size() * 2 + 1);
  for (const ReachableGeometry& rec : reach)
    index.emplace(rec.sig.to_string(), &rec);

  std::vector<ChartReachMemberSets> member_sets;
  member_sets.reserve(bucket.members.size());
  for (const GeometrySignature& member_sig : bucket.members) {
    const auto it = index.find(member_sig.to_string());
    if (it == index.end())
      throw std::runtime_error(
          "chart reach bucket member signature is absent from the chart-only reach: " +
          member_sig.to_string());
    const ReachableGeometry& rec = *it->second;
    const std::vector<std::vector<bs::solver::MultiwayWeightedHand>> ranges =
        chart_reach_ranges(rec, big_blind);
    ChartReachMemberSets sets;
    sets.label = member_sig.to_string();
    sets.live = rec.sig.live;
    sets.card_sets.reserve(ranges.size());
    for (const std::vector<bs::solver::MultiwayWeightedHand>& range : ranges) {
      std::vector<std::array<int, 2>> combos;
      combos.reserve(range.size());
      for (const bs::solver::MultiwayWeightedHand& hand : range)
        combos.push_back(hand.cards);
      // chart_reach_ranges already emits sorted combos; normalize anyway so
      // the same-liveset comparison never depends on producer order.
      std::sort(combos.begin(), combos.end(), combo_less);
      sets.card_sets.push_back(std::move(combos));
    }
    member_sets.push_back(std::move(sets));
  }
  return chart_reach_union_member_ranges(bucket.key, member_sets);
}

}  // namespace bs::stage6
