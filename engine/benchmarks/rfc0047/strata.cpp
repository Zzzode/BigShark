#include "strata.hpp"

#include <bs/abstraction.hpp>
#include <bs/eval.hpp>
#include <bs/heads_up_solver.hpp>
#include <stdexcept>
#include <utility>

namespace bs::rfc0047 {

using bs::poker::GameDef;
using bs::poker::TerminalDepth;

const StratumSpec* find_stratum(std::string_view name) {
  for (const StratumSpec& s : kStrata)
    if (name == s.name)
      return &s;
  return nullptr;
}

std::array<int, 4> turn_board_ids(const StratumSpec& spec) {
  return {bs::cardId(std::string(spec.turn_board[0])), bs::cardId(std::string(spec.turn_board[1])),
          bs::cardId(std::string(spec.turn_board[2])), bs::cardId(std::string(spec.turn_board[3]))};
}

GameDef make_stratum_def(const StratumSpec& spec, std::uint8_t cap) {
  if (cap > 2)
    throw std::invalid_argument("rfc0047 stratum: cap must be 0, 1, or 2");
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
  const std::array<int, 4> b = turn_board_ids(spec);
  def.board = {b[0], b[1], b[2], b[3], 0};
  def.board_size = 4;
  def.preflop = false;
  def.terminal = TerminalDepth::River;
  (void)cap;  // applied at tree construction via ActionAbstraction::identity(cap)
  return def;
}

std::array<int, 5> river_board_ids(const StratumSpec& spec) {
  const std::array<int, 4> t = turn_board_ids(spec);
  return {t[0], t[1], t[2], t[3], bs::cardId(std::string(spec.river))};
}

GameDef make_river_root_def(const StratumSpec& spec, std::uint8_t cap) {
  if (cap > 2)
    throw std::invalid_argument("rfc0047 river stratum: cap must be 0, 1, or 2");
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
  const std::array<int, 5> b = river_board_ids(spec);
  def.board = {b[0], b[1], b[2], b[3], b[4]};
  def.board_size = 5;
  def.preflop = false;
  def.terminal = TerminalDepth::River;
  (void)cap;
  return def;
}

std::vector<bs::solver::WeightedHand> stratum_range(const StratumSpec& spec) {
  std::vector<std::array<int, 2>> combos;
  if (std::string_view(spec.name) == "w1") {
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
    for (const auto& [a, c] : kW1) {
      int x = bs::cardId(std::string(a));
      int y = bs::cardId(std::string(c));
      if (x > y)
        std::swap(x, y);
      combos.push_back({x, y});
    }
  } else {
    for (int x = 0; x < 52; ++x)
      for (int y = x + 1; y < 52; ++y)
        combos.push_back({x, y});
  }
  const std::array<int, 4> board = turn_board_ids(spec);
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

namespace {

// Sorted ascending list of cards not on the board.
std::vector<int> live_cards(std::span<const int> board) {
  std::array<bool, 52> used{};
  for (int c : board) {
    if (c < 0 || c > 51)
      throw std::invalid_argument("rfc0047: board card out of range");
    if (used[c])
      throw std::invalid_argument("rfc0047: duplicate board card");
    used[c] = true;
  }
  std::vector<int> live;
  for (int c = 0; c < 52; ++c)
    if (!used[c])
      live.push_back(c);
  return live;
}

}  // namespace

std::uint32_t live_combo_count(std::span<const int> board) {
  const std::uint32_t n = static_cast<std::uint32_t>(52 - board.size());
  return n * (n - 1u) / 2u;
}

std::uint32_t dense_pair_index(std::span<const int> board, const std::array<int, 2>& hole) {
  std::array<int, 2> h{hole[0], hole[1]};
  if (h[0] == h[1])
    throw std::invalid_argument("rfc0047: holding must be two distinct cards");
  if (h[0] > h[1])
    std::swap(h[0], h[1]);
  const std::vector<int> live = live_cards(board);
  std::int32_t i = -1;
  std::int32_t j = -1;
  for (std::size_t k = 0; k < live.size(); ++k) {
    if (live[k] == h[0])
      i = static_cast<std::int32_t>(k);
    if (live[k] == h[1])
      j = static_cast<std::int32_t>(k);
  }
  if (i < 0 || j < 0)
    throw std::invalid_argument("rfc0047: holding shares a card with the board");
  const auto n = static_cast<std::uint32_t>(live.size());
  const auto ui = static_cast<std::uint32_t>(i);
  const auto uj = static_cast<std::uint32_t>(j);
  const std::uint32_t offset = ui * n - ui * (ui + 1u) / 2u;
  return offset + (uj - ui - 1u);
}

std::array<int, 2> holding_for_dense(std::span<const int> board, std::uint32_t dense) {
  const std::vector<int> live = live_cards(board);
  const auto n = static_cast<std::uint32_t>(live.size());
  if (dense >= n * (n - 1u) / 2u)
    throw std::invalid_argument("rfc0047: dense id out of range");
  std::uint32_t lo = 0;
  std::uint32_t hi = n - 1u;
  while (lo < hi) {
    const std::uint32_t mid = lo + (hi - lo + 1u) / 2u;
    const std::uint32_t off = mid * n - mid * (mid + 1u) / 2u;
    if (off <= dense)
      lo = mid;
    else
      hi = mid - 1u;
  }
  const std::uint32_t i = lo;
  const std::uint32_t off = i * n - i * (i + 1u) / 2u;
  const std::uint32_t j = i + 1u + (dense - off);
  return {live[i], live[j]};
}

}  // namespace bs::rfc0047
