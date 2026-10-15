// Complete six-dealt native cash-hand ledger regressions. All actions and
// public cards pass through GameState from the original preflop root; there
// is no rebase, fabricated snapshot, manual refund credit, trainer or solver.
// Literal payouts verify the current offline zero-rake/clockwise-odd-chip
// profile only, not provider rule equivalence, wire support or GTO quality.
#include <algorithm>
#include <array>
#include <bs/eval.hpp>
#include <bs/game_definition.hpp>
#include <bs/settlement.hpp>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using bs::poker::Action;
using bs::poker::ActionType;
using bs::poker::Chips;
using bs::poker::ContributionSettlement;
using bs::poker::GameDef;
using bs::poker::GameState;
using bs::poker::Phase;
using bs::poker::Street;
using Ledger = std::array<Chips, 6>;
using Utilities = std::array<std::int64_t, 6>;
using Holding = std::array<int, 2>;

void require(bool condition, const std::string& description) {
  if (!condition)
    throw std::runtime_error(description);
}

int card(const char* name) {
  return bs::cardId(std::string(name));
}

struct Refund {
  std::size_t role;
  Chips amount;
};

struct ExpectedLayer {
  Chips level;
  Chips amount;
  std::vector<std::size_t> contributors;
  std::vector<std::size_t> eligible;
  // Winners are deliberately specified in clockwise order, not sorted by
  // input index: the odd chip is assigned left of the original button.
  std::vector<std::size_t> winners;
};

// Roles 0=SB, 1=BB, 2=UTG, 3/4=following seats, 5=button. Rotate the
// original stacks and every expected ledger together across all six buttons.
class Hand {
 public:
  Hand(const char* scenario, std::size_t button, Ledger initial, Chips bigBlind)
      : name_(std::string(scenario) + "/button" + std::to_string(button)),
        button_(button),
        initial_(initial),
        state_(definition(button, initial, bigBlind)) {
    gross_[0] = committed_[0] = bigBlind / 2;
    gross_[1] = committed_[1] = bigBlind;
    verify();
  }

  const GameState& state() const { return state_; }
  std::size_t seat(std::size_t role) const { return (button_ + role + 1) % 6; }

  void act(std::size_t role, Action action, Chips paid, Phase after,
           std::optional<std::size_t> nextRole, std::optional<Refund> refund = std::nullopt) {
    verify();
    require(phase_ == Phase::Action && actor_ == role && state_.actor() == seat(role),
            label("specified role must be the real native actor"));
    const auto legal = state_.legal();
    require(legal.contains(action), label("specified action/kind/target must be legal"));
    if (action.type == ActionType::Call)
      require(legal.call && legal.call_amount == paid, label("literal capped call payment"));
    if (action.type == ActionType::Fold || action.type == ActionType::Check)
      require(paid == 0 && action.target_total == 0, label("non-payment action shape"));
    if (action.type == ActionType::Bet || action.type == ActionType::Raise)
      require(legal.aggressive && legal.aggressive->type == action.type &&
                  action.target_total >= committed_[role] &&
                  action.target_total - committed_[role] == paid,
              label("literal aggressive payment and exact native kind"));

    // Native transitions identify and return any unmatched amount. Updating
    // the expected ledger below is an assertion model, never a second credit
    // to the native state and never an inferred pot-division algorithm.
    state_ = state_.after_action(seat(role), action);
    gross_[role] += paid;
    committed_[role] += paid;
    if (action.type == ActionType::Fold)
      folded_[role] = true;
    if (refund) {
      require(committed_[refund->role] >= refund->amount, label("literal refund level"));
      refunds_[refund->role] += refund->amount;
      committed_[refund->role] -= refund->amount;
    }
    phase_ = after;
    actor_ = nextRole;
    ++step_;
    verify();
  }

