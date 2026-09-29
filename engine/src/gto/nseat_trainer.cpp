// nseat_trainer.cpp — RFC 0009 W2a seat-parameterized external-sampling MCCFR.
//
// The header (bs/nseat_trainer.hpp) states the contract, the update rule, the
// stream derivation, and the refusal layering. This file adds only the
// implementation notes the header does not carry:
//
//   * Every action-node visit asserts its menu equals the shared L3 rule
//     (`bs::tree::abstract_node_menu`) and every chance-node visit asserts the
//     sampled card's child position carries that card as its `incoming_card`
//     edge label, so a walk desynced from the tree fails loudly instead of
//     scoring a game the tree does not describe.
//   * The evaluated root is `GameState(tree.def())`: the same deterministic
//     construction the L3 builder used for node 0, so the walk's state space
//     and the tree's node space are the same game by construction.
//   * The joint deal is conditioned on the rooted board (`def.board`), the
//     same board the tree enumerated its chance children against.
//   * Row references stay valid across recursion: node insertion into a
//     std::map never invalidates references to other elements.
#include <algorithm>
#include <array>
#include <bs/multiway_sampler.hpp>
#include <bs/nseat_trainer.hpp>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <numeric>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace bs::solver {

namespace {

using poker::Action;
using poker::GameState;
using poker::Phase;
using tree::AbstractTree;
using tree::NodeKind;
using tree::TerminalPayload;
using tree::TreeNode;

// The L4-owned card abstraction default. The artifact schema v2 (RFC 0009 D3)
// later makes this declaration explicit; today it is the coarse tier kind the
// stage-6 measurement core also trained under, so the two agree on what a
// bucket means.
inline constexpr abstraction::CardBucketKind kNSeatCardKind =
    abstraction::CardBucketKind::CategoryTiersV1;

// --- pinned sampling primitives --------------------------------------------
// Same conventions as the heads-up solver and the stage-6 trainer: a
// rejecting-threshold bounded index and the top-53-bits unit draw. These are
// re-derived here (not shared) because the dependency fences forbid the
// stage-6 targets; the conventions are what must agree, not the symbols.

std::uint64_t fnv1a(const std::string& bytes) {
  std::uint64_t hash = 0xcbf29ce484222325ULL;
  for (unsigned char byte : bytes) {
    hash ^= byte;
    hash *= 0x100000001b3ULL;
  }
  return hash;
}

// Domain separator so the derived stream can never equal a raw SplitMix64
// seeded with the FNV value ("BSNSTAM2": non-seat stream tag, rev 2 layout).
inline constexpr std::uint64_t kNSeatStreamMixer = 0x42534E5354414D32ULL;

std::size_t bounded_index(SplitMix64& rng, std::size_t bound) {
  if (bound == 0)
    throw std::runtime_error("nseat trainer sampled from an empty list");
  const auto limit = static_cast<std::uint64_t>(bound);
  const std::uint64_t threshold = (std::numeric_limits<std::uint64_t>::max() - limit + 1) % limit;
  for (;;) {
    const std::uint64_t value = rng.next_u64();
    if (value >= threshold)
      return static_cast<std::size_t>(value % limit);
  }
}

double unit_draw(SplitMix64& rng) {
  return static_cast<double>(rng.next_u64() >> 11) * (1.0 / 9007199254740992.0);
}

// Cumulative positive-weight scan in fixed ascending order; never iterate a
// map to consume randomness.
std::size_t weighted_index(SplitMix64& rng, const std::vector<double>& weights) {
  const double point = unit_draw(rng);
  double cumulative = 0.0;
  std::size_t last = 0;
  for (std::size_t i = 0; i < weights.size(); ++i) {
    if (weights[i] <= 0.0)
      continue;
    cumulative += weights[i];
    last = i;
    if (point < cumulative)
      return i;
  }
  return last;
}

// --- request validation and range conversion --------------------------------

// Validates the per-seat ranges for the given seat count and converts them to
// the joint sampler's carrier. A duplicated combo would double its mass in the
// sampler's marginal while the sealed row set would silently pool it, so an
// intra-seat repeat is rejected outright (the same fail-closed rule the
// stage-6 resolve_ranges applies).
std::vector<std::vector<MultiwayWeightedHand>> to_sampler_ranges(
    const std::vector<std::vector<WeightedHand>>& ranges, std::size_t seats) {
  if (ranges.size() != seats)
    throw std::invalid_argument("nseat trainer needs one range per game seat");
  std::vector<std::vector<MultiwayWeightedHand>> out;
  out.reserve(seats);
  for (const std::vector<WeightedHand>& seat : ranges) {
    if (seat.empty())
      throw std::invalid_argument("nseat trainer seat range is empty");
    std::vector<MultiwayWeightedHand> converted;
    converted.reserve(seat.size());
    std::array<bool, 52 * 52> seen{};
    for (const WeightedHand& hand : seat) {
      if (hand.cards[0] < 0 || hand.cards[1] >= 52 || hand.cards[0] >= hand.cards[1])
        throw std::invalid_argument(
            "nseat trainer combo must carry two distinct sorted cards (a < b)");
      if (!std::isfinite(hand.weight) || hand.weight <= 0.0)
        throw std::invalid_argument("nseat trainer combo weight must be finite and positive");
      const std::size_t seen_key =
          static_cast<std::size_t>(hand.cards[0]) * 52 + static_cast<std::size_t>(hand.cards[1]);
      if (seen[seen_key])
        throw std::invalid_argument("nseat trainer seat range repeats a combo");
      seen[seen_key] = true;
      converted.push_back(MultiwayWeightedHand{{hand.cards[0], hand.cards[1]}, hand.weight});
    }
    out.push_back(std::move(converted));
  }
  return out;
}

std::vector<int> board_vector(const GameState& state) {
  return std::vector<int>(state.board().begin(), state.board().end());
}

// Legal runout cards for the sampled world: the board-only set minus EVERY
// seat's two hole cards (folded seats' cards stay removed for the whole
// sweep). Chance must draw from this list -- a card a player holds cannot come
// -- while the tree child is addressed by the card's BOARD-ONLY ordinal,
// exactly the alignment the stage-6 trainer pins.
std::vector<int> legal_runout_cards(const GameState& state,
                                    const std::vector<std::array<int, 2>>& holes) {
  std::array<bool, 52> used{};
  for (int card : state.board())
    used[card] = true;
  for (const std::array<int, 2>& hole : holes)
    used[hole[0]] = used[hole[1]] = true;
  std::vector<int> cards;
  for (int card = 0; card < 52; ++card)
    if (!used[card])
      cards.push_back(card);
  return cards;
}

// The card's index in the ascending board-only list {0..51}\state.board().
// The tree's chance children enumerate exactly that list, so this ordinal IS
// the child position even though the sampled legal list also removed holes.
std::size_t board_only_ordinal(const GameState& state, int card) {
  std::size_t ordinal = 0;
  for (int c = 0; c < card; ++c) {
    bool on_board = false;
    for (int b : state.board())
      if (b == c)
        on_board = true;
    if (!on_board)
      ++ordinal;
  }
  return ordinal;
}

// --- raw rows and the shared row store --------------------------------------

std::size_t row_bytes(std::size_t actions) {
  // The same conservative retained charge the stage-6 row store applies: map
  // node/key/row overhead plus the per-action vectors at grown capacity.
  return 512 + actions * (sizeof(Action) + 2 * sizeof(double)) * 4;
}

// Regret matching with clipped (RM+) regrets, frozen before recursion.
std::vector<double> current_strategy(const NSeatRawRow& row) {
  std::vector<double> sigma(row.actions.size(), 0.0);
  double positive_sum = 0.0;
  for (double r : row.regrets) {
    if (!std::isfinite(r))
      throw std::runtime_error("nseat trainer row carries a non-finite regret");
    positive_sum += std::max(0.0, r);
  }
  if (positive_sum > 0.0) {
    for (std::size_t i = 0; i < row.actions.size(); ++i)
      sigma[i] = std::max(0.0, row.regrets[i]) / positive_sum;
  } else {
    for (double& p : sigma)
      p = 1.0 / static_cast<double>(row.actions.size());
  }
  return sigma;
}

// --- one traverser sweep -----------------------------------------------------

struct SweepContext {
  std::size_t traverser = 0;
  const AbstractTree* tree = nullptr;
  const std::vector<std::array<int, 2>>* holes = nullptr;
  std::map<NSeatInformationKey, NSeatRawRow>* rows = nullptr;
  std::size_t* retained_bytes = nullptr;
  std::size_t* visits = nullptr;
  const NSeatTrainerLimits* limits = nullptr;
  SplitMix64* action_rng = nullptr;
  SplitMix64* chance_rng = nullptr;
  std::size_t depth = 0;
};

NSeatRawRow& touch_row(SweepContext& ctx, const NSeatInformationKey& key,
                       const std::vector<Action>& menu) {
  auto it = ctx.rows->find(key);
  if (it != ctx.rows->end()) {
    if (it->second.actions != menu)
      throw std::runtime_error("nseat trainer row address merged nodes with different menus");
    return it->second;
  }
  if (ctx.rows->size() >= ctx.limits->max_information_sets)
    throw nseat_training_exhausted("nseat trainer exceeded the information-set cap");
  const std::size_t charge = row_bytes(menu.size());
  if (charge > ctx.limits->max_bytes - *ctx.retained_bytes)
    throw nseat_training_exhausted("nseat trainer exceeded the byte cap");
  *ctx.retained_bytes += charge;
  NSeatRawRow row;
  row.actions = menu;
  row.regrets.assign(menu.size(), 0.0);
  row.sums.assign(menu.size(), 0.0);
  return ctx.rows->emplace(key, std::move(row)).first->second;
}

// Visit/depth accounting, checked BEFORE the crossing work. The visit cap
// bounds the unbounded sampled walk; the depth cap bounds a pathological
// recursion (the tree build already enforces the same bound on the finite
// tree).
struct DepthGuard {
  SweepContext& ctx;
  explicit DepthGuard(SweepContext& context) : ctx(context) {
    ++ctx.depth;
    if (ctx.depth > ctx.limits->max_depth)
      throw nseat_training_exhausted("nseat trainer exceeded the depth cap");
    if (*ctx.visits == ctx.limits->max_visits)
      throw nseat_training_exhausted("nseat trainer exceeded the visit cap");
    ++*ctx.visits;
  }
  ~DepthGuard() { --ctx.depth; }
  DepthGuard(const DepthGuard&) = delete;
  DepthGuard& operator=(const DepthGuard&) = delete;
};

// The traverser's exact utility at one terminal, asserting exact chip
// conservation across the whole table exactly as the stage-6 trainer does: a
// fold leaf reads the L3 payload's stored chip_utility; a showdown leaf
// settles the concrete live holdings through the L1 rules.
double terminal_utility(SweepContext& ctx, const GameState& state, std::size_t node_index) {
  const TreeNode& node = ctx.tree->node(node_index);
  if (!node.is_terminal())
    throw std::runtime_error("nseat trainer terminal reached a non-terminal tree node");
  const TerminalPayload& payload = ctx.tree->terminal(node.terminal);
  if (state.phase() == Phase::Folded) {
    if (!payload.folded)
      throw std::runtime_error("nseat trainer fold terminal does not match the tree leaf");
    if (payload.chip_utility.size() != state.player_count())
      throw std::runtime_error("nseat trainer fold payout vector has the wrong seat count");
    std::int64_t sum = 0;
    for (std::int64_t utility : payload.chip_utility)
      sum += utility;
    if (sum != 0)
      throw std::runtime_error("nseat trainer terminal settlement is not zero-sum");
    return static_cast<double>(payload.chip_utility[ctx.traverser]);
  }
  if (state.phase() != Phase::Showdown)
    throw std::runtime_error("nseat trainer terminal reached a non-terminal game state");
  if (payload.folded)
    throw std::runtime_error("nseat trainer showdown terminal does not match the tree leaf");
  if (payload.live_count != state.live_players().size())
    throw std::runtime_error("nseat trainer showdown ledger has the wrong live count");
  std::vector<std::array<int, 2>> live_holes(payload.live_count);
  for (std::size_t i = 0; i < payload.live_count; ++i) {
    const std::size_t seat = payload.live_order[i];
    if (seat >= state.player_count() || state.live_players()[i] != seat)
      throw std::runtime_error("nseat trainer showdown live order desynced from the walk");
    live_holes[i] = (*ctx.holes)[seat];
  }
  const poker::ContributionSettlement settlement = state.settle_showdown(live_holes);
  std::int64_t sum = 0;
  for (std::size_t seat = 0; seat < state.player_count(); ++seat)
    sum += settlement.chip_utility[seat];
  if (sum != 0)
    throw std::runtime_error("nseat trainer terminal settlement is not zero-sum");
  return static_cast<double>(settlement.chip_utility[ctx.traverser]);
}

double walk(SweepContext& ctx, const GameState& state, std::size_t node_index, double own_reach);

double walk_action(SweepContext& ctx, const GameState& state, std::size_t node_index,
                   double own_reach) {
  DepthGuard frame(ctx);
  const TreeNode& node = ctx.tree->node(node_index);
  if (!node.is_action())
    throw std::runtime_error("nseat trainer expected an action node");
  const std::size_t actor = *state.actor();
  if (actor != node.actor)
    throw std::runtime_error("nseat trainer acting seat drifted from the abstract tree");
  const std::vector<Action>& menu = node.actions;
  // The menu the builder stored for this node must be the shared rule's output
  // on the walk's state: otherwise the walk and the node describe different
  // games (a desync that would silently corrupt every row it touches).
  if (menu != tree::abstract_node_menu(state, ctx.tree->action_abstraction(), state.legal()))
    throw std::runtime_error("nseat trainer menu drifted from the abstract tree");
  if (node.children.size() != menu.size())
    throw std::runtime_error("nseat trainer action node degree drifted from its menu");

  NSeatInformationKey key;
  key.own_card_bucket = static_cast<std::uint32_t>(
      abstraction::card_bucket(kNSeatCardKind, (*ctx.holes)[actor], board_vector(state)));
  key.node_index = node_index;
  NSeatRawRow& row = touch_row(ctx, key, menu);
  const std::vector<double> sigma = current_strategy(row);

  if (actor == ctx.traverser) {
    // kFull own-reach-weighted average AT THE TRAVERSER'S OWN NODE, once per
    // sweep (the full derivation is in the header). visits counts own-node
    // touches separately for maturity instrumentation; it is not the weight.
    for (std::size_t a = 0; a < menu.size(); ++a)
      row.sums[a] += own_reach * sigma[a];
    ++row.visits;

    // Enumerate every action over its sampled external continuation, then the
    // unweighted RM+ regret update on return. Node insertion during recursion
    // cannot invalidate the `row` reference (std::map reference stability).
    std::vector<double> children(menu.size(), 0.0);
    for (std::size_t a = 0; a < menu.size(); ++a) {
      GameState next = state.after_action(actor, menu[a]);
      children[a] = walk(ctx, next, node.children[a], own_reach * sigma[a]);
    }
    double value = 0.0;
    for (std::size_t a = 0; a < menu.size(); ++a)
      value += sigma[a] * children[a];
    for (std::size_t a = 0; a < menu.size(); ++a) {
      const double updated = row.regrets[a] + (children[a] - value);
      row.regrets[a] = updated > 0.0 ? updated : 0.0;
    }
    return value;
  }

  // Opponent node: external sampling takes exactly one sampled action; the
  // opponent's reach is integrated out, so own_reach is unchanged and there is
  // no average contribution (the traverser never owns this node).
  const std::size_t sampled = weighted_index(*ctx.action_rng, sigma);
  GameState next = state.after_action(actor, menu[sampled]);
  return walk(ctx, next, node.children[sampled], own_reach);
}

double walk_chance(SweepContext& ctx, const GameState& state, std::size_t node_index,
                   double own_reach) {
  DepthGuard frame(ctx);
  const TreeNode& node = ctx.tree->node(node_index);
  if (!node.is_chance())
    throw std::runtime_error("nseat trainer expected a chance node");
  // Uniform over the sampled world's legal list (holes removed), addressed by
  // the board-only ordinal: the tree's chance children enumerate exactly the
  // ascending {0..51}\board list, so the child's edge label asserts the
  // alignment this walk depends on.
  const std::vector<int> cards = legal_runout_cards(state, *ctx.holes);
  const std::size_t index = bounded_index(*ctx.chance_rng, cards.size());
  const std::size_t ordinal = board_only_ordinal(state, cards[index]);
  if (ordinal >= node.children.size())
    throw std::runtime_error("nseat trainer chance ordinal outside the tree's chance degree");
  if (ctx.tree->node(node.children[ordinal]).incoming_card != cards[index])
    throw std::runtime_error("nseat trainer chance child is not at its board ordinal");
  GameState next = state.after_card(cards[index]);
  // Chance is externally sampled; own reach is unchanged.
  return walk(ctx, next, node.children[ordinal], own_reach);
}

double walk(SweepContext& ctx, const GameState& state, std::size_t node_index, double own_reach) {
  switch (state.phase()) {
    case Phase::Folded:
    case Phase::Showdown:
      return terminal_utility(ctx, state, node_index);
    case Phase::Deal:
      return walk_chance(ctx, state, node_index, own_reach);
    case Phase::Action:
      return walk_action(ctx, state, node_index, own_reach);
  }
  throw std::runtime_error("nseat trainer reached an unrecognized game phase");
}

// Samples this sweep's world (one joint deal over the per-seat ranges,
// conditioned on the rooted board) and runs the traverser's walk from the
// tree's root state.
void run_sweep(const AbstractTree& tree,
               const std::vector<std::vector<MultiwayWeightedHand>>& ranges,
               const std::vector<int>& root_board, std::size_t traverser,
               NSeatTraversalStreams& streams, std::map<NSeatInformationKey, NSeatRawRow>& table,
               std::size_t& retained_bytes, std::size_t& visits, const NSeatTrainerLimits& limits) {
  const MultiwayDeal deal = sample_scalable_joint_deal(ranges, root_board, streams.joint_deal);
  if (deal.hands.size() != ranges.size())
    throw std::runtime_error("nseat trainer joint deal has the wrong seat count");
  std::vector<std::array<int, 2>> holes;
  holes.reserve(deal.hands.size());
  for (const std::array<int, 2>& hand : deal.hands)
    holes.push_back(hand);

  SweepContext ctx;
  ctx.traverser = traverser;
  ctx.tree = &tree;
  ctx.holes = &holes;
  ctx.rows = &table;
  ctx.retained_bytes = &retained_bytes;
  ctx.visits = &visits;
  ctx.limits = &limits;
  ctx.action_rng = &streams.opponent_action;
  ctx.chance_rng = &streams.chance_card;
  ctx.depth = 0;
  // The same deterministic root construction the L3 builder used for node 0.
  const GameState root(tree.def());
  walk(ctx, root, tree.root_index(), 1.0);
}

NSeatPolicyRow seal_row(const NSeatRawRow& raw) {
  NSeatPolicyRow out;
  out.actions = raw.actions;
  out.visits = raw.visits;
  out.probabilities.resize(raw.actions.size());
  const double total = std::accumulate(raw.sums.begin(), raw.sums.end(), 0.0);
  if (total > 0.0) {
    for (std::size_t i = 0; i < raw.actions.size(); ++i)
      out.probabilities[i] = raw.sums[i] / total;
  } else {
    for (double& p : out.probabilities)
      p = 1.0 / static_cast<double>(raw.actions.size());
  }
  return out;
}

// Validates everything the trainer requires of (tree, ranges); shared by the
// production entry point and the debug seam so the two cannot drift.
void validate_request(const AbstractTree& tree,
                      const std::vector<std::vector<WeightedHand>>& ranges) {
  const poker::GameDef& def = tree.def();
  if (def.player_count < 2 || def.player_count > poker::kMaxUnifiedSeats)
    throw std::invalid_argument("nseat trainer supports 2..10 seats");
  if (tree.action_id() != abstraction::identity_action_id())
    throw std::invalid_argument("nseat trainer solves only the identity action abstraction");
  if (def.board_size < 3 || def.board_size > 5)
    throw std::invalid_argument("nseat trainer requires a postflop root (3..5 public cards)");
  if (ranges.size() != def.player_count)
    throw std::invalid_argument("nseat trainer needs one range per game seat");
}

}  // namespace

