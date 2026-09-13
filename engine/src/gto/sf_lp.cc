#include "gto/sf_lp.h"

#include <cmath>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"

namespace bigshark {
namespace alg = open_spiel::algorithms;
using alg::InfostateNode;
using alg::InfostateTree;

namespace {
constexpr double kInf = 1e30;

// Map terminal leaves of the two trees by their full action history
// (chance deal + public actions). Returns opp-terminal -> pl-terminal.
std::map<const InfostateNode*, const InfostateNode*> ConnectTerminals(
    const InfostateTree& pl_tree, const InfostateTree& opp_tree) {
  std::map<std::vector<open_spiel::Action>, const InfostateNode*> by_hist;
  for (InfostateNode* n : pl_tree.leaf_nodes())
    by_hist.emplace(n->TerminalHistory(), n);
  std::map<const InfostateNode*, const InfostateNode*> out;
  for (InfostateNode* n : opp_tree.leaf_nodes())
    out.emplace(n, by_hist.at(n->TerminalHistory()));
  return out;
}
}  // namespace

SequenceFormLp BuildSequenceFormLp(std::shared_ptr<InfostateTree> tree0,
                                   std::shared_ptr<InfostateTree> tree1, int pl) {
  SequenceFormLp out;
  out.optimizer_player = pl;
  out.player_tree = pl == 0 ? tree0 : tree1;
  out.opponent_tree = pl == 0 ? tree1 : tree0;
  LpModel& m = out.model;

  const auto term_map = ConnectTerminals(*out.player_tree, *out.opponent_tree);

  // --- player realization-plan variables + treeplex constraints ---
  // Recursion mirrors SpecifyReachProbsConstraints.
  std::function<void(InfostateNode*)> build_reach = [&](InfostateNode* node) {
    int var = m.AddVar(0.0, 1.0, 0.0);
    out.reach_var[node] = var;
    if (node->type() == alg::kTerminalInfostateNode)
      return;
    if (node->type() == alg::kObservationInfostateNode) {
      for (InfostateNode* child : node->child_iterator()) {
        build_reach(child);
        // -r_parent + r_child = 0
        m.AddRow({{var, -1.0}, {out.reach_var[child], 1.0}}, 'E', 0.0);
      }
      return;
    }
    // decision: -r_parent + sum r_child = 0
    std::vector<std::pair<int, double>> row{{var, -1.0}};
    for (InfostateNode* child : node->child_iterator()) {
      build_reach(child);
      row.push_back({out.reach_var[child], 1.0});
    }
    m.AddRow(std::move(row), 'E', 0.0);
  };
  build_reach(out.player_tree->mutable_root());
  // root realization pinned to 1
  m.lb[out.reach_var[&out.player_tree->root()]] = 1.0;
  m.ub[out.reach_var[&out.player_tree->root()]] = 1.0;

  // --- opponent counterfactual-value variables + constraints ---
  // Recursion mirrors SpecifyCfValuesConstraints.
  std::function<void(InfostateNode*)> build_cf = [&](InfostateNode* node) {
    int var = m.AddVar(-kInf, kInf, 0.0);
    out.cf_var[node] = var;
    if (node->type() == alg::kDecisionInfostateNode) {
      for (InfostateNode* child : node->child_iterator()) {
        build_cf(child);
        // v_child - v_parent <= 0
        m.AddRow({{out.cf_var[child], 1.0}, {var, -1.0}}, 'L', 0.0);
      }
      return;
    }
    std::vector<std::pair<int, double>> row{{var, -1.0}};
    if (node->type() == alg::kTerminalInfostateNode) {
      const InfostateNode* pl_leaf = term_map.at(node);
      double coef = node->terminal_utility() * node->terminal_chance_reach_prob();
      row.push_back({out.reach_var[pl_leaf], coef});
      // -v_node + coef*r <= 0
      m.AddRow(std::move(row), 'L', 0.0);
      return;
    }
    // observation: -v_parent + sum v_child = 0
    for (InfostateNode* child : node->child_iterator()) {
      build_cf(child);
      row.push_back({out.cf_var[child], 1.0});
    }
    m.AddRow(std::move(row), 'E', 0.0);
  };
  build_cf(out.opponent_tree->mutable_root());

  // objective: minimize opponent-root counterfactual value
  m.obj[out.cf_var[&out.opponent_tree->root()]] = 1.0;
  return out;
}

std::string LpToJson(const LpModel& m) {
  std::string s = "{\n";
  absl::StrAppend(&s, "\"num_vars\":", m.num_vars, ",\n\"lb\":[");
  for (size_t i = 0; i < m.lb.size(); ++i)
    absl::StrAppend(&s, i ? "," : "", m.lb[i] <= -kInf / 2 ? "null" : absl::StrCat(m.lb[i]));
  absl::StrAppend(&s, "],\n\"ub\":[");
  for (size_t i = 0; i < m.ub.size(); ++i)
    absl::StrAppend(&s, i ? "," : "", m.ub[i] >= kInf / 2 ? "null" : absl::StrCat(m.ub[i]));
  absl::StrAppend(&s, "],\n\"obj\":[");
  for (size_t i = 0; i < m.obj.size(); ++i)
    absl::StrAppend(&s, i ? "," : "", absl::StrCat(m.obj[i]));
  absl::StrAppend(&s, "],\n\"rows\":[");
  for (size_t r = 0; r < m.rows.size(); ++r) {
    if (r)
      absl::StrAppend(&s, ",");
    absl::StrAppend(&s, "\n {\"sense\":\"", m.rows[r].sense == 'E' ? "E" : "L",
                    "\",\"rhs\":", m.rows[r].rhs, ",\"t\":[");
    for (size_t k = 0; k < m.rows[r].terms.size(); ++k) {
      if (k)
        absl::StrAppend(&s, ",");
      absl::StrAppend(&s, "[", m.rows[r].terms[k].first, ",", m.rows[r].terms[k].second, "]");
    }
    absl::StrAppend(&s, "]}");
  }
  absl::StrAppend(&s, "\n]}\n");
  return s;
}

std::vector<Infopolicy> ExtractPolicy(const SequenceFormLp& lp, const std::vector<double>& sol) {
  std::vector<Infopolicy> out;
  for (auto did : lp.player_tree->AllDecisionIds()) {
    const InfostateNode* node = lp.player_tree->decision_infostate(did);
    Infopolicy p;
    p.infostate = node->infostate_string();
    double sum = 0.0;
    for (int i = 0; i < node->num_children(); ++i)
      sum += sol[lp.reach_var.at(node->child_at(i))];
    for (int i = 0; i < node->num_children(); ++i) {
      double rv = sol[lp.reach_var.at(node->child_at(i))];
      p.probs.push_back(sum > 1e-12 ? rv / sum : 1.0 / node->num_children());
    }
    out.push_back(std::move(p));
  }
  return out;
}

}  // namespace bigshark
