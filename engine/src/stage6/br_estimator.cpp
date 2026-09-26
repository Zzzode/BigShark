#include <algorithm>
#include <array>
#include <bs/abstract_tree.hpp>
#include <bs/stage6/br_estimator.hpp>
#include <bs/stage6/crn_streams.hpp>
#include <bs/stage6/exact_oracle.hpp>
#include <cmath>
#include <limits>
#include <numeric>
#include <unordered_set>
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

void require_finite_legal_distribution(const std::vector<PolicyAction>& dist,
                                       const poker::LegalActions& legal, const char* what) {
  if (dist.empty())
    throw stage6_br_error(std::string(what) + " returned an empty distribution");
  double total = 0.0;
  for (const PolicyAction& pa : dist) {
    if (!std::isfinite(pa.probability) || pa.probability < 0.0)
      throw stage6_br_error(std::string(what) + " probability must be finite and >= 0");
    if (!legal.contains(pa.action))
      throw stage6_br_error(std::string(what) + " selected an illegal action");
    total += pa.probability;
  }
  if (std::abs(total - 1.0) > 1e-9)
    throw stage6_br_error(std::string(what) + " distribution does not sum to 1");
}

// Uniform rejection-free bounded index over a content-addressed unit stream.
// Rejections consume successive counters, never a shared mutable RNG. The
// accepted draw's full coordinates are reported so a pairing gate can record
// them.
std::size_t bounded_crn_index(std::uint64_t seed, std::uint64_t node_token, BrStreamPurpose purpose,
                              std::size_t seat, std::size_t bound, std::uint64_t* draws = nullptr,
                              CrnDrawEvent* accepted_out = nullptr) {
  const auto limit = static_cast<std::uint64_t>(bound);
  const std::uint64_t threshold = (std::numeric_limits<std::uint64_t>::max() - limit + 1) % limit;
  std::uint64_t counter = 0;
  for (;;) {
    const std::uint64_t raw = crn_u64(seed, node_token, purpose, seat, counter);
    if (draws)
      ++*draws;
    if (raw >= threshold) {
      if (accepted_out)
        *accepted_out = {node_token, purpose, seat, counter, raw};
      return static_cast<std::size_t>(raw % limit);
    }
    ++counter;
  }
}

// Unit draw from a raw CRN value (top 53 bits * 2^-53), matching crn_unit so
// a recorded raw value and the draw sampled from it are one operation.
double unit_from_u64(std::uint64_t raw) {
  return static_cast<double>(raw >> 11) * 0x1.0p-53;
}

// The deviator's menu under the configured space: the R8 declared 5-fraction
// menu, or the exact coarse abstraction the bound artifact was trained in.
// Opponent seats never use this — they play their native BehaviorPolicy.
std::vector<poker::Action> deviation_menu(const BrEstimatorConfig& cfg,
                                          const poker::GameState& state, std::size_t seat) {
  if (cfg.deviation_space == DeviationSpace::CoarseAbstraction) {
    if (cfg.coarse_action == nullptr)
      throw stage6_br_error("coarse deviation space requires a non-null coarse_action");
    return bs::tree::abstract_node_menu(state, *cfg.coarse_action, state.legal());
  }
  return declared_behavior_menu(state, seat);
}

// Identity stamped into a frozen table: the declared menu hash, or a tagged
// hash of the coarse abstraction digest so a coarse-space table can never be
// mistaken for a declared-menu table by content_hash or a consumer.
std::uint64_t deviation_menu_identity(const BrEstimatorConfig& cfg) {
  if (cfg.deviation_space != DeviationSpace::CoarseAbstraction)
    return declared_menu_identity_hash();
  if (cfg.coarse_action == nullptr)
    throw stage6_br_error("coarse deviation space requires a non-null coarse_action");
  std::uint64_t hash = 0xcbf29ce484222325ULL;
  auto eat = [&hash](std::uint64_t value) {
    for (unsigned shift = 0; shift < 64; shift += 8) {
      hash ^= static_cast<unsigned char>((value >> shift) & 0xffULL);
      hash *= 0x100000001b3ULL;
    }
  };
  eat(0x636f6172736531ULL);  // "coarse1"
  eat(cfg.coarse_action->id().digest);
  return hash;
}

// The per-run estimator: one game definition, joint-deal measure and fixed
// policies. The learn and confirm walks share only these immutable inputs.
class DeviationEstimator {
 public:
  DeviationEstimator(const poker::GameDef& def,
                     const std::vector<std::vector<bs::solver::MultiwayWeightedHand>>& ranges,
                     const std::vector<const BehaviorPolicy*>& policies,
                     const bs::solver::JointDealTable* enumerated, const BrEstimatorConfig& config)
      : def_(def), ranges_(ranges), policies_(policies), enumerated_(enumerated), cfg_(config) {
    n_ = policies_.size();
    if (n_ < 2 || n_ > 10)
      throw std::invalid_argument("br estimator requires 2..10 seat policies");
    if (def_.player_count != n_ || ranges_.size() != n_)
      throw std::invalid_argument("br estimator requires one range and policy per seat");
    for (const BehaviorPolicy* p : policies_)
      if (!p)
        throw std::invalid_argument("br estimator policy is null");
    board_prefix_.reserve(def_.board_size);
    for (int i = 0; i < def_.board_size; ++i)
      board_prefix_.push_back(def_.board[i]);
  }

