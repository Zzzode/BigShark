// stage6/geometry_enumerator.cpp — the policy-linked preflop walk that
// enumerates chart-only and chart-UNION-deviation flop geometries. This is
// the only geometry half that reaches the deployed chart policy (through the
// GameState->Ctx adapter), so it stays in bigshark_stage6_eval; the pure
// signature/bucketing half is geometry_core.cpp in bigshark_stage6_core.
//
// The chart-only walk is shared by enumerate_chart_flop_geometries (which
// projects signatures and canonical origin lines) and
// enumerate_chart_flop_reach (which additionally carries the reaching
// HandLog, consumed by the chart-reach-restricted hole-range builder in
// chart_reach.cpp).
#include <algorithm>
#include <array>
#include <bs/policy.hpp>
#include <bs/stage6/adapter.hpp>
#include <bs/stage6/baseline_policy.hpp>
#include <bs/stage6/chart_reach.hpp>
#include <bs/stage6/geometry_enumerator.hpp>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace bs::stage6 {
namespace {

// All 1326 distinct holds as card-id pairs, in a stable order.
std::vector<std::array<int, 2>> all_holdings() {
  std::vector<std::array<int, 2>> out;
  out.reserve(1326);
  for (int a = 0; a < 52; ++a)
    for (int b = a + 1; b < 52; ++b)
      out.push_back({a, b});
  return out;
}

// The distinct concrete (kind,total) chart actions any holding maps to at the
// state, expressed as poker::Actions legal in `state`. Uses the pinned chart
// through the same adapter the baseline uses, but sweeps every holding so the
// SUPPORT (not one sampled hand's action) is enumerated.
std::vector<poker::Action> chart_action_support(const poker::GameState& state, std::size_t seat,
                                                const HandLog& log) {
  std::vector<poker::Action> support;
  std::uint64_t per_decision_seed = 0x9e3779b97f4a7c15ULL;
  bool saw_preflop = false;
  for (const std::array<int, 2>& hole : all_holdings()) {
    const Ctx ctx = adapt_to_ctx(state, seat, hole, log, per_decision_seed++);
    if (ctx.street != "preflop") {
      saw_preflop = false;
      break;
    }
    saw_preflop = true;
    const SourcedDecision sd = evaluatePolicySourced(ctx, RiverBackendHint{false});
    const poker::Action action = map_deployed_decision(state, sd.decision);
    if (std::find(support.begin(), support.end(), action) == support.end())
      support.push_back(action);
  }
  if (!saw_preflop)
    throw std::runtime_error("geometry enumerator reached a non-preflop state");
  // Fold/check/call first, then raises ascending, for a stable audit order.
  auto rank = [](poker::ActionType t) {
    return t == poker::ActionType::Fold    ? 0
           : t == poker::ActionType::Check ? 1
           : t == poker::ActionType::Call  ? 2
                                           : 3;
  };
  std::sort(support.begin(), support.end(), [&](const poker::Action& a, const poker::Action& b) {
    if (rank(a.type) != rank(b.type))
      return rank(a.type) < rank(b.type);
    return a.target_total < b.target_total;
  });
  return support;
}

// The finite R8 deviation grid's aggressive targets at the state (every legal
// fold/check/call are already in chart support or are passive backbone).
std::vector<poker::Chips> deviation_targets(const poker::GameState& state) {
  const poker::LegalActions legal = state.legal();
  if (!legal.aggressive)
    return {};
  static const std::array<std::pair<int, int>, 5> fracs = {
      {{1, 3}, {1, 2}, {3, 4}, {1, 1}, {3, 2}}};
  std::vector<poker::Chips> targets;
  targets.push_back(legal.aggressive->minimum);
  const poker::Chips call = legal.call_amount;
  const poker::Chips pot = state.pot();
  for (const auto& [num, den] : fracs) {
    double raw =
        static_cast<double>(call) + static_cast<double>(num) / static_cast<double>(den) *
                                        (static_cast<double>(pot) + static_cast<double>(call));
    auto t = static_cast<poker::Chips>(raw > 0 ? raw : 1);
    t = std::clamp(t, legal.aggressive->minimum, legal.aggressive->maximum);
    targets.push_back(t);
  }
  targets.push_back(legal.aggressive->maximum);
  std::sort(targets.begin(), targets.end());
  targets.erase(std::unique(targets.begin(), targets.end()), targets.end());
  return targets;
}

// Union of chart support and deviation-grid aggressives, deduplicated.
std::vector<poker::Action> branch_support(const poker::GameState& state, std::size_t seat,
                                          const HandLog& log, bool seat_is_deviator) {
  std::vector<poker::Action> support = chart_action_support(state, seat, log);
  if (seat_is_deviator) {
    for (poker::Chips target : deviation_targets(state)) {
      poker::Action action{state.legal().aggressive->type, target};
      if (std::find(support.begin(), support.end(), action) == support.end())
        support.push_back(action);
    }
    std::sort(support.begin(), support.end(), [](const poker::Action& a, const poker::Action& b) {
      auto rank = [](poker::ActionType t) {
        return t == poker::ActionType::Fold    ? 0
               : t == poker::ActionType::Check ? 1
               : t == poker::ActionType::Call  ? 2
                                               : 3;
      };
      const int ra = rank(a.type);
      const int rb = rank(b.type);
      return ra != rb ? ra < rb : a.target_total < b.target_total;
    });
  }
  return support;
}

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

// The canonical one-action encoding shared by every origin line, kept in one
// place so the projection of enumerate_chart_flop_reach and the lines the
// geometry enumerators emit always match byte-for-byte:
// "s<seat>:<int(action.type)>=<target_total> ".
std::string canonical_action_string(const LoggedAction& a) {
  std::ostringstream os;
  os << "s" << a.seat << ":" << static_cast<int>(a.action.type) << "=" << a.action.target_total
     << " ";
  return os.str();
}

// The canonical line for a full preflop log (concatenated action encodings).
std::string canonical_line_string(const HandLog& log) {
  std::string line;
  for (const LoggedAction& a : log.preflop)
    line += canonical_action_string(a);
  return line;
}

// Builds the flop signature for a state that has just closed preflop (the
// next transition deals the flop). Chips are read from the state directly.
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

// Depth-first over preflop decisions until the flop. When `deviator` < player
// count that one seat gets the deviation grid and every other seat chart
// support; when `deviator` >= player_count the walk is chart-only. Every
// reached flop geometry is appended together with the exact preflop log that
// reached it (duplicate signatures kept: the caller decides how to dedupe).
void walk_reach(poker::GameState state, HandLog log, std::size_t deviator,
                std::vector<ReachableGeometry>* out, int depth, std::size_t* nodes) {
  if (nodes)
    ++*nodes;
  if (depth > 64)
    throw std::runtime_error("flop geometry enumeration exceeded depth bound");
  if (state.phase() == poker::Phase::Deal) {
    // Preflop closed: the next deal begins the flop. Record from the state.
    out->push_back(ReachableGeometry{signature_from_state(state), std::move(log)});
    return;
  }
  if (state.phase() != poker::Phase::Action)
    return;  // folded/showdown without a flop (e.g. all fold) -> no geometry
  const std::size_t seat = *state.actor();
  // At least two live seats must remain for a flop geometry.
  if (state.live_players().size() < 2)
    return;
  const bool seat_is_deviator = deviator < state.player_count() && seat == deviator;
  const std::vector<poker::Action> support = branch_support(state, seat, log, seat_is_deviator);
  for (const poker::Action& action : support) {
    // Folds cannot lead to a multi-live flop if they reduce below two; the
    // transition itself reports that. Explore every branch.
    HandLog next_log = log;
    next_log.preflop.push_back(LoggedAction{seat, action});
    poker::GameState next = state.after_action(seat, action);
    walk_reach(std::move(next), std::move(next_log), deviator, out, depth + 1, nodes);
  }
}

// Orders reached geometries by canonical signature string, ties broken by the
// lexicographically smallest canonical line so unique-per-signature keeps it.
bool reach_less(const ReachableGeometry& a, const ReachableGeometry& b) {
  const std::string sa = a.sig.to_string();
  const std::string sb = b.sig.to_string();
  if (sa != sb)
    return sa < sb;
  return canonical_line_string(a.preflop_log) < canonical_line_string(b.preflop_log);
}

}  // namespace

