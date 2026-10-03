// RFC 0007 option A: the declared frontier table.
//
// The table stores pre-computed per-seat values for each (flop, joint deal)
// pair, produced offline by evaluating a flop-rooted blueprint. Lookup is O(1)
// via a canonical 42-bit key. Missing entries throw (fail closed, never
// approximate).
#include <algorithm>
#include <bs/frontier.hpp>
#include <cstdio>
#include <sstream>

namespace bs::gto {

namespace {

// Sort a 3-card flop in place for canonical keying.
void sort3(std::array<int, 3>& cards) {
  std::sort(cards.begin(), cards.end());
}

// Sort a 2-card hand in place for canonical keying.
void sort2(std::array<int, 2>& hand) {
  if (hand[0] > hand[1])
    std::swap(hand[0], hand[1]);
}

}  // namespace

DeclaredFrontierTable::DeclaredFrontierTable(std::vector<Entry> entries) {
  table_.reserve(entries.size());
  for (const Entry& e : entries) {
    std::array<int, 3> flop = e.flop;
    sort3(flop);
    std::array<std::array<int, 2>, 2> hands = e.hands;
    sort2(hands[0]);
    sort2(hands[1]);
    const std::uint64_t key = make_key(flop, hands);
    auto [it, inserted] = table_.emplace(key, e.values);
    if (!inserted)
      throw std::invalid_argument("DeclaredFrontierTable: duplicate (flop, joint deal) entry");
  }
}

std::vector<double> DeclaredFrontierTable::evaluate(std::span<const int> flop,
                                                    std::span<const std::array<int, 2>> hands,
                                                    const bs::tree::TerminalPayload& ledger) const {
  (void)ledger;  // The declared table is pre-computed; the ledger is not needed.
  if (flop.size() != 3)
    throw std::invalid_argument("DeclaredFrontierTable::evaluate: flop must have exactly 3 cards");
  if (hands.size() != 2)
    throw std::invalid_argument(
        "DeclaredFrontierTable::evaluate: frontier evaluation is heads-up only "
        "(2 seats)");

  std::array<int, 3> flop_sorted{};
  std::copy(flop.begin(), flop.end(), flop_sorted.begin());
  sort3(flop_sorted);
  std::array<std::array<int, 2>, 2> hands_sorted{};
  std::copy(hands.begin(), hands.end(), hands_sorted.begin());
  sort2(hands_sorted[0]);
  sort2(hands_sorted[1]);

  const std::uint64_t key = make_key(flop_sorted, hands_sorted);
  auto it = table_.find(key);
  if (it == table_.end()) {
    // Fail closed: never approximate a missing frontier value.
    std::ostringstream oss;
    oss << "DeclaredFrontierTable: no entry for flop [" << flop_sorted[0] << "," << flop_sorted[1]
        << "," << flop_sorted[2] << "] hands [" << hands_sorted[0][0] << "," << hands_sorted[0][1]
        << "] [" << hands_sorted[1][0] << "," << hands_sorted[1][1] << "]";
    throw std::runtime_error(oss.str());
  }
  return {it->second[0], it->second[1]};
}

std::uint64_t DeclaredFrontierTable::make_key(std::span<const int> flop,
                                              std::span<const std::array<int, 2>> hands) {
  // 7 cards × 6 bits = 42 bits, fits in uint64_t.
  std::uint64_t key = 0;
  int shift = 0;
  for (int i = 0; i < 3; ++i) {
    key |= static_cast<std::uint64_t>(flop[i]) << shift;
    shift += 6;
  }
  for (std::size_t seat = 0; seat < 2; ++seat) {
    for (int card = 0; card < 2; ++card) {
      key |= static_cast<std::uint64_t>(hands[seat][card]) << shift;
      shift += 6;
    }
  }
  return key;
}

}  // namespace bs::gto
