// This file requires its own executable: allocation replacement is process-wide.
#include <algorithm>
#include <array>
#include <bs/heads_up.hpp>
#include <bs/heads_up_solver.hpp>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <limits>
#include <new>
#include <optional>
#include <stdexcept>
#include <utility>

#include "gto/heads_up_solver_debug.hpp"

namespace allocation {

constexpr auto no_failure = std::numeric_limits<std::size_t>::max();

struct Counts {
  std::size_t attempts = 0;
  std::size_t failures = 0;
  std::size_t live = 0;
  std::size_t peak = 0;
};

// No dynamically allocated tracking structures, including during unwinding.
struct alignas(std::max_align_t) Header {
  void* base;
  std::size_t bytes;
  std::size_t generation;
};

Counts counts;
std::size_t generation = 0;
std::size_t failure_index = no_failure;
bool active = false;

class Window {
 public:
  explicit Window(std::size_t index = no_failure) {
    ++generation;
    counts = {};
    failure_index = index;
    active = true;
  }
  Window(const Window&) = delete;
  Window& operator=(const Window&) = delete;
  ~Window() { stop(); }

  Counts stop() noexcept {
    active = false;
    failure_index = no_failure;
    return counts;
  }
};

void* allocate(std::size_t bytes, std::size_t alignment = alignof(std::max_align_t)) {
  if (active && counts.attempts++ == failure_index) {
    failure_index = no_failure;
    ++counts.failures;
    throw std::bad_alloc{};
  }
  alignment = std::max(alignment, alignof(Header));
  const auto offset = (sizeof(Header) + alignment - 1) / alignment * alignment;
  const auto payload = std::max(bytes, std::size_t{1});
  if (payload > std::numeric_limits<std::size_t>::max() - offset)
    throw std::bad_alloc{};
  void* base = nullptr;
  if (alignment <= alignof(std::max_align_t))
    base = std::malloc(offset + payload);
  else if (::posix_memalign(&base, alignment, offset + payload) != 0)
    throw std::bad_alloc{};
  if (!base)
    throw std::bad_alloc{};
  auto* result = static_cast<unsigned char*>(base) + offset;
  ::new (result - sizeof(Header)) Header{base, bytes, active ? generation : 0};
  if (active) {
    counts.live += bytes;
    counts.peak = std::max(counts.peak, counts.live);
  }
  return result;
}

void release(void* pointer) noexcept {
  if (!pointer)
    return;
  auto* header = reinterpret_cast<Header*>(static_cast<unsigned char*>(pointer) - sizeof(Header));
  if (header->generation != 0 && header->generation == generation)
    counts.live -= header->bytes;
  std::free(header->base);
}

}  // namespace allocation

void* operator new(std::size_t bytes) {
  return allocation::allocate(bytes);
}
void* operator new[](std::size_t bytes) {
  return allocation::allocate(bytes);
}
void* operator new(std::size_t bytes, std::align_val_t alignment) {
  return allocation::allocate(bytes, static_cast<std::size_t>(alignment));
}
void* operator new[](std::size_t bytes, std::align_val_t alignment) {
  return allocation::allocate(bytes, static_cast<std::size_t>(alignment));
}
void* operator new(std::size_t bytes, const std::nothrow_t&) noexcept {
  try {
    return allocation::allocate(bytes);
  } catch (...) {
    return nullptr;
  }
}
void* operator new[](std::size_t bytes, const std::nothrow_t& tag) noexcept {
  return ::operator new(bytes, tag);
}
void* operator new(std::size_t bytes, std::align_val_t alignment, const std::nothrow_t&) noexcept {
  try {
    return allocation::allocate(bytes, static_cast<std::size_t>(alignment));
  } catch (...) {
    return nullptr;
  }
}
void* operator new[](std::size_t bytes, std::align_val_t alignment,
                     const std::nothrow_t& tag) noexcept {
  return ::operator new(bytes, alignment, tag);
}
void operator delete(void* pointer) noexcept {
  allocation::release(pointer);
}
void operator delete[](void* pointer) noexcept {
  allocation::release(pointer);
}
void operator delete(void* pointer, std::size_t) noexcept {
  allocation::release(pointer);
}
void operator delete[](void* pointer, std::size_t) noexcept {
  allocation::release(pointer);
}
void operator delete(void* pointer, std::align_val_t) noexcept {
  allocation::release(pointer);
}
void operator delete[](void* pointer, std::align_val_t) noexcept {
  allocation::release(pointer);
}
void operator delete(void* pointer, std::size_t, std::align_val_t) noexcept {
  allocation::release(pointer);
}
void operator delete[](void* pointer, std::size_t, std::align_val_t) noexcept {
  allocation::release(pointer);
}
void operator delete(void* pointer, const std::nothrow_t&) noexcept {
  allocation::release(pointer);
}
void operator delete[](void* pointer, const std::nothrow_t&) noexcept {
  allocation::release(pointer);
}
void operator delete(void* pointer, std::align_val_t, const std::nothrow_t&) noexcept {
  allocation::release(pointer);
}
void operator delete[](void* pointer, std::align_val_t, const std::nothrow_t&) noexcept {
  allocation::release(pointer);
}

