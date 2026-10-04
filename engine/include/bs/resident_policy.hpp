// RFC 0005 Stage 6 resident policy lookup (offline only).
//
// A ResidentPolicySet is constructed at startup from an EXPLICIT, operator
// supplied list of supported roots: each entry names one validated immutable
// policy artifact and pins its SHA-256 digest. Every listed artifact is
// loaded, digest-verified through bs::artifacts::load_artifact, measured for
// its honest in-memory footprint, and (when it fits the resident budget and
// has a unique root identity) copied into a compact immutable resident store.
// Construction is the only phase that touches SQLite or the heap; warm
// lookups perform no SQL, no locking, and no heap allocation.
//
// Scope of this stage:
//   - resident blueprint storage and lookup over complete supported roots;
//   - hero-card-independent public belief (per-combo public reach);
//   - the separate hero-private blocker filter at final action selection.
// This stage does NOT wire the host, service, policy, v0, or protocol layers,
// does not implement the resolving gadget or certification, and exposes no
// bound, guarantee, or certification symbol. Artifact v1 never writes bounds;
// a loaded policy is therefore advertised for BLUEPRINT lookup only. Resident
// continuations are loaded, but certification bounds do not yet exist.
#pragma once

#include <array>
#include <bs/game_definition.hpp>
#include <bs/heads_up.hpp>
#include <bs/resolver.hpp>
#include <bs/strategy_artifact.hpp>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace bs::resident {

// RFC 0005 default resident budget: at most 256 MiB of loaded policy rows.
inline constexpr std::size_t kDefaultResidentBudgetBytes = std::size_t{256} << 20;

// Every lookup outcome that is not a covered hit carries exactly one reason.
// The component never emits a fallback policy; the caller chooses fallback.
enum class MissReason : std::uint8_t {
  None,
  // No advertised or known record matches the query's game root.
  RootNotSupported,
  // A pinned artifact was named, but the query state's root does not equal
  // that artifact's canonical root identity.
  RootIdentityMismatch,
  // The matching artifact was loaded and measured but not advertised because
  // its resident footprint exceeded the remaining budget.
  OverBudgetNotAdvertised,
  // The observed path reaches a public node with no resident policy row
  // (truncated/renamed history, or a path continuing past an off-tree
  // action). Public belief is still well-defined at an on-tree dealing
  // boundary or fold terminal; only a missing decision node is a miss.
  MissingHistory,
  // The observed path cannot be replayed against the supported root (for
  // example the event path is longer than the fixed resident key buffer).
  OffTree,
  // The node row exists, but the observed action is not a member of its
  // abstract action set: a legal amount the blueprint abstraction does not
  // contain.
  OffTreeAmount,
  // The observed action is in the row but every live actor combo assigns it
  // probability zero, so the conditioned joint distribution is empty.
  ZeroProbabilityObservedAction,
  // The declared or conditioned ranges leave no card-compatible joint deal
  // (a zero-weight or fully cross-blocked range). The lookup fails closed
  // instead of returning non-finite belief.
  EmptyJointRange,
  // The hero's actual combination has no policy row at the query node.
  UntrainedCombo,
  // The hero's actual combination shares a card with the public board.
  ComboBlockedByBoard,
  // A queried turn/river card differs from the game's fixed runout. This
  // matches HeadsUpPolicy::lookup divergence behavior.
  RunoutDivergence,
  // The hero combination is legal, but every opponent combination is removed
  // by hero/board blockers, so no private opponent belief remains.
  OpponentRangeFullyBlocked,
  // The hero combination is legal and has a row, but its conditioned reach
  // along the observed public path is exactly zero (every observed action
  // factor for this combination was zero). A blueprint row for a combination
  // the observer knows the hero cannot hold is never returned.
  ZeroProbabilityHeroCombination,
  // W2c-ii-b: the matched artifact's root serves a seat count the resident
  // belief model cannot condition exactly. Two and three seats are exact; a
  // 4..10-seat belief or hero-decision query misses declared (a coverage
  // limitation, not a protocol failure). The artifact loads and advertises,
  // and the resolver source still serves it.
  SeatCountNotSupported,
};

const char* to_string(MissReason reason) noexcept;

