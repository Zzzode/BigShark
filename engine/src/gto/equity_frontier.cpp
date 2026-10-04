#include <algorithm>
#include <bs/equity_frontier.hpp>
#include <bs/eval.hpp>
#include <cstddef>
#include <cstdint>
#include <stdexcept>

namespace bs::gto {

namespace {

// FNV-1a 64-bit offset basis and prime, for hashing the cache key bytes.
constexpr std::uint64_t kFnvOffsetBasis = 0xcbf29ce484222325ULL;
constexpr std::uint64_t kFnvPrime = 0x100000001b3ULL;

}  // namespace

std::size_t EquityFrontierEvaluator::KeyHash::operator()(
    const std::vector<int>& key) const noexcept {
  std::uint64_t hash = kFnvOffsetBasis;
  const auto* bytes = reinterpret_cast<const unsigned char*>(key.data());
  const std::size_t nbytes = key.size() * sizeof(int);
  for (std::size_t i = 0; i < nbytes; ++i) {
    hash ^= static_cast<std::uint64_t>(bytes[i]);
    hash *= kFnvPrime;
  }
  return static_cast<std::size_t>(hash);
}

std::vector<int> EquityFrontierEvaluator::make_key(std::span<const int> flop,
                                                   std::span<const std::array<int, 2>> hands,
                                                   const bs::tree::TerminalPayload& ledger) {
  // The key is: sorted flop (3 cards), per-seat sorted hole cards (2 each),
  // then the live-seat set (one int per seat: 0 = folded, 1 = live). The
  // live-set is required because the cached equity depends on which seats
  // compete: the same (flop, hands) pair can reach evaluate() with different
  // fold patterns.
  std::vector<int> key;
  key.reserve(3 + hands.size() * 2 + hands.size());

  // Sorted flop.
  std::array<int, 3> sorted_flop{};
  std::copy(flop.begin(), flop.end(), sorted_flop.begin());
  std::sort(sorted_flop.begin(), sorted_flop.end());
  for (int c : sorted_flop)
    key.push_back(c);

  // Per-seat sorted hole cards.
  for (const auto& hand : hands) {
    std::array<int, 2> sorted_hand = hand;
    std::sort(sorted_hand.begin(), sorted_hand.end());
    key.push_back(sorted_hand[0]);
    key.push_back(sorted_hand[1]);
  }

  // Live-seat set (0 = folded, 1 = live).
  for (std::size_t s = 0; s < hands.size(); ++s)
    key.push_back(ledger.seats[s].folded ? 0 : 1);

  return key;
}

std::vector<double> EquityFrontierEvaluator::evaluate(
    std::span<const int> flop, std::span<const std::array<int, 2>> hands,
    const bs::tree::TerminalPayload& ledger) const {
  const std::size_t n = hands.size();
  if (n < 2 || n > bs::poker::kMaxUnifiedSeats)
    throw std::invalid_argument("equity frontier evaluator supports 2..10 seats");
  if (flop.size() != 3)
    throw std::invalid_argument("equity frontier evaluator requires a 3-card flop");
  if (ledger.player_count != n)
    throw std::invalid_argument(
        "equity frontier evaluator: ledger player_count does not match hands");

  // Count live seats; a frontier with no live seats is invalid.
  std::size_t live_count = 0;
  for (std::size_t s = 0; s < n; ++s)
    if (!ledger.seats[s].folded)
      ++live_count;
  if (live_count == 0)
    throw std::invalid_argument("equity frontier evaluator: no live seats at frontier");

  const std::vector<int> key = make_key(flop, hands, ledger);

  // Check the cache. The value is a vector of N equity fractions.
  std::vector<double> equity(n, 0.0);
  if (auto it = equity_cache_.find(key); it != equity_cache_.end()) {
    equity = it->second;
  } else {
    // Build the remaining card pool: 52 - 3 flop - 2N hole cards. All dealt
    // cards are blocked (including folded seats' cards), matching the
    // physical-deck interpretation of the sampler.
    bool blocked[52] = {};
    for (int c : flop)
      blocked[c] = true;
    for (const auto& hand : hands) {
      blocked[hand[0]] = true;
      blocked[hand[1]] = true;
    }
    int pool[52];
    int pool_size = 0;
    for (int c = 0; c < 52; ++c)
      if (!blocked[c])
        pool[pool_size++] = c;

    // Enumerate all C(pool, 2) turn/river combos. For each combo, evaluate
    // only live seats' hands, find the best score among live seats, and split
    // the runout equally among tied live seats. Folded seats do not compete
    // and receive equity 0; their contributed chips stay in the pot.
    std::vector<double> equity_sum(n, 0.0);
    std::uint32_t scores[bs::poker::kMaxUnifiedSeats] = {};
    for (int i = 0; i < pool_size; ++i) {
      for (int j = i + 1; j < pool_size; ++j) {
        std::uint32_t best = 0;
        for (std::size_t s = 0; s < n; ++s) {
          if (ledger.seats[s].folded)
            continue;
          const int cards[7] = {hands[s][0], hands[s][1], flop[0], flop[1],
                                flop[2],     pool[i],     pool[j]};
          scores[s] = bs::evaluate(cards, 7).score;
          if (scores[s] > best)
            best = scores[s];
        }
        int tied = 0;
        for (std::size_t s = 0; s < n; ++s)
          if (!ledger.seats[s].folded && scores[s] == best)
            ++tied;
        const double share = 1.0 / static_cast<double>(tied);
        for (std::size_t s = 0; s < n; ++s)
          if (!ledger.seats[s].folded && scores[s] == best)
            equity_sum[s] += share;
      }
    }

    const std::size_t total =
        static_cast<std::size_t>(pool_size) * static_cast<std::size_t>(pool_size - 1) / 2;
    for (std::size_t s = 0; s < n; ++s)
      equity[s] = equity_sum[s] / static_cast<double>(total);

    // Insert into the cache if under the cap. Existing entries remain valid;
    // the cap only prevents unbounded growth for N-way preflop training.
    if (equity_cache_.size() < cache_cap_)
      equity_cache_.emplace(key, equity);
  }

  // The pot is the sum of all seats' contributed amounts (including folded
  // seats' dead money).
  double pot = 0;
  for (std::size_t s = 0; s < n; ++s)
    pot += static_cast<double>(ledger.seats[s].contributed);

  // Net chip delta: expected pot share minus own contribution. Folded seats
  // have equity 0, so their utility is -contributed (they lose their dead
  // money), which is correct.
  std::vector<double> values(n);
  for (std::size_t s = 0; s < n; ++s)
    values[s] = equity[s] * pot - static_cast<double>(ledger.seats[s].contributed);
  return values;
}

}  // namespace bs::gto