SplitMix64 derive_nseat_stream(std::uint64_t master_seed, NSeatStreamPurpose purpose,
                               std::uint64_t iteration, std::size_t traverser) {
  // Pinned derivation (header): FNV-1a over the ASCII role string, XORed with
  // the domain mixer, seeding SplitMix64. Deliberately independent of the
  // stage-6 derivation: the two trainers are separate policy streams.
  const std::string role = "bsnseat|" + std::to_string(static_cast<std::uint64_t>(purpose)) + "|" +
                           std::to_string(master_seed) + "|" + std::to_string(iteration) + "|" +
                           std::to_string(static_cast<std::uint64_t>(traverser));
  return SplitMix64(fnv1a(role) ^ kNSeatStreamMixer);
}

NSeatTrainingResult train_nseat(const AbstractTree& tree,
                                const std::vector<std::vector<WeightedHand>>& ranges,
                                std::uint64_t iterations, std::uint64_t master_seed,
                                const NSeatTrainerLimits& limits) {
  validate_request(tree, ranges);
  if (iterations == 0)
    throw std::invalid_argument("nseat trainer requires a positive iteration count");
  if (limits.max_nodes == 0 || limits.max_information_sets == 0 || limits.max_depth == 0 ||
      limits.max_bytes == 0 || limits.max_visits == 0 || limits.wall.count() <= 0)
    throw std::invalid_argument("nseat trainer requires positive resource limits");
  const std::size_t seats = tree.def().player_count;
  // The materialized tree the caller supplied must already fit the request's
  // structural caps; checking here means every entry point enforces them, and
  // the check precedes any retained training allocation.
  if (tree.size() > limits.max_nodes)
    throw nseat_training_exhausted("nseat trainer tree exceeds the node cap");
  if (tree.depth() > limits.max_depth)
    throw nseat_training_exhausted("nseat trainer tree exceeds the depth cap");
  if (tree.accounted_bytes() > limits.max_bytes)
    throw nseat_training_exhausted("nseat trainer tree exceeds the byte cap");
  const auto sampler_ranges = to_sampler_ranges(ranges, seats);
  const std::vector<int> root_board(tree.def().board.begin(),
                                    tree.def().board.begin() + tree.def().board_size);

  std::map<NSeatInformationKey, NSeatRawRow> table;
  std::size_t retained_bytes = 0;
  std::size_t visits = 0;
  // Witness RNG advanced exactly once per COMPLETED iteration. The derived
  // iteration key 0x5749544e45535300 ("WITNESS\0") cannot collide with a real
  // iteration's stream, and its first draw seeds the witness.
  SplitMix64 witness(
      derive_nseat_stream(master_seed, NSeatStreamPurpose::JointDeal, 0x5749544e45535300ULL, 0)
          .next_u64());

  // The wall cap is checked at ITERATION boundaries only, exactly like the
  // stage-6 trainer: no iteration is ever interrupted by the wall, so the
  // returned policy always reflects a whole number of completed iterations.
  // Every other cap throws out of this call (nothing is returned), which keeps
  // the same property for every termination path.
  const auto wall_start = std::chrono::steady_clock::now();
  bool wall_reached = false;
  std::uint64_t completed = 0;
  for (std::uint64_t iter = 0; iter < iterations; ++iter) {
    if (std::chrono::steady_clock::now() - wall_start > limits.wall) {
      wall_reached = true;
      break;
    }
    for (std::size_t traverser = 0; traverser < seats; ++traverser) {
      NSeatTraversalStreams streams{
          derive_nseat_stream(master_seed, NSeatStreamPurpose::JointDeal, iter, traverser),
          derive_nseat_stream(master_seed, NSeatStreamPurpose::OpponentAction, iter, traverser),
          derive_nseat_stream(master_seed, NSeatStreamPurpose::ChanceCard, iter, traverser)};
      run_sweep(tree, sampler_ranges, root_board, traverser, streams, table, retained_bytes, visits,
                limits);
    }
    ++completed;
    witness.next_u64();
  }
  if (wall_reached && completed == 0)
    throw nseat_training_exhausted(
        "nseat trainer completed zero iterations before the wall cap; no policy to publish");

  NSeatTrainingResult result;
  result.termination =
      wall_reached ? NSeatTerminationPhase::WallClock : NSeatTerminationPhase::Complete;
  result.completed_iterations = completed;
  result.nodes = tree.size();
  result.visits = visits;
  result.information_sets = table.size();
  result.accounted_bytes = retained_bytes;
  result.seed = master_seed;
  result.prng_state = witness.next_u64();
  result.algorithm_revision = kNSeatAlgorithmRevision;
  result.policy.game_ = tree.def();
  result.policy.action_id_ = tree.action_id();
  for (const auto& [key, raw] : table)
    result.policy.rows_.emplace(key, seal_row(raw));
  return result;
}

