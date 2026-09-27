#include <bs/stage6/translator.hpp>
#include <cmath>
#include <cstddef>
#include <stdexcept>
#include <utility>
#include <vector>

namespace bs::stage6 {

namespace {

namespace poker = bs::poker;

// Finds an existing projection of `action`, merging in stable first-seen order.
void add_mass(std::vector<PolicyAction>* projected, const poker::Action& action, double mass) {
  for (PolicyAction& pa : *projected) {
    if (pa.action == action) {
      pa.probability += mass;
      return;
    }
  }
  projected->push_back(PolicyAction{action, mass});
}

// Nearest aggressive menu entry by target total; an equidistant tie resolves
// to the smaller total (the explicit comparison also handles an unsorted
// menu). When same_type_only is set, Bet matches only Bet and Raise only
// Raise; otherwise every aggressive entry is eligible. Returns -1 when no
// eligible edge exists.
int nearest_aggressive_index(const std::vector<poker::Action>& coarse_menu,
                             const poker::Action& exact_action, bool same_type_only) {
  int best = -1;
  poker::Chips best_distance = 0;
  for (std::size_t i = 0; i < coarse_menu.size(); ++i) {
    const poker::Action& candidate = coarse_menu[i];
    if (candidate.type != poker::ActionType::Bet && candidate.type != poker::ActionType::Raise)
      continue;
    if (same_type_only && candidate.type != exact_action.type)
      continue;
    const poker::Chips distance = candidate.target_total >= exact_action.target_total
                                      ? candidate.target_total - exact_action.target_total
                                      : exact_action.target_total - candidate.target_total;
    const bool closer = best < 0 || distance < best_distance;
    const bool ties_smaller =
        best >= 0 && distance == best_distance &&
        candidate.target_total < coarse_menu[static_cast<std::size_t>(best)].target_total;
    if (closer || ties_smaller) {
      best = static_cast<int>(i);
      best_distance = distance;
    }
  }
  return best;
}

}  // namespace

const TranslatorId& declared_translator_id() {
  // The eval side returns the core identity with the rule-text digest filled.
  static const TranslatorId id = nearest_target_translator_id();
  return id;
}

poker::Chips nearest_legal_target(double wanted, poker::Chips minimum, poker::Chips maximum) {
  if (!std::isfinite(wanted) || wanted < 0.0)
    throw std::invalid_argument("translator: wanted target must be finite and >= 0");
  if (minimum > maximum)
    throw std::invalid_argument("translator: legal aggressive interval is inverted");
  const double lo = static_cast<double>(minimum);
  const double hi = static_cast<double>(maximum);
  if (wanted <= lo)
    return minimum;
  if (wanted >= hi)
    return maximum;
  // Nearest integer with the EXACT half-way case broken toward the smaller
  // total (round-half-down): frac == 0.5 keeps `base`. Coarse totals are
  // integers, so this is identity inside the interval; the half-integer arm
  // makes the declared R7 tie rule explicit and testable.
  const double base = std::floor(wanted);
  const double frac = wanted - base;
  double snapped = frac <= 0.5 ? base : base + 1.0;
  if (snapped < lo)
    snapped = lo;
  if (snapped > hi)
    snapped = hi;
  return static_cast<poker::Chips>(snapped);
}

poker::Action aggressive_fallback_action(const poker::LegalActions& legal) {
  if (legal.call)
    return poker::Action{poker::ActionType::Call, 0};
  if (legal.check)
    return poker::Action{poker::ActionType::Check, 0};
  if (legal.fold)
    return poker::Action{poker::ActionType::Fold, 0};
  throw std::runtime_error(
      "translator: no legal passive action exists to receive "
      "aggressive mass");
}

std::vector<PolicyAction> translate_coarse_to_exact(const poker::GameState& state, std::size_t seat,
                                                    const std::vector<PolicyAction>& coarse_dist,
                                                    const TranslatorId& id) {
  // The rule identity is name+version (a default-constructed TranslatorId is
  // the frozen v1 rule); digest/parameters describe it and do not gate
  // projection.
  const TranslatorId& v1 = declared_translator_id();
  if (id.name != v1.name || id.version != v1.version) {
    // Only the frozen v1 rule exists; an unknown id must never project under
    // v1 semantics silently.
    throw std::invalid_argument("translator: unknown translator id " + id.to_string());
  }
  if (state.phase() != poker::Phase::Action || state.actor() != seat)
    throw std::invalid_argument("translator: projection requires the acting seat's action phase");
  if (coarse_dist.empty())
    throw std::runtime_error("translator: coarse distribution is empty");

  const poker::LegalActions legal = state.legal();
  std::vector<PolicyAction> projected;
  projected.reserve(coarse_dist.size());
  double total = 0.0;
  for (const PolicyAction& coarse : coarse_dist) {
    if (!std::isfinite(coarse.probability) || coarse.probability < 0.0)
      throw std::runtime_error("translator: coarse probability must be finite and >= 0");
    total += coarse.probability;
    const poker::Action& source = coarse.action;
    poker::Action exact;
    switch (source.type) {
      case poker::ActionType::Fold:
        if (!legal.fold)
          throw std::runtime_error(
              "translator: coarse fold mass at a node where folding is "
              "not legal");
        exact = poker::Action{poker::ActionType::Fold, 0};
        break;
      case poker::ActionType::Check:
        if (!legal.check)
          throw std::runtime_error(
              "translator: coarse check mass at a node where checking is "
              "not legal");
        exact = poker::Action{poker::ActionType::Check, 0};
        break;
      case poker::ActionType::Call:
        if (!legal.call)
          throw std::runtime_error(
              "translator: coarse call mass at a node where calling is "
              "not legal");
        // Domain convention: Call carries target_total 0; after_action derives
        // the amount from the state.
        exact = poker::Action{poker::ActionType::Call, 0};
        break;
      case poker::ActionType::Bet:
      case poker::ActionType::Raise:
        if (legal.aggressive) {
          // The exact action takes the LEGAL aggressive type even when the
          // coarse entry named the other (a coarse bet/raise distinction does
          // not survive projection; only the target total matters).
          const poker::Chips target =
              nearest_legal_target(static_cast<double>(source.target_total),
                                   legal.aggressive->minimum, legal.aggressive->maximum);
          exact = poker::Action{legal.aggressive->type, target};
        } else {
          // Declared R7 fallback: aggressive mass shifts to call, else check,
          // else fold. Fold mass is handled in its own case and never becomes
          // aggressive.
          exact = aggressive_fallback_action(legal);
        }
        break;
    }
    if (!legal.contains(exact))
      throw std::runtime_error("translator: projected action failed the legal-set check");
    add_mass(&projected, exact, coarse.probability);
  }

  if (!std::isfinite(total) || std::fabs(total - 1.0) > 1e-9)
    throw std::runtime_error("translator: coarse distribution does not sum to 1");
  if (projected.empty())
    throw std::runtime_error("translator: projection produced an empty distribution");
  for (const PolicyAction& pa : projected) {
    if (!std::isfinite(pa.probability) || pa.probability < 0.0)
      throw std::runtime_error("translator: merged probability must be finite and >= 0");
    if (!legal.contains(pa.action))
      throw std::runtime_error("translator: merged output action is not legal");
  }
  return projected;
}

int project_exact_to_coarse_index(const std::vector<poker::Action>& coarse_menu,
                                  const poker::Action& exact_action) {
  switch (exact_action.type) {
    case poker::ActionType::Fold:
    case poker::ActionType::Check:
    case poker::ActionType::Call:
      // Passives must match a menu entry exactly (passive actions carry
      // target_total 0); a mismatch or absence is reported, never guessed.
      for (std::size_t i = 0; i < coarse_menu.size(); ++i)
        if (coarse_menu[i] == exact_action)
          return static_cast<int>(i);
      return -1;
    case poker::ActionType::Bet:
    case poker::ActionType::Raise:
      break;
  }
  return nearest_aggressive_index(coarse_menu, exact_action, /*same_type_only=*/true);
}

int project_exact_to_coarse_edge(const std::vector<poker::Action>& coarse_menu,
                                 const poker::Action& exact_action) {
  switch (exact_action.type) {
    case poker::ActionType::Fold:
    case poker::ActionType::Check:
    case poker::ActionType::Call:
      for (std::size_t i = 0; i < coarse_menu.size(); ++i)
        if (coarse_menu[i] == exact_action)
          return static_cast<int>(i);
      return -1;
    case poker::ActionType::Bet:
    case poker::ActionType::Raise:
      break;
  }
  return nearest_aggressive_index(coarse_menu, exact_action, /*same_type_only=*/false);
}

}  // namespace bs::stage6
