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
#include <atomic>
#include <bs/frontier.hpp>
#include <bs/multiway_sampler.hpp>
#include <bs/nseat_trainer.hpp>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <numeric>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace bs::solver {

namespace {

using poker::Action;
using poker::GameState;
using poker::Phase;
using tree::AbstractTree;
using tree::make_frontier_payload;
using tree::NodeKind;
using tree::TerminalPayload;
using tree::TreeNode;

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
std::size_t weighted_index(SplitMix64& rng, std::span<const double> weights) {
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

// Legal runout cards for the sampled world: the board-only set minus EVERY
// seat's two hole cards (folded seats' cards stay removed for the whole
// sweep). Chance must draw from this list -- a card a player holds cannot come
// -- while the tree child is addressed by the card's BOARD-ONLY ordinal,
// exactly the alignment the stage-6 trainer pins.
// Fills `out` with the legal runout cards (board + every player's holes
// removed) and returns the count.  At most 52 cards are ever legal, so a
// `std::array<int, 52>` caller buffer always suffices; this avoids a heap
// allocation on every chance-node visit.
std::size_t legal_runout_cards(const GameState& state, std::span<const std::array<int, 2>> holes,
                               std::span<int> out) {
  std::array<bool, 52> used{};
  for (int card : state.board())
    used[card] = true;
  for (const std::array<int, 2>& hole : holes)
    used[hole[0]] = used[hole[1]] = true;
  std::size_t count = 0;
  for (int card = 0; card < 52; ++card)
    if (!used[card])
      out[count++] = card;
  return count;
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
// Writes into the caller-provided span (must have row.actions.size() elements)
// to avoid a heap allocation on every action-node visit.
void current_strategy(const NSeatRawRow& row, std::span<double> sigma) {
  if (row.regrets.size() != sigma.size())
    throw std::invalid_argument("nseat trainer invalid frozen regret count");
  double positive_sum = 0.0;
  for (double r : row.regrets) {
    if (!std::isfinite(r))
      throw std::runtime_error("nseat trainer row carries a non-finite regret");
    positive_sum += std::max(0.0, r);
  }
  if (!std::isfinite(positive_sum))
    throw std::runtime_error("nseat trainer regret total overflowed");
  if (positive_sum > 0.0) {
    for (std::size_t i = 0; i < row.actions.size(); ++i)
      sigma[i] = std::max(0.0, row.regrets[i]) / positive_sum;
  } else {
    for (std::size_t i = 0; i < row.actions.size(); ++i)
      sigma[i] = 1.0 / static_cast<double>(row.actions.size());
  }
}

// --- one traverser sweep -----------------------------------------------------

// Deltas may be signed; they are not published NSeatRawRow accumulators.
struct SampleDelta {
  // The tree owns immutable menus for the complete training call. All three
  // slices share one allocation; sigma is frozen for this sample's epoch,
  // never recomputed from the signed local regret deltas.
  const std::vector<Action>* actions = nullptr;
  std::vector<double> values;
  std::uint64_t visits = 0;

  std::span<double> regrets() { return {values.data(), actions->size()}; }
  std::span<double> sums() { return {values.data() + actions->size(), actions->size()}; }
  std::span<double> sigma() { return {values.data() + 2 * actions->size(), actions->size()}; }
  std::span<const double> regrets() const { return {values.data(), actions->size()}; }
  std::span<const double> sums() const {
    return {values.data() + actions->size(), actions->size()};
  }
};
// Keep the existing conservative row charge, including its grown-capacity
// allowance, rather than using smaller physical allocations to raise caps.
static_assert(sizeof(SampleDelta) + sizeof(NSeatInformationKey) + 4 * sizeof(void*) <= 512);
static_assert(3 * sizeof(double) <= 4 * (sizeof(Action) + 2 * sizeof(double)));
using RawTable = std::map<NSeatInformationKey, NSeatRawRow>;
using DeltaTable = std::map<NSeatInformationKey, SampleDelta>;

// Master rows, duplicate worker rows, and the central discovery registry all
// reserve bytes before allocation. The master is immutable during computation.
class TrainingBudget {
 public:
  TrainingBudget(const RawTable& master, const NSeatTrainerLimits& limits)
      : master_(master), limits_(limits) {}

  void reserve(std::size_t charge) {
    std::lock_guard lock(mutex_);
    reserve_locked(charge);
  }
  void release(std::size_t charge) {
    std::lock_guard lock(mutex_);
    bytes_ -= charge;
  }
  void reserve_delta(const NSeatInformationKey& key, std::size_t charge) {
    std::lock_guard lock(mutex_);
    const bool discover = !master_.contains(key) && !discovered_.contains(key);
    if (discover && master_.size() + discovered_.size() >= limits_.max_information_sets)
      throw nseat_training_exhausted("nseat trainer exceeded the information-set cap");
    reserve_locked(charge);
    if (discover) {
      try {
        reserve_locked(kRegistryBytes);
        try {
          discovered_.emplace(key, true);
        } catch (...) {
          bytes_ -= kRegistryBytes;
          throw;
        }
      } catch (...) {
        bytes_ -= charge;
        throw;
      }
    }
  }
  void reserve_master(std::size_t charge) {
    std::lock_guard lock(mutex_);
    reserve_locked(charge);
    master_bytes_ += charge;
  }
  void published(const NSeatInformationKey& key) {
    std::lock_guard lock(mutex_);
    if (discovered_.erase(key))
      bytes_ -= kRegistryBytes;
  }
  std::size_t master_bytes() const { return master_bytes_; }

 private:
  void reserve_locked(std::size_t charge) {
    if (charge > limits_.max_bytes - bytes_)
      throw nseat_training_exhausted("nseat trainer exceeded the byte cap");
    bytes_ += charge;
  }
  static constexpr std::size_t kRegistryBytes = 256;
  const RawTable& master_;
  const NSeatTrainerLimits& limits_;
  std::mutex mutex_;
  std::size_t bytes_ = 0;
  std::size_t master_bytes_ = 0;
  std::map<NSeatInformationKey, bool> discovered_;
};

struct DeltaStorage {
  explicit DeltaStorage(TrainingBudget& budget) : budget(budget) {}
  ~DeltaStorage() { clear(); }
  void clear() {
    rows.clear();
    budget.release(bytes);
    bytes = 0;
  }
  TrainingBudget& budget;
  DeltaTable rows;
  std::size_t bytes = 0;
};

struct SweepContext {
  std::size_t traverser = 0;
  const AbstractTree* tree = nullptr;
  std::span<const std::array<int, 2>> holes;
  const RawTable* profile = nullptr;
  DeltaStorage* storage = nullptr;
  std::atomic<std::size_t>* visits = nullptr;
  const NSeatTrainerLimits* limits = nullptr;
  SplitMix64* action_rng = nullptr;
  SplitMix64* chance_rng = nullptr;
  const gto::FrontierEvaluator* frontier = nullptr;
  std::size_t depth = 0;
};

SampleDelta& touch_row(SweepContext& ctx, const NSeatInformationKey& key,
                       const std::vector<Action>& menu) {
  if (menu.empty() || menu.size() > 32)
    throw std::runtime_error("nseat trainer invalid menu size");
  const auto existing = ctx.storage->rows.lower_bound(key);
  if (existing != ctx.storage->rows.end() && existing->first == key) {
    if (*existing->second.actions != menu)
      throw std::runtime_error("nseat trainer delta menu mismatch");
    return existing->second;
  }
  const auto base = ctx.profile->find(key);
  if (base != ctx.profile->end() && base->second.actions != menu)
    throw std::runtime_error("nseat trainer frozen profile menu mismatch");
  const auto charge = row_bytes(menu.size());
  ctx.storage->budget.reserve_delta(key, charge);
  ctx.storage->bytes += charge;
  SampleDelta row;
  row.actions = &menu;
  row.values.assign(3 * menu.size(), 0.0);
  const auto probabilities = row.sigma();
  if (base == ctx.profile->end()) {
    std::fill(probabilities.begin(), probabilities.end(), 1.0 / probabilities.size());
  } else {
    current_strategy(base->second, probabilities);
  }
  return ctx.storage->rows.emplace_hint(existing, key, std::move(row))->second;
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
    auto visits = ctx.visits->load(std::memory_order_relaxed);
    for (;;) {
      if (visits >= ctx.limits->max_visits)
        throw nseat_training_exhausted("nseat trainer exceeded the visit cap");
      if (ctx.visits->compare_exchange_weak(visits, visits + 1, std::memory_order_relaxed))
        break;
    }
  }
  ~DepthGuard() { --ctx.depth; }
  DepthGuard(const DepthGuard&) = delete;
  DepthGuard& operator=(const DepthGuard&) = delete;
};

// The traverser's exact utility at one terminal, asserting exact chip
// conservation across the whole table exactly as the stage-6 trainer does: a
// fold leaf reads the L3 payload's stored chip_utility; a showdown leaf
// settles the concrete live holdings through the L1 rules; a frontier leaf
// (RFC 0007 flop-terminal) evaluates the continuation through the declared
// frontier evaluator.
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
  if (state.phase() == Phase::Frontier) {
    // RFC 0007: the frontier leaf's value is supplied by the declared frontier
    // evaluator, not by the rules' showdown. The evaluator sees the joint deal
    // and the flop board; it must not condition on the acting player's own
    // holding (the contract is documented in bs/frontier.hpp).
    if (ctx.frontier == nullptr)
      throw std::runtime_error(
          "nseat trainer reached a frontier leaf without a frontier evaluator");
    if (payload.folded)
      throw std::runtime_error("nseat trainer frontier terminal does not match the tree leaf");
    // evaluate() accepts spans; pass the board and holes directly.
    const std::vector<double> values = ctx.frontier->evaluate(state.board(), ctx.holes, payload);
    if (values.size() != state.player_count())
      throw std::runtime_error("nseat trainer frontier evaluator returned the wrong seat count");
    return values[ctx.traverser];
  }
  if (state.phase() != Phase::Showdown)
    throw std::runtime_error("nseat trainer terminal reached a non-terminal game state");
  if (payload.folded)
    throw std::runtime_error("nseat trainer showdown terminal does not match the tree leaf");
  if (payload.live_count != state.live_players().size())
    throw std::runtime_error("nseat trainer showdown ledger has the wrong live count");
  // Stack buffer: at most 10 live players at any table.
  std::array<std::array<int, 2>, 10> live_holes{};
  for (std::size_t i = 0; i < payload.live_count; ++i) {
    const std::size_t seat = payload.live_order[i];
    if (seat >= state.player_count() || state.live_players()[i] != seat)
      throw std::runtime_error("nseat trainer showdown live order desynced from the walk");
    live_holes[i] = ctx.holes[seat];
  }
  const poker::ContributionSettlement settlement = state.settle_showdown(
      std::span<const std::array<int, 2>>(live_holes.data(), payload.live_count));
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
      abstraction::card_bucket(kNSeatCardKind, ctx.holes[actor], state.board()));
  key.node_index = node_index;
  SampleDelta& row = touch_row(ctx, key, menu);
  const double* sigma = row.sigma().data();

