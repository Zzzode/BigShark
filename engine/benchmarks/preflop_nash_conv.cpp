// RFC 0009 W4b (extended by RFC 0010): offline NashConv measurement for a
// published preflop blueprint. Loads the schema-v2 preflop artifact, rebuilds
// the game tree, and computes a Monte Carlo NashConv by walking the tree with
// best-response and policy-following traversals over sampled joint deals.
// Heads-up and multiway (2..10 seats) artifacts are both supported: the joint
// deal draws 2N cards and the NashConv is the sum of per-seat BR gaps divided
// by N for exploitability.
//
// Walker design (standard MC NashConv):
// - Strategy walk: sample one action at each node (following the policy),
//   follow one path. At FlopDeal leaves, sample one flop.
// - BR walk: at the traverser's nodes, evaluate all actions and take the
//   max. At opponent nodes, sample one action from the policy. At FlopDeal
//   leaves, sample one flop.
// - Uncovered nodes (MCCFR policies are only defined at visited nodes) use
//   a check>call>fold fallback.
//
// Usage: bigshark-preflop-nash-conv [artifact-path] [deals] [flops] [seed]
//
// This is a manual offline measurement tool, NOT a CTest.
#include <algorithm>
#include <array>
#include <bs/abstract_tree.hpp>
#include <bs/equity_frontier.hpp>
#include <bs/game_definition.hpp>
#include <bs/heads_up_solver.hpp>
#include <bs/prng.hpp>
#include <bs/seat_policy.hpp>
#include <bs/strategy_artifact.hpp>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using bs::gto::EquityFrontierEvaluator;
using bs::poker::Action;
using bs::poker::ActionType;
using bs::poker::GameDef;
using bs::poker::GameState;
using bs::poker::Phase;
using bs::poker::PublicAction;
using bs::tree::AbstractTree;
using bs::tree::NodeKind;
using bs::tree::TerminalPayload;

// Legal runout cards: 0..51 minus board and every seat's hole cards.
std::vector<int> legal_runout_cards(const GameState& state,
                                    const std::vector<std::array<int, 2>>& holes) {
  std::array<bool, 52> used{};
  for (int card : state.board())
    used[card] = true;
  for (const auto& hole : holes)
    used[hole[0]] = used[hole[1]] = true;
  std::vector<int> cards;
  for (int c = 0; c < 52; ++c)
    if (!used[c])
      cards.push_back(c);
  return cards;
}

std::vector<int> board_vector(const GameState& state) {
  return std::vector<int>(state.board().begin(), state.board().end());
}

// Sample one uniform joint deal: 2N cards from a shuffled deck, 2 per seat.
std::vector<std::array<int, 2>> sample_joint_deal(bs::SplitMix64& rng, std::size_t n) {
  std::array<int, 52> deck;
  for (int i = 0; i < 52; ++i)
    deck[i] = i;
  for (int i = 51; i > 0; --i) {
    const std::size_t j = rng.next_u64() % static_cast<std::uint64_t>(i + 1);
    std::swap(deck[i], deck[j]);
  }
  std::vector<std::array<int, 2>> holes(n);
  for (std::size_t s = 0; s < n; ++s) {
    const int c0 = deck[2 * s];
    const int c1 = deck[2 * s + 1];
    holes[s] = {std::min(c0, c1), std::max(c0, c1)};
  }
  return holes;
}

// Sample an action index from a probability distribution.
std::size_t sample_action(const std::vector<double>& probs, bs::SplitMix64& rng) {
  const double r = static_cast<double>(rng.next_u64() % 1000000) / 1000000.0;
  double cumulative = 0.0;
  for (std::size_t i = 0; i < probs.size(); ++i) {
    cumulative += probs[i];
    if (r < cumulative)
      return i;
  }
  return probs.size() - 1;  // fallback: last action
}

struct NashConvWalker {
  const AbstractTree& tree;
  const bs::solver::SeatPolicy& policy;
  const EquityFrontierEvaluator& frontier;
  std::size_t traverser = 0;
  bool best_response = false;
  std::size_t flop_samples = 1;
  bs::SplitMix64* rng = nullptr;
  std::size_t fallbacks = 0;