  void deal(int publicCard, Street street, Phase after, std::optional<std::size_t> nextRole) {
    verify();
    require(phase_ == Phase::Deal && !state_.actor(), label("public card at native deal boundary"));
    state_ = state_.after_card(publicCard);
    board_.push_back(publicCard);
    // Native preflop commitments survive the first two partial-flop cards;
    // the complete flop and each subsequent street reset commitments.
    if (board_.size() >= 3)
      committed_.fill(0);
    street_ = street;
    phase_ = after;
    actor_ = nextRole;
    ++step_;
    verify();
  }

  void flop(const std::array<int, 3>& cards, std::size_t firstRole) {
    deal(cards[0], Street::Flop, Phase::Deal, std::nullopt);
    deal(cards[1], Street::Flop, Phase::Deal, std::nullopt);
    deal(cards[2], Street::Flop, Phase::Action, firstRole);
  }

  void loneCheck(std::size_t role, Phase after) {
    const auto legal = state_.legal();
    require(legal.check && !legal.fold && !legal.call && !legal.aggressive,
            label("current N-seat profile exposes only a sole-actionable-seat check"));
    act(role, {ActionType::Check}, 0, after, std::nullopt);
  }

  void literal(const Ledger& gross, const Ledger& refunds, Chips pot) const {
    require(gross_ == gross && refunds_ == refunds && state_.pot() == pot,
            label("independent complete literal G/F/pot ledger"));
  }

  void showdown(const std::array<Holding, 6>& holes, const Ledger& awards, const Utilities& utility,
                const std::vector<ExpectedLayer>& layers) const {
    verify();
    require(phase_ == Phase::Showdown && board_.size() == 5, label("genuine native showdown"));
    std::vector<Holding> liveHoles;
    for (std::size_t liveSeat : state_.live_players()) {
      const std::size_t role = (liveSeat + 6 - seat(0)) % 6;
      require(!folded_[role], label("live holes use occupied-seat order"));
      liveHoles.push_back(holes[role]);
    }
    // All hole cards are synthetic offline test input. No live opponent
    // private information is acquired by this regression.
    const ContributionSettlement result = state_.settle_showdown(liveHoles);
    require(result.awards.size() == 6 && result.refunds.size() == 6 &&
                result.net_contributions.size() == 6 && result.final_stacks.size() == 6 &&
                result.chip_utility.size() == 6,
            label("settlement retains all six dealt seats"));
    require(result.layers.size() == layers.size() && result.rake == 0,
            label("literal layer count and declared native zero rake"));
    for (std::size_t i = 0; i < layers.size(); ++i) {
      const auto& actual = result.layers[i];
      const auto& expected = layers[i];
      require(actual.contribution_level == expected.level && actual.amount == expected.amount &&
                  actual.rake == 0 && actual.contributors == seatSet(expected.contributors) &&
                  actual.eligible == seatSet(expected.eligible) &&
                  actual.winners == seatOrder(expected.winners),
              label("literal contributors/eligibility/winner order/layer " + std::to_string(i)));
    }
    Chips grossTotal = 0, refundTotal = 0, awardTotal = 0, initialTotal = 0, finalTotal = 0;
    std::int64_t utilityTotal = 0;
    for (std::size_t role = 0; role < 6; ++role) {
      const std::size_t p = seat(role);
      const auto& chips = state_.players()[p];
      require(result.awards[p] == awards[role] && result.refunds[p] == refunds_[role] &&
                  result.net_contributions[p] == gross_[role] - refunds_[role] &&
                  result.chip_utility[p] == utility[role],
              label("literal payout role " + std::to_string(role)));
      require(result.final_stacks[p] == chips.stack + awards[role] &&
                  static_cast<std::int64_t>(initial_[role]) + utility[role] ==
                      static_cast<std::int64_t>(result.final_stacks[p]) &&
                  utility[role] == static_cast<std::int64_t>(awards[role]) +
                                       static_cast<std::int64_t>(refunds_[role]) -
                                       static_cast<std::int64_t>(gross_[role]),
              label("refund counted once and hand-start utility role " + std::to_string(role)));
      if (folded_[role])
        require(result.awards[p] == 0, label("folded dead-money contributor cannot win"));
      grossTotal += gross_[role];
      refundTotal += refunds_[role];
      awardTotal += awards[role];
      initialTotal += initial_[role];
      finalTotal += result.final_stacks[p];
      utilityTotal += utility[role];
    }
    require(result.gross_contributions == grossTotal && result.total_refunds == refundTotal &&
                result.pot == state_.pot() && result.pot == grossTotal - refundTotal &&
                awardTotal + result.rake + refundTotal == grossTotal &&
                utilityTotal == -static_cast<std::int64_t>(result.rake) &&
                finalTotal + result.rake == initialTotal,
            label("complete cash conservation"));
    verify();  // Settlement returns values; it must not credit or mutate the cursor.
    std::printf("[native six-dealt ledger] %s passed\n", name_.c_str());
  }

