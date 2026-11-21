// RFC 0046 Stage-1 frozen stratum table (addendum section 1) and single-cell
// runner. Benchmark-only. The six strata are the ONLY legal cells; a cell is
// a two-seat 100 BB turn-rooted cap-1 (or cap-0 reference) object with the
// frozen root, four-card turn board, and pinned river card used for the
// walk pin and the behavioral hand pair. Chance-free NashConv enumerates the
// 48-card river chance per deal (44 board/hole-compatible outcomes), matching
// the RFC 0045 fixed-universe convention.
#pragma once

#include <array>
#include <bs/eval.hpp>
#include <bs/game_definition.hpp>
#include <bs/heads_up_solver.hpp>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace bs::rfc0046 {

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

// Order MUST match addendum section 1; W1 is one of the six.
inline constexpr std::array<StratumSpec, 6> kStrata{{
    {"w1", "limped", {"2c", "3d", "7h", "Js"}, "9c", 2, 4, 2, 198, 0},
    {"unpaired-rainbow-ahigh", "limped", {"7s", "Kh", "Ad", "Js"}, "9c", 2, 4, 2, 198, 0},
    {"paired", "limped", {"7s", "Kh", "Kd", "Js"}, "9c", 2, 4, 2, 198, 0},
    {"monotone", "limped", {"Qs", "Ks", "As", "Js"}, "9c", 2, 4, 2, 198, 0},
    {"unpaired-twotone", "limped", {"7s", "Kh", "As", "Js"}, "9c", 2, 4, 2, 198, 0},
    {"raised-srp", "single-raised", {"2c", "3d", "7h", "Js"}, "2s", 2, 8, 4, 196, 0},
}};

const StratumSpec* find_stratum(std::string_view name);

// Builds the frozen turn-rooted GameDef for a stratum at the requested action
// cap (0 = unrestricted offline reference, 1 = production menu, 2 = the
// single pre-authorized Gate-3 escalation).
bs::poker::GameDef make_stratum_def(const StratumSpec& spec, std::uint8_t cap);

// The frozen W1 ten-hand anchor range (AA KK QQ JJ TT 99 AKs AQs AKo 98s),
// board blockers removed. Every other stratum uses the full 1,326-combo
// uniform range (board blockers removed).
std::vector<bs::solver::WeightedHand> stratum_range(const StratumSpec& spec);

}  // namespace bs::rfc0046
