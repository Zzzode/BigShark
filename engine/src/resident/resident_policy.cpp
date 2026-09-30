#include <algorithm>
#include <array>
#include <bs/abstraction.hpp>
#include <bs/game_definition.hpp>
#include <bs/heads_up.hpp>
#include <bs/heads_up_solver.hpp>
#include <bs/range.hpp>
#include <bs/resident_policy.hpp>
#include <bs/unified_game.hpp>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <span>
#include <string>
#include <utility>

#include "public_reach.hpp"
#include "resident_index.hpp"

namespace bs::resident {
namespace {

using poker::Action;
using poker::GameDef;
using poker::GameState;
using poker::PublicAction;
using poker::Street;
using solver::UnifiedGame;

constexpr std::size_t kKeyActor = 0;
constexpr std::size_t kKeyOwn0 = 1;
constexpr std::size_t kKeyOwn1 = 2;
constexpr std::size_t kKeyBoardSize = 3;
constexpr std::size_t kKeyBoard = 4;

// Canonical root identity: ordered flop, per-seat stacks, matched
// contributions, pot, big blind, and button. Range weights and the ordered
// rational sizing schedule are identical within one advertised artifact and
// cannot be spoofed by a query; two artifacts sharing a root but differing in
// those fields are both refused as ambiguous duplicates.
//
// Deliberately NOT poker::same_game_def, which additionally compares preflop,
// blinds_posted, ante, variant, and terminal. This mirrors the resolver's
// documented six-field identity (counterfactual_reach.cpp): the fields the
// artifact path can actually store and read back. The two disagree in BOTH
// directions on valid roots (a flop root's blinds_posted is unobservable here),
// so the divergence is documented rather than silently tightened.
bool same_root(const GameDef& a, const GameDef& b) {
  if (a.player_count != b.player_count || a.pot != b.pot || a.big_blind != b.big_blind ||
      a.button != b.button)
    return false;
  if (a.board_size < 3 || b.board_size < 3)
    return false;
  for (std::size_t i = 0; i < 3; ++i)
    if (a.board[i] != b.board[i])
      return false;
  for (std::size_t p = 0; p < a.player_count; ++p)
    if (a.stacks[p] != b.stacks[p] || a.contributions[p] != b.contributions[p])
      return false;
  return true;
}

// Build the unified solver view of a schema-v2 identity. The in-memory
// information key layout is identical across revisions (make_information_key
// and information_key(HeadsUpState) emit the same words for the same state),
// so the v2 game's rows are keyed compatibly and the resident replay serves
// them unchanged. W2c-ii-a loads every seat count: the two-seat belief and
// hero-decision queries miss declared (SeatCountNotSupported) until W2c-ii-b,
// while the resolver source already serves the unified game. Turn/river-rooted
// v2 sources stay refused until the resident root generalizes (RFC 0009 D4).
UnifiedGame build_v2_game(const GameDef& game,
                          const std::vector<std::vector<solver::WeightedHand>>& ranges,
                          const abstraction::SizeSchedule& sizes) {
  if (game.board_size != 3)
    throw std::invalid_argument(
        "resident path supports only flop-rooted v2 artifacts; got board_size " +
        std::to_string(game.board_size));
  UnifiedGame built;
  built.def = game;
  for (std::size_t seat = 0; seat < game.player_count; ++seat)
    built.ranges[seat] = ranges[seat];
  built.sizes = sizes;
  return built;
}

// Honest footprint of the immutable game copy kept with an advertised root.
std::size_t game_resident_bytes(const UnifiedGame& game) {
  std::size_t bytes = solver::kUnifiedGameCopyAccountingBytes;
  for (const auto& range : game.ranges)
    bytes += range.capacity() * sizeof(solver::WeightedHand);
  for (const auto& street : game.sizes) {
    bytes += street.bets.capacity() * sizeof(solver::Fraction);
    bytes += street.raises.capacity() * sizeof(solver::Fraction);
  }
  return bytes;
}

// Fill the canonical information key for one own combination at a history
// prefix, exactly matching solver::make_information_key's layout:
// actor, own0, own1, board size, ordered board, then per event
// street, seat, action kind, exact target total.
bool fill_key(ResidentScratch& scratch, std::size_t actor, std::array<int, 2> own,
              std::span<const int> board, std::span<const PublicAction> events) {
  const std::size_t required = kKeyBoard + board.size() + 4 * events.size();
  if (required > scratch.key.size())
    return false;
  scratch.key[kKeyActor] = actor;
  scratch.key[kKeyOwn0] = static_cast<std::uint64_t>(own[0]);
  scratch.key[kKeyOwn1] = static_cast<std::uint64_t>(own[1]);
  scratch.key[kKeyBoardSize] = static_cast<std::uint64_t>(board.size());
  std::size_t word = kKeyBoard;
  for (int card : board)
    scratch.key[word++] = static_cast<std::uint64_t>(card);
  for (const PublicAction& event : events) {
    scratch.key[word++] = static_cast<std::uint64_t>(event.street);
    scratch.key[word++] = event.seat;
    scratch.key[word++] = static_cast<std::uint64_t>(event.action.type);
    scratch.key[word++] = event.action.target_total;
  }
  scratch.key_size = required;
  return true;
}

const char* artifact_failure_detail(const artifacts::ArtifactError& error) {
  static const char* const names[] = {
      "io",
      "sqlite",
      "corrupt",
      "unsupported-version",
      "invalid-schema",
      "identity-mismatch",
      "invalid-value",
      "digest-mismatch",
      "file-too-large",
      "capacity-exceeded",
      "already-exists",
      "invalid-argument",
  };
  const auto ordinal = static_cast<std::size_t>(error.kind());
  return ordinal < std::size(names) ? names[ordinal] : "artifact-error";
}

}  // namespace

const char* to_string(MissReason reason) noexcept {
  switch (reason) {
    case MissReason::None:
      return "none";
    case MissReason::RootNotSupported:
      return "root-not-supported";
    case MissReason::RootIdentityMismatch:
      return "root-identity-mismatch";
    case MissReason::OverBudgetNotAdvertised:
      return "over-budget-not-advertised";
    case MissReason::MissingHistory:
      return "missing-history";
    case MissReason::OffTree:
      return "off-tree";
    case MissReason::ZeroProbabilityObservedAction:
      return "zero-probability-observed-action";
    case MissReason::OffTreeAmount:
      return "off-tree-amount";
    case MissReason::UntrainedCombo:
      return "untrained-combo";
    case MissReason::ComboBlockedByBoard:
      return "combo-blocked-by-board";
    case MissReason::RunoutDivergence:
      return "runout-divergence";
    case MissReason::ZeroProbabilityHeroCombination:
      return "zero-probability-hero-combination";
    case MissReason::EmptyJointRange:
      return "empty-joint-range";
    case MissReason::OpponentRangeFullyBlocked:
      return "opponent-range-fully-blocked";
    case MissReason::SeatCountNotSupported:
      return "seat-count-not-supported";
  }
  return "unknown";
}

const char* to_string(RootStatus status) noexcept {
  switch (status) {
    case RootStatus::Advertised:
      return "advertised";
    case RootStatus::LoadFailed:
      return "load-failed";
    case RootStatus::OverBudget:
      return "over-budget";
    case RootStatus::DuplicateRoot:
      return "duplicate-root";
    case RootStatus::InvalidRange:
      return "invalid-range";
  }
  return "unknown";
}

struct ResidentPolicySet::Record {
  RootLoadResult result{};
  UnifiedGame game{};
  ResidentIndex index{};
  bool advertised = false;
};

ResidentPolicySet::ResidentPolicySet() = default;
ResidentPolicySet::~ResidentPolicySet() = default;
ResidentPolicySet::ResidentPolicySet(ResidentPolicySet&&) noexcept = default;
ResidentPolicySet& ResidentPolicySet::operator=(ResidentPolicySet&&) noexcept = default;

ResidentPolicySet ResidentPolicySet::build(std::vector<SupportedRootSpec> specs,
                                           const ResidentOptions& options,
                                           std::vector<RootLoadResult>* results) {
  ResidentPolicySet set;
  if (results)
    results->clear();
  std::size_t used_bytes = 0;

  for (const SupportedRootSpec& spec : specs) {
    Record record;
    record.result.path = spec.path;

    try {
      if (!spec.expected_sha256)
        throw std::invalid_argument(
            "supported root requires a pinned SHA-256 digest; unpinned artifacts are not resident");

      // Offline pre-gate: the additive artifact probe performs every physical
      // and schema validation without materializing policy rows, so an
      // oversized root is refused before the full eager load can amplify into
      // the map-plus-index transient.
      artifacts::ArtifactProbe probe = artifacts::probe_artifact(
          spec.path,
          artifacts::LoadOptions{artifacts::kDefaultMaxArtifactBytes, spec.expected_sha256});
      record.result.sha256_hex = probe.sha256_hex;
      // RFC 0009 D4: a schema-v2 probe carries the seat-generic identity;
      // build the unified view of every flop-rooted source. A 3..10-seat
      // source loads and advertises; its belief and hero-decision queries
      // miss declared until W2c-ii-b. Turn/river-rooted shapes stay refused.
      if (probe.game_def)
        record.game = build_v2_game(*probe.game_def, *probe.ranges, *probe.sizes);
      else
        record.game = solver::to_unified_game(probe.game);

      bool duplicate = false;
      for (const Record& other : set.records_)
        if (other.result.sha256_hex == record.result.sha256_hex ||
            same_root(other.game.def, record.game.def))
          duplicate = true;
      if (duplicate) {
        record.result.status = RootStatus::DuplicateRoot;
        record.result.detail = "supported root duplicates an already loaded root identity";
      } else if (!(root_joint_mass(record.game) > 0.0)) {
        record.result.status = RootStatus::InvalidRange;
        record.result.detail =
            "declared ranges leave no positive card-compatible joint deal at the root";
      } else {
        const std::size_t estimated = ResidentIndex::estimate_bytes(
            static_cast<std::size_t>(probe.information_sets),
            static_cast<std::size_t>(probe.action_count),
            static_cast<std::size_t>(probe.total_key_words), record.game);
        record.result.information_sets = static_cast<std::size_t>(probe.information_sets);
        record.result.probability_count = static_cast<std::size_t>(probe.action_count);
        record.result.resident_bytes = estimated;
        if (estimated > options.budget_bytes - used_bytes) {
          record.result.status = RootStatus::OverBudget;
          record.result.detail =
              "probe-estimated resident footprint exceeds the remaining resident budget";
          // Keep the small game identity so later queries name
          // OverBudgetNotAdvertised by canonical root; the compact index was
          // never built, so the oversized row storage is not retained.
        } else {
          // Final truth: the full validated load and the exact measured
          // footprint. The probe guarantees the expensive materialization
          // only happens for roots the conservative bound admitted.
          artifacts::LoadOptions load_options;
          load_options.expected_sha256 = spec.expected_sha256;
          artifacts::LoadedArtifact loaded = artifacts::load_artifact(spec.path, load_options);
          if (loaded.bundle.nseat) {
            // RFC 0009 D4: schema-v2 seat-generic policy. Build the unified
            // view and build the index from the concrete seat-indexed rows.
            const auto& policy = loaded.bundle.nseat->policy;
            record.game = build_v2_game(policy.game(), policy.ranges(), policy.sizes());
            record.index.build(policy.rows());
          } else {
            record.game = solver::to_unified_game(loaded.bundle.result.policy.game());
            record.index.build(loaded.bundle.result.policy.rows());
          }
          record.result.information_sets = record.index.row_count();
          record.result.probability_count = record.index.probability_count();
          record.result.resident_bytes =
              record.index.resident_bytes() + game_resident_bytes(record.game);

          if (record.result.resident_bytes > options.budget_bytes - used_bytes) {
            record.result.status = RootStatus::OverBudget;
            record.result.detail =
                "measured resident footprint exceeds the remaining resident budget";
            record.index = ResidentIndex{};
          } else {
            record.result.status = RootStatus::Advertised;
            record.advertised = true;
            used_bytes += record.result.resident_bytes;
          }
        }
      }
    } catch (const artifacts::ArtifactError& error) {
      record.result.status = RootStatus::LoadFailed;
      record.result.detail = std::string(artifact_failure_detail(error)) + ": " + error.what();
    } catch (const std::exception& error) {
      record.result.status = RootStatus::LoadFailed;
      record.result.detail = error.what();
    }

    if (results)
      results->push_back(record.result);
    set.records_.push_back(std::move(record));
  }
  return set;
}

std::size_t ResidentPolicySet::advertised_roots() const noexcept {
  std::size_t count = 0;
  for (const Record& record : records_)
    if (record.advertised)
      ++count;
  return count;
}

std::size_t ResidentPolicySet::total_resident_bytes() const noexcept {
  std::size_t total = 0;
  for (const Record& record : records_)
    if (record.advertised)
      total += record.result.resident_bytes;
  return total;
}

const RootLoadResult& ResidentPolicySet::root_result(std::size_t index) const {
  return records_.at(index).result;
}

namespace {

struct ResolvedRoot {
  const ResidentPolicySet::Record* record = nullptr;
  MissReason miss = MissReason::None;
};

// Match the query against advertised records, honoring an optional pinned
// artifact digest.
ResolvedRoot resolve_root(const std::vector<ResidentPolicySet::Record>& records,
                          const GameState& state, std::optional<std::string_view> pinned_sha256) {
  if (pinned_sha256) {
    const ResidentPolicySet::Record* pinned = nullptr;
    for (const ResidentPolicySet::Record& record : records)
      if (record.result.sha256_hex == *pinned_sha256)
        pinned = &record;
    if (!pinned)
      return {nullptr, MissReason::RootNotSupported};
    if (!pinned->advertised) {
      const MissReason reason = pinned->result.status == RootStatus::InvalidRange
                                    ? MissReason::EmptyJointRange
                                    : MissReason::OverBudgetNotAdvertised;
      return {nullptr, reason};
    }
    if (!same_root(pinned->game.def, state.def()))
      return {nullptr, MissReason::RootIdentityMismatch};
    return {pinned, MissReason::None};
  }

  const ResidentPolicySet::Record* known = nullptr;
  RootStatus known_status = RootStatus::LoadFailed;
  for (const ResidentPolicySet::Record& record : records) {
    if (!same_root(record.game.def, state.def()))
      continue;
    if (record.advertised)
      return {&record, MissReason::None};
    known = &record;
    known_status = record.result.status;
  }
  if (known)
    return {nullptr, known_status == RootStatus::InvalidRange
                         ? MissReason::EmptyJointRange
                         : MissReason::OverBudgetNotAdvertised};
  return {nullptr, MissReason::RootNotSupported};
}

// Whether a public card is in the solver's chance support at the given deal
// slot. Fixed slots must deal the reserved card (covered by runout_matches),
// and a free slot never deals a card reserved for a LATER fixed slot: the
// trainer's public_cards support excludes every fixed-runout card before any
// deal, so such a branch is outside the trained tree.
bool card_in_chance_support(const UnifiedGame& game, std::size_t slot, int dealt) {
  if (game.fixed_runout[slot])
    return dealt == *game.fixed_runout[slot];
  for (std::size_t later = slot + 1; later < game.fixed_runout.size(); ++later)
    if (game.fixed_runout[later] && dealt == *game.fixed_runout[later])
      return false;
  return true;
}

// Fixed-runout boards must match HeadsUpPolicy::lookup behavior: a turn or
// river card that diverges from the artifact's reserved runout is a miss.
bool runout_matches(const UnifiedGame& game, const GameState& state) {
  for (std::size_t i = 3; i < state.board().size(); ++i) {
    const auto fixed = game.fixed_runout[i - 3];
    if (fixed && state.board()[i] != *fixed)
      return false;
  }
  return true;
}

// Find an action by kind AND exact street target total in a resident row.
bool locate_action(const CompactRowView& row, const Action& wanted, std::size_t& index) {
  for (std::size_t i = 0; i < row.count; ++i)
    if (row.actions[i].type == wanted.type && row.actions[i].target_total == wanted.target_total) {
      index = i;
      return true;
    }
  return false;
}

// Replay one observed action: read the policy probability of every live actor
// combination that still has a positive compatible joint mass, distinguishing
// a missing node from an action-set mismatch. Orphan combinations (positive
// raw reach but no positive joint partner after a free card) never receive a
// row in a complete artifact; their factor is left at zero, which matches
// their zero joint mass.
MissReason read_action_probabilities(const ResidentIndex& index, const ReachModel& model,
                                     ResidentScratch& scratch, const GameState& state,
                                     const PublicAction& event,
                                     std::span<const PublicAction> prefix) {
  // Prefix board size follows the EVENT's street, not the final state.
  const std::size_t prefix_street = static_cast<std::size_t>(event.street);
  const std::size_t prefix_board_size = 3 + prefix_street;
  const std::span<const int> prefix_board(state.board().data(), prefix_board_size);
  const std::size_t actor = event.seat;

  scratch.action_probability.fill(0.0);
  for (int combo = 0; combo < N_COMBOS; ++combo) {
    if (scratch.raw[actor][combo] == 0.0)
      continue;
    if (!model.has_positive_partner(actor, combo))
      continue;  // orphan under the current conditioned board: no row exists
    const auto cards = comboCards(combo);
    std::array<int, 2> own{cards[0], cards[1]};
    if (!fill_key(scratch, actor, own, prefix_board, prefix))
      return MissReason::OffTree;
    CompactRowView row;
    if (!index.find(std::span<const std::uint64_t>(scratch.key.data(), scratch.key_size), row))
      return MissReason::MissingHistory;
    std::size_t action_index = 0;
    if (!locate_action(row, event.action, action_index))
      return MissReason::OffTreeAmount;
    scratch.action_probability[combo] = row.probabilities[action_index];
  }
  return MissReason::None;
}

// Run the full hero-independent public replay and fill the answer's belief
// pointers on success. Public belief is defined at every on-tree public point
// the observed path reaches, including a dealing boundary or a fold terminal;
// the caller asks for a hero decision row only at an action node.
MissReason replay_public_path(const ResidentPolicySet::Record& record, const GameState& state,
                              std::span<const PublicAction> events, ResidentScratch& scratch,
                              ResidentAnswer& answer) {
  ReachModel model(scratch);
  if (!model.initialize(record.game))
    return MissReason::EmptyJointRange;

  std::size_t consumed = 0;
  static constexpr std::array<Street, 3> streets{Street::Flop, Street::Turn, Street::River};
  for (std::size_t street = 0; street < streets.size(); ++street) {
    if (street > 0) {
      const std::size_t board_index = 2 + street;  // board[3] turn, board[4] river
      if (state.board().size() > board_index) {
        const int dealt = state.board()[board_index];
        const std::size_t slot = street - 1;
        if (!card_in_chance_support(record.game, slot, dealt))
          return MissReason::OffTree;
        if (!model.observe_card(dealt))
          return MissReason::EmptyJointRange;
      }
    }
    while (consumed < events.size() && events[consumed].street == streets[street]) {
      const PublicAction& event = events[consumed];
      const std::span<const PublicAction> prefix(events.data(), consumed);
      // W2c-ii-b: the three-seat has_positive_partner reads the cached partner
      // mass, which must be rebuilt for this actor after the latest mutation.
      model.prepare_partner_mass(event.seat);
      const MissReason prob_miss =
          read_action_probabilities(record.index, model, scratch, state, event, prefix);
      if (prob_miss != MissReason::None)
        return prob_miss;
      if (!model.observe_action(event.seat, scratch.action_probability))
        return MissReason::ZeroProbabilityObservedAction;
      ++consumed;
    }
  }
  if (consumed != events.size())
    return MissReason::MissingHistory;

  if (!model.write_marginals())
    return MissReason::EmptyJointRange;
  const std::size_t seats = record.game.def.player_count;
  for (std::size_t seat = 0; seat < seats; ++seat)
    answer.public_reach[seat] = &scratch.marginal[seat];
  answer.public_reach_seats = seats;
  return MissReason::None;
}

ResidentAnswer miss_answer(MissReason reason) {
  ResidentAnswer answer;
  answer.hit = false;
  answer.reason = reason;
  return answer;
}

}  // namespace

ResidentAnswer ResidentPolicySet::public_belief(const GameState& state,
                                                std::span<const PublicAction> history,
                                                std::optional<std::string_view> pinned_sha256,
                                                ResidentScratch& scratch) const {
  const ResolvedRoot resolved = resolve_root(records_, state, pinned_sha256);
  if (!resolved.record)
    return miss_answer(resolved.miss);
  const Record& record = *resolved.record;
  // W2c-ii-b: the belief model is exact for two and three seats; a 4..10-seat
  // artifact loads and advertises, but its per-node belief cannot be conditioned
  // exactly, so the query misses declared (a coverage limitation).
  if (record.game.def.player_count > 3)
    return miss_answer(MissReason::SeatCountNotSupported);
  if (!runout_matches(record.game, state))
    return miss_answer(MissReason::RunoutDivergence);

  ResidentAnswer answer;
  answer.root_index = static_cast<std::size_t>(&record - records_.data());
  answer.artifact_sha256 = record.result.sha256_hex;
  const MissReason replay_miss = replay_public_path(record, state, history, scratch, answer);
  if (replay_miss != MissReason::None)
    return miss_answer(replay_miss);
  if (state.actor())
    answer.actor = *state.actor();
  answer.hit = true;
  answer.reason = MissReason::None;
  return answer;
}

ResidentAnswer ResidentPolicySet::hero_decision(const GameState& state,
                                                std::span<const PublicAction> history,
                                                std::array<int, 2> hero_cards,
                                                std::optional<std::string_view> pinned_sha256,
                                                ResidentScratch& scratch) const {
  const ResolvedRoot resolved = resolve_root(records_, state, pinned_sha256);
  if (!resolved.record)
    return miss_answer(resolved.miss);
  const Record& record = *resolved.record;
  // W2c-ii-b: the belief model behind the hero-private view is exact for two
  // and three seats; a 4..10-seat hero-decision query misses declared.
  if (record.game.def.player_count > 3)
    return miss_answer(MissReason::SeatCountNotSupported);
  if (!runout_matches(record.game, state))
    return miss_answer(MissReason::RunoutDivergence);

  std::sort(hero_cards.begin(), hero_cards.end());
  if (hero_cards[0] < 0 || hero_cards[1] >= 52 || hero_cards[0] == hero_cards[1])
    return miss_answer(MissReason::UntrainedCombo);
  if (state.phase() != poker::Phase::Action || !state.actor())
    return miss_answer(MissReason::MissingHistory);
  for (int card_on_board : state.board())
    if (card_on_board == hero_cards[0] || card_on_board == hero_cards[1])
      return miss_answer(MissReason::ComboBlockedByBoard);

  ResidentAnswer answer;
  answer.root_index = static_cast<std::size_t>(&record - records_.data());
  answer.artifact_sha256 = record.result.sha256_hex;
  answer.hero_cards = hero_cards;

  const MissReason replay_miss = replay_public_path(record, state, history, scratch, answer);
  if (replay_miss != MissReason::None)
    return miss_answer(replay_miss);
  answer.actor = *state.actor();

  // The hero combination's own row at the actual public node.
  const std::span<const int> board(state.board().data(), state.board().size());
  if (!fill_key(scratch, *state.actor(), hero_cards, board, history))
    return miss_answer(MissReason::OffTree);
  CompactRowView compact;
  if (!record.index.find(std::span<const std::uint64_t>(scratch.key.data(), scratch.key_size),
                         compact))
    return miss_answer(MissReason::UntrainedCombo);
  // Failure-safe private conditioning: even with a trained row, never return a
  // blueprint row for a hero combination whose reach along the OBSERVED public
  // path is exactly zero (every observed action factor for it was zero). A
  // per-combo zero can coexist with positive joint mass for other combos, so
  // this is distinct from the hero-independent ZeroProbabilityObservedAction
  // node miss; the skeptic review confirmed the public belief update itself is
  // mathematically valid in that case.
  const int hero_combo = comboIndex(hero_cards[0], hero_cards[1]);
  if (scratch.raw[*state.actor()][hero_combo] == 0.0)
    return miss_answer(MissReason::ZeroProbabilityHeroCombination);
  answer.hero_row.size = compact.count;
  answer.hero_row.actions = compact.actions;
  answer.hero_row.probabilities = compact.probabilities;

  // Hero-private opponent belief: blocker removal only, never renormalized
  // into a relabeled equilibrium range. W2c-ii-b: with three seats there are
  // two opponents; the diagnostic opponent view conditions on the first seat
  // other than the actor. The n-seat resolver gadget (W2c-ii-c) consumes the
  // full joint belief instead of this single-seat view.
  std::size_t opponent = 0;
  for (std::size_t seat = 0; seat < record.game.def.player_count; ++seat) {
    if (seat != *state.actor()) {
      opponent = seat;
      break;
    }
  }
  bool fully_blocked = false;
  ReachModel model(scratch);
  model.opponent_private_view(opponent, hero_cards, fully_blocked);
  if (fully_blocked)
    return miss_answer(MissReason::OpponentRangeFullyBlocked);
  answer.opponent_blocked_reach = scratch.opponent_view.data();

  answer.hit = true;
  answer.reason = MissReason::None;
  return answer;
}

namespace {

// Immutable resolver blueprint view over one advertised resident record. It
// borrows the record's compact rows; no allocation happens per row lookup
// beyond constructing the canonical information key for the query.
class ResidentBlueprintSource final : public resolver::BlueprintSource {
 public:
  explicit ResidentBlueprintSource(const ResidentPolicySet::Record& record) : record_(&record) {}