 private:
  static GameDef definition(std::size_t button, const Ledger& initial, Chips bigBlind) {
    GameDef result{};
    result.player_count = 6;
    result.button = button;
    result.big_blind = bigBlind;
    result.preflop = true;
    result.terminal = bs::poker::TerminalDepth::River;
    result.pot = bigBlind + bigBlind / 2;
    for (std::size_t role = 0; role < 6; ++role)
      result.stacks[(button + role + 1) % 6] = initial[role];
    result.blinds_posted[(button + 1) % 6] = bigBlind / 2;
    result.blinds_posted[(button + 2) % 6] = bigBlind;
    return result;
  }

  std::string label(const std::string& detail) const {
    return name_ + "/step" + std::to_string(step_) + ": " + detail;
  }

  std::vector<std::size_t> seatOrder(const std::vector<std::size_t>& roles) const {
    std::vector<std::size_t> mapped;
    for (std::size_t role : roles)
      mapped.push_back(seat(role));
    return mapped;
  }

  std::vector<std::size_t> seatSet(const std::vector<std::size_t>& roles) const {
    auto mapped = seatOrder(roles);
    std::sort(mapped.begin(), mapped.end());
    return mapped;
  }

  void verify() const {
    require(state_.player_count() == 6 && state_.def().button == button_ && state_.def().preflop &&
                state_.def().board_size == 0 &&
                state_.def().terminal == bs::poker::TerminalDepth::River,
            label("original six-dealt hand-start definition is never rebased or compacted"));
    require(state_.street() == street_ && state_.phase() == phase_,
            label("literal street and phase"));
    require(actor_ ? state_.actor() == seat(*actor_) : !state_.actor(),
            label("literal next actor"));
    require(std::equal(state_.board().begin(), state_.board().end(), board_.begin(), board_.end()),
            label("ordered public board"));
    Chips netTotal = 0;
    std::vector<std::size_t> expectedLive;
    for (std::size_t role = 0; role < 6; ++role) {
      const auto& player = state_.players()[seat(role)];
      const Chips expectedStack = initial_[role] + refunds_[role] - gross_[role];
      require(state_.def().stacks[seat(role)] == initial_[role] && player.stack == expectedStack &&
                  player.contributed == gross_[role] && player.refunded == refunds_[role] &&
                  player.street_committed == committed_[role] &&
                  player.net_contributed() == gross_[role] - refunds_[role] &&
                  player.folded == folded_[role] && player.all_in == (expectedStack == 0),
              label("literal stack/G/F/commit/status role " + std::to_string(role)));
      require(player.stack + player.contributed - player.refunded == initial_[role],
              label("pre-award hand-start chips role " + std::to_string(role)));
      netTotal += gross_[role] - refunds_[role];
      if (!folded_[role])
        expectedLive.push_back(seat(role));
    }
    std::sort(expectedLive.begin(), expectedLive.end());
    const auto live = state_.live_players();
    require(state_.pot() == netTotal &&
                std::equal(live.begin(), live.end(), expectedLive.begin(), expectedLive.end()),
            label("net pot and retained live/dealt distinction"));
  }

