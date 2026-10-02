// practice_simulator.cpp — offline interactive no-limit hold'em practice table.
//
// You are the ONLY human at a 2..10-seat table; every other seat is an offline
// bot (Easy = uniform random over the declared menu, Medium = the pinned chart
// + Monte-Carlo-equity heuristic, i.e. ABC poker — NOT a solved "GTO" policy).
// The table deals real shuffled cards, posts blinds, plays street by street,
// rotates the button, and tracks your session result in big blinds. Every hand
// restarts every seat at the fixed buy-in; it is a local training tool, never
// a money game, and it makes no network call and reads no credentials.
//
// Usage:
//   bigshark-practice [--seats N] [--difficulty easy|medium]
//                    [--stack-bb B] [--hands H] [--seed S]
// At your turn pick an action from the numbered menu (or type f/x/c,
// `b TARGET`, `r TARGET`); `q` ends the session.
#include <array>
#include <bs/behavior_policy.hpp>
#include <bs/engine_client/engine_client.hpp>
#include <bs/heads_up.hpp>
#include <bs/stage6/adapter.hpp>
#include <bs/stage6/practice_table.hpp>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace {

namespace poker = bs::poker;
using namespace bs::poker;
using namespace bs::stage6;

constexpr poker::Chips kBigBlind = 2;

const char* difficulty_name(PracticeDifficulty d) {
  switch (d) {
    case PracticeDifficulty::Easy:
      return "easy (uniform random over the action menu)";
    case PracticeDifficulty::Medium:
      return "medium (pinned charts + equity heuristic, ABC poker; not GTO)";
    case PracticeDifficulty::Engine:
      return "engine (served by a local bigshark-engine process; a descriptive label, not GTO)";
  }
  return "unknown";
}

// Button-relative seat label: heads-up has button/SB vs BB; three seats and up
// have SB, BB, UTG(+k), CO clockwise of the button.
std::string position_label(std::size_t seats, std::size_t button, std::size_t seat) {
  if (seats == 2)
    return seat == button ? "BTN/SB" : "BB";
  const std::size_t d = (seat + seats - button) % seats;  // clockwise distance
  if (d == 0)
    return "BTN";
  if (d == 1)
    return "SB";
  if (d == 2)
    return "BB";
  if (d == seats - 1)
    return "CO";
  return "UTG" + (d == 3 ? std::string{} : "+" + std::to_string(d - 3));
}

const char* street_name(Street s) {
  switch (s) {
    case Street::Preflop:
      return "PREFLOP";
    case Street::Flop:
      return "FLOP";
    case Street::Turn:
      return "TURN";
    case Street::River:
      return "RIVER";
  }
  return "?";
}

std::string action_text(const Action& a) {
  switch (a.type) {
    case ActionType::Fold:
      return "folds";
    case ActionType::Check:
      return "checks";
    case ActionType::Call:
      return "calls";
    case ActionType::Bet:
      return "bets " + std::to_string(a.target_total);
    case ActionType::Raise:
      return "raises to " + std::to_string(a.target_total);
  }
  return "?";
}

struct CliObserver : PracticeObserver {
  std::size_t seats = 0;
  std::size_t human = 0;
  std::size_t button = 0;
  std::size_t hand_id = 0;

  void on_hand_start(const GameState& s, std::size_t id, HoleCards hole) override {
    hand_id = id;
    seats = s.player_count();
    button = s.def().button;
    std::printf("\n=== Hand #%zu  [%s] ===\n", id + 1,
                position_label(seats, button, human).c_str());
    std::printf("Your cards: %s %s\n", card_name(hole[0]).c_str(), card_name(hole[1]).c_str());
    std::printf("Pot %lld | Blinds %lld/%lld\n", static_cast<long long>(s.pot()),
                static_cast<long long>(kBigBlind / 2), static_cast<long long>(kBigBlind));
  }

  void on_decision(const GameState& s, std::size_t seat, bool is_human,
                   const std::vector<PolicyAction>&, Action action) override {
    if (is_human)
      return;
    // Match the server's preflop vocabulary: the big blind's option after a
    // limp is announced as a raise even though the engine types it Bet.
    if (s.street() == Street::Preflop && action.type == ActionType::Bet)
      action = Action{ActionType::Raise, action.target_total};
    std::printf("  seat %zu (%s) %s\n", seat, position_label(seats, button, seat).c_str(),
                action_text(action).c_str());
  }

