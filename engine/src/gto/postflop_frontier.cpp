// RFC 0013 Stage 1: postflop-aware frontier evaluation (implementation).

#include <algorithm>
#include <bs/abstraction.hpp>
#include <bs/game_definition.hpp>
#include <bs/postflop_frontier.hpp>
#include <cmath>
#include <cstring>
#include <stdexcept>

namespace bs::gto {

// ---------------------------------------------------------------------------
// SprBucketConfig
// ---------------------------------------------------------------------------

std::size_t SprBucketConfig::bucket_for_spr(double spr) {
  if (!std::isfinite(spr) || spr <= 0.0)
    throw std::invalid_argument("SPR must be finite and positive");
  for (std::size_t i = 0; i < kBucketCount; ++i) {
    if (spr <= kLowerBounds[i])
      continue;
    const double upper =
        (i + 1 < kBucketCount) ? kLowerBounds[i + 1] : std::numeric_limits<double>::infinity();
    if (spr <= upper)
      return i;
  }
  // SPR > 12 falls in the last bucket (12, inf].
  return kBucketCount - 1;
}

// ---------------------------------------------------------------------------
// NSeatDeclaredFrontierTable
// ---------------------------------------------------------------------------

NSeatDeclaredFrontierTable::NSeatDeclaredFrontierTable(std::vector<Entry> entries,
                                                       Metadata metadata)
    : metadata_(metadata) {
  for (const Entry& e : entries) {
    if (e.hands.size() < 2 || e.hands.size() > poker::kMaxUnifiedSeats)
      throw std::invalid_argument("NSeatDeclaredFrontierTable: seat count out of range");
    if (e.values.size() != e.hands.size())
      throw std::invalid_argument("NSeatDeclaredFrontierTable: values/hands size mismatch");
    if (e.spr_bucket >= SprBucketConfig::kBucketCount)
      throw std::invalid_argument("NSeatDeclaredFrontierTable: SPR bucket out of range");

    Key key{};
    key.canonical_flop = e.canonical_flop;
    key.spr_bucket = e.spr_bucket;
    key.seat_count = e.hands.size();
    for (std::size_t s = 0; s < e.hands.size(); ++s)
      key.hands[s] = e.hands[s];

    auto [it, inserted] = table_.emplace(std::move(key), std::move(e.values));
    if (!inserted)
      throw std::invalid_argument("NSeatDeclaredFrontierTable: duplicate key");
  }
}

const std::vector<double>& NSeatDeclaredFrontierTable::lookup(
    const std::array<int, 3>& canonical_flop, std::uint8_t spr_bucket,
    std::span<const std::array<int, 2>> hands) const {
  Key key{};
  key.canonical_flop = canonical_flop;
  key.spr_bucket = spr_bucket;
  key.seat_count = hands.size();
  for (std::size_t s = 0; s < hands.size(); ++s)
    key.hands[s] = hands[s];

  auto it = table_.find(key);
  if (it == table_.end())
    throw std::invalid_argument("NSeatDeclaredFrontierTable: missing entry");
  return it->second;
}

std::size_t NSeatDeclaredFrontierTable::KeyHash::operator()(const Key& key) const noexcept {
  // FNV-1a 64-bit over the key's raw bytes.
  std::size_t hash = 0xcbf29ce484222325ULL;
  auto mix = [&hash](const void* data, std::size_t len) {
    const auto* bytes = static_cast<const unsigned char*>(data);
    for (std::size_t i = 0; i < len; ++i) {
      hash ^= bytes[i];
      hash *= 0x100000001b3ULL;
    }
  };
  mix(key.canonical_flop.data(), sizeof(key.canonical_flop));
  mix(&key.spr_bucket, sizeof(key.spr_bucket));
  mix(&key.seat_count, sizeof(key.seat_count));
  // Hash only the hands for the active seats (not the full fixed array).
  for (std::size_t s = 0; s < key.seat_count; ++s)
    mix(key.hands[s].data(), sizeof(key.hands[s]));
  return hash;
}

// ---------------------------------------------------------------------------
// PostflopFrontierEvaluator
// ---------------------------------------------------------------------------

PostflopFrontierEvaluator::PostflopFrontierEvaluator(const NSeatDeclaredFrontierTable& table)
    : table_(table) {}

std::vector<double> PostflopFrontierEvaluator::evaluate(
    std::span<const int> flop, std::span<const std::array<int, 2>> hands,
    const bs::tree::TerminalPayload& ledger) const {
  const std::size_t n = hands.size();
  if (n < 2 || n > poker::kMaxUnifiedSeats)
    throw std::invalid_argument("PostflopFrontierEvaluator: seat count out of range");
  if (flop.size() != 3)
    throw std::invalid_argument("PostflopFrontierEvaluator: flop must have 3 cards");
  if (ledger.player_count != n)
    throw std::invalid_argument("PostflopFrontierEvaluator: ledger player_count mismatch");

  // 1. Canonicalize the flop to its suit-isomorphism class.
  const std::array<int, 3> flop_arr = {flop[0], flop[1], flop[2]};
  const abstraction::CanonicalBoard canon = abstraction::canonicalize(flop_arr);

  // 2. Relabel each seat's hole cards through the canonical relabel and sort.
  std::array<std::array<int, 2>, poker::kMaxUnifiedSeats> relabeled{};
  for (std::size_t s = 0; s < n; ++s) {
    for (std::size_t c = 0; c < 2; ++c) {
      const int rank = hands[s][c] / 4;
      const int suit = hands[s][c] % 4;
      relabeled[s][c] = rank * 4 + canon.relabel[suit];
    }
    std::sort(relabeled[s].begin(), relabeled[s].end());
  }

  // 3. Compute pot and effective stack from the ledger.
  poker::Chips pot = 0;
  for (std::size_t s = 0; s < n; ++s)
    pot += ledger.seats[s].contributed;
  if (pot == 0)
    throw std::invalid_argument("PostflopFrontierEvaluator: pot is zero");

  poker::Chips eff_stack = 0;
  bool found_live = false;
  for (std::size_t s = 0; s < n; ++s) {
    if (ledger.seats[s].folded)
      continue;
    if (!found_live || ledger.seats[s].stack < eff_stack)
      eff_stack = ledger.seats[s].stack;
    found_live = true;
  }
  if (!found_live)
    throw std::invalid_argument("PostflopFrontierEvaluator: no live seats");

  // 4. Derive SPR and map to bucket.
  const double spr = static_cast<double>(eff_stack) / static_cast<double>(pot);
  const std::size_t bucket = SprBucketConfig::bucket_for_spr(spr);

  // 5. Look up the stored per-seat values in the declared table.
  const std::vector<double>& stored =
      table_.lookup(canon.board, static_cast<std::uint8_t>(bucket),
                    std::span<const std::array<int, 2>>(relabeled.data(), n));

  // 6. Scale from representative pot to actual pot.
  const double rep_pot = table_.metadata().representative_pot[bucket];
  if (!(rep_pot > 0.0))
    throw std::invalid_argument("PostflopFrontierEvaluator: representative pot not set");
  const double scale = static_cast<double>(pot) / rep_pot;

  std::vector<double> values(n);
  for (std::size_t s = 0; s < n; ++s)
    values[s] = stored[s] * scale;
  return values;
}

}  // namespace bs::gto