  std::string name_;
  std::size_t button_;
  Ledger initial_;
  GameState state_;
  Ledger gross_{};
  Ledger refunds_{};
  Ledger committed_{};
  std::array<bool, 6> folded_{};
  Street street_ = Street::Preflop;
  Phase phase_ = Phase::Action;
  std::optional<std::size_t> actor_{2};
  std::vector<int> board_;
  std::size_t step_ = 0;
};

void refundAndRevival(std::size_t button) {
  Hand hand("A-preflop-refund-revival", button, {100, 60, 100, 100, 100, 100}, 20);
  for (std::size_t role = 2; role <= 5; ++role)
    hand.act(role, {ActionType::Fold}, 0, Phase::Action, (role + 1) % 6);
  hand.act(0, {ActionType::Raise, 100}, 90, Phase::Action, 1);
  require(hand.state().players()[hand.seat(0)].all_in,
          "A native wager really empties the small blind");
  hand.act(1, {ActionType::Call}, 40, Phase::Deal, std::nullopt, Refund{0, 40});
  hand.literal({100, 60, 0, 0, 0, 0}, {40, 0, 0, 0, 0, 0}, 120);
  require(!hand.state().players()[hand.seat(0)].all_in,
          "A native refund revives the six-dealt seat");
  hand.flop({card("2c"), card("3d"), card("7h")}, 0);
  hand.loneCheck(0, Phase::Deal);
  hand.deal(card("9s"), Street::Turn, Phase::Action, 0);
  hand.loneCheck(0, Phase::Deal);
  hand.deal(card("Jc"), Street::River, Phase::Action, 0);
  hand.loneCheck(0, Phase::Showdown);
  std::array<Holding, 6> holes{};
  holes[0] = {card("As"), card("Ad")};
  holes[1] = {card("Ks"), card("Kd")};
  hand.showdown(holes, {120, 0, 0, 0, 0, 0}, {60, -60, 0, 0, 0, 0},
                {{60, 120, {0, 1}, {0, 1}, {0}}});
}

void foldedDeadMoneyAndSingleEligible(std::size_t button) {
  Hand hand("B-dead-money-single-eligible", button, {140, 200, 60, 200, 200, 200}, 20);
  hand.act(2, {ActionType::Call}, 20, Phase::Action, 3);
  hand.act(3, {ActionType::Call}, 20, Phase::Action, 4);
  hand.act(4, {ActionType::Call}, 20, Phase::Action, 5);
  hand.act(5, {ActionType::Fold}, 0, Phase::Action, 0);
  hand.act(0, {ActionType::Raise, 100}, 90, Phase::Action, 1);
  hand.act(1, {ActionType::Call}, 80, Phase::Action, 2);
  hand.act(2, {ActionType::Call}, 40, Phase::Action, 3);
  hand.act(3, {ActionType::Fold}, 0, Phase::Action, 4);
  hand.act(4, {ActionType::Fold}, 0, Phase::Deal, std::nullopt);
  hand.literal({100, 100, 60, 20, 20, 0}, {}, 300);
  hand.flop({card("2c"), card("3d"), card("7h")}, 0);
  // At six dealt seats the small blind is first left of the original button.
  // A BB check before this bet would be illegal; no synthetic check or button
  // rewrite is inserted to mimic heads-up postflop order.
  hand.act(0, {ActionType::Bet, 40}, 40, Phase::Action, 1);
  require(hand.state().players()[hand.seat(0)].all_in,
          "B flop wager really empties the small blind");
  hand.act(1, {ActionType::Fold}, 0, Phase::Deal, std::nullopt, Refund{0, 40});
  hand.literal({140, 100, 60, 20, 20, 0}, {40, 0, 0, 0, 0, 0}, 300);
  require(!hand.state().players()[hand.seat(0)].all_in, "B returned wager revives the native seat");
  hand.deal(card("9s"), Street::Turn, Phase::Action, 0);
  hand.loneCheck(0, Phase::Deal);
  hand.deal(card("Jc"), Street::River, Phase::Action, 0);
  hand.loneCheck(0, Phase::Showdown);
  std::array<Holding, 6> holes{};
  holes[0] = {card("Qs"), card("Qd")};
  holes[2] = {card("As"), card("Ad")};
  hand.showdown(holes, {80, 0, 220, 0, 0, 0}, {-20, -100, 160, -20, -20, 0},
                {{20, 100, {0, 1, 2, 3, 4}, {0, 2}, {2}},
                 {60, 120, {0, 1, 2}, {0, 2}, {2}},
                 {100, 80, {0, 1}, {0}, {0}}});
}

