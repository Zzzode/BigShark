// RFC 0005 Stage 9 solver-level resolver tests.
//
// Every numeric expectation is hand-derived and cross-checked by the
// independently written TerminalOracle in resolver_oracle.{hpp,cpp}, which
// never calls the resolver traversal or the certifier. The tiny game is exact
// (two hero holdings, fixed singleton runout) so all margins and the gadget
// equilibrium are rational numbers asserted to tight tolerances.
#include <algorithm>
#include <array>
#include <bs/eval.hpp>
#include <bs/game_definition.hpp>
#include <bs/heads_up.hpp>
#include <bs/heads_up_solver.hpp>
#include <bs/resolver.hpp>
#include <bs/unified_game.hpp>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "resolver/certifier.hpp"
#include "resolver/counterfactual_reach.hpp"
#include "resolver/gadget_cfr.hpp"
#include "resolver/resolver_common.hpp"
#include "resolver_oracle.hpp"

using namespace bs::poker;
using namespace bs::solver;
using namespace bs::resolver;
namespace rd = bs::resolver::detail;
using bs::resolver_test::OracleDeal;
using bs::resolver_test::TerminalOracle;

#define CHECK(cond)                                                         \
  do {                                                                      \
    if (!(cond)) {                                                          \
      std::printf("CHECK failed: %s (%s:%d)\n", #cond, __FILE__, __LINE__); \
      return 1;                                                             \
    }                                                                       \
  } while (0)

namespace {

int card(const char* name) {
  return bs::cardId(std::string(name));
}
bool near(double a, double b, double tol = 1e-11) {
  return std::isfinite(a) && std::isfinite(b) && std::abs(a - b) <= tol;
}

using Action = bs::poker::Action;

// Map-backed blueprint source for the offline solver tests.
class MapBlueprint final : public BlueprintSource {
 public:
  explicit MapBlueprint(
      UnifiedGame game,
      std::string digest = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef")
      : game_(std::move(game)), digest_(std::move(digest)) {}

  void set_row(const GameState& state, std::span<const PublicAction> history, std::size_t player,
               std::array<int, 2> cards, std::vector<Action> actions, std::vector<double> probs) {
    rows_[{make_information_key(player, cards, state.board(), history), player}] = {
        std::move(actions), std::move(probs)};
  }

  const UnifiedGame& game() const override { return game_; }
  std::string_view artifact_digest() const override { return digest_; }
  std::optional<BlueprintRowView> row(const GameState& state, std::span<const PublicAction> history,
                                      std::size_t player, std::array<int, 2> cards) const override {
    const auto it =
        rows_.find({make_information_key(player, cards, state.board(), history), player});
    if (it == rows_.end())
      return std::nullopt;
    return BlueprintRowView{it->second.actions.data(), it->second.probs.data(),
                            it->second.probs.size()};
  }

 private:
  using Key = std::pair<InformationKey, std::size_t>;
  struct Row {
    std::vector<Action> actions;
    std::vector<double> probs;
  };
  UnifiedGame game_;
  std::string digest_;
  std::map<Key, Row> rows_;
};

struct TinyGame {
  HeadsUpGame game;
  UnifiedGame unified;
  // The resolver path is historyless: GameState plus the explicit public-action
  // path. `hnode` is the same node as a HeadsUpState, kept for the independent
  // oracle and the information_key identity cross-check.
  GameState state;
  std::vector<PublicAction> history;
  HeadsUpState hnode;
  std::array<int, 2> W{};
  std::array<int, 2> L{};
  std::array<int, 2> O{};
  std::vector<Action> actions;

  explicit TinyGame(HeadsUpGame g)
      : game(std::move(g)), unified(to_unified_game(game)), state(unified.def), hnode(game.root) {}
};

TinyGame make_canonical() {
  HeadsUpGame game;
  game.root = {{card("2c"), card("3d"), card("7h")}, {1, 1}, {1, 1}, 2, 1, 1};
  game.fixed_runout = {card("Js"), card("9c")};
  TinyGame t(std::move(game));
  t.W = {card("Ac"), card("Ad")};
  t.L = {card("8c"), card("8d")};
  t.O = {card("Qc"), card("Qd")};
  std::sort(t.W.begin(), t.W.end());
  std::sort(t.L.begin(), t.L.end());
  std::sort(t.O.begin(), t.O.end());
  t.game.ranges[1] = {{t.W, 1}, {t.L, 1}};
  t.game.ranges[0] = {{t.O, 1}};
  t.unified = to_unified_game(t.game);
  t.state = GameState(t.unified.def);
  t.state = t.state.after_action(0, {ActionType::Bet, 1});
  t.history = {{Street::Flop, 0, {ActionType::Bet, 1}}};
  t.hnode = HeadsUpState(t.game.root).after_action(0, {ActionType::Bet, 1});
  t.actions = {{ActionType::Fold}, {ActionType::Call}};
  return t;
}

// Installs the responder jam prefix row and one current-node baseline policy
// (fold/call probabilities for W then L) for every declared responder combo.
void install_prefix(MapBlueprint& bp, const TinyGame& t,
                    const std::vector<std::array<int, 2>>& responder_combos) {
  const GameState root_state(t.unified.def);
  for (const auto& r : responder_combos)
    bp.set_row(root_state, {}, 0, r, {{ActionType::Check}, {ActionType::Bet, 1}}, {0, 1});
}

ResolveLimits test_limits(std::uint64_t iterations = 100000) {
  ResolveLimits limits;
  limits.iterations = iterations;
  limits.time = std::chrono::seconds(30);
  limits.max_nodes = 100000000;
  limits.max_information_sets = 1000000;
  limits.public_seed = 0x5151515151515151ULL;
  return limits;
}

// Build the model directly through the whitebox internal API.
rd::ReachModel build(const TinyGame& t, MapBlueprint& bp, ResolveStatus& status) {
  std::string detail;
  rd::Budget budget(test_limits());
  rd::ReachModel model{t.state, t.history};
  status = rd::build_model(t.state, t.history, bp, budget, model, detail);
  if (status != ResolveStatus::Certified)
    std::printf("build_model status=%d detail=%s\n", static_cast<int>(status), detail.c_str());
  return model;
}

std::vector<OracleDeal> oracle_deals(const rd::ReachModel& model) {
  std::vector<OracleDeal> out;
  // The oracle is two-seat-specific (hero=1, responder=0); the responder is the
  // one non-hero seat.
  const std::size_t responder = model.hero == 0 ? 1 : 0;
  for (const rd::GadgetDeal& deal : model.deals)
    out.push_back({deal.hands[model.hero], deal.hands[responder], deal.weight});
  return out;
}

// Hand-derived canonical checks shared by fixtures.
int check_canonical_margins(const TinyGame& t, const rd::ReachModel& model) {
  TerminalOracle oracle(t.hnode, oracle_deals(model), t.actions, t.game.fixed_runout);
  CHECK(model.infosets.size() == 1);
  const rd::SeatInfoset& infoset = model.infosets[0];
  CHECK(infoset.cards == t.O);
  // Each of W/L pairs with O once at weight 1 -> m(O)=2, unnormalized.
  CHECK(near(infoset.mass, 2.0));
  CHECK(near(oracle.mass(t.O), 2.0));
  return 0;
}

// (i) Terminal-only counterexample: the per-combo fold-winners/call-losers
// whole-range policy has margin 1.5 and must never pass the atomic certifier.
int test_deceptive_whole_range_rejected() {
  TinyGame t = make_canonical();
  MapBlueprint bp(t.unified);
  install_prefix(bp, t, {t.O});
  // Baseline locks the CURRENT node to all-call, centering b(O)=0.
  bp.set_row(t.state, t.history, 1, t.W, t.actions, {0, 1});
  bp.set_row(t.state, t.history, 1, t.L, t.actions, {0, 1});

  ResolveStatus status;
  rd::ReachModel model = build(t, bp, status);
  CHECK(status == ResolveStatus::Certified);
  CHECK(check_canonical_margins(t, model) == 0);
  CHECK(near(model.infosets[0].baseline, 0.0));

  // The deceptive whole-range candidate: fold the winning hand, call the
  // losing hand. It is exactly one atomic map, so the certifier must see the
  // exploitability of the assembled policy rather than certify per combo.
  std::map<InformationKey, PolicyRow> candidate;
  candidate.emplace(information_key(t.hnode, t.W), PolicyRow{t.actions, {1, 0}});
  candidate.emplace(information_key(t.hnode, t.L), PolicyRow{t.actions, {0, 1}});

  rd::Budget cert_budget(test_limits());
  rd::Certification cert = rd::certify_candidate(model, candidate, test_limits(), cert_budget);
  CHECK(!cert.certified);
  CHECK(cert.status == ResolveStatus::CertificationRejected);
  CHECK(cert.margins.size() == 1);
  CHECK(near(cert.margins[0].candidate, 1.5));  // 0.5*(+1) + 0.5*(+2)
  CHECK(near(cert.margins[0].best_response, 1.5));
  CHECK(cert.margins[0].slack > 0.0);

  TerminalOracle oracle(t.hnode, oracle_deals(model), t.actions, t.game.fixed_runout);
  bs::resolver_test::HeroStrategy hero{
      {t.W, {1, 0}},
      {t.L, {0, 1}},
  };
  CHECK(near(oracle.candidate_margin(t.O, hero), 1.5));
  CHECK(oracle.candidate_margin(t.O, hero) >
        model.infosets[0].baseline + kCertPotTolerance * t.game.root.pot);
  return 0;
}

// (ii) A candidate that improves one responder infoset but worsens another is
// rejected even though its local move looked better.
int test_locally_better_globally_worse_rejected() {
  TinyGame t = make_canonical();
  const std::array<int, 2> Rb = [] {
    std::array<int, 2> c{card("4h"), card("4d")};
    std::sort(c.begin(), c.end());
    return c;
  }();
  t.game.ranges[0] = {{t.O, 1}, {Rb, 1}};
  t.unified = to_unified_game(t.game);
  MapBlueprint bp(t.unified);
  install_prefix(bp, t, {t.O, Rb});
  bp.set_row(t.state, t.history, 1, t.W, t.actions, {0, 1});  // baseline all-call
  bp.set_row(t.state, t.history, 1, t.L, t.actions, {0, 1});

  ResolveStatus status;
  rd::ReachModel model = build(t, bp, status);
  CHECK(status == ResolveStatus::Certified);
  CHECK(model.infosets.size() == 2);

  TerminalOracle oracle(t.hnode, oracle_deals(model), t.actions, t.game.fixed_runout);
  // Baseline all-call: O column (-2,+2) -> b(O)=0; Rb column (-2,-2) -> -2.
  bs::resolver_test::BaselineStrategy base{{t.W, {0, 1}}, {t.L, {0, 1}}};
  CHECK(near(oracle.baseline_margin(t.O, base), 0.0));
  CHECK(near(oracle.baseline_margin(Rb, base), -2.0));
  for (const auto& infoset : model.infosets) {
    const double expected = infoset.cards == t.O ? 0.0 : -2.0;
    CHECK(near(infoset.baseline, expected));
  }

  // call winners / fold losers: optimal for O, but it makes Rb worse (-0.5 vs
  // its all-call baseline -2).
  std::map<InformationKey, PolicyRow> candidate;
  candidate.emplace(information_key(t.hnode, t.W), PolicyRow{t.actions, {0, 1}});
  candidate.emplace(information_key(t.hnode, t.L), PolicyRow{t.actions, {1, 0}});
  bs::resolver_test::HeroStrategy hero{{t.W, {0, 1}}, {t.L, {1, 0}}};
  CHECK(near(oracle.candidate_margin(t.O, hero), -0.5));
  CHECK(near(oracle.candidate_margin(Rb, hero), -0.5));

  rd::Budget cert_budget(test_limits());
  rd::Certification cert = rd::certify_candidate(model, candidate, test_limits(), cert_budget);
  CHECK(!cert.certified);
  CHECK(cert.status == ResolveStatus::CertificationRejected);
  bool found_worse = false;
  for (const auto& margin : cert.margins) {
    if (margin.cards == Rb) {
      found_worse = true;
      CHECK(near(margin.candidate, -0.5));
      CHECK(margin.slack > 0.0);  // -0.5 > -2
    }
  }
  CHECK(found_worse);
  return 0;
}

// (iii)+(v) The gadget equilibrium candidate passes every bound and the
// independently computed augmented-game NashConv is within 1e-8 root-pot.
int test_equilibrium_passes_and_converges() {
  TinyGame t = make_canonical();
  MapBlueprint bp(t.unified);
  install_prefix(bp, t, {t.O});
  bp.set_row(t.state, t.history, 1, t.W, t.actions, {0, 1});  // baseline all-call, b(O)=0
  bp.set_row(t.state, t.history, 1, t.L, t.actions, {0, 1});

  ResolveStatus status;
  rd::ReachModel model = build(t, bp, status);
  CHECK(status == ResolveStatus::Certified);

  rd::Budget solve_budget(test_limits());
  rd::GadgetOutput gadget = rd::run_gadget_cfr(model, test_limits(100000), solve_budget);
  CHECK(gadget.status == ResolveStatus::Certified);

  rd::Budget cert_budget(test_limits());
  rd::Certification cert =
      rd::certify_candidate(model, gadget.candidate, test_limits(), cert_budget);
  CHECK(cert.status == ResolveStatus::Certified);
  CHECK(cert.certified);

  const PolicyRow& wrow = gadget.candidate.at(information_key(t.hnode, t.W));
  const PolicyRow& lrow = gadget.candidate.at(information_key(t.hnode, t.L));
  // Equilibrium: call the winner, fold the loser.
  CHECK(wrow.probabilities[1] > 0.9999);
  CHECK(lrow.probabilities[1] < 0.0001);
  for (const auto& margin : cert.margins) {
    CHECK(margin.slack <= 0.0);
    CHECK(near(margin.best_response, 0.0));
  }

  TerminalOracle oracle(t.hnode, oracle_deals(model), t.actions, t.game.fixed_runout);
  bs::resolver_test::HeroStrategy hero;
  bs::resolver_test::ResponderStrategy responder;
  std::map<std::array<int, 2>, double> payoff;
  hero[t.W] = wrow.probabilities;
  hero[t.L] = lrow.probabilities;
  const auto terminate = gadget.terminate.at(GadgetKey{0, t.O});
  responder[t.O] = terminate;
  payoff[t.O] = model.infosets[0].baseline;
  const auto report = oracle.nash_conv(responder, hero, payoff);
  std::printf("equilibrium gadget: iterations=%llu nashConv=%.3e responder=%.3e hero=%.3e\n",
              static_cast<unsigned long long>(gadget.completed_iterations), report.nash_conv,
              report.responder_gain, report.hero_gain);
  CHECK(report.nash_conv >= 0);
  CHECK(report.nash_conv <= kEquilibriumPotTolerance * t.game.root.pot);
  // Production certifier agrees with the independent oracle per infoset.
  CHECK(cert.margins.size() == 1);
  CHECK(near(cert.margins[0].candidate, oracle.candidate_margin(t.O, hero), 1e-10));
  return 0;
}

// (iv) A responder combination with zero counterfactual mass is recorded with
// mass zero, never divided by, and cannot carry candidate reach.
int test_zero_mass_infoset() {
  TinyGame t = make_canonical();
  const std::array<int, 2> Rz = [] {
    std::array<int, 2> c{card("Ad"), card("8d")};
    std::sort(c.begin(), c.end());
    return c;
  }();
  t.game.ranges[0] = {{t.O, 1}, {Rz, 1}};
  t.unified = to_unified_game(t.game);
  MapBlueprint bp(t.unified);
  install_prefix(bp, t, {t.O, Rz});
  bp.set_row(t.state, t.history, 1, t.W, t.actions, {0, 1});
  bp.set_row(t.state, t.history, 1, t.L, t.actions, {0, 1});

  ResolveStatus status;
  rd::ReachModel model = build(t, bp, status);
  CHECK(status == ResolveStatus::Certified);
  CHECK(model.infosets.size() == 1);
  CHECK(model.zero_mass[0].size() == 1);
  CHECK(model.zero_mass[0][0] == Rz);
  for (const rd::GadgetDeal& deal : model.deals)
    CHECK(deal.hands[0] != Rz);

  rd::Budget solve_budget(test_limits());
  rd::GadgetOutput gadget = rd::run_gadget_cfr(model, test_limits(20000), solve_budget);
  rd::Budget cert_budget(test_limits());
  rd::Certification cert =
      rd::certify_candidate(model, gadget.candidate, test_limits(), cert_budget);
  CHECK(cert.status == ResolveStatus::Certified);
  bool recorded_zero = false;
  for (const auto& margin : cert.margins) {
    if (margin.cards == Rz) {
      recorded_zero = true;
      CHECK(!margin.positive_mass);
      CHECK(margin.mass == 0.0);
      CHECK(margin.baseline == 0.0);
    }
  }
  CHECK(recorded_zero);

  TerminalOracle oracle(t.hnode, oracle_deals(model), t.actions, t.game.fixed_runout);
  CHECK(near(oracle.mass(Rz), 0.0));
  return 0;
}

// (vi) Any resource limit while solving or certifying discards the candidate.
int test_deadlines_discard() {
  TinyGame t = make_canonical();
  MapBlueprint bp(t.unified);
  install_prefix(bp, t, {t.O});
  bp.set_row(t.state, t.history, 1, t.W, t.actions, {0, 1});
  bp.set_row(t.state, t.history, 1, t.L, t.actions, {0, 1});

  Resolver resolver;
  // Solve deadline: the budget cannot pay for even one bounded iteration, so
  // the candidate is discarded before any training happens.
  ResolveLimits solve = test_limits();
  solve.time = std::chrono::milliseconds(1);
  ResolveResult solve_result = resolver.resolve(t.state, t.history, bp, solve);
  CHECK(solve_result.status == ResolveStatus::SolveDeadline);
  CHECK(solve_result.candidate.empty());
  CHECK(solve_result.completed_iterations == 0);

  // Solve deadline from the wall clock: a budget that derives a positive cap
  // but is exhausted by the per-node guard before those iterations complete
  // must also discard, never publish a partial candidate.
  Resolver resolver2;
  ResolveLimits wall = test_limits();
  wall.time = std::chrono::milliseconds(2);
  ResolveResult wall_result = resolver2.resolve(t.state, t.history, bp, wall);
  CHECK(wall_result.candidate.empty());

  // Certification deadline: solve completes but the certifier gets one node.
  Resolver resolver3;
  ResolveLimits certify = test_limits(1000);
  certify.certify_max_nodes = 1;
  ResolveResult cert_result = resolver3.resolve(t.state, t.history, bp, certify);
  CHECK(cert_result.status == ResolveStatus::CertifyDeadline);
  CHECK(cert_result.candidate.empty());
  return 0;
}

// (vii) Whole-range, private-independent selection.
int test_private_independence_and_cache() {
  TinyGame t = make_canonical();
  MapBlueprint bp(t.unified);
  install_prefix(bp, t, {t.O});
  bp.set_row(t.state, t.history, 1, t.W, t.actions, {0, 1});
  bp.set_row(t.state, t.history, 1, t.L, t.actions, {0, 1});

  Resolver resolver;
  ResolveResult first = resolver.resolve(t.state, t.history, bp, test_limits(50000));
  CHECK(first.status == ResolveStatus::Certified);
  CHECK(first.candidate.size() == 2);  // the whole range, selected before a hand
  // There is no hero-card argument; both counterfactual rows coexist.
  CHECK(first.candidate.contains(information_key(t.hnode, t.W)));
  CHECK(first.candidate.contains(information_key(t.hnode, t.L)));

  // A different public seed must not move the full-traversal candidate.
  ResolveLimits other_seed = test_limits(50000);
  other_seed.public_seed = 0xabcdef0123456789ULL;
  ResolveResult second = resolver.resolve(t.state, t.history, bp, other_seed);
  CHECK(second.status == ResolveStatus::Certified);
  CHECK(second.candidate.size() == first.candidate.size());
  for (const auto& [key, row] : first.candidate) {
    const auto& match = second.candidate.at(key);
    CHECK(match.actions == row.actions);
    CHECK(match.probabilities.size() == row.probabilities.size());
    for (std::size_t i = 0; i < row.probabilities.size(); ++i)
      CHECK(near(match.probabilities[i], row.probabilities[i], 1e-15));
  }

  // A fresh resolver reproduces the identical certified whole-range policy.
  Resolver resolver3;
  ResolveResult third = resolver3.resolve(t.state, t.history, bp, test_limits(50000));
  CHECK(third.status == ResolveStatus::Certified);
  for (const auto& [key, row] : first.candidate) {
    const auto& match = third.candidate.at(key);
    for (std::size_t i = 0; i < row.probabilities.size(); ++i)
      CHECK(near(match.probabilities[i], row.probabilities[i], 1e-15));
  }
  return 0;
}

// A non-terminal current node (a legal future decision remains after the
// current action) is ineligible, not certified.
int test_non_terminal_node_ineligible() {
  // Deep stacks: after a non-all-in bet and a call there IS a turn decision.
  HeadsUpGame game;
  std::array<int, 2> h0{card("Ac"), card("Ad")};
  std::array<int, 2> h1{card("8c"), card("8d")};
  std::sort(h0.begin(), h0.end());
  std::sort(h1.begin(), h1.end());
  game.root = {{card("2c"), card("3d"), card("7h")}, {10, 10}, {1, 1}, 2, 1, 1};
  game.ranges[0] = {{h0, 1}};
  game.ranges[1] = {{h1, 1}};
  game.fixed_runout = {card("Js"), card("9c")};
  const UnifiedGame unified = to_unified_game(game);
  const GameState root_state(unified.def);
  // p0 makes a small bet; p1 faces fold/call/raise and calling leaves stacks.
  const GameState state = root_state.after_action(0, {ActionType::Bet, 2});
  const std::vector<PublicAction> history{{Street::Flop, 0, {ActionType::Bet, 2}}};
  const HeadsUpState hnode = HeadsUpState(game.root).after_action(0, {ActionType::Bet, 2});
  MapBlueprint bp(unified);
  bp.set_row(root_state, {}, 0, h0, {{ActionType::Check}, {ActionType::Bet, 2}}, {0, 1});

  ResolveLimits limits = test_limits(10);
  rd::Budget budget(limits);
  rd::ReachModel model{state, history};
  std::string detail;
  const ResolveStatus status = rd::build_model(state, history, bp, budget, model, detail);
  CHECK(status == ResolveStatus::Ineligible);
  return 0;
}

// A hero combo that blocks every declared opponent combo contributes zero
// counterfactual mass, participates in no deal and no responder infoset, and
// therefore cannot be exploited by any opponent best response. It must not
// invalidate an otherwise certifiable whole-range candidate: the requirement
// covers every positive-mass hero combo, not every board-unblocked live combo.
int test_hero_combo_without_compatible_opponent() {
  HeadsUpGame game;
  std::array<int, 2> h0{card("Ad"), card("2d")};  // blocks the only opponent combo
  std::array<int, 2> h1{card("8c"), card("8d")};
  std::array<int, 2> o0{card("Ad"), card("Kd")};
  std::sort(h0.begin(), h0.end());
  std::sort(h1.begin(), h1.end());
  std::sort(o0.begin(), o0.end());
  game.root = {{card("2c"), card("3d"), card("7h")}, {1, 1}, {1, 1}, 2, 1, 1};
  game.ranges[1] = {{h0, 1}, {h1, 1}};
  game.ranges[0] = {{o0, 1}};
  game.fixed_runout = {card("Js"), card("9c")};
  const UnifiedGame unified = to_unified_game(game);
  const GameState root_state(unified.def);
  const GameState state = root_state.after_action(0, {ActionType::Bet, 1});
  const std::vector<PublicAction> history{{Street::Flop, 0, {ActionType::Bet, 1}}};
  const HeadsUpState hnode = HeadsUpState(game.root).after_action(0, {ActionType::Bet, 1});
  const std::vector<Action> actions{{ActionType::Fold}, {ActionType::Call}};
  MapBlueprint bp(unified);
  bp.set_row(root_state, {}, 0, o0, {{ActionType::Check}, {ActionType::Bet, 1}}, {0, 1});
  bp.set_row(state, history, 1, h0, actions, {0, 1});
  bp.set_row(state, history, 1, h1, actions, {0, 1});

  rd::Budget build_budget(test_limits());
  rd::ReachModel model{state, history};
  std::string detail;
  const ResolveStatus status = rd::build_model(state, history, bp, build_budget, model, detail);
  CHECK(status == ResolveStatus::Certified);
  // h0 blocks o0 entirely: it is live but carries no deal and no infoset.
  CHECK(model.live_hero.size() == 2);
  CHECK(model.deals.size() == 1);
  CHECK(model.deals[0].hands[model.hero] == h1);
  CHECK(model.infosets.size() == 1);

  // The gadget trains only the dealt hero combo; the certifier must accept the
  // candidate for the positive-mass range and must not fabricate a row for the
  // unreachable combo.
  rd::Budget solve_budget(test_limits());
  rd::GadgetOutput gadget = rd::run_gadget_cfr(model, test_limits(), solve_budget);
  CHECK(gadget.status == ResolveStatus::Certified);
  CHECK(gadget.candidate.size() == 1);
  rd::Budget cert_budget(test_limits());
  rd::Certification cert =
      rd::certify_candidate(model, gadget.candidate, test_limits(), cert_budget);
  CHECK(cert.status == ResolveStatus::Certified);
  CHECK(cert.certified);
  CHECK(cert.margins.size() == 1);
  CHECK(cert.margins[0].positive_mass);

  // End to end: the whole-range candidate must be published, not discarded.
  Resolver resolver;
  const ResolveResult result = resolver.resolve(state, history, bp, test_limits());
  CHECK(result.status == ResolveStatus::Certified);
  CHECK(result.candidate.size() == 1);
  CHECK(result.candidate.contains(information_key(hnode, h1)));
  CHECK(!result.candidate.contains(information_key(hnode, h0)));
  return 0;
}

// A request-derived time budget bounds the training WORK, not just the wall
// clock: the deterministic iteration cap scales with the budget AND with the
// per-iteration cost driver (joint deals x ordered actions), so a short budget
// or a wide range cannot silently spend the configured maximum. The derivation
// depends only on public inputs, so every counterfactual hero combination
// derives the identical cap and the candidate identity stays private-independent.
int test_iteration_cap_scales_with_budget() {
  // Deterministic and monotone in the public budget.
  const std::uint64_t configured = 1000000;
  constexpr std::size_t deals = 100;
  constexpr std::size_t actions = 2;
  const std::uint64_t tiny =
      iteration_cap_for_budget(std::chrono::milliseconds(10), configured, deals, actions, 2);
  const std::uint64_t small =
      iteration_cap_for_budget(std::chrono::milliseconds(100), configured, deals, actions, 2);
  const std::uint64_t large =
      iteration_cap_for_budget(std::chrono::milliseconds(1000), configured, deals, actions, 2);
  CHECK(tiny < small);
  CHECK(small < large);
  CHECK(large <= configured);
  // The configured cap is an upper bound, never exceeded.
  CHECK(iteration_cap_for_budget(std::chrono::milliseconds(600000), configured, deals, actions,
                                 2) == configured);
  // Repeated derivation is identical (no clock or hand dependence).
  CHECK(iteration_cap_for_budget(std::chrono::milliseconds(100), configured, deals, actions, 2) ==
        small);
  // A budget whose reserve consumes it entirely yields a zero cap, never a
  // negative or wrap-around value.
  CHECK(iteration_cap_for_budget(std::chrono::milliseconds(1), configured, deals, actions, 2) == 0);
  CHECK(iteration_cap_for_budget(std::chrono::milliseconds(0), configured, deals, actions, 2) == 0);
  // Degenerate shapes yield zero rather than dividing by a zero cost.
  CHECK(iteration_cap_for_budget(std::chrono::milliseconds(100), configured, 0, actions, 2) == 0);
  CHECK(iteration_cap_for_budget(std::chrono::milliseconds(100), configured, deals, 0, 2) == 0);
  CHECK(iteration_cap_for_budget(std::chrono::milliseconds(100), configured, deals, actions, 0) ==
        0);

  // The cost driver is deals x actions: at a fixed budget, a wider range or a
  // larger action menu gets a strictly smaller cap, which is what makes the
  // cap binding on the profiles the wall clock would otherwise govern.
  const auto budget = std::chrono::milliseconds(1000);
  const std::uint64_t narrow = iteration_cap_for_budget(budget, configured, 10, actions, 2);
  const std::uint64_t wide = iteration_cap_for_budget(budget, configured, 1000, actions, 2);
  const std::uint64_t wide_more_actions = iteration_cap_for_budget(budget, configured, 1000, 8, 2);
  CHECK(wide < narrow);
  CHECK(wide_more_actions < wide);
  // Linear in deals: 100x the deals at a fixed budget gives 1/100 the cap.
  CHECK(narrow / wide >= 99 && narrow / wide <= 101);

  // End to end: the canonical tiny game derives a cap from its own deal count
  // and still certifies, reproducibly.
  TinyGame t = make_canonical();
  MapBlueprint bp(t.unified);
  install_prefix(bp, t, {t.O});
  bp.set_row(t.state, t.history, 1, t.W, t.actions, {0, 1});
  bp.set_row(t.state, t.history, 1, t.L, t.actions, {0, 1});

  ResolveLimits limits = test_limits();
  Resolver resolver;
  const ResolveResult first = resolver.resolve(t.state, t.history, bp, limits);
  CHECK(first.status == ResolveStatus::Certified);
  Resolver resolver2;
  const ResolveResult second = resolver2.resolve(t.state, t.history, bp, limits);
  CHECK(second.status == ResolveStatus::Certified);
  CHECK(second.candidate.size() == first.candidate.size());
  CHECK(second.completed_iterations == first.completed_iterations);

  // A budget too small to pay for one bounded iteration must DISCARD, never
  // publish an untrained candidate as if it had been trained.
  ResolveLimits starved = test_limits();
  starved.time = std::chrono::milliseconds(1);
  Resolver resolver3;
  const ResolveResult discarded = resolver3.resolve(t.state, t.history, bp, starved);
  CHECK(discarded.status == ResolveStatus::SolveDeadline);
  CHECK(discarded.candidate.empty());
  CHECK(discarded.completed_iterations == 0);
  return 0;
}

// The cache identity must include the budget-derived iteration cap, not the
// caller's configured count: two requests at the same public state that differ
// only in solve budget derive different caps and therefore must NOT share a
// cached candidate. Otherwise a short-budget request would be served a
// long-budget candidate and its budget would never bound the training work.
int test_cache_identity_includes_budget() {
  TinyGame t = make_canonical();
  MapBlueprint bp(t.unified);
  install_prefix(bp, t, {t.O});
  bp.set_row(t.state, t.history, 1, t.W, t.actions, {0, 1});
  bp.set_row(t.state, t.history, 1, t.L, t.actions, {0, 1});

  // Learn the model's real deal/action counts once, so the expected caps below
  // are the same ones Resolver::resolve derives internally.
  ResolveStatus status;
  rd::ReachModel model = build(t, bp, status);
  CHECK(status == ResolveStatus::Certified);
  const std::size_t deals = model.deals.size();
  const std::size_t actions = model.node_actions.size();
  CHECK(deals > 0 && actions > 0);

  Resolver resolver;
  ResolveLimits long_budget = test_limits();
  long_budget.time = std::chrono::seconds(30);
  const ResolveResult first = resolver.resolve(t.state, t.history, bp, long_budget);
  CHECK(first.status == ResolveStatus::Certified);
  const std::uint64_t long_cap = iteration_cap_for_budget(long_budget.time, long_budget.iterations,
                                                          deals, actions, model.seat_count);
  // The cap is an upper bound, not a promise: a slow (sanitized) build may stop
  // earlier on the wall clock. Only the bound is asserted here.
  CHECK(first.completed_iterations <= long_cap);

  // Same public state, same configured iteration count and seed, but a budget
  // that derives a strictly smaller cap: the derived value is part of the cache
  // identity, so this request must train under its own cap instead of being
  // answered from the first request's entry. The discriminating property is
  // that the second call RESUMES ITS OWN bounded work rather than returning the
  // first call's iteration evidence verbatim; a slow sanitized build may stop
  // early on the wall clock, so only the bound and the non-reuse are asserted.
  ResolveLimits short_budget = test_limits();
  short_budget.time = std::chrono::milliseconds(200);
  const std::uint64_t short_cap = iteration_cap_for_budget(
      short_budget.time, short_budget.iterations, deals, actions, model.seat_count);
  CHECK(short_cap > 0);
  CHECK(short_cap < long_cap);
  const ResolveResult second = resolver.resolve(t.state, t.history, bp, short_budget);
  CHECK(second.completed_iterations <= short_cap);
  CHECK(second.completed_iterations != first.completed_iterations);

  // A repeated request at the SAME budget may reuse the cache: identical
  // identity yields a certified whole-range candidate without retraining, so
  // the second call reports exactly the first call's iteration evidence. The
  // 30 s budget keeps the derived cap binding well before the wall clock on
  // every supported build (including sanitized ones).
  Resolver resolver2;
  const ResolveResult warmup = resolver2.resolve(t.state, t.history, bp, long_budget);
  CHECK(warmup.status == ResolveStatus::Certified);
  const ResolveResult cached = resolver2.resolve(t.state, t.history, bp, long_budget);
  CHECK(cached.status == ResolveStatus::Certified);
  CHECK(cached.candidate.size() == warmup.candidate.size());
  CHECK(cached.completed_iterations == warmup.completed_iterations);

  // The node caps are part of the identity too: the documented certification
  // safety valve (certify_max_nodes small forces a discard) must not be
  // silently bypassed by a warm entry trained under a generous cap.
  Resolver resolver3;
  const ResolveResult generous = resolver3.resolve(t.state, t.history, bp, long_budget);
  CHECK(generous.status == ResolveStatus::Certified);
  ResolveLimits valved = long_budget;
  valved.certify_max_nodes = 1;
  const ResolveResult capped = resolver3.resolve(t.state, t.history, bp, valved);
  CHECK(capped.status == ResolveStatus::CertifyDeadline);
  CHECK(capped.candidate.empty());
  return 0;
}

// RFC 0009 W2c-ii-c: a three-seat terminal-only resolving gadget. The field is
// the separable sum of the two non-hero seats (seat0 QQ, seat1 TT), each owning
// one -x infoset. The hero (seat2) holds AA (winner) or 88 (loser) and faces a
// single Fold/Call decision after seat0 jams and seat1 calls. Both hero actions
// are terminal-only: a call runs out three ways, a fold leaves seat0 vs seat1
// to run out. The fixed runout {Js, 9c} makes every leaf value an exact rational.
//
// Hand-derived chip utilities (net, zero-sum; each seat has 2 behind and 2 in):
//   deal (AA, QQ, TT):  Fold -> hero -2, seat0 +6, seat1 -4
//                       Call -> hero +8, seat0 -4, seat1 -4  (hero wins)
//   deal (88, QQ, TT):  Fold -> hero -2, seat0 +6, seat1 -4
//                       Call -> hero -4, seat0 +8, seat1 -4  (seat0 wins)
// All-call baseline: b_0(QQ) = (-4 + 8)/2 = 2.0, b_1(TT) = -4.0.
struct ThreeSeatGame {
  UnifiedGame unified;
  GameState state;
  std::vector<PublicAction> history;
  std::array<int, 2> AA{};
  std::array<int, 2> EE{};
  std::array<int, 2> QQ{};
  std::array<int, 2> TT{};
  std::vector<Action> actions;

  explicit ThreeSeatGame(const GameDef& def) : state(def) {}
};

ThreeSeatGame make_three_seat() {
  GameDef def{};
  def.player_count = 3;
  def.button = 2;
  def.big_blind = 2;
  def.stacks = {2, 2, 2, 0, 0, 0, 0, 0, 0, 0};
  def.contributions = {2, 2, 2, 0, 0, 0, 0, 0, 0, 0};
  def.pot = 6;  // 3+ seats: pot == dead money + antes + posted blinds
  def.board = {card("2c"), card("3d"), card("7h"), 0, 0};
  def.board_size = 3;
  def.preflop = false;
  ThreeSeatGame g(def);
  g.unified.def = def;
  g.AA = {card("Ac"), card("Ad")};
  g.EE = {card("8c"), card("8d")};
  g.QQ = {card("Qc"), card("Qd")};
  g.TT = {card("Tc"), card("Td")};
  std::sort(g.AA.begin(), g.AA.end());
  std::sort(g.EE.begin(), g.EE.end());
  std::sort(g.QQ.begin(), g.QQ.end());
  std::sort(g.TT.begin(), g.TT.end());
  g.unified.ranges[0] = {{g.QQ, 1}};
  g.unified.ranges[1] = {{g.TT, 1}};
  g.unified.ranges[2] = {{g.AA, 1}, {g.EE, 1}};
  g.unified.fixed_runout = {card("Js"), card("9c")};
  // seat0 jams (all-in), seat1 calls (all-in); hero seat2 faces Fold/Call.
  const GameState after_bet = g.state.after_action(0, {ActionType::Bet, 2});
  g.state = after_bet.after_action(1, {ActionType::Call});
  g.history = {{Street::Flop, 0, {ActionType::Bet, 2}}, {Street::Flop, 1, {ActionType::Call}}};
  g.actions = {{ActionType::Fold}, {ActionType::Call}};
  return g;
}

// Installs the two prefix rows (seat0 jams, seat1 calls) and the all-call hero
// baseline at the node, centering b_0(QQ)=2.0 and b_1(TT)=-4.0.
void install_three_seat_prefix(MapBlueprint& bp, const ThreeSeatGame& g) {
  const GameState root_state(g.unified.def);
  const GameState after_bet = root_state.after_action(0, {ActionType::Bet, 2});
  const std::vector<PublicAction> bet_prefix{{Street::Flop, 0, {ActionType::Bet, 2}}};
  bp.set_row(root_state, {}, 0, g.QQ, {{ActionType::Check}, {ActionType::Bet, 2}}, {0, 1});
  bp.set_row(after_bet, bet_prefix, 1, g.TT, {{ActionType::Fold}, {ActionType::Call}}, {0, 1});
  bp.set_row(g.state, g.history, 2, g.AA, g.actions, {0, 1});
  bp.set_row(g.state, g.history, 2, g.EE, g.actions, {0, 1});
}

// (W2c-ii-c) The three-seat gadget equilibrium candidate passes PER-SEAT
// unilateral non-regression for BOTH non-hero seats and reports seat_count 3.
int test_three_seat_equilibrium_passes() {
  ThreeSeatGame g = make_three_seat();
  MapBlueprint bp(g.unified);
  install_three_seat_prefix(bp, g);

  std::string detail;
  rd::Budget build_budget(test_limits());
  rd::ReachModel model{g.state, g.history};
  const ResolveStatus status = rd::build_model(g.state, g.history, bp, build_budget, model, detail);
  CHECK(status == ResolveStatus::Certified);
  CHECK(model.seat_count == 3);
  CHECK(model.hero == 2);
  CHECK(model.node_actions == g.actions);  // {Fold, Call}
  // Two joint deals (AA/88 x QQ x TT), each feeding both non-hero infosets.
  CHECK(model.deals.size() == 2);
  CHECK(model.infosets.size() == 2);
  CHECK(near(model.total_mass, 2.0));

  rd::Budget solve_budget(test_limits());
  rd::GadgetOutput gadget = rd::run_gadget_cfr(model, test_limits(100000), solve_budget);
  CHECK(gadget.status == ResolveStatus::Certified);

  const InformationKey aa_key =
      make_information_key(model.hero, g.AA, model.node.board(), model.history);
  const InformationKey ee_key =
      make_information_key(model.hero, g.EE, model.node.board(), model.history);
  const PolicyRow& aa_row = gadget.candidate.at(aa_key);
  const PolicyRow& ee_row = gadget.candidate.at(ee_key);
  // Equilibrium: call the winner, fold the loser.
  CHECK(aa_row.probabilities[1] > 0.9999);
  CHECK(ee_row.probabilities[1] < 0.0001);

  rd::Budget cert_budget(test_limits());
  rd::Certification cert =
      rd::certify_candidate(model, gadget.candidate, test_limits(), cert_budget);
  CHECK(cert.status == ResolveStatus::Certified);
  CHECK(cert.certified);
  CHECK(cert.margins.size() == 2);
  for (const auto& margin : cert.margins) {
    CHECK(margin.positive_mass);
    CHECK(margin.slack <= 0.0);
    if (margin.seat == 0) {
      CHECK(margin.cards == g.QQ);
      CHECK(near(margin.mass, 2.0));
      CHECK(near(margin.baseline, 2.0, 1e-9));
      CHECK(near(margin.candidate, 1.0, 1e-9));
      CHECK(near(margin.best_response, 2.0, 1e-9));
    } else {
      CHECK(margin.seat == 1);
      CHECK(margin.cards == g.TT);
      CHECK(near(margin.mass, 2.0));
      CHECK(near(margin.baseline, -4.0, 1e-9));
      CHECK(near(margin.candidate, -4.0, 1e-9));
      CHECK(near(margin.best_response, -4.0, 1e-9));
    }
  }

  // End to end: the resolver reports seat_count 3, the condition under which
  // the host attaches the multiway diagnostic token.
  Resolver resolver;
  const ResolveResult result = resolver.resolve(g.state, g.history, bp, test_limits());
  CHECK(result.status == ResolveStatus::Certified);
  CHECK(result.seat_count == 3);
  CHECK(result.candidate.size() == 2);
  return 0;
}

// (W2c-ii-c) A deceptive candidate (fold the winner AA, call the loser 88)
// raises seat0's locked-continuation value to 7.0, well above the 2.0 baseline,
// so seat0's unilateral non-regression bound FAILS while seat1's (which loses
// every pot either way) still passes. The per-seat certifier REJECTS it. This
// is the multiway analogue of the two-seat deceptive test: the rejection is
// per-seat, not a two-player equilibrium bound.
int test_three_seat_per_seat_rejection() {
  ThreeSeatGame g = make_three_seat();
  MapBlueprint bp(g.unified);
  install_three_seat_prefix(bp, g);

  std::string detail;
  rd::Budget build_budget(test_limits());
  rd::ReachModel model{g.state, g.history};
  const ResolveStatus status = rd::build_model(g.state, g.history, bp, build_budget, model, detail);
  CHECK(status == ResolveStatus::Certified);

  // Deceptive candidate: fold the winner (AA), call the loser (88).
  const InformationKey aa_key =
      make_information_key(model.hero, g.AA, model.node.board(), model.history);
  const InformationKey ee_key =
      make_information_key(model.hero, g.EE, model.node.board(), model.history);
  std::map<InformationKey, PolicyRow> candidate;
  candidate[aa_key] = {model.node_actions, {1.0, 0.0}};  // AA -> fold
  candidate[ee_key] = {model.node_actions, {0.0, 1.0}};  // 88 -> call

  rd::Budget cert_budget(test_limits());
  rd::Certification cert = rd::certify_candidate(model, candidate, test_limits(), cert_budget);
  CHECK(cert.status == ResolveStatus::CertificationRejected);
  CHECK(!cert.certified);
  CHECK(cert.margins.size() == 2);
  for (const auto& margin : cert.margins) {
    if (margin.seat == 0) {
      CHECK(margin.cards == g.QQ);
      CHECK(near(margin.baseline, 2.0, 1e-9));
      CHECK(near(margin.candidate, 7.0, 1e-9));
      CHECK(margin.slack > 0.0);
    } else {
      CHECK(margin.seat == 1);
      CHECK(margin.cards == g.TT);
      CHECK(near(margin.baseline, -4.0, 1e-9));
      CHECK(near(margin.candidate, -4.0, 1e-9));
      CHECK(margin.slack <= 0.0);
    }
  }
  return 0;
}

}  // namespace

int main() {
  try {
    CHECK(test_deceptive_whole_range_rejected() == 0);
    CHECK(test_locally_better_globally_worse_rejected() == 0);
    CHECK(test_equilibrium_passes_and_converges() == 0);
    CHECK(test_zero_mass_infoset() == 0);
    CHECK(test_deadlines_discard() == 0);
    CHECK(test_private_independence_and_cache() == 0);
    CHECK(test_non_terminal_node_ineligible() == 0);
    CHECK(test_hero_combo_without_compatible_opponent() == 0);
    CHECK(test_iteration_cap_scales_with_budget() == 0);
    CHECK(test_cache_identity_includes_budget() == 0);
    CHECK(test_three_seat_equilibrium_passes() == 0);
    CHECK(test_three_seat_per_seat_rejection() == 0);
  } catch (const std::exception& error) {
    std::printf("Unexpected resolver exception: %s\n", error.what());
    return 1;
  }
  std::printf("test_resolver PASS\n");
  return 0;
}