  // --- Phase 1: learn --------------------------------------------------------

  FrozenBestResponseData learn(std::size_t traverser, std::span<const std::uint64_t> learn_seeds,
                               std::uint64_t seed_hash, BrEstimatorCounts& counts) {
    std::map<InfosetKey, std::vector<poker::Action>> menus;
    std::map<InfosetKey, poker::Action> current;  // b_{k-1}
    std::map<InfosetKey, std::vector<QCell>> q;

    bool stabilized = false;
    std::size_t epoch = 0;
    for (; epoch < cfg_.max_epochs; ++epoch) {
      std::map<InfosetKey, std::vector<QCell>> next_q;
      for (std::uint64_t seed : learn_seeds) {
        const std::vector<HoleCards> holes = draw_joint_holes(seed, counts);
        LearnWalk walk(*this, traverser, seed, holes, menus, current, next_q, counts);
        (void)walk.run(poker::GameState(def_), HandLog{});
      }
      std::map<InfosetKey, poker::Action> improved;
      for (auto& [key, cells] : next_q) {
        // First menu action attaining the strict maximum; identical tie rule
        // to the exact br_group scan.
        std::size_t best = 0;
        for (std::size_t j = 1; j < cells.size(); ++j)
          if (cells[j].mean() > cells[best].mean())
            best = j;
        improved.emplace(key, menus.at(key)[best]);
      }
      q = std::move(next_q);
      // After epoch 0 `current` is the lazily-seeded bootstrap b_0 for exactly
      // the keys the epoch visited (the walk seeds a b_0 row the first time it
      // touches each key), and the visited-key set is constant across epochs,
      // so equality here is a genuine fixed-point test from the first epoch on.
      if (improved == current) {
        stabilized = true;
        current = std::move(improved);
        break;
      }
      current = std::move(improved);
    }

    if (!stabilized && cfg_.require_stabilization)
      throw stage6_br_not_stabilized(
          "br estimator learn table did not reach a fixed point within max_epochs");

    FrozenBestResponseData data;
    data.traverser = traverser;
    data.actions = current;
    data.menus = menus;
    data.q = std::move(q);
    data.menu_identity_hash = deviation_menu_identity(cfg_);
    data.learn_seed_list_hash = seed_hash;
    data.learn_seed_count = learn_seeds.size();
    data.epochs_run = stabilized ? epoch + 1 : epoch;
    data.stabilized = stabilized;
    counts.learn_rollouts += static_cast<std::uint64_t>(learn_seeds.size()) *
                             static_cast<std::uint64_t>(data.epochs_run);
    counts.epochs_per_traverser.push_back(data.epochs_run);
    counts.infosets_per_traverser.push_back(current.size());
    return data;
  }

  // --- Phase 2: confirm ------------------------------------------------------

  // Runs the paired best/profile rollout for one confirm seed.
  ConfirmPairOutput confirm_pair(std::size_t traverser, std::uint64_t seed,
                                 const FrozenBestResponseData& frozen, BrEstimatorCounts& counts) {
    const std::vector<HoleCards> holes = draw_joint_holes(seed, counts);
    ConfirmPairOutput out;
    out.deal_fingerprint = crn_deal_rng(seed).next_u64();
    PairWalk walk(*this, traverser, seed, holes, frozen, out, counts);
    const auto [v_best, v_profile] = walk.run(poker::GameState(def_), HandLog{});
    out.best_utility = v_best;
    out.profile_utility = v_profile;
    return out;
  }

  // A single (non-shared) leg after the best and profile actions diverge.
  std::array<double, 10> confirm_leg(std::size_t traverser, std::uint64_t seed,
                                     const std::vector<HoleCards>& holes, poker::GameState state,
                                     HandLog log, bool best_leg,
                                     const FrozenBestResponseData& frozen, ConfirmPairOutput& out,
                                     BrEstimatorCounts& counts) {
    LegWalk walk(*this, traverser, seed, holes, best_leg, frozen, out, counts);
    return walk.run(std::move(state), std::move(log));
  }

  const poker::GameDef& def() const { return def_; }
  std::size_t seats() const { return n_; }
  const BrEstimatorConfig& config() const { return cfg_; }
  const std::vector<const BehaviorPolicy*>& policies() const { return policies_; }

