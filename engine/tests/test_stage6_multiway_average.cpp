// RFC 0008 stage 6 R12 step 7 — multiplayer full-average parity gate.
//
// P1-1 regression test. The unweighted "sums += sigma per own visit" average
// equals the CFR full average only for N=2; for N>=3 the sampled other-players
// reach contributes a history-varying elementary-symmetric factor e_{N-2} that
// biases it. The shipped trainer uses kFull own-reach weighting
// (sums += pi_traverser * sigma at the traverser's own nodes).
//
// This gate validates that choice the way RFC 0006:197-199 mandates: on a
// small ENUMERABLE three-player validation game it compares the production
// streaming external sampler against an INDEPENDENTLY written full-traversal
// CFR (every joint deal and every action with exact probabilities, no shared
// update code). The game is rooted on the RIVER (five public cards, one chip
// behind each seat, eight joint deals from two disjoint combos per seat) so the
// tree has no chance nodes and full enumeration is cheap inside ordinary
// ctest, while the three-player opponent-reach factor that breaks unweighted
// averaging is fully present.
//
// Two observables must agree:
//   * the average policy, key-for-key over (public path, own card bucket);
//   * the general-sum NashConv  Y = sum_i (BR_i - v_i(profile))  computed by
//     exact infoset-consistent best responses on the restricted game: a
//     biased average moves the equilibrium profile and changes Y, not just the
//     row labels.
#include <algorithm>
#include <array>
#include <bs/abstract_tree.hpp>
#include <bs/abstraction.hpp>
#include <bs/behavior_policy.hpp>
#include <bs/game_definition.hpp>
#include <bs/heads_up.hpp>
#include <bs/multiway_sampler.hpp>
#include <bs/prng.hpp>
#include <bs/stage6/frozen_manifest.hpp>
#include <bs/stage6/geometry.hpp>
#include <bs/stage6/infoset_id.hpp>
#include <bs/stage6/public_path.hpp>
#include <bs/stage6/random_streams.hpp>
#include <bs/stage6/trainer.hpp>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <stdexcept>
#include <unordered_map>
#include <vector>

namespace {

using namespace bs::poker;
using namespace bs::stage6;
using bs::abstraction::ActionAbstraction;
using bs::abstraction::CoverSeeds;
using bs::abstraction::SizeSchedule;
using bs::solver::JointDealTable;
using bs::solver::MultiwayWeightedHand;

constexpr std::size_t kSeats = 3;

int failures = 0;
void check(bool c, const char* w) {
  if (!c) {
    std::printf("FAIL: %s\n", w);
    ++failures;
  }
}

ActionAbstraction coarse_action() {
  SizeSchedule s = bs::abstraction::default_size_schedule();
  for (auto& x : s) {
    x.bets = {{1, 2}};
    x.raises = {{1, 1}};
  }
  return ActionAbstraction::declared(s, CoverSeeds::DeclaredOnly);
}

// Shallow river-rooted 3-seat fixture: one chip behind each, 6 dead, five
// public cards. Its coarse tree has 25 nodes and ZERO chance nodes.
GameDef tiny_def() {
  GameDef def{};
  def.player_count = kSeats;
  def.button = 0;
  def.big_blind = 2;
  def.preflop = false;
  def.stacks = {1, 1, 1, 0, 0, 0, 0, 0, 0, 0};
  def.contributions = {2, 2, 2, 0, 0, 0, 0, 0, 0, 0};
  def.pot = 6;
  def.board = {0, 6, 21, 30, 44};
  def.board_size = 5;
  return def;
}

GeometryBucketKey tiny_key() {
  return GeometryBucketKey{3, 3, 3, 3};
}

// Two disjoint hole pairs per seat, all disjoint across seats and off the
// board: exactly eight equal-weight joint deals.
std::vector<std::vector<MultiwayWeightedHand>> tiny_ranges() {
  static const int cards[kSeats][2][2] = {{{1, 7}, {3, 9}}, {{2, 8}, {5, 11}}, {{4, 10}, {12, 13}}};
  std::vector<std::vector<MultiwayWeightedHand>> r(kSeats);
  for (std::size_t i = 0; i < kSeats; ++i)
    for (int j = 0; j < 2; ++j)
      r[i].push_back(MultiwayWeightedHand{{cards[i][j][0], cards[i][j][1]}, 1.0});
  return r;
}

std::vector<HoleCards> deal_holes(const auto& deal) {
  std::vector<HoleCards> holes;
  for (const auto& h : deal.hands)
    holes.push_back(HoleCards{h[0], h[1]});
  return holes;
}

std::uint32_t bucket_for(const TrainingConfig& config, const HoleCards& holes,
                         const std::vector<int>& board) {
  return static_cast<std::uint32_t>(bs::abstraction::card_bucket(config.card_kind, holes, board));
}

// ----------------------------------------------------- independent full CFR

struct FullRow {
  std::vector<Action> actions;
  std::vector<double> regrets;
  std::vector<double> sums;  // vanilla full average: pi_own * sigma
  // External-measure-weighted average: pi_own * pi_opponents * sigma. An
  // external-sampling sweep reaches I with probability pi_opponents(I), so the
  // sampler's per-sweep contribution 1[reach] pi_own sigma has exactly THIS
  // sum as its expectation. Its per-row normalization is what a sealed
  // sampled artifact estimates; it differs from the vanilla average only while
  // pi_opponents(I) changes over iterations, and both coincide at convergence.
  std::vector<double> xsums;
  std::vector<double> sigma() const {
    double pos = 0.0;
    for (double r : regrets)
      pos += std::max(0.0, r);
    std::vector<double> s(actions.size(), 1.0 / actions.size());
    if (pos > 0.0)
      for (std::size_t i = 0; i < actions.size(); ++i)
        s[i] = std::max(0.0, regrets[i]) / pos;
    return s;
  }
};

using FullTable = std::map<AbstractInfosetKey, FullRow>;

// Vanilla full-traversal RM+ for one traverser on one fixed joint deal.
// Opponent actions are ALL enumerated with exact reach weights; the average
// at the traverser's own node is pi_own * sigma (kFull). No chance: the river
// fixture ends at showdown once the street closes.
struct FullCfr {
  const TrainingConfig* config = nullptr;
  FullTable* table = nullptr;
  std::vector<HoleCards> holes;
  std::size_t traverser = 0;
  std::uint64_t geometry_token = 0;

