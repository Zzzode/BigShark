#include "partition_walker.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <map>
#include <stdexcept>

namespace bs::rfc0046 {
namespace {

using bs::poker::Action;
using bs::poker::GameState;
using bs::poker::Phase;
using bs::poker::Street;
using bs::solver::WeightedHand;
using bs::tree::AbstractTree;

constexpr std::size_t kSeats = 2;

struct View {
  std::array<std::array<int, 2>, kSeats> holes{};
  double reach = 0.0;
};

struct Pooled {
  double value = 0.0;
  double mass = 0.0;
};

struct TermAccum {
  std::array<double, 4> sum{};
  double extension = 0.0;
};

struct Walker {
  const PartitionInput& in;
  const AbstractTree& tree0;
  const std::size_t traverser;
  const std::size_t opponent;

  TermAccum terms;
  double mass_total = 0.0;

  Walker(const PartitionInput& input, std::size_t p)
      : in(input), tree0(*input.tree0), traverser(p), opponent(p == 0 ? 1 : 0) {}

  std::int64_t node1_of(std::size_t n0) const {
    return in.node1_map ? (*in.node1_map)[n0] : static_cast<std::int64_t>(n0);
  }

  static bool collide(const std::array<int, 2>& a, const std::array<int, 2>& b) {
    return a[0] == b[0] || a[0] == b[1] || a[1] == b[0] || a[1] == b[1];
  }

  std::map<std::uint64_t, std::vector<View>> holding_groups() const {
    std::map<std::uint64_t, std::vector<View>> groups;
    for (const WeightedHand& own : in.ranges[traverser])
      for (const WeightedHand& opp : in.ranges[opponent]) {
        if (collide(own.cards, opp.cards))
          continue;
        View v;
        v.holes[traverser] = own.cards;
        v.holes[opponent] = opp.cards;
        v.reach = own.weight * opp.weight;
        const std::uint64_t key =
            (static_cast<std::uint64_t>(std::min(own.cards[0], own.cards[1])) << 8) |
            static_cast<std::uint64_t>(std::max(own.cards[0], own.cards[1]));
        groups[key].push_back(v);
      }
    return groups;
  }

  static double terminal_util(const AbstractTree& tree, const GameState& state, std::size_t n0,
                              const View& v, std::size_t seat) {
    const auto& payload = tree.terminal(tree.node(n0).terminal);
    if (state.phase() == Phase::Folded)
      return static_cast<double>(payload.chip_utility[seat]);
    std::array<std::array<int, 2>, 10> live_holes{};
    for (std::size_t i = 0; i < payload.live_count; ++i)
      live_holes[i] = v.holes[payload.live_order[i]];
    return static_cast<double>(state
                                   .settle_showdown(std::span<const std::array<int, 2>>(
                                       live_holes.data(), payload.live_count))
                                   .chip_utility[seat]);
  }

  std::vector<double> policy(std::size_t seat, std::size_t n0, const std::array<int, 2>& own,
                             const std::vector<Action>& menu) const {
    std::vector<double> out(menu.size(), 0.0);
    in.policy->probabilities(seat, n0, node1_of(n0), own, menu, std::span<double>(out));
    double total = 0.0;
    for (double p : out)
      total += p;
    if (out.empty() || std::abs(total - 1.0) > 1e-9)
      throw std::runtime_error("partition policy returned a non-distribution");
    return out;
  }

  static std::size_t board_ordinal(const GameState& state, int card) {
    std::size_t ordinal = 0;
    for (int c = 0; c < card; ++c) {
      bool on_board = false;
      for (int b : state.board())
        if (b == c)
          on_board = true;
      if (!on_board)
        ++ordinal;
    }
    return ordinal;
  }

