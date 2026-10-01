// RFC 0005 Stage 6 resident policy lookup tests.
//
// The math oracle is written independently of the implementation: the test
// enumerates ranges and card-compatible joint deals itself, derives the
// expected per-combo public reach by hand for check/check, bet/call, fold,
// and zero-probability observations, and cross-checks the normalization
// convention against the solver's own reach hook
// (HeadsUpSolverDebug::response_value_with_reach).
#include <algorithm>
#include <array>
#include <bs/abstract_tree.hpp>
#include <bs/abstraction.hpp>
#include <bs/game_definition.hpp>
#include <bs/heads_up.hpp>
#include <bs/heads_up_solver.hpp>
#include <bs/nseat_trainer.hpp>
#include <bs/range.hpp>
#include <bs/resident_policy.hpp>
#include <bs/seat_policy.hpp>
#include <bs/strategy_artifact.hpp>
#include <bs/unified_game.hpp>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <optional>
#include <random>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "artifacts/artifact_internal.hpp"
#include "gto/heads_up_solver_debug.hpp"
#include "resident/resident_index.hpp"

using namespace bs::poker;
using namespace bs::solver;
using namespace bs::artifacts;
using namespace bs::resident;

#define CHECK(cond)                                                         \
  do {                                                                      \
    if (!(cond)) {                                                          \
      std::printf("CHECK failed: %s (%s:%d)\n", #cond, __FILE__, __LINE__); \
      return false;                                                         \
    }                                                                       \
  } while (0)

// CHECK for helpers whose signature cannot return a bool: aborts.
#define CHECK_ORACLE(cond)                                                  \
  do {                                                                      \
    if (!(cond)) {                                                          \
      std::printf("CHECK failed: %s (%s:%d)\n", #cond, __FILE__, __LINE__); \
      std::abort();                                                         \
    }                                                                       \
  } while (0)

namespace fs = std::filesystem;

namespace {

using bs::artifacts::detail::PolicyAssembler;

int card(const char* name) {
  return bs::cardId(std::string(name));
}

TrainingLimits fast_limits() {
  TrainingLimits limits;
  limits.max_nodes = 500000000;
  limits.max_information_sets = 5000000;
  limits.max_bytes = std::size_t{4} << 30;
  limits.time = std::chrono::minutes{10};
  return limits;
}

bool near(double a, double b, double tolerance = 1e-12) {
  return std::isfinite(a) && std::isfinite(b) && std::abs(a - b) <= tolerance;
}

HeadsUpGame canonicalize(HeadsUpGame game) {
  for (auto& range : game.ranges) {
    double maximum = 0;
    for (auto& hand : range) {
      std::sort(hand.cards.begin(), hand.cards.end());
      maximum = std::max(maximum, hand.weight);
    }
    for (auto& hand : range)
      hand.weight /= maximum;
    std::sort(range.begin(), range.end(),
              [](const auto& a, const auto& b) { return a.cards < b.cards; });
  }
  return game;
}

Sha256Digest parse_sha256(const std::string& hex) {
  Sha256Digest digest{};
  for (std::size_t i = 0; i < digest.size(); ++i) {
    const auto nibble = [](char c) {
      if (c >= '0' && c <= '9')
        return c - '0';
      if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
      return -1;
    };
    digest[i] = static_cast<std::uint8_t>((nibble(hex[2 * i]) << 4) | nibble(hex[2 * i + 1]));
  }
  return digest;
}

TrainingRows to_training_rows(const std::map<InformationKey, DebugRow>& debug_rows) {
  TrainingRows out;
  for (const auto& [key, row] : debug_rows)
    out.emplace(key, TrainingRow{row.actions, row.regrets, row.sums});
  return out;
}

using Overrides = std::map<InformationKey, std::vector<double>>;

// Static solver-tree support: does `combo` have any opponent range combo
// compatible with it and the current board? A complete artifact only has rows
// for actor combos participating in at least one joint deal on the branch;
// free-chance orphan combos correctly have no row.
bool has_static_partner(const HeadsUpGame& game, std::size_t actor, const std::array<int, 2>& combo,
                        const std::vector<int>& board) {
  const std::size_t opponent = 1 - actor;
  for (const WeightedHand& other : game.ranges[opponent]) {
    bool blocked = false;
    for (int board_card : board)
      if (board_card == other.cards[0] || board_card == other.cards[1] || board_card == combo[0] ||
          board_card == combo[1])
        blocked = true;
    if (blocked)
      continue;
    if (other.cards[0] == combo[0] || other.cards[0] == combo[1] || other.cards[1] == combo[0] ||
        other.cards[1] == combo[1])
      continue;
    return true;
  }
  return false;
}

struct PublishedFixture {
  fs::path policy_path;
  HeadsUpGame game;
  std::string sha256_hex;
};

// Train one complete full traversal, optionally override exact published
// rows, checkpoint, and publish an immutable policy artifact.
PublishedFixture publish_game(HeadsUpGame game, std::uint64_t iterations,
                              const Overrides& overrides, const fs::path& dir,
                              const std::string& name) {
  const DebugTrainingOutput trained =
      HeadsUpSolverDebug::train_full(game, iterations, fast_limits());
  if (trained.result.status != TrainingStatus::Complete) {
    std::printf("training did not complete for %s\n", name.c_str());
    std::abort();
  }

  TrainingRows raw_rows = to_training_rows(trained.rows);
  HeadsUpPolicy policy;
  PolicyAssembler::set_game(policy, game);
  for (const auto& [key, trained_row] : trained.result.policy.rows()) {
    PolicyRow row = PolicyRow{trained_row.actions, trained_row.probabilities};
    const auto override_it = overrides.find(key);
    if (override_it != overrides.end()) {
      if (override_it->second.size() != row.actions.size()) {
        std::printf("override action count mismatch for %s\n", name.c_str());
        std::abort();
      }
      row.probabilities = override_it->second;
    }
    PolicyAssembler::add_row(policy, key, row);
  }
  for (const auto& [key, probs] : overrides) {
    const auto row_it = trained.result.policy.rows().find(key);
    if (row_it == trained.result.policy.rows().end()) {
      std::printf("override key absent from trained policy for %s\n", name.c_str());
      std::abort();
    }
    raw_rows.insert_or_assign(key, TrainingRow{row_it->second.actions, probs, probs});
  }

  TrainingResult result = trained.result;
  result.policy = std::move(policy);

  const fs::path checkpoint = dir / (name + "-checkpoint.db");
  const fs::path policy_path = dir / (name + "-policy.db");
  CheckpointProvenance provenance;
  provenance.prng_identifier = kFullTraversalPrngIdentifier;
  provenance.engine_revision = "resident-test-engine-1";
  create_checkpoint(checkpoint, result, raw_rows, provenance);
  const PublishedPolicy published = publish_policy(checkpoint, policy_path, "resident-test-1");

  PublishedFixture fixture;
  fixture.policy_path = policy_path;
  fixture.game = std::move(game);
  fixture.sha256_hex = published.sha256_hex;
  return fixture;
}

// Two chips behind, two combos per side, fixed 9h turn and 8s river. All four
// joint deals are card-compatible.
HeadsUpGame golden_game() {
  HeadsUpGame game;
  game.root = {{card("2c"), card("3d"), card("7h")}, {2, 2}, {1, 1}, 2, 1, 1};
  game.ranges[0] = {{{card("As"), card("Ks")}, 2}, {{card("Qh"), card("Jh")}, 3}};
  game.ranges[1] = {{{card("Ac"), card("Kc")}, 5}, {{card("Qd"), card("Jd")}, 7}};
  game.fixed_runout = {card("9h"), card("8s")};
  return game;
}

// Same ranges, but the turn is free (river fixed 8s) for chance filtering.
HeadsUpGame free_turn_game() {
  HeadsUpGame game = golden_game();
  game.fixed_runout = {std::nullopt, card("8s")};
  return game;
}

// Three combos per side with one cross-blocked pair (AcAd vs AcAs share Ac),
// deeper stacks, fixed 3s/5h runout, SPR 1.
HeadsUpGame matrix_game() {
  HeadsUpGame game;
  game.root = {{card("Ks"), card("7h"), card("2c")}, {10, 10}, {5, 5}, 10, 5, 1};
  game.ranges[0] = {
      {{card("Ac"), card("Ad")}, 2}, {{card("8c"), card("8d")}, 3}, {{card("Kc"), card("Kd")}, 4}};
  game.ranges[1] = {
      {{card("Ac"), card("As")}, 5}, {{card("Tc"), card("Td")}, 7}, {{card("Ah"), card("Kh")}, 6}};
  game.fixed_runout = {card("3s"), card("5h")};
  return game;
}

// Publish an intentionally incomplete policy that retains only the root
// public node rows. Artifact validation permits non-empty partial exports;
// the resident layer must report later nodes as missing history.
PublishedFixture publish_root_only(HeadsUpGame game, const fs::path& dir, const std::string& name) {
  const DebugTrainingOutput trained = HeadsUpSolverDebug::train_full(game, 1, fast_limits());
  HeadsUpPolicy policy;
  PolicyAssembler::set_game(policy, game);
  TrainingRows raw_rows;
  for (const auto& [key, trained_row] : trained.result.policy.rows()) {
    // Root keys are actor, two own cards, board size 3, the flop: 7 words.
    if (key.size() != 7)
      continue;
    PolicyAssembler::add_row(policy, key,
                             PolicyRow{trained_row.actions, trained_row.probabilities});
    const auto raw_it = trained.rows.find(key);
    raw_rows.emplace(
        key, TrainingRow{raw_it->second.actions, raw_it->second.regrets, raw_it->second.sums});
  }
  TrainingResult result = trained.result;
  result.policy = std::move(policy);
  result.information_sets = raw_rows.size();

  const fs::path checkpoint = dir / (name + "-checkpoint.db");
  const fs::path policy_path = dir / (name + "-policy.db");
  CheckpointProvenance provenance;
  provenance.prng_identifier = kFullTraversalPrngIdentifier;
  provenance.engine_revision = "resident-partial-engine-1";
  create_checkpoint(checkpoint, result, raw_rows, provenance);
  const PublishedPolicy published = publish_policy(checkpoint, policy_path, "resident-partial-1");
  return {policy_path, std::move(game), published.sha256_hex};
}

InformationKey key_of(const HeadsUpState& state, std::array<int, 2> own) {
  return information_key(state, own);
}

// Convert a HeadsUpState node to the unified (GameState, PublicAction history)
// view. GameState at two seats reproduces HeadsUpState exactly, so the replay
// cursor and the source agree field for field; the trailing board deal is a
// no-op when the two already agree (it only matters if the source advanced
// past an all-in runout, which the decision-node fixtures never do).
struct UnifiedNode {
  GameState state;
  std::vector<PublicAction> history;
};

UnifiedNode to_unified(const HeadsUpState& source) {
  GameDef def{};
  def.player_count = 2;
  def.button = source.root().button;
  def.big_blind = source.root().big_blind;
  def.stacks[0] = source.root().stacks[0];
  def.stacks[1] = source.root().stacks[1];
  def.contributions[0] = source.root().contributions[0];
  def.contributions[1] = source.root().contributions[1];
  def.pot = source.root().pot;
  def.board[0] = source.root().flop[0];
  def.board[1] = source.root().flop[1];
  def.board[2] = source.root().flop[2];
  def.board_size = 3;
  GameState cursor(def);
  std::vector<PublicAction> history;
  for (const BettingEvent& event : source.history()) {
    while (cursor.street() < event.street)
      cursor = cursor.after_card(source.board()[cursor.board().size()]);
    cursor = cursor.after_action(event.actor, event.action);
    history.push_back({event.street, event.actor, event.action});
  }
  while (cursor.board().size() < source.board().size())
    cursor = cursor.after_card(source.board()[cursor.board().size()]);
  return {std::move(cursor), std::move(history)};
}

// Publish a hand-authored single-row policy for games the trainer refuses to
// build (zero or fully cross-blocked joint ranges). Used to prove the
// resident startup gate rejects validated artifacts that cannot condition.
PublishedFixture publish_synthetic_policy(const HeadsUpGame& game_in, const fs::path& dir,
                                          const std::string& name) {
  HeadsUpGame game = game_in;
  for (auto& range : game.ranges)
    for (auto& hand : range)
      std::sort(hand.cards.begin(), hand.cards.end());

  const HeadsUpState root(game.root);
  const std::vector<Action> actions = abstract_actions(root, game.sizes);
  if (actions.empty()) {
    std::printf("synthetic fixture has no root actions\n");
    std::abort();
  }
  std::vector<double> probs(actions.size(), 1.0 / static_cast<double>(actions.size()));
  const auto own = game.ranges[0].front().cards;
  const InformationKey key = information_key(root, own);

  HeadsUpPolicy policy;
  PolicyAssembler::set_game(policy, game);
  PolicyAssembler::add_row(policy, key, PolicyRow{actions, probs});
  TrainingResult result;
  result.status = TrainingStatus::Complete;
  result.completed_iterations = 1;
  result.information_sets = 1;
  result.policy = std::move(policy);
  TrainingRows rows;
  rows.emplace(key, TrainingRow{actions, std::vector<double>(actions.size(), 0.0), probs});

  const fs::path checkpoint = dir / (name + "-checkpoint.db");
  const fs::path policy_path = dir / (name + "-policy.db");
  CheckpointProvenance provenance;
  provenance.prng_identifier = kFullTraversalPrngIdentifier;
  provenance.engine_revision = "resident-synthetic-engine-1";
  create_checkpoint(checkpoint, result, rows, provenance);
  const PublishedPolicy published = publish_policy(checkpoint, policy_path, "resident-synthetic-1");
  return {policy_path, std::move(game), published.sha256_hex};
}

double reach_of(const ResidentAnswer& answer, std::size_t player, int c0, int c1) {
  return (*answer.public_reach[player])[bs::comboIndex(c0, c1)];
}

bool check_golden_marginals(const ResidentAnswer& answer, double a0, double b0, double a1,
                            double b1) {
  CHECK(answer.hit);
  CHECK(near(reach_of(answer, 0, card("As"), card("Ks")), a0));
  CHECK(near(reach_of(answer, 0, card("Qh"), card("Jh")), b0));
  CHECK(near(reach_of(answer, 1, card("Ac"), card("Kc")), a1));
  CHECK(near(reach_of(answer, 1, card("Qd"), card("Jd")), b1));
  // Marginals sum to one and every other declared-absent combo is zero.
  for (std::size_t player = 0; player < 2; ++player) {
    double total = 0;
    for (double mass : *answer.public_reach[player])
      total += mass;
    CHECK(near(total, 1.0));
  }
  return true;
}

// Independent joint-deal enumeration for the golden two-vs-two ranges.
struct OracleDeal {
  std::array<std::array<int, 2>, 2> hands;
  double probability;
};

std::vector<OracleDeal> oracle_deals(const HeadsUpGame& game) {
  std::vector<OracleDeal> deals;
  double mass = 0;
  for (const auto& h0 : game.ranges[0])
    for (const auto& h1 : game.ranges[1]) {
      if (h0.cards[0] == h1.cards[0] || h0.cards[0] == h1.cards[1] || h0.cards[1] == h1.cards[0] ||
          h0.cards[1] == h1.cards[1])
        continue;
      deals.push_back(OracleDeal{{h0.cards, h1.cards}, h0.weight * h1.weight});
      mass += h0.weight * h1.weight;
    }
  for (auto& deal : deals)
    deal.probability /= mass;
  return deals;
}

// --- RFC 0009 W2c schema-v2 resident projection fixtures -------------------

// A two-seat flop-rooted v2 game: the only v2 shape the resident data layer
// projects onto its heads-up view (RFC 0009 D4). Postflop the non-button
// seat acts first (HeadsUpState actor = 1 - button), matching the GameState
// opener, so the root decision rows belong to seat 1.
GameDef two_seat_flop_def_v2() {
  GameDef def{};
  def.player_count = 2;
  def.button = 0;
  def.big_blind = 2;
  def.stacks = {2, 2, 0, 0, 0, 0, 0, 0, 0, 0};
  def.contributions = {1, 1, 0, 0, 0, 0, 0, 0, 0, 0};
  def.pot = 2;
  def.board = {card("2c"), card("3d"), card("7h"), 0, 0};
  def.board_size = 3;
  return def;
}

// Two combos per seat, all off the flop board and mutually card-distinct.
// Each combo's cards are sorted ascending (a < b) as the nseat trainer
// requires.
std::vector<std::vector<WeightedHand>> two_seat_flop_ranges_v2() {
  return {
      {{{card("Ks"), card("As")}, 2}, {{card("Jh"), card("Qh")}, 3}},
      {{{card("Kc"), card("Ac")}, 5}, {{card("Jd"), card("Qd")}, 7}},
  };
}

// A three-seat flop-rooted v2 game: the resident projection refuses it
// (W2c-ii owns the state-layer generalization), so it must never advertise.
GameDef three_seat_flop_def_v2() {
  GameDef def{};
  def.player_count = 3;
  def.button = 0;
  def.big_blind = 2;
  def.stacks = {2, 2, 2, 0, 0, 0, 0, 0, 0, 0};
  def.contributions = {1, 1, 1, 0, 0, 0, 0, 0, 0, 0};
  def.pot = 3;
  def.board = {card("2c"), card("3d"), card("7h"), 0, 0};
  def.board_size = 3;
  return def;
}

std::vector<std::vector<WeightedHand>> three_seat_flop_ranges_v2() {
  return {
      {{{card("8h"), card("8c")}, 3}, {{card("Ah"), card("Ac")}, 2}},
      {{{card("8s"), card("8d")}, 7}, {{card("As"), card("Ad")}, 5}},
      {{{card("Jh"), card("Jc")}, 13}, {{card("Qh"), card("Qc")}, 11}},
  };
}

// W2c-ii-b collision fixture: two combos per seat, all weight one, with
// deliberate card collisions across seats so the exact inclusion-exclusion
// path is exercised (the mutually-distinct fixture above cannot). Board is
// {2c,3d,7h}; no combo holds a board card. The four card-disjoint joint deals
// are (seat0, seat1, seat2) = (KhAh, QdAd, QcKc), (KsAs, QhAh, QcKc),
// (KsAs, QdAd, QhKh), (KsAs, QdAd, QcKc): joint mass 4, and every seat's
// first combo appears in exactly one deal (marginal 1/4) while its second
// appears in three (marginal 3/4).
std::vector<std::vector<WeightedHand>> three_seat_collision_ranges_v2() {
  return {
      {{{card("Kh"), card("Ah")}, 1}, {{card("Ks"), card("As")}, 1}},
      {{{card("Qh"), card("Ah")}, 1}, {{card("Qd"), card("Ad")}, 1}},
      {{{card("Qh"), card("Kh")}, 1}, {{card("Qc"), card("Kc")}, 1}},
  };
}

// W2c-ii-b bridging fixture: seat 2's first combo AhQd bridges two blocked
// cards (Ah from seat 0's AhKh and Qd from seat 1's QdJd) for the disjoint
// pair (AhKh, QdJd), so compatible_mass's pairwise add-back is non-zero.
// Without that add-back AhQd is subtracted twice for that pair and the deal
// (AhKh, QdJd, 4s5s) vanishes, dropping the joint from 5 to 4. The five
// card-disjoint deals are (AhKh,QdJd,4s5s), (AhKh,QcJc,4s5s),
// (AsKs,QdJd,4s5s), (AsKs,QcJc,AhQd), (AsKs,QcJc,4s5s): joint mass 5, with
// marginals seat0 {2/5, 3/5}, seat1 {2/5, 3/5}, seat2 {1/5, 4/5}. No combo
// holds a board card from three_seat_flop_def_v2 ({2c,3d,7h}).
std::vector<std::vector<WeightedHand>> three_seat_bridging_ranges_v2() {
  return {
      {{{card("Kh"), card("Ah")}, 1}, {{card("Ks"), card("As")}, 1}},
      {{{card("Jd"), card("Qd")}, 1}, {{card("Jc"), card("Qc")}, 1}},
      {{{card("Qd"), card("Ah")}, 1}, {{card("4s"), card("5s")}, 1}},
  };
}

// A four-seat flop-rooted v2 game: it loads and advertises, but W2c-ii-b
// conditions belief exactly only for two and three seats, so a 4-seat belief
// or hero-decision query misses declared (SeatCountNotSupported).
GameDef four_seat_flop_def_v2() {
  GameDef def{};
  def.player_count = 4;
  def.button = 0;
  def.big_blind = 2;
  def.stacks = {2, 2, 2, 2, 0, 0, 0, 0, 0, 0};
  def.contributions = {1, 1, 1, 1, 0, 0, 0, 0, 0, 0};
  def.pot = 4;
  def.board = {card("2c"), card("3d"), card("7h"), 0, 0};
  def.board_size = 3;
  return def;
}

std::vector<std::vector<WeightedHand>> four_seat_flop_ranges_v2() {
  return {
      {{{card("8h"), card("8c")}, 1}, {{card("Ah"), card("Ac")}, 1}},
      {{{card("8s"), card("8d")}, 1}, {{card("As"), card("Ad")}, 1}},
      {{{card("Jh"), card("Jc")}, 1}, {{card("Qh"), card("Qc")}, 1}},
      {{{card("Js"), card("Jd")}, 1}, {{card("Qs"), card("Qd")}, 1}},
  };
}

// --- RFC 0009 W4c-ii schema-v3 class-policy resident fixtures --------------

// A two-seat flop-rooted v3 game on a suit-canonical board. {2s, 7h, Kd} has
// suits {0,1,2} in rank order, so the suit-canonicalization relabel is the
// identity and the board is its own class representative -- the precondition
// the v3 writer's board-canonical integrity check enforces.
GameDef two_seat_flop_canonical_def_v3() {
  GameDef def{};
  def.player_count = 2;
  def.button = 0;
  def.big_blind = 2;
  def.stacks = {2, 2, 0, 0, 0, 0, 0, 0, 0, 0};
  def.contributions = {1, 1, 0, 0, 0, 0, 0, 0, 0, 0};
  def.pot = 2;
  def.board = {card("2s"), card("7h"), card("Kd"), 0, 0};
  def.board_size = 3;
  return def;
}

// Same ranks {2,7,K} with suits permuted out of canonical order: {2h, 7s, Kd}
// has suits {1,0,2}, so the canonical relabel swaps spades and hearts and the
// board is NOT its own class representative. A v3 artifact trained on the
// canonical board above must still serve a query rooted at this board.
GameDef two_seat_flop_noncanonical_def_v3() {
  GameDef def = two_seat_flop_canonical_def_v3();
  def.board = {card("2h"), card("7s"), card("Kd"), 0, 0};
  return def;
}

// Two combos per seat, all off the {2,7,K} flop and mutually card-distinct,
// plus a third seat-0 combo holding 9s so the turn-card relabel is
// discriminating: dealing 9h (which relabels to 9s) must block it, while a
// no-op relabel would leave 9h != 9s and miss the block. The ranges are
// expressed in CANONICAL coordinates (the v3 artifact stores the class
// policy, not a concrete-board policy).
std::vector<std::vector<WeightedHand>> two_seat_flop_ranges_v3() {
  return {
      {{{card("As"), card("Ad")}, 2}, {{card("Ah"), card("Ac")}, 3}, {{card("9s"), card("9c")}, 4}},
      {{{card("Qs"), card("Qd")}, 5}, {{card("Qh"), card("Qc")}, 7}},
  };
}

// A two-seat turn-rooted v2 game: the resident projection is flop-rooted
// only, so a turn/river-rooted source is refused as LoadFailed until the
// state layer generalizes (W2c-ii). The flop-rooted ranges stay off this
// board (the fourth card is 9s, absent from every combo).
GameDef two_seat_turn_def_v2() {
  GameDef def{};
  def.player_count = 2;
  def.button = 0;
  def.big_blind = 2;
  def.stacks = {2, 2, 0, 0, 0, 0, 0, 0, 0, 0};
  def.contributions = {1, 1, 0, 0, 0, 0, 0, 0, 0, 0};
  def.pot = 2;
  def.board = {card("2c"), card("3d"), card("7h"), card("9s"), 0};
  def.board_size = 4;
  return def;
}

struct PublishedV2Fixture {
  fs::path policy_path;
  GameDef def;
  std::string sha256_hex;
};

// Train, checkpoint, and publish a seat-generic v2 policy. W4c-ii: an optional
// card_id produces a schema-v3 class policy instead.
PublishedV2Fixture publish_v2(
    const GameDef& def, const std::vector<std::vector<WeightedHand>>& ranges,
    std::uint64_t iterations, const fs::path& dir, const std::string& name,
    std::optional<bs::abstraction::AbstractionId> card_id = std::nullopt) {
  const bs::tree::AbstractTree tree(def, bs::abstraction::ActionAbstraction::identity());
  const NSeatTrainingResult trained =
      train_nseat(tree, ranges, iterations, 20260930, NSeatTrainerLimits{});
  if (trained.termination != NSeatTerminationPhase::Complete) {
    std::printf("v2 fixture training did not complete for %s\n", name.c_str());
    std::abort();
  }
  const SeatTrainingResult exported = export_seat_policy(trained, tree, ranges, card_id);
  const fs::path checkpoint = dir / (name + "-v2-checkpoint.db");
  const fs::path policy_path = dir / (name + "-v2-policy.db");
  SeatCheckpointProvenance provenance;
  provenance.engine_revision = "resident-v2-test-1";
  create_checkpoint(checkpoint, exported, provenance);
  const PublishedPolicy published = publish_policy(checkpoint, policy_path, "resident-v2-1");
  return {policy_path, def, published.sha256_hex};
}

}  // namespace

// ---------------------------------------------------------------------------
// Golden public reach on a hand-built two-vs-two fixed-runout game.
// ---------------------------------------------------------------------------

static bool test_golden_reach(const fs::path& dir) {
  HeadsUpGame game = canonicalize(golden_game());
  const HeadsUpState root(game.root);
  const HeadsUpState after_check = root.after_action(0, {ActionType::Check});
  const HeadsUpState after_check_check = after_check.after_action(1, {ActionType::Check});
  const HeadsUpState after_bet = root.after_action(0, {ActionType::Bet, 1});
  const HeadsUpState after_bet_call = after_bet.after_action(1, {ActionType::Call});
  const HeadsUpState after_bet_fold = after_bet.after_action(1, {ActionType::Fold});
  const HeadsUpState after_check_bet = after_check.after_action(1, {ActionType::Bet, 1});
  const HeadsUpState after_check_jam = after_check.after_action(1, {ActionType::Bet, 2});
  const HeadsUpState after_jam = root.after_action(0, {ActionType::Bet, 2});
  const HeadsUpState after_jam_call = after_jam.after_action(1, {ActionType::Call});

  const UnifiedNode root_node = to_unified(root);
  const UnifiedNode after_check_node = to_unified(after_check);
  const UnifiedNode after_check_check_node = to_unified(after_check_check);
  const UnifiedNode after_bet_node = to_unified(after_bet);
  const UnifiedNode after_bet_call_node = to_unified(after_bet_call);
  const UnifiedNode after_bet_fold_node = to_unified(after_bet_fold);
  const UnifiedNode after_check_bet_node = to_unified(after_check_bet);
  const UnifiedNode after_check_jam_node = to_unified(after_check_jam);
  const UnifiedNode after_jam_node = to_unified(after_jam);
  const UnifiedNode after_jam_call_node = to_unified(after_jam_call);

  Overrides overrides;
  std::array<int, 2> as_ks{card("As"), card("Ks")};
  std::array<int, 2> qh_jh{card("Qh"), card("Jh")};
  std::array<int, 2> ac_kc{card("Ac"), card("Kc")};
  std::array<int, 2> qd_jd{card("Qd"), card("Jd")};
  for (auto* hand : {&as_ks, &qh_jh, &ac_kc, &qd_jd})
    std::sort(hand->begin(), hand->end());
  const std::vector<double> thirds{0.5, 0.25, 0.25};
  // Seat 1 after check: jam is impossible for the second combo.
  overrides.emplace(key_of(after_check, ac_kc), std::vector<double>{0.5, 0.25, 0.25});
  overrides.emplace(key_of(after_check, qd_jd), std::vector<double>{0.5, 0.5, 0.0});
  // Facing a bet to 1: fold/call/raise to 2.
  overrides.emplace(key_of(after_bet, ac_kc), std::vector<double>{0.2, 0.6, 0.2});
  overrides.emplace(key_of(after_bet, qd_jd), std::vector<double>{0.25, 0.75, 0.0});
  // Facing the jam: call is zero probability for BOTH combos.
  overrides.emplace(key_of(after_jam, ac_kc), std::vector<double>{1.0, 0.0});
  overrides.emplace(key_of(after_jam, qd_jd), std::vector<double>{1.0, 0.0});
  // Uniform root rows for seat 0 (both combos check 50%, bet 1 25%, jam 25%).
  overrides.emplace(key_of(root, as_ks), thirds);
  overrides.emplace(key_of(root, qh_jh), thirds);

  const PublishedFixture fixture = publish_game(game, 1, overrides, dir, "golden");
  // v1 never writes certification bounds or measurements: they exist,
  // validate, and read as empty. Resident continuations are blueprint-only.
  {
    sqlite3* raw = nullptr;
    CHECK(sqlite3_open(fixture.policy_path.string().c_str(), &raw) == SQLITE_OK);
    for (const char* table : {"bounds", "measurements"}) {
      sqlite3_stmt* stmt = nullptr;
      const std::string sql = std::string("SELECT COUNT(*) FROM ") + table;
      CHECK(sqlite3_prepare_v2(raw, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK);
      CHECK(sqlite3_step(stmt) == SQLITE_ROW);
      CHECK(sqlite3_column_int64(stmt, 0) == 0);
      sqlite3_finalize(stmt);
    }
    sqlite3_close(raw);
  }
  std::vector<RootLoadResult> results;
  SupportedRootSpec spec{fixture.policy_path, parse_sha256(fixture.sha256_hex)};
  ResidentPolicySet residents = ResidentPolicySet::build({spec}, {}, &results);
  CHECK(results.size() == 1);
  CHECK(results[0].status == RootStatus::Advertised);
  CHECK(results[0].information_sets > 0);
  CHECK(results[0].resident_bytes > 0);
  CHECK(residents.advertised_roots() == 1);

  ResidentScratch scratch;

  // Root marginals from weights (2,3) x (5,7): 0.4/0.6 and 5/12,7/12.
  CHECK(check_golden_marginals(
      residents.public_belief(root_node.state, root_node.history, std::nullopt, scratch), 0.4, 0.6,
      5.0 / 12.0, 7.0 / 12.0));
  // A constant check probability for every actor combo leaves the belief.
  CHECK(check_golden_marginals(
      residents.public_belief(after_check_node.state, after_check_node.history, std::nullopt,
                              scratch),
      0.4, 0.6, 5.0 / 12.0, 7.0 / 12.0));
  CHECK(check_golden_marginals(
      residents.public_belief(after_check_check_node.state, after_check_check_node.history,
                              std::nullopt, scratch),
      0.4, 0.6, 5.0 / 12.0, 7.0 / 12.0));
  // Constant 0.25 bet probability leaves the belief.
  CHECK(check_golden_marginals(
      residents.public_belief(after_bet_node.state, after_bet_node.history, std::nullopt, scratch),
      0.4, 0.6, 5.0 / 12.0, 7.0 / 12.0));
  // Call: 5/12*0.6 vs 7/12*0.75 -> 4/11 and 7/11.
  CHECK(check_golden_marginals(
      residents.public_belief(after_bet_call_node.state, after_bet_call_node.history, std::nullopt,
                              scratch),
      0.4, 0.6, 4.0 / 11.0, 7.0 / 11.0));
  // Fold: 5/12*0.2 vs 7/12*0.25 -> 4/11 and 7/11.
  CHECK(check_golden_marginals(
      residents.public_belief(after_bet_fold_node.state, after_bet_fold_node.history, std::nullopt,
                              scratch),
      0.4, 0.6, 4.0 / 11.0, 7.0 / 11.0));
  // Seat 1 bet after the check: (5/12*0.25, 7/12*0.5) -> 5/19, 14/19.
  CHECK(check_golden_marginals(
      residents.public_belief(after_check_bet_node.state, after_check_bet_node.history,
                              std::nullopt, scratch),
      0.4, 0.6, 5.0 / 19.0, 14.0 / 19.0));
  // Seat 1 jam: zero for the second combo, positive for the first: the combo
  // is removed, mass stays positive, so this is a covered belief update.
  CHECK(check_golden_marginals(
      residents.public_belief(after_check_jam_node.state, after_check_jam_node.history,
                              std::nullopt, scratch),
      0.4, 0.6, 1.0, 0.0));
  // Seat 1 calling the jam is zero probability for every live combo.
  const ResidentAnswer zero_call = residents.public_belief(
      after_jam_call_node.state, after_jam_call_node.history, std::nullopt, scratch);
  CHECK(!zero_call.hit);
  CHECK(zero_call.reason == MissReason::ZeroProbabilityObservedAction);

  // A folded hand endpoint still has a well-defined conditioned public
  // belief; hero decisions at terminals are rejected separately in the miss
  // suite.

  // Public belief is identical across every hero combination. At the root
  // seat 0 acts; at after_bet seat 1 acts, so each query passes the acting
  // player's own combos.
  const HeadsUpState probe = after_bet;
  const UnifiedNode probe_node = to_unified(probe);
  const ResidentAnswer reference =
      residents.public_belief(probe_node.state, probe_node.history, std::nullopt, scratch);
  ResidentScratch hero_scratch;
  for (const auto& hero_hand : game.ranges[1]) {
    const ResidentAnswer hero = residents.hero_decision(
        probe_node.state, probe_node.history, hero_hand.cards, std::nullopt, hero_scratch);
    CHECK(hero.hit);
    for (std::size_t player = 0; player < 2; ++player)
      for (int combo = 0; combo < bs::N_COMBOS; ++combo)
        CHECK(near((*hero.public_reach[player])[combo], (*reference.public_reach[player])[combo]));
  }

  // Every advertised hero row still sums to one, at both actors' nodes.
  for (const auto& hero_hand : game.ranges[0]) {
    const ResidentAnswer hero = residents.hero_decision(
        root_node.state, root_node.history, hero_hand.cards, std::nullopt, hero_scratch);
    double sum = 0;
    for (std::size_t i = 0; i < hero.hero_row.size; ++i)
      sum += hero.hero_row.probabilities[i];
    CHECK(near(sum, 1.0));
  }
  for (const auto& hero_hand : game.ranges[1]) {
    const ResidentAnswer hero = residents.hero_decision(
        probe_node.state, probe_node.history, hero_hand.cards, std::nullopt, hero_scratch);
    double sum = 0;
    for (std::size_t i = 0; i < hero.hero_row.size; ++i)
      sum += hero.hero_row.probabilities[i];
    CHECK(near(sum, 1.0));
  }

  // Hero-private blocker filter: the opponent view removes combos sharing
  // hero cards and is never renormalized. At the root no actions have
  // happened, so every opponent combo carries its root marginal.
  const ResidentAnswer as_hero = residents.hero_decision(root_node.state, root_node.history, as_ks,
                                                         std::nullopt, hero_scratch);
  CHECK(as_hero.hit);
  double opponent_total = 0;
  for (int combo = 0; combo < bs::N_COMBOS; ++combo) {
    opponent_total += as_hero.opponent_blocked_reach[combo];
    const auto cards = bs::comboCards(combo);
    if (cards[0] == as_ks[0] || cards[1] == as_ks[0] || cards[0] == as_ks[1] ||
        cards[1] == as_ks[1])
      CHECK(as_hero.opponent_blocked_reach[combo] == 0.0);
  }
  // Neither opponent combo shares As or Ks, so the full marginal survives.
  CHECK(near(opponent_total, 1.0));
  return true;
}

// ---------------------------------------------------------------------------
// Public-card filtering on a free-turn fixture.
// ---------------------------------------------------------------------------

static bool test_chance_filter(const fs::path& dir) {
  HeadsUpGame game = canonicalize(free_turn_game());
  const HeadsUpState root(game.root);
  const HeadsUpState checked =
      root.after_action(0, {ActionType::Check}).after_action(1, {ActionType::Check});
  const HeadsUpState turn_as = checked.after_card(card("As"));

  const UnifiedNode checked_node = to_unified(checked);
  const UnifiedNode turn_as_node = to_unified(turn_as);

  const PublishedFixture fixture = publish_game(game, 1, {}, dir, "free-turn");
  std::vector<RootLoadResult> results;
  ResidentPolicySet residents = ResidentPolicySet::build(
      {{fixture.policy_path, parse_sha256(fixture.sha256_hex)}}, {}, &results);
  CHECK(results[0].status == RootStatus::Advertised);

  ResidentScratch scratch;
  // The dealing boundary after check/check already carries the conditioned
  // (here unchanged) public belief.
  const ResidentAnswer before =
      residents.public_belief(checked_node.state, checked_node.history, std::nullopt, scratch);
  CHECK(before.hit);
  CHECK(near(reach_of(before, 0, card("As"), card("Ks")), 0.4));
  CHECK(near(reach_of(before, 1, card("Qd"), card("Jd")), 7.0 / 12.0));

  // After the As turn, player 0's AsKs combination is removed; player 1's
  // marginals are unchanged.
  const ResidentAnswer after =
      residents.public_belief(turn_as_node.state, turn_as_node.history, std::nullopt, scratch);
  CHECK(after.hit);
  CHECK(near(reach_of(after, 0, card("As"), card("Ks")), 0.0));
  CHECK(near(reach_of(after, 0, card("Qh"), card("Jh")), 1.0));
  CHECK(near(reach_of(after, 1, card("Ac"), card("Kc")), 5.0 / 12.0));
  CHECK(near(reach_of(after, 1, card("Qd"), card("Jd")), 7.0 / 12.0));

  // The blocked hero combination is rejected at the same node.
  const ResidentAnswer blocked = residents.hero_decision(
      turn_as_node.state, turn_as_node.history, {card("As"), card("Ks")}, std::nullopt, scratch);
  CHECK(!blocked.hit);
  CHECK(blocked.reason == MissReason::ComboBlockedByBoard);
  return true;
}

// ---------------------------------------------------------------------------
// Reach marginalization cross-check against the solver's own reach hook.
// ---------------------------------------------------------------------------

static bool test_solver_reach_convention(const fs::path& dir) {
  HeadsUpGame game = canonicalize(golden_game());
  const HeadsUpState root(game.root);
  const HeadsUpState after_check = root.after_action(0, {ActionType::Check});
  const HeadsUpState s_bet = after_check.after_action(1, {ActionType::Bet, 1});

  // Non-constant OWN (player-0) action probabilities: the hook cross-check
  // must multiply per-deal reach by the responder-side observed action too,
  // otherwise deleting the actor-0 update stays green.
  Overrides overrides;
  std::array<int, 2> as_ks{card("As"), card("Ks")};
  std::array<int, 2> qh_jh{card("Qh"), card("Jh")};
  std::array<int, 2> ac_kc{card("Ac"), card("Kc")};
  std::array<int, 2> qd_jd{card("Qd"), card("Jd")};
  for (auto* hand : {&as_ks, &qh_jh, &ac_kc, &qd_jd})
    std::sort(hand->begin(), hand->end());
  overrides.emplace(key_of(root, as_ks), std::vector<double>{0.5, 0.25, 0.25});
  overrides.emplace(key_of(root, qh_jh), std::vector<double>{0.2, 0.3, 0.5});
  overrides.emplace(key_of(after_check, ac_kc), std::vector<double>{0.5, 0.25, 0.25});
  overrides.emplace(key_of(after_check, qd_jd), std::vector<double>{0.5, 0.5, 0.0});
  const HeadsUpState after_bet = root.after_action(0, {ActionType::Bet, 1});
  overrides.emplace(key_of(after_bet, ac_kc), std::vector<double>{0.2, 0.6, 0.2});
  overrides.emplace(key_of(after_bet, qd_jd), std::vector<double>{0.25, 0.75, 0.0});

  // Turn and river observed-action rows.
  const HeadsUpState checked = after_check.after_action(1, {ActionType::Check});
  const HeadsUpState turn = checked.after_card(card("9h"));
  overrides.emplace(key_of(turn, as_ks), std::vector<double>{0.5, 0.25, 0.25});
  overrides.emplace(key_of(turn, qh_jh), std::vector<double>{0.1, 0.4, 0.5});
  const HeadsUpState after_turn_bet = turn.after_action(0, {ActionType::Bet, 1});
  overrides.emplace(key_of(after_turn_bet, ac_kc), std::vector<double>{0.2, 0.6, 0.2});
  overrides.emplace(key_of(after_turn_bet, qd_jd), std::vector<double>{0.25, 0.75, 0.0});
  const HeadsUpState after_turn_call = after_turn_bet.after_action(1, {ActionType::Call});
  const HeadsUpState river = after_turn_call.after_card(card("8s"));
  overrides.emplace(key_of(river, as_ks), std::vector<double>{0.5, 0.5});
  overrides.emplace(key_of(river, qh_jh), std::vector<double>{0.75, 0.25});
  const HeadsUpState after_river_jam = river.after_action(0, {ActionType::Bet, 1});
  overrides.emplace(key_of(after_river_jam, ac_kc), std::vector<double>{1.0, 0.0});
  overrides.emplace(key_of(after_river_jam, qd_jd), std::vector<double>{0.0, 1.0});

  const PublishedFixture fixture = publish_game(game, 1, overrides, dir, "hook");
  const LoadedArtifact loaded = load_artifact(fixture.policy_path);
  const HeadsUpPolicy& policy = loaded.bundle.result.policy;
  std::vector<RootLoadResult> results;
  ResidentPolicySet residents = ResidentPolicySet::build(
      {{fixture.policy_path, parse_sha256(fixture.sha256_hex)}}, {}, &results);
  CHECK(results[0].status == RootStatus::Advertised);

  const std::vector<OracleDeal> deals = oracle_deals(game);
  CHECK(deals.size() == 4);
  const std::array<int, 2> own = as_ks;

  // Independent oracle replay for one state: zeroes any joint deal holding a
  // newly dealt card, divides by the per-deal 1/legal-cards support (the
  // solver's public_cards set), and multiplies BOTH players' observed action
  // probabilities (the player-0 factor is the responder's own conditioning).
  auto oracle_reach = [&](const HeadsUpState& target) -> std::vector<double> {
    std::vector<double> reach(deals.size(), 0.0);
    for (std::size_t d = 0; d < deals.size(); ++d)
      if (deals[d].hands[0] == own)
        reach[d] = deals[d].probability;
    HeadsUpState cursor(game.root);
    std::size_t event_index = 0;
    for (std::size_t street = 0; street < 3; ++street) {
      const std::size_t board_index = 2 + street;
      if (street > 0 && target.board().size() > board_index) {
        const int dealt = target.board()[board_index];
        for (std::size_t d = 0; d < deals.size(); ++d) {
          if (reach[d] == 0.0)
            continue;
          bool holds_dealt = false;
          for (const auto& hand : deals[d].hands)
            for (int held : hand)
              holds_dealt = holds_dealt || held == dealt;
          if (holds_dealt) {
            reach[d] = 0.0;  // deal is outside the per-deal chance support
            continue;
          }
          std::array<bool, 52> used{};
          for (int board_card : cursor.board())
            used[board_card] = true;
          for (const auto& hand : deals[d].hands)
            for (int held : hand)
              used[held] = true;
          for (auto fixed : game.fixed_runout)
            if (fixed)
              used[*fixed] = true;
          int legal = 0;
          for (int c = 0; c < 52; ++c)
            if (!used[c])
              ++legal;
          reach[d] /= static_cast<double>(legal);
        }
        cursor = cursor.after_card(dealt);
      }
      while (event_index < target.history().size() &&
             target.history()[event_index].street == cursor.street()) {
        const BettingEvent& event = target.history()[event_index];
        const auto actions = abstract_actions(cursor, game.sizes);
        std::size_t action_index = 0;
        bool found_action = false;
        for (std::size_t i = 0; i < actions.size(); ++i)
          if (actions[i] == event.action) {
            action_index = i;
            found_action = true;
          }
        CHECK_ORACLE(found_action);
        for (std::size_t d = 0; d < deals.size(); ++d) {
          if (reach[d] == 0.0)
            continue;
          const auto* row = policy.lookup(cursor, deals[d].hands[event.actor]);
          CHECK_ORACLE(row != nullptr);
          reach[d] *= row->probabilities[action_index];
        }
        cursor = cursor.after_action(event.actor, event.action);
        ++event_index;
      }
    }
    return reach;
  };

  auto compare = [&](const HeadsUpState& state) -> bool {
    const UnifiedNode node = to_unified(state);
    for (const bool best : {false, true}) {
      const std::vector<double> solver_reach = oracle_reach(state);

      // Resident per-deal reach reconstructed from the public raw factors.
      ResidentScratch scratch;
      const ResidentAnswer answer =
          residents.public_belief(node.state, node.history, std::nullopt, scratch);
      CHECK(answer.hit);
      std::vector<double> resident_reach(deals.size(), 0.0);
      double resident_mass = 0;
      for (std::size_t d = 0; d < deals.size(); ++d) {
        if (deals[d].hands[0] != own)
          continue;
        const int c0 = bs::comboIndex(deals[d].hands[0][0], deals[d].hands[0][1]);
        const int c1 = bs::comboIndex(deals[d].hands[1][0], deals[d].hands[1][1]);
        resident_reach[d] = scratch.raw[0][c0] * scratch.raw[1][c1];
        resident_mass += resident_reach[d];
      }
      double solver_mass = 0;
      for (double reach_value : solver_reach)
        solver_mass += reach_value;
      CHECK(solver_mass > 0);
      CHECK(resident_mass > 0);

      const double solver_value = HeadsUpSolverDebug::response_value_with_reach(
          game, policy, state, 0, own, best, solver_reach, fast_limits());
      const double resident_value = HeadsUpSolverDebug::response_value_with_reach(
          game, policy, state, 0, own, best, resident_reach, fast_limits());
      // The two reach vectors differ only by a positive constant (chance
      // factors and per-node normalization); response values are homogeneous
      // in reach, so the normalized values must agree tightly.
      CHECK(near(solver_value / solver_mass, resident_value / resident_mass, 1e-10));
    }
    return true;
  };

  // Flop (own non-constant action), the free-card-equivalent turn node, the
  // turn/river observed-action nodes, and the river fold/call decision.
  CHECK(compare(s_bet));
  CHECK(compare(turn));
  CHECK(compare(after_turn_bet));
  CHECK(compare(river));
  CHECK(compare(after_river_jam));

  // A genuinely free turn card (river fixed 8s): the independent oracle must
  // zero the joint deals that hold the dealt card. Player-0's own combo here
  // is the surviving QhJh so the response still has positive reach mass.
  HeadsUpGame free_game = canonicalize(free_turn_game());
  const PublishedFixture free_fixture = publish_game(free_game, 1, overrides, dir, "hook-free");
  const LoadedArtifact free_loaded = load_artifact(free_fixture.policy_path);
  const HeadsUpPolicy& free_policy = free_loaded.bundle.result.policy;
  ResidentPolicySet free_residents = ResidentPolicySet::build(
      {{free_fixture.policy_path, parse_sha256(free_fixture.sha256_hex)}}, {}, &results);
  const HeadsUpState free_root(free_game.root);
  const HeadsUpState free_turn_state = free_root.after_action(0, {ActionType::Check})
                                           .after_action(1, {ActionType::Check})
                                           .after_card(card("As"));
  const UnifiedNode free_turn_node = to_unified(free_turn_state);
  const std::vector<OracleDeal> free_deals = oracle_deals(free_game);
  for (const bool best : {false, true}) {
    std::vector<double> reach(free_deals.size(), 0.0);
    for (std::size_t d = 0; d < free_deals.size(); ++d)
      if (free_deals[d].hands[0] == qh_jh)
        reach[d] = free_deals[d].probability;
    for (std::size_t d = 0; d < free_deals.size(); ++d) {
      if (reach[d] == 0.0)
        continue;
      bool holds_as = false;
      for (const auto& hand : free_deals[d].hands)
        for (int held : hand)
          holds_as = holds_as || held == card("As");
      if (holds_as)
        reach[d] = 0.0;
    }
    double mass = 0;
    for (double value : reach)
      mass += value;
    CHECK(mass > 0);
    ResidentScratch scratch;
    const ResidentAnswer answer = free_residents.public_belief(
        free_turn_node.state, free_turn_node.history, std::nullopt, scratch);
    CHECK(answer.hit);
    std::vector<double> resident_reach(free_deals.size(), 0.0);
    double resident_mass = 0;
    for (std::size_t d = 0; d < free_deals.size(); ++d) {
      if (free_deals[d].hands[0] != qh_jh)
        continue;
      const int c0 = bs::comboIndex(free_deals[d].hands[0][0], free_deals[d].hands[0][1]);
      const int c1 = bs::comboIndex(free_deals[d].hands[1][0], free_deals[d].hands[1][1]);
      resident_reach[d] = scratch.raw[0][c0] * scratch.raw[1][c1];
      resident_mass += resident_reach[d];
    }
    CHECK(resident_mass > 0);
    const double solver_value = HeadsUpSolverDebug::response_value_with_reach(
        free_game, free_policy, free_turn_state, 0, qh_jh, best, reach, fast_limits());
    const double resident_value = HeadsUpSolverDebug::response_value_with_reach(
        free_game, free_policy, free_turn_state, 0, qh_jh, best, resident_reach, fast_limits());
    CHECK(near(solver_value / mass, resident_value / resident_mass, 1e-10));
  }
  return true;
}

// ---------------------------------------------------------------------------
// Root marginals with a cross-player blocked joint deal (matrix ranges).
// ---------------------------------------------------------------------------

static bool test_cross_blocked_root(const fs::path& dir) {
  HeadsUpGame game = canonicalize(matrix_game());
  const PublishedFixture fixture = publish_game(game, 4, {}, dir, "matrix");
  std::vector<RootLoadResult> results;
  ResidentPolicySet residents = ResidentPolicySet::build(
      {{fixture.policy_path, parse_sha256(fixture.sha256_hex)}}, {}, &results);
  CHECK(results[0].status == RootStatus::Advertised);

  // Independent oracle over the nine pairs, dropping AcAd vs AcAs.
  double margin[2][3] = {};
  double total = 0;
  for (std::size_t i = 0; i < game.ranges[0].size(); ++i)
    for (std::size_t j = 0; j < game.ranges[1].size(); ++j) {
      const auto& a = game.ranges[0][i];
      const auto& b = game.ranges[1][j];
      if (a.cards[0] == b.cards[0] || a.cards[0] == b.cards[1] || a.cards[1] == b.cards[0] ||
          a.cards[1] == b.cards[1])
        continue;
      const double product = a.weight * b.weight;
      margin[0][i] += product;
      margin[1][j] += product;
      total += product;
    }
  const HeadsUpState root(game.root);
  const UnifiedNode root_node = to_unified(root);
  ResidentScratch scratch;
  const ResidentAnswer answer =
      residents.public_belief(root_node.state, root_node.history, std::nullopt, scratch);
  CHECK(answer.hit);
  for (std::size_t i = 0; i < game.ranges[0].size(); ++i)
    CHECK(near((*answer.public_reach[0])[bs::comboIndex(game.ranges[0][i].cards[0],
                                                        game.ranges[0][i].cards[1])],
               margin[0][i] / total));
  for (std::size_t j = 0; j < game.ranges[1].size(); ++j)
    CHECK(near((*answer.public_reach[1])[bs::comboIndex(game.ranges[1][j].cards[0],
                                                        game.ranges[1][j].cards[1])],
               margin[1][j] / total));

  // Hero AcAd removes the opponent AcAs combination through the blocker view.
  const ResidentAnswer hero = residents.hero_decision(
      root_node.state, root_node.history, {card("Ac"), card("Ad")}, std::nullopt, scratch);
  CHECK(hero.hit);
  CHECK(hero.opponent_blocked_reach[bs::comboIndex(card("Ac"), card("As"))] == 0.0);
  CHECK(hero.opponent_blocked_reach[bs::comboIndex(card("Tc"), card("Td"))] > 0.0);
  CHECK(hero.opponent_blocked_reach[bs::comboIndex(card("Ah"), card("Kh"))] > 0.0);
  return true;
}

// ---------------------------------------------------------------------------
// #8/#9: observed actions on turn and river and non-constant player-0
// conditioning, with exact hand-derived fractions.
// ---------------------------------------------------------------------------

static bool test_street_conditioning(const fs::path& dir) {
  HeadsUpGame game = canonicalize(golden_game());
  const HeadsUpState root(game.root);
  const HeadsUpState after_check = root.after_action(0, {ActionType::Check});
  const HeadsUpState checked = after_check.after_action(1, {ActionType::Check});
  const HeadsUpState turn = checked.after_card(card("9h"));
  const HeadsUpState after_bet = root.after_action(0, {ActionType::Bet, 1});
  const HeadsUpState after_turn_bet = turn.after_action(0, {ActionType::Bet, 1});
  const HeadsUpState after_turn_call = after_turn_bet.after_action(1, {ActionType::Call});
  const HeadsUpState river = after_turn_call.after_card(card("8s"));
  const HeadsUpState after_river_jam = river.after_action(0, {ActionType::Bet, 1});
  const HeadsUpState after_river_reply = after_river_jam.after_action(1, {ActionType::Call});

  const UnifiedNode after_bet_node = to_unified(after_bet);
  const UnifiedNode turn_node = to_unified(turn);
  const UnifiedNode after_turn_bet_node = to_unified(after_turn_bet);
  const UnifiedNode after_turn_call_node = to_unified(after_turn_call);
  const UnifiedNode after_river_jam_node = to_unified(after_river_jam);
  const UnifiedNode after_river_reply_node = to_unified(after_river_reply);

  const std::array<int, 2> as_ks{card("Ks"), card("As")};
  const std::array<int, 2> qh_jh{card("Jh"), card("Qh")};
  Overrides overrides;
  // Flop player 0: non-constant check and bet factors (#9).
  overrides.emplace(key_of(root, as_ks), std::vector<double>{0.5, 0.25, 0.25});
  overrides.emplace(key_of(root, qh_jh), std::vector<double>{0.2, 0.3, 0.5});
  overrides.emplace(key_of(after_bet, {card("Kc"), card("Ac")}),
                    std::vector<double>{0.2, 0.6, 0.2});
  overrides.emplace(key_of(after_bet, {card("Jd"), card("Qd")}),
                    std::vector<double>{0.25, 0.75, 0.0});
  // Turn player 0 factors differ again.
  overrides.emplace(key_of(turn, as_ks), std::vector<double>{0.5, 0.25, 0.25});
  overrides.emplace(key_of(turn, qh_jh), std::vector<double>{0.1, 0.4, 0.5});
  overrides.emplace(key_of(after_turn_bet, {card("Kc"), card("Ac")}),
                    std::vector<double>{0.2, 0.6, 0.2});
  overrides.emplace(key_of(after_turn_bet, {card("Jd"), card("Qd")}),
                    std::vector<double>{0.25, 0.75, 0.0});
  // River player 0 jam factors; player 1 split folds versus calls.
  overrides.emplace(key_of(river, as_ks), std::vector<double>{0.5, 0.5});
  overrides.emplace(key_of(river, qh_jh), std::vector<double>{0.75, 0.25});
  overrides.emplace(key_of(after_river_jam, {card("Kc"), card("Ac")}),
                    std::vector<double>{1.0, 0.0});
  overrides.emplace(key_of(after_river_jam, {card("Jd"), card("Qd")}),
                    std::vector<double>{0.0, 1.0});

  const PublishedFixture fixture = publish_game(game, 1, overrides, dir, "streets");
  std::vector<RootLoadResult> results;
  ResidentPolicySet residents = ResidentPolicySet::build(
      {{fixture.policy_path, parse_sha256(fixture.sha256_hex)}}, {}, &results);
  CHECK(results[0].status == RootStatus::Advertised);
  ResidentScratch scratch;

  // Flop bet: (0.4*0.25, 0.6*0.3) -> 5/14, 9/14 for player 0.
  CHECK(check_golden_marginals(
      residents.public_belief(after_bet_node.state, after_bet_node.history, std::nullopt, scratch),
      5.0 / 14.0, 9.0 / 14.0, 5.0 / 12.0, 7.0 / 12.0));
  // Check/check into the turn: check factors (0.5, 0.2) -> 5/8, 3/8.
  CHECK(check_golden_marginals(
      residents.public_belief(turn_node.state, turn_node.history, std::nullopt, scratch), 5.0 / 8.0,
      3.0 / 8.0, 5.0 / 12.0, 7.0 / 12.0));
  // Turn bet: (5/8*0.25, 3/8*0.4) -> 25/49, 24/49.
  CHECK(check_golden_marginals(
      residents.public_belief(after_turn_bet_node.state, after_turn_bet_node.history, std::nullopt,
                              scratch),
      25.0 / 49.0, 24.0 / 49.0, 5.0 / 12.0, 7.0 / 12.0));
  // Turn call: player 1 (0.6, 0.75) -> 4/11, 7/11.
  CHECK(check_golden_marginals(
      residents.public_belief(after_turn_call_node.state, after_turn_call_node.history,
                              std::nullopt, scratch),
      25.0 / 49.0, 24.0 / 49.0, 4.0 / 11.0, 7.0 / 11.0));
  // River jam: (25/49*0.5, 24/49*0.25) -> 25/37, 12/37.
  CHECK(check_golden_marginals(
      residents.public_belief(after_river_jam_node.state, after_river_jam_node.history,
                              std::nullopt, scratch),
      25.0 / 37.0, 12.0 / 37.0, 4.0 / 11.0, 7.0 / 11.0));
  // Fold/call reply leaves only QdJd for player 1 (terminal belief).
  const ResidentAnswer replied = residents.public_belief(
      after_river_reply_node.state, after_river_reply_node.history, std::nullopt, scratch);
  CHECK(replied.hit);
  CHECK(near(reach_of(replied, 0, card("As"), card("Ks")), 25.0 / 37.0));
  CHECK(near(reach_of(replied, 0, card("Qh"), card("Jh")), 12.0 / 37.0));
  CHECK(near(reach_of(replied, 1, card("Ac"), card("Kc")), 0.0));
  CHECK(near(reach_of(replied, 1, card("Qd"), card("Jd")), 1.0));

  // #4 failure-safe private conditioning: at the turn after the observed
  // check/check, AsKs checked with probability 0.5 here, so use a dedicated
  // forced variant in which it never checks.
  Overrides never_check = overrides;
  never_check.insert_or_assign(key_of(root, as_ks), std::vector<double>{0.0, 0.5, 0.5});
  never_check.insert_or_assign(key_of(root, qh_jh), std::vector<double>{1.0, 0.0, 0.0});
  const PublishedFixture zero_fixture =
      publish_game(game, 1, never_check, dir, "streets-zero-hero");
  ResidentPolicySet zero_residents = ResidentPolicySet::build(
      {{zero_fixture.policy_path, parse_sha256(zero_fixture.sha256_hex)}}, {}, &results);
  const ResidentAnswer zero_hero = zero_residents.hero_decision(turn_node.state, turn_node.history,
                                                                as_ks, std::nullopt, scratch);
  CHECK(!zero_hero.hit);
  CHECK(zero_hero.reason == MissReason::ZeroProbabilityHeroCombination);
  const ResidentAnswer live_hero = zero_residents.hero_decision(turn_node.state, turn_node.history,
                                                                qh_jh, std::nullopt, scratch);
  CHECK(live_hero.hit);
  // Public belief itself is still well defined and hero-independent.
  const ResidentAnswer public_after =
      zero_residents.public_belief(turn_node.state, turn_node.history, std::nullopt, scratch);
  CHECK(public_after.hit);
  CHECK(near(reach_of(public_after, 0, card("As"), card("Ks")), 0.0));
  CHECK(near(reach_of(public_after, 0, card("Qh"), card("Jh")), 1.0));
  return true;
}

// ---------------------------------------------------------------------------
// #1: free-chance orphan combination must not turn the node into
// MissingHistory. After the As turn every surviving player-1 combo shares a
// card with the KcJh player-0 combo, so a complete artifact has no row for it
// at that branch; the node itself is covered.
// ---------------------------------------------------------------------------

static bool test_orphan_free_chance(const fs::path& dir) {
  HeadsUpGame game;
  game.root = {{card("2c"), card("3d"), card("7h")}, {2, 2}, {1, 1}, 2, 1, 1};
  game.ranges[0] = {
      {{card("Kc"), card("Jh")}, 1}, {{card("Ad"), card("9s")}, 1}, {{card("Qc"), card("2h")}, 1}};
  game.ranges[1] = {
      {{card("As"), card("Kh")}, 1}, {{card("Kc"), card("Qd")}, 1}, {{card("Jh"), card("Th")}, 1}};
  game.fixed_runout = {std::nullopt, card("8s")};
  game = canonicalize(std::move(game));

  const PublishedFixture fixture = publish_game(game, 1, {}, dir, "orphan");
  std::vector<RootLoadResult> results;
  ResidentPolicySet residents = ResidentPolicySet::build(
      {{fixture.policy_path, parse_sha256(fixture.sha256_hex)}}, {}, &results);
  CHECK(results[0].status == RootStatus::Advertised);

  const HeadsUpState root(game.root);
  const HeadsUpState checked =
      root.after_action(0, {ActionType::Check}).after_action(1, {ActionType::Check});
  const HeadsUpState turn_as = checked.after_card(card("As"));
  const UnifiedNode turn_as_node = to_unified(turn_as);

  ResidentScratch scratch;
  const ResidentAnswer node =
      residents.public_belief(turn_as_node.state, turn_as_node.history, std::nullopt, scratch);
  CHECK(node.hit);  // the public node is covered despite the orphan
  CHECK(near(reach_of(node, 0, card("Jh"), card("Kc")), 0.0));  // orphan marginal
  CHECK(reach_of(node, 0, card("9s"), card("Ad")) > 0.0);
  CHECK(reach_of(node, 0, card("2h"), card("Qc")) > 0.0);
  // The opponent AsKh combination is board-blocked; the other two survive.
  CHECK(near(reach_of(node, 1, card("As"), card("Kh")), 0.0));
  CHECK(reach_of(node, 1, card("Kc"), card("Qd")) > 0.0);
  CHECK(reach_of(node, 1, card("Jh"), card("Th")) > 0.0);

  // The orphan hero combination has no row on this branch: UntrainedCombo,
  // never a fabricated policy.
  const ResidentAnswer orphan_hero = residents.hero_decision(
      turn_as_node.state, turn_as_node.history, {card("Jh"), card("Kc")}, std::nullopt, scratch);
  CHECK(!orphan_hero.hit);
  CHECK(orphan_hero.reason == MissReason::UntrainedCombo);
  // The other hero combinations have rows.
  CHECK(residents
            .hero_decision(turn_as_node.state, turn_as_node.history, {card("9s"), card("Ad")},
                           std::nullopt, scratch)
            .hit);
  CHECK(residents
            .hero_decision(turn_as_node.state, turn_as_node.history, {card("2h"), card("Qc")},
                           std::nullopt, scratch)
            .hit);

  // The reported defect fires when REPLAYING the orphan actor's observed
  // action on the turn to the opponent's response node: without the orphan
  // gate, read_action_probabilities requires a KcJh row that does not exist
  // and rejects the entire covered node as MissingHistory.
  const HeadsUpState after_turn_check = turn_as.after_action(0, {ActionType::Check});
  const UnifiedNode after_turn_check_node = to_unified(after_turn_check);
  const ResidentAnswer replied = residents.public_belief(
      after_turn_check_node.state, after_turn_check_node.history, std::nullopt, scratch);
  CHECK(replied.hit);
  CHECK(near(reach_of(replied, 0, card("Jh"), card("Kc")), 0.0));
  CHECK(reach_of(replied, 0, card("9s"), card("Ad")) > 0.0);

  // On a different free turn (2s) all three opponents survive, the orphan is
  // no longer orphan, and its row exists and marginal is positive.
  const HeadsUpState turn_2s = checked.after_card(card("2s"));
  const UnifiedNode turn_2s_node = to_unified(turn_2s);
  const ResidentAnswer node_2s =
      residents.public_belief(turn_2s_node.state, turn_2s_node.history, std::nullopt, scratch);
  CHECK(node_2s.hit);
  CHECK(reach_of(node_2s, 0, card("Jh"), card("Kc")) > 0.0);
  CHECK(residents
            .hero_decision(turn_2s_node.state, turn_2s_node.history, {card("Jh"), card("Kc")},
                           std::nullopt, scratch)
            .hit);
  return true;
}

// ---------------------------------------------------------------------------
// #2: zero and fully cross-blocked joint ranges are refused at startup and
// queries fail closed with EmptyJointRange, never hit plus NaN belief.
// ---------------------------------------------------------------------------

static bool test_empty_joint_artifacts(const fs::path& dir) {
  // Two single combos sharing a card: every joint deal is incompatible.
  HeadsUpGame shared;
  shared.root = {{card("2c"), card("3d"), card("7h")}, {2, 2}, {1, 1}, 2, 1, 1};
  shared.ranges[0] = {{{card("As"), card("Ks")}, 1}};
  shared.ranges[1] = {{{card("As"), card("Qh")}, 1}};
  shared.fixed_runout = {card("9h"), card("8s")};
  shared = canonicalize(std::move(shared));
  const PublishedFixture shared_fixture = publish_synthetic_policy(shared, dir, "empty-shared");

  // Declared range with no positive weight. Do not run the trainer-style
  // max normalization: the artifact reader itself accepts nonnegative
  // weights, and a max of zero is exactly the condition the resident gate
  // must catch.
  HeadsUpGame zero_weight;
  zero_weight.root = {{card("2c"), card("3d"), card("7h")}, {3, 3}, {1, 1}, 2, 1, 1};
  zero_weight.ranges[0] = {{{card("As"), card("Ks")}, 0.0}};
  zero_weight.ranges[1] = {{{card("Ac"), card("Kc")}, 1}};
  zero_weight.fixed_runout = shared.fixed_runout;
  for (auto& range : zero_weight.ranges)
    for (auto& hand : range)
      std::sort(hand.cards.begin(), hand.cards.end());
  const PublishedFixture zero_fixture = publish_synthetic_policy(zero_weight, dir, "empty-zero");

  std::vector<RootLoadResult> results;
  ResidentPolicySet residents = ResidentPolicySet::build(
      {{shared_fixture.policy_path, parse_sha256(shared_fixture.sha256_hex)},
       {zero_fixture.policy_path, parse_sha256(zero_fixture.sha256_hex)}},
      {}, &results);
  CHECK(results.size() == 2);
  CHECK(results[0].status == RootStatus::InvalidRange);
  CHECK(results[1].status == RootStatus::InvalidRange);
  CHECK(residents.advertised_roots() == 0);

  ResidentScratch scratch;
  const UnifiedNode shared_node = to_unified(HeadsUpState(shared.root));
  const ResidentAnswer shared_answer =
      residents.public_belief(shared_node.state, shared_node.history, std::nullopt, scratch);
  CHECK(!shared_answer.hit);
  CHECK(shared_answer.reason == MissReason::EmptyJointRange);
  const UnifiedNode zero_node = to_unified(HeadsUpState(zero_weight.root));
  const ResidentAnswer zero_answer =
      residents.public_belief(zero_node.state, zero_node.history, std::nullopt, scratch);
  CHECK(!zero_answer.hit);
  CHECK(zero_answer.reason == MissReason::EmptyJointRange);

  // The probe itself succeeds (the artifact is well formed); only resident
  // advertising refuses it.
  const ArtifactProbe probe = probe_artifact(
      shared_fixture.policy_path,
      LoadOptions{kDefaultMaxArtifactBytes, parse_sha256(shared_fixture.sha256_hex)});
  CHECK(probe.information_sets == 1);
  CHECK(probe.action_count >= 1);
  return true;
}

// ---------------------------------------------------------------------------
// Symmetric orphan regression: the orphan gate must be exact for the ACTOR-1
// case too. After the As free turn, player-1's KhQh has no card-compatible
// player-0 combination (KhKc shares Kh, QhJd shares Qh, As9d holds the dealt
// As), while 9sTs stays live. Replaying player-1's check therefore reads a
// KhQh row that does not exist. The stale-scale bug (card_mass[0] not folded
// with raw[0]) falsely reported this covered node as MissingHistory.
// ---------------------------------------------------------------------------

static bool test_orphan_actor_one(const fs::path& dir) {
  HeadsUpGame game;
  game.root = {{card("2c"), card("3d"), card("7h")}, {2, 2}, {1, 1}, 2, 1, 1};
  game.ranges[0] = {
      {{card("Kh"), card("Kc")}, 1}, {{card("Qh"), card("Jd")}, 1}, {{card("As"), card("9d")}, 1}};
  game.ranges[1] = {{{card("Kh"), card("Qh")}, 1}, {{card("9s"), card("Ts")}, 1}};
  game.fixed_runout = {std::nullopt, card("8c")};
  game = canonicalize(std::move(game));

  // The stale-scale bug only bites after a NON-CONSTANT player-0 action
  // factor: a constant factor is cancelled by renormalization and leaves
  // card_mass[0] consistent. Force nonuniform turn-check probabilities for
  // the two live player-0 combos (As9d is board-blocked on the As turn).
  const HeadsUpState pre_root(game.root);
  const HeadsUpState pre_checked =
      pre_root.after_action(0, {ActionType::Check}).after_action(1, {ActionType::Check});
  const HeadsUpState pre_turn = pre_checked.after_card(card("As"));
  Overrides overrides;
  overrides.emplace(key_of(pre_turn, {card("Kh"), card("Kc")}),
                    std::vector<double>{0.5, 0.25, 0.25});
  overrides.emplace(key_of(pre_turn, {card("Jd"), card("Qh")}), std::vector<double>{0.2, 0.3, 0.5});
  const PublishedFixture fixture = publish_game(game, 1, overrides, dir, "orphan-p1");

  std::vector<RootLoadResult> results;
  ResidentPolicySet residents = ResidentPolicySet::build(
      {{fixture.policy_path, parse_sha256(fixture.sha256_hex)}}, {}, &results);
  CHECK(results[0].status == RootStatus::Advertised);

  const HeadsUpState root(game.root);
  const HeadsUpState flop_checked =
      root.after_action(0, {ActionType::Check}).after_action(1, {ActionType::Check});
  const HeadsUpState turn = flop_checked.after_card(card("As"));
  const HeadsUpState after_turn_check = turn.after_action(0, {ActionType::Check});
  // The defect node is the boundary AFTER player-1 also checks on the turn:
  // replaying that player-1 check reads the orphan's (absent) row, which is
  // where the stale player-0 card masses falsely made it look non-orphan.
  const HeadsUpState after_turn_check_check = after_turn_check.after_action(1, {ActionType::Check});
  CHECK(after_turn_check_check.phase() == Phase::Deal);
  const UnifiedNode after_turn_check_node = to_unified(after_turn_check);
  const UnifiedNode after_turn_check_check_node = to_unified(after_turn_check_check);

  ResidentScratch scratch;
  // The reported defect: replaying player-1's turn check at the covered node.
  const ResidentAnswer node =
      residents.public_belief(after_turn_check_check_node.state,
                              after_turn_check_check_node.history, std::nullopt, scratch);
  CHECK(node.hit);

  // The nonuniform player-0 conditioning leaves both live combos positive and
  // the board-blocked combo at zero; the exact products are checked by the
  // solver-hook tests, here the orphaned player-1 combination must have
  // marginal exactly zero and the surviving combo all of player 1.
  CHECK(near(reach_of(node, 1, card("Kh"), card("Qh")), 0.0));  // orphan
  CHECK(near(reach_of(node, 1, card("9s"), card("Ts")), 1.0));
  CHECK(near(reach_of(node, 0, card("9d"), card("As")), 0.0));  // board removed
  CHECK(reach_of(node, 0, card("Kh"), card("Kc")) > 0.0);
  CHECK(reach_of(node, 0, card("Jd"), card("Qh")) > 0.0);

  // The orphan player-1 combination has no row on this branch.
  const ResidentAnswer orphan_hero =
      residents.hero_decision(after_turn_check_node.state, after_turn_check_node.history,
                              {card("Kh"), card("Qh")}, std::nullopt, scratch);
  CHECK(!orphan_hero.hit);
  CHECK(orphan_hero.reason == MissReason::UntrainedCombo);
  // The live player-1 combination has its row.
  CHECK(residents
            .hero_decision(after_turn_check_node.state, after_turn_check_node.history,
                           {card("9s"), card("Ts")}, std::nullopt, scratch)
            .hit);

  // Representation invariant after folds: player-0 cached card masses stay in
  // raw[0]'s (folded) units, so has_positive_partner is exact for both actors.
  // card_mass counts each live combo's two cards, so its sum is twice the raw
  // marginal sum in the same units.
  double raw0_sum = 0;
  double card0_sum = 0;
  for (int combo = 0; combo < bs::N_COMBOS; ++combo)
    raw0_sum += scratch.raw[0][combo];
  for (int c = 0; c < 52; ++c)
    card0_sum += scratch.card_mass[0][c];
  CHECK(near(card0_sum, 2.0 * raw0_sum, 1e-12));
  return true;
}

// ---------------------------------------------------------------------------
// #5: a free chance slot dealing a card reserved for a later fixed slot is
// outside the trained chance support.
// ---------------------------------------------------------------------------

static bool test_free_slot_reserved_card(const fs::path& dir) {
  HeadsUpGame game = canonicalize(free_turn_game());
  const PublishedFixture fixture = publish_game(game, 1, {}, dir, "free-slot");
  std::vector<RootLoadResult> results;
  ResidentPolicySet residents = ResidentPolicySet::build(
      {{fixture.policy_path, parse_sha256(fixture.sha256_hex)}}, {}, &results);
  CHECK(results[0].status == RootStatus::Advertised);
  const HeadsUpState root(game.root);
  const HeadsUpState checked =
      root.after_action(0, {ActionType::Check}).after_action(1, {ActionType::Check});
  // Free turn dealing the fixed river reservation (8s).
  const HeadsUpState illegal_turn = checked.after_card(card("8s"));
  const UnifiedNode illegal_turn_node = to_unified(illegal_turn);
  ResidentScratch scratch;
  const ResidentAnswer answer = residents.public_belief(
      illegal_turn_node.state, illegal_turn_node.history, std::nullopt, scratch);
  CHECK(!answer.hit);
  CHECK(answer.reason == MissReason::OffTree);
  // Hero selection at that node must miss identically.
  const ResidentAnswer hero_answer =
      residents.hero_decision(illegal_turn_node.state, illegal_turn_node.history,
                              game.ranges[0].front().cards, std::nullopt, scratch);
  CHECK(!hero_answer.hit);
  CHECK(hero_answer.reason == MissReason::OffTree);
  return true;
}

// ---------------------------------------------------------------------------
// #10: the same two-card combination may legally appear in both declared
// ranges; its self-pair must contribute nothing and the inclusion-exclusion
// add-back must execute.
// ---------------------------------------------------------------------------

static bool test_shared_combo_ranges(const fs::path& dir) {
  HeadsUpGame game;
  game.root = {{card("2c"), card("3d"), card("7h")}, {2, 2}, {1, 1}, 2, 1, 1};
  game.ranges[0] = {{{card("As"), card("Ks")}, 2}, {{card("Qh"), card("Jh")}, 3}};
  game.ranges[1] = {{{card("As"), card("Ks")}, 5}, {{card("Qd"), card("Jd")}, 7}};
  game.fixed_runout = {card("9h"), card("8s")};
  game = canonicalize(std::move(game));

  const PublishedFixture fixture = publish_game(game, 1, {}, dir, "shared-combo");
  std::vector<RootLoadResult> results;
  ResidentPolicySet residents = ResidentPolicySet::build(
      {{fixture.policy_path, parse_sha256(fixture.sha256_hex)}}, {}, &results);
  CHECK(results[0].status == RootStatus::Advertised);

  // Brute-force oracle: three compatible deals out of four pairs.
  std::array<double, bs::N_COMBOS> expected0{};
  std::array<double, bs::N_COMBOS> expected1{};
  double total = 0;
  for (std::size_t i = 0; i < game.ranges[0].size(); ++i)
    for (std::size_t j = 0; j < game.ranges[1].size(); ++j) {
      const auto& a = game.ranges[0][i];
      const auto& b = game.ranges[1][j];
      if (a.cards[0] == b.cards[0] || a.cards[0] == b.cards[1] || a.cards[1] == b.cards[0] ||
          a.cards[1] == b.cards[1])
        continue;  // same-combo self-pair and other cross-blocks contribute zero
      const double product = a.weight * b.weight;
      expected0[bs::comboIndex(a.cards[0], a.cards[1])] += product;
      expected1[bs::comboIndex(b.cards[0], b.cards[1])] += product;
      total += product;
    }
  CHECK(total > 0.0);  // exact ratios below use the canonicalized weights
  ResidentScratch scratch;
  const UnifiedNode root_node = to_unified(HeadsUpState(game.root));
  const ResidentAnswer answer =
      residents.public_belief(root_node.state, root_node.history, std::nullopt, scratch);
  CHECK(answer.hit);
  for (int combo = 0; combo < bs::N_COMBOS; ++combo) {
    CHECK(near((*answer.public_reach[0])[combo], expected0[combo] / total));
    CHECK(near((*answer.public_reach[1])[combo], expected1[combo] / total));
  }
  return true;
}

// ---------------------------------------------------------------------------
// #6: the additive artifact probe agrees with the full load aggregates.
// ---------------------------------------------------------------------------

static bool test_probe_api(const fs::path& dir) {
  const HeadsUpGame game = canonicalize(golden_game());
  const PublishedFixture fixture = publish_game(game, 1, {}, dir, "probe");
  const ArtifactProbe probe = probe_artifact(
      fixture.policy_path, LoadOptions{kDefaultMaxArtifactBytes, parse_sha256(fixture.sha256_hex)});
  const LoadedArtifact loaded = load_artifact(fixture.policy_path);
  const auto& rows = loaded.bundle.result.policy.rows();
  CHECK(probe.information_sets == rows.size());
  std::uint64_t actions = 0;
  std::uint64_t words = 0;
  std::uint64_t max_words = 0;
  for (const auto& [key, row] : rows) {
    actions += row.actions.size();
    words += key.size();
    max_words = std::max(max_words, static_cast<std::uint64_t>(key.size()));
  }
  CHECK(probe.action_count == actions);
  CHECK(probe.total_key_words == words);
  CHECK(probe.max_key_words == max_words);
  CHECK(probe.sha256_hex == fixture.sha256_hex);
  CHECK(probe.file_bytes > 0);

  // The probe performs the same digest and corruption checks.
  bool digest_rejected = false;
  try {
    (void)probe_artifact(fixture.policy_path,
                         LoadOptions{kDefaultMaxArtifactBytes, Sha256Digest{}});
  } catch (const ArtifactError& error) {
    digest_rejected = error.kind() == ArtifactErrorKind::DigestMismatch;
  }
  CHECK(digest_rejected);

  // Probe calibration: the pre-gate estimate must be a tight UPPER bound on
  // the exact footprint measured after the index is built, on every fixture
  // depth. A bound below exact would risk advertising an over-budget root.
  for (const auto& [make, name, iterations] :
       {std::tuple<HeadsUpGame (*)(), const char*, std::uint64_t>{golden_game, "g", 1},
        {free_turn_game, "f", 1},
        {matrix_game, "m", 4}}) {
    const PublishedFixture deep_fixture =
        publish_game(canonicalize(make()), iterations, {}, dir, std::string("probe-") + name);
    const ArtifactProbe deep_probe = probe_artifact(
        deep_fixture.policy_path,
        LoadOptions{kDefaultMaxArtifactBytes, parse_sha256(deep_fixture.sha256_hex)});
    const LoadedArtifact deep_loaded = load_artifact(deep_fixture.policy_path);
    const HeadsUpPolicy& deep_policy = deep_loaded.bundle.result.policy;
    ResidentIndex exact_index;
    exact_index.build(deep_policy.rows());
    // Reproduce the resident library's own game-copy measurement.
    const UnifiedGame unified = to_unified_game(deep_policy.game());
    std::size_t exact_bytes = exact_index.resident_bytes();
    exact_bytes += kUnifiedGameCopyAccountingBytes;
    for (const auto& range : unified.ranges)
      exact_bytes += range.capacity() * sizeof(WeightedHand);
    for (const auto& street : unified.sizes) {
      exact_bytes += street.bets.capacity() * sizeof(Fraction);
      exact_bytes += street.raises.capacity() * sizeof(Fraction);
    }
    const std::size_t upper = ResidentIndex::estimate_bytes(
        static_cast<std::size_t>(deep_probe.information_sets),
        static_cast<std::size_t>(deep_probe.action_count),
        static_cast<std::size_t>(deep_probe.total_key_words), unified);
    CHECK(upper >= exact_bytes);
    // Tightness: the bound must not overshoot by a large multiple.
    CHECK(upper <= exact_bytes + deep_probe.information_sets * 40 + 4096);
  }

  // A checkpoint is not a valid policy probe.
  const fs::path checkpoint = dir / "probe-ck-checkpoint.db";
  {
    const DebugTrainingOutput trained = HeadsUpSolverDebug::train_full(game, 1, fast_limits());
    TrainingRows raw_rows;
    for (const auto& [key, row] : trained.rows)
      raw_rows.emplace(key, TrainingRow{row.actions, row.regrets, row.sums});
    CheckpointProvenance provenance;
    provenance.prng_identifier = kFullTraversalPrngIdentifier;
    provenance.engine_revision = "probe-ck-engine";
    create_checkpoint(checkpoint, trained.result, raw_rows, provenance);
  }
  bool checkpoint_rejected = false;
  try {
    (void)probe_artifact(checkpoint,
                         LoadOptions{kDefaultMaxArtifactBytes, parse_sha256(fixture.sha256_hex)});
  } catch (const ArtifactError&) {
    checkpoint_rejected = true;
  }
  CHECK(checkpoint_rejected);
  return true;
}

// ---------------------------------------------------------------------------
// RFC 0009 W2c-ii-a: schema-v2 flop-rooted resident projection.
//
// The resident data layer projects a flop-rooted v2 SeatPolicy onto the
// unified GameState view and serves it through the resident query path. A
// two-seat source answers belief and hero queries; a three-seat source is
// advertised at load but misses every resident query with
// SeatCountNotSupported until the belief path generalizes (W2c-ii-b). A
// turn/river-rooted source is still refused at load (board_size != 3).
// ---------------------------------------------------------------------------

static bool test_v2_resident_projection(const fs::path& dir) {
  // Two-seat flop-rooted v2: advertised and served through the projection.
  const PublishedV2Fixture fixture =
      publish_v2(two_seat_flop_def_v2(), two_seat_flop_ranges_v2(), 200, dir, "v2proj");

  std::vector<RootLoadResult> results;
  ResidentPolicySet residents = ResidentPolicySet::build(
      {{fixture.policy_path, parse_sha256(fixture.sha256_hex)}}, {}, &results);
  CHECK(results.size() == 1);
  CHECK(results[0].status == RootStatus::Advertised);
  CHECK(residents.advertised_roots() == 1);

  // The projected root answers a public-belief query at the root. The root
  // actor is the non-button seat (button 0 -> seat 1), matching the GameState
  // opener, so the root decision rows are seat 1's.
  const GameState root_state(two_seat_flop_def_v2());
  const std::vector<PublicAction> root_history;
  ResidentScratch scratch;
  const ResidentAnswer belief =
      residents.public_belief(root_state, root_history, std::nullopt, scratch);
  CHECK(belief.hit);
  CHECK(belief.reason == MissReason::None);
  // Each player's marginal normalizes to one, confirming the declared ranges
  // flowed through the projection.
  for (std::size_t player = 0; player < 2; ++player) {
    double sum = 0.0;
    for (double mass : *belief.public_reach[player])
      sum += mass;
    CHECK(near(sum, 1.0, 1e-9));
  }

  // A hero decision at the root returns the seat-1 actor's row.
  ResidentScratch hero_scratch;
  const ResidentAnswer hero = residents.hero_decision(
      root_state, root_history, {card("Ac"), card("Kc")}, std::nullopt, hero_scratch);
  CHECK(hero.hit);
  CHECK(hero.hero_row.size > 0);
  double row_sum = 0.0;
  for (std::size_t i = 0; i < hero.hero_row.size; ++i)
    row_sum += hero.hero_row.probabilities[i];
  CHECK(near(row_sum, 1.0, 1e-9));

  // Three-seat v2: the load generalizes to the unified view and advertises,
  // and W2c-ii-b serves its belief and hero-decision queries exactly. The
  // fixture's combos are all mutually card-distinct, so the root marginals
  // are just the declared range weights normalized per seat.
  const PublishedV2Fixture three =
      publish_v2(three_seat_flop_def_v2(), three_seat_flop_ranges_v2(), 200, dir, "v2three");
  std::vector<RootLoadResult> three_results;
  ResidentPolicySet three_set = ResidentPolicySet::build(
      {{three.policy_path, parse_sha256(three.sha256_hex)}}, {}, &three_results);
  CHECK(three_results.size() == 1);
  CHECK(three_results[0].status == RootStatus::Advertised);
  CHECK(three_set.advertised_roots() == 1);

  const GameState three_state(three_seat_flop_def_v2());
  const std::vector<PublicAction> three_history;
  ResidentScratch three_scratch;
  const ResidentAnswer three_belief =
      three_set.public_belief(three_state, three_history, std::nullopt, three_scratch);
  CHECK(three_belief.hit);
  CHECK(three_belief.reason == MissReason::None);
  CHECK(three_belief.public_reach_seats == 3);
  // Each seat's marginal normalizes to one and matches the declared weights.
  {
    const double expected[3][2] = {
        {3.0 / 5.0, 2.0 / 5.0}, {7.0 / 12.0, 5.0 / 12.0}, {13.0 / 24.0, 11.0 / 24.0}};
    const int first[3][2] = {
        {card("8h"), card("8c")}, {card("8s"), card("8d")}, {card("Jh"), card("Jc")}};
    const int second[3][2] = {
        {card("Ah"), card("Ac")}, {card("As"), card("Ad")}, {card("Qh"), card("Qc")}};
    for (std::size_t seat = 0; seat < 3; ++seat) {
      double sum = 0.0;
      for (double mass : *three_belief.public_reach[seat])
        sum += mass;
      CHECK(near(sum, 1.0, 1e-9));
      const int c0 = bs::comboIndex(first[seat][0], first[seat][1]);
      const int c1 = bs::comboIndex(second[seat][0], second[seat][1]);
      CHECK(near((*three_belief.public_reach[seat])[c0], expected[seat][0], 1e-9));
      CHECK(near((*three_belief.public_reach[seat])[c1], expected[seat][1], 1e-9));
    }
  }

  // A hero decision at the root returns the seat-1 actor's row (button 0 ->
  // seat 1 acts first). The hero combo comes from seat 1's declared range.
  const ResidentAnswer three_hero = three_set.hero_decision(
      three_state, three_history, {card("As"), card("Ad")}, std::nullopt, three_scratch);
  CHECK(three_hero.hit);
  CHECK(three_hero.hero_row.size > 0);
  double three_row_sum = 0.0;
  for (std::size_t i = 0; i < three_hero.hero_row.size; ++i)
    three_row_sum += three_hero.hero_row.probabilities[i];
  CHECK(near(three_row_sum, 1.0, 1e-9));

  // Two-seat turn-rooted v2: the projection is flop-rooted only, so a
  // turn/river-rooted source is refused as LoadFailed at load.
  const PublishedV2Fixture turn =
      publish_v2(two_seat_turn_def_v2(), two_seat_flop_ranges_v2(), 200, dir, "v2turn");
  std::vector<RootLoadResult> turn_results;
  ResidentPolicySet turn_set = ResidentPolicySet::build(
      {{turn.policy_path, parse_sha256(turn.sha256_hex)}}, {}, &turn_results);
  CHECK(turn_results.size() == 1);
  CHECK(turn_results[0].status == RootStatus::LoadFailed);
  CHECK(turn_set.advertised_roots() == 0);

  return true;
}

// ---------------------------------------------------------------------------
// W4c-ii: a schema-v3 class policy (suit canonicalization) is trained on the
// canonical class representative and serves a query rooted at a non-canonical
// board of the same class. The resident boundary canonicalizes the query
// flop, relabels hero/board cards into the artifact's coordinate system, and
// answers in canonical coordinates. A board from a different class misses
// declared, and a turn query exercises the turn-card relabel and history
// replay with a canonical prefix board.
// ---------------------------------------------------------------------------

static bool test_v3_resident_projection(const fs::path& dir) {
  // Train and publish a v3 class policy on the canonical board {2s,7h,Kd}.
  const PublishedV2Fixture fixture =
      publish_v2(two_seat_flop_canonical_def_v3(), two_seat_flop_ranges_v3(), 200, dir, "v3proj",
                 bs::abstraction::suit_canonicalization_id());

  std::vector<RootLoadResult> results;
  ResidentPolicySet residents = ResidentPolicySet::build(
      {{fixture.policy_path, parse_sha256(fixture.sha256_hex)}}, {}, &results);
  CHECK(results.size() == 1);
  CHECK(results[0].status == RootStatus::Advertised);
  CHECK(residents.advertised_roots() == 1);

  // Query with the non-canonical board {2h,7s,Kd}: it canonicalizes to the
  // artifact's class representative {2s,7h,Kd}, so the query hits.
  const GameState noncanon_state(two_seat_flop_noncanonical_def_v3());
  const std::vector<PublicAction> root_history;
  ResidentScratch scratch;
  const ResidentAnswer belief =
      residents.public_belief(noncanon_state, root_history, std::nullopt, scratch);
  CHECK(belief.hit);
  CHECK(belief.reason == MissReason::None);
  // Each seat's marginal normalizes to one and matches the declared range
  // weights at the CANONICAL combo indices: the belief model operates in the
  // artifact's coordinate system, so the relabel is invisible to the caller.
  // seat0 has three combos (total 9); seat1 has two (total 12).
  {
    const double expected0[3] = {2.0 / 9.0, 3.0 / 9.0, 4.0 / 9.0};
    const int combos0[3][2] = {
        {card("As"), card("Ad")}, {card("Ah"), card("Ac")}, {card("9s"), card("9c")}};
    const double expected1[2] = {5.0 / 12.0, 7.0 / 12.0};
    const int combos1[2][2] = {{card("Qs"), card("Qd")}, {card("Qh"), card("Qc")}};
    for (std::size_t seat = 0; seat < 2; ++seat) {
      double sum = 0.0;
      for (double mass : *belief.public_reach[seat])
        sum += mass;
      CHECK(near(sum, 1.0, 1e-9));
    }
    for (int k = 0; k < 3; ++k) {
      const int c = bs::comboIndex(combos0[k][0], combos0[k][1]);
      CHECK(near((*belief.public_reach[0])[c], expected0[k], 1e-9));
    }
    for (int k = 0; k < 2; ++k) {
      const int c = bs::comboIndex(combos1[k][0], combos1[k][1]);
      CHECK(near((*belief.public_reach[1])[c], expected1[k], 1e-9));
    }
  }

  // A hero decision at the root returns the seat-1 actor's row. The hero's
  // CONCRETE cards {Qh,Qd} relabel to canonical {Qs,Qd}, which is in seat 1's
  // declared range; the blocking check stays in concrete coordinates.
  ResidentScratch hero_scratch;
  const ResidentAnswer hero = residents.hero_decision(
      noncanon_state, root_history, {card("Qh"), card("Qd")}, std::nullopt, hero_scratch);
  CHECK(hero.hit);
  CHECK(hero.hero_row.size > 0);
  double row_sum = 0.0;
  for (std::size_t i = 0; i < hero.hero_row.size; ++i)
    row_sum += hero.hero_row.probabilities[i];
  CHECK(near(row_sum, 1.0, 1e-9));

  // The resolver blueprint source also serves the v3 artifact. A
  // resolver_source over the non-canonical query board returns a row for the
  // actor's CONCRETE cards {Qh,Qd} (which relabel to canonical {Qs,Qd}),
  // exercising the v3 branch in ResidentBlueprintSource::row: canonicalize,
  // hero-card relabel, canonical prefix-board build, and key lookup.
  {
    auto source = residents.resolver_source(noncanon_state, root_history, std::nullopt);
    CHECK(source != nullptr);
    const auto bp_row = source->row(noncanon_state, root_history, 1, {card("Qh"), card("Qd")});
    CHECK(bp_row.has_value());
    CHECK(bp_row->size > 0);
    double bp_sum = 0.0;
    for (std::size_t i = 0; i < bp_row->size; ++i)
      bp_sum += bp_row->probabilities[i];
    CHECK(near(bp_sum, 1.0, 1e-9));
    // A hero combo blocked by the concrete board misses (blocking stays in
    // concrete coordinates).
    const auto blocked = source->row(noncanon_state, root_history, 1, {card("2h"), card("Qd")});
    CHECK(!blocked.has_value());
  }

  // The canonical board itself also hits (identity relabel).
  const GameState canon_state(two_seat_flop_canonical_def_v3());
  ResidentScratch canon_scratch;
  const ResidentAnswer canon_belief =
      residents.public_belief(canon_state, root_history, std::nullopt, canon_scratch);
  CHECK(canon_belief.hit);
  CHECK(canon_belief.reason == MissReason::None);

  // A board from a different class misses declared (RootNotSupported).
  GameDef other_def = two_seat_flop_noncanonical_def_v3();
  other_def.board = {card("3s"), card("7h"), card("Kd"), 0, 0};
  const GameState other_state(other_def);
  ResidentScratch other_scratch;
  const ResidentAnswer other =
      residents.public_belief(other_state, root_history, std::nullopt, other_scratch);
  CHECK(!other.hit);
  CHECK(other.reason == MissReason::RootNotSupported);

  // A turn query on the non-canonical board exercises the turn-card relabel
  // and the history replay with a canonical prefix board. After check/check on
  // the flop and dealing 9h (which relabels to 9s), the belief conditions
  // exactly. The relabel is discriminating: seat0's {9s,9c} combo must be
  // zeroed (9s is now on the canonical board), and the remaining seat0 combos
  // renormalize to {2/5, 3/5}. A no-op relabel would leave 9h != 9s and fail
  // to block the combo. The root actor is the non-button seat (button 0 ->
  // seat 1).
  const GameState after_seat1_check = noncanon_state.after_action(1, {ActionType::Check});
  const GameState after_flop_checks = after_seat1_check.after_action(0, {ActionType::Check});
  const GameState turn_state = after_flop_checks.after_card(card("9h"));
  const std::vector<PublicAction> turn_history = {
      {Street::Flop, 1, {ActionType::Check}},
      {Street::Flop, 0, {ActionType::Check}},
  };
  ResidentScratch turn_scratch;
  const ResidentAnswer turn_belief =
      residents.public_belief(turn_state, turn_history, std::nullopt, turn_scratch);
  CHECK(turn_belief.hit);
  CHECK(turn_belief.reason == MissReason::None);
  for (std::size_t player = 0; player < 2; ++player) {
    double sum = 0.0;
    for (double mass : *turn_belief.public_reach[player])
      sum += mass;
    CHECK(near(sum, 1.0, 1e-9));
  }
  // The {9s,9c} combo is blocked by the relabeled turn card (9h -> 9s).
  const int blocked_combo = bs::comboIndex(card("9s"), card("9c"));
  CHECK(near((*turn_belief.public_reach[0])[blocked_combo], 0.0, 1e-12));
  // The surviving seat0 combos renormalize over total 5.
  const int as_ad = bs::comboIndex(card("As"), card("Ad"));
  const int ah_ac = bs::comboIndex(card("Ah"), card("Ac"));
  CHECK(near((*turn_belief.public_reach[0])[as_ad], 2.0 / 5.0, 1e-9));
  CHECK(near((*turn_belief.public_reach[0])[ah_ac], 3.0 / 5.0, 1e-9));

  // The resolver source also serves the turn query: the canonical prefix
  // board must include the relabeled turn card (9h -> 9s) so the key matches
  // the artifact's turn rows.
  {
    auto source = residents.resolver_source(turn_state, turn_history, std::nullopt);
    CHECK(source != nullptr);
    const auto bp_row = source->row(turn_state, turn_history, 1, {card("Qh"), card("Qd")});
    CHECK(bp_row.has_value());
    CHECK(bp_row->size > 0);
  }

  return true;
}

// ---------------------------------------------------------------------------
// W2c-ii-b: three-seat belief with card collisions across seats. The exact
// inclusion-exclusion path must reproduce hand-computed marginals, and a
// four-seat artifact must miss declared while still loading and advertising.
// ---------------------------------------------------------------------------

static bool test_three_seat_belief_collision(const fs::path& dir) {
  const PublishedV2Fixture fixture = publish_v2(
      three_seat_flop_def_v2(), three_seat_collision_ranges_v2(), 200, dir, "v2collision");
  std::vector<RootLoadResult> results;
  ResidentPolicySet residents = ResidentPolicySet::build(
      {{fixture.policy_path, parse_sha256(fixture.sha256_hex)}}, {}, &results);
  CHECK(results.size() == 1);
  CHECK(results[0].status == RootStatus::Advertised);
  CHECK(residents.advertised_roots() == 1);

  const GameState state(three_seat_flop_def_v2());
  const std::vector<PublicAction> history;
  ResidentScratch scratch;
  const ResidentAnswer belief = residents.public_belief(state, history, std::nullopt, scratch);
  CHECK(belief.hit);
  CHECK(belief.reason == MissReason::None);
  CHECK(belief.public_reach_seats == 3);

  // Four card-disjoint joint deals, each weight one: every seat's first combo
  // appears in exactly one deal (marginal 1/4) and its second in three (3/4).
  const int first[3][2] = {
      {card("Kh"), card("Ah")}, {card("Qh"), card("Ah")}, {card("Qh"), card("Kh")}};
  const int second[3][2] = {
      {card("Ks"), card("As")}, {card("Qd"), card("Ad")}, {card("Qc"), card("Kc")}};
  for (std::size_t seat = 0; seat < 3; ++seat) {
    double sum = 0.0;
    for (double mass : *belief.public_reach[seat])
      sum += mass;
    CHECK(near(sum, 1.0, 1e-9));
    const int c_first = bs::comboIndex(first[seat][0], first[seat][1]);
    const int c_second = bs::comboIndex(second[seat][0], second[seat][1]);
    CHECK(near((*belief.public_reach[seat])[c_first], 0.25, 1e-9));
    CHECK(near((*belief.public_reach[seat])[c_second], 0.75, 1e-9));
  }

  // A hero decision at the root returns the seat-1 actor's row. The hero combo
  // {Qh,Ah} shares Ah with seat 0's first combo, so the hero-private opponent
  // view (seat 0, the first non-actor seat) must have that combo blocked.
  const ResidentAnswer hero =
      residents.hero_decision(state, history, {card("Qh"), card("Ah")}, std::nullopt, scratch);
  CHECK(hero.hit);
  CHECK(hero.hero_row.size > 0);
  CHECK(hero.opponent_blocked_reach != nullptr);
  // Seat 0's KhAh shares Ah with the hero: blocked. Seat 0's KsAs survives.
  CHECK(hero.opponent_blocked_reach[bs::comboIndex(card("Kh"), card("Ah"))] == 0.0);
  CHECK(hero.opponent_blocked_reach[bs::comboIndex(card("Ks"), card("As"))] > 0.0);

  return true;
}

// W2c-ii-b: the pairwise add-back in compatible_mass is load-bearing. Seat 2's
// AhQd bridges Ah (seat 0's AhKh) and Qd (seat 1's QdJd), so for the disjoint
// pair (AhKh, QdJd) it is subtracted twice through the per-card masses and
// must be added back once. A regression that deleted the pairwise loop would
// drop the joint from 5 to 4 and shift every marginal below.
static bool test_three_seat_belief_bridging(const fs::path& dir) {
  const PublishedV2Fixture fixture =
      publish_v2(three_seat_flop_def_v2(), three_seat_bridging_ranges_v2(), 200, dir, "v2bridging");
  std::vector<RootLoadResult> results;
  ResidentPolicySet residents = ResidentPolicySet::build(
      {{fixture.policy_path, parse_sha256(fixture.sha256_hex)}}, {}, &results);
  CHECK(results.size() == 1);
  CHECK(results[0].status == RootStatus::Advertised);
  CHECK(residents.advertised_roots() == 1);

  const GameState state(three_seat_flop_def_v2());
  const std::vector<PublicAction> history;
  ResidentScratch scratch;
  const ResidentAnswer belief = residents.public_belief(state, history, std::nullopt, scratch);
  CHECK(belief.hit);
  CHECK(belief.reason == MissReason::None);
  CHECK(belief.public_reach_seats == 3);

  // Five card-disjoint deals, each weight one. Seat 2's AhQd is compatible
  // only with (AsKs, QcJc); its 4s5s is compatible with every pair.
  const int first[3][2] = {
      {card("Ah"), card("Kh")}, {card("Qd"), card("Jd")}, {card("Ah"), card("Qd")}};
  const int second[3][2] = {
      {card("As"), card("Ks")}, {card("Qc"), card("Jc")}, {card("4s"), card("5s")}};
  const double expected_first[3] = {2.0 / 5.0, 2.0 / 5.0, 1.0 / 5.0};
  const double expected_second[3] = {3.0 / 5.0, 3.0 / 5.0, 4.0 / 5.0};
  for (std::size_t seat = 0; seat < 3; ++seat) {
    double sum = 0.0;
    for (double mass : *belief.public_reach[seat])
      sum += mass;
    CHECK(near(sum, 1.0, 1e-9));
    const int c_first = bs::comboIndex(first[seat][0], first[seat][1]);
    const int c_second = bs::comboIndex(second[seat][0], second[seat][1]);
    CHECK(near((*belief.public_reach[seat])[c_first], expected_first[seat], 1e-9));
    CHECK(near((*belief.public_reach[seat])[c_second], expected_second[seat], 1e-9));
  }

  return true;
}

static bool test_four_seat_declared_miss(const fs::path& dir) {
  const PublishedV2Fixture fixture =
      publish_v2(four_seat_flop_def_v2(), four_seat_flop_ranges_v2(), 200, dir, "v2four");
  std::vector<RootLoadResult> results;
  ResidentPolicySet residents = ResidentPolicySet::build(
      {{fixture.policy_path, parse_sha256(fixture.sha256_hex)}}, {}, &results);
  // The 4-seat artifact loads and advertises (root existence is exact for
  // every seat count); only the per-node belief is a declared coverage miss.
  CHECK(results.size() == 1);
  CHECK(results[0].status == RootStatus::Advertised);
  CHECK(residents.advertised_roots() == 1);

  const GameState state(four_seat_flop_def_v2());
  const std::vector<PublicAction> history;
  ResidentScratch scratch;
  const ResidentAnswer belief = residents.public_belief(state, history, std::nullopt, scratch);
  CHECK(!belief.hit);
  CHECK(belief.reason == MissReason::SeatCountNotSupported);
  // Actor is seat 1 (button 0 -> next actor); use a seat-1 combo for the hero.
  const ResidentAnswer hero =
      residents.hero_decision(state, history, {card("8s"), card("8d")}, std::nullopt, scratch);
  CHECK(!hero.hit);
  CHECK(hero.reason == MissReason::SeatCountNotSupported);

  return true;
}

// ---------------------------------------------------------------------------
// Coverage misses and startup semantics.
// ---------------------------------------------------------------------------

static bool test_misses(const fs::path& dir) {
  HeadsUpGame golden = canonicalize(golden_game());
  const PublishedFixture golden_fixture = publish_game(golden, 1, {}, dir, "miss-golden");
  HeadsUpGame matrix = canonicalize(matrix_game());
  const PublishedFixture matrix_fixture = publish_game(matrix, 4, {}, dir, "miss-matrix");

  std::vector<RootLoadResult> results;
  ResidentPolicySet residents = ResidentPolicySet::build(
      {{golden_fixture.policy_path, parse_sha256(golden_fixture.sha256_hex)},
       {matrix_fixture.policy_path, parse_sha256(matrix_fixture.sha256_hex)}},
      {}, &results);
  CHECK(results.size() == 2);
  CHECK(results[0].status == RootStatus::Advertised);
  CHECK(results[1].status == RootStatus::Advertised);

  ResidentScratch scratch;
  const HeadsUpState golden_root(golden.root);
  const HeadsUpState matrix_root(matrix.root);
  const UnifiedNode golden_root_node = to_unified(golden_root);
  const UnifiedNode matrix_root_node = to_unified(matrix_root);

  // RootNotSupported: no matrix queries against a golden-only set.
  ResidentPolicySet golden_only = ResidentPolicySet::build(
      {{golden_fixture.policy_path, parse_sha256(golden_fixture.sha256_hex)}}, {}, &results);
  CHECK(golden_only
            .public_belief(matrix_root_node.state, matrix_root_node.history, std::nullopt, scratch)
            .reason == MissReason::RootNotSupported);

  // Pinned digest mismatch.
  const ResidentAnswer mismatch =
      residents.public_belief(golden_root_node.state, golden_root_node.history,
                              std::string_view(matrix_fixture.sha256_hex), scratch);
  CHECK(!mismatch.hit);
  CHECK(mismatch.reason == MissReason::RootIdentityMismatch);

  // Pin to a supported root works.
  const ResidentAnswer pinned =
      residents.public_belief(matrix_root_node.state, matrix_root_node.history,
                              std::string_view(matrix_fixture.sha256_hex), scratch);
  CHECK(pinned.hit);

  // Unknown digest pin.
  CHECK(residents
            .public_belief(matrix_root_node.state, matrix_root_node.history,
                           std::string_view(std::string(64, 'a')), scratch)
            .reason == MissReason::RootNotSupported);

  // Wrong flop order is a different root.
  HeadsUpRoot permuted = matrix.root;
  std::swap(permuted.flop[0], permuted.flop[1]);
  const HeadsUpState permuted_state(permuted);
  const UnifiedNode permuted_node = to_unified(permuted_state);
  CHECK(residents.public_belief(permuted_node.state, permuted_node.history, std::nullopt, scratch)
            .reason == MissReason::RootNotSupported);
  CHECK(residents
            .public_belief(permuted_node.state, permuted_node.history,
                           std::string_view(matrix_fixture.sha256_hex), scratch)
            .reason == MissReason::RootIdentityMismatch);

  // Runout divergence: turn 2h instead of the fixed 3s.
  {
    HeadsUpState state =
        matrix_root.after_action(0, {ActionType::Check}).after_action(1, {ActionType::Check});
    CHECK(state.phase() == Phase::Deal);
    state = state.after_card(card("2h"));
    const UnifiedNode node = to_unified(state);
    const ResidentAnswer answer =
        residents.public_belief(node.state, node.history, std::nullopt, scratch);
    CHECK(!answer.hit);
    CHECK(answer.reason == MissReason::RunoutDivergence);
  }

  // Off-tree amount: a legal bet to 7 at the matrix root is not in the
  // abstract action set {check, 5, 8, 10}.
  {
    const HeadsUpState off = matrix_root.after_action(0, {ActionType::Bet, 7});
    const UnifiedNode off_node = to_unified(off);
    const ResidentAnswer answer =
        residents.public_belief(off_node.state, off_node.history, std::nullopt, scratch);
    CHECK(!answer.hit);
    CHECK(answer.reason == MissReason::OffTreeAmount);
  }

  // Untrained hero combination at a covered node.
  {
    const ResidentAnswer answer =
        residents.hero_decision(matrix_root_node.state, matrix_root_node.history,
                                {card("2h"), card("2d")}, std::nullopt, scratch);
    CHECK(!answer.hit);
    CHECK(answer.reason == MissReason::UntrainedCombo);
  }

  // Hero combo blocked by the public board.
  {
    const ResidentAnswer answer =
        residents.hero_decision(matrix_root_node.state, matrix_root_node.history,
                                {card("Ks"), card("As")}, std::nullopt, scratch);
    CHECK(!answer.hit);
    CHECK(answer.reason == MissReason::ComboBlockedByBoard);
  }

  // A supported root without a pinned digest is refused at construction.
  {
    std::vector<RootLoadResult> unpinned_results;
    ResidentPolicySet unpinned = ResidentPolicySet::build(
        {{matrix_fixture.policy_path, std::nullopt}}, {}, &unpinned_results);
    CHECK(unpinned_results.size() == 1);
    CHECK(unpinned_results[0].status == RootStatus::LoadFailed);
    CHECK(!unpinned_results[0].detail.empty());
    CHECK(unpinned.advertised_roots() == 0);
  }

  // One load failure must not disable another root.
  {
    std::array<std::uint8_t, 32> bad_pin{};
    std::vector<RootLoadResult> failed_results;
    ResidentPolicySet mixed = ResidentPolicySet::build(
        {{dir / "does-not-exist.db", bad_pin},
         {matrix_fixture.policy_path, parse_sha256(matrix_fixture.sha256_hex)}},
        {}, &failed_results);
    CHECK(failed_results.size() == 2);
    CHECK(failed_results[0].status == RootStatus::LoadFailed);
    CHECK(!failed_results[0].detail.empty());
    CHECK(failed_results[1].status == RootStatus::Advertised);
    CHECK(mixed.advertised_roots() == 1);
    CHECK(
        mixed.public_belief(matrix_root_node.state, matrix_root_node.history, std::nullopt, scratch)
            .hit);
  }

  // Over-budget root: loaded and measured but not advertised.
  {
    ResidentOptions options;
    options.budget_bytes = 1;
    std::vector<RootLoadResult> budget_results;
    ResidentPolicySet starved = ResidentPolicySet::build(
        {{matrix_fixture.policy_path, parse_sha256(matrix_fixture.sha256_hex)}}, options,
        &budget_results);
    CHECK(budget_results[0].status == RootStatus::OverBudget);
    CHECK(budget_results[0].resident_bytes > 1);
    CHECK(starved.advertised_roots() == 0);
    CHECK(
        starved
            .public_belief(matrix_root_node.state, matrix_root_node.history, std::nullopt, scratch)
            .reason == MissReason::OverBudgetNotAdvertised);
  }

  // Fully blocked opponent belief after conditioning: the opponent range has
  // two combos sharing the hero's Kc and one combo (QdJd) that does not. The
  // non-sharing combo is the only compatible deal that creates the hero row;
  // forcing it to never bet while the Kc combos bet leaves the opponent
  // belief after the observed bet containing only Kc combos, which the
  // hero-private blocker filter then removes entirely.
  {
    HeadsUpGame blocked_game;
    blocked_game.root = golden.root;
    blocked_game.ranges[0] = {{{card("As"), card("Ks")}, 1},
                              {{card("Ac"), card("Ad")}, 1},
                              {{card("Kc"), card("Qc")}, 1}};
    blocked_game.ranges[1] = {{{card("Ac"), card("Kc")}, 1},
                              {{card("Ad"), card("Kc")}, 1},
                              {{card("Qd"), card("Jd")}, 1}};
    blocked_game.fixed_runout = golden.fixed_runout;
    blocked_game = canonicalize(std::move(blocked_game));

    const HeadsUpState blocked_root(blocked_game.root);
    const HeadsUpState blocked_after_check = blocked_root.after_action(0, {ActionType::Check});
    Overrides blocked_overrides;
    blocked_overrides.emplace(key_of(blocked_after_check, {card("Ac"), card("Kc")}),
                              std::vector<double>{0.5, 0.25, 0.25});
    blocked_overrides.emplace(key_of(blocked_after_check, {card("Ad"), card("Kc")}),
                              std::vector<double>{0.5, 0.25, 0.25});
    blocked_overrides.emplace(key_of(blocked_after_check, {card("Jd"), card("Qd")}),
                              std::vector<double>{1.0, 0.0, 0.0});
    const PublishedFixture blocked_fixture =
        publish_game(blocked_game, 1, blocked_overrides, dir, "fully-blocked");
    ResidentPolicySet blocked_set = ResidentPolicySet::build(
        {{blocked_fixture.policy_path, parse_sha256(blocked_fixture.sha256_hex)}}, {}, &results);
    const HeadsUpState blocked_after_bet =
        blocked_after_check.after_action(1, {ActionType::Bet, 1});
    const UnifiedNode blocked_after_bet_node = to_unified(blocked_after_bet);
    const ResidentAnswer blocked_answer =
        blocked_set.hero_decision(blocked_after_bet_node.state, blocked_after_bet_node.history,
                                  {card("Kc"), card("Qc")}, std::nullopt, scratch);
    CHECK(!blocked_answer.hit);
    CHECK(blocked_answer.reason == MissReason::OpponentRangeFullyBlocked);
    // Public belief itself remains well defined, independent of hero, and the
    // non-sharing combo has been conditioned to zero.
    const ResidentAnswer blocked_public = blocked_set.public_belief(
        blocked_after_bet_node.state, blocked_after_bet_node.history, std::nullopt, scratch);
    CHECK(blocked_public.hit);
    CHECK(near((*blocked_public.public_reach[1])[bs::comboIndex(card("Jd"), card("Qd"))], 0.0));
  }

  // Truncated policy: check is on the retained root rows, but the seat-1
  // node after the check was never published -> missing history.
  {
    const PublishedFixture partial = publish_root_only(golden, dir, "root-only");
    std::vector<RootLoadResult> partial_results;
    ResidentPolicySet partial_set = ResidentPolicySet::build(
        {{partial.policy_path, parse_sha256(partial.sha256_hex)}}, {}, &partial_results);
    CHECK(partial_results[0].status == RootStatus::Advertised);
    const HeadsUpState after_check = golden_root.after_action(0, {ActionType::Check});
    const HeadsUpState after_check_bet = after_check.after_action(1, {ActionType::Bet, 1});
    const UnifiedNode after_check_node = to_unified(after_check);
    const UnifiedNode after_check_bet_node = to_unified(after_check_bet);
    CHECK(
        partial_set
            .public_belief(after_check_node.state, after_check_node.history, std::nullopt, scratch)
            .hit);
    const ResidentAnswer missing = partial_set.public_belief(
        after_check_bet_node.state, after_check_bet_node.history, std::nullopt, scratch);
    CHECK(!missing.hit);
    CHECK(missing.reason == MissReason::MissingHistory);
  }

  // Listing the same root twice reports a duplicate and keeps one advertised.
  {
    std::vector<RootLoadResult> duplicate_results;
    ResidentPolicySet duplicated = ResidentPolicySet::build(
        {{matrix_fixture.policy_path, parse_sha256(matrix_fixture.sha256_hex)},
         {matrix_fixture.policy_path, parse_sha256(matrix_fixture.sha256_hex)}},
        {}, &duplicate_results);
    CHECK(duplicate_results[0].status == RootStatus::Advertised);
    CHECK(duplicate_results[1].status == RootStatus::DuplicateRoot);
    CHECK(duplicated.advertised_roots() == 1);
  }
  return true;
}

// ---------------------------------------------------------------------------
// Compact index: probability normalization, brute-force agreement, and
// complete continuity of the supported published trees.
// ---------------------------------------------------------------------------

namespace {

bool index_matches_map(const ResidentIndex& index,
                       const std::map<InformationKey, PolicyRow>& rows) {
  for (const auto& [key, row] : rows) {
    CompactRowView view;
    CHECK(index.find(key, view));
    CHECK(view.count == row.actions.size());
    double sum = 0;
    for (std::size_t i = 0; i < view.count; ++i) {
      CHECK(view.actions[i] == row.actions[i]);
      CHECK(near(view.probabilities[i], row.probabilities[i]));
      sum += view.probabilities[i];
    }
    CHECK(near(sum, 1.0));
  }
  // Deterministic random probes, including keys that are absent.
  std::mt19937_64 rng(0x5e6d1234ULL);
  std::vector<InformationKey> present;
  for (const auto& [key, row] : rows)
    present.push_back(key);
  for (int trial = 0; trial < 5000; ++trial) {
    InformationKey candidate = present[rng() % present.size()];
    candidate[1 + rng() % 3] += 1 + static_cast<std::uint64_t>(rng() % 7);
    CompactRowView view;
    const bool index_hit = index.find(candidate, view);
    const bool map_hit = rows.contains(candidate);
    CHECK(index_hit == map_hit);
  }
  return true;
}

struct ContinuityWalker {
  const HeadsUpGame& game;
  const ResidentIndex& index;
  std::set<std::string> seen;
  bool ok = true;

  std::string public_name(const HeadsUpState& state) {
    for (const auto& hand : game.ranges[*state.actor()]) {
      bool blocked = false;
      for (int board_card : state.board())
        if (board_card == hand.cards[0] || board_card == hand.cards[1])
          blocked = true;
      if (!blocked)
        return encode_public_key(information_key(state, hand.cards), kArtifactSchemaVersion);
    }
    std::abort();
  }

  bool check_node(const HeadsUpState& state) {
    if (state.phase() == Phase::Folded || state.phase() == Phase::Showdown)
      return true;
    if (state.phase() == Phase::Deal) {
      const std::size_t slot = state.board().size() - 3;
      std::vector<int> cards;
      if (game.fixed_runout[slot]) {
        cards.push_back(*game.fixed_runout[slot]);
      } else {
        std::array<bool, 52> used{};
        for (int card_on_board : state.board())
          used[card_on_board] = true;
        for (auto fixed : game.fixed_runout)
          if (fixed)
            used[*fixed] = true;
        for (int c = 0; c < 52; ++c)
          if (!used[c])
            cards.push_back(c);
      }
      for (int dealt : cards)
        if (!walk(state.after_card(dealt)))
          return false;
      return true;
    }
    const auto actions = abstract_actions(state, game.sizes);
    const std::size_t actor = *state.actor();
    for (const WeightedHand& hand : game.ranges[actor]) {
      bool blocked = false;
      for (int board_card : state.board())
        if (board_card == hand.cards[0] || board_card == hand.cards[1])
          blocked = true;
      if (blocked)
        continue;
      // A free-chance orphan combo (no compatible opponent deal on this
      // branch) never receives a row in the complete training tree.
      if (!has_static_partner(game, actor, hand.cards, state.board()))
        continue;
      CompactRowView view;
      if (!index.find(information_key(state, hand.cards), view)) {
        ok = false;
        return false;
      }
      if (static_cast<std::size_t>(view.count) != actions.size()) {
        ok = false;
        return false;
      }
      for (std::size_t i = 0; i < actions.size(); ++i)
        if (!(view.actions[i] == actions[i])) {
          ok = false;
          return false;
        }
    }
    if (!seen.insert(public_name(state)).second)
      return true;
    for (const Action& action : actions)
      if (!walk(state.after_action(actor, action)))
        return false;
    return true;
  }

  bool walk(const HeadsUpState& state) {
    if (!ok)
      return false;
    return check_node(state);
  }
};

}  // namespace

static bool test_index_and_continuity(const fs::path& dir) {
  struct Case {
    HeadsUpGame (*make)();
    const char* name;
  };
  const HeadsUpGame golden = canonicalize(golden_game());
  const HeadsUpGame free_turn = canonicalize(free_turn_game());
  const HeadsUpGame matrix = canonicalize(matrix_game());

  const PublishedFixture golden_fixture = publish_game(golden, 1, {}, dir, "cont-golden");
  const PublishedFixture free_fixture = publish_game(free_turn, 1, {}, dir, "cont-free");
  const PublishedFixture matrix_fixture = publish_game(matrix, 4, {}, dir, "cont-matrix");

  for (const PublishedFixture& fixture : {golden_fixture, free_fixture, matrix_fixture}) {
    const LoadedArtifact loaded = load_artifact(fixture.policy_path);
    const HeadsUpPolicy& policy = loaded.bundle.result.policy;
    ResidentIndex index;
    index.build(policy.rows());
    CHECK(index.row_count() == policy.rows().size());
    CHECK(index_matches_map(index, policy.rows()));

    ContinuityWalker walker{policy.game(), index, {}, true};
    CHECK(walker.walk(HeadsUpState(policy.game().root)));
  }
  return true;
}

// ---------------------------------------------------------------------------
// Empty bounds: v1 artifacts are advertised for blueprint lookup only and the
// public header exposes no bound, guarantee, or certification symbol.
// ---------------------------------------------------------------------------

static bool test_blueprint_only() {
#ifdef BS_RESIDENT_HEADER_PATH
  std::ifstream header(BS_RESIDENT_HEADER_PATH);
  CHECK(header.good());
  std::string contents((std::istreambuf_iterator<char>(header)), std::istreambuf_iterator<char>());
  // Strip comments: deferral documentation mentions certification, but the
  // compiled surface must expose no guarantee or certification symbol.
  std::string code;
  code.reserve(contents.size());
  for (std::size_t i = 0; i < contents.size();) {
    if (i + 1 < contents.size() && contents[i] == '/' && contents[i + 1] == '/') {
      while (i < contents.size() && contents[i] != '\n')
        ++i;
    } else if (i + 1 < contents.size() && contents[i] == '/' && contents[i + 1] == '*') {
      i += 2;
      while (i + 1 < contents.size() && !(contents[i] == '*' && contents[i + 1] == '/'))
        ++i;
      i += 2;
    } else {
      code.push_back(contents[i++]);
    }
  }
  for (const char* forbidden : {"guarantee", "Guarantee", "certif", "Certif", "bound", "Bound"})
    CHECK(code.find(forbidden) == std::string::npos);
#endif
  return true;
}

int main() {
  const fs::path dir = fs::temp_directory_path() / "bs-resident-test";
  std::error_code ec;
  fs::remove_all(dir, ec);
  fs::create_directories(dir, ec);

  struct Case {
    const char* name;
    std::function<bool(const fs::path&)> run;
  };
  const std::vector<Case> cases = {
      {"golden reach", test_golden_reach},
      {"chance filter", test_chance_filter},
      {"solver reach convention", test_solver_reach_convention},
      {"street conditioning", test_street_conditioning},
      {"orphan free chance", test_orphan_free_chance},
      {"orphan actor one", test_orphan_actor_one},
      {"empty joint artifacts", test_empty_joint_artifacts},
      {"free slot reserved card", test_free_slot_reserved_card},
      {"shared combo ranges", test_shared_combo_ranges},
      {"probe api", test_probe_api},
      {"v2 resident projection", test_v2_resident_projection},
      {"v3 resident projection", test_v3_resident_projection},
      {"three seat belief collision", test_three_seat_belief_collision},
      {"three seat belief bridging", test_three_seat_belief_bridging},
      {"four seat declared miss", test_four_seat_declared_miss},
      {"cross-blocked root", test_cross_blocked_root},
      {"coverage misses", test_misses},
      {"index and continuity", test_index_and_continuity},
      {"blueprint only", [](const fs::path&) { return test_blueprint_only(); }},
  };

  for (const Case& test_case : cases) {
    const fs::path case_dir = dir / test_case.name;
    fs::create_directories(case_dir, ec);
    if (!test_case.run(case_dir))
      return 1;
    std::printf("[resident] %s passed\n", test_case.name);
  }
  std::printf("resident: all tests passed\n");
  return 0;
}