#define CHECK(condition)                                                         \
  do {                                                                           \
    if (!(condition)) {                                                          \
      std::printf("CHECK failed: %s (%s:%d)\n", #condition, __FILE__, __LINE__); \
      return 1;                                                                  \
    }                                                                            \
  } while (0)

namespace {

using namespace bs::poker;
using namespace bs::solver;

HeadsUpGame fixture() {
  HeadsUpGame game;
  game.root = {{0, 5, 22}, {1, 1}, {1, 1}, 2, 1, 1};
  game.ranges[0] = {{{48, 49}, 1}};
  game.ranges[1] = {{{40, 41}, 1}};
  game.fixed_runout = {39, 28};
  return game;
}

bool same_game(const HeadsUpGame& a, const HeadsUpGame& b) {
  if (a.root.flop != b.root.flop || a.root.stacks != b.root.stacks ||
      a.root.contributions != b.root.contributions || a.root.pot != b.root.pot ||
      a.root.big_blind != b.root.big_blind || a.root.button != b.root.button ||
      a.fixed_runout != b.fixed_runout)
    return false;
  for (std::size_t p = 0; p < 2; ++p) {
    if (a.ranges[p].size() != b.ranges[p].size())
      return false;
    for (std::size_t h = 0; h < a.ranges[p].size(); ++h)
      if (a.ranges[p][h].cards != b.ranges[p][h].cards ||
          a.ranges[p][h].weight != b.ranges[p][h].weight)
        return false;
  }
  for (std::size_t s = 0; s < a.sizes.size(); ++s) {
    for (bool raises : {false, true}) {
      const auto& first = raises ? a.sizes[s].raises : a.sizes[s].bets;
      const auto& second = raises ? b.sizes[s].raises : b.sizes[s].bets;
      if (first.size() != second.size())
        return false;
      for (std::size_t i = 0; i < first.size(); ++i)
        if (first[i].numerator != second[i].numerator ||
            first[i].denominator != second[i].denominator)
          return false;
    }
  }
  return true;
}

bool same_policy(const HeadsUpPolicy& a, const HeadsUpPolicy& b) {
  if (!same_game(a.game(), b.game()) || a.rows().size() != b.rows().size())
    return false;
  auto second = b.rows().begin();
  for (const auto& [key, row] : a.rows()) {
    if (key != second->first || row.actions != second->second.actions ||
        row.probabilities != second->second.probabilities)
      return false;
    ++second;
  }
  return true;
}

int test_allocator() {
  allocation::Window window;
  void* scalar = ::operator new(7);
  void* array = ::operator new[](13);
  void* aligned = ::operator new(17, std::align_val_t{64});
  void* aligned_array = ::operator new[](19, std::align_val_t{128});
  const bool alignment_ok = reinterpret_cast<std::uintptr_t>(aligned) % 64 == 0 &&
                            reinterpret_cast<std::uintptr_t>(aligned_array) % 128 == 0;
  ::operator delete(scalar);
  ::operator delete[](array);
  ::operator delete(aligned, std::align_val_t{64});
  ::operator delete[](aligned_array, std::align_val_t{128});
  const auto measured = window.stop();
  CHECK(alignment_ok);
  CHECK(measured.attempts == 4);
  CHECK(measured.peak == 56);
  CHECK(measured.live == 0);
  allocation::Window failure(0);
  void* failed = ::operator new[](1, std::align_val_t{64}, std::nothrow);
  void* recovered = ::operator new(0, std::nothrow);
  const bool recovered_ok = recovered != nullptr;
  ::operator delete[](failed, std::align_val_t{64});
  ::operator delete(recovered);
  const auto injected = failure.stop();
  CHECK(failed == nullptr);
  CHECK(recovered_ok);
  CHECK(injected.attempts == 2);
  CHECK(injected.failures == 1);
  CHECK(injected.live == 0);
  return 0;
}

int test_faults(std::uint64_t iterations) {
  const HeadsUpTrainer trainer(fixture());
  const auto one = trainer.train(1);
  CHECK(one.status == TrainingStatus::Complete);
  CHECK(one.completed_iterations == 1);
  CHECK(!one.policy.rows().empty());

  std::optional<TrainingResult> baseline;
  allocation::Window baseline_window;
  baseline.emplace(trainer.train(iterations));
  const auto baseline_counts = baseline_window.stop();
  CHECK(baseline->status == TrainingStatus::Complete);
  CHECK(baseline->completed_iterations == iterations);
  CHECK(baseline_counts.attempts > 0);
  // Exhaustive fault enumeration is quadratic; keep this a small native test.
  CHECK(baseline_counts.attempts < 10000);
  if (iterations == 2)
    CHECK(!same_policy(one.policy, baseline->policy));

  std::size_t escaped = 0;
  std::array<std::size_t, 2> rolled_back{};
  bool reached_complete = false;
  for (std::size_t index = 0; index <= baseline_counts.attempts; ++index) {
    std::optional<TrainingResult> result;
    bool bad_alloc_escaped = false;
    allocation::Window window(index);
    try {
      result.emplace(trainer.train(iterations));
    } catch (const std::bad_alloc&) {
      bad_alloc_escaped = true;
    }
    const auto measured = window.stop();
    if (bad_alloc_escaped) {
      if (escaped++ == 0)
        std::printf("train(%llu): bad_alloc escaped at allocation %zu\n",
                    static_cast<unsigned long long>(iterations), index);
      CHECK(measured.failures == 1);
      CHECK(measured.live == 0);
      continue;
    }
    CHECK(result.has_value());
    if (measured.failures == 0) {
      CHECK(index == baseline_counts.attempts);
      CHECK(measured.attempts == baseline_counts.attempts);
      CHECK(result->status == TrainingStatus::Complete);
      CHECK(result->completed_iterations == iterations);
      CHECK(result->information_sets == baseline->information_sets);
      CHECK(result->nodes == baseline->nodes);
      CHECK(result->accounted_bytes == baseline->accounted_bytes);
      CHECK(same_policy(result->policy, baseline->policy));
      reached_complete = true;
      break;
    }
    CHECK(measured.failures == 1);
    CHECK(result->status == TrainingStatus::ResourceLimit);
    CHECK(result->completed_iterations < iterations);
    CHECK(result->completed_iterations <= 1);
    ++rolled_back[result->completed_iterations];
    if (result->completed_iterations == 0) {
      CHECK(result->information_sets == 0);
      CHECK(result->policy.rows().empty());
    } else {
      CHECK(result->information_sets == one.information_sets);
      CHECK(same_policy(result->policy, one.policy));
    }
    result.reset();
    CHECK(allocation::counts.live == 0);
  }
  std::printf("train(%llu): allocations=%zu rollback0=%zu rollback1=%zu escaped=%zu\n",
              static_cast<unsigned long long>(iterations), baseline_counts.attempts, rolled_back[0],
              rolled_back[1], escaped);
  CHECK(reached_complete);
  CHECK(rolled_back[0] > 0);
  CHECK(iterations == 1 || rolled_back[1] > 0);
  CHECK(escaped == 0);
  return 0;
}

// Same exhaustive fault enumeration for the production sampled entry point.
// It never builds the debug raw-row table, so every injected failure must
// roll the iteration back; a post-commit ResourceLimit is impossible here.
int test_sampled_faults(std::uint64_t iterations, std::uint64_t seed) {
  const HeadsUpTrainer trainer(fixture());
  const auto one = trainer.train_sampled(1, seed);
  CHECK(one.status == TrainingStatus::Complete);
  CHECK(one.completed_iterations == 1);
  CHECK(!one.policy.rows().empty());
  CHECK(one.prng_state != seed);

  std::optional<TrainingResult> baseline;
  allocation::Window baseline_window;
  baseline.emplace(trainer.train_sampled(iterations, seed));
  const auto baseline_counts = baseline_window.stop();
  CHECK(baseline->status == TrainingStatus::Complete);
  CHECK(baseline->completed_iterations == iterations);
  CHECK(baseline_counts.attempts > 0);
  CHECK(baseline_counts.attempts < 10000);

  std::size_t escaped = 0;
  std::array<std::size_t, 2> rolled_back{};
  bool reached_complete = false;
  for (std::size_t index = 0; index <= baseline_counts.attempts; ++index) {
    std::optional<TrainingResult> result;
    bool bad_alloc_escaped = false;
    allocation::Window window(index);
    try {
      result.emplace(trainer.train_sampled(iterations, seed));
    } catch (const std::bad_alloc&) {
      bad_alloc_escaped = true;
    }
    const auto measured = window.stop();
    if (bad_alloc_escaped) {
      ++escaped;
      CHECK(measured.failures == 1);
      CHECK(measured.live == 0);
      continue;
    }
    CHECK(result.has_value());
    if (measured.failures == 0) {
      CHECK(index == baseline_counts.attempts);
      CHECK(measured.attempts == baseline_counts.attempts);
      CHECK(result->status == TrainingStatus::Complete);
      CHECK(result->completed_iterations == iterations);
      CHECK(result->information_sets == baseline->information_sets);
      CHECK(result->nodes == baseline->nodes);
      CHECK(result->accounted_bytes == baseline->accounted_bytes);
      CHECK(result->prng_state == baseline->prng_state);
      CHECK(same_policy(result->policy, baseline->policy));
      reached_complete = true;
      break;
    }
    CHECK(measured.failures == 1);
    CHECK(result->status == TrainingStatus::ResourceLimit);
    // No debug export exists on this path: the work must have rolled back.
    CHECK(result->completed_iterations < iterations);
    CHECK(result->completed_iterations <= 1);
    ++rolled_back[result->completed_iterations];
    if (result->completed_iterations == 0) {
      CHECK(result->information_sets == 0);
      CHECK(result->policy.rows().empty());
      CHECK(result->prng_state == seed);
    } else {
      CHECK(result->information_sets == one.information_sets);
      CHECK(same_policy(result->policy, one.policy));
      CHECK(result->prng_state == one.prng_state);
    }
    result.reset();
    CHECK(allocation::counts.live == 0);
  }
  std::printf("train_sampled(%llu): allocations=%zu rollback0=%zu rollback1=%zu escaped=%zu\n",
              static_cast<unsigned long long>(iterations), baseline_counts.attempts, rolled_back[0],
              rolled_back[1], escaped);
  CHECK(reached_complete);
  CHECK(rolled_back[0] > 0);
  CHECK(iterations == 1 || rolled_back[1] > 0);
  CHECK(escaped == 0);
  return 0;
}

// Exhaustive fault enumeration for the DEBUG sampled entry point, which
// additionally copies the committed table into DebugRow storage after the
// last iteration commits. A fault inside that export reports ResourceLimit
// with every iteration, the published policy, and the PRNG all committed;
// faults elsewhere roll back exactly as on the production path.
int test_sampled_debug_export_faults(std::uint64_t iterations, std::uint64_t seed) {
  const HeadsUpGame game = fixture();
  const auto one = HeadsUpSolverDebug::train_sampled(game, 1, seed);
  CHECK(one.result.status == TrainingStatus::Complete);
  CHECK(!one.rows.empty());

  std::optional<DebugTrainingOutput> baseline;
  allocation::Window baseline_window;
  baseline.emplace(HeadsUpSolverDebug::train_sampled(game, iterations, seed));
  const auto baseline_counts = baseline_window.stop();
  CHECK(baseline->result.status == TrainingStatus::Complete);
  CHECK(baseline->rows.size() == baseline->result.information_sets);

  std::size_t escaped = 0;
  std::size_t rollback_faults = 0;
  std::size_t export_faults = 0;
  bool reached_complete = false;
  for (std::size_t index = 0; index <= baseline_counts.attempts; ++index) {
    std::optional<DebugTrainingOutput> result;
    bool bad_alloc_escaped = false;
    allocation::Window window(index);
    try {
      result.emplace(HeadsUpSolverDebug::train_sampled(game, iterations, seed));
    } catch (const std::bad_alloc&) {
      bad_alloc_escaped = true;
    }
    const auto measured = window.stop();
    if (bad_alloc_escaped) {
      ++escaped;
      CHECK(measured.failures == 1);
      CHECK(measured.live == 0);
      continue;
    }
    CHECK(result.has_value());
    if (measured.failures == 0) {
      CHECK(result->result.status == TrainingStatus::Complete);
      CHECK(result->result.completed_iterations == iterations);
      CHECK(result->rows.size() == baseline->rows.size());
      CHECK(same_policy(result->result.policy, baseline->result.policy));
      reached_complete = true;
      break;
    }
    CHECK(measured.failures == 1);
    CHECK(result->result.status == TrainingStatus::ResourceLimit);
    if (result->result.completed_iterations == iterations) {
      // Debug-only export fault: all training work stays committed.
      ++export_faults;
      CHECK(result->result.information_sets == baseline->result.information_sets);
      CHECK(result->result.prng_state == baseline->result.prng_state);
      CHECK(same_policy(result->result.policy, baseline->result.policy));
    } else {
      ++rollback_faults;
      CHECK(result->result.completed_iterations < iterations);
      if (result->result.completed_iterations == 0) {
        CHECK(result->result.information_sets == 0);
        CHECK(result->result.policy.rows().empty());
        CHECK(result->result.prng_state == seed);
      } else {
        CHECK(result->result.information_sets == one.result.information_sets);
        CHECK(same_policy(result->result.policy, one.result.policy));
        CHECK(result->result.prng_state == one.result.prng_state);
      }
    }
    result.reset();
    CHECK(allocation::counts.live == 0);
  }
  std::printf("train_sampled_debug(%llu): export_faults=%zu rollback_faults=%zu escaped=%zu\n",
              static_cast<unsigned long long>(iterations), export_faults, rollback_faults, escaped);
  CHECK(reached_complete);
  CHECK(export_faults > 0);
  CHECK(rollback_faults > 0);
  CHECK(escaped == 0);
  return 0;
}

HeadsUpGame many_deals(int cards_per_range, bool overlap) {
  HeadsUpGame game;
  game.root = {{0, 1, 2}, {0, 0}, {1, 1}, 2, 1, 1};
  game.fixed_runout = {3, 4};
  for (std::size_t p = 0; p < 2; ++p) {
    const int start = 5 + (overlap ? 0 : static_cast<int>(p) * cards_per_range);
    for (int a = start; a < start + cards_per_range; ++a)
      for (int b = a + 1; b < start + cards_per_range; ++b)
        game.ranges[p].push_back({{a, b}, 1});
  }
  return game;
}

int test_joint_peak(bool overlap) {
  const auto game = many_deals(overlap ? 11 : 9, overlap);
  std::size_t compatible = 0;
  for (const auto& a : game.ranges[0])
    for (const auto& b : game.ranges[1])
      compatible += a.cards[0] != b.cards[0] && a.cards[0] != b.cards[1] &&
                    a.cards[1] != b.cards[0] && a.cards[1] != b.cards[1];
  CHECK(compatible > 1024);
  const auto pairs = game.ranges[0].size() * game.ranges[1].size();
  CHECK(!overlap || pairs > compatible);
  const HeadsUpTrainer trainer(game);
  std::optional<TrainingResult> baseline;
  allocation::Window window;
  baseline.emplace(trainer.train(0));
  const auto measured = window.stop();
  CHECK(baseline->status == TrainingStatus::Complete);
  CHECK(baseline->completed_iterations == 0);
  CHECK(baseline->policy.rows().empty());
  CHECK(baseline->nodes == pairs);
  // Count requested live payload, excluding this test allocator's own headers.
  CHECK(measured.peak <= baseline->accounted_bytes);
  CHECK(baseline->accounted_bytes < 256 * 1024);
  for (bool exact_fit : {false, true}) {
    TrainingLimits limits;
    limits.max_bytes = baseline->accounted_bytes - (exact_fit ? 0 : 1);
    std::optional<TrainingResult> result;
    allocation::Window bounded;
    result.emplace(trainer.train(0, limits));
    const auto usage = bounded.stop();
    CHECK(usage.peak <= limits.max_bytes);
    CHECK(result->accounted_bytes <= limits.max_bytes);
    CHECK(result->completed_iterations == 0);
    CHECK(result->information_sets == 0);
    CHECK(result->policy.rows().empty());
    CHECK(result->status == (exact_fit ? TrainingStatus::Complete : TrainingStatus::ResourceLimit));
    CHECK(result->nodes == (exact_fit ? pairs : 0));
    if (exact_fit)
      CHECK(same_policy(result->policy, baseline->policy));
    result.reset();
    CHECK(allocation::counts.live == 0);
  }
  std::printf("joint deals: pairs=%zu compatible=%zu peak=%zu accounted=%zu\n", pairs, compatible,
              measured.peak, baseline->accounted_bytes);
  return 0;
}

int test_evaluation_chance_cap() {
  auto game = many_deals(14, false);
  game.fixed_runout[0].reset();
  const auto pairs = game.ranges[0].size() * game.ranges[1].size();
  CHECK(pairs > 8192);
  CHECK(HeadsUpState(game.root).phase() == Phase::Deal);
  const HeadsUpTrainer trainer(game);
  // An all-in root needs no policy rows before entering public-card chance.
  const auto untrained = trainer.train(0);
  CHECK(untrained.status == TrainingStatus::Complete);
  CHECK(untrained.policy.rows().empty());
  TrainingLimits limits;
  limits.max_bytes = 512 * 1024;
  // A missing chance charge must still stop at the first child, not enumerate
  // millions of terminal showdowns; its excessive peak remains observable.
  limits.max_nodes = pairs + 1;
  CHECK(untrained.accounted_bytes + 2 * pairs * sizeof(double) + 8192 < limits.max_bytes);
  CHECK(52 * pairs * sizeof(double) > limits.max_bytes);
  bool resource_limited = false;
  allocation::Window window;
  try {
    (void)trainer.evaluate(untrained.policy, limits);
  } catch (const std::runtime_error& error) {
    resource_limited = std::strcmp(error.what(), "exact evaluation resource limit") == 0;
  }
  const auto measured = window.stop();
  CHECK(resource_limited);
  CHECK(measured.failures == 0);
  CHECK(measured.peak <= limits.max_bytes);
  CHECK(measured.live == 0);
  std::printf("evaluation chance: deals=%zu peak=%zu cap=%zu\n", pairs, measured.peak,
              limits.max_bytes);
  return 0;
}

}  // namespace

int main() {
  int failures = 0;
  try {
    CHECK(test_allocator() == 0);
    failures += test_faults(1);
    failures += test_faults(2);
    failures += test_sampled_faults(1, 17);
    failures += test_sampled_faults(2, 17);
    failures += test_sampled_debug_export_faults(1, 17);
    failures += test_sampled_debug_export_faults(2, 17);
    failures += test_joint_peak(false);
    failures += test_joint_peak(true);
    failures += test_evaluation_chance_cap();
  } catch (const std::exception& error) {
    std::printf("Unexpected allocation regression exception: %s\n", error.what());
    return 1;
  }
  if (failures != 0)
    return 1;
  std::printf("test_heads_up_allocations PASS\n");
  return 0;
}
