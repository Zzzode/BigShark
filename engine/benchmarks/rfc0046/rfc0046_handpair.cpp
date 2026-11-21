// RFC 0046 Stage-1 pre-frozen behavioral hand-pair selector (manual target).
//
// Runs the PINNED reference once - cap-1 exact-key river-rooted solve on the
// monotone board Qs Ks As Js / 9c, seed 4320, 2,000,000 iterations, full live
// range - then selects the deterministic FIRST pair in ConcreteCards dense
// order of two live combos in the SAME five-card CategoryTiersV1 bucket whose
// exact river-best actions differ strictly (per-action pooled best-response
// values against the solved concrete policy; ties prefer the lowest frozen
// action index). Emits the committed manifest JSON. If no pair exists on the
// monotone board it exits non-zero (the caller reruns on the predeclared
// fallback board; this binary takes the board as arguments for that purpose).
//
// Usage:
//   bigshark-rfc0046-handpair <c1> <c2> <c3> <c4> <river> <pot> <contrib> <stack>
// Benchmark-only.
#include <algorithm>
#include <array>
#include <bs/abstract_tree.hpp>
#include <bs/abstraction.hpp>
#include <bs/eval.hpp>
#include <bs/game_definition.hpp>
#include <bs/heads_up_solver.hpp>
#include <bs/nseat_trainer.hpp>
#include <chrono>
#include <cstdio>
#include <map>
#include <span>
#include <string>
#include <vector>

#include "concrete_keyer.hpp"

using namespace bs;
using namespace bs::rfc0046;

namespace {

// Pooled per-action best-response value for one hero combo at the river root
// against the opponent's concrete average policy. No chance nodes at a
// river-rooted tree; enumeration is over compatible opponent combos.
struct RiverBR {
  const tree::AbstractTree& tree;
  const ConcreteCardKeyer& keyer;
  const solver::NSeatTrainingResult& trained;
  std::size_t hero = 0;
  std::size_t opp = 1;

  // Gate-grade reference lookup: the pinned 2,000,000-iteration reference must
  // cover every reached river infoset; a missing row fails closed instead of
  // silently substituting uniform (M5), which would corrupt the argmax choice.
  std::vector<double> sigma_at(std::size_t node, const std::array<int, 2>& own,
                               const std::vector<poker::Action>& menu) const {
    const std::uint32_t b = keyer.dense_for_live_pair(own);
    const auto it =
        trained.raw_rows.find(solver::NSeatInformationKey{b, static_cast<std::uint64_t>(node)});
    if (it == trained.raw_rows.end() || it->second.actions != menu)
      throw std::runtime_error("handpair: pinned reference missing a reached river row");
    std::vector<double> s(menu.size());
    double total = 0.0;
    for (double x : it->second.sums)
      total += x;
    if (!(total > 0.0))
      throw std::runtime_error("handpair: pinned reference row has non-positive sum");
    for (std::size_t a = 0; a < menu.size(); ++a)
      s[a] = it->second.sums[a] / total;
    return s;
  }

  double terminal_hero(const poker::GameState& state, std::size_t node,
                       const std::array<int, 2>& hero_cards,
                       const std::array<int, 2>& opp_cards) const {
    const auto& payload = tree.terminal(tree.node(node).terminal);
    if (state.phase() == poker::Phase::Folded)
      return static_cast<double>(payload.chip_utility[hero]);
    std::array<std::array<int, 2>, 2> packed{};
    for (std::size_t i = 0; i < payload.live_count; ++i) {
      const std::size_t seat = payload.live_order[i];
      packed[i] = (seat == hero) ? hero_cards : opp_cards;
    }
    return static_cast<double>(
        state
            .settle_showdown(std::span<const std::array<int, 2>>(packed.data(), payload.live_count))
            .chip_utility[hero]);
  }

