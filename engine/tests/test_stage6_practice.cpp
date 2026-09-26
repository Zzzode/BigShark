// RFC 0008 stage 6 — offline single-human practice table gate.
//
// The interactive CLI is a thin view; the table core itself is driven here by
// a scripted HumanAgent. The gate pins: full hands actually complete to
// fold/showdown under every supported seat count, per-hand utility and the
// running session stay exactly zero-sum, the button rotates, blinds follow the
// heads-up vs multiway layout, a fold never reveals hole cards, an illegal
// human action is rejected rather than clamped, bad configuration throws, and
// a fixed seed reproduces the identical hand sequence.
#include <bs/stage6/practice_table.hpp>
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <vector>

namespace {

using namespace bs::poker;
using namespace bs::stage6;

int failures = 0;
void check(bool c, const char* w) {
  if (!c) {
    std::printf("FAIL: %s\n", w);
    ++failures;
  }
}

// Deterministic passive human: check when free, otherwise call, otherwise fold.
Action passive_human(const GameState& state, std::size_t /*seat*/, HoleCards /*hole*/,
                     const PracticeTable& /*table*/) {
  const LegalActions legal = state.legal();
  if (legal.check)
    return Action{ActionType::Check, 0};
  if (legal.call)
    return Action{ActionType::Call, 0};
  return Action{ActionType::Fold, 0};
}

// Human that folds at the first chance every hand.
Action folder(const GameState& state, std::size_t /*seat*/, HoleCards, const PracticeTable&) {
  (void)state;
  return Action{ActionType::Fold, 0};
}

Action illegal_human(const GameState&, std::size_t, HoleCards, const PracticeTable&) {
  return Action{ActionType::Raise, 1'000'000};
}

struct Recorder : PracticeObserver {
  std::size_t starts = 0;
  std::size_t ends = 0;
  std::size_t board_notifications = 0;
  std::size_t last_board_size = 0;
  bool board_progression_valid = true;
  std::size_t folded_ends = 0;
  std::size_t showdown_with_cards = 0;
  std::size_t fold_with_cards = 0;
  std::size_t bad_showdown_sets = 0;
  std::size_t human_decisions = 0;

