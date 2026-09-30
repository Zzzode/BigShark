#pragma once

#include <array>
#include <bs/abstraction.hpp>
#include <bs/game_definition.hpp>
#include <bs/heads_up.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <vector>

// RFC 0005 artifact storage reconstructs immutable policies from validated
// checkpoint/export records through this one sanctioned assembler. The grant
// adds no mutator to the public facade and changes no solver behavior.
namespace bs::artifacts::detail {
class PolicyAssembler;
}

namespace bs::solver {

// Solver-private test/debug internals are reached only through a passkey whose
// complete definition lives in engine/src/gto/heads_up_solver_debug.hpp. A
// translation unit that does not include that private header can neither
// construct nor use the key.
namespace detail {
class HeadsUpDebugKey;
}  // namespace detail

// The fractional size schedule now lives in the L2 abstraction component
// (bs/abstraction.hpp). It is re-exported in this namespace so every existing
// `solver::Fraction` / `solver::StreetSizes` / `solver::SizeSchedule` reference,
// the artifact's persisted size rows, and the declared game-copy byte charge
// keep their exact type and layout; only the menu's home moved (RFC 0008
// stage 3). The per-street fractions and their order are unchanged.
using abstraction::default_size_schedule;
using abstraction::Fraction;
using abstraction::SizeSchedule;
using abstraction::StreetSizes;

struct WeightedHand {
  std::array<int, 2> cards;
  double weight;
};

struct HeadsUpGame {
  poker::HeadsUpRoot root{};
  std::array<std::vector<WeightedHand>, 2> ranges;
  SizeSchedule sizes = default_size_schedule();
  // A fixed runout defines a different, conditional validation game. All fixed
  // cards are reserved before dealing private hands or any earlier public card.
  std::array<std::optional<int>, 2> fixed_runout{};
};

// Declared accounting constant for one game copy, used instead of
// `sizeof(HeadsUpGame)` by every byte-charge site. RFC 0007 requires this
// decoupling: `accounted_bytes` reaches the artifact manifest, the frozen
// matrix gates, the allocation exact-fit test, and the resident footprint
// estimate, and deriving it from struct layout would let an unrelated field
// addition move all of them silently. Extending `SizeSchedule` (a fourth
// street entry) is exactly such an addition.
//
// The value is a documented budget, not a measurement: it is at least the
// current struct size, so the charge stays conservative, and it changes only by
// a deliberate edit with the rebase evidence the RFC requires.
inline constexpr std::size_t kGameCopyAccountingBytes = 368;
static_assert(kGameCopyAccountingBytes >= sizeof(HeadsUpGame),
              "the declared game-copy charge must stay at least the real struct size");

std::vector<poker::Action> abstract_actions(const poker::HeadsUpState& state,
                                            const SizeSchedule& sizes);

// Seat-generic twin for a historyless GameState (RFC 0009 W2c-ii). At two seats
// the opponent is the other seat, so for an equivalent state it builds the
// identical ordered menu. Three-or-more-seat callers must not assume the
// opponent is a single seat; this overload still resolves the "other" seat as
// 1-actor and is intended for the two-seat resolver path.
std::vector<poker::Action> abstract_actions(const poker::GameState& state,
                                            const SizeSchedule& sizes);

using InformationKey = std::vector<std::uint64_t>;
InformationKey information_key(const poker::HeadsUpState& state, std::array<int, 2> own_cards);

// Unified information-key constructor for 2..10 seats (RFC 0009 D3/D4). Builds
// the SAME layout `information_key` produces for a HeadsUpState —
// [actor, card0, card1, board_count, board_ids..., (street, seat, type, target)
// per observed public action] — but from a historyless GameState's components:
// the acting seat, the hero's two private cards (sorted ascending), the public
// board as an ordered prefix, and the observed public-action path carried
// explicitly because GameState stores no history. The resident source and the
// resolver candidate both use it so an n-seat request and a two-seat request
// for the same state produce the same key. `cards` must be two distinct cards
// in 0..51, none on the board; `board` must be 0, 3, 4, or 5 cards.
InformationKey make_information_key(std::size_t actor, std::array<int, 2> cards,
                                    std::span<const int> board,
                                    std::span<const poker::PublicAction> path);

struct PolicyRow {
  std::vector<poker::Action> actions;
  std::vector<double> probabilities;
};

class HeadsUpPolicy {
 public:
  const HeadsUpGame& game() const { return game_; }
  const std::map<InformationKey, PolicyRow>& rows() const { return rows_; }
  // No uniform fill, off-tree translation, or stack/range scaling on a miss.
  const PolicyRow* lookup(const poker::HeadsUpState& state, std::array<int, 2> own_cards) const;

