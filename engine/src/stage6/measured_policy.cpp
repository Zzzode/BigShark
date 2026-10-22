#include <algorithm>
#include <bs/abstract_tree.hpp>
#include <bs/stage6/measured_policy.hpp>
#include <cmath>

namespace bs::stage6 {
namespace {

// The interface's legality contract is the caller's, but a policy that returns
// an action the state forbids is a harness defect the walk would otherwise
// treat as a poker outcome. Checked here so both adapters fail the same way.
void require_legal(const std::vector<PolicyAction>& dist, const poker::LegalActions& legal,
                   const char* source) {
  if (dist.empty())
    throw Stage6PolicyError(std::string(source) + " returned an empty distribution");
  double total = 0.0;
  for (const PolicyAction& pa : dist) {
    if (!std::isfinite(pa.probability) || pa.probability < 0.0)
      throw Stage6PolicyError(std::string(source) + " returned a non-finite/negative probability");
    if (!legal.contains(pa.action))
      throw Stage6PolicyError(std::string(source) + " returned an action that is not legal");
    total += pa.probability;
  }
  if (std::abs(total - 1.0) > 1e-9)
    throw Stage6PolicyError(std::string(source) + " distribution does not sum to 1");
}

}  // namespace

std::vector<poker::PublicAction> public_action_history(const HandLog& log) {
  // Streets ascending, each in recorded order: this is the order
  // `replay_public_path` consumes, and it is the order the actions actually
  // happened in for any legal hand.
  std::vector<poker::PublicAction> out;
  out.reserve(log.preflop.size() + log.flop.size() + log.turn.size() + log.river.size());
  const auto append = [&out](const std::vector<LoggedAction>& actions, poker::Street street) {
    for (const LoggedAction& a : actions)
      out.push_back(poker::PublicAction{street, a.seat, a.action});
  };
  append(log.preflop, poker::Street::Preflop);
  append(log.flop, poker::Street::Flop);
  append(log.turn, poker::Street::Turn);
  append(log.river, poker::Street::River);
  return out;
}

NSeatBehaviorPolicy::NSeatBehaviorPolicy(const solver::NSeatPolicy& policy,
                                         const tree::AbstractTree& tree)
    : policy_(policy), tree_(tree) {
  // A def or action-identity mismatch makes every lookup an exact miss, which
  // would surface as a throw on the first decision rather than as a clear
  // construction error. Report it where it is introduced.
  if (!poker::same_game_def(policy_.game(), tree_.def()))
    throw std::invalid_argument("NSeatBehaviorPolicy: policy and tree define different games");
  if (policy_.action_id() != tree_.action_id())
    throw std::invalid_argument("NSeatBehaviorPolicy: policy and tree use different action ids");
}

std::vector<PolicyAction> NSeatBehaviorPolicy::distribution(
    const poker::GameState& state, std::size_t seat, HoleCards hole,
    const PolicyContext& /*context*/) const {
  // `lookup` requires ascending own cards; the walk interface does not promise
  // an order and the flop library's combos are already sorted, so normalize
  // rather than assume.
  std::array<int, 2> own = hole;
  std::sort(own.begin(), own.end());

  const solver::NSeatPolicyRow* row = policy_.lookup(tree_, state, seat, own);
  if (!row) {
    ++misses_;
    throw Stage6PolicyError("NSeatBehaviorPolicy: no row covers this information set");
  }
  ++served_;

  std::vector<PolicyAction> dist;
  dist.reserve(row->actions.size());
  for (std::size_t i = 0; i < row->actions.size(); ++i)
    dist.push_back(PolicyAction{row->actions[i], row->probabilities[i]});
  require_legal(dist, state.legal(), "NSeatBehaviorPolicy");
  return dist;
}

ResidentBehaviorPolicy::ResidentBehaviorPolicy(const resident::ResidentPolicySet& resident,
                                               std::optional<std::string_view> pinned_sha256)
    : resident_(resident), pinned_sha256_(pinned_sha256) {}

std::vector<PolicyAction> ResidentBehaviorPolicy::distribution(const poker::GameState& state,
                                                               std::size_t seat, HoleCards hole,
                                                               const PolicyContext& context) const {
  // The resident serving path answers for the ACTING seat and reads the hero's
  // own cards from the query; a request for any other seat is not something it
  // can serve, and the walk would be asking the wrong question.
  if (!state.actor() || *state.actor() != seat) {
    ++misses_;
    throw Stage6PolicyError("ResidentBehaviorPolicy: requested seat is not the acting seat");
  }

  const std::vector<poker::PublicAction> history = context.hand_log
                                                       ? public_action_history(*context.hand_log)
                                                       : std::vector<poker::PublicAction>{};

  const resident::ResidentAnswer answer =
      resident_.hero_decision(state, history, hole, pinned_sha256_, scratch_);
  if (!answer.hit) {
    ++misses_;
    last_miss_ = answer.reason;
    throw Stage6PolicyError(std::string("ResidentBehaviorPolicy: resident miss (") +
                            resident::to_string(answer.reason) + ")");
  }
  ++served_;

  std::vector<PolicyAction> dist;
  dist.reserve(answer.hero_row.size);
  for (std::size_t i = 0; i < answer.hero_row.size; ++i)
    dist.push_back(PolicyAction{answer.hero_row.actions[i], answer.hero_row.probabilities[i]});
  require_legal(dist, state.legal(), "ResidentBehaviorPolicy");
  return dist;
}

}  // namespace bs::stage6