  void on_board(const GameState& s) override {
    // The core coalesces the per-card flop deal phases: this fires once per
    // completed flop (3), turn (4), and river (5).
    std::string cards;
    for (int c : s.board()) {
      cards += card_name(c);
      cards += ' ';
    }
    std::printf("--- %s: %s (pot %lld) ---\n", street_name(s.street()), cards.c_str(),
                static_cast<long long>(s.pot()));
  }

  void on_hand_end(const GameState& s, const std::array<double, 10>& u, bool folded, std::size_t,
                   const std::vector<std::pair<std::size_t, HoleCards>>& shown) override {
    if (folded) {
      std::printf("Hand ends by a fold (pot %lld).\n", static_cast<long long>(s.pot()));
    } else {
      std::string cards;
      for (int c : s.board()) {
        cards += card_name(c);
        cards += ' ';
      }
      std::printf("Showdown  board: %s\n", cards.c_str());
      for (const auto& [seat, hole] : shown)
        std::printf("  seat %zu shows %s %s\n", seat, card_name(hole[0]).c_str(),
                    card_name(hole[1]).c_str());
    }
    const double hu = u[human];
    std::printf("You %s %lld chips this hand (%+.2f bb).\n", hu >= 0 ? "win" : "lose",
                static_cast<long long>(std::llround(std::abs(hu))),
                hu / static_cast<double>(kBigBlind));
  }
};

// Renders the table and parses one human action. Throws QuitSession when the
// human asks to leave.
struct QuitSession {};

void prompt_human(const GameState& state, std::size_t seat, const PracticeTable& table,
                  Action& out) {
  const std::size_t n = table.seats();
  std::printf("\n-- %s -- pot %lld --\n", street_name(state.street()),
              static_cast<long long>(state.pot()));
  for (std::size_t i = 0; i < n; ++i) {
    const auto& p = state.players()[i];
    const char* mark = i == table.button() ? "BTN" : "   ";
    const char* who = i == seat ? "YOU" : "bot";
    std::printf("  [%s] seat %zu %-3s %-7s stack %-5lld in %-5lld%s%s\n", mark, i, who,
                position_label(n, table.button(), i).c_str(), static_cast<long long>(p.stack),
                static_cast<long long>(p.street_committed), p.folded ? " FOLDED" : "",
                p.all_in ? " ALL-IN" : "");
  }
  if (state.board().size() > 0) {
    std::string cards;
    for (int c : state.board()) {
      cards += card_name(c);
      cards += ' ';
    }
    std::printf("Board: %s\n", cards.c_str());
  }

  const LegalActions legal = state.legal();
  const std::vector<Action> menu = declared_behavior_menu(state, seat);
  std::printf("Legal:");
  if (legal.fold)
    std::printf(" fold");
  if (legal.check)
    std::printf(" check");
  if (legal.call)
    std::printf(" call(%lld)", static_cast<long long>(legal.call_amount));
  if (legal.aggressive) {
    const bool preflop = state.street() == Street::Preflop;
    std::printf(" %s [%lld..%lld]",
                (!preflop && legal.aggressive->type == ActionType::Bet) ? "bet" : "raise",
                static_cast<long long>(legal.aggressive->minimum),
                static_cast<long long>(legal.aggressive->maximum));
  }
  std::printf("\n");
  for (std::size_t i = 0; i < menu.size(); ++i)
    std::printf("  %zu) %s\n", i + 1, action_text(menu[i]).c_str());

  for (;;) {
    std::printf("> ");
    std::fflush(stdout);
    std::string line;
    if (!std::getline(std::cin, line))
      throw QuitSession{};
    std::istringstream ss(line);
    std::string tok;
    if (!(ss >> tok))
      continue;
    if (tok == "q" || tok == "quit" || tok == "exit")
      throw QuitSession{};
    Action a{ActionType::Fold, 0};
    bool parsed = true;
    if (tok == "f" || tok == "fold")
      a = Action{ActionType::Fold, 0};
    else if (tok == "x" || tok == "check")
      a = Action{ActionType::Check, 0};
    else if (tok == "c" || tok == "call")
      a = Action{ActionType::Call, 0};
    else if (tok == "b" || tok == "bet" || tok == "r" || tok == "raise") {
      long long target = 0;
      if (!(ss >> target) || target <= 0) {
        std::printf("usage: %s TARGET_TOTAL\n", tok.c_str());
        continue;
      }
      // Use the exact aggressive verb the state requires (the preflop big
      // blind option is server-labelled "raise" but typed Bet in the engine).
      if (!legal.aggressive) {
        std::printf("no wager is legal here; pick again.\n");
        continue;
      }
      a = Action{legal.aggressive->type, static_cast<poker::Chips>(target)};
    } else {
      char* end = nullptr;
      const long idx = std::strtol(tok.c_str(), &end, 10);
      if (end == tok.c_str() || idx < 1 || static_cast<std::size_t>(idx) > menu.size()) {
        std::printf("unrecognized input (use a menu number, f/x/c, b/r TOTAL, or q)\n");
        continue;
      }
      a = menu[static_cast<std::size_t>(idx - 1)];
    }
    (void)parsed;
    if (!legal.contains(a)) {
      std::printf("that action is not legal here; pick again.\n");
      continue;
    }
    out = a;
    return;
  }
}

