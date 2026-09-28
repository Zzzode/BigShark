// stage6/mccfr_trainer.cpp — external-sampling multiplayer MCCFR (RM+ regrets,
// kFull own-reach-weighted average strategy).
//
// One iteration runs one sweep per seat (that seat is the traverser). A sweep
// draws one product-conditional joint hole deal (the RFC 0006 restart sampler
// over all-1326 uniform per-seat ranges) and one three-card root flop from the
// 52-2N residual deck, builds the bucket's reduced rooted-flop GameDef, and
// walks:
//
//   * traverser action node: freeze sigma, enumerate EVERY menu action,
//     recurse into each sampled continuation, accumulate the kFull average
//     sums[a] += own_reach * sigma[a] once per sweep at this own node, and
//     apply the external-sampling RM+ update R[a] = max(0, R[a] + v_a - v).
//     Chance, the joint deal and opponent actions are sampled on the external
//     measure, so the only reach factor propagated is the traverser's own;
//   * opponent action node: touch the row keyed by that seat's own card
//     bucket and sample ONE action with the current policy. The opponent's
//     reach is integrated out: own_reach is unchanged and there is NO average
//     contribution (the traverser does not own this node);
//   * deal node: sample ONE legal public card (board plus ALL 2N hole cards
//     removed) and continue; its edge token is the board-only ordinal;
//   * terminal: exact GameState settle_fold / settle_showdown, zero-sum
//     asserted.
//
// kFull own-reach weighting at the acting player's own node (the full argument
// is at walk_action): per-row the sealed sampled policy is an unbiased
// estimator of the external-measure (pi_own*pi_opponents) full average and
// coincides with the vanilla full average at N=2 / convergence. The enumerable
// three-player parity gate (test_stage6_multiway_average) pins the sampled
// sealed policy against that external-measure reference and against full-tree
// NashConv; it is documented there that it does NOT discriminate the weighting
// on this near-pure fixture (the 1e-9 single-sweep algebra gate does).
//
// Two public-node cursors share one walk rule. Materialized mode (live==2)
// walks the coarse AbstractTree and keys rows by TreeNode index; streaming
// mode (live>=3) builds no tree, recomputes the menu with the shared L3 rule,
// and keys rows by the PublicPath hash. Both modes maintain the PublicPath
// identically along the sampled trajectory (push/pop around enumerated
// traverser branches, advancing push for the sampled opponent action and
// chance card); verify_materialized_path_alignment exhaustively proves the two
// addressing schemes identify the same public nodes.
#include <algorithm>
#include <array>
#include <bs/abstract_tree.hpp>
#include <bs/abstraction.hpp>
#include <bs/game_definition.hpp>
#include <bs/multiway_sampler.hpp>
#include <bs/prng.hpp>
#include <bs/stage6/geometry.hpp>
#include <bs/stage6/infoset_id.hpp>
#include <bs/stage6/public_path.hpp>
#include <bs/stage6/random_streams.hpp>
#include <bs/stage6/trainer.hpp>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <numeric>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace bs::stage6 {