  void on_hand_start(const GameState& initial, std::size_t, HoleCards) override {
    ++starts;
    (void)initial;
  }
  void on_decision(const GameState&, std::size_t, bool is_human,
                   const std::vector<PolicyAction>& dist, Action) override {
    if (is_human)
      ++human_decisions;
    else
      check(!dist.empty(), "bot decisions carry a distribution for rendering");
  }
  void on_board(const GameState& s) override {
    const std::size_t expected = board_notifications == 0 ? 3 : last_board_size + 1;
    if (s.board().size() != expected)
      board_progression_valid = false;
    last_board_size = s.board().size();
    ++board_notifications;
  }
  void on_hand_end(const GameState& terminal, const std::array<double, 10>&, bool folded,
                   std::size_t,
                   const std::vector<std::pair<std::size_t, HoleCards>>& shown) override {
    ++ends;
    if (folded) {
      ++folded_ends;
      if (!shown.empty())
        ++fold_with_cards;
      return;
    }
    ++showdown_with_cards;
    if (shown.size() != terminal.live_players().size())
      ++bad_showdown_sets;
    for (const auto& [seat, hole] : shown) {
      bool is_live = false;
      for (std::size_t live : terminal.live_players())
        if (live == seat)
          is_live = true;
      if (!is_live || hole[0] == hole[1])
        ++bad_showdown_sets;
    }
  }
  void hand_boundary() {
    board_notifications = 0;
    last_board_size = 0;
  }
};

double sum_n(const std::array<double, 10>& u, std::size_t n) {
  double s = 0.0;
  for (std::size_t i = 0; i < n; ++i)
    s += u[i];
  return s;
}

void run_passive_session(PracticeDifficulty difficulty, std::size_t seats, std::size_t hands,
                         std::uint64_t seed) {
  PracticeConfig cfg;
  cfg.seats = seats;
  cfg.human_seat = 0;
  cfg.big_blind = 2;
  cfg.starting_stack = 200;
  cfg.difficulty = difficulty;
  cfg.rng_seed = seed;
  PracticeTable table(cfg, passive_human);
  Recorder rec;

  for (std::size_t h = 0; h < hands; ++h) {
    const std::size_t expected_button = h % seats;
    check(table.button() == expected_button, "button rotates one seat per hand");
    const std::array<double, 10> u = table.play_hand(&rec);
    check(std::abs(sum_n(u, seats)) < 1e-9, "per-hand chip utility is zero-sum");
    check(rec.board_progression_valid,
          "board observer fires at exactly the completed flop/turn/river");
    rec.hand_boundary();
  }
  check(table.hands_played() == hands, "all requested hands completed");
  check(rec.starts == hands && rec.ends == hands, "observer sees one start/end per hand");
  double session_sum = 0.0;
  for (std::size_t s = 0; s < seats; ++s)
    session_sum += table.session_result(s);
  check(std::abs(session_sum) < 1e-9, "session P/L is zero-sum");
  check(rec.fold_with_cards == 0, "a folded hand never reveals hole cards");
  check(rec.bad_showdown_sets == 0, "showdown reveals exactly the live seats' hole cards");
  check(rec.folded_ends + rec.showdown_with_cards == hands,
        "every hand ends classified as fold or showdown");
}

void test_seat_matrix_completes() {
  for (std::size_t seats : {2u, 3u, 6u, 9u, 10u}) {
    run_passive_session(PracticeDifficulty::Easy, seats, 20, 11 + seats);
    run_passive_session(PracticeDifficulty::Medium, seats, 20, 97 + seats);
  }
}

void test_blind_layouts() {
  // Heads-up: button (seat 0) posts the SB 1, seat 1 posts BB 2.
  {
    PracticeConfig cfg;
    cfg.seats = 2;
    cfg.rng_seed = 5;
    PracticeTable table(cfg, folder);
    struct BlindCheck : PracticeObserver {
      void on_hand_start(const GameState& s, std::size_t, HoleCards) override {
        check(s.pot() == 3, "heads-up root pot is SB+BB");
        check(s.players()[0].contributed == 1, "heads-up button posts the small blind");
        check(s.players()[1].contributed == 2, "heads-up other seat posts the big blind");
      }
    } check;
    table.play_hand(&check);
  }
  // Three-handed: SB is button+1, BB is button+2.
  {
    PracticeConfig cfg;
    cfg.seats = 3;
    cfg.rng_seed = 5;
    PracticeTable table(cfg, folder);
    struct BlindCheck : PracticeObserver {
      void on_hand_start(const GameState& s, std::size_t, HoleCards) override {
        check(s.pot() == 3, "3-seat root pot is SB+BB");
        check(s.players()[0].contributed == 0, "3-seat button posts no blind");
        check(s.players()[1].contributed == 1, "3-seat button+1 is the small blind");
        check(s.players()[2].contributed == 2, "3-seat button+2 is the big blind");
      }
    } check;
    table.play_hand(&check);
  }
}

void test_fold_privacy() {
  // Two seats: the human folds first, so the hand MUST terminate immediately —
  // no board, and the winner takes the pot without revealing a card. This holds
  // for every seed.
  for (std::uint64_t seed = 1; seed <= 50; ++seed) {
    PracticeConfig cfg;
    cfg.seats = 2;
    cfg.rng_seed = seed;
    PracticeTable table(cfg, folder);
    Recorder rec;
    const std::array<double, 10> u = table.play_hand(&rec);
    check(rec.folded_ends == 1, "heads-up fold ends the hand");
    check(rec.board_notifications == 0, "heads-up fold deals no board cards");
    check(rec.fold_with_cards == 0, "a fold reveals no hole cards");
    check(std::abs(sum_n(u, 2)) < 1e-9, "folded hand stays zero-sum");
  }

  // Three seats: the human (UTG, seat 0) folding does NOT end the hand — the
  // bots play on and may reach a board or showdown. The privacy assertion that
  // still holds for every seed is that a FOLD ending reveals no cards; the
  // human's own cards are never shown unless the human reached a showdown.
  for (std::uint64_t seed = 1; seed <= 200; ++seed) {
    PracticeConfig cfg;
    cfg.seats = 3;
    cfg.difficulty = PracticeDifficulty::Easy;
    cfg.rng_seed = seed;
    PracticeTable table(cfg, folder);
    Recorder rec;
    const std::array<double, 10> u = table.play_hand(&rec);
    if (rec.folded_ends)
      check(rec.fold_with_cards == 0, "3-seat fold ending reveals no cards");
    check(std::abs(sum_n(u, 3)) < 1e-9, "3-seat hand stays zero-sum");
  }
}

// At a showdown the human's revealed pair equals the cards dealt to the human
// at hand start; at a fold nothing is shown. Driven with check/call over many
// seeds so both outcomes occur.
void test_revealed_identity() {
  struct IdObserver : PracticeObserver {
    HoleCards dealt{-1, -1};
    HoleCards shown_human{-1, -1};
    bool human_shown = false;
    void on_hand_start(const GameState&, std::size_t, HoleCards hole) override { dealt = hole; }
    void on_hand_end(const GameState&, const std::array<double, 10>&, bool folded, std::size_t,
                     const std::vector<std::pair<std::size_t, HoleCards>>& shown) override {
      if (folded) {
        check(shown.empty(), "a fold ending carries no shown cards");
        return;
      }
      for (const auto& [seat, hole] : shown)
        if (seat == 0) {
          human_shown = true;
          shown_human = hole;
        }
    }
  };

  bool saw_human_showdown = false;
  for (std::uint64_t seed = 1; seed <= 100; ++seed) {
    PracticeConfig cfg;
    cfg.seats = 4;
    cfg.difficulty = PracticeDifficulty::Easy;
    cfg.rng_seed = seed;
    IdObserver obs;
    PracticeTable table(cfg, passive_human);
    (void)table.play_hand(&obs);
    if (obs.human_shown) {
      saw_human_showdown = true;
      check(obs.shown_human[0] == obs.dealt[0] && obs.shown_human[1] == obs.dealt[1],
            "the pair revealed for the human is the pair dealt to the human");
    }
  }
  check(saw_human_showdown, "the check/call sweep reaches human showdowns");

  // The fold-ending half: the human folds first heads-up, which always ends
  // the hand with an empty shown set.
  for (std::uint64_t seed = 1; seed <= 10; ++seed) {
    PracticeConfig cfg;
    cfg.seats = 2;
    cfg.rng_seed = seed;
    IdObserver obs;
    PracticeTable table(cfg, folder);
    (void)table.play_hand(&obs);
    check(!obs.human_shown, "a folded hand never shows the human's cards");
  }
}

void test_illegal_human_action_throws() {
  PracticeConfig cfg;
  cfg.seats = 2;
  cfg.rng_seed = 1;
  PracticeTable table(cfg, illegal_human);
  bool threw = false;
  try {
    (void)table.play_hand(nullptr);
  } catch (const std::runtime_error&) {
    threw = true;
  }
  check(threw, "an illegal human raise is rejected, never clamped or auto-submitted");
}

void test_bad_configuration_throws() {
  auto throws_for = [](PracticeConfig c) {
    try {
      PracticeTable t(c, passive_human);
      (void)t;
    } catch (const std::invalid_argument&) {
      return true;
    }
    return false;
  };
  PracticeConfig one;
  one.seats = 1;
  check(throws_for(one), "one seat is rejected");
  PracticeConfig eleven;
  eleven.seats = 11;
  check(throws_for(eleven), "eleven seats are rejected");
  PracticeConfig bad_human;
  bad_human.seats = 3;
  bad_human.human_seat = 3;
  check(throws_for(bad_human), "an out-of-table human seat is rejected");
  PracticeConfig short_stack;
  short_stack.seats = 2;
  short_stack.starting_stack = 1;
  check(throws_for(short_stack), "a stack below the big blind is rejected");
  PracticeConfig tiny_blind;
  tiny_blind.seats = 2;
  tiny_blind.big_blind = 1;
  check(throws_for(tiny_blind), "a big blind below 2 is rejected at construction");
}

void test_seeded_reproducibility() {
  auto run = [](std::uint64_t seed) {
    PracticeConfig cfg;
    cfg.seats = 3;
    cfg.difficulty = PracticeDifficulty::Medium;
    cfg.rng_seed = seed;
    PracticeTable table(cfg, passive_human);
    std::vector<double> human_results;
    for (std::size_t h = 0; h < 15; ++h)
      human_results.push_back(table.play_hand(nullptr)[0]);
    return human_results;
  };
  const std::vector<double> a = run(1234);
  const std::vector<double> b = run(1234);
  const std::vector<double> c = run(4321);
  check(a == b, "identical seed reproduces the identical hand outcomes");
  check(a != c, "a different seed changes the outcomes");
}

}  // namespace

int main() {
  test_seat_matrix_completes();
  test_blind_layouts();
  test_fold_privacy();
  test_revealed_identity();
  test_illegal_human_action_throws();
  test_bad_configuration_throws();
  test_seeded_reproducibility();
  if (failures) {
    std::printf("STAGE 6 PRACTICE TABLE FAILED: %d\n", failures);
    return 1;
  }
  std::puts("STAGE 6 PRACTICE TABLE PASSED");
  return 0;
}
