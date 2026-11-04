// RFC 0047 Stage-1 frozen stratum table and concrete combo indexing.
//
// Re-implemented from the RFC 0046 frozen specification (the RFC 0047 harness
// does NOT link bigshark_rfc0046_bench). The six strata are the same frozen
// Gate objects. Benchmark-only under the rfc0047 link guard.
#pragma once

#include <array>
#include <bs/game_definition.hpp>
#include <bs/heads_up_solver.hpp>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace bs::rfc0047 {

struct StratumSpec {
  const char* name;
  const char* root_token;  // "limped" | "single-raised"
  std::array<const char*, 4> turn_board;
  const char* river;
  int big_blind = 2;
  int pot = 0;
  int contribution = 0;
  int stack_behind = 0;
  int button = 0;
};

// Order/content identical to the RFC 0046 Stage-0 frozen addendum section 1.
inline constexpr std::array<StratumSpec, 6> kStrata{{
    {"w1", "limped", {"2c", "3d", "7h", "Js"}, "9c", 2, 4, 2, 198, 0},
    {"unpaired-rainbow-ahigh", "limped", {"7s", "Kh", "Ad", "Js"}, "9c", 2, 4, 2, 198, 0},
    {"paired", "limped", {"7s", "Kh", "Kd", "Js"}, "9c", 2, 4, 2, 198, 0},
    {"monotone", "limped", {"Qs", "Ks", "As", "Js"}, "9c", 2, 4, 2, 198, 0},
    {"unpaired-twotone", "limped", {"7s", "Kh", "As", "Js"}, "9c", 2, 4, 2, 198, 0},
    {"raised-srp", "single-raised", {"2c", "3d", "7h", "Js"}, "2s", 2, 8, 4, 196, 0},
}};

const StratumSpec* find_stratum(std::string_view name);

// Build the frozen turn-rooted GameDef (four public cards, terminal=River).
bs::poker::GameDef make_stratum_def(const StratumSpec& spec, std::uint8_t cap);

// Build a river-rooted GameDef for the stratum's frozen five-card board
// (turn_board + pinned river), same pot/contributions. This is the standalone
// exact river subgame at the check-to-river frontier; other frontier ledgers
// (after turn actions) are built per leaf in later S2 steps.
bs::poker::GameDef make_river_root_def(const StratumSpec& spec, std::uint8_t cap);

std::array<int, 5> river_board_ids(const StratumSpec& spec);

std::array<int, 4> turn_board_ids(const StratumSpec& spec);

// Full 1,326-combo uniform range with the four turn-board cards removed
// (1,128 combos); W1 uses its frozen ten-hand anchor instead.
std::vector<bs::solver::WeightedHand> stratum_range(const StratumSpec& spec);

// Dense bijection for two distinct non-board hole cards over a board of 3..5
// public cards. With n = 52-board.size() live cards sorted ascending, pairs
// map 0..C(n,2)-1. Turn (4 board): n=48 -> 1,128. River (5 board): n=47 ->
// 1,081. Throws on a board-colliding or non-distinct holding.
std::uint32_t dense_pair_index(std::span<const int> board, const std::array<int, 2>& hole);
std::array<int, 2> holding_for_dense(std::span<const int> board, std::uint32_t dense);
std::uint32_t live_combo_count(std::span<const int> board);  // C(52-board.size(),2)

}  // namespace bs::rfc0047