 private:
  // Draws the one joint hole deal for a seed from the enumerated table or the
  // scalable product-conditional dealer.
  std::vector<HoleCards> draw_joint_holes(std::uint64_t seed, BrEstimatorCounts& counts) {
    bs::SplitMix64 rng = crn_deal_rng(seed);
    std::vector<std::array<int, 2>> hands;
    if (enumerated_) {
      const std::size_t index =
          bs::solver::sample_joint_deal(*enumerated_, ranges_, rng, cfg_.dealer_max_attempts);
      hands = enumerated_->deals[index].hands;
    } else {
      std::size_t attempts = 0;
      bs::solver::MultiwayDeal deal = bs::solver::sample_scalable_joint_deal(
          ranges_, board_prefix_, rng, cfg_.dealer_max_attempts, &attempts);
      counts.deal_restarts += attempts > 0 ? static_cast<std::uint64_t>(attempts - 1) : 0;
      hands = std::move(deal.hands);
    }
    std::vector<HoleCards> holes;
    holes.reserve(hands.size());
    for (const std::array<int, 2>& hand : hands)
      holes.push_back(HoleCards{hand[0], hand[1]});
    if (holes.size() != n_)
      throw stage6_br_error("joint dealer returned the wrong number of hole hands");
    return holes;
  }

  // --- Walks ---------------------------------------------------------------

  // Shared helpers and state live in this nested base.
  struct WalkBase {
    DeviationEstimator& owner;
    std::size_t traverser = 0;
    std::uint64_t seed = 0;
    const std::vector<HoleCards>& holes;
    BrEstimatorCounts& counts;
    bool flop_checked = false;

    WalkBase(DeviationEstimator& o, std::size_t t, std::uint64_t s, const std::vector<HoleCards>& h,
             BrEstimatorCounts& c)
        : owner(o), traverser(t), seed(s), holes(h), counts(c) {}

    std::uint64_t node_token(const poker::GameState& state, const HandLog& log) const {
      return public_history_hash(state, log);
    }

    std::vector<int> available_runout_cards(const poker::GameState& state) const {
      std::array<bool, 52> used{};
      for (int card : state.board())
        used[card] = true;
      for (const HoleCards& h : holes)
        for (int card : h)
          used[card] = true;
      std::vector<int> cards;
      for (int card = 0; card < 52; ++card)
        if (!used[card])
          cards.push_back(card);
      if (cards.empty())
        throw stage6_br_error("deal node has no available runout card");
      return cards;
    }

    // One runout card via one rejection-free bounded CRN draw over the
    // conditional deck (board plus EVERY seat's holes removed, folders
    // included). The stream seat is fixed at 0 on purpose: a public card
    // belongs to no player, and keying without the traverser index is what
    // lets the per-traverser runs and best/profile legs share the draw — do
    // not "personalize" it with the traverser.
    poker::GameState draw_runout(poker::GameState state, std::uint64_t token) {
      const std::vector<int> cards = available_runout_cards(state);
      const std::size_t index = bounded_crn_index(seed, token, BrStreamPurpose::RunoutCard, 0,
                                                  cards.size(), &counts.card_draws);
      poker::GameState next = state.after_card(cards[index]);
      if (!flop_checked && next.board().size() == 3 && next.live_players().size() >= 2) {
        if (owner.cfg_.coverage)
          owner.cfg_.coverage->require_covered(flop_signature(next));
        flop_checked = true;
      }
      return next;
    }

    std::vector<PolicyAction> query_policy(const poker::GameState& state, const HandLog& log,
                                           std::size_t seat) const {
      PolicyContext ctx;
      ctx.hand_log = &log;
      const std::uint64_t token = public_history_hash(state, log);
      ctx.decision_seed = crn_u64(seed, token, BrStreamPurpose::PolicyDecision, seat, 0);
      return owner.policies_[seat]->distribution(state, seat, holes[seat], ctx);
    }

    std::array<double, 10> settle(const poker::GameState& state, bool folded) const {
      std::array<double, 10> utility{};
      long long sum = 0;
      if (folded) {
        const poker::ContributionSettlement settlement = state.settle_fold();
        for (std::size_t s = 0; s < owner.n_; ++s) {
          utility[s] = static_cast<double>(settlement.chip_utility[s]);
          sum += settlement.chip_utility[s];
        }
      } else {
        std::vector<std::array<int, 2>> live_holes;
        for (std::size_t seat : state.live_players())
          live_holes.push_back(holes[seat]);
        const poker::ContributionSettlement settlement = state.settle_showdown(live_holes);
        for (std::size_t s = 0; s < owner.n_; ++s) {
          utility[s] = static_cast<double>(settlement.chip_utility[s]);
          sum += settlement.chip_utility[s];
        }
      }
      if (sum != 0)
        throw stage6_br_error("br estimator terminal settlement is not zero-sum");
      return utility;
    }
  };

  // Phase-1 epoch walk: a full declared-menu bush at every traverser node on
  // the sampled opponent/chance spine. Each node returns the value vector of
  // the PREDECESSOR frozen action, so every accumulated row estimates
  // "this menu action here, b_{k-1} at every other traverser infoset".
  struct LearnWalk final : WalkBase {
    std::map<InfosetKey, std::vector<poker::Action>>& menus;
    std::map<InfosetKey, poker::Action>& previous;
    std::map<InfosetKey, std::vector<QCell>>& q;