std::vector<ReachableGeometry> enumerate_chart_flop_reach_hits(std::size_t player_count,
                                                               poker::Chips big_blind) {
  std::vector<ReachableGeometry> all;
  const poker::GameDef def = make_preflop_def(player_count, big_blind);
  std::size_t nodes = 0;
  walk_reach(poker::GameState(def), HandLog{}, player_count, &all, 0, &nodes);
  std::fprintf(stderr, "[geometry] n=%zu chart-only walk_nodes=%zu raw_reaches=%zu\n", player_count,
               nodes, all.size());
  return all;
}

std::vector<ReachableGeometry> enumerate_chart_flop_reach(std::size_t player_count,
                                                          poker::Chips big_blind) {
  std::vector<ReachableGeometry> all = enumerate_chart_flop_reach_hits(player_count, big_blind);
  const std::size_t raw_reaches = all.size();
  std::sort(all.begin(), all.end(), reach_less);
  // Unique per signature: when two walks reached the same geometry retain the
  // lexicographically smallest canonical line (it sorts first within a
  // signature group).
  std::vector<ReachableGeometry> unique;
  unique.reserve(all.size());
  for (const ReachableGeometry& rec : all) {
    if (unique.empty() || !(unique.back().sig == rec.sig))
      unique.push_back(rec);
  }
  // `geometries` is the post-dedupe count the previous chart-only log always
  // carried (kept for log continuity); `reaches` additionally reports the raw
  // walk hits before signature dedupe.
  std::fprintf(stderr, "[geometry] n=%zu chart-only dedupe reaches=%zu geometries=%zu\n",
               player_count, raw_reaches, unique.size());
  return unique;
}

