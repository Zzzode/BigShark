// Native HiGHS backend for the solver-neutral SequenceFormLp model.
#pragma once
#include <string>
#include <vector>

#include "gto/sf_lp.h"

namespace bigshark {

struct HighsSolveResult {
  bool ok = false;
  std::string status;
  double objective = 0.0;      // raw LP objective (minimized)
  std::vector<double> primal;  // col primal values
};

// Solve the sparse LP in-process with HiGHS. Logs nothing on success.
HighsSolveResult SolveLpHighs(const LpModel& model);

// Convenience: solve the sequence-form LP for its optimizer player and return
// the equilibrium behavior policy plus the game value to the optimizer.
struct SfEquilibrium {
  bool ok = false;
  double game_value_to_optimizer = 0.0;
  std::vector<Infopolicy> policy;
};
SfEquilibrium SolveSequenceForm(SequenceFormLp& lp);

}  // namespace bigshark