void debug_run_one_nseat_sweep(const AbstractTree& tree,
                               const std::vector<std::vector<WeightedHand>>& ranges,
                               std::size_t traverser, NSeatTraversalStreams& streams,
                               std::map<NSeatInformationKey, NSeatRawRow>& rows) {
  validate_request(tree, ranges);
  if (traverser >= tree.def().player_count)
    throw std::invalid_argument("debug sweep traverser is not a live seat");
  const auto sampler_ranges = to_sampler_ranges(ranges, tree.def().player_count);
  const std::vector<int> root_board(tree.def().board.begin(),
                                    tree.def().board.begin() + tree.def().board_size);
  const NSeatTrainerLimits limits;  // the seam runs under the default caps
  std::size_t retained_bytes = 0;
  std::size_t visits = 0;
  run_sweep(tree, sampler_ranges, root_board, traverser, streams, rows, retained_bytes, visits,
            limits);
}

const NSeatPolicyRow* NSeatPolicy::lookup(const tree::AbstractTree& tree,
                                          const poker::GameState& state, std::size_t seat,
                                          std::array<int, 2> own_cards) const {
  if (!poker::same_game_def(game_, tree.def()) || action_id_ != tree.action_id())
    return nullptr;
  if (seat >= game_.player_count || state.phase() != poker::Phase::Action || !state.actor() ||
      *state.actor() != seat)
    return nullptr;
  if (own_cards[0] < 0 || own_cards[1] > 51 || own_cards[0] >= own_cards[1])
    return nullptr;
  for (int card : state.board())
    if (card == own_cards[0] || card == own_cards[1])
      return nullptr;
  std::uint32_t bucket = 0;
  try {
    bucket = static_cast<std::uint32_t>(
        abstraction::card_bucket(kNSeatCardKind, own_cards, board_vector(state)));
  } catch (const std::invalid_argument&) {
    // A partial board has no defined bucket: an exact miss, never a guess.
    return nullptr;
  }

  // Depth-first search for the unique-enough node whose observable ledger
  // equals the query's; children are pushed in reverse so the LIFO pop visits
  // them in ascending child order (the deterministic ambiguity resolution the
  // header documents). Iterative: the native stack stays out of it.
  struct Frame {
    std::size_t node;
    GameState state;
  };
  const auto same_observable = [](const GameState& a, const GameState& b) {
    if (a.player_count() != b.player_count() || a.phase() != b.phase() ||
        a.street() != b.street() || a.pot() != b.pot() ||
        a.last_full_raise() != b.last_full_raise())
      return false;
    if (a.board().size() != b.board().size())
      return false;
    for (std::size_t i = 0; i < a.board().size(); ++i)
      if (a.board()[i] != b.board()[i])
        return false;
    for (std::size_t s = 0; s < a.player_count(); ++s) {
      const poker::GamePlayer& pa = a.players()[s];
      const poker::GamePlayer& pb = b.players()[s];
      if (pa.stack != pb.stack || pa.street_committed != pb.street_committed ||
          pa.contributed != pb.contributed || pa.refunded != pb.refunded ||
          pa.folded != pb.folded || pa.all_in != pb.all_in || pa.raise_rights != pb.raise_rights ||
          pa.pending != pb.pending)
        return false;
    }
    return true;
  };
  std::vector<Frame> stack;
  stack.push_back(Frame{tree.root_index(), GameState(tree.def())});
  while (!stack.empty()) {
    Frame frame = std::move(stack.back());
    stack.pop_back();
    const TreeNode& node = tree.node(frame.node);
    if (node.kind == NodeKind::TerminalFold || node.kind == NodeKind::TerminalShowdown)
      continue;
    if (node.kind == NodeKind::Chance) {
      const std::vector<int> cards = tree::public_runout_cards(frame.state);
      if (cards.size() != node.children.size())
        return nullptr;  // a tree desynced from the walk cannot be addressed: exact miss
      for (std::size_t i = cards.size(); i-- > 0;)
        stack.push_back(Frame{node.children[i], frame.state.after_card(cards[i])});
      continue;
    }
    if (frame.state.phase() == poker::Phase::Action && *frame.state.actor() == seat &&
        same_observable(frame.state, state)) {
      const auto it =
          rows_.find(NSeatInformationKey{bucket, static_cast<std::uint64_t>(frame.node)});
      return it == rows_.end() ? nullptr : &it->second;
    }
    if (node.children.size() != node.actions.size())
      return nullptr;
    for (std::size_t a = node.actions.size(); a-- > 0;)
      stack.push_back(
          Frame{node.children[a], frame.state.after_action(node.actor, node.actions[a])});
  }
  return nullptr;
}

}  // namespace bs::solver