  double terminal(const GameState& st, bool folded) const {
    std::vector<std::array<int, 2>> live;
    if (!folded)
      for (std::size_t s : st.live_players())
        live.push_back(holes[s]);
    const ContributionSettlement set = folded ? st.settle_fold() : st.settle_showdown(live);
    long long conserve = 0;
    for (std::size_t s = 0; s < kSeats; ++s)
      conserve += set.chip_utility[s];
    if (conserve != 0)
      throw std::runtime_error("full-CFR settlement is not zero-sum");
    return static_cast<double>(set.chip_utility[traverser]);
  }

  FullRow& touch(const AbstractInfosetKey& key, const std::vector<Action>& menu) const {
    auto it = table->find(key);
    if (it != table->end()) {
      if (it->second.actions != menu)
        throw std::runtime_error("full-CFR address merged nodes with different menus");
      return it->second;
    }
    return table
        ->emplace(key, FullRow{menu, std::vector<double>(menu.size(), 0.0),
                               std::vector<double>(menu.size(), 0.0),
                               std::vector<double>(menu.size(), 0.0)})
        .first->second;
  }

  double walk(const GameState& st, std::array<double, 10> reach, PublicPath path) const {
    if (st.phase() == Phase::Folded)
      return terminal(st, true);
    if (st.phase() == Phase::Showdown)
      return terminal(st, false);

    const std::size_t actor = *st.actor();
    const std::vector<int> board(st.board().begin(), st.board().end());
    AbstractInfosetKey key;
    key.own_card_bucket = bucket_for(*config, holes[actor], board);
    key.path_hash = path.hash();
    const std::vector<Action> menu = bs::tree::abstract_node_menu(st, config->action, st.legal());
    FullRow& row = touch(key, menu);
    const std::vector<double> sigma = row.sigma();

    if (actor == traverser) {
      double opponents = 1.0;
      for (std::size_t i = 0; i < kSeats; ++i)
        if (i != actor)
          opponents *= reach[i];
      for (std::size_t a = 0; a < menu.size(); ++a) {
        row.sums[a] += reach[actor] * sigma[a];
        row.xsums[a] += reach[actor] * opponents * sigma[a];
      }
    }

    double opponent_reach = 1.0;
    for (std::size_t i = 0; i < kSeats; ++i)
      if (i != actor)
        opponent_reach *= reach[i];

    std::vector<double> child(menu.size(), 0.0);
    for (std::size_t a = 0; a < menu.size(); ++a) {
      std::array<double, 10> next_reach = reach;
      next_reach[actor] *= sigma[a];
      PublicPath next_path = path;
      next_path.on_action(actor, a);
      child[a] = walk(st.after_action(actor, menu[a]), next_reach, next_path);
    }
    double value = 0.0;
    for (std::size_t a = 0; a < menu.size(); ++a)
      value += sigma[a] * child[a];
    if (actor == traverser)
      for (std::size_t a = 0; a < menu.size(); ++a)
        row.regrets[a] = std::max(0.0, row.regrets[a] + opponent_reach * (child[a] - value));
    return value;
  }
};

FrozenArtifactRows seal_full(const FullTable& table, bool external_measure) {
  FrozenArtifactRows out;
  for (const auto& [key, row] : table) {
    const std::vector<double>& src = external_measure ? row.xsums : row.sums;
    double total = 0.0;
    for (double s : src)
      total += s;
    AbstractPolicyRow ar;
    ar.abstract_actions = row.actions;
    ar.visits = 0;
    ar.probabilities.resize(row.actions.size());
    for (std::size_t i = 0; i < row.actions.size(); ++i)
      ar.probabilities[i] = total > 0.0 ? src[i] / total : 1.0 / row.actions.size();
    out.emplace(key, std::move(ar));
  }
  return out;
}

double xmass(const FullTable& table, const AbstractInfosetKey& key) {
  double total = 0.0;
  auto it = table.find(key);
  if (it != table.end())
    for (double s : it->second.xsums)
      total += s;
  return total;
}

// --------------------------------------- exact best response on the tiny game
//
// Infoset-consistent general-sum best response for one player against a fixed
// profile, evaluated as a public-tree recursion over SETS of weighted worlds
// (one world per joint deal). When the BR player acts, worlds sharing the
// player's own card bucket form one infoset partition and take ONE best action
// together; when an opponent acts, each world branches by that opponent's
// profile probability conditioned on that world's private bucket. The BR value
// is the expected chip utility over the eight equal-weight deals.

struct World {
  GameState state;
  std::size_t deal = 0;
  double weight = 0.0;
};

struct Profile {
  const TrainingConfig* config = nullptr;
  const FrozenArtifactRows* rows = nullptr;
  std::uint64_t geometry_token = 0;

