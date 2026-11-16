// RFC 0049 Stage 1 Gate 2: typed convergence verdict classification.
//
// Pure, allocation-light decision logic separated from the probe driver so
// native tests can exercise every verdict without a long solve. The
// classification follows the accepted RFC 0049 Gate 2 rule exactly:
//   - no finite rung row                 -> INCONCLUSIVE
//   - any rung with e_embed <= gate      -> PASS
//   - no pass, stop predicate latched    -> BUDGET_NO_GO (partial curve)
//   - no pass, full ladder completed     -> CONVERGENCE_NO_GO
#pragma once

#include <cstdint>
#include <vector>

namespace bs::rfc0047 {

constexpr double kRfc0049GateBb = 0.002;

struct OfflineRungRow {
  std::uint64_t iteration = 0;
  double bounded_nash_conv_bb = 0.0;
  double exact_exploitability_bb = 0.0;
  double pure_menu_term_bb = 0.0;
  double e_embed_bb = 0.0;
  double wall_ms = 0.0;
};

enum class OfflineConvergenceVerdict {
  Pass,
  ConvergenceNoGo,
  BudgetNoGo,
  Inconclusive,
};

const char* verdict_token(OfflineConvergenceVerdict verdict);

struct OfflineConvergenceOutcome {
  OfflineConvergenceVerdict verdict = OfflineConvergenceVerdict::Inconclusive;
  std::vector<OfflineRungRow> rows;
  bool stopped_early = false;
};

// Classify the flushed rung rows. `stopped_early` is the solver's latched
// stop-predicate flag. Rows are expected to be finite and already flushed;
// a non-finite entry is treated as missing instrumentation and yields
// INCONCLUSIVE rather than a pass.
OfflineConvergenceOutcome classify_convergence(std::vector<OfflineRungRow> rows,
                                               bool stopped_early);

}  // namespace bs::rfc0047
