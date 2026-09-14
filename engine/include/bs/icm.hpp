// Bounded, offline Independent Chip Model arithmetic in prize atomic units.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace bs::poker {

inline constexpr std::uint64_t kMaxIcmAmount = (std::uint64_t{1} << 53) - 1;
inline constexpr std::size_t kMaxIcmPlayers = 10;

struct IcmPlayer {
  std::string id;
  std::uint64_t stack = 0;
};

struct IcmField {
  // Every remaining player, including other tables; no historical eliminations.
  std::vector<IcmPlayer> players;
  // One entry per remaining place, best first, including explicit unpaid zeros.
  std::vector<std::uint64_t> payouts;
  // Identifies a prize atomic unit, never a chip-EV conversion or exchange rate.
  std::string payout_unit;
  // Caller attestation: complete public field and payout data are available.
  bool complete = false;
};

enum class IcmTieRule {
  RejectTies,
  // Select only when the provider declares ascending unsigned ID-byte order.
  StableIdByteOrder,
};

struct IcmEquities {
  std::string payout_unit;
  // Same order as IcmField::players. Expected values are never rounded.
  std::vector<double> equities;
};

struct IcmPlayerUtility {
  std::string id;
  double prehand_equity = 0;
  // Actual prizes for newly busted players only, in exact payout atomic units.
  std::uint64_t awarded_prize = 0;
  // Zero for busted players; the sole survivor has equity payouts[0].
  double survivor_equity = 0;
  double terminal_equity = 0;
  double utility = 0;
};

struct IcmTerminalUtility {
  std::string payout_unit;
  // Pre-hand player order, independent of post-hand input order.
  std::vector<IcmPlayerUtility> players;
};

// Supports 2..10 positive stacks with nonempty unique stable IDs. Payouts are
// nonincreasing, with chip and prize totals each <= kMaxIcmAmount. Invalid or
// unsupported input throws std::invalid_argument. Numerical invariant failures
// throw std::logic_error; finite double sums use 1e-10 relative prize tolerance.
IcmEquities icm_equities(const IcmField& field);

// Post-hand stacks must be fully settled, conserve chips, and contain exactly
// the pre-hand IDs. All zero stacks are newly busted in this one hand. Fewer
// pre-hand chips finish lower; equal pre-hand chips split the occupied prizes.
// Ties require the explicit supported rule even when division has no remainder.
// One survivor is supported, zero survivors are rejected. No rake, historical
// eliminations, remote-field inference, or chip settlement occurs in this API.
IcmTerminalUtility icm_terminal_utility(const IcmField& prehand,
                                        const std::vector<IcmPlayer>& posthand,
                                        IcmTieRule tie_rule = IcmTieRule::RejectTies);

}  // namespace bs::poker
