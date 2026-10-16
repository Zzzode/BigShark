#include <algorithm>
#include <bs/equity_frontier.hpp>
#include <bs/eval.hpp>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <vector>

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

std::vector<int> EquityFrontierEvaluator::make_key(std::span<const int> board,
                                                   std::span<const std::array<int, 2>> hands,
                                                   const bs::tree::TerminalPayload& ledger) {
  // The key is: sorted board (3/4/5 cards), per-seat sorted hole cards (2
  // each), the live-seat set (one int per seat: 0 = folded, 1 = live), and
  // per-seat net contributions (two ints each: low/high 32 bits). The
  // contribution suffix is required because the cached expected award
  // depends on the pot layer structure, which changes with the contribution
  // pattern even for the same (board, hands, live-set).
  std::vector<int> key;
  key.reserve(board.size() + hands.size() * 2 + hands.size() + hands.size() * 2);

  // Sorted board.
  std::vector<int> sorted_board(board.begin(), board.end());
  std::sort(sorted_board.begin(), sorted_board.end());
  for (int c : sorted_board)
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

  // Per-seat net contributions (G - F), split into low/high ints.
  for (std::size_t s = 0; s < hands.size(); ++s) {
    const auto net = ledger.seats[s].contributed - ledger.seats[s].refunded;
    key.push_back(static_cast<int>(net & 0xFFFFFFFFu));
    key.push_back(static_cast<int>(net >> 32));
  }

  return key;
}

std::vector<double> EquityFrontierEvaluator::evaluate(
    std::span<const int> board, std::span<const std::array<int, 2>> hands,
    const bs::tree::TerminalPayload& ledger) const {
  const std::size_t n = hands.size();
  if (n < 2 || n > bs::poker::kMaxUnifiedSeats)
    throw std::invalid_argument("equity frontier evaluator supports 2..10 seats");
  if (board.size() != 3 && board.size() != 4 && board.size() != 5)
    throw std::invalid_argument("equity frontier evaluator requires a 3/4/5-card board");
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

  // Compute net contributions (G - F) and form pot layers. Each layer
  // captures the contribution increment that a subset of seats competes for.
  // Layers are deterministic: they depend only on the ledger, not on the
  // runout. This mirrors the layer logic in settle_contributions.
  bs::poker::Chips net[bs::poker::kMaxUnifiedSeats]{};
  std::vector<bs::poker::Chips> levels;
  for (std::size_t s = 0; s < n; ++s) {
    net[s] = ledger.seats[s].contributed - ledger.seats[s].refunded;
    if (net[s] != 0)
      levels.push_back(net[s]);
  }
  std::sort(levels.begin(), levels.end());
  levels.erase(std::unique(levels.begin(), levels.end()), levels.end());

  struct Layer {
    bs::poker::Chips amount = 0;
    std::vector<std::size_t> eligible;
  };
  std::vector<Layer> layers;
  layers.reserve(levels.size());
  bs::poker::Chips previous = 0;
  for (bs::poker::Chips level : levels) {
    Layer layer;
    std::size_t contributors = 0;
    for (std::size_t s = 0; s < n; ++s) {
      if (net[s] >= level) {
        ++contributors;
        if (!ledger.seats[s].folded)
          layer.eligible.push_back(s);
      }
    }
    if (contributors < 2)
      throw std::invalid_argument(
          "equity frontier evaluator: unmatched contribution (missing refund)");
    if (layer.eligible.empty())
      throw std::invalid_argument("equity frontier evaluator: pot layer has no eligible winner");
    layer.amount = (level - previous) * static_cast<bs::poker::Chips>(contributors);
    layers.push_back(std::move(layer));
    previous = level;
  }

  const std::vector<int> key = make_key(board, hands, ledger);

  // Check the cache. The value is a vector of N expected awards (expected
  // amount won from the pot, before subtracting net contribution).
  std::vector<double> expected_award(n, 0.0);
  bool found = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (auto it = award_cache_.find(key); it != award_cache_.end()) {
      expected_award = it->second;
      found = true;
    }
  }

  if (!found) {
    // Build the remaining card pool: 52 - board - 2N hole cards. All dealt
    // cards are blocked (including folded seats' cards), matching the
    // physical-deck interpretation of the sampler.
    bool blocked[52] = {};
    for (int c : board)
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

    std::vector<double> award_sum(n, 0.0);
    std::size_t total_runouts = 0;

    // Settle one runout: compute showdown scores for live seats, then award
    // each layer to the best hand among its eligible seats.
    auto settle_runout = [&](const int full_board[5]) {
      std::uint32_t scores[bs::poker::kMaxUnifiedSeats] = {};
      for (std::size_t s = 0; s < n; ++s) {
        if (ledger.seats[s].folded)
          continue;
        const int cards[7] = {hands[s][0],   hands[s][1],   full_board[0], full_board[1],
                              full_board[2], full_board[3], full_board[4]};
        scores[s] = bs::evaluate(cards, 7).score;
      }
      for (const auto& layer : layers) {
        std::uint32_t best = 0;
        for (std::size_t s : layer.eligible)
          if (scores[s] > best)
            best = scores[s];
        int tied = 0;
        for (std::size_t s : layer.eligible)
          if (scores[s] == best)
            ++tied;
        const double share = static_cast<double>(layer.amount) / static_cast<double>(tied);
        for (std::size_t s : layer.eligible)
          if (scores[s] == best)
            award_sum[s] += share;
      }
      ++total_runouts;
    };

    if (board.size() == 5) {
      // River: no cards to enumerate. Direct showdown settlement.
      settle_runout(board.data());
    } else if (board.size() == 4) {
      // Turn: enumerate all C(pool, 1) river combos.
      for (int i = 0; i < pool_size; ++i) {
        int full_board[5];
        std::copy(board.begin(), board.end(), full_board);
        full_board[4] = pool[i];
        settle_runout(full_board);
      }
    } else {
      // Flop (3 cards): enumerate all C(pool, 2) turn/river combos.
      for (int i = 0; i < pool_size; ++i) {
        for (int j = i + 1; j < pool_size; ++j) {
          int full_board[5];
          std::copy(board.begin(), board.end(), full_board);
          full_board[3] = pool[i];
          full_board[4] = pool[j];
          settle_runout(full_board);
        }
      }
    }

    for (std::size_t s = 0; s < n; ++s)
      expected_award[s] = award_sum[s] / static_cast<double>(total_runouts);

    // Insert into the cache if under the cap. Existing entries remain valid;
    // the cap only prevents unbounded growth for N-way preflop training.
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (award_cache_.size() < cache_cap_)
        award_cache_.emplace(key, expected_award);
    }
  }

  // Net chip delta: expected award minus net contribution. Folded seats
  // have expected award 0, so their utility is -net (they lose their dead
  // money), which is correct.
  std::vector<double> values(n);
  for (std::size_t s = 0; s < n; ++s)
    values[s] = expected_award[s] - static_cast<double>(net[s]);
  return values;
}

}  // namespace bs::gto
