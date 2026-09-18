#include <algorithm>
#include <array>
#include <bs/heads_up.hpp>
#include <bs/resolver.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>

#include "certifier.hpp"
#include "counterfactual_reach.hpp"
#include "gadget_cfr.hpp"

namespace bs::resolver {

const char* to_string(ResolveStatus status) noexcept {
  switch (status) {
    case ResolveStatus::Certified:
      return "certified";
    case ResolveStatus::CertificationRejected:
      return "certification-rejected";
    case ResolveStatus::SolveDeadline:
      return "solve-deadline-exceeded";
    case ResolveStatus::CertifyDeadline:
      return "certify-deadline-exceeded";
    case ResolveStatus::Ineligible:
      return "unsupported-terminal-only-feature";
    case ResolveStatus::CoverageMiss:
      return "blueprint-coverage-miss";
    case ResolveStatus::InvalidInput:
      return "invalid-resolve-input";
  }
  return "unknown";
}

namespace detail {
namespace {

constexpr std::string_view kAlgorithmRevision = "rfc0005-stage9-gadget-linear-rev1";
constexpr std::string_view kUtilityIdentifier = "zero-sum-chip-net-v1";

// Deterministic public cache identity (RFC 0005 line 317). It never contains
// the actual hero hand or any request sampling seed. `effective_iterations` is
// the budget-derived cap actually used for training (not the requested
// configured count), so two requests that share every public state but differ
// in solve budget cannot collide on one key and serve each other's candidate:
// the cap is a function of the budget, and the key must be too.
std::string cache_key(const ReachModel& model, const BlueprintSource& blueprint,
                      const ResolveLimits& limits, std::uint64_t effective_iterations) {
  std::ostringstream out;
  out << kAlgorithmRevision << '|' << kUtilityIdentifier << '|' << blueprint.artifact_digest()
      << '|';
  const auto& root = model.node.root();
  for (int card : root.flop)
    out << card << ',';
  out << '|' << root.stacks[0] << ',' << root.stacks[1] << '|' << root.contributions[0] << ','
      << root.contributions[1] << '|' << root.pot << '|' << root.big_blind << '|' << root.button
      << '|';
  for (int card : model.node.board())
    out << card << ',';
  out << '|';
  for (const auto& event : model.node.history())
    out << static_cast<int>(event.street) << ':' << event.actor << ':'
        << static_cast<int>(event.action.type) << ':' << event.action.target_total << ';';
  out << '|';
  for (const auto& street : model.game->sizes) {
    for (const auto& fraction : street.bets)
      out << fraction.numerator << '/' << fraction.denominator << ',';
    out << ';';
    for (const auto& fraction : street.raises)
      out << fraction.numerator << '/' << fraction.denominator << ',';
    out << ';';
  }
  out << "|it" << effective_iterations << "|sd" << limits.public_seed;
  // Resource caps that change the trained/verified result are part of the
  // identity too: a warm hit must not bypass a caller's own node caps (the
  // documented certification safety valve would silently no-op otherwise).
  out << "|mn" << limits.max_nodes << "|cm" << limits.certify_max_nodes;
  return out.str();
}

ResolveLimits phase_limits(const ResolveLimits& limits, std::chrono::milliseconds time,
                           std::size_t max_nodes) {
  ResolveLimits phase = limits;
  phase.time = time;
  phase.max_nodes = max_nodes;
  return phase;
}

}  // namespace
}  // namespace detail

// In-process cache of complete certified whole-range policies only.
struct Resolver::Cache {
  std::mutex mutex;
  std::string key;
  ResolveResult result;
  bool populated = false;
};

Resolver::Resolver() : cache_(std::make_unique<Cache>()) {}
Resolver::~Resolver() = default;

void Resolver::clear_cache() const noexcept {
  std::lock_guard<std::mutex> lock(cache_->mutex);
  cache_->populated = false;
  cache_->key.clear();
}

ResolveResult Resolver::resolve(const bs::poker::HeadsUpState& node,
                                const BlueprintSource& blueprint,
                                const ResolveLimits& limits) const {
  ResolveResult result;
  result.root_pot = static_cast<double>(node.root().pot);

  const auto start = std::chrono::steady_clock::now();
  const auto receipt_deadline = start + limits.time;
  // Reserve at least 5 ms and 10% of the budget for return processing.
  const auto reserve = std::max(std::chrono::milliseconds(5), limits.time / 10);
  const auto work_deadline = receipt_deadline - reserve;
  if (work_deadline <= start) {
    // The reserved return window already consumes the whole request budget.
    result.status = ResolveStatus::SolveDeadline;
    return result;
  }

  detail::Budget build_budget(detail::phase_limits(
      limits, std::chrono::duration_cast<std::chrono::milliseconds>(work_deadline - start),
      limits.max_nodes));
  detail::ReachModel model{node};
  std::string detail;
  ResolveStatus model_status;
  try {
    model_status = detail::build_model(node, blueprint, build_budget, model, detail);
  } catch (const std::exception&) {
    result.status = ResolveStatus::InvalidInput;
    return result;
  }
  if (model_status != ResolveStatus::Certified) {
    result.status = model_status;
    return result;
  }
  result.node_actions = model.node_actions;

  // Derive the budget-bounded iteration cap HERE, before the cache lookup: the
  // cost driver is the joint-deal count times the ordered-action count, known
  // only once the model exists, and the derived value must be part of the cache
  // identity so two requests that share every public state but differ in solve
  // budget cannot collide on one key and serve each other's candidate. The
  // derivation reads public inputs only (request budget, enumerated deals, the
  // node's action count), so every counterfactual hero combination derives the
  // identical cap. A zero cap means the budget cannot pay for one bounded
  // iteration: discard rather than publish an untrained candidate.
  ResolveLimits solve_limits = limits;
  solve_limits.iterations = iteration_cap_for_budget(limits.time, limits.iterations,
                                                     model.deals.size(), model.node_actions.size());
  if (solve_limits.iterations == 0) {
    result.status = ResolveStatus::SolveDeadline;
    return result;
  }

  const std::string key = detail::cache_key(model, blueprint, limits, solve_limits.iterations);
  {
    std::lock_guard<std::mutex> lock(cache_->mutex);
    if (cache_->populated && cache_->key == key) {
      ResolveResult hit = cache_->result;
      hit.status = ResolveStatus::Certified;
      return hit;
    }
  }

  // Split the remaining monotone window between solve and independent
  // certification so a long solve cannot starve the certifier.
  const auto after_build = std::chrono::steady_clock::now();
  const auto solve_deadline = start + (work_deadline - start) * 7 / 10;
  auto solve_window =
      std::chrono::duration_cast<std::chrono::milliseconds>(solve_deadline - after_build);
  if (solve_window.count() <= 0)
    solve_window = std::chrono::milliseconds(1);
  detail::Budget solve_budget(detail::phase_limits(solve_limits, solve_window, limits.max_nodes));
  detail::GadgetOutput gadget = detail::run_gadget_cfr(model, solve_limits, solve_budget);
  result.completed_iterations = gadget.completed_iterations;
  result.nodes = gadget.nodes;
  result.information_sets = gadget.information_sets;
  if (gadget.status != ResolveStatus::Certified) {
    result.status = ResolveStatus::SolveDeadline;
    return result;
  }

  auto certify_window = std::chrono::duration_cast<std::chrono::milliseconds>(
      work_deadline - std::chrono::steady_clock::now());
  if (certify_window.count() <= 0) {
    result.status = ResolveStatus::CertifyDeadline;
    return result;
  }
  const std::size_t certify_nodes =
      limits.certify_max_nodes == 0 ? limits.max_nodes : limits.certify_max_nodes;
  detail::Budget certify_budget(detail::phase_limits(limits, certify_window, certify_nodes));
  detail::Certification certification =
      detail::certify_candidate(model, gadget.candidate, limits, certify_budget);
  result.margins = std::move(certification.margins);
  if (certification.status == ResolveStatus::CertifyDeadline) {
    result.status = ResolveStatus::CertifyDeadline;
    return result;
  }
  if (certification.status != ResolveStatus::Certified) {
    result.status = certification.status;  // rejected or coverage failure
    return result;
  }

  result.candidate = std::move(gadget.candidate);
  for (const auto& [cards, tc] : gadget.terminate)
    result.gadget_terminate.emplace(cards, tc);
  result.status = ResolveStatus::Certified;

  {
    std::lock_guard<std::mutex> lock(cache_->mutex);
    cache_->key = key;
    cache_->result = result;
    cache_->populated = true;
  }
  return result;
}

}  // namespace bs::resolver
