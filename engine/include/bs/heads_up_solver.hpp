#pragma once

#include <array>
#include <bs/heads_up.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <vector>

namespace bs::solver {

struct Fraction {
  std::uint64_t numerator;
  std::uint64_t denominator;
};

struct StreetSizes {
  std::vector<Fraction> bets{{1, 3}, {3, 4}, {3, 2}};
  std::vector<Fraction> raises{{1, 2}, {1, 1}};
};

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
  // Independent information-set response evaluation on the same legal game.
  // Incomplete policy or insufficient evaluation limits are explicit errors.
  ExactEvaluation evaluate(const HeadsUpPolicy& policy, TrainingLimits limits = {}) const;

 private:
  HeadsUpGame game_;
};

}  // namespace bs::solver
