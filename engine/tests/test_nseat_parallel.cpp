// RFC 0015: native parallel-trainer gates. These known-deal river fixtures
// measure legal deviations only in their declared singleton-range games.
// Fixed-width repeatability is checked; general equilibrium convergence and
// identical policies across different batch widths are not certified.
#include <algorithm>
#include <array>
#include <bit>
#include <bs/abstract_tree.hpp>
#include <bs/abstraction.hpp>
#include <bs/eval.hpp>
#include <bs/frontier.hpp>
#include <bs/game_definition.hpp>
#include <bs/nseat_trainer.hpp>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#define CHECK(condition)                                                                  \
  do {                                                                                    \
    if (!(condition)) {                                                                   \
      std::fprintf(stderr, "CHECK failed: %s (%s:%d)\n", #condition, __FILE__, __LINE__); \
      return 1;                                                                           \
    }                                                                                     \
  } while (false)

namespace {

using namespace bs::poker;
using namespace bs::solver;
using namespace bs::tree;

using Ranges = std::vector<std::vector<WeightedHand>>;
using Hands = std::vector<std::array<int, 2>>;

int card(const char* name) {
  return bs::cardId(std::string(name));
}

std::array<int, 2> hand(const char* first, const char* second) {
  std::array<int, 2> result{card(first), card(second)};
  std::sort(result.begin(), result.end());
  return result;
}

GameDef river(std::size_t seats = 2) {
  GameDef def;
  def.player_count = seats;
  def.button = seats == 2 ? 1 : 0;
  def.big_blind = seats == 2 ? 1 : 2;
  for (std::size_t seat = 0; seat < seats; ++seat) {
    def.stacks[seat] = 1;
    def.contributions[seat] = 1;
  }
  def.pot = static_cast<Chips>(seats);
  def.board = {card("2c"), card("3d"), card("7h"), card("Ks"), card("9c")};
  def.board_size = 5;
  return def;
}

Ranges ranges(std::size_t seats = 2) {
  Ranges result{{{hand("Ac", "Ah"), 1.0}}, {{hand("Kc", "Kd"), 1.0}}};
  if (seats == 3)
    result.push_back({{hand("Qc", "Qd"), 1.0}});
  return result;
}

Hands known_hands(const Ranges& ranges) {
  Hands hands;
  for (const auto& range : ranges) {
    if (range.size() != 1)
      throw std::runtime_error("exact fixture response requires singleton ranges");
    hands.push_back(range.front().cards);
  }
  return hands;
}

NSeatTrainerLimits limits() {
  NSeatTrainerLimits result;
  result.wall = std::chrono::seconds(10);
  return result;
}

template <class Error, class F>
bool throws_type(F&& run) {
  try {
    run();
  } catch (const Error&) {
    return true;
  } catch (...) {
    return false;
  }
  return false;
}

bool same_result(const NSeatTrainingResult& a, const NSeatTrainingResult& b) {
  if (a.termination != b.termination || a.completed_iterations != b.completed_iterations ||
      a.nodes != b.nodes || a.visits != b.visits || a.information_sets != b.information_sets ||
      a.batch_size != b.batch_size || a.completed_epochs != b.completed_epochs ||
      a.accounted_bytes != b.accounted_bytes || a.seed != b.seed || a.prng_state != b.prng_state ||
      a.algorithm_revision != b.algorithm_revision ||
      !same_game_def(a.policy.game(), b.policy.game()) ||
      a.policy.action_id() != b.policy.action_id() ||
      a.policy.rows().size() != b.policy.rows().size() || a.raw_rows.size() != b.raw_rows.size())
    return false;
  for (const auto& [key, row] : a.policy.rows()) {
    const auto found = b.policy.rows().find(key);
    if (found == b.policy.rows().end() || row.actions != found->second.actions ||
        row.probabilities != found->second.probabilities || row.visits != found->second.visits)
      return false;
  }
  for (const auto& [key, row] : a.raw_rows) {
    const auto found = b.raw_rows.find(key);
    if (found == b.raw_rows.end() || row.actions != found->second.actions ||
        row.regrets != found->second.regrets || row.sums != found->second.sums ||
        row.visits != found->second.visits)
      return false;
  }
  return true;
}

bool valid_result(const AbstractTree& tree, const NSeatTrainingResult& result,
                  std::uint64_t iterations, std::uint64_t seed) {
  if (result.termination != NSeatTerminationPhase::Complete ||
      result.completed_iterations != iterations || result.seed != seed || result.batch_size == 0 ||
      result.completed_epochs !=
          iterations / result.batch_size + (iterations % result.batch_size != 0) ||
      result.nodes != tree.size() || result.information_sets != result.policy.rows().size() ||
      result.information_sets != result.raw_rows.size() ||
      !same_game_def(result.policy.game(), tree.def()) ||
      result.policy.action_id() != tree.action_id())
    return false;
  std::size_t own_visits = 0;
  for (const auto& [key, row] : result.policy.rows()) {
    if (key.node_index >= tree.size())
      return false;
    const auto& node = tree.node(key.node_index);
    const auto raw = result.raw_rows.find(key);
    if (!node.is_action() || node.actions != row.actions ||
        row.probabilities.size() != row.actions.size() || raw == result.raw_rows.end() ||
        raw->second.actions != row.actions || raw->second.regrets.size() != row.actions.size() ||
        raw->second.sums.size() != row.actions.size() || raw->second.visits != row.visits)
      return false;
    double total = 0.0;
    for (std::size_t i = 0; i < row.actions.size(); ++i) {
      const double probability = row.probabilities[i];
      if (!std::isfinite(probability) || probability < 0.0 ||
          !std::isfinite(raw->second.regrets[i]) || raw->second.regrets[i] < 0.0 ||
          !std::isfinite(raw->second.sums[i]) || raw->second.sums[i] < 0.0)
        return false;
      total += probability;
    }
    if (std::abs(total - 1.0) > 1e-12)
      return false;
    own_visits += row.visits;
  }
  if (own_visits > result.visits)
    return false;
  bs::SplitMix64 witness(
      derive_nseat_stream(seed, NSeatStreamPurpose::JointDeal, 0x5749544e45535300ULL, 0)
          .next_u64());
  for (std::uint64_t i = 0; i < iterations; ++i)
    witness.next_u64();
  return result.prng_state == witness.next_u64();
}

// RFC 0017 pins the complete pre-optimization result, not just two runs of
// the same new implementation. Encode fields explicitly, never object padding,
// and retain every floating-point bit (including a possible signed zero).
std::uint64_t result_fingerprint(const NSeatTrainingResult& result) {
  std::uint64_t hash = 0xcbf29ce484222325ULL;
  const auto word = [&hash](std::uint64_t value) {
    for (unsigned byte = 0; byte < 8; ++byte) {
      hash ^= (value >> (8 * byte)) & 0xff;
      hash *= 0x100000001b3ULL;
    }
  };
  const auto text = [&word](const std::string& value) {
    word(value.size());
    for (unsigned char byte : value)
      word(byte);
  };
  const auto actions = [&word](const std::vector<Action>& menu) {
    word(menu.size());
    for (const auto& action : menu) {
      word(static_cast<std::uint64_t>(action.type));
      word(static_cast<std::uint64_t>(action.target_total));
    }
  };
  const auto doubles = [&word](const std::vector<double>& values) {
    word(values.size());
    for (double value : values)
      word(std::bit_cast<std::uint64_t>(value));
  };
  word(static_cast<std::uint64_t>(result.termination));
  word(result.completed_iterations);
  word(result.batch_size);
  word(result.completed_epochs);
  word(result.nodes);
  word(result.visits);
  word(result.information_sets);
  word(result.accounted_bytes);
  word(result.seed);
  word(result.prng_state);
  word(result.algorithm_revision);
  const auto& game = result.policy.game();
  word(game.player_count);
  word(game.button);
  word(static_cast<std::uint64_t>(game.big_blind));
  word(static_cast<std::uint64_t>(game.ante));
  word(static_cast<std::uint64_t>(game.pot));
  word(game.board_size);
  for (std::size_t seat = 0; seat < kMaxUnifiedSeats; ++seat) {
    word(static_cast<std::uint64_t>(game.stacks[seat]));
    word(static_cast<std::uint64_t>(game.contributions[seat]));
    word(static_cast<std::uint64_t>(game.blinds_posted[seat]));
    word(static_cast<std::uint64_t>(game.street_committed[seat]));
    word(game.folded[seat]);
    word(game.raise_rights[seat]);
  }
  for (int card : game.board)
    word(static_cast<std::uint64_t>(card));
  word(static_cast<std::uint64_t>(game.last_full_raise));
  word(game.preflop);
  word(static_cast<std::uint64_t>(game.variant));
  word(static_cast<std::uint64_t>(game.terminal));
  const auto& action_id = result.policy.action_id();
  text(action_id.name);
  word(action_id.version);
  text(action_id.parameters);
  word(action_id.digest);
  word(result.raw_rows.size());
  for (const auto& [key, row] : result.raw_rows) {
    word(key.own_card_bucket);
    word(key.node_index);
    actions(row.actions);
    doubles(row.regrets);
    doubles(row.sums);
    word(row.visits);
  }
  word(result.policy.rows().size());
  for (const auto& [key, row] : result.policy.rows()) {
    word(key.own_card_bucket);
    word(key.node_index);
    actions(row.actions);
    doubles(row.probabilities);
    word(row.visits);
  }
  return hash;
}

int allocation_optimization_fingerprints() {
  // Captured through the native release test before any RFC 0017 production
  // optimization, ordered by board, seed, then width. These are not hashes
  // regenerated from the optimized implementation.
  constexpr std::array<std::uint64_t, 12> baseline{
      0xe5f7e3b4837a0853ULL, 0x645fc20990a8b459ULL, 0xcb1c05869ecbeaeeULL, 0xc04eb3fda164db10ULL,
      0xb9e9455e86ee067bULL, 0x41450705a546333dULL, 0x5dde9d48f5926fcbULL, 0x7089e2755d2ab165ULL,
      0x7f5d94ee1d59ed6bULL, 0xfd24ac5d2d29a82aULL, 0x86811d3a1933c8d2ULL, 0x158ac9fc5a7b5979ULL};
  std::size_t fixture = 0;
  const Ranges weighted{{{hand("Ac", "Ah"), 1.0}, {hand("Qh", "Qs"), 3.0}},
                        {{hand("Kc", "Kd"), 5.0}, {hand("Jc", "Jd"), 2.0}}};
  for (bool chance : {false, true}) {
    auto def = river();
    if (chance) {
      def.board_size = 4;
      def.board[4] = 0;
    }
    const AbstractTree tree(def, bs::abstraction::ActionAbstraction::identity());
    for (std::uint64_t seed : {17ULL, 43ULL}) {
      for (std::size_t width : {std::size_t{1}, std::size_t{2}, std::size_t{4}}) {
        // Eleven samples covers complete epochs and partial tails at widths
        // two and four, with nonuniform joint deals and turn-to-river chance.
        const auto result =
            train_nseat_parallel(tree, weighted, 11, seed, limits(), nullptr, width);
        CHECK(valid_result(tree, result, 11, seed));
        const auto fingerprint = result_fingerprint(result);
        std::printf("  allocation baseline chance=%d seed=%llu width=%zu fingerprint=%016llx\n",
                    chance, static_cast<unsigned long long>(seed), width,
                    static_cast<unsigned long long>(fingerprint));
        CHECK(fingerprint == baseline[fixture++]);
      }
    }
  }
  CHECK(fixture == baseline.size());
  return 0;
}

// This evaluates the entire native public game, not the trainer's sampled
// walk. Every player knows every hand from the declared singleton ranges, so
// taking a responder maximum here never grants hidden information.
double exact_value(const AbstractTree& tree, const GameState& state, std::size_t index,
                   const Hands& hands, const NSeatPolicy& policy, std::size_t responder,
                   bool respond) {
  const auto& node = tree.node(index);
  if (node.kind == NodeKind::TerminalFold)
    return static_cast<double>(state.settle_fold().chip_utility[responder]);
  if (node.kind == NodeKind::TerminalShowdown) {
    Hands live;
    for (std::size_t seat : state.live_players())
      live.push_back(hands[seat]);
    return static_cast<double>(state.settle_showdown(live).chip_utility[responder]);
  }
  if (!node.is_action() || !state.actor())
    throw std::runtime_error("river reference unexpectedly encountered chance");
  const auto actor = *state.actor();
  const auto bucket = static_cast<std::uint32_t>(bs::abstraction::card_bucket(
      bs::abstraction::CardBucketKind::CategoryTiersV1, hands[actor], state.board()));
  const auto found = policy.rows().find({bucket, static_cast<std::uint64_t>(index)});
  if (found == policy.rows().end() || found->second.actions != node.actions)
    throw std::runtime_error("river reference encountered an uncovered or mismatched policy row");
  double value = actor == responder && respond ? -std::numeric_limits<double>::infinity() : 0.0;
  for (std::size_t action = 0; action < node.actions.size(); ++action) {
    const double child = exact_value(tree, state.after_action(actor, node.actions[action]),
                                     node.children[action], hands, policy, responder, respond);
    if (actor == responder && respond)
      value = std::max(value, child);
    else
      value += found->second.probabilities[action] * child;
  }
  return value;
}

double exact_gap(const AbstractTree& tree, const Ranges& ranges, const NSeatPolicy& policy,
                 const std::string& context) {
  const auto hands = known_hands(ranges);
  double gap = 0.0;
  double zero_sum = 0.0;
  for (std::size_t seat = 0; seat < tree.def().player_count; ++seat) {
    const auto root = GameState(tree.def());
    const double profile = exact_value(tree, root, tree.root_index(), hands, policy, seat, false);
    const double response = exact_value(tree, root, tree.root_index(), hands, policy, seat, true);
    std::fprintf(stderr, "  %s seat=%zu profile=%+.9f response=%+.9f gap=%.9f\n", context.c_str(),
                 seat, profile, response, response - profile);
    if (!std::isfinite(profile) || !std::isfinite(response) || response + 1e-12 < profile)
      throw std::runtime_error("invalid exact legal response value");
    zero_sum += profile;
    gap += response - profile;
  }
  if (std::abs(zero_sum) > 1e-10)
    throw std::runtime_error("native profile values failed chip conservation");
  return gap;
}

void dump_rows(const AbstractTree& tree, const NSeatTrainingResult& result) {
  for (const auto& [key, row] : result.policy.rows()) {
    const auto& raw = result.raw_rows.at(key);
    std::fprintf(stderr, "    node=%llu actor=%zu own_visits=%llu actions(type,p,sum,regret):",
                 static_cast<unsigned long long>(key.node_index), tree.node(key.node_index).actor,
                 static_cast<unsigned long long>(row.visits));
    for (std::size_t a = 0; a < row.actions.size(); ++a)
      std::fprintf(stderr, " (%d,%.9f,%.9f,%.9f)", static_cast<int>(row.actions[a].type),
                   row.probabilities[a], raw.sums[a], raw.regrets[a]);
    std::fprintf(stderr, "\n");
  }
}

int one_worker_and_clamping() {
  for (std::size_t seats : {std::size_t{2}, std::size_t{3}}) {
    const AbstractTree tree(river(seats), bs::abstraction::ActionAbstraction::identity());
    const auto known = ranges(seats);
    for (const std::uint64_t seed : {1ULL, 17ULL, 43ULL}) {
      const auto serial = train_nseat(tree, known, 64, seed, limits());
      const auto single = train_nseat_parallel(tree, known, 64, seed, limits(), nullptr, 1);
      CHECK(same_result(serial, single));
      CHECK(valid_result(tree, single, 64, seed));
    }
    const auto serial = train_nseat(tree, known, 16, 42, limits());
    const auto zero = train_nseat_parallel(tree, known, 16, 42, limits(), nullptr, 0);
    CHECK(same_result(serial, zero));
    const auto one = train_nseat(tree, known, 1, 42, limits());
    const auto excessive = train_nseat_parallel(tree, known, 1, 42, limits(), nullptr, 128);
    CHECK(same_result(one, excessive));
  }
  return 0;
}

int zero_opponent_reach_still_averages_response() {
  const AbstractTree tree(river(), bs::abstraction::ActionAbstraction::identity());
  const auto known = ranges();
  const auto hands = known_hands(known);
  NSeatInitialState initial;
  initial.completed_iterations = 1;
  // Freeze a genuine equilibrium current profile: the known losing hand
  // checks or folds; the known winner checks or calls. The winner's response
  // to a root bet has a stale historical 50:50 average, despite current Call.
  for (const auto& node : tree.nodes()) {
    if (!node.is_action())
      continue;
    std::size_t chosen = node.actions.size();
    for (std::size_t a = 0; a < node.actions.size(); ++a) {
      const auto type = node.actions[a].type;
      if (type == ActionType::Check || (node.actor == 0 && type == ActionType::Fold) ||
          (node.actor == 1 && type == ActionType::Call)) {
        chosen = a;
        break;
      }
    }
    CHECK(chosen < node.actions.size());
    NSeatRawRow raw;
    raw.actions = node.actions;
    raw.regrets.assign(node.actions.size(), 0.0);
    raw.sums.assign(node.actions.size(), 0.0);
    raw.regrets[chosen] = 1.0;
    raw.sums[chosen] = 1.0;
    raw.visits = 10;
    const auto bucket = static_cast<std::uint32_t>(bs::abstraction::card_bucket(
        kNSeatCardKind, hands[node.actor], GameState(tree.def()).board()));
    initial.rows.emplace(NSeatInformationKey{bucket, static_cast<std::uint64_t>(node.index)},
                         std::move(raw));
  }
  const auto& root = tree.node(tree.root_index());
  std::size_t bet = root.actions.size();
  for (std::size_t a = 0; a < root.actions.size(); ++a)
    if (root.actions[a].type == ActionType::Bet)
      bet = a;
  CHECK(bet < root.actions.size());
  const auto response_node = root.children[bet];
  const auto bucket = static_cast<std::uint32_t>(
      bs::abstraction::card_bucket(kNSeatCardKind, hands[1], GameState(tree.def()).board()));
  const NSeatInformationKey response_key{bucket, static_cast<std::uint64_t>(response_node)};
  auto& stale = initial.rows.at(response_key);
  CHECK(stale.actions.size() == 2);
  std::fill(stale.sums.begin(), stale.sums.end(), 0.5);
  std::size_t call = 0;
  while (call < stale.actions.size() && stale.actions[call].type != ActionType::Call)
    ++call;
  CHECK(call < stale.actions.size());
  const auto result = train_nseat(tree, known, 16, 42, limits(), nullptr, &initial);
  const auto& repaired = result.raw_rows.at(response_key);
  // No regret sweep reaches this response: the opponent assigns exactly zero
  // probability to Bet. Averaging must nevertheless count this player's own
  // reach of one on each of the sixteen independent full-action passes.
  CHECK(repaired.visits == 10);
  CHECK(repaired.regrets == stale.regrets);
  CHECK(std::abs(repaired.sums[call] - 16.5) < 1e-12);
  CHECK(std::abs(repaired.sums[1 - call] - 0.5) < 1e-12);
  CHECK(std::abs(result.policy.rows().at(response_key).probabilities[call] - 16.5 / 17.0) < 1e-12);
  return 0;
}

int independent_average_weights() {
  const AbstractTree tree(river(), bs::abstraction::ActionAbstraction::identity());
  const auto known = ranges();
  const auto hands = known_hands(known);
  NSeatInitialState initial;
  initial.completed_iterations = 1;
  for (const auto& node : tree.nodes()) {
    if (!node.is_action())
      continue;
    CHECK(node.actions.size() == 2);
    NSeatRawRow row;
    row.actions = node.actions;
    row.sums.assign(2, 0.0);
    // These are deliberately non-equilibrium mixtures. Averaging must use
    // the frozen input, not the profile after this epoch's regret update.
    if (node.index == 0)
      row.regrets = {0.4, 0.6};
    else if (node.actor == 0)
      row.regrets = {0.25, 0.75};
    else if (node.actions[0].type == ActionType::Check)
      row.regrets = {0.7, 0.3};
    else
      row.regrets = {0.1, 0.9};
    const auto bucket = static_cast<std::uint32_t>(bs::abstraction::card_bucket(
        kNSeatCardKind, hands[node.actor], GameState(tree.def()).board()));
    initial.rows.emplace(NSeatInformationKey{bucket, static_cast<std::uint64_t>(node.index)}, row);
  }
  const auto result = train_nseat(tree, known, 1, 42, limits(), nullptr, &initial);
  CHECK(result.completed_iterations == 2 && result.completed_epochs == 2);
  for (const auto& [key, frozen] : initial.rows) {
    const auto& node = tree.node(key.node_index);
    const auto& actual = result.raw_rows.at(key);
    // The loser acts again only after its own root Check: reach is .4.
    // The winner's own reach is one in both opponent Check/Bet branches.
    const double own_reach = node.actor == 0 && node.index != 0 ? 0.4 : 1.0;
    for (std::size_t a = 0; a < frozen.actions.size(); ++a)
      CHECK(std::abs(actual.sums[a] - own_reach * frozen.regrets[a]) < 1e-12);
  }
  return 0;
}

int frozen_batch_and_one_clipping_step() {
  const AbstractTree tree(river(), bs::abstraction::ActionAbstraction::identity());
  const auto known = ranges();
  const auto& root = tree.node(0);
  CHECK(root.actions.size() == 2 && root.actions[0].type == ActionType::Check &&
        root.actions[1].type == ActionType::Bet);
  // Under an empty frozen profile every menu is uniform. The fixed known
  // loser receives -1 from check/check, -1.5 from check/bet followed by its
  // uniform Fold/Call, +1 from bet/fold, and -2 from bet/call.
  const auto delta = [](std::uint64_t seed, std::uint64_t sample) {
    auto rng = derive_nseat_stream(seed, NSeatStreamPurpose::OpponentAction, sample, 0);
    const double first = static_cast<double>(rng.next_u64() >> 11) * 0x1.0p-53;
    const double second = static_cast<double>(rng.next_u64() >> 11) * 0x1.0p-53;
    const double check = first < 0.5 ? -1.0 : -1.5;
    const double bet = second < 0.5 ? 1.0 : -2.0;
    return 0.5 * (check - bet);
  };
  std::uint64_t seed = 0;
  while (seed < 100 && delta(seed, 0) * delta(seed, 1) >= 0.0)
    ++seed;
  CHECK(seed < 100);
  const double combined = (delta(seed, 0) + delta(seed, 1)) / 2.0;
  const auto result = train_nseat_parallel(tree, known, 2, seed, limits(), nullptr, 2);
  const auto hands = known_hands(known);
  const auto bucket = static_cast<std::uint32_t>(
      bs::abstraction::card_bucket(kNSeatCardKind, hands[0], GameState(tree.def()).board()));
  const NSeatInformationKey key{bucket, 0};
  const auto& raw = result.raw_rows.at(key);
  CHECK(std::abs(raw.regrets[0] - std::max(0.0, combined)) < 1e-12);
  CHECK(std::abs(raw.regrets[1] - std::max(0.0, -combined)) < 1e-12);
  CHECK(raw.visits == 2);
  CHECK(raw.sums == std::vector<double>({0.5, 0.5}));
  CHECK(result.policy.rows().at(key).probabilities == std::vector<double>({0.5, 0.5}));
  CHECK(result.batch_size == 2 && result.completed_epochs == 1);
  const auto tail = train_nseat_parallel(tree, known, 3, seed, limits(), nullptr, 2);
  CHECK(tail.batch_size == 2 && tail.completed_epochs == 2 && tail.completed_iterations == 3);
  const double positive = raw.regrets[0] + raw.regrets[1];
  CHECK(positive > 0.0);
  for (std::size_t a = 0; a < 2; ++a) {
    const double expected = raw.sums[a] + raw.regrets[a] / positive;
    // The one-sample tail is an equal-weight epoch: divide by its actual one
    // sample, never by configured width two or by the global sample count.
    CHECK(std::abs(tail.raw_rows.at(key).sums[a] - expected) < 1e-12);
  }
  return 0;
}

int multiworker_quality() {
  constexpr std::uint64_t iterations = 2000;
  std::size_t quality_failures = 0;
  for (std::size_t seats : {std::size_t{2}, std::size_t{3}}) {
    const AbstractTree tree(river(seats), bs::abstraction::ActionAbstraction::identity());
    const auto known = ranges(seats);
    double lowest = std::numeric_limits<double>::infinity();
    double highest = 0.0;
    for (const std::uint64_t seed : {1ULL, 17ULL, 43ULL}) {
      const auto serial = train_nseat(tree, known, iterations, seed, limits());
      const auto serial_context =
          "seats=" + std::to_string(seats) + " seed=" + std::to_string(seed) + " workers=1";
      const double serial_gap = exact_gap(tree, known, serial.policy, serial_context);
      if (serial_gap > 0.1 * static_cast<double>(tree.def().pot)) {
        dump_rows(tree, serial);
        ++quality_failures;
      }
      for (const std::size_t workers : {std::size_t{2}, std::size_t{4}}) {
        std::optional<NSeatTrainingResult> previous;
        for (int repeat = 0; repeat < 3; ++repeat) {
          const auto result =
              train_nseat_parallel(tree, known, iterations, seed, limits(), nullptr, workers);
          CHECK(valid_result(tree, result, iterations, seed));
          CHECK(result.batch_size == workers);
          if (previous)
            CHECK(same_result(*previous, result));
          else
            previous = result;
          CHECK(result.visits >= iterations * seats);
          const auto context = "seats=" + std::to_string(seats) + " seed=" + std::to_string(seed) +
                               " workers=" + std::to_string(workers) +
                               " repeat=" + std::to_string(repeat);
          const double gap = exact_gap(tree, known, result.policy, context);
          std::fprintf(stderr, "  %s summed_gap=%.9f serial_gap=%.9f bound=%.9f\n", context.c_str(),
                       gap, serial_gap, 0.1 * static_cast<double>(tree.def().pot));
          if (gap > 0.1 * static_cast<double>(tree.def().pot) ||
              gap > serial_gap + 0.1 * static_cast<double>(tree.def().pot)) {
            if (quality_failures == 0)
              dump_rows(tree, result);
            ++quality_failures;
          }
          lowest = std::min(lowest, gap);
          highest = std::max(highest, gap);
        }
      }
    }
    std::printf("  %zu-seat singleton-range parallel exact gap: [%g, %g] chips\n", seats, lowest,
                highest);
  }
  CHECK(quality_failures == 0);
  return 0;
}

class ZeroFrontier final : public bs::gto::FrontierEvaluator {
 public:
  std::vector<double> evaluate(std::span<const int>, std::span<const std::array<int, 2>>,
                               const TerminalPayload& ledger) const override {
    return std::vector<double>(ledger.player_count, 0.0);
  }
};

class InjectedFailure : public std::runtime_error {
 public:
  InjectedFailure() : std::runtime_error("native injected frontier failure") {}
};

class FailingFrontier final : public bs::gto::FrontierEvaluator {
 public:
  std::vector<double> evaluate(std::span<const int>, std::span<const std::array<int, 2>>,
                               const TerminalPayload&) const override {
    throw InjectedFailure();
  }
};

int frontier_accounting_and_failure() {
  GameDef def;
  def.player_count = 2;
  def.button = 0;
  def.big_blind = 2;
  def.stacks = {2, 2};
  def.blinds_posted = {1, 2};
  def.pot = 2;
  def.preflop = true;
  def.terminal = TerminalDepth::Flop;
  const AbstractTree tree(def, bs::abstraction::ActionAbstraction::identity());
  CHECK(tree.size() == 1 && tree.node(0).is_flop_deal());
  const auto known = ranges();
  const ZeroFrontier frontier;
  for (const std::size_t workers :
       {std::size_t{1}, std::size_t{2}, std::size_t{4}, std::size_t{128}}) {
    const auto result = train_nseat_parallel(tree, known, 11, 42, limits(), &frontier, workers);
    CHECK(valid_result(tree, result, 11, 42));
    CHECK(result.visits == 22);
    CHECK(result.raw_rows.empty());
    CHECK(result.policy.rows().empty());
  }
  const FailingFrontier failing;
  CHECK(throws_type<InjectedFailure>(
      [&] { train_nseat_parallel(tree, known, 11, 42, limits(), &failing, 4); }));
  CHECK(throws_type<std::invalid_argument>(
      [&] { train_nseat_parallel(tree, known, 11, 42, limits(), nullptr, 4); }));
  return 0;
}

int invalid_and_resource_limits() {
  const AbstractTree tree(river(), bs::abstraction::ActionAbstraction::identity());
  const auto known = ranges();
  CHECK(throws_type<std::invalid_argument>(
      [&] { train_nseat_parallel(tree, known, 0, 42, limits(), nullptr, 4); }));
  auto bad = limits();
  bad.max_nodes = 0;
  CHECK(throws_type<std::invalid_argument>(
      [&] { train_nseat_parallel(tree, known, 10, 42, bad, nullptr, 4); }));
  bad = limits();
  bad.wall = std::chrono::milliseconds(0);
  CHECK(throws_type<std::invalid_argument>(
      [&] { train_nseat_parallel(tree, known, 10, 42, bad, nullptr, 4); }));
  CHECK(throws_type<std::invalid_argument>(
      [&] { train_nseat_parallel(tree, {known.front()}, 10, 42, limits(), nullptr, 4); }));
  NSeatInitialState incompatible;
  incompatible.algorithm_revision = 1;
  CHECK(throws_type<std::invalid_argument>(
      [&] { train_nseat(tree, known, 10, 42, limits(), nullptr, &incompatible); }));
  incompatible.algorithm_revision = kNSeatAlgorithmRevision;
  incompatible.batch_size = 2;
  CHECK(throws_type<std::invalid_argument>(
      [&] { train_nseat(tree, known, 10, 42, limits(), nullptr, &incompatible); }));
  const auto baseline = train_nseat(tree, known, 64, 42, limits());
  auto shared_bytes = limits();
  shared_bytes.max_bytes = std::max(tree.accounted_bytes(), baseline.accounted_bytes) + 4096;
  const auto fitted = train_nseat(tree, known, 64, 42, shared_bytes);
  CHECK(same_result(baseline, fitted));
  CHECK(fitted.accounted_bytes <= shared_bytes.max_bytes);
  // The serial-fitting cap must not be granted separately to each worker's
  // simultaneously live delta map and sampler scratch buffers.
  CHECK(throws_type<nseat_training_exhausted>(
      [&] { train_nseat_parallel(tree, known, 64, 42, shared_bytes, nullptr, 8); }));
  for (int resource = 0; resource < 5; ++resource) {
    auto capped = limits();
    if (resource == 0)
      capped.max_nodes = tree.size() - 1;
    else if (resource == 1)
      capped.max_depth = tree.depth() - 1;
    else if (resource == 2)
      capped.max_bytes = tree.accounted_bytes() - 1;
    else if (resource == 3)
      capped.max_information_sets = 1;
    else
      capped.max_visits = 1;
    CHECK(throws_type<nseat_training_exhausted>(
        [&] { train_nseat_parallel(tree, known, 100, 42, capped, nullptr, 4); }));
  }
  // The wall applies at iteration boundaries. A run either returns completed
  // iterations or rejects zero completed work; it must never publish zero.
  auto wall = limits();
  wall.wall = std::chrono::milliseconds(20);
  try {
    const auto result = train_nseat_parallel(tree, known, 1'000'000, 42, wall, nullptr, 4);
    CHECK(result.termination == NSeatTerminationPhase::WallClock);
    CHECK(result.completed_iterations > 0 && result.completed_iterations < 1'000'000);
    CHECK(result.policy.rows().size() == result.raw_rows.size());
  } catch (const nseat_training_exhausted& error) {
    CHECK(std::string(error.what()).find("zero iterations") != std::string::npos);
  }
  return 0;
}

}  // namespace

int main() {
  try {
    if (allocation_optimization_fingerprints() || one_worker_and_clamping() ||
        frontier_accounting_and_failure() || invalid_and_resource_limits() ||
        zero_opponent_reach_still_averages_response() || independent_average_weights() ||
        frozen_batch_and_one_clipping_step() || multiworker_quality())
      return 1;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "unexpected exception: %s\n", error.what());
    return 1;
  }
  std::printf("n-seat parallel verification PASS\n");
  return 0;
}
