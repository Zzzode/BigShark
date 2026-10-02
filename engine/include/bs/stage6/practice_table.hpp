// stage6/practice_table.hpp — offline single-human practice table core.
//
// A reusable, NON-interactive engine for a practice session: exactly one seat
// is the human (driven through an injected HumanAgent callback), every other
// seat is a deterministic bot BehaviorPolicy. It deals real shuffled cards,
// posts blinds, advances a full hand street by street, settles fold/showdown
// with the exact L1 rules, rotates the button, and tracks each seat's chip P/L
// across hands. The interactive CLI (engine/benchmarks/practice_simulator.cpp)
// is a thin view over this core, so the whole loop is unit-testable without a
// terminal.
//
// Strictly offline: it links bigshark_practice -> bigshark_stage6_eval for the
// bot policies, never bigshark_service / the engine host / a protocol, and
// never reads ~/.river-club config or makes a network call. It is a LOCAL
// training tool, not a money game: stacks reset/rebuy are the caller's choice,
// and the bot policy is labelled for what it is (chart + heuristic), never
// "GTO".
#pragma once

#include <array>
#include <bs/behavior_policy.hpp>
#include <bs/game_definition.hpp>
#include <bs/heads_up.hpp>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <utility>
#include <vector>

namespace bs::stage6 {

// The human's decision callback. It receives the CURRENT action state, the
// acting (human) seat, that seat's two hole cards, and the public action log;
// it must return one concrete LEGAL action. An interactive frontend renders the
// state and blocks for input here; a test returns a scripted action. Unlike a
// BehaviorPolicy this is stateful/IO-shaped by design (it is the one seat that
// is not a fixed policy).
using HumanAgent = std::function<poker::Action(const poker::GameState& state, std::size_t seat,
                                               HoleCards hole, const class PracticeTable& table)>;

// One observer for side effects a frontend wants (render the state, announce a
// dealt card, the settlement, etc.). The core calls these as the hand
// progresses; tests pass a no-op. All are optional/no-op by default.
struct PracticeObserver {
  virtual ~PracticeObserver() = default;
  // Called at the start of every hand after cards are dealt, before any
  // action. `human_hole` is the HUMAN seat's pair only.
  virtual void on_hand_start(const poker::GameState& /*initial*/, std::size_t /*hand_id*/,
                             HoleCards /*human_hole*/) {}
  // Called before each action is applied (human or bot), with the current
  // state, the distribution the bot sampled (empty for the human's own
  // decision), and the concrete action about to be taken.
  virtual void on_decision(const poker::GameState& /*state*/, std::size_t /*seat*/,
                           bool /*is_human*/, const std::vector<PolicyAction>& /*dist*/,
                           poker::Action /*action*/) {}
  // Called once per completed public street: after the three-card flop, the
  // turn, and the river (board size 3, 4, then 5). The engine's per-card Deal
  // phases are coalesced here.
  virtual void on_board(const poker::GameState& /*state*/) {}
  // Called once at the terminal with per-seat signed chip utility for the
  // hand. `shown_holes` lists ONLY the seats whose cards are public at a
  // showdown (the live players); it is empty when the hand ends by a fold.
  virtual void on_hand_end(const poker::GameState& /*terminal*/,
                           const std::array<double, 10>& /*u*/, bool /*folded*/,
                           std::size_t /*hand_id*/,
                           const std::vector<std::pair<std::size_t, HoleCards>>& /*shown_holes*/) {}
};

// Difficulty selects the bot roster. The labels are deliberately descriptive;
// none of these is a solved equilibrium ("GTO") policy.
enum class PracticeDifficulty {
  Easy,    // declared-menu uniform random bot: loose, passive/aggressive random
  Medium,  // pinned preflop charts + postflop Monte-Carlo heuristic (ABC poker)
  Engine,  // engine-served tier: a local bigshark-engine process decides through
           // the v1 framed contract; a descriptive label, never "GTO". The
           // PracticeTable leaves Engine bot seats null; the caller must inject
           // a policy via set_bot() before the first hand.
};

struct PracticeConfig {
  std::size_t seats = 2;       // 2..10, including the human
  std::size_t human_seat = 0;  // fixed logical seat; button rotates under it
  poker::Chips big_blind = 2;
  poker::Chips starting_stack = 200;  // chips behind at the start of every hand
  PracticeDifficulty difficulty = PracticeDifficulty::Medium;
  std::uint64_t rng_seed = 0;  // 0 -> seeded from a fixed default
};

class PracticeTable {
 public:
  PracticeTable(PracticeConfig config, HumanAgent human);
  ~PracticeTable();

  // Plays one hand from a fresh shuffle (blinds posted by the current button
  // placement), returns that hand's per-seat signed chip utility and updates
  // session P/L and advances the button. Throws std::invalid_argument /
  // std::runtime_error on an illegal configuration or a human/bot action that
  // is not legal (the latter is an engine/programming error, never clamped).
  std::array<double, 10> play_hand(PracticeObserver* observer = nullptr);

  // Session accounting.
  std::size_t hands_played() const noexcept;
  std::size_t seats() const noexcept;
  std::size_t human_seat() const noexcept;
  poker::Chips big_blind() const noexcept;
  // Current button seat for the NEXT / in-progress hand (rotated each hand).
  std::size_t button() const noexcept;
  // Cumulative signed chip result per seat across all completed hands.
  double session_result(std::size_t seat) const;
  const PracticeConfig& config() const noexcept;

  // The board prefix revealed SO FAR in the current hand (0..5 entries). This
  // is the only chance information a frontend gets: other seats' hole cards
  // are never exposed before the showdown, and unrevealed board cards stay
  // hidden. Between hands it retains the previous hand's completed board.
  std::vector<int> revealed_board() const;

  // Replaces the bot at `seat` with a caller-owned policy. Used by the
  // Engine-served tier to inject an EngineServedPolicy without
  // bigshark_practice knowing the client type. Throws std::invalid_argument
  // on an out-of-range seat, the human seat, or a null bot.
  void set_bot(std::size_t seat, std::unique_ptr<BehaviorPolicy> bot);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// Builds one bot BehaviorPolicy for the requested difficulty. The returned
// object is owned by the caller. Easy = uniform over the declared menu;
// Medium = the pinned chart + heuristic baseline.
std::unique_ptr<BehaviorPolicy> make_practice_bot(PracticeDifficulty difficulty);

}  // namespace bs::stage6
