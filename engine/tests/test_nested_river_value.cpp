// RFC 0039 Stage 1: nested river boundary leaf identity and river-rooted
// game construction.
//
// The load-bearing correctness property is structural, not numeric: the
// river-rooted GameDef reconstructed from a turn streets(1) frontier
// ledger must reproduce the EXACT river subtree the full DepthLimit::none()
// reference grows after the same river card — same openers, same ordered
// menus (action types and target totals), same terminals — with the outer
// button preserved and every street-state field reset. The action-
// abstraction digest assertion only proves schedule/cap; this constructive
// node-by-node isomorphism is what proves the ledger reconstruction
// (independent review finding, RFC 0039 round 1).
#include <array>
#include <bs/abstract_tree.hpp>
#include <bs/abstraction.hpp>
#include <bs/continuation_range.hpp>
#include <bs/continuation_value.hpp>
#include <bs/eval.hpp>
#include <bs/game_definition.hpp>
#include <bs/heads_up_solver.hpp>
#include <bs/nseat_trainer.hpp>
#include <bs/root_value_extractor.hpp>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

#define CHECK(cond)                                                                          \
  do {                                                                                       \
    if (!(cond))                                                                             \
      throw std::logic_error(std::string("CHECK failed: ") + #cond + " (" + __FILE__ + ":" + \
                             std::to_string(__LINE__) + ")");                                \
  } while (0)

