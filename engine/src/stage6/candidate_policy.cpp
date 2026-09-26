// stage6/candidate_policy.cpp — composed candidate implementation.
//
// Charts preflop (shared chart_preflop helper); postflop the candidate replays
// the hand onto a reduced representative shadow game, locates the sealed
// average row (TreeNode index for live==2, PublicPath hash for live>=3), and
// projects its representative-chip coarse distribution onto the concrete legal
// actions through the R7 translator. See candidate_policy.hpp for the binding
// and totality contract.
#include <array>
#include <bs/abstract_tree.hpp>
#include <bs/abstraction.hpp>
#include <bs/eval.hpp>
#include <bs/game_definition.hpp>
#include <bs/stage6/candidate_policy.hpp>
#include <bs/stage6/chart_preflop.hpp>
#include <bs/stage6/public_path.hpp>
#include <bs/stage6/translator.hpp>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace bs::stage6 {

namespace {

namespace poker = bs::poker;
using bs::abstraction::card_bucket;
using bs::abstraction::CardBucketKind;

constexpr std::array<int, 3> kTreeBoard = {0, 6, 21};

// One installed binding plus its cached materialized coarse tree (live==2).
struct Installed {
  CandidateBinding binding;
  CardBucketKind card_kind = CardBucketKind::CategoryTiersV1;
  std::unique_ptr<bs::tree::AbstractTree> tree;
};

CardBucketKind card_kind_from_id(const bs::abstraction::AbstractionId& id) {
  if (id == bs::abstraction::card_abstraction_id(CardBucketKind::CategoryTiersV1))
    return CardBucketKind::CategoryTiersV1;
  if (id == bs::abstraction::card_abstraction_id(CardBucketKind::Identity))
    return CardBucketKind::Identity;
  throw stage6_candidate_error("candidate binding names an unknown card abstraction: " +
                               id.to_string());
}

// Builds the reduced representative rooted-flop def for one binding and flop.
poker::GameDef reduced_def(const GeometryBucket& bucket, const std::array<int, 3>& flop) {
  const std::size_t live = bucket.key.live_count;
  poker::GameDef def{};
  def.player_count = live;
  def.button = 0;
  def.big_blind = bucket.big_blind;
  def.preflop = false;
  poker::Chips total = 0;
  for (std::size_t i = 0; i < live; ++i) {
    def.stacks[i] = bucket.representative_stacks[i];
    def.contributions[i] = bucket.representative_contrib[i];
    total += bucket.representative_contrib[i];
  }
  if (total != bucket.representative_pot)
    throw stage6_candidate_error("binding representative pot does not reconcile");
  def.pot = bucket.representative_pot;
  def.board = {flop[0], flop[1], flop[2], 0, 0};
  def.board_size = 3;
  return def;
}

// Board-only ordinal, identical to the trainer/tree convention.
std::size_t board_only_ordinal(const poker::GameState& shadow, int card) {
  std::size_t ordinal = 0;
  for (int c = 0; c < card; ++c) {
    bool on_board = false;
    for (int b : shadow.board())
      if (b == c)
        on_board = true;
    if (!on_board)
      ++ordinal;
  }
  return ordinal;
}

// Coarse menu at one shadow node: the cached tree's stored actions (materialized
// binding) or the shared L3 rule recomputed on the fly (streaming binding).
std::vector<poker::Action> shadow_menu(const Installed& installed, const poker::GameState& shadow,
                                       std::size_t node) {
  if (installed.tree)
    return installed.tree->node(node).actions;
  return bs::tree::abstract_node_menu(shadow, installed.binding.action, shadow.legal());
}

// True when `def` IS the reduced representative rooted game of `bucket` (modulo
// the concrete board values, which tree topology ignores). The reduced pot is
// the equal sum of the live seats' contributions and intentionally does NOT
// round-trip to the bucket's pot_bb (folded-seat dead money is dropped by
// Revision 3), so a representative game must be recognized by identity rather
// than re-bucketed from its chips.
bool def_is_representative(const GeometryBucket& bucket, const poker::GameDef& def) {
  if (def.board_size < 3 || def.player_count != bucket.key.live_count)
    return false;
  for (std::size_t i = 0; i < bucket.key.live_count; ++i) {
    if (def.stacks[i] != bucket.representative_stacks[i])
      return false;
    if (def.contributions[i] != bucket.representative_contrib[i])
      return false;
  }
  return def.pot == bucket.representative_pot;
}

}  // namespace