  // Best-response-tail pooled walk. At traverser nodes every menu action is
  // evaluated and the per-holding g(I) increment is emitted into terms; the
  // best BR-tail value returns. own_prefix is the traverser's all-H own
  // action reach to this node.
  double br_walk(const GameState& state, std::size_t n0, std::vector<View> views,
                 double own_prefix) {
    std::vector<View*> live;
    for (View& v : views)
      if (v.reach > 0.0)
        live.push_back(&v);
    if (live.empty())
      return 0.0;
    const auto& node = tree0.node(n0);

    if (node.is_terminal()) {
      double sum = 0.0;
      for (const View* v : live)
        sum += v->reach * terminal_util(tree0, state, n0, *v, traverser);
      return sum;
    }

    if (node.is_chance()) {
      double sum = 0.0;
      for (const View* v : live) {
        std::array<bool, 52> used{};
        for (int c : state.board())
          used[c] = true;
        used[v->holes[0][0]] = used[v->holes[0][1]] = true;
        used[v->holes[1][0]] = used[v->holes[1][1]] = true;
        std::size_t avail = 0;
        for (int c = 0; c < 52; ++c)
          if (!used[c])
            ++avail;
        for (int card = 0; card < 52; ++card) {
          if (used[card])
            continue;
          View child = *v;
          child.reach /= static_cast<double>(avail);
          sum += br_walk(state.after_card(card), node.children[board_ordinal(state, card)],
                         std::vector<View>{child}, own_prefix);
        }
      }
      return sum;
    }

    const std::size_t actor = *state.actor();
    const auto& menu = node.actions;

    if (actor == traverser) {
      // One infoset per call: all live views share the traverser holding.
      const auto own = live.front()->holes[actor];
      const std::vector<double> sigma = policy(traverser, n0, own, menu);
      std::vector<double> child(menu.size(), 0.0);
      for (std::size_t a = 0; a < menu.size(); ++a) {
        std::vector<View> branch;
        branch.reserve(live.size());
        for (View* v : live)
          branch.push_back(*v);
        child[a] = br_walk(state.after_action(actor, menu[a]), node.children[a], std::move(branch),
                           own_prefix * sigma[a]);
      }
      std::size_t best = 0;
      for (std::size_t a = 1; a < menu.size(); ++a)
        if (child[a] > child[best])
          best = a;
      double under_h = 0.0;
      for (std::size_t a = 0; a < menu.size(); ++a)
        under_h += sigma[a] * child[a];
      const double g = own_prefix * (child[best] - under_h);

      const std::int64_t n1 = node1_of(n0);
      PartitionTerm label;
      if (n1 < 0) {
        label = PartitionTerm::Action;
        terms.extension += g;
      } else {
        // Rule 2 by ACTION MEMBERSHIP, not index prefix: the BR argmax is a
        // cap-0-only action iff it is absent from the mapped cap-1 node's
        // menu. Ties prefer the in-menu action by the strict-inequality argmax
        // above (lowest index wins ties), matching the TS evaluator (M3).
        bool inside_cap1 = true;
        if (in.tree1) {
          const auto& cap1_menu = in.tree1->node(static_cast<std::size_t>(n1)).actions;
          inside_cap1 = false;
          for (const auto& cap1_action : cap1_menu)
            if (cap1_action == menu[best]) {
              inside_cap1 = true;
              break;
            }
        }
        if (!inside_cap1)
          label = PartitionTerm::Action;
        else if (node.street == Street::Turn)
          label = PartitionTerm::Boundary;
        else if (node.street == Street::River)
          label = PartitionTerm::River;
        else
          label = PartitionTerm::Trunk;
        terms.sum[static_cast<std::size_t>(label)] += g;
      }
      return child[best];
    }

    // Opponent node: branch under the opponent's concrete H policy.
    double sum = 0.0;
    for (std::size_t a = 0; a < menu.size(); ++a) {
      std::vector<View> branch;
      branch.reserve(live.size());
      for (View* v : live) {
        const std::vector<double> sigma = policy(opponent, n0, v->holes[opponent], menu);
        if (sigma[a] > 0.0) {
          View child = *v;
          child.reach *= sigma[a];
          branch.push_back(child);
        }
      }
      if (!branch.empty())
        sum += br_walk(state.after_action(actor, menu[a]), node.children[a], std::move(branch),
                       own_prefix);
    }
    return sum;
  }

  // Independent all-H expectation walk (no best response), used only to
  // verify the telescoping identity.
  double h_walk(const GameState& state, std::size_t n0, std::vector<View> views) {
    std::vector<View*> live;
    for (View& v : views)
      if (v.reach > 0.0)
        live.push_back(&v);
    if (live.empty())
      return 0.0;
    const auto& node = tree0.node(n0);
    if (node.is_terminal()) {
      double sum = 0.0;
      for (const View* v : live)
        sum += v->reach * terminal_util(tree0, state, n0, *v, traverser);
      return sum;
    }
    if (node.is_chance()) {
      double sum = 0.0;
      for (const View* v : live) {
        std::array<bool, 52> used{};
        for (int c : state.board())
          used[c] = true;
        used[v->holes[0][0]] = used[v->holes[0][1]] = true;
        used[v->holes[1][0]] = used[v->holes[1][1]] = true;
        std::size_t avail = 0;
        for (int c = 0; c < 52; ++c)
          if (!used[c])
            ++avail;
        for (int card = 0; card < 52; ++card) {
          if (used[card])
            continue;
          View child = *v;
          child.reach /= static_cast<double>(avail);
          sum += h_walk(state.after_card(card), node.children[board_ordinal(state, card)],
                        std::vector<View>{child});
        }
      }
      return sum;
    }
    const std::size_t actor = *state.actor();
    const auto& menu = node.actions;
    double sum = 0.0;
    for (std::size_t a = 0; a < menu.size(); ++a) {
      std::vector<View> branch;
      branch.reserve(live.size());
      for (View* v : live) {
        const std::vector<double> sigma = policy(actor, n0, v->holes[actor], menu);
        if (sigma[a] > 0.0) {
          View child = *v;
          child.reach *= sigma[a];
          branch.push_back(child);
        }
      }
      if (!branch.empty())
        sum += h_walk(state.after_action(actor, menu[a]), node.children[a], std::move(branch));
    }
    return sum;
  }

