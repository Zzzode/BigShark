// Action and card abstraction for RFC 0008 stage 3 (the L2 layer).
//
// This component sits ABOVE the rules: it depends only on `bigshark_poker`'s
// public types and the rules never know how they are abstracted (RFC 0008
// Dependency Rules -- L2 depends only on L1). A solver/resident asks this
// component for an ordered action menu or a card bucket; nothing here calls a
// solver or performs IO.
//
// Two abstractions, each with its own declared identity:
//
//   * Action abstraction. A declared, ordered menu derived from a legal set.
//     The first (identity) implementation is the RFC 0007 per-street pot
//     fraction schedule made explicit. For the unabstracted legal game it
//     reproduces the shipped menu element-for-element, which is the stage-3
//     "identity abstraction has zero measured error" gate.
//
//   * Card abstraction. A concrete two-card holding plus board mapped to a
//     bucket. The identity implementation is the seven-card evaluator score
//     itself (a strictly order-preserving map, so it merges no two hands of
//     different strength); a declared, versioned strength-TIER bucketing is the
//     first deliberately lossy one and carries a measured (never asserted)
//     merge rate.
//
// `AbstractionId` is a declaration-time value carried by the game/menu, not a
// per-state field, so it never allocates on a rules hot path. It stores NO
// claimed error: measured error is an evidence artifact keyed by the id, never
// a quantity the id asserts about itself (RFC 0008 L2). Persistence is a later
// stage; this type is in-memory and deliberately does not widen the frozen
// RFC 0007 artifact schema.
#pragma once

#include <array>
#include <bs/heads_up.hpp>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace bs::abstraction {

// A positive, reduced pot fraction. Reduced form is part of the declared
// identity so 2/4 and 1/2 are the same fraction.
struct Fraction {
  std::uint64_t numerator = 1;
  std::uint64_t denominator = 1;
  bool operator==(const Fraction&) const = default;
};

// One street's declared aggressive menu. Bets and raises are separate ordered
// lists, exactly as RFC 0007's schedule already expresses them.
struct StreetSizes {
  std::vector<Fraction> bets{{1, 3}, {3, 4}, {3, 2}};
  std::vector<Fraction> raises{{1, 2}, {1, 1}};
  bool operator==(const StreetSizes&) const = default;
};

// Indexed by bs::poker::Street (Flop=0, Turn=1, River=2, Preflop=3).
using SizeSchedule = std::array<StreetSizes, 4>;
static_assert(SizeSchedule{}.size() == static_cast<std::size_t>(poker::Street::Preflop) + 1,
              "SizeSchedule must index every poker::Street");

// The schedule a new game starts from: default pot fractions on the postflop
// streets and the preflop menu. This is the explicit identity action
// abstraction; the values are unchanged from the shipped RFC 0007 schedule.
SizeSchedule default_size_schedule();

// Thrown when a policy/training id is looked up under a different abstraction.
// Distinct type so a host can map it to the RFC 0008 typed refusal rather than
// a generic invalid-argument path.
class abstraction_mismatch : public std::invalid_argument {
 public:
  abstraction_mismatch(const std::string& requested, const std::string& trained)
      : std::invalid_argument("abstraction id mismatch"),
        requested_(requested),
        trained_(trained) {}
  const std::string& requested() const noexcept { return requested_; }
  const std::string& trained() const noexcept { return trained_; }

 private:
  std::string requested_;
  std::string trained_;
};

// A declared abstraction: human name, schema version within that name, the
// canonical parameters that fully determine the map, and a deterministic
// digest of the three. Two ids are the same abstraction iff every field is
// equal; the digest makes accidental parameter drift visible. The digest is a
// reproducible FNV-1a hash, never random, so an id round-tps byte-for-byte.
struct AbstractionId {
  std::string name;
  std::uint32_t version = 0;
  std::string parameters;
  std::uint64_t digest = 0;

  bool operator==(const AbstractionId& other) const {
    return name == other.name && version == other.version && parameters == other.parameters &&
           digest == other.digest;
  }
  bool operator!=(const AbstractionId& other) const { return !(*this == other); }

  std::string to_string() const;
};

// Deterministic 64-bit FNV-1a over the canonical form; exposed so tests and
// later persistence can recompute the same digest without an instance.
std::uint64_t abstraction_digest(const std::string& name, std::uint32_t version,
                                 const std::string& parameters);

// Typed cross-check used wherever a policy trained under `trained` is requested
// under `requested`: equal ids pass, anything else throws abstraction_mismatch.
// A caller that requests the unabstracted game must ask for the identity id
// explicitly; abstraction is never applied silently.
void require_same_abstraction(const AbstractionId& requested, const AbstractionId& trained);

// The declared identity action abstraction: the RFC 0007 per-street pot
// fraction schedule. Its id parameters are the canonical serialization of
// `default_size_schedule()`, so changing a fraction changes the digest.
AbstractionId identity_action_id();

