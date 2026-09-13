#include "gto/highs_backend.h"

#include <limits>
#include <vector>

#include "Highs.h"

namespace bigshark {
namespace {
constexpr double kPosInf = 1.0e30;
constexpr double kNegInf = -1.0e30;
}  // namespace

HighsSolveResult SolveLpHighs(const LpModel& model) {
  HighsSolveResult out;
  Highs highs;
  highs.setOptionValue("output_flag", false);
  highs.setOptionValue("parallel", "off");
  highs.setOptionValue("presolve", "on");

  const int n = model.num_vars;
  // columns
  for (int j = 0; j < n; ++j) {
    const double lo = model.lb[j] <= kNegInf / 2 ? kNegInf : model.lb[j];
    const double hi = model.ub[j] >= kPosInf / 2 ? kPosInf : model.ub[j];
    highs.addCol(model.obj[j], lo, hi, 0, nullptr, nullptr);
  }
  // rows
  for (const LpModel::Con& con : model.rows) {
    std::vector<int> idx;
    std::vector<double> val;
    idx.reserve(con.terms.size());
    val.reserve(con.terms.size());
    for (auto [v, c] : con.terms) {
      idx.push_back(v);
      val.push_back(c);
    }
    if (con.sense == 'E')
      highs.addRow(con.rhs, con.rhs, (int)idx.size(), idx.data(), val.data());
    else
      highs.addRow(kNegInf, con.rhs, (int)idx.size(), idx.data(), val.data());
  }

  HighsStatus sc = highs.run();
  const HighsModelStatus ms = highs.getModelStatus();
  const bool optimal = ms == HighsModelStatus::kOptimal && sc == HighsStatus::kOk;
  out.ok = optimal;
  out.status = highs.modelStatusToString(ms);
  if (optimal) {
    const HighsInfo& info = highs.getInfo();
    out.objective = info.objective_function_value;
    const HighsSolution& sol = highs.getSolution();
    out.primal.assign(sol.col_value.begin(), sol.col_value.end());
  }
  return out;
}

SfEquilibrium SolveSequenceForm(SequenceFormLp& lp) {
  SfEquilibrium eq;
  HighsSolveResult r = SolveLpHighs(lp.model);
  eq.ok = r.ok;
  if (!r.ok)
    return eq;
  // ortools/OpenSpiel convention: optimizer's equilibrium value = -objective.
  eq.game_value_to_optimizer = -r.objective;
  eq.policy = ExtractPolicy(lp, r.primal);
  return eq;
}

}  // namespace bigshark
