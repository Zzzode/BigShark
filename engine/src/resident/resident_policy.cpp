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
bool same_root(const GameDef& a, const GameDef& b,
               const std::array<int, 3>* canonical_b = nullptr) {
  if (a.player_count != b.player_count || a.pot != b.pot || a.big_blind != b.big_blind ||
      a.button != b.button)
    return false;
  // RFC 0007: a preflop root (board_size == 0) has no board to compare. Match
  // only when both are preflop; a preflop/postflop mismatch is a root mismatch.
  if (a.board_size == 0 || b.board_size == 0) {
    if (a.board_size != b.board_size)
      return false;
    // Both preflop: compare blinds_posted (derived deterministically on read).
    for (std::size_t p = 0; p < a.player_count; ++p)
      if (a.blinds_posted[p] != b.blinds_posted[p])
        return false;
  } else {
    if (a.board_size < 3 || b.board_size < 3)
      return false;
    if (canonical_b) {
      // W4c-ii: the query's flop was canonicalized; compare against the class
      // representative stored in the record.
      for (std::size_t i = 0; i < 3; ++i)
        if (a.board[i] != (*canonical_b)[i])
          return false;
    } else {
      for (std::size_t i = 0; i < 3; ++i)
        if (a.board[i] != b.board[i])
          return false;
    }
  }
  for (std::size_t p = 0; p < a.player_count; ++p)
    if (a.stacks[p] != b.stacks[p] || a.contributions[p] != b.contributions[p])
      return false;
  return true;
}

