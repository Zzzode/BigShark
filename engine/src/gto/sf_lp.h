// Solver-neutral sequence-form LP for a two-player zero-sum river game.
//
// This is a direct port of OpenSpiel's canonical
// open_spiel::algorithms::ortools::SequenceFormLpSpecification (Koller-
// Megiddo-von Stengel), with the OR-Tools backend replaced by a plain sparse
// model so any LP backend (HiGHS) can consume it.
//
// For a chosen optimizer player `pl` the model has two variable families:
//   * reach prob r for every node of pl's infostate tree (the sequence-form
//     realization plan; root pinned to 1), with treeplex equality constraints
//       observation node: r_parent = r_child (each child)
//       decision node:    r_parent = sum_children r_child
//   * counterfactual value v for every node of the opponent's tree (free),
//       decision child:  v_child <= v_parent
//       observation:     v_parent = sum_children v_child
//       terminal:        v_t <= u_t * p_chance(t) * r(matched pl terminal)
// Objective: minimize v at the opponent root.  pl's equilibrium value is
// -objective (ortools convention).
#pragma once
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "open_spiel/algorithms/infostate_tree.h"

namespace bigshark {

struct LpModel {
  struct Con {
    std::vector<std::pair<int, double>> terms;  // var index, coefficient
    char sense;                                 // 'E' equality or 'L' <= rhs
    double rhs;
  };
  int num_vars = 0;
  std::vector<double> lb, ub, obj;  // +-infinity => free bound
  std::vector<Con> rows;

  int AddVar(double lo, double hi, double c) {
    lb.push_back(lo);
    ub.push_back(hi);
    obj.push_back(c);
    return num_vars++;
  }
  void AddRow(std::vector<std::pair<int, double>> terms, char sense, double rhs) {
    rows.push_back({std::move(terms), sense, rhs});
  }
};

struct SequenceFormLp {
  std::shared_ptr<open_spiel::algorithms::InfostateTree> player_tree;
  std::shared_ptr<open_spiel::algorithms::InfostateTree> opponent_tree;
  LpModel model;
  int optimizer_player;
  // solution-extraction maps
  std::unordered_map<const open_spiel::algorithms::InfostateNode*, int> reach_var;
  std::unordered_map<const open_spiel::algorithms::InfostateNode*, int> cf_var;
};

// Assemble the LP for optimizer `pl` (0 or 1).
SequenceFormLp BuildSequenceFormLp(std::shared_ptr<open_spiel::algorithms::InfostateTree> tree0,
                                   std::shared_ptr<open_spiel::algorithms::InfostateTree> tree1,
                                   int pl);

// Serialize to a sparse JSON document consumable by the python HiGHS oracle.
std::string LpToJson(const LpModel& m);

// Behavior probability at each decision infostate from a reach-var solution
// vector: infostate string -> {action index -> probability}.
struct Infopolicy {
  std::string infostate;
  std::vector<double> probs;
};
std::vector<Infopolicy> ExtractPolicy(const SequenceFormLp& lp,
                                      const std::vector<double>& solution);

}  // namespace bigshark