  static std::size_t fallback_index(const std::vector<Action>& menu) {
    for (std::size_t i = 0; i < menu.size(); ++i)
      if (menu[i].type == ActionType::Check)
        return i;
    for (std::size_t i = 0; i < menu.size(); ++i)
      if (menu[i].type == ActionType::Call)
        return i;
    for (std::size_t i = 0; i < menu.size(); ++i)
      if (menu[i].type == ActionType::Fold)
        return i;
    return 0;
  }

  // Look up the policy row for this node, or build a fallback distribution.
  std::vector<double> policy_probs(const GameState& state, std::size_t actor,
                                   const std::vector<Action>& menu,
                                   const std::vector<PublicAction>& path,
                                   const std::vector<std::array<int, 2>>& holes) {
    const auto board = board_vector(state);
    const auto key = bs::solver::make_information_key(actor, holes[actor], board, path);
    const auto it = policy.rows().find(key);
    if (it != policy.rows().end()) {
      const auto& row = it->second;
      if (row.actions != menu)
        throw std::runtime_error("nash-conv: row menu drifted from the tree node");
      return row.probabilities;
    }
    ++fallbacks;
    std::vector<double> probs(menu.size(), 0.0);
    probs[fallback_index(menu)] = 1.0;
    return probs;
  }

  double walk(const GameState& state, std::size_t node_index,
              const std::vector<std::array<int, 2>>& holes, const std::vector<PublicAction>& path) {
    const auto& node = tree.node(node_index);

    if (node.kind == NodeKind::TerminalFold)
      return static_cast<double>(tree.terminal(node.terminal).chip_utility[traverser]);

    if (node.kind == NodeKind::TerminalShowdown) {
      const auto& payload = tree.terminal(node.terminal);
      std::vector<std::array<int, 2>> live(payload.live_count);
      for (std::size_t i = 0; i < payload.live_count; ++i)
        live[i] = holes[payload.live_order[i]];
      return static_cast<double>(state.settle_showdown(live).chip_utility[traverser]);
    }

    if (node.is_flop_deal()) {
      double total = 0.0;
      for (std::size_t s = 0; s < flop_samples; ++s) {
        GameState s1 = state;
        {
          auto cards = legal_runout_cards(s1, holes);
          s1 = s1.after_card(cards[rng->next_u64() % cards.size()]);
        }
        GameState s2 = s1;
        {
          auto cards = legal_runout_cards(s1, holes);
          s2 = s1.after_card(cards[rng->next_u64() % cards.size()]);
        }
        GameState frontier_state = s2;
        {
          auto cards = legal_runout_cards(s2, holes);
          frontier_state = s2.after_card(cards[rng->next_u64() % cards.size()]);
        }
        if (frontier_state.phase() != Phase::Frontier)
          throw std::runtime_error("nash-conv: flop-deal leaf did not reach the frontier");
        const TerminalPayload payload = bs::tree::make_frontier_payload(frontier_state);
        const std::vector<int> flop(frontier_state.board().begin(), frontier_state.board().end());
        std::vector<std::array<int, 2>> hands(frontier_state.player_count());
        for (std::size_t seat = 0; seat < frontier_state.player_count(); ++seat)
          hands[seat] = holes[seat];
        total += frontier.evaluate(flop, hands, payload)[traverser];
      }
      return total / static_cast<double>(flop_samples);
    }

    if (!node.is_action())
      throw std::runtime_error("nash-conv: unexpected node kind");
    const std::size_t actor = *state.actor();
    const auto& menu = node.actions;
    const auto probs = policy_probs(state, actor, menu, path, holes);

    // Build the child path with a placeholder action.
    std::vector<PublicAction> child_path;
    child_path.reserve(path.size() + 1);
    child_path = path;
    child_path.push_back({state.street(), actor, {}});

    // BR walk at the traverser's nodes: evaluate all actions, take the max.
    if (best_response && actor == traverser) {
      double best = -std::numeric_limits<double>::infinity();
      for (std::size_t a = 0; a < menu.size(); ++a) {
        child_path.back().action = menu[a];
        best = std::max(
            best, walk(state.after_action(actor, menu[a]), node.children[a], holes, child_path));
      }
      return best;
    }

    // Strategy walk (or BR at opponent nodes): sample one action, follow it.
    const std::size_t chosen = sample_action(probs, *rng);
    child_path.back().action = menu[chosen];
    return walk(state.after_action(actor, menu[chosen]), node.children[chosen], holes, child_path);
  }
};

}  // namespace

