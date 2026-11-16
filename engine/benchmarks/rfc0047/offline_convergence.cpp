#include "offline_convergence.hpp"

#include <cmath>
#include <utility>

namespace bs::rfc0047 {

const char* verdict_token(OfflineConvergenceVerdict verdict) {
  switch (verdict) {
    case OfflineConvergenceVerdict::Pass:
      return "pass";
    case OfflineConvergenceVerdict::ConvergenceNoGo:
      return "convergence_no_go";
    case OfflineConvergenceVerdict::BudgetNoGo:
      return "budget_no_go";
    case OfflineConvergenceVerdict::Inconclusive:
      return "inconclusive";
  }
  return "inconclusive";
}

OfflineConvergenceOutcome classify_convergence(std::vector<OfflineRungRow> rows,
                                               bool stopped_early) {
  OfflineConvergenceOutcome out;
  out.stopped_early = stopped_early;

  for (const OfflineRungRow& row : rows) {
    if (!std::isfinite(row.e_embed_bb) || !std::isfinite(row.bounded_nash_conv_bb) ||
        !std::isfinite(row.exact_exploitability_bb)) {
      out.rows = std::move(rows);
      out.verdict = OfflineConvergenceVerdict::Inconclusive;
      return out;
    }
  }

  if (rows.empty()) {
    out.verdict = OfflineConvergenceVerdict::Inconclusive;
  } else {
    bool passes = false;
    for (const OfflineRungRow& row : rows) {
      if (row.e_embed_bb <= kRfc0049GateBb) {
        passes = true;
        break;
      }
    }
    if (passes)
      out.verdict = OfflineConvergenceVerdict::Pass;
    else if (stopped_early)
      out.verdict = OfflineConvergenceVerdict::BudgetNoGo;
    else
      out.verdict = OfflineConvergenceVerdict::ConvergenceNoGo;
  }
  out.rows = std::move(rows);
  return out;
}

}  // namespace bs::rfc0047