std::vector<GeometrySignature> enumerate_chart_flop_geometries(
    std::size_t player_count, poker::Chips big_blind, std::vector<std::string>* origin_lines) {
  // Thin projection over the log-carrying chart-only walk: one entry per
  // unique signature, canonical lines collected and sorted/deduped exactly as
  // the previous implementation did.
  const std::vector<ReachableGeometry> reach = enumerate_chart_flop_reach(player_count, big_blind);
  std::vector<GeometrySignature> out;
  std::vector<std::string> lines;
  out.reserve(reach.size());
  if (origin_lines)
    lines.reserve(reach.size());
  for (const ReachableGeometry& rec : reach) {
    out.push_back(rec.sig);
    if (origin_lines)
      lines.push_back(canonical_line_string(rec.preflop_log));
  }
  if (origin_lines) {
    std::sort(lines.begin(), lines.end());
    lines.erase(std::unique(lines.begin(), lines.end()), lines.end());
    origin_lines->insert(origin_lines->end(), lines.begin(), lines.end());
  }
  return out;
}

std::vector<GeometrySignature> enumerate_flop_geometries(std::size_t player_count,
                                                         poker::Chips big_blind,
                                                         std::vector<std::string>* origin_lines) {
  std::vector<std::string> lines;
  const poker::GameDef def = make_preflop_def(player_count, big_blind);
  // Run once per canonical deviator seat. A deviator seat that must post a
  // blind still deviates on its later voluntary decisions. Seat 0 (button) is
  // always included (also covers the no-deviator chart-only reach because the
  // grid is a superset of chart support).
  //
  // The previous implementation deduped DURING the walk (a signature was
  // recorded together with its FIRST-encounter origin line); keep that exact
  // behavior here rather than taking the lexicographically smallest line, so
  // the union/deviation matrix and its origin lines are unchanged.
  std::vector<ReachableGeometry> first_hit;
  for (std::size_t deviator = 0; deviator < player_count; ++deviator) {
    std::vector<ReachableGeometry> reached;
    poker::GameState state(def);
    HandLog log;
    std::size_t nodes = 0;
    walk_reach(std::move(state), std::move(log), deviator, &reached, 0, &nodes);
    for (ReachableGeometry& rec : reached) {
      const auto it =
          std::find_if(first_hit.begin(), first_hit.end(),
                       [&](const ReachableGeometry& kept) { return kept.sig == rec.sig; });
      if (it == first_hit.end()) {
        if (origin_lines)
          lines.push_back(canonical_line_string(rec.preflop_log));
        first_hit.push_back(std::move(rec));
      }
    }
    std::fprintf(stderr, "[geometry] n=%zu deviator=%zu walk_nodes=%zu total_sigs=%zu\n",
                 player_count, deviator, nodes, first_hit.size());
  }
  std::sort(first_hit.begin(), first_hit.end(),
            [](const ReachableGeometry& a, const ReachableGeometry& b) {
              return a.sig.to_string() < b.sig.to_string();
            });
  std::vector<GeometrySignature> out;
  out.reserve(first_hit.size());
  for (const ReachableGeometry& rec : first_hit)
    out.push_back(rec.sig);
  if (origin_lines) {
    std::sort(lines.begin(), lines.end());
    lines.erase(std::unique(lines.begin(), lines.end()), lines.end());
    origin_lines->insert(origin_lines->end(), lines.begin(), lines.end());
  }
  return out;
}

}  // namespace bs::stage6