  const AbstractPolicyRow* row(const GameState& st, std::size_t actor,
                               const std::vector<HoleCards>& holes, std::uint64_t path_hash) const {
    const std::vector<int> board(st.board().begin(), st.board().end());
    AbstractInfosetKey key;
    key.own_card_bucket = bucket_for(*config, holes[actor], board);
    key.path_hash = path_hash;
    auto it = rows->find(key);
    return it == rows->end() ? nullptr : &it->second;
  }
};

double terminal_world(const World& w, const std::vector<std::vector<HoleCards>>& all_holes,
                      std::size_t player) {
  const auto& holes = all_holes[w.deal];
  const GameState& st = w.state;
  ContributionSettlement set;
  if (st.phase() == Phase::Folded) {
    set = st.settle_fold();
  } else {
    std::vector<std::array<int, 2>> live;
    for (std::size_t s : st.live_players())
      live.push_back(holes[s]);
    set = st.settle_showdown(live);
  }
  return static_cast<double>(set.chip_utility[player]);
}

double profile_value_recurse(std::vector<World> worlds, const Profile& profile,
                             const std::vector<std::vector<HoleCards>>& all_holes,
                             std::size_t player, PublicPath path) {
  // All worlds at a public node share phase/actor (public actions are taken in
  // lockstep); split off the terminal contribution first.
  std::vector<World> live_worlds;
  double acc = 0.0;
  for (World& w : worlds) {
    if (w.state.phase() == Phase::Folded || w.state.phase() == Phase::Showdown)
      acc += w.weight * terminal_world(w, all_holes, player);
    else
      live_worlds.push_back(std::move(w));
  }
  if (live_worlds.empty())
    return acc;

  const GameState& probe = live_worlds.front().state;
  const std::size_t actor = *probe.actor();
  const std::vector<Action> menu =
      bs::tree::abstract_node_menu(probe, profile.config->action, probe.legal());

  if (actor == player) {
    // Partition worlds by the player's own card bucket; weight each action by
    // the player's OWN profile probability (this computes v_i(profile), not the
    // best response).
    std::unordered_map<std::uint32_t, std::vector<World>> partitions;
    for (World& w : live_worlds) {
      const std::vector<int> board(w.state.board().begin(), w.state.board().end());
      const std::uint32_t b = bucket_for(*profile.config, all_holes[w.deal][player], board);
      partitions[b].push_back(std::move(w));
    }
    for (auto& [b, ws] : partitions) {
      const AbstractPolicyRow* r =
          profile.row(ws.front().state, player, all_holes[ws.front().deal], path.hash());
      for (std::size_t a = 0; a < menu.size(); ++a) {
        const double pa = r ? r->probabilities.at(a) : 1.0 / menu.size();
        if (pa <= 0.0)
          continue;
        std::vector<World> branch;
        branch.reserve(ws.size());
        for (const World& w : ws)
          branch.push_back({w.state.after_action(player, menu[a]), w.deal, w.weight * pa});
        PublicPath next = path;
        next.on_action(player, a);
        acc += profile_value_recurse(std::move(branch), profile, all_holes, player, next);
      }
    }
    return acc;
  }

  // Opponent: per-world probability of each public action.
  for (std::size_t a = 0; a < menu.size(); ++a) {
    std::vector<World> branch;
    for (const World& w : live_worlds) {
      const AbstractPolicyRow* r = profile.row(w.state, actor, all_holes[w.deal], path.hash());
      if (r && r->abstract_actions != menu)
        throw std::runtime_error("BR opponent row menu drifted from the shared menu rule");
      const double pa = r ? r->probabilities.at(a) : 1.0 / menu.size();
      if (pa <= 0.0)
        continue;
      branch.push_back({w.state.after_action(actor, menu[a]), w.deal, w.weight * pa});
    }
    if (branch.empty())
      continue;
    PublicPath next = path;
    next.on_action(actor, a);
    acc += profile_value_recurse(std::move(branch), profile, all_holes, player, next);
  }
  return acc;
}

// Best response: same world-set recursion, but at `player`'s nodes each own
// bucket partition takes the single action maximizing its continuation value.
double br_recurse(std::vector<World> worlds, const Profile& profile,
                  const std::vector<std::vector<HoleCards>>& all_holes, std::size_t player,
                  PublicPath path) {
  std::vector<World> live_worlds;
  double acc = 0.0;
  for (World& w : worlds) {
    if (w.state.phase() == Phase::Folded || w.state.phase() == Phase::Showdown)
      acc += w.weight * terminal_world(w, all_holes, player);
    else
      live_worlds.push_back(std::move(w));
  }
  if (live_worlds.empty())
    return acc;

  const GameState& probe = live_worlds.front().state;
  const std::size_t actor = *probe.actor();
  const std::vector<Action> menu =
      bs::tree::abstract_node_menu(probe, profile.config->action, probe.legal());

  if (actor == player) {
    std::unordered_map<std::uint32_t, std::vector<World>> partitions;
    for (World& w : live_worlds) {
      const std::vector<int> board(w.state.board().begin(), w.state.board().end());
      const std::uint32_t b = bucket_for(*profile.config, all_holes[w.deal][player], board);
      partitions[b].push_back(std::move(w));
    }
    for (auto& [b, ws] : partitions) {
      double best = -1e100;
      for (std::size_t a = 0; a < menu.size(); ++a) {
        std::vector<World> branch;
        branch.reserve(ws.size());
        for (const World& w : ws)
          branch.push_back({w.state.after_action(player, menu[a]), w.deal, w.weight});
        PublicPath next = path;
        next.on_action(player, a);
        best = std::max(best, br_recurse(std::move(branch), profile, all_holes, player, next));
      }
      acc += best;
    }
    return acc;
  }

  for (std::size_t a = 0; a < menu.size(); ++a) {
    std::vector<World> branch;
    for (const World& w : live_worlds) {
      const AbstractPolicyRow* r = profile.row(w.state, actor, all_holes[w.deal], path.hash());
      if (r && r->abstract_actions != menu)
        throw std::runtime_error("BR opponent row menu drifted from the shared menu rule");
      const double pa = r ? r->probabilities.at(a) : 1.0 / menu.size();
      if (pa <= 0.0)
        continue;
      branch.push_back({w.state.after_action(actor, menu[a]), w.deal, w.weight * pa});
    }
    if (branch.empty())
      continue;
    PublicPath next = path;
    next.on_action(actor, a);
    acc += br_recurse(std::move(branch), profile, all_holes, player, next);
  }
  return acc;
}

// General-sum NashConv scalar Y = sum_i (BR_i - v_i) on the restricted game.
double nash_conv(const FrozenArtifactRows& rows, const TrainingConfig& config,
                 const JointDealTable& deals, const std::vector<std::vector<HoleCards>>& all_holes,
                 std::array<double, kSeats>* values_out) {
  Profile profile;
  profile.config = &config;
  profile.rows = &rows;
  profile.geometry_token = geometry_bucket_token(tiny_key());

  std::vector<World> roots;
  for (std::size_t d = 0; d < deals.deals.size(); ++d)
    roots.push_back({GameState(tiny_def()), d, deals.deals[d].weight});

  double y = 0.0;
  for (std::size_t p = 0; p < kSeats; ++p) {
    PublicPath path(profile.geometry_token);
    const double vp =
        profile_value_recurse(roots, profile, all_holes, p, PublicPath(profile.geometry_token));
    const double brp = br_recurse(roots, profile, all_holes, p, path);
    if (values_out)
      (*values_out)[p] = vp;
    y += brp - vp;
  }
  return y;
}

// --------------------------------------------------------------------- the gate

struct CheckpointMetrics {
  std::size_t common = 0;
  std::size_t only_full = 0;
  std::size_t only_sampled = 0;
  double weighted_mean = 0.0;
  double y_full = 0.0;
  double y_sampled = 0.0;
};

// Trains BOTH processes from scratch for `iterations` and compares their sealed
// policies. Deterministic for (iterations, master_seed).
CheckpointMetrics run_checkpoint(const TrainingConfig& base_config, const GameDef& def,
                                 const JointDealTable& deals,
                                 const std::vector<std::vector<HoleCards>>& all_holes,
                                 std::uint64_t geometry_token, std::uint64_t iterations,
                                 std::uint64_t master_seed) {
  TrainingConfig config = base_config;
  config.iterations = iterations;

  FullTable full_table;
  for (std::uint64_t it = 0; it < iterations; ++it) {
    for (std::size_t d = 0; d < deals.deals.size(); ++d) {
      for (std::size_t tr = 0; tr < kSeats; ++tr) {
        FullCfr cfr;
        cfr.config = &config;
        cfr.table = &full_table;
        cfr.holes = all_holes[d];
        cfr.traverser = tr;
        cfr.geometry_token = geometry_token;
        std::array<double, 10> reach{1, 1, 1, 1, 1, 1, 1, 1, 1, 1};
        cfr.walk(GameState(def), reach, PublicPath(geometry_token));
      }
    }
  }
  const FrozenArtifactRows full_avg = seal_full(full_table, /*external_measure=*/false);
  const FrozenArtifactRows full_xavg = seal_full(full_table, /*external_measure=*/true);

  std::map<AbstractInfosetKey, TrainerRow> sampled;
  for (std::uint64_t it = 0; it < iterations; ++it) {
    for (std::size_t d = 0; d < deals.deals.size(); ++d) {
      for (std::size_t tr = 0; tr < kSeats; ++tr) {
        bs::SplitMix64 action_rng =
            derive_stream(master_seed, StreamPurpose::TrainOpponentAction, it,
                          static_cast<std::uint64_t>(d * kSeats + tr) + 1);
        bs::SplitMix64 chance_rng(0x424242);  // never consumed: river fixture
        debug_run_fixed_world_sweep(config, def, geometry_token, all_holes[d], tr, action_rng,
                                    chance_rng, sampled);
      }
    }
  }
  FrozenArtifactRows sampled_avg;
  for (const auto& [key, row] : sampled)
    sampled_avg.emplace(key, row.average_row());

  CheckpointMetrics m;
  double wnum = 0.0, wden = 0.0;
  for (const auto& [key, frow] : full_xavg) {
    auto it = sampled_avg.find(key);
    if (it == sampled_avg.end()) {
      ++m.only_full;
      continue;
    }
    if (it->second.abstract_actions != frow.abstract_actions) {
      check(false, "common key carries the same menu");
      continue;
    }
    const double mass = xmass(full_table, key);
    for (std::size_t i = 0; i < frow.probabilities.size(); ++i) {
      wnum += mass * std::abs(frow.probabilities[i] - it->second.probabilities[i]);
    }
    wden += mass;
    ++m.common;
  }
  for (const auto& [key, row] : sampled_avg)
    if (!full_xavg.contains(key))
      ++m.only_sampled;
  m.weighted_mean = wden ? wnum / wden : 9.0;

  m.y_full = nash_conv(full_avg, config, deals, all_holes, nullptr);
  m.y_sampled = nash_conv(sampled_avg, config, deals, all_holes, nullptr);
  std::printf(
      "[multiway-parity] it=%6llu rows f=%zu s=%zu common=%zu only_f=%zu only_s=%zu "
      "weighted_mean=%.5f NashConv full=%.5f sampled=%.5f |dY|=%.5f\n",
      static_cast<unsigned long long>(iterations), full_xavg.size(), sampled_avg.size(), m.common,
      m.only_full, m.only_sampled, m.weighted_mean, m.y_full, m.y_sampled,
      std::abs(m.y_full - m.y_sampled));
  return m;
}

int test_multiway_full_average_parity() {
  TrainingConfig config;
  config.action = coarse_action();
  config.card_kind = bs::abstraction::CardBucketKind::CategoryTiersV1;
  config.geometry_matrix_hash = 0x5151515151515151ULL;
  config.chart_digest_sha256 = "multiway-parity";

  const GameDef def = tiny_def();
  const auto ranges = tiny_ranges();
  const std::vector<int> board(def.board.begin(), def.board.begin() + def.board_size);
  const JointDealTable deals = bs::solver::enumerate_joint_deals(ranges, board);
  check(deals.deals.size() == 8, "the river fixture has exactly eight joint deals");
  std::vector<std::vector<HoleCards>> all_holes;
  for (const auto& d : deals.deals)
    all_holes.push_back(deal_holes(d));
  const std::uint64_t geometry_token = geometry_bucket_token(tiny_key());
  const std::uint64_t master_seed = std::strtoull(std::getenv("PARITY_SEED") ?: "777", nullptr, 10);

  const CheckpointMetrics early =
      run_checkpoint(config, def, deals, all_holes, geometry_token, 12'000, master_seed);
  const CheckpointMetrics late =
      run_checkpoint(config, def, deals, all_holes, geometry_token, 30'000, master_seed);

  // Structural identity: the sampler addresses exactly the full game's
  // information sets with the same menus, on both checkpoints.
  for (const CheckpointMetrics& m : {early, late}) {
    check(m.only_full == 0, "the sampler reaches every full-traversal information set");
    check(m.only_sampled == 0, "the sampler creates no information set the full tree lacks");
    check(m.common >= 10, "the tiny game exposes a substantial shared information-set set");
  }

  // Finite-time estimator agreement against the correct external-measure
  // target (pi_own*pi_opponents weighting), mass-weighted so an infoset behind
  // a one-in-a-hundred-thousand opponent deviation cannot dominate.
  //
  // SCOPE: the precise kFull weighting is pinned at 1e-9 by the single-sweep
  // algebra battery (test_stage6_trainer, both cursors), and the unweighted
  // sums+=sigma mutation goes RED there (got 0.5 vs ref 0.25). THIS aggregate
  // gate does NOT discriminate that mutation: the tiny fixture converges to
  // near-PURE equilibria, so pi_opponents is ~{0,1}, the e_{N-2} bias vanishes
  // and the sealed NashConv is unchanged (verified across seeds 1/43/777). Its
  // job is the end-to-end N>=3 property the algebra gate cannot show —
  // identical information-set key sets, external-measure row agreement, and
  // both independent processes reaching the same near-equilibrium NashConv.
  check(late.weighted_mean < 0.03,
        "sampled average matches the external-measure full reference (mass-weighted <0.03)");

  // Economic invariant: both independently produced SEALED profiles reach the
  // same near-equilibrium of the restricted game, and it tightens with more
  // iterations. This is the general-sum NashConv scalar R11 uses later.
  check(late.y_full < 0.01, "the full-CFR profile is near equilibrium at the late checkpoint");
  check(late.y_sampled < 0.01,
        "the sampled trainer profile is near equilibrium at the late checkpoint");
  check(std::abs(late.y_full - late.y_sampled) < 0.005,
        "sampled and full profiles have the same NashConv (<0.005 chip)");
  check(late.y_sampled < early.y_sampled, "sampled NashConv contracts with more iterations");
  check(late.y_full < early.y_full, "full-CFR NashConv contracts with more iterations");
  return 0;
}

}  // namespace

int main() {
  failures += test_multiway_full_average_parity();
  if (failures) {
    std::printf("STAGE 6 MULTIWAY AVERAGE PARITY FAILED: %d\n", failures);
    return 1;
  }
  std::puts("STAGE 6 MULTIWAY AVERAGE PARITY PASSED");
  return 0;
}
