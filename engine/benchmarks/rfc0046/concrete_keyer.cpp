#include "concrete_keyer.hpp"

#include <algorithm>
#include <stdexcept>

namespace bs::rfc0046 {

namespace {

std::array<int, 2> normalized(const std::array<int, 2>& hole) {
  std::array<int, 2> out = hole;
  if (out[0] > out[1])
    std::swap(out[0], out[1]);
  return out;
}

}  // namespace

ConcreteCardKeyer::ConcreteCardKeyer(std::span<const int> board4) {
  if (board4.size() != kTurnBoardCards)
    throw std::invalid_argument("ConcreteCardKeyer: expected a four-card turn board");
  for (std::size_t i = 0; i < kTurnBoardCards; ++i) {
    if (board4[i] < 0 || board4[i] > 51)
      throw std::invalid_argument("ConcreteCardKeyer: board card out of range");
    board4_[i] = board4[i];
    blocked_[board4[i]] = true;
  }
  for (int card = 0; card < 52; ++card)
    if (!blocked_[card])
      live_.push_back(card);
  live_rank_.fill(-1);
  for (std::size_t i = 0; i < live_.size(); ++i)
    live_rank_[live_[i]] = static_cast<std::int32_t>(i);
  if (live_.size() != kLiveCardsOnTurn)
    throw std::invalid_argument("ConcreteCardKeyer: board must have four distinct cards");
}

std::uint32_t ConcreteCardKeyer::dense_for_live_pair(const std::array<int, 2>& hole) const {
  const std::array<int, 2> h = normalized(hole);
  const std::int32_t i = live_rank_[h[0]];
  const std::int32_t j = live_rank_[h[1]];
  if (i < 0 || j < 0)
    throw std::invalid_argument("ConcreteCardKeyer: holding shares a card with the board");
  if (i >= j)
    throw std::invalid_argument("ConcreteCardKeyer: holding must be two distinct cards");
  const auto ni = static_cast<std::uint32_t>(i);
  const auto nj = static_cast<std::uint32_t>(j);
  const std::uint32_t offset = ni * kLiveCardsOnTurn - ni * (ni + 1u) / 2u;
  return offset + (nj - ni - 1u);
}

std::uint32_t ConcreteCardKeyer::bucket(const std::array<int, 2>& hole,
                                        std::span<const int> board) const {
  if (board.size() < kTurnBoardCards)
    throw std::invalid_argument("ConcreteCardKeyer: board prefix shorter than turn");
  for (std::size_t i = 0; i < kTurnBoardCards; ++i)
    if (board[i] != board4_[i])
      throw std::invalid_argument("ConcreteCardKeyer: board prefix does not match the keyer");
  return dense_for_live_pair(hole);
}

std::array<int, 2> ConcreteCardKeyer::holding(std::uint32_t dense) const {
  if (dense >= kConcreteBucketCap)
    throw std::invalid_argument("ConcreteCardKeyer: dense id out of range");
  // Find largest i with offset(i) <= dense, with offset(i) = i*n - i*(i+1)/2.
  std::uint32_t lo = 0;
  std::uint32_t hi = kLiveCardsOnTurn - 1;
  while (lo < hi) {
    const std::uint32_t mid = lo + (hi - lo + 1u) / 2u;
    const std::uint32_t off = mid * kLiveCardsOnTurn - mid * (mid + 1u) / 2u;
    if (off <= dense)
      lo = mid;
    else
      hi = mid - 1u;
  }
  const std::uint32_t i = lo;
  const std::uint32_t off = i * kLiveCardsOnTurn - i * (i + 1u) / 2u;
  const std::uint32_t j = i + 1u + (dense - off);
  return {live_[i], live_[j]};
}

std::vector<std::array<int, 2>> live_unblocked_holdings(std::span<const int> board) {
  std::array<bool, 52> blocked{};
  for (int card : board) {
    if (card < 0 || card > 51)
      throw std::invalid_argument("live_unblocked_holdings: board card out of range");
    blocked[card] = true;
  }
  std::vector<int> live;
  for (int card = 0; card < 52; ++card)
    if (!blocked[card])
      live.push_back(card);
  std::vector<std::array<int, 2>> out;
  out.reserve(live.size() * (live.size() - 1) / 2);
  for (std::size_t i = 0; i < live.size(); ++i)
    for (std::size_t j = i + 1; j < live.size(); ++j)
      out.push_back({live[i], live[j]});
  return out;
}

}  // namespace bs::rfc0046