  if (actor == ctx.traverser) {
    ++row.visits;

    // Enumerate every action over its sampled external continuation, then the
    // unweighted RM+ regret update on return. Node insertion during recursion
    // cannot invalidate the `row` reference (std::map reference stability).
    // No lock is held during recursion: the recursive walk only touches rows
    // through its own touch_row/current_strategy calls.
    std::array<double, 32> children{};
    for (std::size_t a = 0; a < menu.size(); ++a) {
      GameState next = state.after_action(actor, menu[a]);
      children[a] = walk(ctx, next, node.children[a], own_reach * sigma[a]);
    }
    double value = 0.0;
    for (std::size_t a = 0; a < menu.size(); ++a)
      value += sigma[a] * children[a];
    for (std::size_t a = 0; a < menu.size(); ++a)
      row.regrets()[a] += children[a] - value;
    return value;
  }

  // Opponent node: external sampling takes exactly one sampled action; the
  // opponent's reach is integrated out, so own_reach is unchanged and there is
  // no average contribution (the traverser never owns this node).
  const std::size_t sampled =
      weighted_index(*ctx.action_rng, std::span<const double>(sigma, menu.size()));
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
  std::array<int, 52> cards_buf{};
  const std::size_t cards_count = legal_runout_cards(state, ctx.holes, cards_buf);
  const std::size_t index = bounded_index(*ctx.chance_rng, cards_count);
  const std::size_t ordinal = board_only_ordinal(state, cards_buf[index]);
  if (ordinal >= node.children.size())
    throw std::runtime_error("nseat trainer chance ordinal outside the tree's chance degree");
  if (ctx.tree->node(node.children[ordinal]).incoming_card != cards_buf[index])
    throw std::runtime_error("nseat trainer chance child is not at its board ordinal");
  GameState next = state.after_card(cards_buf[index]);
  // Chance is externally sampled; own reach is unchanged.
  return walk(ctx, next, node.children[ordinal], own_reach);
}