// W4c-ii: apply a suit relabel to a card id. relabel[s] = canonical suit, so
// new_card = rank*4 + relabel[old_suit]. The identity relabel {0,1,2,3} leaves
// every card unchanged, which is the v2 (exact-board) case.
int relabel_card(int card, const std::array<int, 4>& relabel) {
  return (card / 4) * 4 + relabel[static_cast<std::size_t>(card % 4)];
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
  // RFC 0007: a preflop flop-terminal root (board_size == 0) is accepted
  // alongside flop-rooted (board_size == 3) sources. Turn/river-rooted v2
  // sources stay refused until the resident root generalizes (RFC 0009 D4).
  if (game.board_size != 3 && game.board_size != 0)
    throw std::invalid_argument(
        "resident path supports only flop-rooted or preflop v2 artifacts; got board_size " +
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

// Compact the scratch's canonical information key into the compact key
// buffer. Returns false when the key cannot be compacted (OffTree).
bool compact_scratch_key(ResidentScratch& scratch) {
  scratch.compact_key_size = ResidentIndex::compact_information_key(
      std::span<const std::uint64_t>(scratch.key.data(), scratch.key_size),
      std::span<std::uint8_t>(scratch.compact_key.data(), scratch.compact_key.size()));
  return scratch.compact_key_size > 0;
}

// Expand compact actions into poker::Action for the public view.
void expand_actions(const CompactRowView& row, std::span<poker::Action> out) {
  for (std::size_t i = 0; i < row.count; ++i) {
    out[i].type = static_cast<poker::ActionType>(row.actions[i].type);
    out[i].target_total = row.actions[i].target;
  }
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
  // RFC 0009 W4c-ii: the declared card abstraction. nullopt marks an
  // exact-board (v2) artifact; a set id marks a class policy (v3) whose
  // stored board is the canonical class representative.
  std::optional<abstraction::AbstractionId> card_abstraction;
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
      // W4c-ii: a v3 probe carries the declared card abstraction; a v2 probe
      // leaves it at nullopt (exact board).
      record.card_abstraction = probe.manifest.card_abstraction;

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
            // W4c-ii: the v3 reader reconstructs the card abstraction from the
            // manifest; the v2 reader leaves it at nullopt.
            record.card_abstraction = loaded.bundle.nseat->card_id;
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

// W4c-ii: the canonical flop of a query against a v3 record, or nullptr for a
// v2 record (or a query with fewer than three board cards, which same_root
// then refuses). The storage is caller-owned so the pointer stays valid
// through the same_root call.
const std::array<int, 3>* canonical_query_board(const ResidentPolicySet::Record& record,
                                                const GameDef& query_def,
                                                std::array<int, 3>& storage) {
  if (!record.card_abstraction || query_def.board_size < 3)
    return nullptr;
  storage =
      abstraction::canonicalize({query_def.board[0], query_def.board[1], query_def.board[2]}).board;
  return &storage;
}

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
    std::array<int, 3> canon_board{};
    if (!same_root(pinned->game.def, state.def(),
                   canonical_query_board(*pinned, state.def(), canon_board)))
      return {nullptr, MissReason::RootIdentityMismatch};
    return {pinned, MissReason::None};
  }

  const ResidentPolicySet::Record* known = nullptr;
  RootStatus known_status = RootStatus::LoadFailed;
  for (const ResidentPolicySet::Record& record : records) {
    std::array<int, 3> canon_board{};
    if (!same_root(record.game.def, state.def(),
                   canonical_query_board(record, state.def(), canon_board)))
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
// W4c-ii: for a v3 record the dealt card is relabeled into canonical
// coordinates before the comparison.
bool runout_matches(const UnifiedGame& game, const GameState& state,
                    const std::array<int, 4>& relabel) {
  for (std::size_t i = 3; i < state.board().size(); ++i) {
    const auto fixed = game.fixed_runout[i - 3];
    if (fixed && relabel_card(state.board()[i], relabel) != *fixed)
      return false;
  }
  return true;
}

// Find an action by kind AND exact street target total in a resident row.
bool locate_action(const CompactRowView& row, const Action& wanted, std::size_t& index) {
  const auto wanted_type = static_cast<std::uint8_t>(wanted.type);
  for (std::size_t i = 0; i < row.count; ++i)
    if (row.actions[i].type == wanted_type &&
        static_cast<std::uint64_t>(row.actions[i].target) == wanted.target_total) {
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
  // RFC 0007: a preflop event has no board cards; postflop events carry the
  // flop (3) plus the street's turn/river card.
  const std::size_t prefix_board_size = (event.street == Street::Preflop) ? 0 : 3 + prefix_street;
  // W4c-ii: build the canonical prefix board — the artifact's canonical flop
  // plus relabeled turn/river cards — so the information key matches the
  // class-policy rows. For v2 the relabel is the identity and the canonical
  // flop equals the query flop, so the board is byte-identical to the old
  // state.board() span.
  std::size_t canon_size = 0;
  for (std::size_t i = 0; i < 3 && i < prefix_board_size; ++i)
    scratch.canonical_board_buf[canon_size++] = scratch.canonical_flop[i];
  for (std::size_t i = 3; i < prefix_board_size; ++i)
    scratch.canonical_board_buf[canon_size++] =
        relabel_card(state.board()[i], scratch.canonical_relabel);
  const std::span<const int> prefix_board(scratch.canonical_board_buf.data(), canon_size);
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
    if (!compact_scratch_key(scratch))
      return MissReason::OffTree;
    CompactRowView row;
    if (!index.find(
            std::span<const std::uint8_t>(scratch.compact_key.data(), scratch.compact_key_size),
            row))
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
  // RFC 0007: preflop events (flop-terminal games only). No board cards are
  // dealt before the frontier, so this loop only replays observed actions.
  while (consumed < events.size() && events[consumed].street == Street::Preflop) {
    const PublicAction& event = events[consumed];
    const std::span<const PublicAction> prefix(events.data(), consumed);
    model.prepare_partner_mass(event.seat);
    const MissReason prob_miss =
        read_action_probabilities(record.index, model, scratch, state, event, prefix);
    if (prob_miss != MissReason::None)
      return prob_miss;
    if (!model.observe_action(event.seat, scratch.action_probability))
      return MissReason::ZeroProbabilityObservedAction;
    ++consumed;
  }
  static constexpr std::array<Street, 3> streets{Street::Flop, Street::Turn, Street::River};
  for (std::size_t street = 0; street < streets.size(); ++street) {
    if (street > 0) {
      const std::size_t board_index = 2 + street;  // board[3] turn, board[4] river
      if (state.board().size() > board_index) {
        // W4c-ii: relabel the dealt card into canonical coordinates so the
        // belief model (which operates in the artifact's coordinate system)
        // conditions on the right card.
        const int dealt = relabel_card(state.board()[board_index], scratch.canonical_relabel);
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
  // W4c-ii: set up the canonical translation for class-based (v3) artifacts.
  // For v2 the relabel is the identity and the canonical flop equals the query
  // flop, so every downstream function behaves exactly as before.
  // RFC 0007: a preflop record (board_size == 0) has no flop to canonicalize;
  // the identity relabel and a zeroed canonical flop are correct.
  if (record.card_abstraction && record.game.def.board_size >= 3) {
    const auto canon =
        abstraction::canonicalize({state.board()[0], state.board()[1], state.board()[2]});
    scratch.canonical_relabel = canon.relabel;
  } else {
    scratch.canonical_relabel = {0, 1, 2, 3};
  }
  if (record.game.def.board_size >= 3) {
    scratch.canonical_flop = {record.game.def.board[0], record.game.def.board[1],
                              record.game.def.board[2]};
  } else {
    scratch.canonical_flop = {0, 0, 0};
  }
  if (!runout_matches(record.game, state, scratch.canonical_relabel))
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
  // W4c-ii: set up the canonical translation for class-based (v3) artifacts.
  // RFC 0007: a preflop record (board_size == 0) has no flop to canonicalize.
  if (record.card_abstraction && record.game.def.board_size >= 3) {
    const auto canon =
        abstraction::canonicalize({state.board()[0], state.board()[1], state.board()[2]});
    scratch.canonical_relabel = canon.relabel;
  } else {
    scratch.canonical_relabel = {0, 1, 2, 3};
  }
  if (record.game.def.board_size >= 3) {
    scratch.canonical_flop = {record.game.def.board[0], record.game.def.board[1],
                              record.game.def.board[2]};
  } else {
    scratch.canonical_flop = {0, 0, 0};
  }
  if (!runout_matches(record.game, state, scratch.canonical_relabel))
    return miss_answer(MissReason::RunoutDivergence);

  std::sort(hero_cards.begin(), hero_cards.end());
  if (hero_cards[0] < 0 || hero_cards[1] >= 52 || hero_cards[0] == hero_cards[1])
    return miss_answer(MissReason::UntrainedCombo);
  if (state.phase() != poker::Phase::Action || !state.actor())
    return miss_answer(MissReason::MissingHistory);
  // The blocking check stays in concrete coordinates: it verifies the actual
  // hero hand does not share a card with the actual board.
  for (int card_on_board : state.board())
    if (card_on_board == hero_cards[0] || card_on_board == hero_cards[1])
      return miss_answer(MissReason::ComboBlockedByBoard);

  // W4c-ii: relabel the hero's hole cards into canonical coordinates for key
  // building, the raw-reach check, and the opponent-private view. The belief
  // model operates in the artifact's coordinate system, so the hero combo
  // index and the opponent blocker removal must use the canonical cards.
  const std::array<int, 2> canon_hero{relabel_card(hero_cards[0], scratch.canonical_relabel),
                                      relabel_card(hero_cards[1], scratch.canonical_relabel)};

  ResidentAnswer answer;
  answer.root_index = static_cast<std::size_t>(&record - records_.data());
  answer.artifact_sha256 = record.result.sha256_hex;
  answer.hero_cards = hero_cards;

  const MissReason replay_miss = replay_public_path(record, state, history, scratch, answer);
  if (replay_miss != MissReason::None)
    return miss_answer(replay_miss);
  answer.actor = *state.actor();

  // The hero combination's own row at the actual public node. Build the
  // canonical board (artifact's flop + relabeled turn/river) so the
  // information key matches the class-policy rows.
  std::array<int, 5> canon_board_buf{};
  std::size_t canon_board_size = 0;
  for (std::size_t i = 0; i < 3 && i < state.board().size(); ++i)
    canon_board_buf[canon_board_size++] = scratch.canonical_flop[i];
  for (std::size_t i = 3; i < state.board().size(); ++i)
    canon_board_buf[canon_board_size++] = relabel_card(state.board()[i], scratch.canonical_relabel);
  const std::span<const int> canon_board(canon_board_buf.data(), canon_board_size);
  if (!fill_key(scratch, *state.actor(), canon_hero, canon_board, history))
    return miss_answer(MissReason::OffTree);
  if (!compact_scratch_key(scratch))
    return miss_answer(MissReason::OffTree);
  CompactRowView compact;
  if (!record.index.find(
          std::span<const std::uint8_t>(scratch.compact_key.data(), scratch.compact_key_size),
          compact))
    return miss_answer(MissReason::UntrainedCombo);
  // Failure-safe private conditioning: even with a trained row, never return a
  // blueprint row for a hero combination whose reach along the OBSERVED public
  // path is exactly zero (every observed action factor for it was zero). A
  // per-combo zero can coexist with positive joint mass for other combos, so
  // this is distinct from the hero-independent ZeroProbabilityObservedAction
  // node miss; the skeptic review confirmed the public belief update itself is
  // mathematically valid in that case.
  const int hero_combo = comboIndex(canon_hero[0], canon_hero[1]);
  if (scratch.raw[*state.actor()][hero_combo] == 0.0)
    return miss_answer(MissReason::ZeroProbabilityHeroCombination);
  // Expand compact actions into scratch so the public view exposes
  // poker::Action. Probabilities are stored as exact doubles and are
  // referenced directly from the immutable blob.
  expand_actions(compact, std::span<poker::Action>(scratch.action_scratch.data(), compact.count));
  answer.hero_row.size = compact.count;
  answer.hero_row.actions = scratch.action_scratch.data();
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
  model.opponent_private_view(opponent, canon_hero, fully_blocked);
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
// beyond constructing the canonical information key for the query. The
// compact index stores 3-byte actions, so a mutable scratch buffer expands
// them into the poker::Action the resolver expects; probabilities are exact
// doubles referenced directly from the immutable blob. The resolver calls
// row() one at a time (each view is consumed before the next call), so the
// shared scratch is safe.
class ResidentBlueprintSource final : public resolver::BlueprintSource {
 public:
  explicit ResidentBlueprintSource(const ResidentPolicySet::Record& record) : record_(&record) {}

  const solver::UnifiedGame& game() const override { return record_->game; }

  std::string_view artifact_digest() const override { return record_->result.sha256_hex; }

  std::optional<resolver::BlueprintRowView> row(const GameState& state,
                                                std::span<const PublicAction> history,
                                                std::size_t player,
                                                std::array<int, 2> cards) const override {
    if (player >= record_->game.def.player_count || !state.actor() || *state.actor() != player)
      return std::nullopt;
    std::sort(cards.begin(), cards.end());
    if (cards[0] < 0 || cards[1] >= 52 || cards[0] == cards[1])
      return std::nullopt;
    // The blocking check stays in concrete coordinates.
    for (int public_card : state.board())
      if (public_card == cards[0] || public_card == cards[1])
        return std::nullopt;
    // W4c-ii: for a v3 class policy, canonicalize the query board and relabel
    // the hero's hole cards so the information key matches the artifact's
    // canonical-coordinate rows. For v2 the relabel is the identity and the
    // canonical flop equals the query flop, so the key is byte-identical to
    // the old state.board() path.
    std::array<int, 4> relabel{0, 1, 2, 3};
    if (record_->card_abstraction) {
      const auto canon =
          abstraction::canonicalize({state.board()[0], state.board()[1], state.board()[2]});
      relabel = canon.relabel;
    }
    const std::array<int, 2> canon_cards{relabel_card(cards[0], relabel),
                                         relabel_card(cards[1], relabel)};
    std::array<int, 5> canon_board_buf{};
    std::size_t canon_board_size = 0;
    for (std::size_t i = 0; i < 3 && i < state.board().size(); ++i)
      canon_board_buf[canon_board_size++] = record_->game.def.board[i];
    for (std::size_t i = 3; i < state.board().size(); ++i)
      canon_board_buf[canon_board_size++] = relabel_card(state.board()[i], relabel);
    const solver::InformationKey key = solver::make_information_key(
        player, canon_cards, std::span<const int>(canon_board_buf.data(), canon_board_size),
        history);
    // Compact the information key for the compact index.
    const std::size_t compact_size = ResidentIndex::compact_information_key(
        std::span<const std::uint64_t>(key.data(), key.size()),
        std::span<std::uint8_t>(compact_key_.data(), compact_key_.size()));
    if (compact_size == 0)
      return std::nullopt;
    CompactRowView compact;
    if (!record_->index.find(std::span<const std::uint8_t>(compact_key_.data(), compact_size),
                             compact))
      return std::nullopt;
    // Expand compact actions into the mutable scratch so the resolver sees
    // poker::Action. Probabilities are exact doubles in the immutable blob
    // and are referenced directly (valid for the set's lifetime).
    for (std::size_t i = 0; i < compact.count; ++i) {
      action_scratch_[i].type = static_cast<poker::ActionType>(compact.actions[i].type);
      action_scratch_[i].target_total = compact.actions[i].target;
    }
    resolver::BlueprintRowView view;
    view.actions = action_scratch_.data();
    view.probabilities = compact.probabilities;
    view.size = compact.count;
    return view;
  }

 private:
  const ResidentPolicySet::Record* record_;
  // Mutable scratch for compact→poker::Action expansion. The resolver
  // calls row() one at a time on the calling thread, so this is safe.
  mutable std::array<std::uint8_t, 512> compact_key_{};
  mutable std::array<poker::Action, 32> action_scratch_{};
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
