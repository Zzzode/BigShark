// cfr_solver.h — bounded full-tree DCFR+ river solver (no external deps).
//
// Validated against the exact sequence-form LP: ~0.01% pot exploitability at
// 100k iterations on mixed ranges (see engine/tests/test_gto.cpp). This is the
// always-available backend when the exact LP is disabled (no HiGHS) or the deal
// count exceeds the time budget.
#pragma once
#include <array>
#include <bs/range.hpp>
#include <unordered_map>
#include <vector>

namespace bs::gto {

// Behavior table for one decision node: global combo id -> probabilities for
// its legal actions (2 or 3; unused third slot is 0).
using NodePolicy = std::unordered_map<int, std::array<double, 3>>;

// Six nodes in RiverNode enum order.
struct CfrEquilibrium {
  bool ok = false;
  double value_to_ip = 0.0;
  double exploitability_pot = 1.0;
  int iterations_done = 0;
  std::array<NodePolicy, 6> node;
};

struct CfrOptions {
  int iterations = 100000;
  double time_budget_s = 0.0;  // >0: stop early when exceeded (strategy kept so far)
};

CfrEquilibrium SolveCfr(const std::vector<int>& board, int pot, float bet_frac, float raise_frac,
                        const Range& ip_range, const Range& oop_range, const CfrOptions& opts = {});

}  // namespace bs::gto