int main(int argc, char** argv) {
  const std::string artifact_path =
      argc > 1 ? argv[1] : "artifacts/preflop-profile/preflop-profile.db";
  const std::size_t num_deals = argc > 2 ? std::stoul(argv[2]) : 1000;
  const std::size_t flop_samples = argc > 3 ? std::stoul(argv[3]) : 1;
  const std::uint64_t seed = argc > 4 ? std::stoull(argv[4]) : 42;

  std::printf("Preflop NashConv measurement\n");
  std::printf("  artifact:     %s\n", artifact_path.c_str());
  std::printf("  deals:        %zu\n", num_deals);
  std::printf("  flops/leaf:   %zu\n", flop_samples);
  std::printf("  seed:         %llu\n", static_cast<unsigned long long>(seed));

  bs::artifacts::LoadOptions opts;
  opts.max_file_bytes = 1ULL << 33;  // 8 GiB
  bs::artifacts::LoadedArtifact loaded;
  try {
    loaded = bs::artifacts::load_artifact(artifact_path, opts);
  } catch (const std::exception& e) {
    std::fprintf(stderr, "failed to load artifact: %s\n", e.what());
    return 1;
  }
  if (!loaded.bundle.nseat) {
    std::fprintf(stderr, "artifact is not a v2/v3 nseat bundle\n");
    return 1;
  }
  const auto& policy = loaded.bundle.nseat->policy;
  std::printf("  info sets:    %zu\n", policy.rows().size());

  const GameDef& def = policy.game();
  const std::size_t seats = def.player_count;
  const AbstractTree tree(def, bs::abstraction::ActionAbstraction::identity());
  std::printf("  tree nodes:   %zu\n", tree.size());
  std::printf("  seats:        %zu\n", seats);

  const EquityFrontierEvaluator frontier;

  bs::SplitMix64 rng(seed);
  std::vector<double> strategy_value(seats, 0.0);
  std::vector<double> br_value(seats, 0.0);
  std::size_t uncovered = 0;
  std::size_t total_fallbacks = 0;

  const auto start = std::chrono::steady_clock::now();
  for (std::size_t d = 0; d < num_deals; ++d) {
    const auto holes = sample_joint_deal(rng, seats);
    const GameState root(def);
    for (std::size_t p = 0; p < seats; ++p) {
      try {
        NashConvWalker walker{tree,         policy, frontier, p, /*best_response=*/false,
                              flop_samples, &rng};
        strategy_value[p] += walker.walk(root, tree.root_index(), holes, {});
        total_fallbacks += walker.fallbacks;
        NashConvWalker br_walker{tree,         policy, frontier, p, /*best_response=*/true,
                                 flop_samples, &rng};
        br_value[p] += br_walker.walk(root, tree.root_index(), holes, {});
        total_fallbacks += br_walker.fallbacks;
      } catch (const std::exception& e) {
        ++uncovered;
        if (uncovered <= 3)
          std::fprintf(stderr, "deal %zu player %zu: %s\n", d, p, e.what());
      }
    }
    if ((d + 1) % 100 == 0)
      std::printf("  ... %zu/%zu deals\n", d + 1, num_deals);
  }
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - start);

  const std::size_t valid = num_deals * seats - uncovered;
  if (valid == 0) {
    std::fprintf(stderr, "all deals failed\n");
    return 1;
  }
  for (std::size_t p = 0; p < seats; ++p) {
    strategy_value[p] /= static_cast<double>(valid);
    br_value[p] /= static_cast<double>(valid);
  }
  double nash_conv = 0.0;
  for (std::size_t p = 0; p < seats; ++p)
    nash_conv += br_value[p] - strategy_value[p];
  const double exploitability = nash_conv / static_cast<double>(seats);
  const double bb = static_cast<double>(def.big_blind);

  std::printf("\nResults (%zu valid player-deals, %zu uncovered, %zu fallbacks):\n", valid,
              uncovered, total_fallbacks);
  for (std::size_t p = 0; p < seats; ++p) {
    std::printf("  player %zu: strategy=%+.2f  br=%+.2f  gap=%+.2f\n", p, strategy_value[p],
                br_value[p], br_value[p] - strategy_value[p]);
  }
  std::printf("  NashConv:       %.2f chips\n", nash_conv);
  std::printf("  Exploitability: %.2f chips (%.3f BB)\n", exploitability, exploitability / bb);
  std::printf("  wall time:      %.1f s\n", elapsed.count() / 1000.0);
  return 0;
}