  double group_mass(const std::vector<View>& views) const {
    double m = 0.0;
    for (const View& v : views)
      m += v.reach;
    return m;
  }
};

}  // namespace

std::vector<std::int64_t> map_cap1_nodes(const AbstractTree& tree0, const AbstractTree& tree1) {
  std::vector<std::int64_t> mapping(tree0.size(), -1);
  struct Frame {
    std::size_t n0;
    std::size_t n1;
  };
  std::vector<Frame> stack{{tree0.root_index(), tree1.root_index()}};
  while (!stack.empty()) {
    const Frame f = stack.back();
    stack.pop_back();
    mapping[f.n0] = static_cast<std::int64_t>(f.n1);
    const auto& a = tree0.node(f.n0);
    const auto& b = tree1.node(f.n1);
    if (a.is_chance() && b.is_chance()) {
      for (std::size_t i = 0; i < a.children.size(); ++i) {
        const int card = tree0.node(a.children[i]).incoming_card;
        for (std::size_t j = 0; j < b.children.size(); ++j)
          if (tree1.node(b.children[j]).incoming_card == card) {
            stack.push_back({a.children[i], b.children[j]});
            break;
          }
      }
    } else if (a.is_action() && b.is_action()) {
      // The cap-1 menu must be an ORDER-PRESERVING PREFIX of the cap-0 menu:
      // tree0 children 0..b.actions.size()-1 pair with the cap-1 actions in
      // order. This is what makes lowest-index argmax tie-breaking equivalent
      // to "prefer the in-menu action" (finding n2). Fail closed if the menu
      // builders ever diverge.
      if (b.actions.size() > a.actions.size())
        throw std::runtime_error("map_cap1_nodes: cap-1 menu larger than cap-0 menu");
      for (std::size_t j = 0; j < b.actions.size(); ++j)
        if (a.actions[j] != b.actions[j])
          throw std::runtime_error("map_cap1_nodes: cap-1 menu is not an ordered prefix of cap-0");
      for (std::size_t i = 0; i < b.actions.size(); ++i)
        stack.push_back({a.children[i], b.children[i]});
    }
  }
  return mapping;
}

PartitionResult partition_nash_conv(const PartitionInput& input) {
  if (!input.tree0 || !input.keyer || !input.policy)
    throw std::invalid_argument("partition_nash_conv: missing input");
  PartitionResult out;
  const double pot = static_cast<double>(input.tree0->def().pot);
  const double bb = static_cast<double>(input.tree0->def().big_blind);

  double nc_chip = 0.0;
  std::array<double, 4> terms{};
  double extension = 0.0;
  std::array<double, 2> per_player_resid{};
  const GameState root(input.tree0->def());

  for (std::size_t p = 0; p < kSeats; ++p) {
    Walker walker(input, p);
    auto groups = walker.holding_groups();
    double br_total = 0.0;
    double h_total = 0.0;
    double mass = 0.0;
    for (auto& [key, views] : groups) {
      (void)key;
      const double m = walker.group_mass(views);
      mass += m;
      br_total += walker.br_walk(root, input.tree0->root_index(), views, 1.0);
    }
    for (auto& [key, views] : groups) {
      (void)key;
      h_total += walker.h_walk(root, input.tree0->root_index(), views);
    }
    if (mass <= 0.0)
      throw std::runtime_error("partition_nash_conv: zero deal mass");
    const double diff = (br_total - h_total) / mass;
    double term_total = 0.0;
    for (std::size_t i = 0; i < 4; ++i) {
      walker.terms.sum[i] /= mass;
      terms[i] += walker.terms.sum[i];
      term_total += walker.terms.sum[i];
    }
    extension += walker.terms.extension / mass;
    per_player_resid[p] = std::abs(diff - term_total) / pot;
    nc_chip += diff;
    out.joint_deals = std::max(out.joint_deals, groups.size());
  }

  // Frozen metric is the SUMMED chance-free NashConv in BB (addendum §5/§8;
  // RFC 0045 gate precedent compute_nash_conv(...).nash_conv / big_blind, with
  // no live-seat divisor). nc_chip is already the sum over both traversers, so
  // the BB conversion divides by BB ONLY - not by seat count (finding B4).
  out.nash_conv_chip = nc_chip;
  out.nash_conv_bb = nc_chip / bb;
  for (std::size_t i = 0; i < 4; ++i) {
    out.terms_chip[i] = terms[i];
    out.terms_bb[i] = terms[i] / bb;
  }
  out.extension_mass_chip = extension;
  out.residual_pot = per_player_resid;
  return out;
}

}  // namespace bs::rfc0046
