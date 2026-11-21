#include "strata.hpp"

#include <array>
#include <bs/abstraction.hpp>
#include <bs/eval.hpp>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>

namespace bs::rfc0046 {

using bs::poker::GameDef;
using bs::poker::TerminalDepth;

const StratumSpec* find_stratum(std::string_view name) {
  for (const StratumSpec& s : kStrata)
    if (name == s.name)
      return &s;
  return nullptr;
}

GameDef make_stratum_def(const StratumSpec& spec, std::uint8_t cap) {
  if (cap > 2)
    throw std::invalid_argument("rfc0046 stratum: cap must be 0, 1, or 2");
  GameDef def{};
  def.player_count = 2;
  def.button = static_cast<std::uint8_t>(spec.button);
  def.big_blind = spec.big_blind;
  def.ante = 0;
  const poker::Chips stack = static_cast<poker::Chips>(spec.stack_behind);
  const poker::Chips contributed = static_cast<poker::Chips>(spec.contribution);
  def.stacks = {stack, stack, 0, 0, 0, 0, 0, 0, 0, 0};
  def.contributions = {contributed, contributed, 0, 0, 0, 0, 0, 0, 0, 0};
  def.pot = spec.pot;
  def.board = {
      bs::cardId(std::string(spec.turn_board[0])), bs::cardId(std::string(spec.turn_board[1])),
      bs::cardId(std::string(spec.turn_board[2])), bs::cardId(std::string(spec.turn_board[3])), 0};
  def.board_size = 4;
  def.preflop = false;
  def.terminal = TerminalDepth::River;
  // cap is applied at tree construction via ActionAbstraction::identity(cap);
  // the GameDef itself is cap-free.
  (void)cap;
  return def;
}

std::vector<bs::solver::WeightedHand> stratum_range(const StratumSpec& spec) {
  std::vector<std::array<int, 2>> combos;
  if (std::string_view(spec.name) == "w1") {
    // Frozen W1 ten-hand anchor (subgame_nash_conv make_representative_range).
    static const std::array<std::pair<const char*, const char*>, 10> kW1{{
        {"Ac", "Ad"},
        {"Kc", "Kd"},
        {"Qc", "Qd"},
        {"Jc", "Jd"},
        {"Tc", "Td"},
        {"9c", "9d"},
        {"Ac", "Kc"},
        {"Ac", "Qc"},
        {"Ac", "Kd"},
        {"9c", "8c"},
    }};
    for (const auto& [a, b] : kW1) {
      int x = bs::cardId(std::string(a));
      int y = bs::cardId(std::string(b));
      if (x > y)
        std::swap(x, y);
      combos.push_back({x, y});
    }
  } else {
    for (int x = 0; x < 52; ++x)
      for (int y = x + 1; y < 52; ++y)
        combos.push_back({x, y});
  }
  // Turn-rooted range blocks only the four public board cards; the pinned
  // river is a chance outcome, not a blocker (a combo containing it is live on
  // the other 43 runouts).
  std::array<int, 4> board{
      bs::cardId(std::string(spec.turn_board[0])), bs::cardId(std::string(spec.turn_board[1])),
      bs::cardId(std::string(spec.turn_board[2])), bs::cardId(std::string(spec.turn_board[3]))};
  std::vector<bs::solver::WeightedHand> out;
  for (const auto& [a, b] : combos) {
    bool blocks = false;
    for (int c : board)
      if (c == a || c == b)
        blocks = true;
    if (!blocks)
      out.push_back(bs::solver::WeightedHand{{a, b}, 1.0});
  }
  return out;
}

}  // namespace bs::rfc0046
