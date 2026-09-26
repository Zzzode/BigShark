#include <algorithm>
#include <array>
#include <bs/behavior_policy.hpp>
#include <cmath>
#include <stdexcept>
#include <utility>
#include <vector>

namespace bs::stage6 {

namespace {

// The published deviation/uniform menu: pot fractions for an aggressive total
// measured against pot-after-call, between the legal minimum and the cap.
// Denser than the candidate's training menu by design (R8); the minimum and
// the all-in cap are always added separately. Declared as exact reduced
// numerator/denominator pairs so the reference has a stable textual identity.
constexpr std::array<std::pair<int, int>, 5> kPotFractions = {{
    {1, 3},
    {1, 2},
    {3, 4},
    {1, 1},
    {3, 2},
}};

void require_finite_distribution(const std::vector<PolicyAction>& actions) {
  if (actions.empty())
    throw std::runtime_error("behavior policy returned an empty distribution");
  double total = 0.0;
  for (const PolicyAction& pa : actions) {
    if (!std::isfinite(pa.probability) || pa.probability < 0.0)
      throw std::runtime_error("behavior policy probability must be finite and >= 0");
    total += pa.probability;
  }
  if (std::fabs(total - 1.0) > 1e-9)
    throw std::runtime_error("behavior policy probabilities must sum to 1");
}

// Adds an aggressive target if it is not already present, preserving order.
void add_unique(std::vector<poker::Action>* menu, poker::ActionType type, poker::Chips target) {
  for (const poker::Action& existing : *menu)
    if (existing.type == type && existing.target_total == target)
      return;
  menu->push_back(poker::Action{type, target});
}

}  // namespace

std::vector<poker::Action> declared_behavior_menu(const poker::GameState& state, std::size_t seat) {
  if (state.phase() != poker::Phase::Action || state.actor() != seat)
    throw std::invalid_argument("declared_behavior_menu requires the seat's action phase");
  const poker::LegalActions legal = state.legal();
  std::vector<poker::Action> menu;
  if (legal.fold)
    menu.push_back(poker::Action{poker::ActionType::Fold, 0});
  if (legal.check)
    menu.push_back(poker::Action{poker::ActionType::Check, 0});
  if (legal.call)
    // Domain convention: a call action carries target_total 0; the amount to
    // call is derived from the state by after_action (LegalActions::contains
    // requires exactly 0 for a call).
    menu.push_back(poker::Action{poker::ActionType::Call, 0});
  if (legal.aggressive) {
    const poker::ActionType aggressive_type = legal.aggressive->type;
    const poker::Chips minimum = legal.aggressive->minimum;
    const poker::Chips cap = legal.aggressive->maximum;
    const poker::Chips call = legal.call_amount;
    const poker::Chips pot = state.pot();
    add_unique(&menu, aggressive_type, minimum);
    for (const auto& [numerator, denominator] : kPotFractions) {
      // Target = call + fraction * (pot + call). With no bet ahead this is a
      // Bet (call == 0), so the formula reduces to a pot-fraction bet, which
      // is the intended unified convention. Snapped to the nearest legal
      // integer and clamped into the inclusive aggressive interval.
      const double raw = static_cast<double>(call) +
                         static_cast<double>(numerator) / static_cast<double>(denominator) *
                             (static_cast<double>(pot) + static_cast<double>(call));
      auto target = static_cast<poker::Chips>(std::llround(raw));
      target = std::clamp(target, minimum, cap);
      add_unique(&menu, aggressive_type, target);
    }
    add_unique(&menu, aggressive_type, cap);
  }
  if (menu.empty())
    throw std::runtime_error("legal action state produced an empty behavior menu");
  return menu;
}

std::vector<PolicyAction> UniformBehaviorPolicy::distribution(
    const poker::GameState& state, std::size_t seat, HoleCards /*hole*/,
    const PolicyContext& /*context*/) const {
  std::vector<poker::Action> menu = declared_behavior_menu(state, seat);
  const double mass = 1.0 / static_cast<double>(menu.size());
  std::vector<PolicyAction> result;
  result.reserve(menu.size());
  for (const poker::Action& action : menu)
    result.push_back(PolicyAction{action, mass});
  require_finite_distribution(result);
  return result;
}

std::uint64_t declared_menu_identity_hash() noexcept {
  // Length-framed FNV-1a over the exact fraction schedule and the fixed passive
  // ordering. Snapped chip totals do not enter: those are state-dependent.
  std::uint64_t hash = 0xcbf29ce484222325ULL;
  auto eat = [&hash](std::uint64_t value) {
    for (unsigned shift = 0; shift < 64; shift += 8) {
      hash ^= static_cast<unsigned char>((value >> shift) & 0xffULL);
      hash *= 0x100000001b3ULL;
    }
  };
  static constexpr std::uint64_t kMenuTag = 0x6d656e752d7631ULL;  // "menu-v1"
  eat(kMenuTag);
  eat(kPotFractions.size());
  for (const auto& [numerator, denominator] : kPotFractions) {
    eat(static_cast<std::uint64_t>(numerator));
    eat(static_cast<std::uint64_t>(denominator));
  }
  return hash;
}

poker::Action sample_distribution(const std::vector<PolicyAction>& distribution, double unit_draw) {
  if (!std::isfinite(unit_draw) || unit_draw < 0.0 || unit_draw >= 1.0)
    throw std::invalid_argument("unit draw must be in [0, 1)");
  double cumulative = 0.0;
  for (const PolicyAction& pa : distribution) {
    cumulative += pa.probability;
    if (unit_draw < cumulative)
      return pa.action;
  }
  // Floating-point tail: return the last positive-mass action.
  if (!distribution.empty())
    return distribution.back().action;
  throw std::runtime_error("cannot sample from an empty distribution");
}

}  // namespace bs::stage6