struct CandidateBehaviorPolicy::Impl {
  // GeometryBucketKey::to_string() -> installed binding.
  std::unordered_map<std::string, std::unique_ptr<Installed>> bindings;
  CandidateMissPolicy miss_policy = CandidateMissPolicy::Throw;
  mutable std::uint64_t unvisited_misses = 0;

  Installed* find(const GeometryBucketKey& key) const {
    auto it = bindings.find(key.to_string());
    return it == bindings.end() ? nullptr : it->second.get();
  }
};

CandidateBehaviorPolicy::CandidateBehaviorPolicy(std::vector<CandidateBinding> supplied,
                                                 CandidateMissPolicy miss_policy)
    : impl_(std::make_unique<Impl>()) {
  impl_->miss_policy = miss_policy;
  for (CandidateBinding& in : supplied) {
    auto installed = std::make_unique<Installed>();
    installed->binding = std::move(in);
    const GeometryBucket& b = installed->binding.bucket;
    if (!b.actionable)
      throw stage6_candidate_error("candidate binding for " + b.key.to_string() +
                                   " is an all-in-at-flop runout bucket");
    if (b.big_blind == 0)
      throw stage6_candidate_error("candidate binding is missing its big blind");
    if (b.representative_stacks.size() != b.key.live_count ||
        b.representative_contrib.size() != b.key.live_count)
      throw stage6_candidate_error("candidate binding representative inputs do not match");
    const FrozenManifest& manifest = installed->binding.manifest;
    if (manifest.bucket != b.key)
      throw stage6_candidate_error("candidate manifest bucket disagrees with its binding");
    if (manifest.action_abstraction != installed->binding.action.id())
      throw stage6_candidate_error(
          "candidate binding action abstraction disagrees with the frozen manifest");
    installed->card_kind = card_kind_from_id(manifest.card_abstraction);

    if (b.key.live_count == 2) {
      const poker::GameDef tree_def = reduced_def(b, kTreeBoard);
      installed->tree =
          std::make_unique<bs::tree::AbstractTree>(tree_def, installed->binding.action);
    }
    const std::string map_key = b.key.to_string();
    if (impl_->bindings.count(map_key))
      throw stage6_candidate_error("duplicate candidate binding for bucket " + map_key);
    impl_->bindings.emplace(map_key, std::move(installed));
  }
}

CandidateBehaviorPolicy::~CandidateBehaviorPolicy() = default;

std::size_t CandidateBehaviorPolicy::binding_count() const noexcept {
  return impl_->bindings.size();
}

std::uint64_t CandidateBehaviorPolicy::unvisited_misses() const noexcept {
  return impl_->unvisited_misses;
}

