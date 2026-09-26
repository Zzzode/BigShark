#include <bs/policy.hpp>
#include <bs/stage6/adapter.hpp>
#include <bs/stage6/baseline_policy.hpp>
#include <bs/stage6/chart_preflop.hpp>
#include <stdexcept>

namespace bs::stage6 {

bool is_preflop_state(const poker::GameState& state) {
  return state.board().size() < 3;
}

poker::Action pinned_chart_action(const poker::GameState& state, std::size_t seat, HoleCards hole,
                                  const PolicyContext& context) {
  if (!context.hand_log)
    throw std::invalid_argument("the pinned chart policy requires the simulator's hand log");
  if (context.decision_seed == 0)
    throw std::invalid_argument("the pinned chart policy requires a nonzero deterministic seed");
  const Ctx ctx =
      adapt_to_ctx(state, seat, hole, *context.hand_log, context.decision_seed, AdapterConfig{});
  const SourcedDecision sourced =
      evaluatePolicySourced(ctx, RiverBackendHint{/*.allow_exact=*/false});
  // map_deployed_decision is the single legal-action mapping the baseline uses;
  // calling it here keeps the preflop path literally identical for the pinned
  // baseline and the composed candidate.
  return map_deployed_decision(state, sourced.decision);
}

}  // namespace bs::stage6