// True when `id` names the declared-coarse action abstraction family
// (RFC 0008 DeclaredOnly menu). The trainer accepts any schedule in this
// family; the digest still pins the exact schedule for artifact compatibility.
bool is_declared_coarse_action_id(const AbstractionId& id);

// Card abstraction families. `Identity` maps a holding to its current
// made-hand evaluator score (strictly strength-order-preserving: it never
// merges two DISTINCT made strengths). That is a zero-error representation of
// current made-hand strength, and on the river (the terminal street) that IS
// showdown value; on the flop/turn two holdings can share a made-hand score
// while having different runout equity (draws), so it is not lossless with
// respect to game value there and the zero-error claim is scoped to
// river-terminal fixtures. `CategoryTiersV1` maps it to the hand category
// 1..9, which deliberately merges every hand within a category and is the
// first declared lossy bucketing, carried only with a measured merge rate.
// `Preflop169` is the standard preflop abstraction: 13 pairs + 78 suited +
// 78 offsuit = 169 buckets, computed directly from card ranks and suits
// without the evaluator. At postflop (non-empty board) it falls back to
// CategoryTiersV1, so river-terminal games are unaffected.
enum class CardBucketKind { Identity, CategoryTiersV1, Preflop169 };

AbstractionId card_abstraction_id(CardBucketKind kind);

// The chips the menu builder needs from a concrete state, passed as plain
// scalars so the abstraction stays independent of either rules state type. The
// solver adapter fills these from a HeadsUpState; a future multiway adapter
// fills them from a GameState.
struct MenuContext {
  poker::Street street = poker::Street::Flop;
  poker::Chips pot = 0;
  poker::Chips actor_committed = 0;
  poker::Chips opponent_committed = 0;
  poker::Chips opponent_stack = 0;
};

// Selects whether an action menu seeds the legal minimum wager and the
// effective all-in cap independently of the declared pot fractions.
//
//   * MinAndCap (default): always offer the minimum full wager and the cap,
//     even when the fraction list is empty. This is the exact shipped RFC
//     0007 behavior and the identity abstraction's rule.
//   * DeclaredOnly: offer ONLY the clamped declared fraction targets. The
//     minimum and cap appear only when a fraction clamps onto one. An empty
//     fraction list therefore yields a purely passive menu (fold/check/call).
//     This is the coarse multiplayer-CFR menu: it removes the forced
//     min-bet/jam at every aggressive node that otherwise multiplies the
//     abstract tree, without changing any identity schedule.
enum class CoverSeeds { MinAndCap, DeclaredOnly };

// The state-neutral ordered-menu rule shared by every profile. Fold/check/call
// are emitted when legal; bets/raises add each declared pot fraction of the
// pot after the call (rounded up, clamped to [minimum, cap]), then sort and
// de-duplicate. Under MinAndCap (the identity rule) the legal minimum and the
// effective all-in cap are seeded first, reproducing the shipped RFC 0007
// computation exactly; under DeclaredOnly they are not. At most 32 actions.
std::vector<poker::Action> build_action_menu(const poker::LegalActions& legal,
                                             const StreetSizes& street_sizes,
                                             const MenuContext& context,
                                             CoverSeeds seeds = CoverSeeds::MinAndCap);

// The multi-seat (3..10) menu context. There is no single opponent: a raise is
// legal if ANY other live, non-folded seat can respond (the existential rule in
// the 3+ profile), so the cap is the DEEPEST cover across every such seat, not
// the cover of whichever seat happens to act next. The caller computes that
// cover from a GameState as max over other live, non-folded seats of
// (street_committed + stack) and supplies it; L2 stays free of any rules-state
// type. The fractions then clamp to [minimum, cover] under the same rule as the
// heads-up menu. At two seats the max has one term and the result is identical
// to build_action_menu, so the identity path and digest do not move.
struct MultiwayMenuContext {
  poker::Street street = poker::Street::Flop;
  poker::Chips pot = 0;
  poker::Chips actor_committed = 0;
  // Deepest total a wager can be matched to: the maximum street total any other
  // live, non-folded seat can reach. The caller computes it with checked
  // arithmetic; it is min-ed against the legal maximum inside the builder.
  poker::Chips cover = 0;
};

std::vector<poker::Action> build_multiway_action_menu(const poker::LegalActions& legal,
                                                      const StreetSizes& street_sizes,
                                                      const MultiwayMenuContext& context,
                                                      CoverSeeds seeds = CoverSeeds::MinAndCap);

// A declared action abstraction: identity schedule plus its id. `menu` selects
// the street entry and delegates to build_action_menu. `declared()` mints the
// stage-6 coarse abstraction: the SAME schedule type but a distinct identity
// ("rfc0008-declared-coarse") whose menus honor the given CoverSeeds, so the
// coarse trainer can suppress the forced min/cap without moving a byte of the
// identity schedule or its golden digest.
class ActionAbstraction {
 public:
  explicit ActionAbstraction(SizeSchedule schedule)
      : schedule_(reduced(std::move(schedule))),
        seeds_(CoverSeeds::MinAndCap),
        id_(make_id(schedule_)) {}