namespace {

using namespace bs;
using namespace bs::poker;
using namespace bs::tree;
using bs::abstraction::ActionAbstraction;
using bs::gto::make_river_boundary_leaf;
using bs::gto::nested_river_game;

int card(const char* name) {
  return cardId(std::string(name));
}

// 2/3-seat 100 BB limped turn root, identical chip convention to the M2
// fixture (big blind 2, one chip per seat in, stacks behind 199, board
// 2c 3d 7h Js).
GameDef make_turn_def(std::size_t seats) {
  GameDef def{};
  def.player_count = seats;
  def.button = seats - 1;
  def.big_blind = 2;
  for (std::size_t i = 0; i < seats; ++i) {
    def.stacks[i] = 199;
    def.contributions[i] = 1;
  }
  def.pot = static_cast<Chips>(seats);
  def.board = {card("2c"), card("3d"), card("7h"), card("Js"), 0};
  def.board_size = 4;
  def.preflop = false;
  def.terminal = TerminalDepth::River;
  return def;
}

// Replay a frontier node's action-only ancestor path from the root and
// return the matching full-tree river Action subtree root: follow the same
// menu edge indices through Action nodes, then at the river Chance node take
// the child whose incoming card equals the frontier leaf's river card.
const TreeNode& reference_river_root(const AbstractTree& ref, const AbstractTree& dl,
                                     std::size_t frontier_index) {
  // Collect action-edge child indices root -> frontier (bottom-up).
  std::vector<std::size_t> edges;
  std::size_t cur = frontier_index;
  while (dl.node(cur).parent != kNoNode) {
    const TreeNode& n = dl.node(cur);
    const TreeNode& parent = dl.node(n.parent);
    if (parent.kind == NodeKind::Action) {
      std::size_t edge = 0;
      while (parent.children[edge] != cur)
        ++edge;
      edges.push_back(edge);
    }
    cur = n.parent;
  }
  // Replay top-down in the reference tree.
  std::size_t node = ref.root_index();
  for (auto it = edges.rbegin(); it != edges.rend(); ++it) {
    const TreeNode& n = ref.node(node);
    CHECK(n.kind == NodeKind::Action);
    node = n.children[*it];
  }
  // node is now the reference river Chance node (turn street closed).
  const TreeNode& chance = ref.node(node);
  CHECK(chance.kind == NodeKind::Chance);
  const int river_card = dl.node(frontier_index).incoming_card;
  for (const std::size_t child : chance.children)
    if (ref.node(child).incoming_card == river_card)
      return ref.node(child);
  throw std::logic_error("reference river child not found");
}

// Recursive structural equivalence of two subtrees: kind, street, actor,
// the complete ordered menu (action types + target totals), child count,
// AND — at terminal nodes — the full settlement ledger (player count,
// per-seat contributed/refunded/stack/folded, live order/count, board),
// so a menu-identical tree with a wrong pot/stack reconstruction cannot pass.
bool same_terminal_payload(const TerminalPayload& a, const TerminalPayload& b) {
  if (a.player_count != b.player_count || a.live_count != b.live_count ||
      a.board_size != b.board_size || a.folded != b.folded)
    return false;
  for (int k = 0; k < 5; ++k)
    if (a.board[k] != b.board[k])
      return false;
  for (std::size_t s = 0; s < a.player_count; ++s) {
    if (a.seats[s].contributed != b.seats[s].contributed ||
        a.seats[s].refunded != b.seats[s].refunded || a.seats[s].stack != b.seats[s].stack ||
        a.seats[s].folded != b.seats[s].folded)
      return false;
  }
  for (std::size_t k = 0; k < a.live_count; ++k)
    if (a.live_order[k] != b.live_order[k])
      return false;
  return true;
}

bool same_subtree_shape(const AbstractTree& a, std::size_t ai, const AbstractTree& b,
                        std::size_t bi) {
  const TreeNode& x = a.node(ai);
  const TreeNode& y = b.node(bi);
  if (x.kind != y.kind || x.street != y.street)
    return false;
  if (x.kind == NodeKind::Action) {
    if (x.actor != y.actor || x.actions.size() != y.actions.size())
      return false;
    for (std::size_t i = 0; i < x.actions.size(); ++i)
      if (x.actions[i].type != y.actions[i].type ||
          x.actions[i].target_total != y.actions[i].target_total)
        return false;
  }
  if (x.is_terminal() || y.is_terminal()) {
    if (x.terminal == kNoNode || y.terminal == kNoNode ||
        !same_terminal_payload(a.terminal(x.terminal), b.terminal(y.terminal)))
      return false;
  }
  if (x.children.size() != y.children.size())
    return false;
  for (std::size_t i = 0; i < x.children.size(); ++i)
    if (!same_subtree_shape(a, x.children[i], b, y.children[i]))
      return false;
  return true;
}

int test_two_seat_river_subtree_matches_reference() {
  const GameDef def = make_turn_def(2);
  const ActionAbstraction menu = ActionAbstraction::identity(/*cap=*/1);
  AbstractTree dl(def, menu, TreeLimits{}, DepthLimit::streets(1));
  AbstractTree ref(def, menu, TreeLimits{}, DepthLimit::none());
  CHECK(dl.has_frontier_leaves());
  CHECK(!ref.has_frontier_leaves());

  std::size_t frontier_count = 0;
  for (std::size_t i = 0; i < dl.size(); ++i) {
    if (dl.node(i).kind != NodeKind::TerminalFrontier)
      continue;
    ++frontier_count;
    const auto leaf = make_river_boundary_leaf(dl, i);
    CHECK(leaf.outer_action_id == dl.action_id());
    CHECK(leaf.board().size() == 5);
    CHECK(leaf.outer_def.button == def.button);

    const GameDef river_def = nested_river_game(leaf);
    // Explicit field contract.
    CHECK(river_def.button == def.button);
    CHECK(river_def.big_blind == def.big_blind);
    CHECK(river_def.board_size == 5);
    CHECK(!river_def.preflop);
    CHECK(river_def.terminal == TerminalDepth::River);
    CHECK(river_def.last_full_raise == 0);
    CHECK(river_def.raises_this_street == 0);
    Chips sum = 0;
    for (std::size_t s = 0; s < 2; ++s) {
      sum += river_def.contributions[s];
      CHECK(river_def.street_committed[s] == 0);
    }
    CHECK(river_def.pot == sum);

    AbstractTree nested(river_def, dl.action_abstraction(), TreeLimits{}, DepthLimit::none());
    // RFC 0030 identity assertion.
    CHECK(nested.action_id() == dl.action_id());
    CHECK(!nested.has_frontier_leaves());

    const TreeNode& ref_root = reference_river_root(ref, dl, i);
    CHECK(ref_root.kind == NodeKind::Action);
    CHECK(ref_root.street == Street::River);
    CHECK(same_subtree_shape(ref, ref_root.index, nested, nested.root_index()));
  }
  CHECK(frontier_count > 0);
  return 0;
}

int test_three_seat_cap_one_nested_tree() {
  const GameDef def = make_turn_def(3);
  const ActionAbstraction menu = ActionAbstraction::identity(/*cap=*/1);
  AbstractTree dl(def, menu, TreeLimits{}, DepthLimit::streets(1));
  std::size_t checked = 0;
  for (std::size_t i = 0; i < dl.size(); ++i) {
    if (dl.node(i).kind != NodeKind::TerminalFrontier)
      continue;
    const auto leaf = make_river_boundary_leaf(dl, i);
    const GameDef river_def = nested_river_game(leaf);
    AbstractTree nested(river_def, dl.action_abstraction(), TreeLimits{}, DepthLimit::none());
    CHECK(nested.action_id() == dl.action_id());
    // Every frontier leaf carries at least the two seats that continue to
    // the river; a turn fold path legitimately leaves one seat folded in
    // the ledger, so live count follows the payload, not a fixed three.
    std::size_t live = 0;
    for (std::size_t s = 0; s < 3; ++s)
      if (!river_def.folded[s])
        ++live;
    CHECK(live == leaf.ledger.live_count);
    CHECK(live >= 2);
    CHECK(nested.node(nested.root_index()).actor == dl.terminal(dl.node(i).terminal).live_order[0]);
    ++checked;
  }
  CHECK(checked > 0);
  return 0;
}

int test_folded_dead_money_kept() {
  // Hand-built leaf ledger: three-seat hand, seat 2 folded on the flop, its
  // net contribution stays in the pot.
  GameDef outer = make_turn_def(3);
  gto::RiverBoundaryLeaf leaf;
  leaf.outer_def = outer;
  leaf.outer_action_id = ActionAbstraction::identity(1).id();
  leaf.ledger.player_count = 3;
  leaf.ledger.live_count = 2;
  leaf.ledger.live_order = {0,       1,       kNoNode, kNoNode, kNoNode,
                            kNoNode, kNoNode, kNoNode, kNoNode, kNoNode};
  leaf.ledger.board = {card("2c"), card("3d"), card("7h"), card("Js"), card("9c")};
  leaf.ledger.board_size = 5;
  for (std::size_t s = 0; s < 3; ++s) {
    leaf.ledger.seats[s].contributed = 1;
    leaf.ledger.seats[s].stack = (s == 2) ? 200 : 199;
  }
  leaf.ledger.seats[2].folded = true;

  const GameDef river = nested_river_game(leaf);
  CHECK(river.folded[2]);
  CHECK(river.pot == 3);
  CHECK(river.contributions[2] == 1);
  // A folded seat is legal on a 3+ rooted board; validate must accept it.
  validate(river);
  AbstractTree nested(river, ActionAbstraction::identity(1), TreeLimits{}, DepthLimit::none());
  CHECK(!nested.has_frontier_leaves());
  return 0;
}

int test_fail_closed() {
  const GameDef def = make_turn_def(2);
  AbstractTree dl(def, ActionAbstraction::identity(1), TreeLimits{}, DepthLimit::streets(1));
  bool threw = false;
  try {
    (void)make_river_boundary_leaf(dl, dl.root_index());  // root is an Action node
  } catch (const std::invalid_argument&) {
    threw = true;
  }
  CHECK(threw);

  gto::RiverBoundaryLeaf leaf;
  leaf.outer_def = def;
  leaf.outer_action_id = ActionAbstraction::identity(1).id();
  leaf.ledger.player_count = 2;
  leaf.ledger.live_count = 1;  // below two live seats
  leaf.ledger.board_size = 5;
  threw = false;
  try {
    (void)nested_river_game(leaf);
  } catch (const std::invalid_argument&) {
    threw = true;
  }
  CHECK(threw);
  return 0;
}

int test_leaf_ranges_differential() {
  using namespace bs::solver;
  GameDef def = make_turn_def(2);
  for (auto& st : def.stacks)
    st = 199;
  const ActionAbstraction menu = ActionAbstraction::identity(1);
  AbstractTree dl(def, menu, TreeLimits{}, DepthLimit::streets(1));

  const char* names[10][2] = {{"Ac", "Ad"}, {"Kc", "Kd"}, {"Qc", "Qd"}, {"Jc", "Jd"}, {"Tc", "Td"},
                              {"9c", "9d"}, {"Ac", "Kc"}, {"Ac", "Qc"}, {"Ac", "Kd"}, {"9c", "8c"}};
  std::vector<WeightedHand> root;
  std::uint64_t root_mask = 0;
  for (auto& n : names) {
    const auto c = std::array<int, 2>{card(n[0]), card(n[1])};
    root.push_back({c, 1.0});
    root_mask |= 1ULL << c[0] | 1ULL << c[1];
  }
  std::vector<std::vector<WeightedHand>> ranges(2, root);

  // Deterministic NON-UNIFORM policy: action a gets weight a+2, normalized.
  // This makes the differential test non-tautological — a kernel that picks
  // the wrong edge, actor, or card bucket no longer agrees with the brute
  // force path walk the way a uniform 1/menu policy would.
  std::map<NSeatInformationKey, NSeatPolicyRow> rows;
  for (std::size_t i = 0; i < dl.size(); ++i) {
    const TreeNode& n = dl.node(i);
    if (n.kind != NodeKind::Action)
      continue;
    NSeatPolicyRow row;
    row.actions = n.actions;
    row.probabilities.resize(n.actions.size());
    double total = 0.0;
    for (std::size_t a = 0; a < n.actions.size(); ++a) {
      row.probabilities[a] = static_cast<double>(a + 2);
      total += row.probabilities[a];
    }
    for (double& p : row.probabilities)
      p /= total;
    for (std::uint32_t b = 1; b <= 9; ++b)
      rows.emplace(NSeatInformationKey{b, static_cast<std::uint64_t>(i)}, row);
  }
  NSeatPolicy policy = NSeatPolicy::from_parts(def, menu.id(), std::move(rows));

  std::size_t leaf_index = kNoNode;
  for (std::size_t i = 0; i < dl.size(); ++i) {
    if (dl.node(i).kind != NodeKind::TerminalFrontier)
      continue;
    const int rc = dl.node(i).incoming_card;
    if (!(root_mask & (1ULL << rc))) {
      leaf_index = i;
      break;
    }
  }
  CHECK(leaf_index != kNoNode);

  auto exported = export_leaf_continuation_ranges(policy, dl, ranges, leaf_index);
  CHECK(exported.has_value());

  const int river_card = dl.node(leaf_index).incoming_card;
  struct PLE {
    std::size_t parent;
    std::size_t edge;
  };
  std::vector<PLE> path2;
  for (std::size_t cur = leaf_index; cur != dl.root_index();) {
    const TreeNode& n = dl.node(cur);
    const TreeNode& par = dl.node(n.parent);
    std::size_t edge = 0;
    while (par.children[edge] != cur)
      ++edge;
    path2.push_back({n.parent, edge});
    cur = n.parent;
  }
  std::reverse(path2.begin(), path2.end());

  std::array<std::map<int, double>, 2> brute;
  std::array<double, 2> tot{0.0, 0.0};
  for (const auto& a : root)
    for (const auto& b : root) {
      if (a.cards[0] == b.cards[0] || a.cards[0] == b.cards[1] || a.cards[1] == b.cards[0] ||
          a.cards[1] == b.cards[1])
        continue;
      const std::uint64_t mask =
          1ULL << a.cards[0] | 1ULL << a.cards[1] | 1ULL << b.cards[0] | 1ULL << b.cards[1];
      if (mask & (1ULL << river_card))
        continue;
      bool blocked = false;
      for (int k = 0; k < 5; ++k)
        if (mask & (1ULL << (*exported).board[k]))
          blocked = true;
      if (blocked)
        continue;
      double reach = 1.0;
      const std::array<std::array<int, 2>, 2> hands{a.cards, b.cards};
      for (const PLE& e : path2) {
        const TreeNode& n = dl.node(e.parent);
        if (n.kind != NodeKind::Action)
          continue;  // observed river chance edge: no probability factor
        // Independently look up the acting seat's NON-uniform policy row for
        // this deal's bucket on the 4-card turn board (mirrors walk_action's
        // key addressing without sharing the kernel's code path).
        const std::size_t actor = n.actor;
        const std::uint32_t bucket = static_cast<std::uint32_t>(
            bs::abstraction::card_bucket(bs::solver::kNSeatCardKind, hands[actor],
                                         std::span<const int>(def.board.data(), def.board_size)));
        const auto it =
            policy.rows().find(NSeatInformationKey{bucket, static_cast<std::uint64_t>(e.parent)});
        reach *= it->second.probabilities[e.edge];
      }
      const int ca = bs::comboIndex(a.cards[0], a.cards[1]);
      const int cb = bs::comboIndex(b.cards[0], b.cards[1]);
      brute[0][ca] += reach;
      brute[1][cb] += reach;
      tot[0] += reach;
      tot[1] += reach;
    }

  for (std::size_t s = 0; s < 2; ++s) {
    double norm = 0.0;
    for (const auto& wh : (*exported).ranges[s])
      norm += wh.weight;
    CHECK(std::fabs(norm - 1.0) < 1e-12);
    for (const auto& [ci, w] : brute[s]) {
      const double expected = w / tot[s];
      const auto cards = bs::comboCards(ci);
      const auto& out = (*exported).ranges[s];
      auto it = std::find_if(out.begin(), out.end(), [&](const WeightedHand& wh) {
        return wh.cards[0] == cards[0] && wh.cards[1] == cards[1];
      });
      CHECK(it != out.end());
      CHECK(std::fabs(it->weight - expected) < 1e-9);
    }
    CHECK((*exported).ranges[s].size() == brute[s].size());
  }
  return 0;
}

bool cards_collide2(std::array<int, 2> a, std::array<int, 2> b) {
  return a[0] == b[0] || a[0] == b[1] || a[1] == b[0] || a[1] == b[1];
}

int test_nested_model_refine_and_marginals() {
  using namespace bs::solver;
  GameDef def = make_turn_def(2);
  for (auto& st : def.stacks)
    st = 199;
  const ActionAbstraction menu = ActionAbstraction::identity(1);
  AbstractTree dl(def, menu, TreeLimits{}, DepthLimit::streets(1));

  const char* names[10][2] = {{"Ac", "Ad"}, {"Kc", "Kd"}, {"Qc", "Qd"}, {"Jc", "Jd"}, {"Tc", "Td"},
                              {"9c", "9d"}, {"Ac", "Kc"}, {"Ac", "Qc"}, {"Ac", "Kd"}, {"9c", "8c"}};

  std::size_t leaf_index = kNoNode;
  for (std::size_t i = 0; i < dl.size(); ++i) {
    if (dl.node(i).kind == NodeKind::TerminalFrontier) {
      leaf_index = i;
      break;
    }
  }
  CHECK(leaf_index != kNoNode);
  const auto leaf = make_river_boundary_leaf(dl, leaf_index);

  std::uint64_t board_mask = 0;
  for (int k = 0; k < 5; ++k)
    board_mask |= 1ULL << leaf.board()[k];
  std::vector<WeightedHand> seat_range;
  for (auto& n : names) {
    const auto c = std::array<int, 2>{card(n[0]), card(n[1])};
    if (!(board_mask & (1ULL << c[0] | 1ULL << c[1])))
      seat_range.push_back({c, 1.0});
  }
  std::vector<std::vector<WeightedHand>> ranges(2, seat_range);

  gto::NestedRiverValueModel model(dl, /*master_seed=*/7);
  double unused = 0.0;
  CHECK(model.lookup_scalar(leaf, 0, seat_range[0].cards, unused) == gto::ContinuationStatus::Miss);

  gto::ContinuationBudget budget;
  budget.added_iterations = 8000;
  budget.wall = std::chrono::seconds(180);
  CHECK(model.refine(leaf, ranges, ranges, budget) == gto::ContinuationStatus::Solved);

  const gto::ContinuationValues* cv = model.values(leaf);
  CHECK(cv != nullptr);
  CHECK(cv->marginals.size() == 2);
  CHECK(!cv->warm);
  for (std::size_t s = 0; s < 2; ++s)
    CHECK(cv->marginals[s].size() == seat_range.size());

  // Before an absolute gap is recorded as passing, the matrix exists but is
  // NOT served (RFC 0039 fail-closed: stall without a passing absolute
  // measurement never publishes).
  const int first_ci = cv->marginals[0].begin()->first;
  const auto cards0 = bs::comboCards(first_ci);
  double scalar_pre = 0.0;
  CHECK(model.lookup_scalar(leaf, 0, cards0, scalar_pre) == gto::ContinuationStatus::Miss);
  // Simulate the offline driver recording a passing absolute inner gap.
  model.record_inner_gap(leaf, /*gap_chips=*/0.01, /*passed=*/true);

  // lookup_scalar agrees with the stored matrix once the gate has passed.
  double scalar = 0.0;
  CHECK(model.lookup_scalar(leaf, 0, cards0, scalar) == gto::ContinuationStatus::Solved);
  CHECK(std::fabs(scalar - cv->marginals[0].begin()->second) < 1e-12);

  // A re-refine under new (drifted) ranges invalidates the pass flag until
  // the driver re-measures, even though the matrix is refreshed.
  auto drifted2 = ranges;
  drifted2[0].front().weight = 1.3;
  CHECK(model.refine(leaf, drifted2, ranges, budget) == gto::ContinuationStatus::Solved);
  CHECK(model.lookup_scalar(leaf, 0, cards0, scalar) == gto::ContinuationStatus::Miss);
  // Restore the original ranges/gate for the marginal checks below.
  CHECK(model.refine(leaf, ranges, ranges, budget) == gto::ContinuationStatus::Solved);
  model.record_inner_gap(leaf, 0.01, true);
  CHECK(model.lookup_scalar(leaf, 0, cards0, scalar) == gto::ContinuationStatus::Solved);
  // values()/inner_policy()/inner_tree() point at the MOST RECENT refine()
  // artifacts; re-fetch after the re-refines above (the earlier cv pointer
  // was invalidated when the entry was replaced).
  cv = model.values(leaf);
  CHECK(cv != nullptr);

  // Brute-force the SAME conditional expectation the model must compute:
  // for seat p holding c, average the policy-following root utility over
  // card-compatible opponent combos weighted by the opponent marginal
  // (per-own conditional normalization), using the model's own inner policy
  // through extract_root_value.
  const NSeatPolicy* policy = model.inner_policy(leaf);
  const tree::AbstractTree* inner = model.inner_tree(leaf);
  CHECK(policy != nullptr && inner != nullptr);
  const poker::GameState root_state(inner->def());

  auto brute_conditional = [&](std::size_t p, std::array<int, 2> own) {
    double usum = 0.0, wsum = 0.0;
    for (const auto& opp : seat_range) {
      if (cards_collide2(own, opp.cards))
        continue;
      std::vector<std::array<int, 2>> deal(2);
      deal[p] = own;
      deal[1 - p] = opp.cards;
      const std::vector<double> u = bs::gto::extract_root_value(*policy, *inner, root_state, deal);
      usum += opp.weight * u[p];
      wsum += opp.weight;
    }
    return usum / wsum;
  };
  for (std::size_t p = 0; p < 2; ++p)
    for (const auto& wh : seat_range) {
      const int ci = bs::comboIndex(wh.cards[0], wh.cards[1]);
      const double expected = brute_conditional(p, wh.cards);
      CHECK(std::fabs(cv->marginals[p].at(ci) - expected) < 1e-9);
    }

  // Distributional zero-sum under the global joint measure Q: each player's
  // Q-marginal weights the conditional values; the two expectations sum to
  // zero (settlement is zero-sum for every joint deal).
  auto global_expectation = [&](std::size_t p) {
    double z = 0.0, u = 0.0;
    for (const auto& own : seat_range) {
      for (const auto& opp : seat_range) {
        if (cards_collide2(own.cards, opp.cards))
          continue;
        const double w = own.weight * opp.weight;
        const int ci = bs::comboIndex(own.cards[0], own.cards[1]);
        z += w;
        u += w * cv->marginals[p].at(ci);
      }
    }
    return u / z;
  };
  const double e0 = global_expectation(0);
  const double e1 = global_expectation(1);
  CHECK(std::fabs(e0 + e1) < 1e-9);

  // RFC 0012 bit-identical warm resume with UNCHANGED ranges adds iterations
  // cumulatively. This is the fourth refine on identical ranges (the three
  // above exercised gap gating with one drifted round in between that
  // cold-restarted the counter), so pin the exact cumulative total rather
  // than a vacuous lower bound.
  const std::uint64_t before = model.values(leaf)->inner_completed_iterations;
  CHECK(model.refine(leaf, ranges, ranges, budget) == gto::ContinuationStatus::Solved);
  CHECK(model.values(leaf)->warm);
  CHECK(model.values(leaf)->inner_completed_iterations == before + 8000);

  // Under DRIFTING ranges the warm state is only a heuristic start and the
  // acceptance flag is reset (must be re-measured); the refine still
  // succeeds and reports it used the warm candidate for row-shape-identical
  // trees, but its result is not served until a gap is recorded.
  auto drifted = ranges;
  drifted[0].front().weight = 1.7;
  CHECK(model.refine(leaf, drifted, ranges, budget) == gto::ContinuationStatus::Solved);
  double after_drift = 0.0;
  CHECK(model.lookup_scalar(leaf, 0, cards0, after_drift) == gto::ContinuationStatus::Miss);
  return 0;
}

// RFC 0039 AC4: the solve-scoped warm map is keyed by the FRONTIER NODE
// INDEX, so two distinct river leaves never share warm state; and a zero
// added-iteration / malformed-budget request is a Decline rather than an
// exception. Also pins the Miss -> Solved -> scalar lifecycle.
int test_warm_key_isolation_and_statuses() {
  using namespace bs::solver;
  GameDef def = make_turn_def(2);
  for (auto& st : def.stacks)
    st = 199;
  const ActionAbstraction menu = ActionAbstraction::identity(1);
  AbstractTree dl(def, menu, TreeLimits{}, DepthLimit::streets(1));

  std::vector<std::size_t> leaves;
  for (std::size_t i = 0; i < dl.size(); ++i)
    if (dl.node(i).kind == NodeKind::TerminalFrontier)
      leaves.push_back(i);
  CHECK(leaves.size() >= 2);

  gto::NestedRiverValueModel model(dl, 11);
  const auto first = make_river_boundary_leaf(dl, leaves[0]);
  const auto second = make_river_boundary_leaf(dl, leaves[1]);

  // Refine one leaf from uniform policy ranges at the tiny representative
  // support (reuse the leaf-range exporter with a uniform sealed policy).
  auto uniform_policy = [&] {
    std::map<NSeatInformationKey, NSeatPolicyRow> ur;
    for (std::size_t i = 0; i < dl.size(); ++i) {
      const TreeNode& n = dl.node(i);
      if (!n.is_action())
        continue;
      NSeatPolicyRow row;
      row.actions = n.actions;
      row.probabilities.assign(n.actions.size(), 1.0 / static_cast<double>(n.actions.size()));
      for (std::uint32_t b = 1; b <= 9; ++b)
        ur.emplace(NSeatInformationKey{b, static_cast<std::uint64_t>(i)}, row);
    }
    return NSeatPolicy::from_parts(def, menu.id(), std::move(ur));
  }();
  const NSeatPolicy policy = std::move(uniform_policy);
  // Root ranges: blocked 10-hand representative set.
  const char* names[10][2] = {{"Ac", "Ad"}, {"Kc", "Kd"}, {"Qc", "Qd"}, {"Jc", "Jd"}, {"Tc", "Td"},
                              {"9c", "9d"}, {"Ac", "Kc"}, {"Ac", "Qc"}, {"Ac", "Kd"}, {"9c", "8c"}};
  std::vector<std::vector<WeightedHand>> root(2);
  for (std::size_t s = 0; s < 2; ++s)
    for (auto& n : names) {
      int a = card(n[0]), b = card(n[1]);
      if (a > b)
        std::swap(a, b);
      root[s].push_back({{a, b}, 1.0});
    }
  auto r1 = export_leaf_continuation_ranges(policy, dl, root, leaves[0]);
  CHECK(r1.has_value());
  gto::ContinuationBudget budget;
  budget.added_iterations = 2000;
  budget.wall = std::chrono::seconds(120);
  CHECK(model.refine(first, r1->ranges, root, budget) == gto::ContinuationStatus::Solved);
  // The OTHER leaf is still a Miss — warm state never leaks across leaves.
  double v = 0.0;
  CHECK(model.lookup_scalar(second, 0, r1->ranges[0].front().cards, v) ==
        gto::ContinuationStatus::Miss);
  CHECK(model.inner_tree(second) == nullptr);

  // Fail-closed across a FAILED re-refine: certify this leaf's matrix, then
  // have a later refinement Decline under new (drifted) ranges. The old
  // matrix was certified for the old ranges only, so it MUST stop being
  // served — the outer trainer has to fall back to the equity frontier.
  model.record_inner_gap(first, /*gap_chips=*/0.01, /*passed=*/true);
  double certified = 0.0;
  CHECK(model.lookup_scalar(first, 0, r1->ranges[0].front().cards, certified) ==
        gto::ContinuationStatus::Solved);
  gto::ContinuationBudget zero = budget;
  zero.added_iterations = 0;
  CHECK(model.refine(first, r1->ranges, root, zero) == gto::ContinuationStatus::Decline);
  CHECK(model.lookup_scalar(first, 0, r1->ranges[0].front().cards, certified) ==
        gto::ContinuationStatus::Miss);
  // An Invalid call must keep the leaf unserved as well.
  auto bad = r1->ranges;
  bad.push_back({});
  CHECK(model.refine(first, bad, root, budget) == gto::ContinuationStatus::Invalid);
  CHECK(model.lookup_scalar(first, 0, r1->ranges[0].front().cards, certified) ==
        gto::ContinuationStatus::Miss);
  // A later successful refinement plus a fresh passing gap serves again.
  CHECK(model.refine(first, r1->ranges, root, budget) == gto::ContinuationStatus::Solved);
  CHECK(model.lookup_scalar(first, 0, r1->ranges[0].front().cards, certified) ==
        gto::ContinuationStatus::Miss);
  model.record_inner_gap(first, 0.01, true);
  CHECK(model.lookup_scalar(first, 0, r1->ranges[0].front().cards, certified) ==
        gto::ContinuationStatus::Solved);
  return 0;
}

// RFC 0039 convergence-series fixture: nested inner gap must fall as inner
// iterations grow. Here we proxy "inner gap" by the change in root marginal
// magnitude between refinement levels on a tiny 2-seat river subgame (the
// absolute BR-gap driver in the measurement binary supplies the formal
// number; this test pins monotone stabilization of the sealed values).
int test_inner_refinement_stabilizes() {
  using namespace bs::solver;
  GameDef def = make_turn_def(2);
  for (auto& st : def.stacks)
    st = 199;
  const ActionAbstraction menu = ActionAbstraction::identity(1);
  AbstractTree dl(def, menu, TreeLimits{}, DepthLimit::streets(1));
  std::size_t leaf_index = kNoNode;
  for (std::size_t i = 0; i < dl.size(); ++i)
    if (dl.node(i).kind == NodeKind::TerminalFrontier) {
      leaf_index = i;
      break;
    }
  const auto leaf = make_river_boundary_leaf(dl, leaf_index);

  const char* names[10][2] = {{"Ac", "Ad"}, {"Kc", "Kd"}, {"Qc", "Qd"}, {"Jc", "Jd"}, {"Tc", "Td"},
                              {"9c", "9d"}, {"Ac", "Kc"}, {"Ac", "Qc"}, {"Ac", "Kd"}, {"9c", "8c"}};
  std::vector<std::vector<WeightedHand>> root(2);
  std::uint64_t bm = 0;
  for (int k = 0; k < 5; ++k)
    bm |= 1ULL << leaf.board()[k];
  for (std::size_t s = 0; s < 2; ++s)
    for (auto& n : names) {
      int a = card(n[0]), b = card(n[1]);
      if (a > b)
        std::swap(a, b);
      if (!(bm & (1ULL << a | 1ULL << b)))
        root[s].push_back({{a, b}, 1.0});
    }

  gto::NestedRiverValueModel model(dl, 23);
  std::uint64_t cumulative = 0;
  auto run_level = [&](std::uint64_t iters, bool expect_warm) {
    gto::ContinuationBudget b;
    b.added_iterations = iters;
    b.wall = std::chrono::seconds(120);
    CHECK(model.refine(leaf, root, root, b) == gto::ContinuationStatus::Solved);
    const auto* cv = model.values(leaf);
    CHECK(cv != nullptr);
    CHECK((cv->warm && expect_warm) || (!cv->warm && !expect_warm));
    // Cumulative iteration totals: a cold start reports its own count; warm
    // resume adds the new iterations to the prior absolute counter.
    CHECK(cv->inner_completed_iterations == cumulative + iters);
    cumulative += iters;
    double norm = 0.0;
    for (const auto& [ci, val] : cv->marginals[0])
      norm += val * val;
    return norm;
  };
  const double n1 = run_level(1000, /*warm=*/false);
  const double n2 = run_level(3000, /*warm=*/true);
  const double n3 = run_level(9000, /*warm=*/true);
  // Successive refinements change the value vector less as it stabilizes.
  CHECK(std::fabs(n3 - n2) <= std::fabs(n2 - n1) + 1e-6);
  return 0;
}

// RFC 0039 AC2 (blocker B): the nested marginal matrix must match an
// INDEPENDENT direct full solve, not just re-walk the model's own policy.
// On a shallow 2-BB-deep 2-seat fixture the river subgame tree is small.
// Because the worst-combo statistic of two single MCCFR runs is dominated
// by fixed-seed tail noise (it does not shrink monotonically with more
// iterations on one run), each side is solved kRuns times with independent
// seeds and the per-combo conditioned values are averaged: the mean of
// independent unbiased estimators converges to the equilibrium value, and
// the averaging removes the per-run tail. The nested side runs the actual
// NestedRiverValueModel (kRuns cold instances); the oracle side is a
// separately constructed DepthLimit::none() river tree solved directly.
// Every averaged per-seat per-combo value must agree within 0.005 BB
// (0.01 chips at BB = 2).
int test_nested_marginals_match_independent_full_solve() {
  using namespace bs::solver;
  constexpr std::uint64_t kRuns = 3;
  constexpr std::uint64_t kIters = 2'000'000;

  GameDef def = make_turn_def(2);
  for (auto& st : def.stacks)
    st = 4;  // 2 BB total; river tree is minimal and tightly convergable
  const ActionAbstraction menu = ActionAbstraction::identity(1);
  AbstractTree dl(def, menu, TreeLimits{}, DepthLimit::streets(1));

  const char* names[10][2] = {{"Ac", "Ad"}, {"Kc", "Kd"}, {"Qc", "Qd"}, {"Jc", "Jd"}, {"Tc", "Td"},
                              {"9c", "9d"}, {"Ac", "Kc"}, {"Ac", "Qc"}, {"Ac", "Kd"}, {"9c", "8c"}};
  std::vector<WeightedHand> root;
  for (auto& n : names) {
    int a = card(n[0]), b = card(n[1]);
    if (a > b)
      std::swap(a, b);
    root.push_back({{a, b}, 1.0});
  }
  const std::vector<std::vector<WeightedHand>> root_ranges(2, root);

  // Uniform sealed outer policy; export the exact conditioned ranges of the
  // first river frontier leaf (both players checked the turn through).
  std::map<NSeatInformationKey, NSeatPolicyRow> ur;
  for (std::size_t i = 0; i < dl.size(); ++i) {
    const TreeNode& n = dl.node(i);
    if (!n.is_action())
      continue;
    NSeatPolicyRow row;
    row.actions = n.actions;
    row.probabilities.assign(n.actions.size(), 1.0 / static_cast<double>(n.actions.size()));
    for (std::uint32_t b = 1; b <= 9; ++b)
      ur.emplace(NSeatInformationKey{b, static_cast<std::uint64_t>(i)}, row);
  }
  const NSeatPolicy outer_policy = NSeatPolicy::from_parts(def, menu.id(), std::move(ur));

  std::size_t leaf_index = kNoNode;
  for (std::size_t i = 0; i < dl.size(); ++i)
    if (dl.node(i).kind == NodeKind::TerminalFrontier) {
      leaf_index = i;
      break;
    }
  CHECK(leaf_index != kNoNode);
  const auto leaf = make_river_boundary_leaf(dl, leaf_index);
  const auto exported = export_leaf_continuation_ranges(outer_policy, dl, root_ranges, leaf_index);
  CHECK(exported.has_value());
  CHECK(exported->ranges.size() == 2);
  CHECK(!exported->ranges[0].empty() && !exported->ranges[1].empty());

  const GameDef river_def = nested_river_game(leaf);
  AbstractTree oracle_tree(river_def, menu, TreeLimits{}, DepthLimit::none());
  NSeatTrainerLimits limits;
  limits.wall = std::chrono::seconds(300);
  const poker::GameState river_state(river_def);

  // Per-combo conditioned value from a solved policy: average the
  // policy-following root utility over the conditioned opponent distribution.
  auto conditioned_values = [&](const NSeatPolicy& policy, const AbstractTree& tree) {
    std::array<std::map<int, double>, 2> values;
    for (std::size_t p = 0; p < 2; ++p)
      for (const WeightedHand& own : exported->ranges[p]) {
        double usum = 0.0, wsum = 0.0;
        for (const WeightedHand& opp : exported->ranges[1 - p]) {
          if (cards_collide2(own.cards, opp.cards))
            continue;
          std::vector<std::array<int, 2>> deal(2);
          deal[p] = own.cards;
          deal[1 - p] = opp.cards;
          const std::vector<double> u =
              bs::gto::extract_root_value(policy, tree, river_state, deal);
          usum += opp.weight * u[p];
          wsum += opp.weight;
        }
        CHECK(wsum > 0.0);
        values[p][bs::comboIndex(own.cards[0], own.cards[1])] = usum / wsum;
      }
    return values;
  };

  std::array<std::map<int, double>, 2> inner_mean{};
  std::array<std::map<int, double>, 2> oracle_mean{};
  gto::ContinuationBudget budget;
  budget.added_iterations = kIters;
  budget.wall = std::chrono::seconds(300);

  for (std::uint64_t run = 0; run < kRuns; ++run) {
    // Nested side: a fresh cold model per independent seed.
    gto::NestedRiverValueModel model(dl, /*master_seed=*/0xBEEF + run * 7919);
    CHECK(model.refine(leaf, exported->ranges, root_ranges, budget) ==
          gto::ContinuationStatus::Solved);
    const gto::ContinuationValues* cv = model.values(leaf);
    CHECK(cv != nullptr);
    for (std::size_t p = 0; p < 2; ++p)
      for (const auto& [ci, v] : cv->marginals[p])
        inner_mean[p][ci] += v / static_cast<double>(kRuns);

    // Oracle side: direct full solve of the same game, independent seeds.
    const NSeatTrainingResult oracle =
        train_nseat(oracle_tree, exported->ranges, kIters,
                    /*master_seed=*/987'654'321 + run * 104'729, limits);
    auto values = conditioned_values(oracle.policy, oracle_tree);
    for (std::size_t p = 0; p < 2; ++p)
      for (const auto& [ci, v] : values[p])
        oracle_mean[p][ci] += v / static_cast<double>(kRuns);
  }

  // Release builds (g++ -O3, the AC2 evidence configuration) enforce the
  // strict 0.005 BB/combo gate. Debug/ASan builds use a different compiler
  // and optimization level (clang++ -O0); floating-point codegen
  // differences flip rare sampled-action branches over millions of MCCFR
  // iterations, so each build follows a deterministic but different MC
  // trajectory with a larger worst-combo tail (observed 0.025 BB). ASan's
  // job is memory/lifetime checking, and 0.05 BB still bounds the result
  // tightly enough to reject a wiring bug (which produces 0.1+ BB); the
  // tight AC2 band is enforced by the release ctest.
#ifdef NDEBUG
  constexpr double kToleranceChips = 0.01;  // 0.005 BB at BB = 2
  constexpr const char* kGateName = "0.005 BB (release AC2 gate)";
#else
  constexpr double kToleranceChips = 0.10;  // 0.05 BB debug/ASan sanity band
  constexpr const char* kGateName = "0.05 BB (debug/ASan sanity band)";
#endif
  double worst = 0.0;
  for (std::size_t p = 0; p < 2; ++p) {
    CHECK(inner_mean[p].size() == oracle_mean[p].size());
    for (const auto& [ci, nested_value] : inner_mean[p]) {
      const auto it = oracle_mean[p].find(ci);
      CHECK(it != oracle_mean[p].end());
      worst = std::max(worst, std::fabs(nested_value - it->second));
    }
  }
  std::fprintf(stderr,
               "nested oracle (%llu-seed mean, %llu iters/run): worst per-combo delta %.6f "
               "chips (%.5f BB; gate %s)\n",
               static_cast<unsigned long long>(kRuns), static_cast<unsigned long long>(kIters),
               worst, worst / static_cast<double>(def.big_blind), kGateName);
  if (worst >= kToleranceChips)
    CHECK(!"nested marginals must match the independent full solve within the per-build band");
  return 0;
}

}  // namespace

int main() {
  if (test_two_seat_river_subtree_matches_reference())
    return 1;
  if (test_three_seat_cap_one_nested_tree())
    return 1;
  if (test_folded_dead_money_kept())
    return 1;
  if (test_fail_closed())
    return 1;
  if (test_leaf_ranges_differential())
    return 1;
  if (test_nested_model_refine_and_marginals())
    return 1;
  if (test_warm_key_isolation_and_statuses())
    return 1;
  if (test_inner_refinement_stabilizes())
    return 1;
  if (test_nested_marginals_match_independent_full_solve())
    return 1;
  std::printf("test_nested_river_value: all checks passed\n");
  return 0;
}