std::vector<PolicyAction> CandidateBehaviorPolicy::distribution(
    const poker::GameState& state, std::size_t seat, HoleCards hole,
    const PolicyContext& context) const {
  // Preflop: exactly the pinned chart decision.
  if (is_preflop_state(state)) {
    const poker::Action action = pinned_chart_action(state, seat, hole, context);
    return std::vector<PolicyAction>{PolicyAction{action, 1.0}};
  }
  if (!context.hand_log)
    throw stage6_candidate_error("the candidate requires the simulator's hand log postflop");
  const HandLog& log = *context.hand_log;

  // ---- Bind to one installed artifact. Two entry shapes:
  //   * a REDUCED REPRESENTATIVE rooted-flop GameDef (R11): its stacks,
  //     contributions and pot match a binding's representative inputs exactly.
  //     The reduced pot drops folded-seat dead money, so it must NOT be
  //     re-bucketed from chips; match by representative identity and treat all
  //     def seats as the flop-live set in their natural order.
  //   * a full-hand preflop GameDef (the simulator): replay the observed
  //     preflop actions to the flop deal and bind by the real flop signature's
  //     bucket key.
  Installed* installed = nullptr;
  std::vector<std::size_t> live_seats;
  bool representative_root = false;
  if (state.def().board_size >= 3) {
    for (auto& [map_key, candidate_installed] : impl_->bindings) {
      if (def_is_representative(candidate_installed->binding.bucket, state.def())) {
        installed = candidate_installed.get();
        break;
      }
    }
    if (!installed)
      throw stage6_candidate_error(
          "no frozen candidate artifact for the reduced representative rooted game");
    representative_root = true;
    for (std::size_t i = 0; i < state.def().player_count; ++i)
      live_seats.push_back(i);
  } else {
    poker::GameState pre(state.def());
    for (const LoggedAction& a : log.preflop)
      pre = pre.after_action(a.seat, a.action);
    if (pre.phase() != poker::Phase::Deal)
      throw stage6_candidate_error("candidate could not replay the preflop close to a flop deal");
    poker::GameState at_flop = pre;
    for (std::size_t i = 0; i < 3; ++i)
      at_flop = at_flop.after_card(state.board()[i]);
    const GeometrySignature signature = flop_signature(at_flop);
    const GeometryBucketKey key = bucket_key_for(signature, state.def().big_blind);
    installed = impl_->find(key);
    if (!installed)
      throw stage6_candidate_error("no frozen candidate artifact for flop geometry " +
                                   key.to_string());
    live_seats = signature.live;
  }

  const CandidateBinding& binding = installed->binding;
  const GeometryBucket& bucket = binding.bucket;
  const std::size_t live_count = bucket.key.live_count;
  if (live_seats.size() != live_count)
    throw stage6_candidate_error("flop live count disagrees with the bound bucket");

  // Original flop-live seat -> reduced representative seat. In a representative
  // rooted game the def seats ARE the live seats in order; in a full-hand game
  // the live set is the signature's ascending live order (live_seats).
  std::array<int, 10> original_to_reduced{};
  original_to_reduced.fill(-1);
  for (std::size_t i = 0; i < live_count; ++i)
    original_to_reduced[live_seats[i]] = static_cast<int>(i);
  const int reduced_seat = original_to_reduced[seat];
  if (reduced_seat < 0)
    throw stage6_candidate_error("acting seat is not in the bucket's flop-live set");

  const std::array<int, 3> flop = {state.board()[0], state.board()[1], state.board()[2]};
  // The shadow is the reduced representative rooted game; the observed
  // postflop log (populated by the estimator/simulator on every query) is
  // replayed onto it below. This is identical on both entry shapes; the
  // binding above differed only.
  poker::GameState shadow(reduced_def(bucket, flop));
  PublicPath path(geometry_bucket_token(bucket.key));
  std::size_t node = installed->tree ? installed->tree->root_index() : bs::tree::kNoNode;
  (void)representative_root;

  // Applies the real public cards needed until the shadow reaches an action
  // state (a street may have closed after the prior observed action).
  auto advance_deals = [&](std::size_t through_board) {
    while (shadow.phase() == poker::Phase::Deal) {
      const std::size_t want = shadow.board().size();
      if (want >= through_board)
        throw stage6_candidate_error("shadow needs a public card the real state has not dealt");
      const int card = state.board()[want];
      const std::size_t ordinal = board_only_ordinal(shadow, card);
      if (installed->tree)
        node = installed->tree->node(node).children.at(ordinal);
      else
        path.on_chance(ordinal);
      shadow = shadow.after_card(card);
    }
  };

  // Replay every observed postflop action onto the representative shadow game.
  const std::array<const std::vector<LoggedAction>*, 3> streets = {
      {&log.flop, &log.turn, &log.river}};
  for (const std::vector<LoggedAction>* street_log : streets) {
    for (const LoggedAction& observed : *street_log) {
      advance_deals(state.board().size());
      if (shadow.phase() != poker::Phase::Action)
        throw stage6_candidate_error("shadow was not at an action state before a logged action");
      const int rep_actor = original_to_reduced[observed.seat];
      if (rep_actor < 0)
        throw stage6_candidate_error("logged postflop action came from a non-live seat");
      if (*shadow.actor() != static_cast<std::size_t>(rep_actor))
        throw stage6_candidate_error("representative shadow actor disagrees with the hand log");

      const std::vector<poker::Action> menu = shadow_menu(*installed, shadow, node);
      const int ordinal = project_exact_to_coarse_index(menu, observed.action);
      if (ordinal < 0) {
        std::string detail =
            "observed type=" + std::to_string(static_cast<int>(observed.action.type)) +
            " target=" + std::to_string(observed.action.target_total) + " coarse{";
        for (const poker::Action& ca : menu)
          detail += std::to_string(static_cast<int>(ca.type)) + ":" +
                    std::to_string(ca.target_total) + ",";
        detail += "}";
        throw stage6_candidate_error("observed action is outside the frozen coarse menu (" +
                                     detail + ")");
      }
      const std::size_t a = static_cast<std::size_t>(ordinal);
      if (installed->tree)
        node = installed->tree->node(node).children.at(a);
      else
        path.on_action(static_cast<std::size_t>(rep_actor), a);
      // The materialized node / streaming path identify a COARSE public node, so
      // the shadow game advances with the matched coarse action. R11 drives the
      // candidate over the reduced representative game with coarse-reachable
      // actions; a fine off-abstraction deviation is rejected above rather than
      // desynchronizing node addressing from the chip state.
      shadow = shadow.after_action(static_cast<std::size_t>(rep_actor), menu[a]);
    }
  }
  advance_deals(state.board().size());
  if (shadow.phase() != poker::Phase::Action ||
      *shadow.actor() != static_cast<std::size_t>(reduced_seat))
    throw stage6_candidate_error("replayed shadow does not reach the acting seat's node");

  // ---- Locate the frozen row at this public node + own card bucket. --------
  const std::vector<int> board_vec(state.board().begin(), state.board().end());
  const std::uint32_t own_bucket =
      static_cast<std::uint32_t>(card_bucket(installed->card_kind, hole, board_vec));
  AbstractInfosetKey row_key;
  row_key.artifact_content_hash = binding.manifest.artifact_content_hash;
  row_key.own_card_bucket = own_bucket;
  if (installed->tree)
    row_key.tree_node_index = node;
  else
    row_key.path_hash = path.hash();

  const FrozenArtifactRows& rows = binding.rows;
  auto row_it = rows.find(row_key);
  // The coarse menu at this structurally-valid node is known whether or not a
  // sampled row exists.
  const std::vector<poker::Action> menu = shadow_menu(*installed, shadow, node);
  if (row_it == rows.end()) {
    // A structurally-valid coarse node the sampled training never touched. In
    // the measurement mode answer with the SAME uniform value an all-zero
    // average row seals to (average_row's documented fallback), and count it;
    // deployed mode stays fail-closed. Genuine abstraction violations are
    // rejected elsewhere and never reach here.
    if (impl_->miss_policy != CandidateMissPolicy::UniformOnUnvisited)
      throw stage6_candidate_error("no frozen average row for the reached information set (" +
                                   bucket.key.to_string() + ")");
    ++impl_->unvisited_misses;
    std::vector<PolicyAction> uniform;
    const double mass = 1.0 / static_cast<double>(menu.size());
    uniform.reserve(menu.size());
    for (const poker::Action& action : menu)
      uniform.push_back(PolicyAction{action, mass});
    return translate_coarse_to_exact(state, seat, uniform, declared_translator_id());
  }
  const AbstractPolicyRow& row = row_it->second;
  if (menu != row.abstract_actions)
    throw stage6_candidate_error("frozen row menu disagrees with the representative node menu");

  std::vector<PolicyAction> coarse;
  coarse.reserve(row.probabilities.size());
  for (std::size_t i = 0; i < row.probabilities.size(); ++i)
    coarse.push_back(PolicyAction{row.abstract_actions[i], row.probabilities[i]});

  // Project representative-chip coarse mass onto the CURRENT concrete legal
  // actions through the R7 translator; it validates legality and never clamps.
  return translate_coarse_to_exact(state, seat, coarse, declared_translator_id());
}

}  // namespace bs::stage6
