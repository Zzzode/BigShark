#pragma once

#include <array>
#include <bs/heads_up.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
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

struct Fraction {
  std::uint64_t numerator;
  std::uint64_t denominator;
};

struct StreetSizes {
  std::vector<Fraction> bets{{1, 3}, {3, 4}, {3, 2}};
  std::vector<Fraction> raises{{1, 2}, {1, 1}};
};

// One entry per POSTFLOP street, indexed by the Street enum value (Flop = 0,
// Turn = 1, River = 2). A preflop-rooted game reuses the flop entry; a
// dedicated preflop menu is part of the artifact-format work that preflop
// persistence needs and is deliberately out of this change.
using SizeSchedule = std::array<StreetSizes, 3>;

struct WeightedHand {
  std::array<int, 2> cards;
  double weight;
};

struct HeadsUpGame {
  poker::HeadsUpRoot root{};
  std::array<std::vector<WeightedHand>, 2> ranges;
  SizeSchedule sizes;
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
inline constexpr std::size_t kGameCopyAccountingBytes = 320;
static_assert(kGameCopyAccountingBytes >= sizeof(HeadsUpGame),
              "the declared game-copy charge must stay at least the real struct size");

std::vector<poker::Action> abstract_actions(const poker::HeadsUpState& state,
                                            const SizeSchedule& sizes);

using InformationKey = std::vector<std::uint64_t>;
InformationKey information_key(const poker::HeadsUpState& state, std::array<int, 2> own_cards);

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
  HeadsUpGame game_{{}, {}, {StreetSizes{{}, {}}, StreetSizes{{}, {}}, StreetSizes{{}, {}}}, {}};
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
