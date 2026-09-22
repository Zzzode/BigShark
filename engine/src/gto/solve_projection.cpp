#include <bs/detail/solve_projection.hpp>

namespace bs::solver::detail {

poker::HeadsUpRoot project_heads_up_root(const poker::GameDef& def) {
  poker::HeadsUpRoot root{};
  root.stacks = {def.stacks[0], def.stacks[1]};
  root.contributions = {def.contributions[0], def.contributions[1]};
  root.pot = def.pot;
  root.big_blind = def.big_blind;
  root.button = def.button;
  root.preflop = def.preflop;
  if (def.preflop) {
    root.flop = {-1, -1, -1};
    root.blinds_posted = {def.blinds_posted[0], def.blinds_posted[1]};
  } else {
    root.flop = {def.board[0], def.board[1], def.board[2]};
    root.blinds_posted = {0, 0};
  }
  return root;
}

}  // namespace bs::solver::detail
