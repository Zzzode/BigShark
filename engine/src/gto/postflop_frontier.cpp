// RFC 0013 Stage 1: postflop-aware frontier evaluation (implementation).

#include <algorithm>
#include <bs/abstract_tree.hpp>
#include <bs/abstraction.hpp>
#include <bs/game_definition.hpp>
#include <bs/nseat_policy_codec.hpp>
#include <bs/postflop_config.hpp>
#include <bs/postflop_frontier.hpp>
#include <bs/root_value_extractor.hpp>
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

  // All-in (zero SPR): no postflop decisions remain. Exact all-in equity is
  // the equilibrium value, not a heuristic approximation.
  if (!std::isfinite(spr) || spr <= 0.0)
    return equity_fallback_.evaluate(flop, hands, ledger);

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

// ---------------------------------------------------------------------------
// OnTheFlyPostflopEvaluator
// ---------------------------------------------------------------------------

struct OnTheFlyPostflopEvaluator::CachedConfig {
  solver::NSeatPolicy policy;
  std::unique_ptr<tree::AbstractTree> tree;
  std::atomic<std::size_t> last_access{0};
};

struct OnTheFlyPostflopEvaluator::Cache {
  std::map<std::pair<std::array<int, 3>, std::uint8_t>, std::shared_ptr<CachedConfig>> entries;
  std::atomic<std::size_t> access_counter{0};
};

OnTheFlyPostflopEvaluator::OnTheFlyPostflopEvaluator(const NSeatDeclaredFrontierTable& table,
                                                     std::filesystem::path policy_dir,
                                                     std::size_t cache_capacity)
    : table_(table),
      policy_dir_(std::move(policy_dir)),
      cache_capacity_(cache_capacity),
      cache_(std::make_unique<Cache>()) {}

OnTheFlyPostflopEvaluator::~OnTheFlyPostflopEvaluator() = default;

std::shared_ptr<const OnTheFlyPostflopEvaluator::CachedConfig>
OnTheFlyPostflopEvaluator::get_config(const std::array<int, 3>& flop_class,
                                      std::uint8_t spr_bucket) const {
  using Key = std::pair<std::array<int, 3>, std::uint8_t>;
  const Key key{flop_class, spr_bucket};

  // Fast path: check the cache under a read lock.
  {
    std::shared_lock<std::shared_mutex> lock(cache_mutex_);
    auto it = cache_->entries.find(key);
    if (it != cache_->entries.end()) {
      cache_hits_.fetch_add(1, std::memory_order_relaxed);
      it->second->last_access.store(
          cache_->access_counter.fetch_add(1, std::memory_order_relaxed) + 1,
          std::memory_order_relaxed);
      return it->second;
    }
  }

  cache_misses_.fetch_add(1, std::memory_order_relaxed);

  // Load the policy from disk (outside any lock — this is the slow path).
  const std::string fname = solver::policy_file_name(flop_class, spr_bucket);
  const std::filesystem::path policy_path = policy_dir_ / fname;

  const poker::GameDef def = make_postflop_def(flop_class, spr_bucket);
  const abstraction::ActionAbstraction abstraction = make_postflop_abstraction(spr_bucket);

  solver::NSeatPolicy policy =
      solver::load_nseat_policy(policy_path, def, abstraction.id(), spr_bucket);

  // Build or load the tree. Use the same limits as the oracle to ensure
  // identical tree structure. If a serialized tree cache file (.atf) exists,
  // load it (~100 ms) instead of building from scratch (~5 s).
  tree::TreeLimits limits;
  limits.max_bytes = std::size_t{4} << 30;  // 4 GiB
  limits.max_nodes = 5'000'000;

  std::unique_ptr<tree::AbstractTree> tree_ptr;
  const std::string tree_fname = tree::tree_file_name(flop_class, spr_bucket);
  const std::filesystem::path tree_path = policy_dir_ / tree_fname;
  if (std::filesystem::exists(tree_path)) {
    tree_ptr = std::make_unique<tree::AbstractTree>(
        tree::AbstractTree::load(tree_path, def, abstraction, limits));
  } else {
    tree_ptr = std::make_unique<tree::AbstractTree>(def, abstraction, limits);
  }

  auto config = std::make_shared<CachedConfig>();
  config->policy = std::move(policy);
  config->tree = std::move(tree_ptr);

  // Insert under a write lock. Double-check: another thread may have loaded
  // the same config while we were doing disk I/O.
  std::unique_lock<std::shared_mutex> lock(cache_mutex_);
  auto it = cache_->entries.find(key);
  if (it != cache_->entries.end()) {
    // Another thread won the race — discard our freshly loaded config and
    // use theirs.
    it->second->last_access.store(
        cache_->access_counter.fetch_add(1, std::memory_order_relaxed) + 1,
        std::memory_order_relaxed);
    return it->second;
  }

  // Evict the least-recently-used entry if the cache is full.
  if (cache_->entries.size() >= cache_capacity_) {
    auto lru = cache_->entries.begin();
    for (auto cit = cache_->entries.begin(); cit != cache_->entries.end(); ++cit) {
      if (cit->second->last_access.load(std::memory_order_relaxed) <
          lru->second->last_access.load(std::memory_order_relaxed))
        lru = cit;
    }
    cache_->entries.erase(lru);
  }

  config->last_access.store(cache_->access_counter.fetch_add(1, std::memory_order_relaxed) + 1,
                            std::memory_order_relaxed);
  auto [inserted_it, inserted] = cache_->entries.try_emplace(key, std::move(config));
  return inserted_it->second;
}