    LearnWalk(DeviationEstimator& o, std::size_t t, std::uint64_t s,
              const std::vector<HoleCards>& h, std::map<InfosetKey, std::vector<poker::Action>>& m,
              std::map<InfosetKey, poker::Action>& prev,
              std::map<InfosetKey, std::vector<QCell>>& qq, BrEstimatorCounts& c)
        : WalkBase(o, t, s, h, c), menus(m), previous(prev), q(qq) {}

    std::array<double, 10> run(poker::GameState state, HandLog log) {
      switch (state.phase()) {
        case poker::Phase::Deal: {
          const std::uint64_t token = node_token(state, log);
          return run(draw_runout(std::move(state), token), std::move(log));
        }
        case poker::Phase::Folded:
          return settle(state, true);
        case poker::Phase::Showdown:
          return settle(state, false);
        case poker::Phase::Action:
          break;
      }
      ++counts.learn_action_nodes;
      const std::size_t seat = *state.actor();
      const poker::LegalActions legal = state.legal();

      if (seat != traverser) {
        const std::vector<PolicyAction> dist = query_policy(state, log, seat);
        require_finite_legal_distribution(dist, legal, "br estimator opponent policy");
        const std::uint64_t token = node_token(state, log);
        const double draw = crn_unit(seed, token, BrStreamPurpose::OpponentAction, seat, 0);
        const poker::Action chosen = sample_distribution(dist, draw);
        if (!legal.contains(chosen))
          throw stage6_br_error("br estimator opponent sampled an illegal action");
        HandLog next_log = log;
        street_log_ref(next_log, state.street()).push_back(LoggedAction{seat, chosen});
        return run(state.after_action(seat, chosen), std::move(next_log));
      }

      const InfosetKey key = make_infoset_key(state, log, traverser, holes[traverser]);
      std::vector<poker::Action> menu = deviation_menu(owner.cfg_, state, seat);
      if (menu.empty())
        throw stage6_br_error("br estimator deviation menu is empty");
      for (const poker::Action& action : menu)
        if (!legal.contains(action))
          throw stage6_br_error("br estimator declared deviation action is not legal");

      auto menu_it = menus.find(key);
      if (menu_it == menus.end()) {
        menus.emplace(key, menu);
      } else if (menu_it->second != menu) {
        throw stage6_br_error("br estimator menu changed within one information set");
      }
      auto cells_it = q.find(key);
      if (cells_it == q.end())
        cells_it = q.emplace(key, std::vector<QCell>(menu.size())).first;
      if (cells_it->second.size() != menu.size())
        throw stage6_br_error("br estimator Q row length does not match its menu");

      // Bootstrap b_0 lazily on the first epoch's first visit.
      if (!previous.contains(key))
        previous.emplace(key, menu[bootstrap_index(state, log, seat, menu)]);

      std::size_t predecessor = 0;
      {
        const poker::Action chosen = previous.at(key);
        bool found = false;
        for (std::size_t j = 0; j < menu.size(); ++j)
          if (menu[j] == chosen) {
            predecessor = j;
            found = true;
            break;
          }
        if (!found)
          throw stage6_br_error("br estimator frozen predecessor action left its menu");
      }

      std::array<double, 10> predecessor_value{};
      for (std::size_t j = 0; j < menu.size(); ++j) {
        HandLog next_log = log;
        street_log_ref(next_log, state.street()).push_back(LoggedAction{seat, menu[j]});
        const std::array<double, 10> child =
            run(state.after_action(seat, menu[j]), std::move(next_log));
        QCell& cell = cells_it->second[j];
        cell.sum += child[traverser];
        ++cell.visits;
        if (j == predecessor)
          predecessor_value = child;
      }
      return predecessor_value;
    }

    // Highest-probability action of the traverser's own profile at this
    // information set, restricted to the deviation menu; an off-menu profile
    // action bootstraps to the first menu entry. Ties follow profile order.
    std::size_t bootstrap_index(const poker::GameState& state, const HandLog& log, std::size_t seat,
                                const std::vector<poker::Action>& menu) {
      const std::vector<PolicyAction> dist = query_policy(state, log, seat);
      const poker::LegalActions legal = state.legal();
      require_finite_legal_distribution(dist, legal, "br estimator traverser profile");
      std::size_t best_profile = 0;
      for (std::size_t k = 1; k < dist.size(); ++k)
        if (dist[k].probability > dist[best_profile].probability)
          best_profile = k;
      const poker::Action wanted = dist[best_profile].action;
      for (std::size_t j = 0; j < menu.size(); ++j)
        if (menu[j] == wanted)
          return j;
      return 0;
    }
  };

  // A single (non-shared) leg after divergence. CRN content addressing keeps
  // its later draws paired with the other leg at every reconvergent node.
  struct LegWalk final : WalkBase {
    bool best_leg = false;
    const FrozenBestResponseData& frozen;
    ConfirmPairOutput& out;

    LegWalk(DeviationEstimator& o, std::size_t t, std::uint64_t s, const std::vector<HoleCards>& h,
            bool best, const FrozenBestResponseData& f, ConfirmPairOutput& out_param,
            BrEstimatorCounts& c)
        : WalkBase(o, t, s, h, c), best_leg(best), frozen(f), out(out_param) {}