// Per-root startup outcome, reported in the same order as the input specs.
// One failed root never disables another.
enum class RootStatus : std::uint8_t {
  Advertised,
  LoadFailed,
  OverBudget,
  DuplicateRoot,
  // The validated artifact is well formed, but its declared ranges leave no
  // positive card-compatible joint deal at the root, so no policy can be
  // conditioned on it.
  InvalidRange,
};

const char* to_string(RootStatus status) noexcept;

struct SupportedRootSpec {
  std::filesystem::path path;
  // Pinned SHA-256 digest of the artifact bytes. Startup rejects the file
  // unless this digest matches; the pin is mandatory for a supported root.
  std::optional<artifacts::Sha256Digest> expected_sha256;
};

struct ResidentOptions {
  // Total resident budget shared across all advertised roots.
  std::size_t budget_bytes = kDefaultResidentBudgetBytes;
};

struct RootLoadResult {
  std::filesystem::path path;
  RootStatus status = RootStatus::LoadFailed;
  // Empty on success; otherwise the artifact error kind/message or the
  // resident construction failure.
  std::string detail;
  // Populated whenever the artifact was loaded successfully.
  std::string sha256_hex;
  std::size_t information_sets = 0;
  std::size_t probability_count = 0;
  // Honest in-memory footprint of the compact resident records for this
  // root, measured even when the root is OverBudget and not advertised. It
  // counts neither the SQLite page cache nor the file size nor construction
  // temporaries.
  std::size_t resident_bytes = 0;
};

// Immutable view of one resident policy row. The pointers reference the
// resident set's immutable contiguous storage and stay valid for the set's
// lifetime. Probabilities sum to one within 1e-12.
struct ResidentRowView {
  std::size_t size = 0;
  const poker::Action* actions = nullptr;
  const double* probabilities = nullptr;
};

// Caller-owned fixed storage reused across queries. It contains every buffer
// a lookup touches, so the warm query path allocates no heap memory. The
// object is large (about 340 KiB); keep it out of tight stack frames.
struct ResidentScratch {
  // Per-player per-combo raw public reach (product of observed policy
  // probabilities over the artifact's declared range weights), indexed by
  // bs::comboIndex.
  std::array<std::array<double, 1326>, poker::kMaxUnifiedSeats> raw{};
  // Final normalized per-player public marginals, each summing to one.
  std::array<std::array<double, 1326>, poker::kMaxUnifiedSeats> marginal{};
  // Opponent marginal with combinations sharing the actual hero cards
  // removed; written only by a hero decision query.
  std::array<double, 1326> opponent_view{};
  // Per-card summed raw mass, rebuilt while computing joint mass.
  std::array<std::array<double, 52>, poker::kMaxUnifiedSeats> card_mass{};
  // W2c-ii-b: cached per-seat partner mass J_{-seat}(combo) for the
  // three-seat belief path, filled by ReachModel::prepare_partner_mass. The
  // two-seat path answers has_positive_partner in O(1) and leaves this unset.
  std::array<std::array<double, 1326>, poker::kMaxUnifiedSeats> partner_mass{};
  // Per-observed-action probability scratch for actor combos.
  std::array<double, 1326> action_probability{};
  // Fixed-capacity canonical information key (same layout as
  // solver::information_key).
  std::array<std::uint64_t, 256> key{};
  std::size_t key_size = 0;
  // Compact (compressed) information key for resident index lookup. Filled
  // from `key` by compact_information_key before each index.find call.
  std::array<std::uint8_t, 512> compact_key{};
  std::size_t compact_key_size = 0;
  // Expanded action scratch for the public ResidentRowView. The compact
  // index stores 3-byte CompactActions; the public view exposes poker::Action,
  // so this buffer holds the expanded values.
  std::array<poker::Action, 32> action_scratch{};
  // Deference scratch for the codebook probability layout. When the resident
  // index stores probabilities as uint8 indices into a per-root codebook, the
  // boundary defers them here so the public view exposes exact doubles.
  std::array<double, 32> probability_scratch{};
  // RFC 0009 W4c-ii: canonical translation for class-based (v3) artifacts.
  // canonical_relabel[s] = canonical suit for concrete suit s (identity for
  // v2). canonical_flop is the artifact's canonical flop board, which equals
  // the query's flop for v2 and is the class representative for v3.
  // canonical_board_buf is scratch space for the canonical prefix board used
  // in key building. All three are set per query at the resident boundary
  // before any downstream function reads them.
  std::array<int, 4> canonical_relabel{0, 1, 2, 3};
  std::array<int, 3> canonical_flop{};
  std::array<int, 5> canonical_board_buf{};
};

