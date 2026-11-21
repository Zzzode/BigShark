// practice_simulator.cpp — offline interactive no-limit hold'em practice table.
//
// You are the ONLY human at a 2..10-seat table; every other seat is an offline
// bot served exclusively by a local bigshark-engine process over the framed
// engine protocol: an equilibrium policy, never a random or heuristic tier. If
// the engine is unreachable the session aborts — there is no fallback opponent.
// The table deals real shuffled cards, posts blinds, plays street by street,
// rotates the button, and tracks your session result in big blinds. Every hand
// restarts every seat at the fixed buy-in; it is a local training tool, never
// a money game, and it makes no network call and reads no credentials.
//
// Usage:
//   bigshark-practice [--seats N] [--stack-bb B] [--hands H] [--seed S]
//                     [--engine-path PATH] [--resident-root PATH=SHA256]
//                     [--flop-library DIR] [--solve-budget-ms N]
//                     [--engine-timeout-ms N] [--lazy]
// At your turn pick an action from the numbered menu (or type f/x/c,
// `b TARGET`, `r TARGET`); `q` ends the session.
#include <array>
#include <bs/behavior_policy.hpp>
#include <bs/eval.hpp>
#include <bs/heads_up.hpp>
#include <bs/practice/engine_process.hpp>
#include <bs/practice/engine_served_policy.hpp>
#include <bs/stage6/adapter.hpp>
#include <bs/stage6/practice_table.hpp>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace {

namespace poker = bs::poker;
using namespace bs::poker;
using namespace bs::stage6;

constexpr poker::Chips kBigBlind = 2;

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
    std::printf("Your cards: %s %s\n", bs::cardName(hole[0]).c_str(),
                bs::cardName(hole[1]).c_str());
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
      cards += bs::cardName(c);
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
        cards += bs::cardName(c);
        cards += ' ';
      }
      std::printf("Showdown  board: %s\n", cards.c_str());
      for (const auto& [seat, hole] : shown)
        std::printf("  seat %zu shows %s %s\n", seat, bs::cardName(hole[0]).c_str(),
                    bs::cardName(hole[1]).c_str());
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
      cards += bs::cardName(c);
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
    std::printf(" %s [%lld..%lld]", preflop ? "raise" : "bet",
                static_cast<long long>(legal.aggressive->minimum),
                static_cast<long long>(legal.aggressive->maximum));
  }
  std::printf("\n");
  for (std::size_t i = 0; i < menu.size(); ++i)
    std::printf("  %zu) %s\n", i + 1, action_text(menu[i]).c_str());
  std::printf("  q) quit\n");

  for (;;) {
    std::printf("> ");
    std::fflush(stdout);
    std::string line;
    if (!std::getline(std::cin, line))
      throw QuitSession{};
    // Trim trailing whitespace.
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r' || line.back() == ' '))
      line.pop_back();
    if (line.empty())
      continue;
    if (line == "q" || line == "quit")
      throw QuitSession{};

    Action a;
    if (line == "f" || line == "fold")
      a = Action{ActionType::Fold, 0};
    else if (line == "x" || line == "check")
      a = Action{ActionType::Check, 0};
    else if (line == "c" || line == "call")
      a = Action{ActionType::Call, 0};
    else if (line.size() > 2 && (line[0] == 'b' || line[0] == 'r') && line[1] == ' ') {
      const char* end = line.c_str() + line.size();
      char* parse_end = nullptr;
      const long long target = std::strtoll(line.c_str() + 2, &parse_end, 10);
      if (parse_end == end && target > 0)
        a = Action{line[0] == 'b' ? ActionType::Bet : ActionType::Raise,
                   static_cast<poker::Chips>(target)};
      else {
        std::printf("usage: b TARGET or r TARGET (positive integer)\n");
        continue;
      }
    } else {
      // Numbered menu selection.
      char* parse_end = nullptr;
      const long long idx = std::strtoll(line.c_str(), &parse_end, 10);
      if (parse_end == line.c_str() + line.size() && idx >= 1 &&
          static_cast<std::size_t>(idx) <= menu.size()) {
        a = menu[static_cast<std::size_t>(idx - 1)];
      } else {
        std::printf("pick a menu number, f/x/c, b TARGET, r TARGET, or q.\n");
        continue;
      }
    }
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
  std::uint64_t seed = 0;
  // The practice table has one opponent tier: a local engine process
  // serving equilibrium policies.
  std::string engine_path = "bin/bigshark-engine";
  std::vector<std::string> resident_roots;  // "<path>=<sha256>"; empty = none
  std::string flop_library;                 // directory path; empty = none
  // RFC 0041 Amendment 1: the offline decision envelope is a 10-second p95.
  std::uint32_t solve_budget_ms = 10000;
  std::uint32_t engine_timeout_ms = 30000;
  bool lazy = false;
};