    poker::Action frozen_action(const poker::GameState& state, const HandLog& log,
                                const poker::Action& profile_action) {
      const InfosetKey key = make_infoset_key(state, log, traverser, holes[traverser]);
      auto it = frozen.actions.find(key);
      if (it == frozen.actions.end()) {
        if (owner.cfg_.on_confirm_miss == FrozenMissPolicy::Throw)
          throw stage6_br_infoset_miss(
              "br estimator confirm rollout reached an unfrozen information set");
        ++out.misses;
        return profile_action;
      }
      const poker::Action action = it->second;
      auto menu_it = frozen.menus.find(key);
      if (menu_it == frozen.menus.end() || !state.legal().contains(action))
        throw stage6_br_error("br estimator frozen action is not legal at confirm time");
      bool in_menu = false;
      for (const poker::Action& m : menu_it->second)
        if (m == action)
          in_menu = true;
      if (!in_menu)
        throw stage6_br_error("br estimator frozen action left its stored menu");
      return action;
    }

    std::array<double, 10> run(poker::GameState state, HandLog log) {
      switch (state.phase()) {
        case poker::Phase::Deal: {
          const std::uint64_t token = node_token(state, log);
          const std::vector<int> cards = available_runout_cards(state);
          CrnDrawEvent accepted;
          const std::size_t index = bounded_crn_index(seed, token, BrStreamPurpose::RunoutCard, 0,
                                                      cards.size(), &counts.card_draws, &accepted);
          (best_leg ? out.best_draw_log : out.profile_draw_log).push_back(accepted);
          poker::GameState next = state.after_card(cards[index]);
          if (!flop_checked && next.board().size() == 3 && next.live_players().size() >= 2) {
            if (owner.cfg_.coverage)
              owner.cfg_.coverage->require_covered(flop_signature(next));
            flop_checked = true;
          }
          return run(std::move(next), std::move(log));
        }
        case poker::Phase::Folded:
          return settle(state, true);
        case poker::Phase::Showdown:
          return settle(state, false);
        case poker::Phase::Action:
          break;
      }
      ++counts.confirm_action_nodes;
      const std::size_t seat = *state.actor();
      const poker::LegalActions legal = state.legal();

      if (seat != traverser) {
        const std::vector<PolicyAction> dist = query_policy(state, log, seat);
        require_finite_legal_distribution(dist, legal, "br estimator confirm opponent policy");
        const std::uint64_t token = node_token(state, log);
        const std::uint64_t raw = crn_u64(seed, token, BrStreamPurpose::OpponentAction, seat, 0);
        (best_leg ? out.best_draw_log : out.profile_draw_log)
            .push_back({token, BrStreamPurpose::OpponentAction, seat, 0, raw});
        const poker::Action chosen = sample_distribution(dist, unit_from_u64(raw));
        if (!legal.contains(chosen))
          throw stage6_br_error("br estimator confirm opponent sampled an illegal action");
        HandLog next_log = log;
        street_log_ref(next_log, state.street()).push_back(LoggedAction{seat, chosen});
        return run(state.after_action(seat, chosen), std::move(next_log));
      }

      const std::uint64_t token = node_token(state, log);
      const std::vector<PolicyAction> profile_dist = query_policy(state, log, seat);
      require_finite_legal_distribution(profile_dist, legal,
                                        "br estimator confirm traverser profile");
      const poker::Action profile_action = sample_distribution(
          profile_dist,
          crn_unit(seed, token, BrStreamPurpose::TraverserProfileAction, traverser, 0));
      const poker::Action action =
          best_leg ? frozen_action(state, log, profile_action) : profile_action;
      if (!legal.contains(action))
        throw stage6_br_error("br estimator confirm leg selected an illegal action");
      HandLog next_log = log;
      street_log_ref(next_log, state.street()).push_back(LoggedAction{seat, action});
      return run(state.after_action(seat, action), std::move(next_log));
    }
  };

  // Paired recursion: one shared walk until the legs choose different
  // traverser actions, then two CRN-paired single legs.
  struct PairWalk final : WalkBase {
    const FrozenBestResponseData& frozen;
    ConfirmPairOutput& out;

    PairWalk(DeviationEstimator& o, std::size_t t, std::uint64_t s, const std::vector<HoleCards>& h,
             const FrozenBestResponseData& f, ConfirmPairOutput& out_param, BrEstimatorCounts& c)
        : WalkBase(o, t, s, h, c), frozen(f), out(out_param) {}