void boardTieAndOddChip(std::size_t button) {
  Hand hand("C-board-tie-odd-chip", button, {100, 100, 100, 100, 100, 100}, 2);
  hand.act(2, {ActionType::Call}, 2, Phase::Action, 3);
  hand.act(3, {ActionType::Fold}, 0, Phase::Action, 4);
  hand.act(4, {ActionType::Fold}, 0, Phase::Action, 5);
  hand.act(5, {ActionType::Fold}, 0, Phase::Action, 0);
  hand.act(0, {ActionType::Call}, 1, Phase::Action, 1);
  hand.act(1, {ActionType::Check}, 0, Phase::Deal, std::nullopt);
  hand.flop({card("2c"), card("3d"), card("4h")}, 0);
  hand.act(0, {ActionType::Check}, 0, Phase::Action, 1);
  hand.act(1, {ActionType::Bet, 3}, 3, Phase::Action, 2);
  hand.act(2, {ActionType::Call}, 3, Phase::Action, 0);
  hand.act(0, {ActionType::Call}, 3, Phase::Deal, std::nullopt);
  hand.deal(card("5s"), Street::Turn, Phase::Action, 0);
  hand.act(0, {ActionType::Check}, 0, Phase::Action, 1);
  hand.act(1, {ActionType::Bet, 2}, 2, Phase::Action, 2);
  hand.act(2, {ActionType::Fold}, 0, Phase::Action, 0);
  hand.act(0, {ActionType::Call}, 2, Phase::Deal, std::nullopt);
  hand.deal(card("6c"), Street::River, Phase::Action, 0);
  hand.act(0, {ActionType::Check}, 0, Phase::Action, 1);
  hand.act(1, {ActionType::Check}, 0, Phase::Showdown, std::nullopt);
  hand.literal({7, 7, 5, 0, 0, 0}, {}, 19);
  std::array<Holding, 6> holes{};
  holes[0] = {card("As"), card("Ad")};
  holes[1] = {card("Ks"), card("Kd")};
  hand.showdown(holes, {10, 9, 0, 0, 0, 0}, {3, 2, -5, 0, 0, 0},
                {{5, 15, {0, 1, 2}, {0, 1}, {0, 1}}, {7, 4, {0, 1}, {0, 1}, {0, 1}}});
}

}  // namespace

int main() {
  try {
    for (std::size_t button = 0; button < 6; ++button) {
      refundAndRevival(button);
      foldedDeadMoneyAndSingleEligible(button);
      boardTieAndOddChip(button);
    }
  } catch (const std::exception& error) {
    std::fprintf(stderr, "native six-dealt cash ledger failure: %s\n", error.what());
    return 1;
  }
  std::puts("Private native six-dealt cash ledger engineering checks: PASS (18 complete hands)");
  std::puts(
      "This is not GTO quality, provider-equivalence, or production wire acceptance evidence.");
  return 0;
}
