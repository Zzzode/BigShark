// RFC 0007: the frontier evaluator contract for flop-terminal games.
// RFC 0024: generalized to any street boundary (3/4/5-card board).
//
// A flop-terminal game (TerminalDepth::Flop) ends when a completed flop would
// open action; the frontier leaf's value is supplied by a declared frontier
// evaluator rather than by the rules' showdown. The evaluator produces per-seat
// chip utility for a given board and joint deal.
//
// RFC 0024 extends the interface to accept a 3/4/5-card board, enabling value
// estimation at any street boundary for depth-limited resolving. The
// "frontier" is interpreted as the boundary of the solved subgame, not
// specifically the flop.
//
// Contract (RFC 0007, scope extended by RFC 0010 and RFC 0024):
//  - Identity: the evaluator declares the game identity of its flop-rooted
//    policy. Two evaluators with the same identity produce the same values.
//  - Normalization: values are expectations over the policy's action
//    distribution, not deterministic best responses.
//  - Conditioning: values are NOT conditioned on the acting player's own
//    holding. The evaluator sees the joint deal but must not use a seat's
//    knowledge of its own cards to condition the opponent's range.
//  - Scope: 2..10 seats (RFC 0010). Implementations must reject
//    player_count < 2 or player_count > kMaxUnifiedSeats. The
//    DeclaredFrontierTable remains heads-up only (its Entry is fixed-size
//    and keyed to a 2-seat blueprint).
//  - Folded seats: only live seats (ledger.seats[s].folded == false)
//    compete for the pot. Folded seats receive equity 0; their contributed
//    chips remain in the pot as dead money.
//  - Board size (RFC 0024): the board is 3 (flop), 4 (turn), or 5 (river)
//    cards. Implementations must throw std::invalid_argument for unsupported
//    board sizes (fail closed).
#pragma once

#include <array>
#include <bs/abstract_tree.hpp>
#include <cstddef>
#include <cstdint>
#include <span>
#include <unordered_map>
#include <vector>

namespace bs::gto {

// The frontier evaluator interface. Implementations supply per-seat chip
// utility for a frontier leaf. The interface supports 2..10 seats
// (RFC 0010) and 3/4/5-card boards (RFC 0024); implementations must reject
// seat counts or board sizes outside their supported range.
// Folded seats (ledger.seats[s].folded == true) receive equity 0.
class FrontierEvaluator {
 public:
  virtual ~FrontierEvaluator() = default;

  // Per-seat chip utility for the given board and joint deal.
  // `board` is 3 (flop), 4 (turn), or 5 (river) card ids (0..51).
  // `hands` is per-seat sorted hole cards (2 cards each, 2..10 entries).
  // `ledger` is the terminal payload's seat ledger (folded flags + pot).
  // Returns one value per seat in chip units.
  // Throws on a missing entry or unsupported seat count/board size (fail closed).
  virtual std::vector<double> evaluate(std::span<const int> board,
                                       std::span<const std::array<int, 2>> hands,
                                       const bs::tree::TerminalPayload& ledger) const = 0;
};

// RFC 0007 option A: a declared frontier table. An immutable map from
// (flop, joint deal) to per-seat values, supplied by the caller as a declared
// input (produced offline by evaluating a flop-rooted blueprint). Lookup is
// O(1) via a hash map.
//
// The table is the first implementation per RFC 0007's rollout plan. Option B
// (nested evaluation, in-process blueprint lookup) is deferred to a future
// stage; the option is chosen by measured cost (byte budget, then wall time).
//
// Heads-up only (RFC 0010): the Entry struct is fixed-size for 2 seats and
// keyed to a 2-seat blueprint. N-way declared tables are future work if
// nested evaluation is revived.
//
// RFC 0024: flop-specific only. evaluate() throws std::invalid_argument for
// 4/5-card boards (fail closed).
class DeclaredFrontierTable : public FrontierEvaluator {
 public:
  // A single table entry: the 3 flop cards (ascending), each seat's 2 hole
  // cards (ascending within seat), and the per-seat chip utility values.
  struct Entry {
    std::array<int, 3> flop;
    std::array<std::array<int, 2>, 2> hands;  // [seat][card]
    std::array<double, 2> values;             // [seat]
  };

  // Construct from a list of entries. Duplicate keys throw
  // std::invalid_argument. The table is immutable after construction.
  explicit DeclaredFrontierTable(std::vector<Entry> entries);

  // Throws std::invalid_argument if board.size() != 3 (flop-specific only).
  std::vector<double> evaluate(std::span<const int> board,
                               std::span<const std::array<int, 2>> hands,
                               const bs::tree::TerminalPayload& ledger) const override;

  std::size_t size() const noexcept { return table_.size(); }

 private:
  // Canonical 42-bit key: 3 flop cards + 2×2 hole cards, 6 bits each,
  // sorted within each group (flop and per-seat hands).
  static std::uint64_t make_key(std::span<const int> flop,
                                std::span<const std::array<int, 2>> hands);

  std::unordered_map<std::uint64_t, std::array<double, 2>> table_;
};

}  // namespace bs::gto