    std::pair<std::array<double, 10>, std::array<double, 10>> run(poker::GameState state,
                                                                  HandLog log) {
      switch (state.phase()) {
        case poker::Phase::Deal: {
          const std::uint64_t token = node_token(state, log);
          const std::vector<int> cards = available_runout_cards(state);
          CrnDrawEvent accepted;
          const std::size_t index = bounded_crn_index(seed, token, BrStreamPurpose::RunoutCard, 0,
                                                      cards.size(), &counts.card_draws, &accepted);
          out.best_draw_log.push_back(accepted);
          out.profile_draw_log.push_back(accepted);
          poker::GameState next = state.after_card(cards[index]);
          if (!flop_checked && next.board().size() == 3 && next.live_players().size() >= 2) {
            if (owner.cfg_.coverage)
              owner.cfg_.coverage->require_covered(flop_signature(next));
            flop_checked = true;
          }
          return run(std::move(next), std::move(log));
        }
        case poker::Phase::Folded: {
          const std::array<double, 10> u = settle(state, true);
          return {u, u};
        }
        case poker::Phase::Showdown: {
          const std::array<double, 10> u = settle(state, false);
          return {u, u};
        }
        case poker::Phase::Action:
          break;
      }
      ++counts.confirm_action_nodes;
      const std::size_t seat = *state.actor();
      const poker::LegalActions legal = state.legal();

      if (seat != traverser) {
        const std::vector<PolicyAction> dist = query_policy(state, log, seat);
        require_finite_legal_distribution(dist, legal, "br estimator confirm opponent policy");
        const std::uint64_t token = node_token(state, log);
        const std::uint64_t raw = crn_u64(seed, token, BrStreamPurpose::OpponentAction, seat, 0);
        const CrnDrawEvent event{token, BrStreamPurpose::OpponentAction, seat, 0, raw};
        out.best_draw_log.push_back(event);
        out.profile_draw_log.push_back(event);
        const poker::Action chosen = sample_distribution(dist, unit_from_u64(raw));
        if (!legal.contains(chosen))
          throw stage6_br_error("br estimator confirm opponent sampled an illegal action");
        HandLog next_log = log;
        street_log_ref(next_log, state.street()).push_back(LoggedAction{seat, chosen});
        return run(state.after_action(seat, chosen), std::move(next_log));
      }

      const std::uint64_t token = node_token(state, log);
      const std::vector<PolicyAction> profile_dist = query_policy(state, log, seat);
      require_finite_legal_distribution(profile_dist, legal,
                                        "br estimator confirm traverser profile");
      const double profile_draw =
          crn_unit(seed, token, BrStreamPurpose::TraverserProfileAction, traverser, 0);
      const poker::Action profile_action = sample_distribution(profile_dist, profile_draw);

      const InfosetKey key = make_infoset_key(state, log, traverser, holes[traverser]);
      auto it = frozen.actions.find(key);
      poker::Action best_action;
      if (it == frozen.actions.end()) {
        if (owner.cfg_.on_confirm_miss == FrozenMissPolicy::Throw)
          throw stage6_br_infoset_miss(
              "br estimator confirm rollout reached an unfrozen information set");
        ++out.misses;
        best_action = profile_action;
      } else {
        best_action = it->second;
        auto menu_it = frozen.menus.find(key);
        if (menu_it == frozen.menus.end() || !legal.contains(best_action))
          throw stage6_br_error("br estimator frozen action is not legal at confirm time");
        bool in_menu = false;
        for (const poker::Action& m : menu_it->second)
          if (m == best_action)
            in_menu = true;
        if (!in_menu)
          throw stage6_br_error("br estimator frozen action left its stored menu");
      }

      if (best_action == profile_action) {
        ++counts.paired_same_action_merges;
        HandLog next_log = log;
        street_log_ref(next_log, state.street()).push_back(LoggedAction{seat, best_action});
        const auto [v_best, v_profile] =
            run(state.after_action(seat, best_action), std::move(next_log));
        return {v_best, v_profile};
      }

      HandLog best_log = log;
      street_log_ref(best_log, state.street()).push_back(LoggedAction{seat, best_action});
      HandLog profile_log = log;
      street_log_ref(profile_log, state.street()).push_back(LoggedAction{seat, profile_action});
      out.diverged = true;
      const std::array<double, 10> v_best =
          owner.confirm_leg(traverser, seed, holes, state.after_action(seat, best_action),
                            std::move(best_log), true, frozen, out, counts);
      const std::array<double, 10> v_profile =
          owner.confirm_leg(traverser, seed, holes, state.after_action(seat, profile_action),
                            std::move(profile_log), false, frozen, out, counts);
      return {v_best, v_profile};
    }
  };

  const poker::GameDef& def_;
  const std::vector<std::vector<bs::solver::MultiwayWeightedHand>>& ranges_;
  const std::vector<const BehaviorPolicy*>& policies_;
  const bs::solver::JointDealTable* enumerated_ = nullptr;
  const BrEstimatorConfig& cfg_;
  std::size_t n_ = 0;
  std::vector<int> board_prefix_;
};

}  // namespace