 private:
  friend class HeadsUpTrainer;
  friend class detail::HeadsUpDebugKey;
  friend class ::bs::artifacts::detail::PolicyAssembler;
  // Empty policies must be constructible while recovering from allocation
  // failure; avoid allocating the normal default size schedule here.
  HeadsUpGame game_{
      {},
      {},
      {StreetSizes{{}, {}}, StreetSizes{{}, {}}, StreetSizes{{}, {}}, StreetSizes{{}, {}}},
      {}};
  std::map<InformationKey, PolicyRow> rows_;
};

struct TrainingLimits {
  std::size_t max_nodes = 1000000;
  std::size_t max_information_sets = 1000000;
  std::size_t max_depth = 256;
  std::size_t max_bytes = std::size_t{1} << 30;
  std::chrono::milliseconds time{60000};
};

enum class TrainingStatus { Complete, ResourceLimit };

struct TrainingResult {
  TrainingStatus status = TrainingStatus::ResourceLimit;
  std::uint64_t completed_iterations = 0;
  std::size_t nodes = 0;
  std::size_t information_sets = 0;
  std::size_t accounted_bytes = 0;
  // Sampled traversal bookkeeping (RFC 0004 algorithm/PRNG revision 1). The
  // full traversal leaves both fields at zero. prng_state is the SplitMix64
  // state after the last committed iteration; re-running train_sampled with
  // the same seed reproduces it and every published policy.
  std::uint64_t seed = 0;
  std::uint64_t prng_state = 0;
  HeadsUpPolicy policy;
};

struct ExactEvaluation {
  std::array<double, 2> profile_value{};
  std::array<double, 2> best_response_value{};
  double nash_conv = 0;
  double exploitability_pot = 0;
};

class HeadsUpTrainer {
 public:
  // Full traversal baseline. Construction validates and canonicalizes the game.
  explicit HeadsUpTrainer(HeadsUpGame game);
  TrainingResult train(std::uint64_t iterations, TrainingLimits limits = {}) const;
  // External-sampling MCCFR, two-player AverageType::kSimple, RFC 0004
  // revision 1: each iteration samples one weighted joint private deal per
  // traverser (traverser 0 then traverser 1), enumerates traverser actions,
  // samples opponent actions and public cards, and freezes the local policy
  // before recursion. Entropy comes exclusively from a SplitMix64 stream
  // initialized to seed; an interrupted iteration restores that stream and
  // discards the prospective table.
  TrainingResult train_sampled(std::uint64_t iterations, std::uint64_t seed,
                               TrainingLimits limits = {}) const;
  // Independent information-set response evaluation on the same legal game.
  // Incomplete policy or insufficient evaluation limits are explicit errors.
  ExactEvaluation evaluate(const HeadsUpPolicy& policy, TrainingLimits limits = {}) const;

 private:
  friend class detail::HeadsUpDebugKey;
  // Solver-private pinned sampled driver hook guarded by the debug passkey,
  // which is constructible only in translation units including the private
  // debug header. Runs the same driver as the public train_sampled entry.
  static TrainingResult debug_run_sampled(const HeadsUpGame& game, std::uint64_t iterations,
                                          std::uint64_t seed, TrainingLimits limits,
                                          const detail::HeadsUpDebugKey& key);
  HeadsUpGame game_;
};

}  // namespace bs::solver