  // Fixed-action subtree value for one joint deal.
  double walk(const poker::GameState& state, std::size_t node, const std::array<int, 2>& hero_cards,
              const std::array<int, 2>& opp_cards, std::size_t fixed_root_action) const {
    const auto& n = tree.node(node);
    if (n.is_terminal())
      return terminal_hero(state, node, hero_cards, opp_cards);
    const std::size_t actor = *state.actor();
    const auto& menu = n.actions;
    const auto own = (actor == hero) ? hero_cards : opp_cards;
    const auto sigma = sigma_at(node, own, menu);
    double total = 0.0;
    for (std::size_t a = 0; a < menu.size(); ++a) {
      // At the root the hero takes exactly fixed_root_action when this is its
      // node; elsewhere both seats follow the policy.
      const bool root_hero = (node == tree.root_index() && actor == hero);
      const double p = root_hero ? (a == fixed_root_action ? 1.0 : 0.0) : sigma[a];
      if (p > 0.0)
        total += p * walk(state.after_action(actor, menu[a]), n.children[a], hero_cards, opp_cards,
                          fixed_root_action);
    }
    return total;
  }

  // Per-action pooled value for one hero holding over every compatible opp
  // combo (uniform prior, blocker filtered).
  std::vector<double> action_values(const std::array<int, 2>& hero_cards) const {
    const poker::GameState root(tree.def());
    const auto& menu = tree.node(tree.root_index()).actions;
    std::vector<double> values(menu.size(), 0.0);
    double mass = 0.0;
    for (int x = 0; x < 52; ++x)
      for (int y = x + 1; y < 52; ++y) {
        const std::array<int, 2> opp_cards{x, y};
        if (opp_cards[0] == hero_cards[0] || opp_cards[0] == hero_cards[1] ||
            opp_cards[1] == hero_cards[0] || opp_cards[1] == hero_cards[1])
          continue;
        bool blocked = false;
        for (int b : root.board())
          if (b == x || b == y)
            blocked = true;
        if (blocked)
          continue;
        for (std::size_t a = 0; a < menu.size(); ++a)
          values[a] += walk(root, tree.root_index(), hero_cards, opp_cards, a);
        mass += 1.0;
      }
    if (mass > 0.0)
      for (double& v : values)
        v /= mass;
    return values;
  }
};

}  // namespace