  const solver::UnifiedGame& game() const override { return record_->game; }

  std::string_view artifact_digest() const override { return record_->result.sha256_hex; }

  std::optional<resolver::BlueprintRowView> row(const GameState& state,
                                                std::span<const PublicAction> history,
                                                std::size_t player,
                                                std::array<int, 2> cards) const override {
    if (player > 1 || !state.actor() || *state.actor() != player)
      return std::nullopt;
    std::sort(cards.begin(), cards.end());
    if (cards[0] < 0 || cards[1] >= 52 || cards[0] == cards[1])
      return std::nullopt;
    for (int public_card : state.board())
      if (public_card == cards[0] || public_card == cards[1])
        return std::nullopt;
    const solver::InformationKey key =
        solver::make_information_key(player, cards, state.board(), history);
    CompactRowView compact;
    if (!record_->index.find(std::span<const std::uint64_t>(key.data(), key.size()), compact))
      return std::nullopt;
    resolver::BlueprintRowView view;
    view.actions = compact.actions;
    view.probabilities = compact.probabilities;
    view.size = compact.count;
    return view;
  }

 private:
  const ResidentPolicySet::Record* record_;
};

}  // namespace

std::unique_ptr<resolver::BlueprintSource> ResidentPolicySet::resolver_source(
    const GameState& state, std::span<const PublicAction> history,
    std::optional<std::string_view> pinned_sha256) const {
  const ResolvedRoot resolved = resolve_root(records_, state, pinned_sha256);
  if (!resolved.record || !resolved.record->advertised)
    return nullptr;
  return std::make_unique<ResidentBlueprintSource>(*resolved.record);
}

}  // namespace bs::resident
