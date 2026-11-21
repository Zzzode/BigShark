#include "certifier.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <optional>
#include <stdexcept>
#include <vector>

namespace bs::rfc0046 {
namespace {

using bs::poker::Action;
using bs::poker::GameState;
using bs::poker::Phase;
using bs::solver::NSeatInformationKey;
using bs::solver::WeightedHand;

struct MissingRow : std::runtime_error {
  std::size_t node = 0;
  std::array<int, 2> hero{-1, -1};
  MissingRow(std::size_t node_index, std::array<int, 2> hero_cards)
      : std::runtime_error("certifier: missing/incompatible hero candidate row"),
        node(node_index),
        hero(hero_cards) {}
};

// One joint deal compatible with a fixed responder holding: the responder's
// cards are constant across views; the view carries the hero combo and the
// deal's reach weight.
struct CView {
  std::array<int, 2> hero{};
  double reach = 0.0;
};

struct Pooled {
  double value = 0.0;
  double mass = 0.0;
};

// Exact pooled best-response walker for ONE fixed responder combo. Views flow
// together: at a responder node one menu action is chosen from the pooled
// value over every compatible hero deal; at a hero node each view branches
// under that hero's concrete candidate policy; chance splits each view over
// its own legal river cards. This is the same information-set-consistent
// pooling discipline as the engine's pooled BR NashConv walker.
struct CertWalker {
  const CertifierInput& in;
  const tree::AbstractTree& tree;
  const std::array<int, 2> resp;

  CertWalker(const CertifierInput& input, const std::array<int, 2>& responder_holding)
      : in(input), tree(*input.tree), resp(responder_holding) {}

