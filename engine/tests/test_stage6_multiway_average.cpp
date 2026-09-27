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
#include <bs/eval.hpp>
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
  // Deliberately BIASED per-visit reference: sigma once per own-node touch
  // with NO reach weight. kFull (sums) is unbiased; vsums is the wrong N>=3
  // estimator, pinned so the mixed-equilibrium gate proves the sampler
  // discriminates kFull from per-visit.
  std::vector<double> vsums;
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
        row.vsums[a] += sigma[a];
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

// Biased per-visit reference profile (sums += sigma, no own-reach weight).
FrozenArtifactRows seal_per_visit(const FullTable& table) {
  FrozenArtifactRows out;
  for (const auto& [key, row] : table) {
    double total = 0.0;
    for (double s : row.vsums)
      total += s;
    AbstractPolicyRow ar;
    ar.abstract_actions = row.actions;
    ar.visits = 0;
    ar.probabilities.resize(row.actions.size());
    for (std::size_t i = 0; i < row.actions.size(); ++i)
      ar.probabilities[i] = total > 0.0 ? row.vsums[i] / total : 1.0 / row.actions.size();
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
double nash_conv_on(const GameDef& def, std::uint64_t geometry_token,
                    const FrozenArtifactRows& rows, const TrainingConfig& config,
                    const JointDealTable& deals,
                    const std::vector<std::vector<HoleCards>>& all_holes) {
  Profile profile;
  profile.config = &config;
  profile.rows = &rows;
  profile.geometry_token = geometry_token;

  std::vector<World> roots;
  for (std::size_t d = 0; d < deals.deals.size(); ++d)
    roots.push_back({GameState(def), d, deals.deals[d].weight});

  double y = 0.0;
  for (std::size_t p = 0; p < kSeats; ++p) {
    PublicPath path(profile.geometry_token);
    const double vp =
        profile_value_recurse(roots, profile, all_holes, p, PublicPath(profile.geometry_token));
    const double brp = br_recurse(roots, profile, all_holes, p, path);
    y += brp - vp;
  }
  return y;
}

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

namespace mixgame {

// --- R11 item 4, revised: the N>=3 own-reach weighting pinned TWO ways.
//
// The shipped external-sampling trainer accumulates the kFull average
//   sums[a] += pi_own(I) * sigma(I,a)           (AverageWeighting::OwnReach)
// at every traverser node; the deliberately biased alternative is
//   sums[a] += sigma(I,a)                        (AverageWeighting::PerVisit).
// At a converged equilibrium sigma stops changing, so at every row the
// opponent reach pi_-i(I) is constant across iterations; after per-row
// normalization the vanilla, external-measure, and per-visit averages seal the
// SAME equilibrium policy. kFull and per-visit can therefore differ ONLY as a
// finite-time, history-varying effect, and that effect carries no systematic
// NashConv sign. Two rigorous gates replace the earlier (unattainable)
// "signed exploitability gap" claim:
//
//   Part 1 - a deterministic estimator-identity gate pinning the exact
//   production ternary on a pinned opponent mix. The sealed target row equals
//   the closed forms (10/11,1/11) for kFull and (1/2,1/2) for per-visit, to
//   1e-9; a mutation replacing pi_own*sigma by sigma collapses kFull onto the
//   per-visit vector and goes RED.
//
//   Part 2 - a vanishing-at-convergence gate: natural full CFR (all seats
//   traversed) on a small genuinely-mixed 3-player river game reaches a
//   near-equilibrium (general-sum NashConv < 0.02) whose sealed kFull and
//   per-visit full references agree under external-reach mass weighting
//   (< 0.01). This is full-CFR only; late-checkpoint production-sampler
//   agreement is pinned separately by the parity gate above.
//
// Both run on the SAME compact river game: seat0 is an all-in, never-acting
// dummy (the e_{N-2} opponent-reach factor must come from a MIXING live seat,
// not the dummy), and seats 1 (B) and 2 (C) play a nine-node game.

// Board 2s 3d 6h 8d Qs. B acts first, then C. With two chips behind and a
// three-chip pot the half-pot bet is exactly the all-in, so the coarse tree is
// the nine-node game  check/bet -> fold/call  with no raises and no chance.
constexpr std::array<int, 5> kBoard = {0, 6, 17, 26, 40};  // 2s 3d 6h 8d Qs

// Strength ordering, asserted with bs::evaluate below:
//   B-set(88) > C-two-pair(6Q) > B-top-pair(AQ) > B-air(AJ) > C-air(K9) > dummy.
// seat0 dummy (all-in, always loses): Jc 9c
const std::array<HoleCards, 1> kDummy{{{{39, 23}}}};
// seat1 B: set {8s,8h}, top pair {Qh,Ac}, air {As,Jh}
const std::array<HoleCards, 3> kB{{{{24, 25}}, {{41, 51}}, {{48, 37}}}};
// seat2 C: two pair {6d,Qc}, air {Kd,9h}
const std::array<HoleCards, 2> kC{{{{18, 43}}, {{50, 21}}}};

GameDef mix_def() {
  GameDef def{};
  def.player_count = 3;
  def.button = 0;
  def.big_blind = 2;
  def.preflop = false;
  def.stacks = {0, 2, 2, 0, 0, 0, 0, 0, 0, 0};
  def.contributions = {1, 1, 1, 0, 0, 0, 0, 0, 0, 0};
  def.pot = 3;
  def.board = {kBoard[0], kBoard[1], kBoard[2], kBoard[3], kBoard[4]};
  def.board_size = 5;
  return def;
}

GeometryBucketKey mix_key() {
  // 3 players, ~1.5 bb pot, 3 live, 2 acting.
  return GeometryBucketKey{3, 2, 3, 2};
}

ActionAbstraction mix_action() {
  SizeSchedule s = bs::abstraction::default_size_schedule();
  for (auto& x : s) {
    x.bets = {{1, 2}};
    x.raises = {{1, 1}};
  }
  return ActionAbstraction::declared(s, CoverSeeds::DeclaredOnly);
}

// The six joint worlds (1 dummy combo x 3 B combos x 2 C combos), weight 1.
JointDealTable mix_deals() {
  JointDealTable table;
  for (const HoleCards& hb : kB)
    for (const HoleCards& hc : kC) {
      bs::solver::MultiwayDeal d;
      d.weight = 1.0;
      d.hands = {kDummy[0], hb, hc};
      table.deals.push_back(d);
    }
  return table;
}

// --------------------------------------------------------------- shared tools

const std::uint64_t kToken = geometry_bucket_token(mix_key());

// Converges one independent full-CFR table for `iterations` over all worlds.
FullTable converge_full(const GameDef& def, const TrainingConfig& config,
                        const std::vector<std::vector<HoleCards>>& all_holes,
                        std::size_t iterations) {
  FullTable table;
  for (std::size_t it = 0; it < iterations; ++it)
    for (std::size_t d = 0; d < all_holes.size(); ++d)
      for (std::size_t tr = 0; tr < kSeats; ++tr) {
        FullCfr cfr;
        cfr.config = &config;
        cfr.table = &table;
        cfr.holes = all_holes[d];
        cfr.traverser = tr;
        cfr.geometry_token = kToken;
        std::array<double, 10> reach{1, 1, 1, 1, 1, 1, 1, 1, 1, 1};
        cfr.walk(GameState(def), reach, PublicPath(kToken));
      }
  return table;
}

// True when some REACHED row seals a strict interior mix: for a two-action
// row its first-action mass is inside (lo, hi); larger menus keep every mass
// in range. Positive external-reach mass is required so an unreached row
// sealed to the uniform default can never satisfy the claim vacuously.
bool has_interior_row(const FullTable& full, const FrozenArtifactRows& rows, double lo, double hi) {
  for (const auto& [key, row] : rows) {
    auto fit = full.find(key);
    if (fit == full.end())
      continue;
    double xmass = 0.0;
    for (double v : fit->second.xsums)
      xmass += v;
    if (xmass <= 0.0)
      continue;
    if (row.abstract_actions.size() < 2)
      continue;
    bool interior = true;
    for (double q : row.probabilities)
      if (!(q > lo && q < hi))
        interior = false;
    if (row.probabilities.size() == 2)
      interior = row.probabilities[0] > lo && row.probabilities[0] < hi;
    if (interior)
      return true;
  }
  return false;
}

// ------------------------------------------------------------- Part 1 gate
//
// One traverser (B=seat1). C never traverses, so its regrets are never updated
// and every C row keeps the default uniform strategy: C's V row after a B
// check is [Check,Bet] at a permanent 0.5/0.5 mix, so the B fold/call row I is
// reached with opponent reach pi_-i(I)=0.5 and, critically, the SAME sampled
// check/bet indicator on matching (sweep, world) indices in both phases.
//
// Immediately before EVERY world sweep we overwrite B's regrets:
//   * at its root U ([Check,Bet-all-in])
//   * at I ([Fold,Call]) for its AIR hand, the single pinned row key.
// Phase A: U (10,0) -> own check-reach r_A=1.0, I (10,0) -> sigma_A=(1,0).
// Phase B: U (1,9)  -> own check-reach r_B=0.1, I (0,10) -> sigma_B=(0,1).
// The accumulating sums survive across phases; the per-world RNG is keyed only
// by (sweep, world), so the set of C-bet visits to I is identical in the two
// phases with equal mass m. The sealed pinned row is therefore EXACTLY
//   per-visit : (m*(1,0)+m*(0,1))/(2m)        = (1/2,1/2)
//   kFull     : (1*(1,0)+0.1*(0,1))/(1+0.1)    = (10/11,1/11).
namespace part1 {

constexpr std::size_t kTraverser = 1;  // B
constexpr std::size_t kPerPhase = 2000;
constexpr std::size_t kAirB = 2;  // kB index whose I row is pinned

std::uint32_t bucket_of(const TrainingConfig& config, const HoleCards& h) {
  const std::vector<int> board(kBoard.begin(), kBoard.end());
  return static_cast<std::uint32_t>(bs::abstraction::card_bucket(config.card_kind, h, board));
}

struct Keys {
  std::vector<AbstractInfosetKey> u;  // B root rows, one per B bucket
  AbstractInfosetKey i;               // B air fold/call row after check->bet
};

Keys discover_keys(const TrainingConfig& config) {
  Keys keys;
  const PublicPath root(kToken);
  auto make_key = [](std::uint32_t bucket, std::uint64_t path) {
    AbstractInfosetKey k;
    k.own_card_bucket = bucket;
    k.path_hash = path;
    return k;
  };
  for (const HoleCards& h : kB)
    keys.u.push_back(make_key(bucket_of(config, h), root.hash()));
  PublicPath ip(kToken);
  ip.on_action(1, 0);  // B checks (ordinal 0 at U)
  ip.on_action(2, 1);  // C bets all-in (ordinal 1 at V)
  keys.i = make_key(bucket_of(config, kB[kAirB]), ip.hash());
  return keys;
}

void force_row(std::map<AbstractInfosetKey, TrainerRow>& rows, const AbstractInfosetKey& key,
               const std::vector<Action>& menu, const std::vector<double>& regrets) {
  auto it = rows.find(key);
  if (it == rows.end()) {
    TrainerRow row;
    row.actions = menu;
    row.regrets.assign(menu.size(), 0.0);
    row.sums.assign(menu.size(), 0.0);
    it = rows.emplace(key, std::move(row)).first;
  }
  it->second.actions = menu;
  it->second.regrets = regrets;
}

FrozenArtifactRows build_profile(const GameDef& def, const TrainingConfig& base,
                                 const std::vector<std::vector<HoleCards>>& all_holes,
                                 AverageWeighting weighting, std::uint64_t master_seed,
                                 const Keys& keys, const std::vector<Action>& menu_u,
                                 const std::vector<Action>& menu_i) {
  TrainingConfig config = base;
  config.average_weighting = weighting;
  std::map<AbstractInfosetKey, TrainerRow> rows;

  for (int phase = 0; phase < 2; ++phase) {
    // RM+ positive regrets map directly onto the intended current strategy.
    const std::vector<double> ru =
        phase == 0 ? std::vector<double>{10.0, 0.0} : std::vector<double>{1.0, 9.0};
    const std::vector<double> ri =
        phase == 0 ? std::vector<double>{10.0, 0.0} : std::vector<double>{0.0, 10.0};
    for (std::size_t sweep = 0; sweep < kPerPhase; ++sweep)
      for (std::size_t d = 0; d < all_holes.size(); ++d) {
        // Force immediately before EVERY world sweep, at every B root and at
        // the pinned I row; regret updates on return from this world are
        // overwritten before the next.
        for (const AbstractInfosetKey& k : keys.u)
          force_row(rows, k, menu_u, ru);
        force_row(rows, keys.i, menu_i, ri);

        // Keyed by (sweep, world) only, so phase A and phase B observe the
        // identical opponent check/bet sequence and hence identical I visits.
        bs::SplitMix64 action_rng = derive_stream(master_seed, StreamPurpose::TrainOpponentAction,
                                                  sweep, static_cast<std::uint64_t>(d) + 1);
        bs::SplitMix64 chance_rng(0x4d4958);  // never consumed: river fixture
        debug_run_fixed_world_sweep(config, def, kToken, all_holes[d], kTraverser, action_rng,
                                    chance_rng, rows);
      }
  }

  FrozenArtifactRows out;
  for (const auto& [key, row] : rows)
    out.emplace(key, row.average_row());
  return out;
}

int run(const GameDef& def, const TrainingConfig& config,
        const std::vector<std::vector<HoleCards>>& all_holes) {
  int local = 0;
  const Keys keys = discover_keys(config);
  const std::vector<Action> menu_u{Action{ActionType::Check}, Action{ActionType::Bet, 2}};
  const std::vector<Action> menu_i{Action{ActionType::Fold}, Action{ActionType::Call}};

  for (std::uint64_t seed : {1ULL, 43ULL, 777ULL, 424242ULL}) {
    const FrozenArtifactRows kf = build_profile(def, config, all_holes, AverageWeighting::OwnReach,
                                                seed, keys, menu_u, menu_i);
    const FrozenArtifactRows pv = build_profile(def, config, all_holes, AverageWeighting::PerVisit,
                                                seed, keys, menu_u, menu_i);

    auto at = [](const FrozenArtifactRows& rows,
                 const AbstractInfosetKey& k) -> const std::vector<double>& {
      auto it = rows.find(k);
      if (it == rows.end())
        throw std::runtime_error("part1 missing pinned row");
      return it->second.probabilities;
    };
    const std::vector<double>& q = at(kf, keys.i);
    const std::vector<double>& p = at(pv, keys.i);

    constexpr double eps = 1e-9;
    check(q.size() == 2 && p.size() == 2, "part1 pinned row is a binary fold/call row");
    check(std::abs(q[0] - 10.0 / 11.0) < eps && std::abs(q[1] - 1.0 / 11.0) < eps,
          "part1 kFull seals (10/11,1/11): own check-reach 1.0 vs 0.1 weighted");
    check(std::abs(p[0] - 0.5) < eps && std::abs(p[1] - 0.5) < eps,
          "part1 per-visit seals the unweighted (1/2,1/2) visit mix");
    check(std::abs(q[0] - p[0]) > 0.4,
          "part1 kFull and per-visit sealed rows differ by the full own-reach factor");

    // B's root U has own reach exactly 1, so kFull and per-visit seal identical
    // root distributions; the weighting diverges only DOWNSTREAM of C's mix.
    for (const AbstractInfosetKey& u : keys.u) {
      const std::vector<double>& a = at(kf, u);
      const std::vector<double>& b = at(pv, u);
      check(a.size() == b.size() && a.size() == 2, "part1 root rows are binary check/bet rows");
      for (std::size_t i = 0; i < a.size(); ++i)
        check(std::abs(a[i] - b[i]) < eps,
              "part1 the own-reach-1 root row is identical under both weightings");
    }

    std::printf("[mix-part1] seed=%llu I(air) kFull=(%.9f,%.9f) perVisit=(%.9f,%.9f)\n",
                (unsigned long long)seed, q[0], q[1], p[0], p[1]);
  }
  return local;
}

}  // namespace part1

// ------------------------------------------------------------- Part 2 gate
//
// Natural self-play (all three seats traversed; the dummy is all-in and never
// acts) on the same compact river game. This is the full-CFR-only confirmation
// of the theory: once the sealed profile is near equilibrium, the kFull
// (pi_own-weighted) and the unweighted per-visit full average agree after
// per-row normalization. They are compared with EXTERNAL-REACH mass weights
// (the row's xsums total divided by the global xsums sum) over rows with
// nonzero external mass only, so an off-path row sealed to the uniform default
// cannot contribute. The late-checkpoint sampler-vs-full agreement is pinned
// separately by test_multiway_full_average_parity and is not repeated here.
int part2(const GameDef& def, const TrainingConfig& config,
          const std::vector<std::vector<HoleCards>>& all_holes, const JointDealTable& deals) {
  int local = 0;
  constexpr std::size_t kIters = 400'000;
  const FullTable full = converge_full(def, config, all_holes, kIters);
  const FrozenArtifactRows eq_kfull = seal_full(full, false);
  const FrozenArtifactRows eq_pervisit = seal_per_visit(full);

  // (a) a genuinely mixed equilibrium row exists.
  check(has_interior_row(full, eq_kfull, 0.05, 0.95),
        "part2 the converged equilibrium contains a strict interior-mixed row");

  // (b) the kFull/per-visit bias vanishes near equilibrium, measured with
  // external-reach mass over reached rows only.
  double wnum = 0.0, wden = 0.0;
  for (const auto& [key, frow] : full) {
    double mass = 0.0;
    for (double v : frow.xsums)
      mass += v;
    if (mass <= 0.0)
      continue;
    auto a = eq_kfull.find(key);
    auto b = eq_pervisit.find(key);
    if (a == eq_kfull.end() || b == eq_pervisit.end() ||
        a->second.abstract_actions != b->second.abstract_actions)
      continue;
    for (std::size_t i = 0; i < a->second.probabilities.size(); ++i)
      wnum += mass * std::abs(a->second.probabilities[i] - b->second.probabilities[i]);
    wden += mass;
  }
  const double full_l1 = wden > 0.0 ? wnum / wden : 9.0;
  std::printf("[mix-part2] full kFull vs per-visit external-mass-L1 at %zu iters = %.6f\n", kIters,
              full_l1);
  check(full_l1 < 0.01, "part2 kFull and per-visit full references agree at convergence (<0.01)");

  const double y_full = nash_conv_on(def, kToken, eq_kfull, config, deals, all_holes);
  std::printf("[mix-part2] full-CFR reference NashConv = %.6f\n", y_full);
  check(y_full < 0.02, "part2 the converged full-CFR profile is near equilibrium (<0.02)");
  return local;
}

int test_mixed_equilibrium_discriminates_weighting() {
  TrainingConfig config;
  config.action = mix_action();
  config.card_kind = bs::abstraction::CardBucketKind::Identity;
  config.geometry_matrix_hash = 0x4d49584e54454402ULL;
  config.chart_digest_sha256 = "mix-equilibrium-item4-v2";

  const GameDef def = mix_def();
  const JointDealTable deals = mix_deals();
  check(deals.deals.size() == 6, "the mixed fixture has exactly six joint deals");
  std::vector<std::vector<HoleCards>> all_holes;
  for (const auto& d : deals.deals)
    all_holes.push_back(deal_holes(d));

  // Pinned card ordering under bs::evaluate (all off-board, mutually distinct):
  //   B-set > C-two-pair > B-top-pair > B-air > C-air > dummy.
  auto strength = [&](const HoleCards& h) {
    std::array<int, 7> c{h[0], h[1], kBoard[0], kBoard[1], kBoard[2], kBoard[3], kBoard[4]};
    return ::bs::evaluate(c.data(), 7).score;
  };
  check(strength(kB[0]) > strength(kC[0]) && strength(kC[0]) > strength(kB[1]) &&
            strength(kB[1]) > strength(kB[2]) && strength(kB[2]) > strength(kC[1]) &&
            strength(kC[1]) > strength(kDummy[0]),
        "pinned holdings satisfy Bset>C2pair>Btoppair>Bair>Cair>dummy");

  int local = 0;
  local += part1::run(def, config, all_holes);
  local += part2(def, config, all_holes, deals);
  return local;
}

}  // namespace mixgame

}  // namespace

int main() {
  failures += test_multiway_full_average_parity();
  failures += mixgame::test_mixed_equilibrium_discriminates_weighting();
  if (failures) {
    std::printf("STAGE 6 MULTIWAY AVERAGE PARITY FAILED: %d\n", failures);
    return 1;
  }
  std::puts("STAGE 6 MULTIWAY AVERAGE PARITY PASSED");
  return 0;
}