int main(int argc, char** argv) {
  if (argc != 9 && argc != 10) {
    std::fprintf(stderr, "usage: %s c1 c2 c3 c4 river pot contrib stack [--fallback]\n", argv[0]);
    return 2;
  }
  const bool fallback_used = (argc == 10 && std::string_view(argv[9]) == "--fallback");
  if (argc == 10 && !fallback_used) {
    std::fprintf(stderr, "handpair: optional 9th argument must be --fallback\n");
    return 2;
  }
  const std::array<int, 5> board{cardId(std::string(argv[1])), cardId(std::string(argv[2])),
                                 cardId(std::string(argv[3])), cardId(std::string(argv[4])),
                                 cardId(std::string(argv[5]))};
  const int pot = std::stoi(argv[6]);
  const int contrib = std::stoi(argv[7]);
  const int stack = std::stoi(argv[8]);

  poker::GameDef def{};
  def.player_count = 2;
  def.button = 0;
  def.big_blind = 2;
  def.stacks = {static_cast<poker::Chips>(stack), static_cast<poker::Chips>(stack)};
  def.contributions = {static_cast<poker::Chips>(contrib), static_cast<poker::Chips>(contrib)};
  def.pot = pot;
  def.board = board;
  def.board_size = 5;
  def.preflop = false;
  def.terminal = poker::TerminalDepth::River;
  const tree::AbstractTree tree(def, abstraction::ActionAbstraction::identity(1));
  const std::array<int, 4> board4{board[0], board[1], board[2], board[3]};
  const ConcreteCardKeyer keyer(board4);

  std::vector<solver::WeightedHand> range;
  for (const auto& h : live_unblocked_holdings(board))
    range.push_back(solver::WeightedHand{h, 1.0});
  std::vector<std::vector<solver::WeightedHand>> ranges{range, range};

  solver::NSeatTrainerLimits limits;
  limits.wall = std::chrono::seconds(3600);
  constexpr std::uint64_t kIters = 2'000'000;
  const auto trained =
      solver::train_nseat(tree, ranges, kIters, 4320, limits, nullptr, nullptr, nullptr, &keyer);
  if (trained.completed_iterations != kIters) {
    std::fprintf(stderr, "handpair: reference did not complete %llu iterations\n",
                 static_cast<unsigned long long>(kIters));
    return 3;
  }

  const poker::GameState probe(def);
  const std::size_t root_actor = probe.actor().value_or(0);
  RiverBR br{tree, keyer, trained, root_actor, root_actor == 0 ? std::size_t{1} : std::size_t{0}};
  struct Combo {
    std::array<int, 2> cards;
    std::uint32_t dense;
    std::uint32_t category;
    std::size_t argmax;
    double margin;  // V(best) - V(second best), strict > 0 required
  };
  std::vector<Combo> combos;
  for (const auto& h : live_unblocked_holdings(board)) {
    const std::uint32_t cat = static_cast<std::uint32_t>(
        abstraction::card_bucket(abstraction::CardBucketKind::CategoryTiersV1, h, board));
    const std::vector<double> values = br.action_values(h);
    std::size_t best = 0;
    std::size_t second = values.size() > 1 ? 1 : 0;
    if (values.size() > 1 && values[1] > values[0]) {
      best = 1;
      second = 0;
    }
    for (std::size_t a = 2; a < values.size(); ++a)
      if (values[a] > values[best]) {
        second = best;
        best = a;
      } else if (values[a] > values[second]) {
        second = a;
      }
    combos.push_back(
        Combo{h, keyer.dense_for_live_pair(h), cat, best, values[best] - values[second]});
  }

  for (std::size_t i = 0; i < combos.size(); ++i)
    for (std::size_t j = i + 1; j < combos.size(); ++j) {
      if (combos[i].category != combos[j].category)
        continue;
      if (combos[i].argmax == combos[j].argmax)
        continue;
      // Both argmax decisions must be strictly optimal (no ties).
      if (!(combos[i].margin > 0.0 && combos[j].margin > 0.0))
        continue;
      // Strictly different argmax: non-vacuous pair. Emit manifest.
      const auto name = [](int c) { return cardName(c); };
      // FNV-1a digest of the pinned reference identity (the evaluator pins
      // seed/iters and hard-requires this digest, M5).
      const std::string identity = std::string("rfc0046-handpair|") + argv[1] + argv[2] + argv[3] +
                                   argv[4] + "|" + argv[5] + "|cap1|fulluniform|seed4320|" +
                                   std::to_string(kIters) + "|alg" +
                                   std::to_string(solver::kNSeatAlgorithmRevision);
      std::uint64_t digest = 0xcbf29ce484222325ull;
      for (char c : identity) {
        digest ^= static_cast<unsigned char>(c);
        digest *= 0x100000001b3ull;
      }
      char digest_hex[17];
      std::snprintf(digest_hex, sizeof(digest_hex), "%016llx",
                    static_cast<unsigned long long>(digest));
      std::printf("{\n");
      std::printf("  \"reference_digest\": \"%s\",\n", digest_hex);
      std::printf("  \"board\": \"%s%s%s%s\",\n  \"river\": \"%s\",\n", argv[1], argv[2], argv[3],
                  argv[4], argv[5]);
      std::printf("  \"seed\": 4320,\n  \"iterations\": 2000000,\n");
      std::printf("  \"combo_a\": [\"%s\", \"%s\"],\n  \"combo_b\": [\"%s\", \"%s\"],\n",
                  name(combos[i].cards[0]).c_str(), name(combos[i].cards[1]).c_str(),
                  name(combos[j].cards[0]).c_str(), name(combos[j].cards[1]).c_str());
      std::printf("  \"category_id\": %u,\n", combos[i].category);
      std::printf("  \"action_indices\": [%zu, %zu],\n", combos[i].argmax, combos[j].argmax);
      std::printf("  \"value_gap\": %.12g,\n", std::min(combos[i].margin, combos[j].margin));
      std::printf("  \"dense_ids\": [%u, %u],\n  \"fallback_used\": %s\n}\n", combos[i].dense,
                  combos[j].dense, fallback_used ? "true" : "false");
      return 0;
    }
  std::fprintf(stderr, "handpair: no argmax-separating same-category pair on this board\n");
  return 4;
}
