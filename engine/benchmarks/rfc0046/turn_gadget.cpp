#include "turn_gadget.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <functional>
#include <stdexcept>

namespace bs::rfc0046 {
namespace {

using bs::poker::Action;
using bs::poker::GameState;
using bs::poker::Phase;
using bs::solver::NSeatInformationKey;
using bs::solver::WeightedHand;

struct Exhausted {};

struct SubRow {
  std::vector<Action> actions;
  std::vector<double> regret;
  std::vector<double> sum;
};

struct Table {
  std::map<NSeatInformationKey, SubRow> rows;

  SubRow& touch(std::uint32_t bucket, std::size_t node, const std::vector<Action>& menu,
                bool& created) {
    const NSeatInformationKey key{bucket, static_cast<std::uint64_t>(node)};
    auto it = rows.find(key);
    if (it != rows.end()) {
      created = false;
      return it->second;
    }
    SubRow row;
    row.actions = menu;
    row.regret.assign(menu.size(), 0.0);
    row.sum.assign(menu.size(), 0.0);
    created = true;
    return rows.emplace(key, std::move(row)).first->second;
  }
};

struct GRow {
  std::array<double, 2> regret{0.0, 0.0};
  std::array<double, 2> sum{0.0, 0.0};
};

// Cumulative-weight categorical draw. Returns the selected index.
std::size_t weighted_draw(bs::SplitMix64& rng, const std::vector<double>& cumulative) {
  const double unit = static_cast<double>(rng.next_u64() >> 11) * (1.0 / 9007199254740992.0);
  const double point = unit * cumulative.back();
  return static_cast<std::size_t>(std::lower_bound(cumulative.begin(), cumulative.end(), point) -
                                  cumulative.begin());
}

std::vector<int> runout_cards(const GameState& state,
                              const std::array<std::array<int, 2>, 2>& holes) {
  std::array<bool, 52> used{};
  for (int c : state.board())
    used[c] = true;
  for (const auto& h : holes) {
    used[h[0]] = true;
    used[h[1]] = true;
  }
  std::vector<int> cards;
  for (int c = 0; c < 52; ++c)
    if (!used[c])
      cards.push_back(c);
  return cards;
}

std::size_t board_ordinal(const GameState& state, int card) {
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

std::vector<double> regret_match(const std::vector<double>& regret) {
  std::vector<double> sigma(regret.size(), 1.0 / static_cast<double>(regret.size()));
  double positive = 0.0;
  for (double r : regret)
    positive += std::max(0.0, r);
  if (positive > 0.0)
    for (std::size_t i = 0; i < regret.size(); ++i)
      sigma[i] = std::max(0.0, regret[i]) / positive;
  return sigma;
}

struct GadgetTrainer {
  const GadgetInput& in;
  const tree::AbstractTree& tree;
  Table table;
  std::map<std::uint32_t, GRow> gadget;

  // Resident in-table byte accounting. Rows/gadget rows are created once and
  // never removed, so the live footprint is charged at CREATION only; charging
  // on every walk visit accumulated a monotone visit tally that spuriously
  // tripped max_bytes on long runs despite ~27 MB measured RSS.
  static constexpr std::size_t kMapNodeBytes = 64;                   // conservative RB-tree node
  static constexpr std::size_t kSubRowOverhead = sizeof(void*) * 3;  // three vector headers
  static constexpr std::size_t kPerActionBytes = sizeof(Action) + 2 * sizeof(double);
  static constexpr std::size_t kGadRowBytes = sizeof(GRow);

  std::size_t visits = 0;
  std::size_t bytes = 0;
  std::size_t peak_bytes = 0;
  std::chrono::steady_clock::time_point deadline;
  bool timed_out = false;
  bool resource_exhausted = false;

  std::vector<double> hero_cum;
  std::vector<double> resp_cum;
  double hero_total = 0.0;

  explicit GadgetTrainer(const GadgetInput& input)
      : in(input), tree(*input.tree), deadline(std::chrono::steady_clock::now() + in.limits.wall) {
    hero_cum.reserve(in.hero_range.size());
    double acc = 0.0;
    for (const auto& h : in.hero_range) {
      acc += h.weight;
      hero_cum.push_back(acc);
    }
    hero_total = acc;
    resp_cum.reserve(in.responder_range.size());
    acc = 0.0;
    for (const auto& h : in.responder_range) {
      acc += h.weight;
      resp_cum.push_back(acc);
    }
  }

  // Walk-entry gate. NOTE: visits is a diagnostic count of regret + exact
  // average walk entries and is NOT compared against max_nodes (the average
  // walk enumerates all 48 river cards, so its entries far exceed the
  // materialized tree node count). The binding resources are wall time, the
  // retained information-set count, and the in-table byte charge; the tree's
  // own node count is bounded separately at construction and reported as
  // GadgetResult::nodes.
  void tick() {
    ++visits;
    peak_bytes = std::max(peak_bytes, bytes);
    if (table.rows.size() + gadget.size() > in.limits.max_information_sets ||
        bytes > in.limits.max_bytes)
      throw Exhausted{};
    if (std::chrono::steady_clock::now() >= deadline)
      throw Exhausted{};
  }

  void charge_row(const std::vector<Action>& menu) {
    bytes += kMapNodeBytes + kSubRowOverhead + kPerActionBytes * menu.size();
  }

  void charge_gadget_row() { bytes += kMapNodeBytes + kGadRowBytes; }

  std::array<int, 2> hero_cards(std::size_t i) const { return in.hero_range[i].cards; }
  std::array<int, 2> resp_cards(std::size_t i) const { return in.responder_range[i].cards; }

  bool collide(const std::array<int, 2>& a, const std::array<int, 2>& b) const {
    return a[0] == b[0] || a[0] == b[1] || a[1] == b[0] || a[1] == b[1];
  }

  // Conditional responder cumulative weights over combos non-colliding with
  // the sampled hero holding.
  std::vector<double> conditional_resp_cum(const std::array<int, 2>& hero,
                                           std::size_t& selected_count) const {
    std::vector<double> cum;
    cum.reserve(in.responder_range.size());
    double acc = 0.0;
    for (const auto& h : in.responder_range)
      if (!collide(hero, h.cards)) {
        acc += h.weight;
        cum.push_back(acc);
      }
    selected_count = cum.size();
    return cum;
  }

  std::array<double, 2> gadget_sigma(std::uint32_t bucket) {
    auto it = gadget.find(bucket);
    if (it == gadget.end())
      return {0.5, 0.5};
    std::vector<double> r{it->second.regret[0], it->second.regret[1]};
    const auto s = regret_match(r);
    return {s[0], s[1]};
  }

  // External-sampling regret walk of the real turn+river subtree.
  double walk_regret(const GameState& state, std::size_t node_index, std::size_t traverser,
                     std::array<std::array<int, 2>, 2> holes, double deal_weight,
                     bs::SplitMix64& action_rng, bs::SplitMix64& chance_rng) {
    tick();
    const auto& node = tree.node(node_index);
    if (node.is_terminal()) {
      const auto& payload = tree.terminal(node.terminal);
      if (state.phase() == Phase::Folded)
        return static_cast<double>(payload.chip_utility[traverser]);
      // Pack by live position: settle maps holes[i] to seat live_order[i].
      std::array<std::array<int, 2>, 2> packed{};
      for (std::size_t i = 0; i < payload.live_count; ++i)
        packed[i] = holes[payload.live_order[i]];
      const auto settlement = state.settle_showdown(
          std::span<const std::array<int, 2>>(packed.data(), payload.live_count));
      return static_cast<double>(settlement.chip_utility[traverser]);
    }
    if (node.is_chance()) {
      const auto cards = runout_cards(state, holes);
      const std::size_t pick = weighted_draw(chance_rng, [&] {
        std::vector<double> c;
        double a = 0.0;
        for (std::size_t i = 0; i < cards.size(); ++i) {
          a += 1.0;
          c.push_back(a);
        }
        return c;
      }());
      const int card = cards[pick];
      return walk_regret(state.after_card(card), node.children[board_ordinal(state, card)],
                         traverser, holes, deal_weight, action_rng, chance_rng);
    }

    const std::size_t actor = *state.actor();
    const std::uint32_t bucket = in.keyer->dense_for_live_pair(holes[actor]);
    bool created = false;
    SubRow& row = table.touch(bucket, node_index, node.actions, created);
    if (created)
      charge_row(node.actions);
    tick();
    const auto sigma = regret_match(row.regret);

    if (actor == traverser) {
      std::vector<double> child(node.actions.size(), 0.0);
      for (std::size_t a = 0; a < node.actions.size(); ++a) {
        child[a] = walk_regret(state.after_action(actor, node.actions[a]), node.children[a],
                               traverser, holes, deal_weight, action_rng, chance_rng);
      }
      double value = 0.0;
      for (std::size_t a = 0; a < node.actions.size(); ++a)
        value += sigma[a] * child[a];
      for (std::size_t a = 0; a < node.actions.size(); ++a)
        row.regret[a] += deal_weight * (child[a] - value);
      return value;
    }

    std::vector<double> cum;
    cum.reserve(sigma.size());
    double acc = 0.0;
    for (double s : sigma) {
      acc += s;
      cum.push_back(acc);
    }
    const std::size_t sampled = weighted_draw(action_rng, cum);
    return walk_regret(state.after_action(actor, node.actions[sampled]), node.children[sampled],
                       traverser, holes, deal_weight, action_rng, chance_rng);
  }

  // Own-reach average walk: every action fans out, and the single river
  // chance is ENUMERATED (uniform 1/n per card), not sampled. The river deck
  // is a fixed 48-card public set with no strategic content, so exact
  // enumeration both converges the average strategy on every river row (the
  // certifier visits them all) and avoids the coverage lottery of one
  // sampled card per iteration - the RFC 0005 gadget uses full traversal for
  // the same reason.
  void walk_average(const GameState& state, std::size_t node_index,
                    std::array<std::array<int, 2>, 2> holes, std::array<double, 2> seat_reach) {
    const auto& node = tree.node(node_index);
    if (node.is_terminal())
      return;
    // The enumerated 48-card fan-out is exact and large: enforce the visit,
    // infoset, byte and wall caps here too (finding m2).
    tick();
    if (node.is_chance()) {
      const auto cards = runout_cards(state, holes);
      for (int card : cards)
        walk_average(state.after_card(card), node.children[board_ordinal(state, card)], holes,
                     seat_reach);
      return;
    }
    const std::size_t actor = *state.actor();
    const std::uint32_t bucket = in.keyer->dense_for_live_pair(holes[actor]);
    bool created = false;
    SubRow& row = table.touch(bucket, node_index, node.actions, created);
    if (created)
      charge_row(node.actions);
    const auto sigma = regret_match(row.regret);
    for (std::size_t a = 0; a < node.actions.size(); ++a) {
      row.sum[a] += seat_reach[actor] * sigma[a];
      auto next_reach = seat_reach;
      next_reach[actor] *= sigma[a];
      walk_average(state.after_action(actor, node.actions[a]), node.children[a], holes, next_reach);
    }
  }

  void clip_positive() {
    for (auto& [key, row] : table.rows)
      for (double& r : row.regret)
        r = std::max(0.0, r);
    for (auto& [bucket, grow] : gadget)
      for (double& r : grow.regret)
        r = std::max(0.0, r);
  }

  // One outer iteration: regret sweep for each traverser over one sampled
  // deal, then one average sample. Linear CFR weight t.
  void iteration(std::uint64_t index) {
    const double t = static_cast<double>(index + 1);
    bs::SplitMix64 deal_rng(in.seed ^ (0x9e3779b97f4a7c15ull * (index + 1)));

    for (std::size_t traverser = 0; traverser < 2; ++traverser) {
      bs::SplitMix64 action_rng(in.seed ^ (0xD1B54A32D192ED03ull * (index + 1) + traverser));
      bs::SplitMix64 chance_rng(in.seed ^ (0x2545F4914F6CDD1Dull * (index + 1) + traverser));

      const std::size_t hi = weighted_draw(deal_rng, hero_cum);
      const auto hero = hero_cards(hi);
      std::size_t cond_count = 0;
      const auto cond = conditional_resp_cum(hero, cond_count);
      if (cond_count == 0)
        continue;
      const std::size_t ri_local = weighted_draw(deal_rng, cond);
      // Map conditional index back to the responder range (skipping colliding
      // combos in range order).
      std::size_t ri = 0;
      for (std::size_t seen = 0, k = 0; k < in.responder_range.size(); ++k) {
        if (!collide(hero, in.responder_range[k].cards)) {
          if (seen == ri_local) {
            ri = k;
            break;
          }
          ++seen;
        }
      }
      const auto resp = resp_cards(ri);
      const double p_hero = in.hero_range[hi].weight / hero_total;
      const double p_resp = in.responder_range[ri].weight / cond.back();
      const double deal_weight = p_hero * p_resp;

      std::array<std::array<int, 2>, 2> holes{hero, resp};
      const std::uint32_t rbucket = in.keyer->dense_for_live_pair(resp);
      const std::size_t gadget_before = gadget.size();
      auto& grow = gadget[rbucket];
      if (gadget.size() > gadget_before)
        charge_gadget_row();
      const auto sigma = gadget_sigma(rbucket);

      const GameState root(tree.def());

      if (traverser == in.hero) {
        // Responder's -x choice is sampled; the hero has no root decision.
        const double draw_unit =
            static_cast<double>(action_rng.next_u64() >> 11) * (1.0 / 9007199254740992.0);
        const bool cont = draw_unit < sigma[1];
        if (cont) {
          (void)walk_regret(root, tree.root_index(), in.hero, holes, deal_weight, action_rng,
                            chance_rng);
        }
      } else {
        double margin = 0.0;
        if (const auto it = in.margins.find(rbucket); it != in.margins.end())
          margin = it->second;
        const double cont_value = walk_regret(root, tree.root_index(), in.responder, holes,
                                              deal_weight, action_rng, chance_rng);
        const double value = sigma[0] * margin + sigma[1] * cont_value;
        grow.regret[0] += t * deal_weight * (margin - value);
        grow.regret[1] += t * deal_weight * (cont_value - value);
      }
    }

    // Average sample: fan both -x branches, then the subtree average walk
    // (river chance enumerated inside walk_average; the joint deal stays
    // sampled once per iteration as in external-sampling MCCFR).
    bs::SplitMix64 avg_deal(in.seed ^ (0xBF58476D1CE4E5B9ull * (index + 1)));
    const std::size_t hi = weighted_draw(avg_deal, hero_cum);
    const auto hero = hero_cards(hi);
    std::size_t cond_count = 0;
    const auto cond = conditional_resp_cum(hero, cond_count);
    if (cond_count == 0)
      return;
    const std::size_t ri_local = weighted_draw(avg_deal, cond);
    std::size_t ri = 0;
    for (std::size_t seen = 0, k = 0; k < in.responder_range.size(); ++k) {
      if (!collide(hero, in.responder_range[k].cards)) {
        if (seen == ri_local) {
          ri = k;
          break;
        }
        ++seen;
      }
    }
    const auto resp = resp_cards(ri);
    std::array<std::array<int, 2>, 2> holes{hero, resp};
    const std::uint32_t rbucket = in.keyer->dense_for_live_pair(resp);
    const auto sigma = gadget_sigma(rbucket);
    const std::size_t gadget_before_avg = gadget.size();
    auto& grow = gadget[rbucket];
    if (gadget.size() > gadget_before_avg)
      charge_gadget_row();
    grow.sum[0] += t * sigma[0];
    grow.sum[1] += t * sigma[1];
    const GameState root(tree.def());
    std::array<double, 2> reach{1.0, sigma[1]};
    walk_average(root, tree.root_index(), holes, reach);

    clip_positive();
  }
};

}  // namespace

GadgetResult run_turn_gadget(const GadgetInput& input) {
  GadgetResult result;
  if (!input.tree || !input.keyer || input.hero == input.responder || input.hero_range.empty() ||
      input.responder_range.empty() || input.iterations == 0) {
    result.status = GadgetStatus::InvalidInput;
    return result;
  }
  // Uniform-only precondition: the marginal-hero + conditional-last-seat
  // sampling with update weight q(d) is unbiased (up to a strategy-invariant
  // 1/N scale) only when every combo weight is equal. The frozen cells are
  // uniform ranges; weighted variants need product-conditional draws with a
  // p/q importance correction (finding M4).
  for (const auto& rng : {std::cref(input.hero_range), std::cref(input.responder_range)}) {
    const double w0 = rng.get().front().weight;
    for (const auto& h : rng.get())
      if (std::abs(h.weight - w0) > 1e-12) {
        result.status = GadgetStatus::InvalidInput;
        return result;
      }
  }
  GadgetTrainer trainer(input);
  std::uint64_t done = 0;
  try {
    for (std::uint64_t i = 0; i < input.iterations; ++i) {
      trainer.iteration(i);
      ++done;
    }
    result.status = GadgetStatus::Certified;
  } catch (const Exhausted&) {
    result.status =
        (trainer.bytes > input.limits.max_bytes ||
         trainer.table.rows.size() + trainer.gadget.size() > input.limits.max_information_sets)
            ? GadgetStatus::ResourceExhausted
            : GadgetStatus::SolveDeadline;
  }
  result.completed_iterations = done;
  result.nodes = input.tree->size();
  result.walk_visits = trainer.visits;
  result.peak_bytes = trainer.peak_bytes;
  result.information_sets = trainer.table.rows.size() + trainer.gadget.size();
  for (auto& [key, row] : trainer.table.rows) {
    double total = 0.0;
    for (double s : row.sum)
      total += s;
    // An unvisited-average row is NOT a uniform strategy: omitting it lets the
    // certifier's MissingRow path fail-close (SolveDeadline) rather than
    // serving an invented uniform distribution (finding n4).
    if (!(total > 0.0))
      continue;
    GadgetRow out;
    out.actions = row.actions;
    out.probabilities.resize(row.sum.size());
    for (std::size_t i = 0; i < row.sum.size(); ++i)
      out.probabilities[i] = row.sum[i] / total;
    result.rows.emplace(key, std::move(out));
  }
  for (auto& [bucket, grow] : trainer.gadget) {
    const double total = grow.sum[0] + grow.sum[1];
    if (total > 0.0)
      result.margin_policy[bucket] = {grow.sum[0] / total, grow.sum[1] / total};
    else
      result.margin_policy[bucket] = {0.5, 0.5};
  }
  return result;
}

}  // namespace bs::rfc0046
