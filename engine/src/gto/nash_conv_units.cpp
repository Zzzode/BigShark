#include <bs/nash_conv_units.hpp>
#include <stdexcept>

namespace bs::solver {

std::size_t live_seat_count(const poker::GameDef& def) {
  std::size_t live = 0;
  for (std::size_t seat = 0; seat < def.player_count; ++seat)
    if (!def.folded[seat])
      ++live;
  return live;
}

NashConvNormalisation normalise_nash_conv(const poker::GameDef& def, double nash_conv) {
  const std::size_t live = live_seat_count(def);
  if (live == 0)
    throw std::invalid_argument("nash conv normalisation requires at least one live seat");
  if (def.big_blind <= 0)
    throw std::invalid_argument("nash conv normalisation requires a positive big blind");

  NashConvNormalisation out;
  out.nash_conv = nash_conv;
  out.live_seats = live;
  out.exploitability = nash_conv / static_cast<double>(live);
  out.exploitability_bb = out.exploitability / static_cast<double>(def.big_blind);
  return out;
}

}  // namespace bs::solver