void usage() {
  std::printf(
      "usage: bigshark-practice [--seats 2..10]\n"
      "                        [--stack-bb N] [--hands N] [--seed S]\n"
      "                        [--engine-path PATH] [--resident-root PATH=SHA256]\n"
      "                        [--flop-library DIR] [--solve-budget-ms N]\n"
      "                        [--engine-timeout-ms N] [--lazy]\n"
      "\n"
      "Opponents are served exclusively by a local bigshark-engine process\n"
      "(equilibrium tier); there is no random or heuristic opponent.\n");
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
      else if (arg == "--engine-path")
        args.engine_path = next();
      else if (arg == "--resident-root")
        args.resident_roots.push_back(next());
      else if (arg == "--flop-library")
        args.flop_library = next();
      else if (arg == "--solve-budget-ms")
        args.solve_budget_ms = static_cast<std::uint32_t>(std::stoul(next()));
      else if (arg == "--engine-timeout-ms")
        args.engine_timeout_ms = static_cast<std::uint32_t>(std::stoul(next()));
      else if (arg == "--lazy")
        args.lazy = true;
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

  PracticeConfig cfg;
  cfg.seats = args.seats;
  cfg.human_seat = 0;
  cfg.big_blind = kBigBlind;
  cfg.starting_stack = static_cast<poker::Chips>(args.stack_bb * kBigBlind);
  cfg.rng_seed = args.seed;

  bool quit = false;
  CliObserver observer;
  PracticeTable table(
      cfg, [&](const GameState& s, std::size_t seat, HoleCards, const PracticeTable& t) -> Action {
        Action a;
        prompt_human(s, seat, t, a);
        return a;
      });

  // Single opponent tier: spawn one EngineProcess shared across all bot
  // seats and inject an EngineServedPolicy for each non-human seat. If the
  // engine cannot be reached the session aborts — a random/heuristic
  // fallback opponent does not exist by design.
  bs::practice::EngineProcessConfig ec;
  ec.engine_path = args.engine_path;
  ec.resident_roots = args.resident_roots;
  ec.flop_library = args.flop_library;
  ec.solve_budget_ms = args.solve_budget_ms;
  ec.timeout_ms = args.engine_timeout_ms;
  ec.lazy = args.lazy;
  bs::practice::EngineProcess engine(std::move(ec));
  if (!engine.start()) {
    std::fprintf(stderr,
                 "failed to start the engine opponent (%s): %s\n"
                 "The practice table has no non-engine opponent tier; start or "
                 "point --engine-path at a reachable bigshark-engine.\n",
                 args.engine_path.c_str(), engine.last_error().c_str());
    return 1;
  }
  std::printf("Engine opponent connected: %s, solve budget %u ms.\n", args.engine_path.c_str(),
              args.solve_budget_ms);
  std::printf("Offline practice table: %zu seats, you are seat 0, %zu bb effective.\n", cfg.seats,
              args.stack_bb);
  std::printf(
      "Every hand restarts at the full buy-in; session result is tracked in bb. "
      "Type q to leave.\n");
  std::vector<bs::practice::EngineServedPolicy*> engine_policies;
  for (std::size_t s = 0; s < cfg.seats; ++s) {
    if (s == cfg.human_seat)
      continue;
    auto policy =
        std::make_unique<bs::practice::EngineServedPolicy>(engine, s, args.solve_budget_ms);
    engine_policies.push_back(policy.get());
    table.set_bot(s, std::move(policy));
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

  if (!engine_policies.empty()) {
    std::size_t total_served = 0;
    std::size_t total_fallbacks = 0;
    for (const auto* p : engine_policies) {
      total_served += p->decisions_served();
      total_fallbacks += p->fallbacks();
    }
    std::printf("\nEngine tier: %zu bot seats, %zu decisions served, %zu fallbacks.\n",
                engine_policies.size(), total_served, total_fallbacks);
  }

  std::printf("\nFinal session result: %+.1f bb over %zu hands. Thanks for practicing.\n",
              table.session_result(table.human_seat()) / kBigBlind, table.hands_played());
  return 0;
}