// Virtual flop deal: the tree stores a single FlopDeal leaf instead of the
// 3-level chance subtree. For a preflop game (empty board), sample 3 cards
// inline to deal the flop. For a flop-rooted game (board already has 3
// cards), use the current state directly — no cards to deal.
double walk_flop_deal(SweepContext& ctx, const GameState& state, std::size_t node_index,
                      double own_reach) {
  DepthGuard frame(ctx);
  const TreeNode& node = ctx.tree->node(node_index);
  if (!node.is_flop_deal())
    throw std::runtime_error("nseat trainer expected a flop-deal leaf");
  if (ctx.frontier == nullptr)
    throw std::runtime_error("nseat trainer reached a flop-deal leaf without a frontier evaluator");

  const std::size_t board_size = state.board().size();
  GameState frontier_state = state;  // overwritten below
  // Reused across the three sequential flop-deal draws; each call overwrites
  // from index 0 so no stale cards survive.
  std::array<int, 52> cards_buf{};

  if (board_size == 0) {
    // Preflop game: sample 3 cards sequentially to deal the flop.
    GameState s1 = state;
    {
      const std::size_t n = legal_runout_cards(s1, ctx.holes, cards_buf);
      const std::size_t idx = bounded_index(*ctx.chance_rng, n);
      s1 = s1.after_card(cards_buf[idx]);
    }
    GameState s2 = s1;
    {
      const std::size_t n = legal_runout_cards(s1, ctx.holes, cards_buf);
      const std::size_t idx = bounded_index(*ctx.chance_rng, n);
      s2 = s1.after_card(cards_buf[idx]);
    }
    {
      const std::size_t n = legal_runout_cards(s2, ctx.holes, cards_buf);
      const std::size_t idx = bounded_index(*ctx.chance_rng, n);
      frontier_state = s2.after_card(cards_buf[idx]);
    }
    if (frontier_state.phase() != Phase::Frontier)
      throw std::runtime_error("nseat trainer flop-deal leaf did not reach the frontier");
  } else if (board_size == 3) {
    // Flop-rooted game: the flop is already dealt. Use the current state.
    frontier_state = state;
  } else {
    throw std::runtime_error("nseat trainer flop-deal leaf with unexpected board size " +
                             std::to_string(board_size));
  }

  const tree::TerminalPayload payload = make_frontier_payload(frontier_state);
  // evaluate() accepts spans, so pass the board and holes directly — no
  // intermediate heap copies.
  const std::vector<double> values =
      ctx.frontier->evaluate(frontier_state.board(), ctx.holes, payload);
  if (values.size() != frontier_state.player_count())
    throw std::runtime_error("nseat trainer frontier evaluator returned the wrong seat count");
  // Chance is externally sampled; own reach is unchanged.
  return values[ctx.traverser];
}