std::vector<double> OnTheFlyPostflopEvaluator::evaluate(
    std::span<const int> flop, std::span<const std::array<int, 2>> hands,
    const bs::tree::TerminalPayload& ledger) const {
  const std::size_t n = hands.size();
  if (n < 2 || n > poker::kMaxUnifiedSeats)
    throw std::invalid_argument("OnTheFlyPostflopEvaluator: seat count out of range");
  if (flop.size() != 3)
    throw std::invalid_argument("OnTheFlyPostflopEvaluator: flop must have 3 cards");
  if (ledger.player_count != n)
    throw std::invalid_argument("OnTheFlyPostflopEvaluator: ledger player_count mismatch");

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
    throw std::invalid_argument("OnTheFlyPostflopEvaluator: pot is zero");

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
    throw std::invalid_argument("OnTheFlyPostflopEvaluator: no live seats");

  // 4. Derive SPR and map to bucket.
  const double spr = static_cast<double>(eff_stack) / static_cast<double>(pot);

  // All-in (zero SPR): no postflop decisions remain. Exact all-in equity is
  // the equilibrium value, not a heuristic approximation.
  if (!std::isfinite(spr) || spr <= 0.0) {
    ++equity_fallbacks_;
    return equity_fallback_.evaluate(flop, hands, ledger);
  }

  const std::size_t bucket = SprBucketConfig::bucket_for_spr(spr);

  // 5. Try the declared table first (fast path).
  try {
    const std::vector<double>& stored =
        table_.lookup(canon.board, static_cast<std::uint8_t>(bucket),
                      std::span<const std::array<int, 2>>(relabeled.data(), n));
    ++table_hits_;

    // 6. Scale from representative pot to actual pot.
    const double rep_pot = table_.metadata().representative_pot[bucket];
    if (!(rep_pot > 0.0))
      throw std::invalid_argument("OnTheFlyPostflopEvaluator: representative pot not set");
    const double scale = static_cast<double>(pot) / rep_pot;

    std::vector<double> values(n);
    for (std::size_t s = 0; s < n; ++s)
      values[s] = stored[s] * scale;
    return values;
  } catch (const std::invalid_argument&) {
    // Missing entry — fall through to on-the-fly computation.
  }

  // On-the-fly computation is only implemented for heads-up (2 seats).
  // Multiway policy persistence is a non-goal (RFC 0014).
  if (n != 2)
    throw std::invalid_argument(
        "OnTheFlyPostflopEvaluator: on-the-fly miss requires 2 seats (got " + std::to_string(n) +
        ")");

  // 7. On-the-fly: load policy, rebuild tree, extract root value.
  auto config = get_config(canon.board, static_cast<std::uint8_t>(bucket));

  const poker::GameState root_state(config->policy.game());
  const std::array<std::array<int, 2>, 2> deal_arr = {relabeled[0], relabeled[1]};
  const std::vector<double> extracted =
      extract_root_value(config->policy, *config->tree, root_state,
                         std::span<const std::array<int, 2>>(deal_arr.data(), 2));

  ++computed_;

  // 8. Scale from representative pot to actual pot.
  const double rep_pot = table_.metadata().representative_pot[bucket];
  if (!(rep_pot > 0.0))
    throw std::invalid_argument("OnTheFlyPostflopEvaluator: representative pot not set");
  const double scale = static_cast<double>(pot) / rep_pot;

  std::vector<double> values(n);
  for (std::size_t s = 0; s < n; ++s)
    values[s] = extracted[s] * scale;
  return values;
}

}  // namespace bs::gto
