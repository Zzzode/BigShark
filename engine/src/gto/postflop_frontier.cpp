// RFC 0013 Stage 1: postflop-aware frontier evaluation (implementation).

#include <algorithm>
#include <bs/abstract_tree.hpp>
#include <bs/abstraction.hpp>
#include <bs/game_definition.hpp>
#include <bs/nseat_policy_codec.hpp>
#include <bs/postflop_config.hpp>
#include <bs/postflop_frontier.hpp>
#include <bs/root_value_extractor.hpp>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <future>
#include <mutex>
#include <semaphore>
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

namespace {

using Clock = std::chrono::steady_clock;

std::uint64_t elapsed_ns(Clock::time_point start) {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count());
}

// Local lifetime ledger. Charges survive eviction until the last evaluation
// releases its policy. Load reservations share this exact same budget.
struct PolicyMemory {
  explicit PolicyMemory(std::size_t cap) : limit(cap) {}
  std::mutex mutex;
  std::condition_variable changed;
  const std::size_t limit;
  std::size_t bytes = 0;
  std::size_t peak = 0;
  std::size_t generation = 0;

  bool reserve(std::size_t charge) {
    std::lock_guard lock(mutex);
    if (charge > limit - bytes)
      return false;
    bytes += charge;
    peak = std::max(peak, bytes);
    return true;
  }
  void release(std::size_t charge) {
    {
      std::lock_guard lock(mutex);
      bytes -= charge;
      ++generation;
    }
    changed.notify_all();
  }
  void published() {
    {
      std::lock_guard lock(mutex);
      ++generation;
    }
    changed.notify_all();
  }
};

struct Reservation {
  std::shared_ptr<PolicyMemory> memory;
  std::size_t charge = 0;
  ~Reservation() {
    if (charge)
      memory->release(charge);
  }
  void retain(std::size_t bytes) {
    if (bytes > charge)
      throw std::runtime_error("postflop cache: decoded policy exceeded its reservation");
    memory->release(charge - bytes);
    charge = 0;  // The published policy now owns the remaining charge.
  }
};

struct LoadSlot {
  explicit LoadSlot(std::counting_semaphore<>& slots) : slots(slots) { slots.acquire(); }
  ~LoadSlot() { slots.release(); }
  std::counting_semaphore<>& slots;
};

struct PolicyCharge {
  std::shared_ptr<PolicyMemory> memory;
  std::size_t bytes = 0;
  ~PolicyCharge() {
    if (bytes)
      memory->release(bytes);
  }
};

}  // namespace

struct OnTheFlyPostflopEvaluator::CachedConfig {
  // Destroyed last, after the policy's allocations have actually been freed.
  PolicyCharge charge;
  solver::NSeatPolicy policy;
  std::shared_ptr<const PostflopEvaluationTemplate> evaluation;
  mutable std::atomic<std::size_t> last_access{0};
};

struct OnTheFlyPostflopEvaluator::Cache {
  using Key = std::pair<std::array<int, 3>, std::uint8_t>;
  using Result = std::shared_ptr<const CachedConfig>;
  explicit Cache(const PostflopCacheLimits& limits)
      : memory(std::make_shared<PolicyMemory>(limits.max_policy_bytes)),
        load_slots(static_cast<std::ptrdiff_t>(limits.max_concurrent_loads)) {}

  std::map<Key, Result> entries;
  std::map<Key, std::shared_future<Result>> flights;
  std::atomic<std::size_t> access_counter{0};
  std::shared_ptr<PolicyMemory> memory;
  std::counting_semaphore<> load_slots;
  std::mutex template_mutex;
  std::array<std::shared_ptr<const PostflopEvaluationTemplate>, SprBucketConfig::kBucketCount>
      templates{};
  std::size_t template_bytes = 0;
  std::size_t template_loads = 0;
  std::atomic<std::size_t> policy_loads{0};
  std::atomic<std::uint64_t> policy_load_ns{0};
  std::atomic<std::uint64_t> template_load_ns{0};
  std::atomic<std::uint64_t> extract_ns{0};
};