std::uint64_t FrozenBestResponseData::content_hash() const noexcept {
  std::uint64_t hash = 0xcbf29ce484222325ULL;
  auto eat = [&hash](std::uint64_t value) {
    for (unsigned shift = 0; shift < 64; shift += 8) {
      hash ^= static_cast<unsigned char>((value >> shift) & 0xffULL);
      hash *= 0x100000001b3ULL;
    }
  };
  eat(0x66726f7a656e31ULL);  // "frozen1"
  eat(traverser);
  eat(menu_identity_hash);
  eat(learn_seed_list_hash);
  eat(learn_seed_count);
  eat(epochs_run);
  eat(stabilized ? 1 : 0);
  eat(actions.size());
  for (const auto& [key, action] : actions) {
    eat(key.content_hash());
    eat(static_cast<std::uint64_t>(static_cast<int>(action.type)));
    eat(action.target_total);
  }
  return hash;
}

std::vector<PolicyAction> FrozenBestResponsePolicy::distribution(
    const poker::GameState& state, std::size_t seat, HoleCards hole,
    const PolicyContext& context) const {
  if (!context.hand_log)
    throw stage6_br_error("frozen best response requires the public hand log");
  if (seat != data_.traverser)
    throw stage6_br_error("frozen best response queried at a non-traverser seat");
  const InfosetKey key = make_infoset_key(state, *context.hand_log, seat, hole);
  const auto it = data_.actions.find(key);
  if (it == data_.actions.end())
    throw stage6_br_infoset_miss("frozen best response has no action for this information set");
  const poker::Action action = it->second;
  if (!state.legal().contains(action))
    throw stage6_br_error("frozen best response action is not legal at this state");
  const auto menu_it = data_.menus.find(key);
  if (menu_it == data_.menus.end())
    throw stage6_br_error("frozen best response is missing its stored menu");
  bool in_menu = false;
  for (const poker::Action& menu_action : menu_it->second)
    if (menu_action == action)
      in_menu = true;
  if (!in_menu)
    throw stage6_br_error("frozen best response action is outside its stored menu");
  return {{action, 1.0}};
}

FrozenBestResponseData mc_learn_frozen_best_response(
    const poker::GameDef& def,
    const std::vector<std::vector<bs::solver::MultiwayWeightedHand>>& ranges,
    const std::vector<const BehaviorPolicy*>& policies, std::size_t traverser,
    std::span<const std::uint64_t> learn_seeds, const bs::solver::JointDealTable* enumerated,
    const BrEstimatorConfig& config, BrEstimatorCounts* counts_out) {
  if (learn_seeds.empty())
    throw std::invalid_argument("br estimator learn requires a nonempty seed list");
  if (traverser >= policies.size())
    throw std::invalid_argument("br estimator learn traverser is not a seated policy");
  BrEstimatorCounts local_counts;
  BrEstimatorCounts& counts = counts_out ? *counts_out : local_counts;
  DeviationEstimator estimator(def, ranges, policies, enumerated, config);
  return estimator.learn(traverser, learn_seeds, seed_list_hash(learn_seeds), counts);
}

ConfirmPairOutput mc_confirm_pair(
    const poker::GameDef& def,
    const std::vector<std::vector<bs::solver::MultiwayWeightedHand>>& ranges,
    const std::vector<const BehaviorPolicy*>& policies, std::size_t traverser, std::uint64_t seed,
    const FrozenBestResponseData& frozen, const bs::solver::JointDealTable* enumerated,
    const BrEstimatorConfig& config, BrEstimatorCounts* counts_out) {
  if (traverser >= policies.size())
    throw std::invalid_argument("br estimator confirm traverser is not a seated policy");
  BrEstimatorCounts local_counts;
  BrEstimatorCounts& counts = counts_out ? *counts_out : local_counts;
  DeviationEstimator estimator(def, ranges, policies, enumerated, config);
  ConfirmPairOutput out = estimator.confirm_pair(traverser, seed, frozen, counts);
  ++counts.confirm_rollouts;
  return out;
}