namespace {

namespace poker = bs::poker;
using bs::abstraction::card_bucket;
using bs::tree::AbstractTree;
using bs::tree::TreeLimits;
using poker::Action;
using poker::Chips;
using poker::ContributionSettlement;
using poker::GameDef;
using poker::GameState;
using poker::Phase;

// Representative root flop baked into the materialized-tree build. Tree
// topology is board-value-independent (chance edges enumerate every non-board
// card), so the concrete values only have to be three distinct cards; the
// training sweeps substitute their own sampled flops without rebuilding.
constexpr std::array<int, 3> kTreeBoard = {0, 6, 21};

// --- FNV-1a (file-local; the trainer's config hash never leaves the TU) ------

std::uint64_t fnv1a(const std::string& bytes) {
  std::uint64_t hash = 0xcbf29ce484222325ULL;
  for (unsigned char byte : bytes) {
    hash ^= byte;
    hash *= 0x100000001b3ULL;
  }
  return hash;
}

// --- Sampling primitives (threshold/draw conventions shared with the solver)-

std::size_t bounded_index(bs::SplitMix64& rng, std::size_t bound) {
  if (bound == 0)
    throw std::runtime_error("trainer sampled from an empty list");
  const auto limit = static_cast<std::uint64_t>(bound);
  const std::uint64_t threshold = (std::numeric_limits<std::uint64_t>::max() - limit + 1) % limit;
  for (;;) {
    const std::uint64_t value = rng.next_u64();
    if (value >= threshold)
      return static_cast<std::size_t>(value % limit);
  }
}

// Cumulative unit-draw scan in fixed ascending order; never iterate a map to
// consume randomness.
std::size_t weighted_index(bs::SplitMix64& rng, const std::vector<double>& weights) {
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

// --- Ranges and dealing ------------------------------------------------------

// All 1326 unordered holdings at unit weight, one vector per seat.
std::vector<std::vector<bs::solver::MultiwayWeightedHand>> uniform_ranges(std::size_t seats) {
  std::vector<std::vector<bs::solver::MultiwayWeightedHand>> ranges(seats);
  for (std::size_t s = 0; s < seats; ++s) {
    ranges[s].reserve(1326);
    for (int a = 0; a < 52; ++a)
      for (int b = a + 1; b < 52; ++b)
        ranges[s].push_back(bs::solver::MultiwayWeightedHand{{a, b}, 1.0});
  }
  return ranges;
}

// Resolves the joint-deal support for one training run exactly once (the
// caller keeps the instance and passes a const reference into every sweep;
// never re-resolve per iteration). Nullptr means the production all-1326
// self-build. A non-null carrier is copied out and fully validated: one
// non-empty range per game seat, sorted distinct in-range cards, finite
// strictly-positive weights. A non-null carrier under the Uniform profile is
// the allowed explicit-uniform path (it must be hash-inert there); the only
// profile/carrier mismatch refused is ChartReach without a carrier.
std::vector<std::vector<bs::solver::MultiwayWeightedHand>> resolve_ranges(const TrainingConfig& cfg,
                                                                          std::size_t seats) {
  if (cfg.range_profile == RangeProfile::ChartReach && cfg.root_ranges == nullptr)
    throw std::invalid_argument("chart-reach range profile requires config.root_ranges");
  if (cfg.root_ranges == nullptr)
    return uniform_ranges(seats);
  if (cfg.root_ranges->size() != seats)
    throw std::invalid_argument("root_ranges must contain one range per game seat");
  std::vector<std::vector<bs::solver::MultiwayWeightedHand>> ranges;
  ranges.reserve(seats);
  for (std::size_t s = 0; s < seats; ++s) {
    const std::vector<bs::solver::MultiwayWeightedHand>& source = (*cfg.root_ranges)[s];
    if (source.empty())
      throw std::invalid_argument("root_ranges seat range is empty");
    std::vector<bs::solver::MultiwayWeightedHand> seat;
    seat.reserve(source.size());
    // A duplicated combo would double its mass in the sampler's marginal CDF
    // while the content hash dedupes combos and masked it, so reject any
    // intra-seat repeat outright (current producers emit sorted unique sets).
    std::array<bool, 52 * 52> seen{};
    for (const bs::solver::MultiwayWeightedHand& hand : source) {
      if (hand.cards[0] < 0 || hand.cards[0] >= 52 || hand.cards[1] < 0 || hand.cards[1] >= 52)
        throw std::invalid_argument("root_ranges combo card is outside the deck [0,52)");
      if (hand.cards[0] >= hand.cards[1])
        throw std::invalid_argument(
            "root_ranges combo must carry two distinct sorted cards (a < b)");
      if (!std::isfinite(hand.weight) || hand.weight <= 0.0)
        throw std::invalid_argument(
            "root_ranges combo weight must be finite and strictly positive");
      const std::size_t key =
          static_cast<std::size_t>(hand.cards[0]) * 52 + static_cast<std::size_t>(hand.cards[1]);
      if (seen[key])
        throw std::invalid_argument("root_ranges seat range contains a duplicated combo");
      seen[key] = true;
      seat.push_back(hand);
    }
    ranges.push_back(std::move(seat));
  }
  return ranges;
}

// Three distinct flop cards sampled uniformly without replacement from the
// deck minus every seat's two hole cards (partial Fisher-Yates pop).
std::array<int, 3> sample_root_flop(const std::vector<HoleCards>& holes, bs::SplitMix64& rng) {
  std::array<bool, 52> blocked{};
  for (const HoleCards& h : holes)
    blocked[h[0]] = blocked[h[1]] = true;
  std::vector<int> available;
  available.reserve(52 - holes.size() * 2);
  for (int c = 0; c < 52; ++c)
    if (!blocked[c])
      available.push_back(c);
  std::array<int, 3> flop{};
  for (std::size_t pick = 0; pick < 3; ++pick) {
    const std::size_t index = bounded_index(rng, available.size());
    flop[pick] = available[index];
    available[index] = available.back();
    available.pop_back();
  }
  return flop;
}

// Legal runout cards in ASCENDING order: {0..51} minus the board minus ALL 2N
// hole cards (folded seats' cards stay removed for the whole sweep).
std::vector<int> legal_runout_cards(const GameState& state, const std::vector<HoleCards>& holes) {
  std::array<bool, 52> used{};
  for (int c : state.board())
    used[c] = true;
  for (const HoleCards& h : holes)
    used[h[0]] = used[h[1]] = true;
  std::vector<int> cards;
  for (int c = 0; c < 52; ++c)
    if (!used[c])
      cards.push_back(c);
  return cards;
}

// Board-only ordinal: the card's index in ASCENDING {0..51}\\state.board().
// This is BOTH the tree builder's chance-child position (its chance edges
// enumerate exactly that list) and the streaming chance token, so the two
// cursors align even though the sampled legal list also removes hole cards.
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

// Builds the reduced rooted-flop definition for one bucket + sampled flop: the
// live-count seats, equal reconciled dead contributions, no posted blinds.
GameDef rooted_def_for(const GeometryBucket& bucket, const std::array<int, 3>& flop) {
  const std::size_t live = bucket.key.live_count;
  GameDef def{};
  def.player_count = live;
  def.button = 0;
  def.big_blind = bucket.big_blind;
  def.preflop = false;
  Chips total = 0;
  for (std::size_t p = 0; p < live; ++p) {
    def.stacks[p] = bucket.representative_stacks[p];
    def.contributions[p] = bucket.representative_contrib[p];
    total += bucket.representative_contrib[p];
  }
  if (total != bucket.representative_pot)
    throw std::runtime_error("representative dead contributions do not reconcile to pot");
  def.pot = bucket.representative_pot;
  def.board = {flop[0], flop[1], flop[2], 0, 0};
  def.board_size = 3;
  return def;
}

// The shared coarse menu rule; called at every action node in both modes.
std::vector<Action> coarse_menu(const GameState& state, const TrainingConfig& config) {
  return bs::tree::abstract_node_menu(state, config.action, state.legal());
}

// --- Row store ---------------------------------------------------------------

class RowStore {
 public:
  RowStore(const TrainerLimits& limits, std::map<AbstractInfosetKey, TrainerRow>& rows)
      : limits_(limits), rows_(&rows) {}

  // Touches (creating on first visit) the row for one public node + own card
  // bucket. A re-visited row must present the identical menu or the node
  // addressing has merged two distinct games.
  TrainerRow& touch(const AbstractInfosetKey& key, const std::vector<Action>& menu) {
    auto it = rows_->find(key);
    if (it != rows_->end()) {
      if (it->second.actions != menu) {
        std::ostringstream os;
        os << "trainer row address merged nodes with different menus: key(path=" << std::hex
           << key.path_hash << std::dec << ",node=" << key.tree_node_index
           << ",bucket=" << key.own_card_bucket << ") existing={";
        for (const Action& a : it->second.actions)
          os << static_cast<int>(a.type) << ":" << a.target_total << ",";
        os << "} new={";
        for (const Action& a : menu)
          os << static_cast<int>(a.type) << ":" << a.target_total << ",";
        os << "}";
        throw std::runtime_error(os.str());
      }
      return it->second;
    }
    if (rows_->size() >= limits_.max_information_sets)
      throw stage6_training_exhausted("trainer exceeded the information-set cap");
    const std::size_t charge = row_bytes(menu.size());
    if (charge > limits_.max_bytes - bytes_)
      throw stage6_training_exhausted("trainer exceeded the byte cap");
    TrainerRow row;
    row.actions = menu;
    row.regrets.assign(menu.size(), 0.0);
    row.sums.assign(menu.size(), 0.0);
    bytes_ += charge;
    return rows_->emplace(key, std::move(row)).first->second;
  }

 private:
  static std::size_t row_bytes(std::size_t actions) {
    // Conservative retained charge: map node/key/row overhead plus the three
    // per-action vectors at grown capacity, in the same spirit as the solver's
    // accounting rather than the exact allocator.
    return 512 + actions * (sizeof(Action) + 2 * sizeof(double)) * 4;
  }

  const TrainerLimits& limits_;
  std::map<AbstractInfosetKey, TrainerRow>* rows_;
  std::size_t bytes_ = 0;

  friend std::size_t row_store_bytes(const RowStore&);
};

std::size_t row_store_bytes(const RowStore& store) {
  return store.bytes_;
}

// --- One traverser sweep -----------------------------------------------------

struct SweepContext {
  std::size_t traverser = 0;
  const TrainingConfig* config = nullptr;
  RowStore* store = nullptr;
  const std::vector<HoleCards>* holes = nullptr;
  bs::SplitMix64* action_rng = nullptr;
  bs::SplitMix64* chance_rng = nullptr;
  const AbstractTree* tree = nullptr;  // null in streaming mode
  PublicPath* path = nullptr;
  std::uint64_t depth = 0;
};

// Full-average (kFull) own reach of the ACTING player on the current sweep.
// External sampling samples every non-traverser action and the deal/chance, so
// the only reach factor not integrated out is the traverser's own. At the
// traverser's own nodes the average accumulates own_reach * sigma. A sweep
// reaches the node with probability pi_opponents, so the per-row NORMALIZED
// sampled policy is an unbiased estimator of the external-measure full
// average (pi_own*pi_opponents weighting); it coincides with the vanilla full
// average per-row only when opponent reach is ~{0,1} (convergence) or trivially
// for N=2. An unweighted per-visit snapshot drops the pi_opponents factor and,
// for N>=3 while play is still mixed, carries a history-varying bias.

// Terminal utility for the traverser, asserting exact chip conservation.
double terminal_utility(const SweepContext& ctx, const GameState& state, bool folded) {
  std::vector<std::array<int, 2>> live_holes;
  if (!folded)
    for (std::size_t seat : state.live_players())
      live_holes.push_back((*ctx.holes)[seat]);
  ContributionSettlement settlement =
      folded ? state.settle_fold() : state.settle_showdown(live_holes);
  long long sum = 0;
  for (std::size_t s = 0; s < state.player_count(); ++s)
    sum += settlement.chip_utility[s];
  if (sum != 0)
    throw std::runtime_error("trainer terminal settlement is not zero-sum");
  return static_cast<double>(settlement.chip_utility[ctx.traverser]);
}

double walk(SweepContext& ctx, const GameState& state, std::size_t node_index, double own_reach);

void check_depth(SweepContext& ctx) {
  if (ctx.depth > ctx.config->limits.max_depth)
    throw stage6_training_exhausted("trainer exceeded the depth cap");
}

// RAII frame depth: a throw on any branch must not leave a stale depth, since
// the same context never crosses sweeps but a future reuse must start clean.
struct DepthGuard {
  SweepContext& ctx;
  DepthGuard(SweepContext& context) : ctx(context) {
    ++ctx.depth;
    check_depth(ctx);
  }
  ~DepthGuard() { --ctx.depth; }
  DepthGuard(const DepthGuard&) = delete;
  DepthGuard& operator=(const DepthGuard&) = delete;
};

double walk_action(SweepContext& ctx, const GameState& state, const std::vector<Action>& menu,
                   std::size_t node_index, double own_reach) {
  DepthGuard frame(ctx);
  const bool materialized = ctx.tree != nullptr;
  const std::size_t actor = *state.actor();
  const std::vector<int> board(state.board().begin(), state.board().end());
  const std::uint32_t own_bucket =
      static_cast<std::uint32_t>(card_bucket(ctx.config->card_kind, (*ctx.holes)[actor], board));

  AbstractInfosetKey key;
  key.own_card_bucket = own_bucket;
  if (materialized)
    // Node index abstracts away the concrete board value: chance child
    // positions are board-only ordinals, so the same abstract node reached on
    // a different sampled flop pools into one row. The alignment proof pins
    // node<->path injectivity on any representative board.
    key.tree_node_index = node_index;
  else
    // Streaming rows carry concrete public cards in the path (own holdings are
    // still bucketed), by design: no tree is built at live>=3.
    key.path_hash = ctx.path->hash();
  TrainerRow& row = ctx.store->touch(key, menu);
  const std::vector<double> sigma = row.current_strategy();

  if (actor == ctx.traverser) {
    // Full-average update AT THE ACTING PLAYER'S OWN NODE, once per sweep,
    // weighted by this sweep's own reach to the node (kFull semantics). This
    // is the unbiased CFR average for every player count. visits counts own
    // node touches separately for maturity instrumentation; it is not the
    // average weight.
    for (std::size_t a = 0; a < menu.size(); ++a)
      row.sums[a] += ctx.config->average_weighting == AverageWeighting::PerVisit
                         ? sigma[a]
                         : own_reach * sigma[a];
    ++row.visits;

    // Enumerate every action over its sampled external continuation, then the
    // unweighted RM+ regret update on return. The traverser's own reach into a
    // child multiplies by the probability of taking that child action. The
    // path push/pop brackets each hypothetical branch; row references stay
    // valid across map insertion.
    std::vector<double> children(menu.size(), 0.0);
    for (std::size_t a = 0; a < menu.size(); ++a) {
      GameState next = state.after_action(actor, menu[a]);
      const std::size_t prefix = ctx.path->tokens().size();
      ctx.path->on_action(actor, a);
      const std::size_t child_node =
          materialized ? ctx.tree->node(node_index).children.at(a) : bs::tree::kNoNode;
      children[a] = walk(ctx, next, child_node, own_reach * sigma[a]);
      // The sampled continuation advanced its own permanent edges; restore
      // the full prefix before enumerating the next action.
      while (ctx.path->tokens().size() > prefix)
        ctx.path->pop();
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
  // NO average contribution here (the traverser never owns this node). The
  // sampled branch advances the path permanently in both cursor modes.
  const std::size_t sampled = weighted_index(*ctx.action_rng, sigma);
  GameState next = state.after_action(actor, menu[sampled]);
  ctx.path->on_action(actor, sampled);
  const std::size_t child_node =
      materialized ? ctx.tree->node(node_index).children.at(sampled) : bs::tree::kNoNode;
  return walk(ctx, next, child_node, own_reach);
}

double walk_deal(SweepContext& ctx, const GameState& state, std::size_t node_index,
                 double own_reach) {
  DepthGuard frame(ctx);
  const bool materialized = ctx.tree != nullptr;
  const std::vector<int> cards = legal_runout_cards(state, *ctx.holes);
  const std::size_t pick = bounded_index(*ctx.chance_rng, cards.size());
  const int card = cards[pick];
  const std::size_t ordinal = board_only_ordinal(state, card);
  GameState next = state.after_card(card);
  std::size_t child_node = bs::tree::kNoNode;
  if (materialized) {
    // The tree's chance children enumerate {0..51}\\board ascending, so the
    // child position is exactly this card's board-only ordinal.
    child_node = ctx.tree->node(node_index).children.at(ordinal);
  } else {
    ctx.path->on_chance(ordinal);
  }
  // Chance is externally sampled; own reach is unchanged.
  return walk(ctx, next, child_node, own_reach);
}

double walk(SweepContext& ctx, const GameState& state, std::size_t node_index, double own_reach) {
  const bool materialized = ctx.tree != nullptr;
  switch (state.phase()) {
    case Phase::Folded:
      return terminal_utility(ctx, state, true);
    case Phase::Showdown:
      return terminal_utility(ctx, state, false);
    case Phase::Deal:
      return walk_deal(ctx, state, node_index, own_reach);
    case Phase::Action: {
      if (materialized) {
        const auto& node = ctx.tree->node(node_index);
        if (!node.is_action())
          throw std::runtime_error("trainer materialized walk desynced: expected action node");
        const std::vector<Action> menu = coarse_menu(state, *ctx.config);
        if (menu != node.actions)
          throw std::runtime_error("trainer materialized menu drifted from the abstract tree");
        return walk_action(ctx, state, menu, node_index, own_reach);
      }
      return walk_action(ctx, state, coarse_menu(state, *ctx.config), node_index, own_reach);
    }
  }
  throw std::runtime_error("trainer reached an unrecognized game phase");
}

// Content stamp for one trained artifact: FNV over the GAME identity surfaces
// that must agree for a row address to mean the same game across processes
// (action/card abstraction, geometry matrix, bucket, chart digest). Training
// PROVENANCE (iteration count, master seed, rows hash) is the FrozenManifest;
// a longer run on the same game seals the same-keyed rows with different
// probabilities, and the manifest/rows hash distinguishes the artifacts.
std::uint64_t artifact_identity_hash(const GeometryBucket& bucket, const TrainingConfig& config) {
  std::ostringstream os;
  os << config.action.id().digest << '|'
     << bs::abstraction::card_abstraction_id(config.card_kind).digest << '|'
     << config.geometry_matrix_hash << '|' << geometry_bucket_token(bucket.key) << '|'
     << config.chart_digest_sha256;
  // range_profile is deliberately INERT for Uniform (even with an explicit
  // all-1326 carrier and a nonzero range_content_hash) so every existing
  // uniform artifact keeps its byte-identical identity. Under chart-reach the
  // marker + content hash make the conditioned game explicit; the
  // geometry_matrix_hash already differs there because the driver feeds the
  // chart-only matrix hash.
  if (config.range_profile == RangeProfile::ChartReach)
    os << '|' << "chart-reach" << '|' << config.range_content_hash;
  return fnv1a(os.str());
}

// Seals trained rows into the frozen average-policy map in canonical key
// order, stamping each key with the artifact identity. A row that never
// received an average contribution freezes to uniform (average_row() already
// implements that fallback).
FrozenArtifactRows seal_rows(const std::map<AbstractInfosetKey, TrainerRow>& trained,
                             std::uint64_t artifact_hash) {
  FrozenArtifactRows out;
  for (const auto& [in_key, row] : trained) {
    AbstractInfosetKey key = in_key;
    key.artifact_content_hash = artifact_hash;
    out.emplace(std::move(key), row.average_row());
  }
  return out;
}

std::uint64_t training_config_hash(const TrainingConfig& config) {
  std::ostringstream os;
  os << config.action.id().name << '|' << config.action.id().version << '|'
     << config.action.id().digest << '|'
     << bs::abstraction::card_abstraction_id(config.card_kind).digest << '|'
     << config.geometry_matrix_hash << '|' << config.chart_digest_sha256 << '|' << config.iterations
     << '|' << config.master_seed;
  // See artifact_identity_hash: the range fields are hash-inert under Uniform
  // and appended only for the chart-reach conditioned game.
  if (config.range_profile == RangeProfile::ChartReach)
    os << '|' << "chart-reach" << '|' << config.range_content_hash;
  return fnv1a(os.str());
}

// Exhaustive cursor-alignment proof over one materialized tree. Replays every
// tree edge with the real GameState transition while maintaining a PublicPath
// exactly as the walk does, and records the path hash at every action node.
// Distinct action nodes must hash distinctly (path tokens are lossless over
// the edge alphabet) and chance-child positions must equal the board-only
// ordinals. Iterative to avoid any native-stack dependence.
struct AlignFrame {
  GameState state;
  std::size_t node = 0;
  // Full edge-token vector on the frame's branch (the path minus the
  // four-token geometry prefix). Snapshotted per frame: with a LIFO work stack
  // the shared PublicPath must be rebuilt on pop, never merely truncated,
  // because the enqueue-time sibling edge token is no longer on the path.
  std::vector<std::uint16_t> edges;
  std::size_t depth = 0;
};

// Runs one external-sampling traverser sweep against an external row table.
// The deal/flop RNGs produce this sweep's world; action/chance RNGs drive the
// sampled external measure. Shared by train_bucket and debug_run_one_sweep.
void run_sweep(const GeometryBucket& bucket, const TrainingConfig& config, const AbstractTree* tree,
               std::size_t traverser,
               const std::vector<std::vector<bs::solver::MultiwayWeightedHand>>& ranges,
               std::uint64_t geometry_token, bs::SplitMix64& deal_rng, bs::SplitMix64& flop_rng,
               bs::SplitMix64& action_rng, bs::SplitMix64& chance_rng, RowStore& store) {
  const bs::solver::MultiwayDeal deal =
      bs::solver::sample_scalable_joint_deal(ranges, {}, deal_rng);
  std::vector<HoleCards> holes;
  holes.reserve(bucket.key.live_count);
  for (const auto& hand : deal.hands)
    holes.push_back(HoleCards{hand[0], hand[1]});
  const std::array<int, 3> flop = sample_root_flop(holes, flop_rng);

  PublicPath path(geometry_token);
  SweepContext ctx;
  ctx.traverser = traverser;
  ctx.config = &config;
  ctx.store = &store;
  ctx.holes = &holes;
  ctx.action_rng = &action_rng;
  ctx.chance_rng = &chance_rng;
  ctx.tree = tree;
  ctx.path = &path;
  ctx.depth = 0;
  GameState state(rooted_def_for(bucket, flop));
  walk(ctx, state, tree ? tree->root_index() : bs::tree::kNoNode, 1.0);
}

}  // namespace

std::vector<double> TrainerRow::current_strategy() const {
  std::vector<double> sigma(actions.size(), 0.0);
  double positive_sum = 0.0;
  for (double r : regrets) {
    if (!std::isfinite(r))
      throw std::runtime_error("trainer row carries a non-finite regret");
    positive_sum += std::max(0.0, r);
  }
  if (positive_sum > 0.0) {
    for (std::size_t i = 0; i < actions.size(); ++i)
      sigma[i] = std::max(0.0, regrets[i]) / positive_sum;
  } else {
    for (double& p : sigma)
      p = 1.0 / static_cast<double>(actions.size());
  }
  return sigma;
}

AbstractPolicyRow TrainerRow::average_row() const {
  AbstractPolicyRow out;
  out.abstract_actions = actions;
  out.visits = visits;
  out.probabilities.resize(actions.size());
  const double total = std::accumulate(sums.begin(), sums.end(), 0.0);
  if (total > 0.0) {
    for (std::size_t i = 0; i < actions.size(); ++i)
      out.probabilities[i] = sums[i] / total;
  } else {
    for (double& p : out.probabilities)
      p = 1.0 / static_cast<double>(actions.size());
  }
  return out;
}

TrainerMode materialize_or_stream(const GeometryBucket& bucket) {
  return bucket.key.live_count == 2 ? TrainerMode::Materialized : TrainerMode::Streaming;
}

AlignmentReport debug_verify_tree_paths(const AbstractTree& tree, const GameDef& def,
                                        const TrainingConfig& config,
                                        std::uint64_t geometry_token) {
  AlignmentReport report;
  std::set<std::uint64_t> action_hashes;

  PublicPath path(geometry_token);
  std::vector<AlignFrame> work;
  work.push_back({GameState(def), tree.root_index(), {}, 0});

  while (!work.empty()) {
    AlignFrame frame = std::move(work.back());
    work.pop_back();
    // Rebuild the full path from this frame's snapshotted edge vector.
    path.reset(geometry_token);
    for (std::uint16_t token : frame.edges)
      path.append_edge_token(token);
    const GameState& state = frame.state;
    const std::size_t node_index = frame.node;
    const auto& node = tree.node(node_index);

    if (node.is_terminal()) {
      ++report.terminal_nodes;
      if (state.phase() != Phase::Folded && state.phase() != Phase::Showdown)
        throw std::runtime_error("path alignment: terminal node maps to a non-terminal state");
      continue;
    }
    if (node.is_chance()) {
      ++report.chance_nodes;
      if (state.phase() != Phase::Deal)
        throw std::runtime_error("path alignment: chance node maps to a non-deal state");
      const std::vector<int> cards = bs::tree::public_runout_cards(state);
      if (cards.size() != node.children.size())
        throw std::runtime_error("path alignment: chance child count mismatch");
      for (std::size_t i = 0; i < cards.size(); ++i) {
        if (board_only_ordinal(state, cards[i]) != i)
          throw std::runtime_error("path alignment: chance child is not at its board ordinal");
        std::vector<std::uint16_t> child_edges = frame.edges;
        child_edges.push_back(static_cast<std::uint16_t>(0x8000u | i));
        work.push_back({state.after_card(cards[i]), node.children[i], std::move(child_edges),
                        frame.depth + 1});
      }
      continue;
    }
    // Action node.
    ++report.action_nodes;
    if (state.phase() != Phase::Action)
      throw std::runtime_error("path alignment: action node maps to a non-action state");
    if (*state.actor() != node.actor)
      throw std::runtime_error("path alignment: acting seat mismatch");
    const std::vector<Action> menu = coarse_menu(state, config);
    if (menu != node.actions)
      throw std::runtime_error("path alignment: shared menu rule drifted from the tree menu");
    if (!action_hashes.insert(path.hash()).second)
      throw std::runtime_error("path alignment: two action nodes share a path hash");

    const std::size_t seat = *state.actor();
    if (seat >= 10 || node.actions.size() > 32)
      throw std::runtime_error("path alignment: action token out of range");
    for (std::size_t a = 0; a < node.actions.size(); ++a) {
      std::vector<std::uint16_t> child_edges = frame.edges;
      child_edges.push_back(static_cast<std::uint16_t>((seat << 5) | a));
      work.push_back({state.after_action(seat, node.actions[a]), node.children[a],
                      std::move(child_edges), frame.depth + 1});
    }
  }
  report.distinct_action_paths = action_hashes.size();
  if (report.distinct_action_paths != report.action_nodes)
    throw std::runtime_error("path alignment: not every action node had a distinct path");
  return report;
}

AlignmentReport verify_materialized_path_alignment(const GeometryBucket& bucket,
                                                   const TrainingConfig& config) {
  if (bucket.key.live_count != 2)
    throw std::invalid_argument("path alignment is proven on a live==2 representative");
  const GameDef def = rooted_def_for(bucket, kTreeBoard);
  TreeLimits limits;
  limits.max_depth = config.limits.max_depth;
  limits.max_bytes = config.limits.max_bytes;
  AbstractTree tree(def, config.action, limits);
  return debug_verify_tree_paths(tree, def, config, geometry_bucket_token(bucket.key));
}

BucketTrainingResult train_bucket(const GeometryBucket& bucket, const TrainingConfig& config) {
  if (!bucket.actionable)
    throw std::invalid_argument("train_bucket called on an all-in-at-flop runout bucket");
  if (bucket.key.live_count < 2 || bucket.key.live_count > 10)
    throw std::invalid_argument("train_bucket supports 2..10 live seats");
  if (config.iterations == 0)
    throw std::invalid_argument("train_bucket requires a positive iteration count");
  if (bucket.big_blind == 0)
    throw std::invalid_argument("train_bucket bucket is missing its big-blind denomination");
  if (bucket.representative_stacks.size() != bucket.key.live_count ||
      bucket.representative_contrib.size() != bucket.key.live_count)
    throw std::invalid_argument("bucket representative inputs do not match the live seat count");

  const TrainerMode mode = materialize_or_stream(bucket);
  const std::size_t seats = bucket.key.live_count;
  std::map<AbstractInfosetKey, TrainerRow> table;
  RowStore store(config.limits, table);  // diagnostics off in production

  // Materialized mode builds ONE coarse tree on representative chips. Sweeps
  // substitute sampled flops; node indices are board-value-independent.
  std::unique_ptr<AbstractTree> tree;
  if (mode == TrainerMode::Materialized) {
    const GameDef tree_def = rooted_def_for(bucket, kTreeBoard);
    TreeLimits tree_limits;
    tree_limits.max_depth = config.limits.max_depth;
    tree_limits.max_bytes = config.limits.max_bytes;
    tree = std::make_unique<AbstractTree>(tree_def, config.action, tree_limits);
  }
  const std::uint64_t geometry_token = geometry_bucket_token(bucket.key);
  // Resolved once per training run; every sweep of the iteration loop shares
  // this one const instance.
  const auto ranges = resolve_ranges(config, seats);

  const auto wall_start = std::chrono::steady_clock::now();
  bool wall_reached = false;
  std::uint64_t completed = 0;
  // Witness RNG advanced exactly once per fully completed iteration, so the
  // report stamps how much deterministic work finished.
  bs::SplitMix64 witness(derive_stream(config.master_seed, StreamPurpose::TrainJointDeal,
                                       0x5749544e45535300ULL, geometry_token)
                             .next_u64());

  for (std::uint64_t iter = 0; iter < config.iterations; ++iter) {
    if ((iter & 0xffULL) == 0) {
      const auto elapsed = std::chrono::steady_clock::now() - wall_start;
      if (elapsed > config.limits.wall) {
        wall_reached = true;
        break;
      }
    }
    for (std::size_t traverser = 0; traverser < seats; ++traverser) {
      bs::SplitMix64 deal_rng =
          derive_stream(config.master_seed, StreamPurpose::TrainJointDeal, iter, traverser);
      bs::SplitMix64 flop_rng =
          derive_stream(config.master_seed, StreamPurpose::TrainRootBoard, iter, traverser);
      bs::SplitMix64 action_rng =
          derive_stream(config.master_seed, StreamPurpose::TrainOpponentAction, iter, traverser);
      bs::SplitMix64 chance_rng =
          derive_stream(config.master_seed, StreamPurpose::TrainBoardRunout, iter, traverser);
      run_sweep(bucket, config, tree.get(), traverser, ranges, geometry_token, deal_rng, flop_rng,
                action_rng, chance_rng, store);
    }
    ++completed;
    witness.next_u64();
  }

  // A wall cap that let zero iterations finish leaves no promotable artifact;
  // refuse rather than seal an empty result whose manifest would claim the
  // requested iteration count.
  if (completed == 0)
    throw stage6_training_exhausted(
        "trainer completed zero iterations before the wall cap; no artifact to seal");

  BucketTrainingResult result;
  result.key = bucket.key;
  result.mode = mode;
  result.artifact_content_hash = artifact_identity_hash(bucket, config);
  result.rows = seal_rows(table, result.artifact_content_hash);
  result.report.key = bucket.key;
  result.report.mode = mode;
  result.report.information_sets = table.size();
  result.report.retained_bytes = row_store_bytes(store);
  result.report.iterations_completed = completed;
  result.report.prng_final_state = witness.next_u64();
  result.report.wall_reached = wall_reached;
  return result;
}

void debug_run_one_sweep(const GeometryBucket& bucket, const TrainingConfig& config,
                         const AbstractTree* tree, std::size_t traverser, bs::SplitMix64& deal_rng,
                         bs::SplitMix64& flop_rng, bs::SplitMix64& action_rng,
                         bs::SplitMix64& chance_rng,
                         std::map<AbstractInfosetKey, TrainerRow>& rows) {
  if (traverser >= bucket.key.live_count)
    throw std::invalid_argument("debug sweep traverser is not a live seat");
  const bool want_tree = bucket.key.live_count == 2;
  if (want_tree != (tree != nullptr))
    throw std::invalid_argument("debug sweep tree presence must match the bucket cursor mode");
  const std::uint64_t geometry_token = geometry_bucket_token(bucket.key);
  const auto ranges = resolve_ranges(config, bucket.key.live_count);
  RowStore store(config.limits, rows);
  run_sweep(bucket, config, tree, traverser, ranges, geometry_token, deal_rng, flop_rng, action_rng,
            chance_rng, store);
}

void debug_run_fixed_world_sweep(const TrainingConfig& config, const GameDef& root,
                                 std::uint64_t geometry_token, const std::vector<HoleCards>& holes,
                                 std::size_t traverser, bs::SplitMix64& action_rng,
                                 bs::SplitMix64& chance_rng,
                                 std::map<AbstractInfosetKey, TrainerRow>& rows) {
  if (root.player_count < 3 || root.player_count > 10)
    throw std::invalid_argument("fixed-world sweep is a streaming (3..10 seat) seam");
  if (holes.size() != root.player_count)
    throw std::invalid_argument("fixed-world sweep needs one hole pair per seat");
  if (traverser >= root.player_count)
    throw std::invalid_argument("fixed-world sweep traverser is not a live seat");
  std::array<bool, 52> used{};
  for (int c : root.board)
    used[c] = true;
  for (const HoleCards& h : holes) {
    if (h[0] == h[1] || used[h[0]] || used[h[1]])
      throw std::invalid_argument("fixed-world sweep deal conflicts with itself or the board");
    used[h[0]] = used[h[1]] = true;
  }
  RowStore store(config.limits, rows);
  PublicPath path(geometry_token);
  SweepContext ctx;
  ctx.traverser = traverser;
  ctx.config = &config;
  ctx.store = &store;
  ctx.holes = &holes;
  ctx.action_rng = &action_rng;
  ctx.chance_rng = &chance_rng;
  ctx.tree = nullptr;
  ctx.path = &path;
  ctx.depth = 0;
  GameState state(root);
  walk(ctx, state, bs::tree::kNoNode, 1.0);
}

FrozenManifest manifest_for(const GeometryBucket& bucket, const TrainingConfig& config,
                            const FrozenArtifactRows& rows, std::uint64_t iterations_completed) {
  FrozenManifest m;
  m.action_abstraction = config.action.id();
  m.card_abstraction = bs::abstraction::card_abstraction_id(config.card_kind);
  m.translator = nearest_target_translator_id();
  m.geometry_matrix_hash = config.geometry_matrix_hash;
  m.bucket = bucket.key;
  m.chart_digest_sha256 = config.chart_digest_sha256;
  m.training_config_hash = training_config_hash(config);
  // The artifact identity stamp must match what seal_rows wrote into every key
  // and what the reader addresses by; recompute it from the same identity
  // inputs rather than trusting a caller-supplied value.
  m.artifact_content_hash = artifact_identity_hash(bucket, config);
  m.iterations = config.iterations;
  m.iterations_completed = iterations_completed;
  m.master_seed = config.master_seed;
  m.rows_content_hash = hash_artifact_rows(rows);
  return m;
}

}  // namespace bs::stage6
