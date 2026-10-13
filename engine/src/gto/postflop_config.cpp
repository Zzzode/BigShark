// RFC 0013 Stage 2: shared postflop game configuration (implementation).

#include <bs/postflop_config.hpp>
#include <bs/postflop_frontier.hpp>  // SprBucketConfig
#include <cmath>

namespace bs::gto {

poker::GameDef make_postflop_def(const std::array<int, 3>& canonical_flop, std::size_t spr_bucket) {
  constexpr poker::Chips kPot = 100;
  const double rep_spr = SprBucketConfig::kRepresentativeSpr[spr_bucket];
  const poker::Chips stack = static_cast<poker::Chips>(std::round(kPot * rep_spr));

  poker::GameDef def{};
  def.player_count = 2;
  def.button = 0;  // heads-up: button = SB, acts first postflop
  def.big_blind = 2;
  def.ante = 0;
  def.stacks = {stack, stack, 0, 0, 0, 0, 0, 0, 0, 0};
  def.contributions = {kPot / 2, kPot / 2, 0, 0, 0, 0, 0, 0, 0, 0};
  def.pot = kPot;
  def.board = {canonical_flop[0], canonical_flop[1], canonical_flop[2], 0, 0};
  def.board_size = 3;
  def.preflop = false;
  def.blinds_posted = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
  def.terminal = poker::TerminalDepth::River;
  return def;
}

abstraction::ActionAbstraction make_postflop_abstraction(std::size_t spr_bucket) {
  abstraction::SizeSchedule schedule = abstraction::default_size_schedule();

  if (spr_bucket >= 4) {
    abstraction::StreetSizes sizes;
    sizes.bets = {{1, 1}};
    sizes.raises = {{1, 1}};
    schedule[static_cast<std::size_t>(poker::Street::Flop)] = sizes;
    schedule[static_cast<std::size_t>(poker::Street::Turn)] = sizes;
    schedule[static_cast<std::size_t>(poker::Street::River)] = sizes;
  } else if (spr_bucket >= 2) {
    abstraction::StreetSizes sizes;
    sizes.bets = {{1, 2}, {1, 1}};
    sizes.raises = {{1, 1}};
    schedule[static_cast<std::size_t>(poker::Street::Flop)] = sizes;
    schedule[static_cast<std::size_t>(poker::Street::Turn)] = sizes;
    schedule[static_cast<std::size_t>(poker::Street::River)] = sizes;
  }
  // Buckets 0-1 keep the default schedule.

  return abstraction::ActionAbstraction::declared(std::move(schedule),
                                                  abstraction::CoverSeeds::DeclaredOnly);
}

}  // namespace bs::gto