double walk(SweepContext& ctx, const GameState& state, std::size_t node_index, double own_reach) {
  switch (state.phase()) {
    case Phase::Folded:
    case Phase::Showdown:
    case Phase::Frontier:
      return terminal_utility(ctx, state, node_index);
    case Phase::Deal: {
      const TreeNode& node = ctx.tree->node(node_index);
      if (node.is_flop_deal())
        return walk_flop_deal(ctx, state, node_index, own_reach);
      return walk_chance(ctx, state, node_index, own_reach);
    }
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
               NSeatTraversalStreams& streams, const RawTable& profile, DeltaStorage& storage,
               std::atomic<std::size_t>& visits, const NSeatTrainerLimits& limits,
               const gto::FrontierEvaluator* frontier) {
  const MultiwayDeal deal = sample_scalable_joint_deal(ranges, root_board, streams.joint_deal);
  if (deal.hands.size() != ranges.size())
    throw std::runtime_error("nseat trainer joint deal has the wrong seat count");
  // Stack buffer: at most 10 seats.
  std::array<std::array<int, 2>, 10> holes_buf{};
  for (std::size_t i = 0; i < deal.hands.size(); ++i)
    holes_buf[i] = deal.hands[i];
  const std::span<const std::array<int, 2>> holes(holes_buf.data(), deal.hands.size());

  SweepContext ctx;
  ctx.traverser = traverser;
  ctx.tree = &tree;
  ctx.holes = holes;
  ctx.profile = &profile;
  ctx.storage = &storage;
  ctx.visits = &visits;
  ctx.limits = &limits;
  ctx.action_rng = &streams.opponent_action;
  ctx.chance_rng = &streams.chance_card;
  ctx.frontier = frontier;
  ctx.depth = 0;
  // The same deterministic root construction the L3 builder used for node 0.
  const GameState root(tree.def());
  walk(ctx, root, tree.root_index(), 1.0);
}

// No utility evaluation occurs here. Opponent reach never gates averaging.
void walk_average(SweepContext& ctx, const GameState& state, std::size_t index,
                  const std::array<double, poker::kMaxUnifiedSeats>& reach) {
  const auto& node = ctx.tree->node(index);
  if (node.is_terminal() || node.is_flop_deal())
    return;
  DepthGuard guard(ctx);
  if (node.is_chance()) {
    std::array<int, 52> cards{};
    const auto count = legal_runout_cards(state, ctx.holes, cards);
    const int card = cards[bounded_index(*ctx.chance_rng, count)];
    const auto ordinal = board_only_ordinal(state, card);
    if (ordinal >= node.children.size() ||
        ctx.tree->node(node.children[ordinal]).incoming_card != card)
      throw std::runtime_error("nseat average chance ordinal mismatch");
    walk_average(ctx, state.after_card(card), node.children[ordinal], reach);
    return;
  }
  if (!node.is_action() || !state.actor() || node.actor != *state.actor() ||
      node.children.size() != node.actions.size() ||
      node.actions !=
          tree::abstract_node_menu(state, ctx.tree->action_abstraction(), state.legal()))
    throw std::runtime_error("nseat average action projection mismatch");
  const auto actor = *state.actor();
  const NSeatInformationKey key{static_cast<std::uint32_t>(abstraction::card_bucket(
                                    kNSeatCardKind, ctx.holes[actor], state.board())),
                                static_cast<std::uint64_t>(index)};
  auto& delta = touch_row(ctx, key, node.actions);
  const auto sigma = delta.sigma();
  for (std::size_t a = 0; a < node.actions.size(); ++a) {
    delta.sums()[a] += reach[actor] * sigma[a];
    auto child_reach = reach;
    child_reach[actor] *= sigma[a];
    walk_average(ctx, state.after_action(actor, node.actions[a]), node.children[a], child_reach);
  }
}

void run_average(const AbstractTree& tree,
                 const std::vector<std::vector<MultiwayWeightedHand>>& ranges,
                 const std::vector<int>& board, const RawTable& profile, DeltaStorage& storage,
                 std::uint64_t seed, std::uint64_t sample, std::atomic<std::size_t>& visits,
                 const NSeatTrainerLimits& limits) {
  auto deal_rng = derive_nseat_stream(seed, NSeatStreamPurpose::AverageJointDeal, sample, 0);
  auto card_rng = derive_nseat_stream(seed, NSeatStreamPurpose::AverageChanceCard, sample, 0);
  const auto deal = sample_scalable_joint_deal(ranges, board, deal_rng);
  SweepContext ctx;
  ctx.tree = &tree;
  ctx.holes = deal.hands;
  ctx.profile = &profile;
  ctx.storage = &storage;
  ctx.visits = &visits;
  ctx.limits = &limits;
  ctx.chance_rng = &card_rng;
  std::array<double, poker::kMaxUnifiedSeats> reach{};
  std::fill_n(reach.begin(), tree.def().player_count, 1.0);
  walk_average(ctx, GameState(tree.def()), tree.root_index(), reach);
}

// A generation starts only after the whole cohort has been constructed.
// Partial thread creation and worker exceptions therefore cannot strand a
// fixed-count barrier. Every generation rendezvous completes before reduction.
class EpochWorkers {
 public:
  EpochWorkers(std::size_t count, std::function<void(std::size_t)> work)
      : work_(std::move(work)), errors_(count) {
    threads_.reserve(count);
    try {
      for (std::size_t i = 0; i < count; ++i)
        threads_.emplace_back([this, i] { worker(i); });
    } catch (...) {
      stop();
      throw;
    }
  }
  ~EpochWorkers() { stop(); }
  void run() {
    std::unique_lock lock(mutex_);
    std::fill(errors_.begin(), errors_.end(), nullptr);
    remaining_ = threads_.size();
    ++generation_;
    ready_.notify_all();
    finished_.wait(lock, [this] { return remaining_ == 0; });
    for (const auto& error : errors_)
      if (error)
        std::rethrow_exception(error);
  }

 private:
  void stop() {
    {
      std::lock_guard lock(mutex_);
      stop_ = true;
    }
    ready_.notify_all();
    for (auto& thread : threads_)
      if (thread.joinable())
        thread.join();
  }
  void worker(std::size_t index) {
    std::uint64_t seen = 0;
    for (;;) {
      {
        std::unique_lock lock(mutex_);
        ready_.wait(lock, [&] { return stop_ || generation_ != seen; });
        if (stop_)
          return;
        seen = generation_;
      }
      try {
        work_(index);
      } catch (...) {
        errors_[index] = std::current_exception();
      }
      {
        std::lock_guard lock(mutex_);
        if (--remaining_ == 0)
          finished_.notify_one();
      }
    }
  }
  std::function<void(std::size_t)> work_;
  std::vector<std::exception_ptr> errors_;
  std::vector<std::thread> threads_;
  std::mutex mutex_;
  std::condition_variable ready_;
  std::condition_variable finished_;
  std::uint64_t generation_ = 0;
  std::size_t remaining_ = 0;
  bool stop_ = false;
};

// Merge sparse maps with only O(batch width) cursor storage. Each public key
// sums contributions in ascending sample order and is clipped exactly once.
void reduce_batch(RawTable& table, std::span<const std::unique_ptr<DeltaStorage>> stores,
                  std::size_t samples, TrainingBudget& budget) {
  std::vector<DeltaTable::const_iterator> cursors;
  cursors.reserve(samples);
  for (std::size_t i = 0; i < samples; ++i)
    cursors.push_back(stores[i]->rows.begin());
  for (;;) {
    const NSeatInformationKey* next = nullptr;
    for (std::size_t i = 0; i < samples; ++i)
      if (cursors[i] != stores[i]->rows.end() && (!next || cursors[i]->first < *next))
        next = &cursors[i]->first;
    if (!next)
      break;
    const auto key = *next;
    std::array<double, 32> regrets{};
    std::array<double, 32> sums{};
    std::uint64_t own_visits = 0;
    const std::vector<Action>* menu = nullptr;
    for (std::size_t i = 0; i < samples; ++i) {
      if (cursors[i] == stores[i]->rows.end() || cursors[i]->first != key)
        continue;
      const auto& delta = cursors[i]->second;
      if (menu && *menu != *delta.actions)
        throw std::runtime_error("nseat reduction menu mismatch");
      menu = delta.actions;
      if (delta.visits > std::numeric_limits<std::uint64_t>::max() - own_visits)
        throw nseat_training_exhausted("nseat own visit counter overflow");
      own_visits += delta.visits;
      for (std::size_t a = 0; a < menu->size(); ++a) {
        regrets[a] += delta.regrets()[a];
        sums[a] += delta.sums()[a];
      }
      ++cursors[i];
    }
    auto found = table.lower_bound(key);
    if (found == table.end() || found->first != key) {
      budget.reserve_master(row_bytes(menu->size()));
      NSeatRawRow raw;
      raw.actions = *menu;
      raw.regrets.assign(menu->size(), 0.0);
      raw.sums.assign(menu->size(), 0.0);
      found = table.emplace_hint(found, key, std::move(raw));
      budget.published(key);
    }
    auto& raw = found->second;
    if (own_visits > std::numeric_limits<std::uint64_t>::max() - raw.visits)
      throw nseat_training_exhausted("nseat own visit counter overflow");
    raw.visits += own_visits;
    for (std::size_t a = 0; a < menu->size(); ++a) {
      const double regret = raw.regrets[a] + regrets[a] / static_cast<double>(samples);
      const double sum = raw.sums[a] + sums[a] / static_cast<double>(samples);
      if (!std::isfinite(regret) || !std::isfinite(sum) || sum < 0.0)
        throw std::runtime_error("nseat reduction produced invalid accumulators");
      raw.regrets[a] = std::max(0.0, regret);
      raw.sums[a] = sum;
    }
  }
}

NSeatPolicyRow seal_row(const NSeatRawRow& raw) {
  NSeatPolicyRow out;
  out.actions = raw.actions;
  out.visits = raw.visits;
  out.probabilities.resize(raw.actions.size());
  const double total = std::accumulate(raw.sums.begin(), raw.sums.end(), 0.0);
  if (!std::isfinite(total))
    throw std::runtime_error("nseat sealed average total overflowed");
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
  // The trainer accepts the identity action abstraction and any declared-coarse
  // abstraction (RFC 0008 DeclaredOnly menu). The coarse path removes the
  // forced min-bet/jam seeds that explode the abstract tree at deeper stacks,
  // making 10 BB+ library training practical. Custom-schedule identity-style
  // abstractions (same name, different digest) are still refused.
  if (tree.action_id() != abstraction::identity_action_id() &&
      !abstraction::is_declared_coarse_action_id(tree.action_id()))
    throw std::invalid_argument(
        "nseat trainer requires the identity or a declared-coarse action abstraction");
  if (def.board_size < 3 || def.board_size > 5) {
    // RFC 0007 (scope extended by RFC 0010): a flop-terminal preflop root is
    // accepted for 2..10 seats; the frontier evaluator supplies leaf values.
    // GameDef validation enforces the equal-stack single-pot precondition.
    if (def.board_size == 0 && def.preflop && def.terminal == poker::TerminalDepth::Flop) {
      // Flop-terminal preflop root: accepted for 2..10 seats.
    } else {
      throw std::invalid_argument(
          "nseat trainer requires a postflop root (3..5 public cards) or a "
          "flop-terminal preflop root");
    }
  }
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

namespace {

void validate_initial(const AbstractTree& tree, const NSeatInitialState* initial) {
  if (!initial)
    return;
  if (initial->algorithm_revision != kNSeatAlgorithmRevision || initial->batch_size != 1)
    throw std::invalid_argument("nseat warm start requires revision two and batch one");
  for (const auto& [key, raw] : initial->rows) {
    if (key.node_index >= tree.size() || !tree.node(key.node_index).is_action() ||
        raw.actions != tree.node(key.node_index).actions || raw.actions.empty() ||
        raw.actions.size() > 32 || raw.regrets.size() != raw.actions.size() ||
        raw.sums.size() != raw.actions.size())
      throw std::invalid_argument("nseat warm start has invalid row projection");
    for (std::size_t a = 0; a < raw.actions.size(); ++a)
      if (!std::isfinite(raw.regrets[a]) || raw.regrets[a] < 0.0 || !std::isfinite(raw.sums[a]) ||
          raw.sums[a] < 0.0)
        throw std::invalid_argument("nseat warm start has invalid accumulators");
  }
}

NSeatTrainingResult train_impl(const AbstractTree& tree,
                               const std::vector<std::vector<WeightedHand>>& ranges,
                               std::uint64_t iterations, std::uint64_t seed,
                               const NSeatTrainerLimits& limits,
                               const gto::FrontierEvaluator* frontier, std::size_t workers,
                               const NSeatInitialState* initial) {
  validate_request(tree, ranges);
  validate_initial(tree, initial);
  if (iterations == 0 || limits.max_nodes == 0 || limits.max_information_sets == 0 ||
      limits.max_depth == 0 || limits.max_bytes == 0 || limits.max_visits == 0 ||
      limits.wall.count() <= 0)
    throw std::invalid_argument("nseat trainer requires positive samples and resource limits");
  if (tree.def().terminal == poker::TerminalDepth::Flop && !frontier)
    throw std::invalid_argument(
        "nseat trainer: a flop-terminal game requires a frontier evaluator");
  if (tree.size() > limits.max_nodes || tree.depth() > limits.max_depth ||
      tree.accounted_bytes() > limits.max_bytes)
    throw nseat_training_exhausted("nseat trainer tree exceeds structural resource limits");
  workers = std::max(std::size_t{1}, workers);
  if (static_cast<std::uint64_t>(workers) > iterations)
    workers = static_cast<std::size_t>(iterations);
  const std::uint64_t start_sample = initial ? initial->completed_iterations : 0;
  if (iterations > std::numeric_limits<std::uint64_t>::max() - start_sample)
    throw std::invalid_argument("nseat sample index overflow");
  RawTable table;
  TrainingBudget budget(table, limits);
  // Caller-owned inputs are excluded; their copied/converted representations
  // are included. The fixed sampler reservation covers marginal combo,
  // weight/cumulative vectors (including geometric-growth overlap), proposals
  // and stream-string buffers for one simultaneous sample per worker.
  std::size_t combos = 0;
  for (const auto& range : ranges) {
    if (range.size() > limits.max_bytes / 64 ||
        range.size() > std::numeric_limits<std::size_t>::max() - combos)
      throw nseat_training_exhausted("nseat input conversion exceeds the byte cap");
    combos += range.size();
  }
  const auto seats = tree.def().player_count;
  const auto sampler_overhead = seats * 256 + 512;
  if (limits.max_bytes < sampler_overhead || combos > (limits.max_bytes - sampler_overhead) / 64)
    throw nseat_training_exhausted("nseat sampler buffers exceed the byte cap");
  const std::size_t conversion_charge = seats * 256 + combos * 32;
  const std::size_t sampler_charge = sampler_overhead + combos * 64;
  budget.reserve(conversion_charge);
  if (sampler_charge > limits.max_bytes || workers > limits.max_bytes / sampler_charge)
    throw nseat_training_exhausted("nseat concurrent samplers exceed the byte cap");
  budget.reserve(workers * sampler_charge);
  const auto sampler_ranges = to_sampler_ranges(ranges, seats);
  const std::vector<int> board(tree.def().board.begin(),
                               tree.def().board.begin() + tree.def().board_size);
  if (initial) {
    if (initial->rows.size() > limits.max_information_sets)
      throw nseat_training_exhausted("nseat warm start exceeds the information-set cap");
    for (const auto& [key, raw] : initial->rows) {
      budget.reserve_master(row_bytes(raw.actions.size()));
      table.emplace(key, raw);
    }
  }
  constexpr std::size_t kWorkerControlBytes = 512;
  static_assert(sizeof(DeltaStorage) + sizeof(std::unique_ptr<DeltaStorage>) +
                    sizeof(DeltaTable::const_iterator) + sizeof(std::exception_ptr) +
                    sizeof(std::thread) + 64 <=
                kWorkerControlBytes);
  if (workers > (limits.max_bytes / kWorkerControlBytes))
    throw nseat_training_exhausted("nseat worker controls exceed the byte cap");
  budget.reserve(workers * kWorkerControlBytes);
  std::vector<std::unique_ptr<DeltaStorage>> stores;
  stores.reserve(workers);
  for (std::size_t i = 0; i < workers; ++i)
    stores.push_back(std::make_unique<DeltaStorage>(budget));
  std::atomic<std::size_t> visits{0};
  std::uint64_t base_sample = start_sample;
  std::size_t active = 0;
  const auto evaluate = [&](std::size_t slot) {
    if (slot >= active)
      return;
    const auto sample = base_sample + slot;
    for (std::size_t seat = 0; seat < tree.def().player_count; ++seat) {
      NSeatTraversalStreams streams{
          derive_nseat_stream(seed, NSeatStreamPurpose::JointDeal, sample, seat),
          derive_nseat_stream(seed, NSeatStreamPurpose::OpponentAction, sample, seat),
          derive_nseat_stream(seed, NSeatStreamPurpose::ChanceCard, sample, seat)};
      run_sweep(tree, sampler_ranges, board, seat, streams, table, *stores[slot], visits, limits,
                frontier);
    }
    run_average(tree, sampler_ranges, board, table, *stores[slot], seed, sample, visits, limits);
  };
  const auto wall_start = std::chrono::steady_clock::now();
  std::unique_ptr<EpochWorkers> cohort;
  if (workers > 1) {
    budget.reserve(sizeof(EpochWorkers) + sizeof(evaluate) + 128);
    cohort = std::make_unique<EpochWorkers>(workers, evaluate);
  }
  std::uint64_t completed = 0;
  std::uint64_t epochs = initial ? initial->completed_iterations : 0;
  bool wall_reached = false;
  while (completed < iterations) {
    if (std::chrono::steady_clock::now() - wall_start > limits.wall) {
      wall_reached = true;
      break;
    }
    active = static_cast<std::size_t>(std::min<std::uint64_t>(workers, iterations - completed));
    base_sample = start_sample + completed;
    if (cohort)
      cohort->run();
    else
      evaluate(0);
    reduce_batch(table, stores, active, budget);
    for (auto& storage : stores)
      storage->clear();
    completed += active;
    ++epochs;
  }
  // Join even on the success path before publication, not only in RAII cleanup.
  cohort.reset();
  if (completed == 0)
    throw nseat_training_exhausted("nseat trainer completed zero iterations before the wall cap");
  NSeatTrainingResult result;
  result.termination =
      wall_reached ? NSeatTerminationPhase::WallClock : NSeatTerminationPhase::Complete;
  result.completed_iterations = start_sample + completed;
  result.completed_epochs = epochs;
  result.batch_size = workers;
  result.nodes = tree.size();
  result.visits = visits.load(std::memory_order_relaxed);
  result.information_sets = table.size();
  result.seed = seed;
  SplitMix64 witness(
      derive_nseat_stream(seed, NSeatStreamPurpose::JointDeal, 0x5749544e45535300ULL, 0)
          .next_u64());
  for (std::uint64_t i = 0; i < result.completed_iterations; ++i)
    witness.next_u64();
  result.prng_state = witness.next_u64();
  result.algorithm_revision = kNSeatAlgorithmRevision;
  std::map<NSeatInformationKey, NSeatPolicyRow> sealed;
  std::size_t sealed_bytes = 0;
  for (const auto& [key, raw] : table) {
    const auto charge = row_bytes(raw.actions.size());
    budget.reserve(charge);
    sealed_bytes += charge;
    sealed.emplace(key, seal_row(raw));
  }
  result.accounted_bytes = budget.master_bytes() + sealed_bytes;
  result.policy = NSeatPolicy::from_parts(tree.def(), tree.action_id(), std::move(sealed));
  result.raw_rows = std::move(table);
  return result;
}

}  // namespace

NSeatTrainingResult train_nseat(const AbstractTree& tree,
                                const std::vector<std::vector<WeightedHand>>& ranges,
                                std::uint64_t iterations, std::uint64_t seed,
                                const NSeatTrainerLimits& limits,
                                const gto::FrontierEvaluator* frontier,
                                const NSeatInitialState* initial) {
  return train_impl(tree, ranges, iterations, seed, limits, frontier, 1, initial);
}

NSeatTrainingResult train_nseat_parallel(const AbstractTree& tree,
                                         const std::vector<std::vector<WeightedHand>>& ranges,
                                         std::uint64_t iterations, std::uint64_t seed,
                                         const NSeatTrainerLimits& limits,
                                         const gto::FrontierEvaluator* frontier,
                                         std::size_t workers) {
  return train_impl(tree, ranges, iterations, seed, limits, frontier, workers, nullptr);
}

void debug_run_one_nseat_sweep(const AbstractTree& tree,
                               const std::vector<std::vector<WeightedHand>>& ranges,
                               std::size_t traverser, NSeatTraversalStreams& streams,
                               RawTable& rows, const gto::FrontierEvaluator* frontier) {
  validate_request(tree, ranges);
  if (traverser >= tree.def().player_count ||
      (tree.def().terminal == poker::TerminalDepth::Flop && !frontier))
    throw std::invalid_argument("nseat debug sweep invalid traverser or missing frontier");
  const auto sampler_ranges = to_sampler_ranges(ranges, tree.def().player_count);
  const std::vector<int> board(tree.def().board.begin(),
                               tree.def().board.begin() + tree.def().board_size);
  NSeatTrainerLimits limits;
  TrainingBudget budget(rows, limits);
  for (const auto& [key, raw] : rows) {
    (void)key;
    budget.reserve_master(row_bytes(raw.actions.size()));
  }
  auto delta = std::make_unique<DeltaStorage>(budget);
  std::atomic<std::size_t> visits{0};
  run_sweep(tree, sampler_ranges, board, traverser, streams, rows, *delta, visits, limits,
            frontier);
  std::array<std::unique_ptr<DeltaStorage>, 1> stores{std::move(delta)};
  reduce_batch(rows, stores, 1, budget);
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
        abstraction::card_bucket(kNSeatCardKind, own_cards, state.board()));
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
    if (node.kind == NodeKind::TerminalFold || node.kind == NodeKind::TerminalShowdown ||
        node.kind == NodeKind::TerminalFrontier || node.kind == NodeKind::FlopDeal)
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