BrEstimatorResult estimate_deviation_gains(
    const poker::GameDef& def,
    const std::vector<std::vector<bs::solver::MultiwayWeightedHand>>& ranges,
    const std::vector<const BehaviorPolicy*>& policies, std::span<const std::uint64_t> learn_seeds,
    std::span<const std::uint64_t> confirm_seeds, const bs::solver::JointDealTable* enumerated,
    const BrEstimatorConfig& config) {
  if (learn_seeds.empty() || confirm_seeds.empty())
    throw std::invalid_argument("br estimator requires nonempty learn and confirm seed lists");
  if (config.deviation_space == DeviationSpace::CoarseAbstraction &&
      config.coarse_action == nullptr)
    throw std::invalid_argument(
        "br estimator coarse deviation space requires a non-null coarse_action");
  // Every seed in each list must be distinct: the confirm replicates are
  // treated as an iid series by the Student-t machinery and the learn rows
  // accumulate per-seed samples. A duplicate would silently overweight a draw.
  {
    std::unordered_set<std::uint64_t> learn_set(learn_seeds.begin(), learn_seeds.end());
    if (learn_set.size() != learn_seeds.size())
      throw std::invalid_argument("br estimator learn seed list contains a duplicate seed");
    std::unordered_set<std::uint64_t> confirm_set(confirm_seeds.begin(), confirm_seeds.end());
    if (confirm_set.size() != confirm_seeds.size())
      throw std::invalid_argument("br estimator confirm seed list contains a duplicate seed");
    for (std::uint64_t seed : confirm_seeds)
      if (learn_set.contains(seed))
        throw std::invalid_argument("br estimator learn and confirm seed lists must be disjoint");
  }

  const std::size_t n = policies.size();
  BrEstimatorResult result;
  result.learn_seed_list_hash = seed_list_hash(learn_seeds);
  result.confirm_seed_list_hash = seed_list_hash(confirm_seeds);
  result.frozen.resize(n);
  result.gain.assign(n, std::vector<double>(confirm_seeds.size(), 0.0));
  result.counts.epochs_per_traverser.reserve(n);
  result.counts.infosets_per_traverser.reserve(n);
  result.counts.confirm_misses_per_traverser.reserve(n);
  result.nashconv_replicates.assign(confirm_seeds.size(), 0.0);

  for (std::size_t traverser = 0; traverser < n; ++traverser) {
    BrEstimatorCounts traverser_counts;
    result.frozen[traverser] = mc_learn_frozen_best_response(
        def, ranges, policies, traverser, learn_seeds, enumerated, config, &traverser_counts);
    std::size_t seed_index = 0;
    std::size_t traverser_misses = 0;
    for (std::uint64_t seed : confirm_seeds) {
      ConfirmPairOutput pair =
          mc_confirm_pair(def, ranges, policies, traverser, seed, result.frozen[traverser],
                          enumerated, config, &traverser_counts);
      traverser_misses += pair.misses;
      for (std::size_t s = 0; s < n; ++s) {
        if (!std::isfinite(pair.best_utility[s]) || !std::isfinite(pair.profile_utility[s]))
          throw stage6_br_error("br estimator produced a non-finite terminal utility");
      }
      if (std::abs(std::accumulate(pair.best_utility.begin(),
                                   pair.best_utility.begin() + static_cast<std::ptrdiff_t>(n),
                                   0.0)) > 1e-7 ||
          std::abs(std::accumulate(pair.profile_utility.begin(),
                                   pair.profile_utility.begin() + static_cast<std::ptrdiff_t>(n),
                                   0.0)) > 1e-7)
        throw stage6_br_error("br estimator paired terminal vectors are not zero-sum");
      result.gain[traverser][seed_index] =
          pair.best_utility[traverser] - pair.profile_utility[traverser];
      ++seed_index;
    }
    result.counts.learn_rollouts += traverser_counts.learn_rollouts;
    result.counts.confirm_rollouts += traverser_counts.confirm_rollouts;
    result.counts.learn_action_nodes += traverser_counts.learn_action_nodes;
    result.counts.confirm_action_nodes += traverser_counts.confirm_action_nodes;
    result.counts.card_draws += traverser_counts.card_draws;
    result.counts.deal_restarts += traverser_counts.deal_restarts;
    result.counts.paired_same_action_merges += traverser_counts.paired_same_action_merges;
    result.counts.epochs_per_traverser.insert(result.counts.epochs_per_traverser.end(),
                                              traverser_counts.epochs_per_traverser.begin(),
                                              traverser_counts.epochs_per_traverser.end());
    result.counts.infosets_per_traverser.insert(result.counts.infosets_per_traverser.end(),
                                                traverser_counts.infosets_per_traverser.begin(),
                                                traverser_counts.infosets_per_traverser.end());
    result.counts.confirm_misses_per_traverser.push_back(traverser_misses);
  }

  for (std::size_t t = 0; t < n; ++t) {
    double sum = 0.0;
    for (double g : result.gain[t])
      sum += g;
    result.mean_gain[t] = sum / static_cast<double>(confirm_seeds.size());
  }
  for (std::size_t s = 0; s < confirm_seeds.size(); ++s) {
    double y = 0.0;
    for (std::size_t t = 0; t < n; ++t)
      y += result.gain[t][s];
    result.nashconv_replicates[s] = y;
  }
  result.mean_nashconv =
      std::accumulate(result.nashconv_replicates.begin(), result.nashconv_replicates.end(), 0.0) /
      static_cast<double>(confirm_seeds.size());
  return result;
}

FrozenBestResponseData learn_exact_frozen_best_response(
    const poker::GameDef& def,
    const std::vector<std::vector<bs::solver::MultiwayWeightedHand>>& ranges,
    const std::vector<const BehaviorPolicy*>& policies, std::size_t traverser) {
  if (traverser >= policies.size())
    throw std::invalid_argument("exact freeze traverser is not a seated policy");
  ExactBrChoiceSink sink;
  OracleCounts counts;
  (void)exact_best_response_utility(def, ranges, policies, traverser, &counts, &sink);

  FrozenBestResponseData data;
  data.traverser = traverser;
  data.actions = std::move(sink.actions);
  data.menus = std::move(sink.menus);
  data.menu_identity_hash = declared_menu_identity_hash();
  data.learn_seed_list_hash = 0;
  data.learn_seed_count = 0;
  data.epochs_run = 1;
  data.stabilized = true;
  return data;
}

}  // namespace bs::stage6
