// RFC 0009 W2c-ii -- the unified solver game view.
//
// ONE seat-generic game description shared by the resident (blueprint storage
// and belief) and the resolver (terminal-only gadget). It replaces the
// two-seat-only `solver::HeadsUpGame` in the serving path so a 3..10-seat
// artifact can load and serve through the same engine. The root identity is a
// `poker::GameDef`; the per-seat ranges, sizing schedule, and the v1-only
// fixed runout ride alongside it. `fixed_runout` is preserved field-for-field
// so certified two-seat v1 fixed-runout artifacts do not regress.
//
// It lives in the SOLVER domain so `bs::resolver` depends on it without a
// layering inversion; `bs::resident` implements `resolver::BlueprintSource`
// over it.
#pragma once

#include <array>
#include <bs/abstraction.hpp>
#include <bs/game_definition.hpp>
#include <bs/heads_up_solver.hpp>
#include <cstddef>
#include <optional>
#include <vector>

namespace bs::solver {

struct UnifiedGame {
  // Seat-generic root identity (board, stacks, contributions, pot, blinds,
  // button). For a flop-rooted serving game `preflop` is false, every
  // `blinds_posted` is zero, and `pot` is the sum of the matched contributions.
  poker::GameDef def{};
  // Per-seat declared weighted ranges, seat-indexed. Only seats
  // `0 .. def.player_count - 1` are populated.
  std::array<std::vector<WeightedHand>, poker::kMaxUnifiedSeats> ranges{};
  abstraction::SizeSchedule sizes = abstraction::default_size_schedule();
  // A fixed runout defines a different, conditional validation game. All fixed
  // cards are reserved before dealing private hands or any earlier public card.
  // v1 artifacts may carry one; v2 artifacts leave it empty.
  std::array<std::optional<int>, 2> fixed_runout{};
};

// Declared accounting constant for one unified game copy, used instead of
// `sizeof(UnifiedGame)` by every byte-charge site (the resident footprint
// estimate and the artifact manifest's `accounted_bytes`). RFC 0007 requires
// this decoupling: deriving the charge from struct layout would let an
// unrelated field addition move the frozen accounting silently. The value is a
// documented budget, not a measurement: it stays at least the real struct size
// and changes only by a deliberate edit with rebase evidence.
inline constexpr std::size_t kUnifiedGameCopyAccountingBytes = 1024;
static_assert(kUnifiedGameCopyAccountingBytes >= sizeof(UnifiedGame),
              "the declared unified game-copy charge must stay at least the real struct size");

// Converts a two-seat `HeadsUpGame` (the v1 artifact form) to the unified view.
// The root maps field-for-field onto a flop-rooted two-seat `GameDef`
// (`preflop` and `blinds_posted` carried across, zero for a flop-rooted game),
// and the ranges, sizing schedule, and fixed runout are preserved.
UnifiedGame to_unified_game(const HeadsUpGame& game);

}  // namespace bs::solver
