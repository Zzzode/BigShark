// RFC 0048 D243 native test: TurnGame constructor-failure resource
// qualification. This file requires its own executable because the allocation
// replacement is process-wide (same isolation convention as
// test_heads_up_allocations.cpp); no production allocator API, global hook, or
// management layer is added. A fault window throws std::bad_alloc at one
// chosen MAIN-THREAD allocation ordinal while the REAL TurnGame constructor
// runs its production map_leaves/precompute_folds/layout path, exercising
// partial-construction cleanup through genuine allocation failure. Worker
// threads never consume a fault ordinal: the probe dispatches no parallel
// region, so the parked pool is runtime noise and the construction's
// allocation order is the deterministic single-thread sequence. No
// substitute allocator inside production, no source/log matching, zero CFR
// iterations; LeakSanitizer audits process exit independently of the in-process
// live-byte accounting.
#include <algorithm>
#include <array>
#include <bs/eval.hpp>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <limits>
#include <new>
#include <stdexcept>
#include <thread>
#include <vector>

#include "../benchmarks/rfc0047/strata.hpp"
#include "../benchmarks/rfc0047/turn_solver.hpp"

#define CHECK(cond)                                                         \
  do {                                                                      \
    if (!(cond)) {                                                          \
      std::printf("CHECK failed: %s (%s:%d)\n", #cond, __FILE__, __LINE__); \
      return false;                                                         \
    }                                                                       \
  } while (0)

using namespace bs::rfc0047;

// Process-local fault injector, built on the established
// test_heads_up_allocations seam (malloc-backed operator new with a tracking
// header), with main-thread ordinal keying so parked pool workers cannot
// perturb the deterministic construction allocation sequence.
namespace allocation {

constexpr auto no_failure = std::numeric_limits<std::size_t>::max();

struct Counts {
  std::size_t attempts = 0;  // main-thread allocations only
  std::size_t failures = 0;
  std::size_t live = 0;
  std::size_t peak = 0;
};

struct alignas(std::max_align_t) Header {
  void* base;
  std::size_t bytes;
  std::size_t generation;
};

Counts counts;
std::size_t generation = 0;
std::size_t failure_index = no_failure;
bool active = false;
std::thread::id main_thread;

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
  const bool on_main = std::this_thread::get_id() == main_thread;
  if (active && on_main && counts.attempts++ == failure_index) {
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

namespace {

// Shallow-stack w1-shaped stratum: identical production constructor and
// layout() ownership code, small enough for deterministic ordinal sampling.
StratumSpec small_stratum() {
  const StratumSpec* w1 = find_stratum("w1");
  if (w1 == nullptr)
    throw std::runtime_error("w1 stratum missing");
  StratumSpec spec = *w1;
  spec.name = "d243-fault";
  spec.stack_behind = 8;
  spec.contribution = 2;
  spec.pot = 4;
  return spec;
}

TurnSolveConfig make_empty_leaf_config(const StratumSpec& spec) {
  // An empty leaf span is a real constructor boundary: map_leaves is a
  // no-op and layout() then allocates regret/sum tables for EVERY action
  // node (2410 here vs 2074 on the leaf-intercepted production shape), so
  // the exercised ownership loop is a strict superset of the production
  // table set with the identical allocation and cleanup code.
  TurnSolveConfig config;
  config.def = make_stratum_def(spec, 1);
  config.iterations = 0;
  config.linear_averaging = true;
  config.threads = 1;
  config.leaves = {};
  return config;
}

bool attempt_construction(const TurnSolveConfig& config, bool expect_throw) {
  bool threw = false;
  try {
    (void)turn_game_lifecycle_probe(config);
  } catch (const std::bad_alloc&) {
    threw = true;
  }
  CHECK(threw == expect_throw);
  return true;
}

// Baseline: unarmed construction is deterministic and leaves no live bytes.
bool test_baseline(const TurnSolveConfig& config, std::size_t& baseline_attempts,
                   std::size_t& action_nodes) {
  allocation::Window window;
  const TurnGameLifecycleInfo info = turn_game_lifecycle_probe(config);
  const allocation::Counts measured = window.stop();
  CHECK(measured.failures == 0);
  CHECK(measured.live == 0);
  CHECK(measured.attempts > 0);

  allocation::Window repeat;
  const TurnGameLifecycleInfo again = turn_game_lifecycle_probe(config);
  const allocation::Counts second = repeat.stop();
  CHECK(second.attempts == measured.attempts);
  CHECK(second.live == 0);
  CHECK(again.turn_action_nodes == info.turn_action_nodes);
  baseline_attempts = measured.attempts;
  action_nodes = info.turn_action_nodes;
  std::printf("D243 baseline: attempts=%zu action_nodes=%zu peak=%zu B\n", measured.attempts,
              action_nodes, measured.peak);
  return true;
}

// Deterministic ordinal bands rather than an infeasible exhaustive sweep of
// the ~6e4-alloc constructor: early Pool/tree failures, mid-construction
// container failures, the layout boundary, and a dense/strided sample across
// the reg/sum table loop and the scratch pool at the tail. Every faulted
// construction must release all partially allocated tables (live == 0).
std::vector<std::size_t> fault_ordinals(std::size_t total) {
  std::vector<std::size_t> ordinals;
  for (std::size_t i = 0; i < 5; ++i)
    ordinals.push_back(i);  // Pool and earliest tree allocations.
  ordinals.push_back(total / 4);
  ordinals.push_back(total / 2);
  ordinals.push_back(3 * total / 4);
  // Layout owns the final ~2*action_nodes table allocations plus ~100 scratch
  // and backing-array allocations; sample its boundary, deep interior, and the
  // dense scratch/final-table edge.
  for (std::size_t back :
       {120ULL, 200ULL, 400ULL, 800ULL, 1500ULL, 2500ULL, 3500ULL, 4500ULL, 5000ULL})
    if (back < total)
      ordinals.push_back(total - back);
  std::sort(ordinals.begin(), ordinals.end());
  ordinals.erase(std::unique(ordinals.begin(), ordinals.end()), ordinals.end());
  return ordinals;
}

bool test_fault_bands(const TurnSolveConfig& config, std::size_t baseline_attempts) {
  const std::vector<std::size_t> ordinals = fault_ordinals(baseline_attempts);
  std::size_t observed = 0;
  for (std::size_t index : ordinals) {
    CHECK(index < baseline_attempts);
    allocation::Window window(index);
    CHECK(attempt_construction(config, true));
    const allocation::Counts measured = window.stop();
    CHECK(measured.failures == 1);
    // The rollback contract: no partially constructed table outlives the
    // failed constructor. This is the RED detector for the layout() gap.
    CHECK(measured.live == 0);
    ++observed;
  }
  // One ordinal past the last allocation: a complete, clean construction.
  allocation::Window clean(baseline_attempts);
  CHECK(attempt_construction(config, false));
  const allocation::Counts measured = clean.stop();
  CHECK(measured.failures == 0);
  CHECK(measured.live == 0);
  std::printf("D243 bands: %zu faulted constructions, 1 clean, all live==0\n", observed);
  return true;
}

// Failure-retry and repeated init/destroy: after a fault the process must
// still construct and destroy repeatedly with identical allocation footprints
// and zero retained bytes across windows.
bool test_retry_and_repeat(const TurnSolveConfig& config, std::size_t baseline_attempts) {
  {
    allocation::Window window(baseline_attempts / 2);
    CHECK(attempt_construction(config, true));
    const allocation::Counts measured = window.stop();
    CHECK(measured.failures == 1);
    CHECK(measured.live == 0);
  }
  for (int round = 0; round < 3; ++round) {
    allocation::Window window;
    CHECK(attempt_construction(config, false));
    const allocation::Counts measured = window.stop();
    CHECK(measured.failures == 0);
    CHECK(measured.live == 0);
    CHECK(measured.attempts == baseline_attempts);
  }
  return true;
}

}  // namespace

int main() {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  allocation::main_thread = std::this_thread::get_id();
  const StratumSpec spec = small_stratum();
  const TurnSolveConfig config = make_empty_leaf_config(spec);
  std::size_t baseline_attempts = 0;
  std::size_t action_nodes = 0;
  if (!test_baseline(config, baseline_attempts, action_nodes) ||
      !test_fault_bands(config, baseline_attempts) ||
      !test_retry_and_repeat(config, baseline_attempts))
    return 1;
  // Structural identity: the empty-leaf path tables up every action node.
  CHECK(action_nodes >= 2000);
  std::printf("rfc0047_turn_construction_fault: all checks passed\n");
  return 0;
}
