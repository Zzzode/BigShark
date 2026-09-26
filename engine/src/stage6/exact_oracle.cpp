#include <array>
#include <bs/stage6/exact_oracle.hpp>
#include <bs/stage6/infoset_key.hpp>
#include <cmath>
#include <map>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

namespace bs::stage6 {
namespace {

namespace poker = bs::poker;

std::vector<LoggedAction>& street_log_ref(HandLog& log, poker::Street street) {
  switch (street) {
    case poker::Street::Preflop:
      return log.preflop;
    case poker::Street::Flop:
      return log.flop;
    case poker::Street::Turn:
      return log.turn;
    case poker::Street::River:
      return log.river;
  }
  return log.river;
}

// Full recursion for one fixed joint hole deal. `used_holes` is the union of
// every seat's two hole cards, so chance averages over runouts conditioned on
// the FULL deal (including seats that later fold), matching the L1
// after_card rule and the 2p precedent. When `omni_seat` is a seat, that seat
// picks the best deviation action SEPARATELY FOR THIS DEAL — an omniscient
// quantity (it can see opponent holdings) exposed only as the named upper
// bound exact_omniscient_deviation_utility. `omni_seat == nullopt` is the
// joint-profile expectation.
std::array<double, 10> recurse(poker::GameState state, const std::vector<HoleCards>& holes,
                               const std::vector<const BehaviorPolicy*>& policies, HandLog log,
                               std::optional<std::size_t> omni_seat, OracleCounts& counts) {
  std::array<double, 10> value{};
  switch (state.phase()) {
    case poker::Phase::Action: {
      ++counts.action_nodes;
      const std::size_t seat = *state.actor();
      const poker::LegalActions legal = state.legal();

      // Omniscient per-deal deviation (diagnostic only).
      if (omni_seat && seat == *omni_seat) {
        const std::vector<poker::Action> menu = declared_behavior_menu(state, seat);
        if (menu.empty())
          throw std::runtime_error("best response deviation menu is empty");
        bool have_best = false;
        for (const poker::Action& action : menu) {
          if (!legal.contains(action))
            throw std::runtime_error("declared deviation action is not legal");
          HandLog next_log = log;
          street_log_ref(next_log, state.street()).push_back(LoggedAction{seat, action});
          const std::array<double, 10> child =
              recurse(state.after_action(seat, action), holes, policies, std::move(next_log),
                      omni_seat, counts);
          if (!have_best || child[seat] > value[seat]) {
            value = child;
            have_best = true;
          }
        }
        return value;
      }

      PolicyContext ctx;
      ctx.hand_log = &log;
      ctx.decision_seed = 0x6f7261636c65ULL;  // fixed; oracle policies are deterministic
      const std::vector<PolicyAction> dist =
          policies[seat]->distribution(state, seat, holes[seat], ctx);
      if (dist.empty())
        throw std::runtime_error("oracle policy returned an empty distribution");
      double total = 0.0;
      for (const PolicyAction& pa : dist) {
        // std::isfinite is required: a NaN mass makes BOTH `< 0` and
        // `> 1+eps` false and the final `abs(total-1) > eps` false as well, so
        // without it a NaN probability would silently poison this exact R9
        // reference value. This mirrors require_finite_distribution.
        if (!std::isfinite(pa.probability) || pa.probability < 0.0)
          throw std::runtime_error("oracle policy probability must be finite and >= 0");
        if (!legal.contains(pa.action))
          throw std::runtime_error("oracle policy selected an illegal action");
        total += pa.probability;
      }
      if (std::abs(total - 1.0) > 1e-9)
        throw std::runtime_error("oracle policy distribution does not sum to 1");
      for (const PolicyAction& pa : dist) {
        HandLog next_log = log;
        street_log_ref(next_log, state.street()).push_back(LoggedAction{seat, pa.action});
        const std::array<double, 10> child =
            recurse(state.after_action(seat, pa.action), holes, policies, std::move(next_log),
                    omni_seat, counts);
        for (std::size_t s = 0; s < state.player_count(); ++s)
          value[s] += pa.probability * child[s];
      }
      return value;
    }
    case poker::Phase::Deal: {
      ++counts.chance_nodes;
      std::array<bool, 52> used{};
      for (int card : state.board())
        used[card] = true;
      for (const HoleCards& h : holes)
        for (int card : h)
          used[card] = true;
      std::vector<int> available;
      for (int card = 0; card < 52; ++card)
        if (!used[card])
          available.push_back(card);
      if (available.empty())
        throw std::runtime_error("oracle deal node has no available runout card");
      const double inv = 1.0 / static_cast<double>(available.size());
      for (int card : available) {
        const std::array<double, 10> child =
            recurse(state.after_card(card), holes, policies, log, omni_seat, counts);
        for (std::size_t s = 0; s < state.player_count(); ++s)
          value[s] += inv * child[s];
      }
      return value;
    }
    case poker::Phase::Folded: {
      ++counts.fold_leaves;
      const poker::ContributionSettlement settlement = state.settle_fold();
      for (std::size_t s = 0; s < state.player_count(); ++s)
        value[s] = static_cast<double>(settlement.chip_utility[s]);
      return value;
    }
    case poker::Phase::Showdown: {
      ++counts.showdown_leaves;
      std::vector<std::array<int, 2>> live_holes;
      for (std::size_t seat : state.live_players())
        live_holes.push_back(holes[seat]);
      const poker::ContributionSettlement settlement = state.settle_showdown(live_holes);
      for (std::size_t s = 0; s < state.player_count(); ++s)
        value[s] = static_cast<double>(settlement.chip_utility[s]);
      return value;
    }
  }
  return value;  // unreachable; switch covers every Phase
}

struct BRView {
  std::vector<HoleCards> holes;
  double weight = 0.0;  // unconditional joint-deal weight
  double reach = 0.0;   // probability of reaching this public node (fixed BR + opp/chance)
};

// An unnormalized reach-weighted value and the mass it is summed over.
struct PooledValue {
  std::array<double, 10> value{};
  double mass = 0.0;
};

// Runout cards available to a view: 52 minus the public board and ALL fixed
// hole cards of that deal (including seats that later fold).
std::size_t available_count(const poker::GameState& state, const BRView& v) {
  std::array<bool, 52> used{};
  for (int card : state.board())
    used[card] = true;
  for (const HoleCards& h : v.holes)
    for (int card : h)
      used[card] = true;
  std::size_t count = 0;
  for (int card = 0; card < 52; ++card)
    if (!used[card])
      ++count;
  return count;
}

// Best-response walk for ONE fixed traverser holding shared by every view (so
// a traverser node is one information set and the chosen action pools opponent
// holdings). Returns the UNNORMALIZED sum over views of weight*reach*payoff and
// its total mass; the caller normalizes/combines across traverser holdings.
// Mirrors heads_up response(): traverser nodes compare unnormalized candidate
// sums (incoming reach is identical across actions), opponent nodes weight by
// per-view policy probability, deal nodes split reach over conditional runouts.
PooledValue br_group(poker::GameState state, std::vector<BRView> views,
                     const std::vector<const BehaviorPolicy*>& policies, HandLog log,
                     std::size_t traverser, OracleCounts& counts,
                     ExactBrChoiceSink* sink = nullptr) {
  PooledValue out;
  std::vector<BRView*> live;
  for (BRView& v : views)
    if (v.reach > 0.0)
      live.push_back(&v);
  if (live.empty())
    return out;

  switch (state.phase()) {
    case poker::Phase::Action: {
      ++counts.action_nodes;
      const std::size_t seat = *state.actor();
      const poker::LegalActions legal = state.legal();

      if (seat == traverser) {
        // One action for the whole information set. Pick the deviation action
        // with the best POOLED traverser value (max over unnormalized sums; the
        // incoming mass is shared by every candidate so the ordering equals
        // max over conditional means). Ties resolve to the first menu action.
        const std::vector<poker::Action> menu = declared_behavior_menu(state, seat);
        if (menu.empty())
          throw std::runtime_error("best response deviation menu is empty");
        bool have_best = false;
        PooledValue best;
        poker::Action best_action = menu.front();
        for (const poker::Action& action : menu) {
          if (!legal.contains(action))
            throw std::runtime_error("declared deviation action is not legal");
          std::vector<BRView> children;
          children.reserve(live.size());
          for (BRView* v : live)
            children.push_back({v->holes, v->weight, v->reach});  // fixed action, reach unchanged
          HandLog next_log = log;
          street_log_ref(next_log, state.street()).push_back(LoggedAction{seat, action});
          PooledValue child = br_group(state.after_action(seat, action), std::move(children),
                                       policies, std::move(next_log), traverser, counts, sink);
          if (!have_best || child.value[seat] > best.value[seat]) {
            best = std::move(child);
            best_action = action;
            have_best = true;
          }
        }
        if (sink) {
          // Every view at this node shares the traverser's own holding (the
          // walk is invoked per own-holding group), so the node is one
          // information set and gets one exported choice.
          const HoleCards own = live.front()->holes[traverser];
          const InfosetKey key = make_infoset_key(state, log, traverser, own);
          sink->menus[key] = menu;
          sink->actions[key] = best_action;
        }
        return best;
      }

      // Opponent node: branch on each public action; a view's child reach is
      // its parent reach times the probability that opponent assigns that
      // action conditional on the view's (that opponent's) holding.
      PolicyContext ctx;
      ctx.hand_log = &log;
      ctx.decision_seed = 0x6f7261636c65ULL;
      // Cache each view's action probabilities once.
      std::vector<std::vector<PolicyAction>> dists(live.size());
      for (std::size_t i = 0; i < live.size(); ++i) {
        dists[i] = policies[seat]->distribution(state, seat, live[i]->holes[seat], ctx);
        double pt = 0.0;
        for (const PolicyAction& pa : dists[i]) {
          if (!std::isfinite(pa.probability) || pa.probability < 0.0)
            throw std::runtime_error("best response opponent probability must be finite and >=0");
          if (!legal.contains(pa.action))
            throw std::runtime_error("best response opponent selected an illegal action");
          pt += pa.probability;
        }
        if (std::abs(pt - 1.0) > 1e-9)
          throw std::runtime_error("best response opponent distribution does not sum to 1");
      }
      // Public branch actions: the union over views, in a stable legal order.
      std::vector<poker::Action> branches;
      for (const auto& dist : dists)
        for (const PolicyAction& pa : dist) {
          bool seen = false;
          for (const poker::Action& a : branches)
            if (a == pa.action)
              seen = true;
          if (!seen)
            branches.push_back(pa.action);
        }
      for (const poker::Action& action : branches) {
        std::vector<BRView> children;
        children.reserve(live.size());
        for (std::size_t i = 0; i < live.size(); ++i) {
          double p = 0.0;
          for (const PolicyAction& pa : dists[i])
            if (pa.action == action)
              p = pa.probability;
          if (p > 0.0)
            children.push_back({live[i]->holes, live[i]->weight, live[i]->reach * p});
        }
        if (children.empty())
          continue;
        HandLog next_log = log;
        street_log_ref(next_log, state.street()).push_back(LoggedAction{seat, action});
        PooledValue child = br_group(state.after_action(seat, action), std::move(children),
                                     policies, std::move(next_log), traverser, counts, sink);
        for (std::size_t s = 0; s < state.player_count(); ++s)
          out.value[s] += child.value[s];
        out.mass += child.mass;
      }
      return out;
    }
    case poker::Phase::Deal: {
      ++counts.chance_nodes;
      // Public card dealt to every view, but a view splits reach only over the
      // cards available conditional on its full fixed deal.
      for (int card = 0; card < 52; ++card) {
        std::vector<BRView> children;
        children.reserve(live.size());
        for (BRView* v : live) {
          std::array<bool, 52> used{};
          for (int c : state.board())
            used[c] = true;
          for (const HoleCards& h : v->holes)
            for (int c : h)
              used[c] = true;
          if (used[card])
            continue;
          const double avail = static_cast<double>(available_count(state, *v));
          if (avail == 0.0)
            throw std::runtime_error("best response deal node has no runout card");
          children.push_back({v->holes, v->weight, v->reach / avail});
        }
        if (children.empty())
          continue;
        PooledValue child = br_group(state.after_card(card), std::move(children), policies, log,
                                     traverser, counts, sink);
        for (std::size_t s = 0; s < state.player_count(); ++s)
          out.value[s] += child.value[s];
        out.mass += child.mass;
      }
      return out;
    }
    case poker::Phase::Folded: {
      ++counts.fold_leaves;
      const poker::ContributionSettlement settlement = state.settle_fold();
      for (BRView* v : live)
        out.mass += v->weight * v->reach;
      for (std::size_t s = 0; s < state.player_count(); ++s)
        out.value[s] = out.mass * static_cast<double>(settlement.chip_utility[s]);
      return out;
    }
    case poker::Phase::Showdown: {
      ++counts.showdown_leaves;
      // Each view resolves the same public board with its own holdings.
      for (BRView* v : live) {
        std::vector<std::array<int, 2>> live_holes;
        for (std::size_t seat : state.live_players())
          live_holes.push_back(v->holes[seat]);
        const poker::ContributionSettlement settlement = state.settle_showdown(live_holes);
        const double w = v->weight * v->reach;
        out.mass += w;
        for (std::size_t s = 0; s < state.player_count(); ++s)
          out.value[s] += w * static_cast<double>(settlement.chip_utility[s]);
      }
      return out;
    }
  }
  return out;
}

}  // namespace

std::array<double, 10> exact_deal_utility(const poker::GameDef& def,
                                          const std::vector<HoleCards>& holes,
                                          const std::vector<const BehaviorPolicy*>& policies,
                                          OracleCounts* counts) {
  if (holes.size() != policies.size() || holes.size() != def.player_count)
    throw std::invalid_argument("oracle: one hole pair and policy per seat required");
  OracleCounts local;
  OracleCounts& c = counts ? *counts : local;
  poker::GameState state(def);
  return recurse(std::move(state), holes, policies, HandLog{}, std::nullopt, c);
}

std::array<double, 10> exact_oracle_utility(
    const poker::GameDef& def,
    const std::vector<std::vector<bs::solver::MultiwayWeightedHand>>& ranges,
    const std::vector<const BehaviorPolicy*>& policies, OracleCounts* counts) {
  if (ranges.size() != policies.size() || ranges.size() != def.player_count)
    throw std::invalid_argument("oracle: one range and policy per seat required");
  std::vector<int> board(def.board.begin(), def.board.begin() + def.board_size);
  const bs::solver::JointDealTable table = bs::solver::enumerate_joint_deals(ranges, board);
  OracleCounts local;
  OracleCounts& c = counts ? *counts : local;
  c.joint_deals = table.deals.size();

  std::array<double, 10> value{};
  for (const bs::solver::MultiwayDeal& deal : table.deals) {
    std::vector<HoleCards> holes;
    holes.reserve(deal.hands.size());
    for (const std::array<int, 2>& hand : deal.hands)
      holes.push_back(HoleCards{hand[0], hand[1]});
    const std::array<double, 10> per_deal =
        recurse(poker::GameState(def), holes, policies, HandLog{}, std::nullopt, c);
    for (std::size_t s = 0; s < def.player_count; ++s)
      value[s] += deal.weight * per_deal[s];
  }
  return value;
}

std::array<double, 10> exact_omniscient_deviation_utility(
    const poker::GameDef& def,
    const std::vector<std::vector<bs::solver::MultiwayWeightedHand>>& ranges,
    const std::vector<const BehaviorPolicy*>& policies, std::size_t traverser,
    OracleCounts* counts) {
  if (ranges.size() != policies.size() || ranges.size() != def.player_count)
    throw std::invalid_argument("oracle: one range and policy per seat required");
  if (traverser >= policies.size())
    throw std::invalid_argument("deviation traverser is not a seated policy");
  std::vector<int> board(def.board.begin(), def.board.begin() + def.board_size);
  const bs::solver::JointDealTable table = bs::solver::enumerate_joint_deals(ranges, board);
  OracleCounts local;
  OracleCounts& c = counts ? *counts : local;
  c.joint_deals = table.deals.size();

  std::array<double, 10> value{};
  for (const bs::solver::MultiwayDeal& deal : table.deals) {
    std::vector<HoleCards> holes;
    holes.reserve(deal.hands.size());
    for (const std::array<int, 2>& hand : deal.hands)
      holes.push_back(HoleCards{hand[0], hand[1]});
    const std::array<double, 10> per_deal =
        recurse(poker::GameState(def), holes, policies, HandLog{},
                std::optional<std::size_t>{traverser}, c);
    for (std::size_t s = 0; s < def.player_count; ++s)
      value[s] += deal.weight * per_deal[s];
  }
  return value;
}

std::array<double, 10> exact_best_response_utility(
    const poker::GameDef& def,
    const std::vector<std::vector<bs::solver::MultiwayWeightedHand>>& ranges,
    const std::vector<const BehaviorPolicy*>& policies, std::size_t traverser, OracleCounts* counts,
    ExactBrChoiceSink* sink) {
  if (ranges.size() != policies.size() || ranges.size() != def.player_count)
    throw std::invalid_argument("oracle: one range and policy per seat required");
  if (traverser >= policies.size())
    throw std::invalid_argument("best response traverser is not a seated policy");
  std::vector<int> board(def.board.begin(), def.board.begin() + def.board_size);
  const bs::solver::JointDealTable table = bs::solver::enumerate_joint_deals(ranges, board);
  OracleCounts local;
  OracleCounts& c = counts ? *counts : local;
  c.joint_deals = table.deals.size();

  // Group joint deals by the traverser's OWN holding: that pair plus the public
  // history defines its information set, and one deviation action is fixed per
  // group before pooling opponent holdings. Each group walk returns reach*weight
  // unnormalized sums; combine across groups and normalize by total mass.
  std::map<std::pair<int, int>, std::vector<BRView>> groups;
  for (const bs::solver::MultiwayDeal& deal : table.deals) {
    std::vector<HoleCards> holes;
    holes.reserve(deal.hands.size());
    for (const std::array<int, 2>& hand : deal.hands)
      holes.push_back(HoleCards{hand[0], hand[1]});
    const HoleCards own = holes[traverser];
    groups[{std::min(own[0], own[1]), std::max(own[0], own[1])}].push_back(
        {std::move(holes), deal.weight, 1.0});
  }

  PooledValue total;
  for (auto& [key, views] : groups) {
    (void)key;
    PooledValue g =
        br_group(poker::GameState(def), std::move(views), policies, HandLog{}, traverser, c, sink);
    for (std::size_t s = 0; s < def.player_count; ++s)
      total.value[s] += g.value[s];
    total.mass += g.mass;
  }
  if (!(total.mass > 0.0))
    throw std::runtime_error("best response accumulated zero mass");
  std::array<double, 10> value{};
  const double inv = 1.0 / total.mass;
  for (std::size_t s = 0; s < def.player_count; ++s)
    value[s] = total.value[s] * inv;
  return value;
}

}  // namespace bs::stage6
