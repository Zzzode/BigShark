// Contribution settlement for the declared offline RFC 0006 chip profile.
#pragma once

#include <bs/heads_up.hpp>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace bs::poker {

// The contribution ledger's own seat bound. The unified game definition
// (RFC 0008 §L1) carries the same value as `kMaxUnifiedSeats`; the constant
// lives here so the L0 layer does not depend upward, and the two are tied by a
// static assertion in game_definition.hpp.
inline constexpr std::size_t kMaxContributionSeats = 10;

enum class OddChipRule { Unspecified, ClockwiseLeftOfButton };
enum class RakeRule { Unspecified, PotPercentageFloor };

struct RakePolicy {
  RakeRule rule = RakeRule::Unspecified;
  std::uint32_t basis_points = 0;
  Chips cap = 0;  // Zero permits no rake, not unlimited rake.
  bool no_flop_no_drop = false;
};

struct SettlementPlayer {
  std::size_t seat = 0;
  // Gross contributed G and cumulative uncalled refunds F; stack includes F.
  // The caller must identify and credit refunds before invoking settlement.
  PlayerChips chips;
  // Higher scores win. Required only for members of contested eligible sets.
  // Card evaluation and validation remain the caller's responsibility.
  std::optional<std::uint32_t> showdown_score;
};

struct SettlementInput {
  std::vector<SettlementPlayer> players;
  // Seats increase clockwise. Gaps and an unoccupied button are permitted.
  std::size_t seat_count = 0;
  std::size_t button = 0;
  OddChipRule odd_chip_rule = OddChipRule::Unspecified;
  RakePolicy rake;
  bool flop_dealt = false;
};

struct PotLayer {
  Chips contribution_level = 0;
  Chips amount = 0;
  Chips rake = 0;
  // Entries index SettlementInput::players, not seat numbers.
  std::vector<std::size_t> contributors;
  std::vector<std::size_t> eligible;
  // Ordered clockwise left of the button, including a tied button last.
  std::vector<std::size_t> winners;
};

struct ContributionSettlement {
  // Player vectors preserve input order; layers are lowest contribution first.
  std::vector<Chips> net_contributions;
  std::vector<Chips> awards;
  std::vector<Chips> refunds;
  std::vector<Chips> final_stacks;
  std::vector<std::int64_t> chip_utility;
  std::vector<PotLayer> layers;
  Chips gross_contributions = 0;
  Chips total_refunds = 0;
  Chips pot = 0;
  Chips rake = 0;
};

// Supports 2..10 players with explicit odd-chip and rake rules. The 2..6
// range is verified bit-for-bit against the RFC 0006 fixtures and the Stage 11
// exhaustive grid; the 7..10 extension (RFC 0008 stage 2) shares the same
// algorithm and is covered by the grid extension's independent chip
// conservation and side-pot checks.
// G/F are validated before subtraction. Gross total and sum(stack + G - F)
// must each fit kMaxHeadsUpChips (2^53 - 1), as must all chip inputs.
// A sole-contributor positive layer is invalid: this function never invents
// refunds. A sole eligible winner among multiple contributors is valid.
// final_stacks = stack + awards; chip_utility = awards + F - G.
// Invalid input throws std::invalid_argument; integer/profile overflow throws
// std::overflow_error. Input is unchanged on both success and failure.
ContributionSettlement settle_contributions(const SettlementInput& input);

}  // namespace bs::poker