  static ActionAbstraction identity() { return ActionAbstraction(default_size_schedule()); }

  // The declared-only coarse abstraction. `schedule` carries the coarse
  // fractions; `seeds` is serialized into the identity parameters so that
  // MinAndCap and DeclaredOnly mint distinct digests even at equal fractions.
  static ActionAbstraction declared(SizeSchedule schedule, CoverSeeds seeds);

  const AbstractionId& id() const noexcept { return id_; }
  const SizeSchedule& schedule() const noexcept { return schedule_; }
  CoverSeeds cover_seeds() const noexcept { return seeds_; }

  std::vector<poker::Action> menu(const poker::LegalActions& legal,
                                  const MenuContext& context) const {
    return build_action_menu(legal, schedule_[static_cast<std::size_t>(context.street)], context,
                             seeds_);
  }

  // The multi-seat (3..10) menu; honors the same CoverSeeds as `menu`.
  std::vector<poker::Action> multiway_menu(const poker::LegalActions& legal,
                                           const MultiwayMenuContext& context) const {
    return build_multiway_action_menu(legal, schedule_[static_cast<std::size_t>(context.street)],
                                      context, seeds_);
  }

 private:
  ActionAbstraction(SizeSchedule schedule, CoverSeeds seeds, AbstractionId id)
      : schedule_(std::move(schedule)), seeds_(seeds), id_(std::move(id)) {}
  static AbstractionId make_id(const SizeSchedule& schedule);
  static AbstractionId make_declared_id(const SizeSchedule& schedule, CoverSeeds seeds);
  // Reduced form is part of declared identity: an unreduced fraction such as
  // 2/4 is normalized to 1/2 at declaration so the stored schedule and its
  // canonical id always agree (a zero component is rejected here).
  static SizeSchedule reduced(SizeSchedule schedule);
  SizeSchedule schedule_;
  CoverSeeds seeds_;
  AbstractionId id_;
};

// Seven-card score (the identity card bucket). `hole` is two card ids and
// `board` is 0/3/4/5 card ids; the result is the evaluator score, whose order
// is hand strength. An empty board (preflop, RFC 0007) scores the two hole
// cards alone (pair or high-card category).
std::uint32_t strength_bucket(const std::array<int, 2>& hole, const std::vector<int>& board);

// The declared bucket for a holding under a card abstraction family.
std::uint32_t card_bucket(CardBucketKind kind, const std::array<int, 2>& hole,
                          const std::vector<int>& board);

// Suit-isomorphic flop canonicalization (RFC 0009 D5.1). A concrete 3-card
// flop folds onto its isomorphism class: the canonical representative is the
// component-wise minimum, over the 24 suit permutations, of the board's key —
// the (rank, suit) pairs sorted by rank first and suit second. A card id is
// rank*4+suit, already rank-major, so sorting ids is exactly that order and
// the key is never a string (the encoding's "10" vs "9" text order cannot
// leak in). `relabel` is the permutation itself, never an index into the
// enumeration: relabel[s] is the canonical suit that concrete suit s maps to.
// Symmetric boards tie on the minimum; the lexicographically smallest
// relabel among minimizers wins, so the map is total and deterministic. This
// reduces the 22,100 flops to 1,755 isomorphism classes.
struct CanonicalBoard {
  std::array<int, 3> board{};
  std::array<int, 4> relabel{};
  bool operator==(const CanonicalBoard&) const = default;
};

// The information token for a holding under a class policy: the sorted rank
// pair plus the four-slot vector of own-suit multiplicities across canonical
// suits 0..3 (slot i counts the holding's cards whose relabeled suit is i,
// zero included). The vector always has length four and sums to two, so the
// map is total. The token is the identity a class policy keys its rows on;
// like every card abstraction it is deliberately coarser than the exact game
// (its measured merge rate is test evidence, never a claimed zero), and it is
// also finer than the board's stabilizer orbits, so a class policy carries
// separate rows for suit-symmetric spots — a coverage property, not a
// correctness one.
struct OwnCardToken {
  std::array<int, 2> ranks{};
  std::array<int, 4> suit_mult{};
  bool operator==(const OwnCardToken&) const = default;
};

// Fold a concrete flop onto its class. Card ids must be 0..51 and distinct;
// anything else fails closed.
CanonicalBoard canonicalize(const std::array<int, 3>& board);

// Re-express a concrete holding under a class relabel: ranks unchanged, suits
// mapped through `relabel`, then the multiplicity vector over canonical
// suits 0..3. The holding must be two distinct card ids 0..51 and `relabel`
// must be a permutation of 0..3; a malformed relabel would silently merge or
// drop suit slots, so it is rejected.
OwnCardToken own_card_token(const std::array<int, 2>& holding, const std::array<int, 4>& relabel);

// The declared identity of the suit-canonicalization map. Its parameters
// serialize the comparison order, the tie-break, and the token shape, so any
// future change to any of them moves the digest.
AbstractionId suit_canonicalization_id();

}  // namespace bs::abstraction