  static bool collide(const std::array<int, 2>& a, const std::array<int, 2>& b) {
    return a[0] == b[0] || a[0] == b[1] || a[1] == b[0] || a[1] == b[1];
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

  std::vector<double> hero_sigma(std::size_t node_index, const std::array<int, 2>& hero,
                                 const std::vector<Action>& menu) const {
    const std::uint32_t hb = in.keyer->dense_for_live_pair(hero);
    const auto it =
        in.policy->find(NSeatInformationKey{hb, static_cast<std::uint64_t>(node_index)});
    if (it == in.policy->end() || it->second.actions != menu ||
        it->second.probabilities.size() != menu.size())
      throw MissingRow(node_index, hero);
    return it->second.probabilities;
  }

  double terminal_util(const GameState& state, std::size_t node_index,
                       const std::array<int, 2>& hero) const {
    const auto& payload = tree.terminal(tree.node(node_index).terminal);
    if (state.phase() == Phase::Folded)
      return static_cast<double>(payload.chip_utility[in.responder]);
    std::array<std::array<int, 2>, 2> packed{};
    for (std::size_t i = 0; i < payload.live_count; ++i) {
      const std::size_t seat = payload.live_order[i];
      packed[i] = (seat == in.responder) ? resp : hero;
    }
    return static_cast<double>(
        state
            .settle_showdown(std::span<const std::array<int, 2>>(packed.data(), payload.live_count))
            .chip_utility[in.responder]);
  }

  Pooled walk(const GameState& state, std::size_t node_index, std::vector<CView> views) {
    std::vector<CView*> live;
    live.reserve(views.size());
    for (CView& v : views)
      if (v.reach > 0.0)
        live.push_back(&v);
    Pooled out;
    if (live.empty())
      return out;

    const auto& node = tree.node(node_index);
    if (node.is_terminal()) {
      for (const CView* v : live) {
        const double u = terminal_util(state, node_index, v->hero);
        out.value += v->reach * u;
        out.mass += v->reach;
      }
      return out;
    }

    if (node.is_chance()) {
      for (const CView* v : live) {
        std::array<bool, 52> used{};
        for (int c : state.board())
          used[c] = true;
        used[resp[0]] = used[resp[1]] = true;
        used[v->hero[0]] = used[v->hero[1]] = true;
        std::size_t avail = 0;
        for (int c = 0; c < 52; ++c)
          if (!used[c])
            ++avail;
        for (int card = 0; card < 52; ++card) {
          if (used[card])
            continue;
          CView child = *v;
          child.reach /= static_cast<double>(avail);
          const Pooled sub =
              walk(state.after_card(card), node.children[board_ordinal(state, card)], {child});
          out.value += sub.value;
          out.mass += sub.mass;
        }
      }
      return out;
    }

    const std::size_t actor = *state.actor();
    const auto& menu = node.actions;

    if (actor == in.responder) {
      // One pooled best action for the responder infoset (fixed combo).
      Pooled best;
      bool have = false;
      for (std::size_t a = 0; a < menu.size(); ++a) {
        std::vector<CView> branch;
        branch.reserve(live.size());
        for (CView* v : live)
          branch.push_back(*v);  // fixed action: reach unchanged
        const Pooled sub =
            walk(state.after_action(in.responder, menu[a]), node.children[a], std::move(branch));
        if (!have || sub.value > best.value) {
          best = sub;
          have = true;
        }
      }
      return best;
    }

    // Hero node: per-view concrete candidate probabilities.
    for (std::size_t a = 0; a < menu.size(); ++a) {
      std::vector<CView> branch;
      branch.reserve(live.size());
      for (CView* v : live) {
        const std::vector<double> sigma = hero_sigma(node_index, v->hero, menu);
        if (sigma[a] > 0.0) {
          CView child = *v;
          child.reach *= sigma[a];
          branch.push_back(child);
        }
      }
      if (!branch.empty()) {
        const Pooled sub =
            walk(state.after_action(in.hero, menu[a]), node.children[a], std::move(branch));
        out.value += sub.value;
        out.mass += sub.mass;
      }
    }
    return out;
  }

  // Expected responder continuation value when best-responding, normalized by
  // total compatible deal mass.
  double continuation() {
    std::vector<CView> views;
    double total = 0.0;
    for (const WeightedHand& h : in.hero_range) {
      if (collide(resp, h.cards))
        continue;
      views.push_back(CView{h.cards, h.weight});
      total += h.weight;
    }
    if (views.empty() || total <= 0.0)
      return 0.0;
    const GameState root(tree.def());
    const Pooled value = walk(root, tree.root_index(), std::move(views));
    return value.value / total;
  }
};

}  // namespace

CertifierResult certify_turn_subgame(const CertifierInput& input) {
  CertifierResult out;
  if (!input.tree || !input.keyer || !input.policy)
    return out;
  const double tol = input.pot_tolerance * static_cast<double>(input.tree->def().pot);
  out.max_slack = -std::numeric_limits<double>::infinity();
  out.certified = true;

  for (const WeightedHand& rc : input.responder_range) {
    bool any_hero = false;
    for (const WeightedHand& h : input.hero_range)
      if (!CertWalker::collide(rc.cards, h.cards)) {
        any_hero = true;
        break;
      }
    if (!any_hero)
      continue;  // zero-mass combo: not certified, not counted
    CertWalker walker(input, rc.cards);
    double cont = 0.0;
    try {
      cont = walker.continuation();
    } catch (const MissingRow& e) {
      // Underconverged solve: a positive-mass reached hero infoset has no
      // row. This is a deadline/convergence miss, not a bound-unreachable
      // result.
      out.certified = false;
      out.reason = CertifierFailReason::MissingRow;
      out.max_slack = std::numeric_limits<double>::infinity();
      out.missing_hero_node = static_cast<std::uint32_t>(e.node);
      out.missing_hero_cards = e.hero;
      return out;
    }
    const std::uint32_t rbucket = input.keyer->dense_for_live_pair(rc.cards);
    const auto margin_it = input.margins.find(rbucket);
    // Gate inputs must carry a margin for every positive-mass responder
    // combo; silently defaulting to 0 would bias the bound (finding m7).
    if (margin_it == input.margins.end() && input.require_all_margins)
      throw std::runtime_error("certifier: no locked margin for positive-mass responder combo");
    const double baseline = margin_it == input.margins.end() ? 0.0 : margin_it->second;
    const double slack = cont - baseline - tol;
    out.margins[rbucket] = {baseline, cont};
    ++out.positive_combos;
    if (slack > out.max_slack) {
      out.max_slack = slack;
      out.worst_bucket = rbucket;
    }
    if (slack > 0.0) {
      out.certified = false;
      out.reason = CertifierFailReason::BoundExceeded;
    }
  }
  return out;
}

}  // namespace bs::rfc0046