OnTheFlyPostflopEvaluator::OnTheFlyPostflopEvaluator(const NSeatDeclaredFrontierTable& table,
                                                     std::filesystem::path policy_dir,
                                                     std::size_t cache_capacity,
                                                     PostflopCacheLimits limits)
    : table_(table),
      policy_dir_(std::move(policy_dir)),
      cache_capacity_(cache_capacity),
      limits_(limits) {
  if (cache_capacity == 0 || limits.max_load_bytes <= sizeof(CachedConfig) + 256 ||
      limits.max_policy_bytes < limits.max_load_bytes || limits.max_concurrent_loads == 0 ||
      limits.max_concurrent_loads > static_cast<std::size_t>(std::counting_semaphore<>::max()) ||
      limits.max_template_bytes <= sizeof(PostflopEvaluationTemplate) + sizeof(tree::AbstractTree))
    throw std::invalid_argument("postflop cache: invalid entry, memory, or loading limits");
  cache_ = std::make_unique<Cache>(limits);
}

OnTheFlyPostflopEvaluator::~OnTheFlyPostflopEvaluator() = default;

std::shared_ptr<const PostflopEvaluationTemplate> OnTheFlyPostflopEvaluator::get_template(
    std::uint8_t bucket) const {
  std::lock_guard lock(cache_->template_mutex);
  if (auto existing = cache_->templates.at(bucket))
    return existing;

  const auto start = Clock::now();
  constexpr std::array<int, 3> representative{0, 1, 2};
  const auto def = make_postflop_def(representative, bucket);
  const auto abstraction = make_postflop_abstraction(bucket);
  const auto overhead = sizeof(PostflopEvaluationTemplate) + sizeof(tree::AbstractTree);
  const auto remaining = limits_.max_template_bytes - cache_->template_bytes;
  if (remaining <= overhead)
    throw tree::tree_resource_exhausted("postflop templates exceeded the aggregate byte cap");
  tree::TreeLimits limits;
  limits.max_nodes = 5'000'000;
  limits.max_bytes = remaining - overhead;
  const auto path = policy_dir_ / tree::tree_file_name(representative, bucket);
  auto tree = std::filesystem::exists(path)
                  ? tree::AbstractTree::load(path, def, abstraction, limits)
                  : tree::AbstractTree(def, abstraction, limits);
  auto evaluation = std::make_shared<const PostflopEvaluationTemplate>(std::move(tree));
  const auto charge = evaluation->accounted_bytes();
  if (charge > remaining)
    throw tree::tree_resource_exhausted("postflop template accounting exceeded its reservation");
  cache_->template_bytes += charge;
  ++cache_->template_loads;
  cache_->template_load_ns.fetch_add(elapsed_ns(start), std::memory_order_relaxed);
  cache_->templates[bucket] = evaluation;
  return evaluation;
}

PostflopCacheStatistics OnTheFlyPostflopEvaluator::statistics() const {
  PostflopCacheStatistics result;
  {
    std::lock_guard lock(cache_->memory->mutex);
    result.policy_bytes = cache_->memory->bytes;
    result.peak_policy_bytes = cache_->memory->peak;
  }
  {
    std::lock_guard lock(cache_->template_mutex);
    result.template_bytes = cache_->template_bytes;
    result.template_loads = cache_->template_loads;
  }
  result.policy_loads = cache_->policy_loads.load(std::memory_order_relaxed);
  result.policy_load_ms = cache_->policy_load_ns.load(std::memory_order_relaxed) / 1e6;
  result.template_load_ms = cache_->template_load_ns.load(std::memory_order_relaxed) / 1e6;
  result.extract_ms = cache_->extract_ns.load(std::memory_order_relaxed) / 1e6;
  return result;
}