struct Args {
  std::size_t seats = 6;
  std::size_t stack_bb = 100;
  std::size_t hands = 0;  // 0 = until quit
  PracticeDifficulty difficulty = PracticeDifficulty::Medium;
  std::uint64_t seed = 0;
  // Engine tier (--difficulty engine).
  std::string engine_path = "bin/bigshark-engine";
  std::string resident_root;  // "<path>=<sha256>"; empty = no resident roots
  std::uint32_t solve_budget_ms = 1000;
  std::uint32_t engine_timeout_ms = 30000;
};

void usage() {
  std::printf(
      "usage: bigshark-practice [--seats 2..10] [--difficulty easy|medium|engine]\n"
      "                        [--stack-bb N] [--hands N] [--seed S]\n"
      "                        [--engine-path PATH] [--resident-root PATH=SHA256]\n"
      "                        [--solve-budget-ms N] [--engine-timeout-ms N]\n");
}

}  // namespace

int main(int argc, char** argv) {
  Args args;
  try {
    for (int i = 1; i < argc; ++i) {
      const std::string arg = argv[i];
      auto next = [&]() -> std::string {
        if (i + 1 >= argc)
          throw std::runtime_error("missing value for " + arg);
        return argv[++i];
      };
      if (arg == "--seats")
        args.seats = std::stoul(next());
      else if (arg == "--stack-bb")
        args.stack_bb = std::stoul(next());
      else if (arg == "--hands")
        args.hands = std::stoul(next());
      else if (arg == "--seed")
        args.seed = std::stoull(next());
      else if (arg == "--difficulty") {
        const std::string d = next();
        if (d == "easy")
          args.difficulty = PracticeDifficulty::Easy;
        else if (d == "medium")
          args.difficulty = PracticeDifficulty::Medium;
        else if (d == "engine")
          args.difficulty = PracticeDifficulty::Engine;
        else
          throw std::runtime_error("unknown --difficulty '" + d + "' (want easy|medium|engine)");
      } else if (arg == "--engine-path")
        args.engine_path = next();
      else if (arg == "--resident-root")
        args.resident_root = next();
      else if (arg == "--solve-budget-ms")
        args.solve_budget_ms = static_cast<std::uint32_t>(std::stoul(next()));
      else if (arg == "--engine-timeout-ms")
        args.engine_timeout_ms = static_cast<std::uint32_t>(std::stoul(next()));
      else if (arg == "--help" || arg == "-h") {
        usage();
        return 0;
      } else
        throw std::runtime_error("unknown argument: " + arg);
    }
  } catch (const std::exception& e) {
    std::fprintf(stderr, "%s\n", e.what());
    usage();
    return 2;
  }
  if (args.seats < 2 || args.seats > 10 || args.stack_bb == 0) {
    std::fprintf(stderr, "seats must be 2..10 and --stack-bb positive\n");
    return 2;
  }
  if (args.difficulty == PracticeDifficulty::Engine && args.seats != 2) {
    std::fprintf(stderr, "the engine tier is heads-up only in this stage; use --seats 2\n");
    return 2;
  }

  PracticeConfig cfg;
  cfg.seats = args.seats;
  cfg.human_seat = 0;
  cfg.big_blind = kBigBlind;
  cfg.starting_stack = static_cast<poker::Chips>(args.stack_bb * kBigBlind);
  cfg.difficulty = args.difficulty;
  cfg.rng_seed = args.seed;

  std::printf("Offline practice table: %zu seats, you are seat 0, %zu bb effective, bots: %s.\n",
              cfg.seats, args.stack_bb, difficulty_name(cfg.difficulty));
  std::printf(
      "Every hand restarts at the full buy-in; session result is tracked in bb. "
      "Type q to leave.\n");

  bool quit = false;
  CliObserver observer;
  PracticeTable table(
      cfg, [&](const GameState& s, std::size_t seat, HoleCards, const PracticeTable& t) -> Action {
        Action a;
        prompt_human(s, seat, t, a);
        return a;
      });

  // Engine tier: inject an EngineServedPolicy into the bot seat. The table
  // takes ownership; the raw pointer is valid until the table is destroyed.
  bs::engine_client::EngineServedPolicy* engine_policy = nullptr;
  if (args.difficulty == PracticeDifficulty::Engine) {
    bs::engine_client::EngineClientConfig ec;
    ec.engine_path = args.engine_path;
    ec.resident_root = args.resident_root;
    ec.solve_budget_ms = args.solve_budget_ms;
    ec.timeout_ms = args.engine_timeout_ms;
    auto policy = std::make_unique<bs::engine_client::EngineServedPolicy>(ec);
    engine_policy = policy.get();
    table.set_bot(1, std::move(policy));
    if (engine_policy->start()) {
      std::printf(
          "Engine tier: protocol minor %u, build %s, resident pipeline %s, "
          "solve budget %u ms.\n",
          engine_policy->negotiated_minor(), engine_policy->engine_version().c_str(),
          engine_policy->resident_pipeline_advertised() ? "available" : "not advertised",
          args.solve_budget_ms);
    } else {
      std::printf(
          "Engine tier: WARNING engine unreachable (%s); every bot decision "
          "will fall back to check/call/fold.\n",
          args.engine_path.c_str());
    }
  }

  for (std::size_t h = 0; args.hands == 0 || h < args.hands; ++h) {
    try {
      (void)table.play_hand(&observer);
    } catch (const QuitSession&) {
      quit = true;
      break;
    } catch (const std::exception& e) {
      std::fprintf(stderr, "\npractice session aborted: %s\n", e.what());
      return 1;
    }
    const double result_bb = table.session_result(table.human_seat()) / kBigBlind;
    std::printf("Session after %zu hand(s): %+.1f bb\n", table.hands_played(), result_bb);
    if (quit)
      break;
  }

  if (engine_policy) {
    const bs::engine_client::EngineServedStats s = engine_policy->stats();
    const double avg = s.served > 0 ? static_cast<double>(s.total_latency_us) / s.served : 0.0;
    std::printf(
        "\nEngine tier: %llu decisions, %llu served, %llu fallbacks "
        "(%llu timeouts, %llu engine errors, %llu protocol errors, %llu restarts); "
        "latency avg/min/max = %.0f/%llu/%llu us over %llu served decisions.\n",
        static_cast<unsigned long long>(s.decisions), static_cast<unsigned long long>(s.served),
        static_cast<unsigned long long>(s.fallbacks), static_cast<unsigned long long>(s.timeouts),
        static_cast<unsigned long long>(s.engine_errors),
        static_cast<unsigned long long>(s.protocol_errors),
        static_cast<unsigned long long>(s.restarts), avg,
        static_cast<unsigned long long>(s.min_latency_us),
        static_cast<unsigned long long>(s.max_latency_us),
        static_cast<unsigned long long>(s.served));
  }

  std::printf("\nFinal session result: %+.1f bb over %zu hands. Thanks for practicing.\n",
              table.session_result(table.human_seat()) / kBigBlind, table.hands_played());
  return 0;
}
