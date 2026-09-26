#include <bs/eval.hpp>
#include <bs/stage6/simulator.hpp>
#include <cmath>
#include <string>
#include <utility>
#include <vector>

namespace bs::stage6 {
namespace {

namespace poker = bs::poker;

// Deterministic per-decision seed, independent of the sampling RNG so a
// policy's internal choice does not depend on how many random draws preceded
// it beyond the declared counter. Never zero.
std::uint64_t decision_seed(std::uint64_t hand, std::uint64_t decision) {
  std::uint64_t z = 0x5336312d6465636eULL ^ hand ^ (decision + 1) * 0x9e3779b97f4a7c15ULL;
  z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
  z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
  z ^= z >> 31;
  return z ? z : 1ULL;
}

// A unit [0,1) draw from a SplitMix64 value: the top 53 bits divide by 2^53.
double unit_draw(bs::SplitMix64& rng) {
  return static_cast<double>(rng.next_u64() >> 11) * 0x1.0p-53;
}

std::vector<LoggedAction>& street_log(HandLog& log, poker::Street street) {
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
  return log.river;  // unreachable; -Werror=switch exhausts the enum above
}

}  // namespace

HandSimulator::HandSimulator(std::vector<const BehaviorPolicy*> policies,
                             const GeometryCoverage* coverage, bs::SplitMix64& rng)
    : policies_(std::move(policies)), coverage_(coverage), rng_(rng) {
  if (policies_.size() < 2 || policies_.size() > 10)
    throw stage6_sim_error("simulator requires 2..10 seat policies");
  for (const BehaviorPolicy* p : policies_)
    if (!p)
      throw stage6_sim_error("simulator policy is null");
}

SimResult HandSimulator::run(const poker::GameDef& def, const std::vector<HoleCards>& holes,
                             const std::array<int, 5>& board, std::uint64_t hand_id) {
  const std::size_t n = policies_.size();
  if (def.player_count != n)
    throw stage6_sim_error("hand def seat count does not match policy count");
  if (holes.size() != n)
    throw stage6_sim_error("one hole pair required per seat");

  // Predetermined deck integrity: all holes and board cards distinct and in
  // range. The whole chance outcome is removed from the deck before any action.
  {
    std::array<bool, 52> used{};
    auto take = [&](int card, const char* what) {
      if (card < 0 || card >= 52 || used[card])
        throw stage6_sim_error(std::string("duplicate or out-of-range ") + what);
      used[card] = true;
    };
    for (std::size_t s = 0; s < n; ++s) {
      take(holes[s][0], "hole card");
      take(holes[s][1], "hole card");
    }
    for (int card : board)
      take(card, "board card");
  }

  poker::GameState state(def);
  HandLog log;
  std::uint64_t decisions = 0;
  bool flop_checked = false;

  // The board is dealt only on Phase::Deal transitions, advancing the
  // predetermined five-card runout. There is no chance sampling inside the
  // loop body.
  while (true) {
    switch (state.phase()) {
      case poker::Phase::Action: {
        const std::size_t seat = *state.actor();
        const poker::LegalActions legal = state.legal();
        PolicyContext ctx;
        ctx.hand_log = &log;
        ctx.decision_seed = decision_seed(hand_id, decisions++);
        std::vector<PolicyAction> dist =
            policies_[seat]->distribution(state, seat, holes[seat], ctx);
        if (dist.empty())
          throw stage6_sim_error("policy returned an empty distribution");
        double total = 0.0;
        for (const PolicyAction& pa : dist) {
          if (!std::isfinite(pa.probability) || pa.probability < 0.0)
            throw stage6_sim_error("policy distribution has a non-finite/negative probability");
          total += pa.probability;
        }
        if (std::abs(total - 1.0) > 1e-9)
          throw stage6_sim_error("policy distribution does not sum to 1");
        const poker::Action chosen = sample_distribution(dist, unit_draw(rng_));
        if (!legal.contains(chosen))
          throw stage6_sim_error("policy selected an action that is not legal");
        street_log(log, state.street()).push_back(LoggedAction{seat, chosen});
        state = state.after_action(seat, chosen);
        break;
      }
      case poker::Phase::Deal: {
        const std::size_t want = state.board().size();  // next board index to deal
        if (want >= 5)
          throw stage6_sim_error("deal phase with a full board");
        state = state.after_card(board[want]);
        // The instant the three-card flop exists, the candidate geometry
        // lookup must be total.
        if (!flop_checked && state.board().size() == 3 && state.live_players().size() >= 2) {
          if (coverage_)
            coverage_->require_covered(flop_signature(state));
          flop_checked = true;
        }
        break;
      }
      case poker::Phase::Folded: {
        const poker::ContributionSettlement settlement = state.settle_fold();
        SimResult result;
        result.folded = true;
        long long sum = 0;
        for (std::size_t s = 0; s < n; ++s) {
          result.utility[s] = static_cast<double>(settlement.chip_utility[s]);
          sum += settlement.chip_utility[s];
        }
        if (sum != 0)
          throw stage6_sim_error("fold settlement is not zero-sum");
        return result;
      }
      case poker::Phase::Showdown: {
        std::vector<std::array<int, 2>> live_holes;
        live_holes.reserve(state.live_players().size());
        for (std::size_t seat : state.live_players())
          live_holes.push_back(holes[seat]);
        const poker::ContributionSettlement settlement = state.settle_showdown(live_holes);
        SimResult result;
        result.folded = false;
        long long sum = 0;
        for (std::size_t s = 0; s < n; ++s) {
          result.utility[s] = static_cast<double>(settlement.chip_utility[s]);
          sum += settlement.chip_utility[s];
        }
        if (sum != 0)
          throw stage6_sim_error("showdown settlement is not zero-sum");
        return result;
      }
    }
  }
}

}  // namespace bs::stage6