std::shared_ptr<const OnTheFlyPostflopEvaluator::CachedConfig>
OnTheFlyPostflopEvaluator::get_config(const std::array<int, 3>& flop_class,
                                      std::uint8_t spr_bucket) const {
  const Cache::Key key{flop_class, spr_bucket};
  std::shared_future<Cache::Result> flight;
  std::shared_ptr<std::promise<Cache::Result>> promise;
  {
    std::unique_lock lock(cache_mutex_);
    if (auto found = cache_->entries.find(key); found != cache_->entries.end()) {
      ++cache_hits_;
      found->second->last_access.store(++cache_->access_counter, std::memory_order_relaxed);
      return found->second;
    }
    ++cache_misses_;
    if (auto pending = cache_->flights.find(key); pending != cache_->flights.end()) {
      flight = pending->second;
    } else {
      promise = std::make_shared<std::promise<Cache::Result>>();
      flight = promise->get_future().share();
      cache_->flights.emplace(key, flight);
    }
  }
  if (!promise)
    return flight.get();

  try {
    LoadSlot slot(cache_->load_slots);
    Reservation reservation{cache_->memory};
    // Evict retained entries until a load can reserve its worst-case bytes.
    // Active users keep their own charge; wait without holding the cache lock.
    for (;;) {
      std::unique_lock lock(cache_mutex_);
      if (cache_->memory->reserve(limits_.max_load_bytes)) {
        reservation.charge = limits_.max_load_bytes;
        break;
      }
      if (!cache_->entries.empty()) {
        const auto lru = std::min_element(
            cache_->entries.begin(), cache_->entries.end(), [](const auto& a, const auto& b) {
              return a.second->last_access.load(std::memory_order_relaxed) <
                     b.second->last_access.load(std::memory_order_relaxed);
            });
        cache_->entries.erase(lru);
        continue;
      }
      std::unique_lock budget_lock(cache_->memory->mutex);
      // Snapshot while the entry mutex still excludes insertion. A released
      // reservation can turn into a cached (evictable) policy before the full
      // reservation fits, so publication also wakes this outer eviction loop.
      const auto generation = cache_->memory->generation;
      lock.unlock();
      cache_->memory->changed.wait(budget_lock, [&] {
        return limits_.max_load_bytes <= cache_->memory->limit - cache_->memory->bytes ||
               cache_->memory->generation != generation;
      });
    }

    const auto def = make_postflop_def(flop_class, spr_bucket);
    const auto abstraction = make_postflop_abstraction(spr_bucket);
    const auto start = Clock::now();
    auto policy = solver::load_nseat_policy(
        policy_dir_ / solver::policy_file_name(flop_class, spr_bucket), def, abstraction.id(),
        spr_bucket, limits_.max_load_bytes - sizeof(CachedConfig) - 256);
    cache_->policy_load_ns.fetch_add(elapsed_ns(start), std::memory_order_relaxed);
    ++cache_->policy_loads;
    auto evaluation = get_template(spr_bucket);
    if (!evaluation->matches(policy))
      throw std::runtime_error("postflop cache: policy/template identity mismatch");
    for (const auto& [key, row] : policy.rows()) {
      if (key.node_index >= evaluation->size())
        throw std::runtime_error("postflop cache: policy node outside template");
      const auto node = evaluation->node(key.node_index);
      if (node.kind != tree::NodeKind::Action || row.actions.size() != node.actions.size() ||
          !std::equal(row.actions.begin(), row.actions.end(), node.actions.begin()))
        throw std::runtime_error("postflop cache: policy menu differs from template");
    }
    auto config = std::make_shared<CachedConfig>();
    config->policy = std::move(policy);
    config->evaluation = std::move(evaluation);
    config->charge.memory = cache_->memory;
    const auto policy_bytes = solver::nseat_policy_accounted_bytes(config->policy);
    if (policy_bytes > limits_.max_load_bytes - sizeof(CachedConfig) - 256)
      throw std::runtime_error("postflop cache: policy accounting exceeded load cap");
    const auto charge = policy_bytes + sizeof(CachedConfig) + 256;
    reservation.retain(charge);
    config->charge.bytes = charge;

    {
      std::unique_lock lock(cache_mutex_);
      if (cache_->entries.size() >= cache_capacity_) {
        const auto lru = std::min_element(
            cache_->entries.begin(), cache_->entries.end(), [](const auto& a, const auto& b) {
              return a.second->last_access.load(std::memory_order_relaxed) <
                     b.second->last_access.load(std::memory_order_relaxed);
            });
        cache_->entries.erase(lru);
      }
      config->last_access.store(++cache_->access_counter, std::memory_order_relaxed);
      cache_->entries.emplace(key, config);
      cache_->memory->published();
      promise->set_value(config);
      cache_->flights.erase(key);
    }
    return config;
  } catch (...) {
    std::unique_lock lock(cache_mutex_);
    promise->set_exception(std::current_exception());
    cache_->flights.erase(key);
    throw;
  }
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
  const auto extract_start = Clock::now();
  const std::vector<double> extracted =
      extract_root_value(config->policy, *config->evaluation, root_state,
                         std::span<const std::array<int, 2>>(deal_arr.data(), 2));

  cache_->extract_ns.fetch_add(elapsed_ns(extract_start), std::memory_order_relaxed);
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