struct ResidentAnswer {
  bool hit = false;
  MissReason reason = MissReason::None;
  // Valid on a hit: index into the construction order and stable digest view.
  std::size_t root_index = 0;
  std::string_view artifact_sha256;
  std::size_t actor = 0;
  // Public belief: per-seat 1326-entry arrays indexed by bs::comboIndex; each
  // seat's marginal sums to one. Identical for every hero combination. The
  // first `public_reach_seats` entries are valid (W2c-ii-b: two or three).
  std::array<const std::array<double, 1326>*, poker::kMaxUnifiedSeats> public_reach{};
  std::size_t public_reach_seats = 0;
  // Hero decision only: the hero's own policy row and the hero-private
  // opponent belief (opponent combos blocked by hero/board removed).
  ResidentRowView hero_row{};
  std::array<int, 2> hero_cards{-1, -1};
  const double* opponent_blocked_reach = nullptr;
};

class ResidentPolicySet {
 public:
  ResidentPolicySet();
  ~ResidentPolicySet();
  ResidentPolicySet(const ResidentPolicySet&) = delete;
  ResidentPolicySet& operator=(const ResidentPolicySet&) = delete;
  ResidentPolicySet(ResidentPolicySet&&) noexcept;
  ResidentPolicySet& operator=(ResidentPolicySet&&) noexcept;

  // Load, verify, measure, and advertise the explicit supported roots. Never
  // throws for a bad artifact: outcomes are reported per root and construction
  // continues. Results appear in input order. As with other container views,
  // answer pointers and string views are invalidated by moving the set.
  static ResidentPolicySet build(std::vector<SupportedRootSpec> specs,
                                 const ResidentOptions& options,
                                 std::vector<RootLoadResult>* results);

  std::size_t advertised_roots() const noexcept;
  std::size_t total_resident_bytes() const noexcept;
  const RootLoadResult& root_result(std::size_t index) const;

  // Compute the hero-card-independent public belief at a public decision
  // node. The state is matched by its canonical root identity. When
  // pinned_sha256 is given, only that artifact may answer. No hole-card
  // argument exists by design: the result must be identical for every hero
  // combination. `history` is the observed public-action path from the
  // artifact's flop root to `state` (GameState stores no history).
  ResidentAnswer public_belief(const poker::GameState& state,
                               std::span<const poker::PublicAction> history,
                               std::optional<std::string_view> pinned_sha256,
                               ResidentScratch& scratch) const;

  // Public belief plus hero-private selection: verify the actual hero combo,
  // return its resident policy row, and produce the blocker-filtered opponent
  // view. Never invents a uniform policy or renormalizes a relabeled range.
  ResidentAnswer hero_decision(const poker::GameState& state,
                               std::span<const poker::PublicAction> history,
                               std::array<int, 2> hero_cards,
                               std::optional<std::string_view> pinned_sha256,
                               ResidentScratch& scratch) const;

  // RFC 0005 Stage 9 offline resolver access. Binds an immutable blueprint
  // source to the advertised record matching `state`'s canonical root (and an
  // optional pinned digest), exposing the record's UnifiedGame and the
  // (state, history, player, own-cards) blueprint rows the two-seat resolver
  // path consumes (seats 0..1; the n-seat gadget lands in W2c-ii-c). This is
  // OFF the warm no-allocation decision path and allocates; it exists only to
  // feed the bounded resolver. Returns nullptr when no advertised record
  // matches or the pin does not resolve. The returned source borrows the set's
  // immutable records and stays valid while the set is alive and unmoved.
  std::unique_ptr<resolver::BlueprintSource> resolver_source(
      const poker::GameState& state, std::span<const poker::PublicAction> history,
      std::optional<std::string_view> pinned_sha256 = std::nullopt) const;

  // TU-private record definition lives in resident_policy.cpp; the name is
  // public only so the implementation's file-local helpers can name it.
  struct Record;

 private:
  std::vector<Record> records_;
};

}  // namespace bs::resident
