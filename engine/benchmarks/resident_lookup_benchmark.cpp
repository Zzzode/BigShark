// RFC 0005 Stage 6 resident lookup latency benchmark (manual target).
//
// Two measurements are produced, both from deterministically published
// COMPLETE full-traversal policies (one full-traversal iteration visits the
// complete information-set tree with an all-zero-regret uniform policy;
// average weights normalize to the published probabilities exactly, so the
// exports validate). This is a latency fixture, not a quality claim.
//
//  1. Budget gate evidence on the frozen RFC 0004 Stage 4 SPR-10 dry
//     asymmetric fixture (Ks7h2c, 100/200 behind, 10-chip pot, three weighted
//     combos per side with one cross-blocked pair, fixed 3s/5h runout): the
//     matrix-measured 493,500 information sets. The root is loaded and
//     measured but must NOT be advertised under the 256 MiB default budget.
//
//  2. Warm lookup latency on an AGGREGATE of six complete supported roots
//     using the same three-combo ranges and SPR-4 stack profile (the frozen
//     matrix fixture depth, 19,176 sets per root) on six pairwise distinct
//     flops that never collide with the range cards. The aggregate totals at
//     least 100,000 resident information sets and fits the budget honestly.
//     A large warm batch mixes hero-decision hits across every root with
//     every coverage-miss kind; p50/p95/p99 are sort-based, timing uses
//     steady_clock, and a global allocation counter MUST stay zero across the
//     warm batch.
//
// The runner is NOT a CTest and does not fail the build if p99 misses the
// 10 ms promotion target; it reports the measured number as blocking
// promotion.
#include <algorithm>
#include <array>
#include <atomic>
#include <bs/game_definition.hpp>
#include <bs/heads_up.hpp>
#include <bs/heads_up_solver.hpp>
#include <bs/range.hpp>
#include <bs/resident_policy.hpp>
#include <bs/strategy_artifact.hpp>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <map>
#include <new>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#if defined(__APPLE__) || defined(__linux__)
#include <sys/resource.h>
#endif

#include "artifacts/artifact_internal.hpp"
#include "gto/heads_up_solver_debug.hpp"

using namespace bs::poker;
using namespace bs::solver;
using namespace bs::artifacts;
using namespace bs::resident;

namespace fs = std::filesystem;

namespace {

using bs::artifacts::detail::PolicyAssembler;

std::atomic<std::uint64_t> g_alloc_count{0};
std::atomic<std::uint64_t> g_alloc_bytes{0};

int card_id(const char* name) {
  return bs::cardId(std::string(name));
}

std::int64_t peak_rss_bytes() {
#if defined(__APPLE__) || defined(__linux__)
  struct rusage usage{};
  if (getrusage(RUSAGE_SELF, &usage) != 0)
    return -1;
#if defined(__APPLE__)
  return static_cast<std::int64_t>(usage.ru_maxrss);
#else
  return static_cast<std::int64_t>(usage.ru_maxrss) * 1024;
#endif
#else
  return -1;
#endif
}

TrainingLimits bench_limits() {
  TrainingLimits limits;
  limits.max_nodes = 5000000000ULL;
  limits.max_information_sets = 20000000;
  limits.max_bytes = std::size_t{8} << 30;
  limits.time = std::chrono::minutes{30};
  return limits;
}

// The shared frozen matrix ranges: three weighted combos per side, with the
// AcAd versus AcAs cross-blocked pair.
HeadsUpGame base_game(std::array<int, 3> flop, std::array<Chips, 2> stacks,
                      std::array<std::optional<int>, 2> runout) {
  HeadsUpGame game;
  game.root = {flop, stacks, {5, 5}, 10, 5, 1};
  game.ranges[0] = {{{card_id("Ac"), card_id("Ad")}, 2},
                    {{card_id("8c"), card_id("8d")}, 3},
                    {{card_id("Kc"), card_id("Kd")}, 4}};
  game.ranges[1] = {{{card_id("Ac"), card_id("As")}, 5},
                    {{card_id("Tc"), card_id("Td")}, 7},
                    {{card_id("Ah"), card_id("Kh")}, 6}};
  game.fixed_runout = runout;
  return game;
}

HeadsUpGame canonicalize(HeadsUpGame game) {
  for (auto& range : game.ranges) {
    double maximum = 0;
    for (auto& hand : range) {
      std::sort(hand.cards.begin(), hand.cards.end());
      maximum = std::max(maximum, hand.weight);
    }
    for (auto& hand : range)
      hand.weight /= maximum;
    std::sort(range.begin(), range.end(),
              [](const auto& a, const auto& b) { return a.cards < b.cards; });
  }
  return game;
}

struct Published {
  fs::path path;
  HeadsUpGame game;
  Sha256Digest digest{};
  std::string sha256_hex;
  std::uint64_t file_bytes = 0;
  std::size_t information_sets = 0;
};

Published publish_complete(HeadsUpGame game, std::uint64_t iterations, const fs::path& dir,
                           const std::string& name) {
  const DebugTrainingOutput trained =
      HeadsUpSolverDebug::train_full(game, iterations, bench_limits());
  if (trained.result.status != TrainingStatus::Complete) {
    std::fprintf(stderr, "training did not complete for %s\n", name.c_str());
    std::exit(2);
  }
  TrainingRows rows;
  for (const auto& [key, row] : trained.rows)
    rows.emplace(key, TrainingRow{row.actions, row.regrets, row.sums});
  const fs::path checkpoint = dir / (name + "-checkpoint.db");
  const fs::path policy = dir / (name + "-policy.db");
  CheckpointProvenance provenance;
  provenance.prng_identifier = kFullTraversalPrngIdentifier;
  provenance.engine_revision = "resident-benchmark-1";
  create_checkpoint(checkpoint, trained.result, rows, provenance);
  const PublishedPolicy out = publish_policy(checkpoint, policy, "resident-benchmark-1");
  return {policy,         std::move(game), out.sha256,
          out.sha256_hex, out.file_bytes,  trained.result.information_sets};
}

// Small fixed-runout two-vs-two game with a forced zero-call row facing a jam.
Published publish_zero_prob(const fs::path& dir) {
  HeadsUpGame source;
  source.root = {{card_id("2c"), card_id("3d"), card_id("7h")}, {2, 2}, {1, 1}, 2, 1, 1};
  source.ranges[0] = {{{card_id("As"), card_id("Ks")}, 2}, {{card_id("Qh"), card_id("Jh")}, 3}};
  source.ranges[1] = {{{card_id("Ac"), card_id("Kc")}, 5}, {{card_id("Qd"), card_id("Jd")}, 7}};
  source.fixed_runout = {card_id("9h"), card_id("8s")};
  source = canonicalize(source);

  const DebugTrainingOutput trained = HeadsUpSolverDebug::train_full(source, 1, bench_limits());
  const HeadsUpState root(source.root);
  const HeadsUpState after_jam = root.after_action(0, {ActionType::Bet, 2});
  static const std::array<std::array<int, 2>, 2> jam_combos = {
      {{card_id("Ac"), card_id("Kc")}, {card_id("Qd"), card_id("Jd")}}};
  TrainingResult result = trained.result;
  TrainingRows rows;
  // Start from an EMPTY policy (not trained.result.policy): add_row emplaces,
  // so reusing the populated policy would silently keep the original rows and
  // disagree with the forced raw average weights.
  HeadsUpPolicy forced_policy;
  PolicyAssembler::set_game(forced_policy, source);
  for (const auto& [key, old_row] : trained.rows) {
    const auto trained_it = trained.result.policy.rows().find(key);
    if (trained_it == trained.result.policy.rows().end())
      std::exit(3);
    PolicyRow policy_row{trained_it->second.actions, trained_it->second.probabilities};
    for (const auto& own : jam_combos)
      if (key == information_key(after_jam, own)) {
        policy_row.probabilities.assign(policy_row.actions.size(), 0.0);
        policy_row.probabilities[0] = 1.0;  // fold only
      }
    (void)old_row;
    PolicyAssembler::add_row(forced_policy, key, policy_row);
    rows.emplace(
        key, TrainingRow{policy_row.actions, policy_row.probabilities, policy_row.probabilities});
  }
  result.policy = std::move(forced_policy);
  const fs::path checkpoint = dir / "zero-checkpoint.db";
  const fs::path policy = dir / "zero-policy.db";
  CheckpointProvenance provenance;
  provenance.prng_identifier = kFullTraversalPrngIdentifier;
  provenance.engine_revision = "resident-benchmark-zero";
  create_checkpoint(checkpoint, result, rows, provenance);
  const PublishedPolicy out = publish_policy(checkpoint, policy, "resident-benchmark-zero");
  return {policy,         std::move(source), out.sha256,
          out.sha256_hex, out.file_bytes,    result.information_sets};
}

// Rebuilds the seat-generic GameState view of a flop-rooted HeadsUpState by
// replaying its observed public-action log. GameState(player_count == 2)
// reproduces HeadsUpState field for field, so the cursor and the source agree;
// this adapter exists only because the resident query path now speaks
// GameState + PublicAction history.
struct UnifiedNode {
  GameState state;
  std::vector<PublicAction> history;
};

UnifiedNode to_unified(const HeadsUpState& source) {
  GameDef def{};
  def.player_count = 2;
  def.button = source.root().button;
  def.big_blind = source.root().big_blind;
  def.stacks[0] = source.root().stacks[0];
  def.stacks[1] = source.root().stacks[1];
  def.contributions[0] = source.root().contributions[0];
  def.contributions[1] = source.root().contributions[1];
  def.pot = source.root().pot;
  def.board[0] = source.root().flop[0];
  def.board[1] = source.root().flop[1];
  def.board[2] = source.root().flop[2];
  def.board_size = 3;
  GameState cursor(def);
  std::vector<PublicAction> history;
  for (const BettingEvent& event : source.history()) {
    while (cursor.street() < event.street)
      cursor = cursor.after_card(source.board()[cursor.board().size()]);
    cursor = cursor.after_action(event.actor, event.action);
    history.push_back({event.street, event.actor, event.action});
  }
  while (cursor.board().size() < source.board().size())
    cursor = cursor.after_card(source.board()[cursor.board().size()]);
  return {std::move(cursor), std::move(history)};
}

struct Query {
  GameState state;
  std::vector<PublicAction> history;
  std::array<int, 2> hero;
  std::size_t root;
};

// Enumerate EVERY covered hero-decision query of one supported root: the
// fixed-runout DFS visits all public action nodes and adds one query per
// unblocked declared actor combination, so the query count equals the root's
// information-set count. Each root gets its own vector and seen set; a shared
// global cap must never make the first root truncate the others.
void collect_queries(const HeadsUpGame& game, std::size_t root, HeadsUpState state,
                     std::vector<Query>& queries, std::set<std::string>& seen) {
  if (state.phase() == Phase::Folded || state.phase() == Phase::Showdown)
    return;
  if (state.phase() == Phase::Deal) {
    const std::size_t slot = state.board().size() - 3;
    if (game.fixed_runout[slot]) {
      collect_queries(game, root, state.after_card(*game.fixed_runout[slot]), queries, seen);
      return;
    }
    std::array<bool, 52> used{};
    for (int board_card : state.board())
      used[board_card] = true;
    for (auto fixed : game.fixed_runout)
      if (fixed)
        used[*fixed] = true;
    for (int dealt = 0; dealt < 52; ++dealt)
      if (!used[dealt])
        collect_queries(game, root, state.after_card(dealt), queries, seen);
    return;
  }
  const std::size_t actor = *state.actor();
  std::array<int, 2> name_cards{};
  bool have_name = false;
  for (const WeightedHand& hand : game.ranges[actor]) {
    bool blocked = false;
    for (int board_card : state.board())
      if (board_card == hand.cards[0] || board_card == hand.cards[1])
        blocked = true;
    if (!blocked && !have_name) {
      name_cards = hand.cards;
      have_name = true;
    }
  }
  if (!have_name)
    return;
  const std::string name =
      encode_public_key(information_key(state, name_cards), kArtifactSchemaVersion);
  if (!seen.insert(name).second)
    return;
  const UnifiedNode node = to_unified(state);
  for (const WeightedHand& hand : game.ranges[actor]) {
    bool blocked = false;
    for (int board_card : state.board())
      if (board_card == hand.cards[0] || board_card == hand.cards[1])
        blocked = true;
    if (!blocked)
      queries.push_back({node.state, node.history, hand.cards, root});
  }
  for (const Action& action : abstract_actions(state, game.sizes))
    collect_queries(game, root, state.after_action(actor, action), queries, seen);
}

double percentile(std::vector<double>& values, double q) {
  const std::size_t index =
      std::min(values.size() - 1, static_cast<std::size_t>(q * values.size()));
  return values[index];
}

}  // namespace

// Allocation interposition for the zero-heap-allocation warm-path claim.
void* operator new(std::size_t size) {
  g_alloc_count.fetch_add(1, std::memory_order_relaxed);
  g_alloc_bytes.fetch_add(size, std::memory_order_relaxed);
  if (void* pointer = std::malloc(size))
    return pointer;
  throw std::bad_alloc();
}
void* operator new[](std::size_t size) {
  g_alloc_count.fetch_add(1, std::memory_order_relaxed);
  g_alloc_bytes.fetch_add(size, std::memory_order_relaxed);
  if (void* pointer = std::malloc(size))
    return pointer;
  throw std::bad_alloc();
}
void operator delete(void* pointer) noexcept {
  std::free(pointer);
}
void operator delete[](void* pointer) noexcept {
  std::free(pointer);
}
void operator delete(void* pointer, std::size_t) noexcept {
  std::free(pointer);
}
void operator delete[](void* pointer, std::size_t) noexcept {
  std::free(pointer);
}

int main() {
  using Clock = std::chrono::steady_clock;
  const fs::path dir = fs::temp_directory_path() / "bs-resident-benchmark";
  std::error_code ec;
  fs::remove_all(dir, ec);
  fs::create_directories(dir, ec);

  // --- 1. SPR-10 large-root evidence -------------------------------------
  // The 493,500-set fixture was over-budget (301 MiB) under the original
  // word-key/double layout. The compact layout reduces it to ~68 MiB, so it
  // now advertises under the 256 MiB default. The over-budget refusal path
  // is covered by the unit suite (budget_bytes=1 -> OverBudget).
  const HeadsUpGame spr10 = canonicalize(base_game({card_id("Ks"), card_id("7h"), card_id("2c")},
                                                   {100, 200}, {card_id("3s"), card_id("5h")}));
  const auto spr10_start = Clock::now();
  const Published spr10_pub = publish_complete(spr10, 4, dir, "spr10");
  const double spr10_train_ms =
      std::chrono::duration<double, std::milli>(Clock::now() - spr10_start).count();
  std::vector<RootLoadResult> spr10_results;
  ResidentPolicySet spr10_set = ResidentPolicySet::build({{spr10_pub.path, spr10_pub.digest}},
                                                         ResidentOptions{}, &spr10_results);
  if (spr10_results[0].status != RootStatus::Advertised) {
    std::fprintf(stderr, "expected SPR-10 advertised, got status %d bytes %zu\n",
                 static_cast<int>(spr10_results[0].status), spr10_results[0].resident_bytes);
    return 2;
  }
  std::printf("# SPR-10 single root (493500-set matrix fixture):\n");
  std::printf(
      "#   train_ms=%.3f file_bytes=%llu resident_bytes=%zu budget_bytes=%zu "
      "advertised=yes (fits under the compact layout)\n\n",
      spr10_train_ms, static_cast<unsigned long long>(spr10_pub.file_bytes),
      spr10_results[0].resident_bytes, static_cast<std::size_t>(kDefaultResidentBudgetBytes));

  // --- 2. Six-root SPR-4 aggregate for warm latency ----------------------
  // Pairwise distinct flops, none sharing a card with the declared ranges
  // (Ac Ad As 8c 8d Kc Kd Tc Td Ah Kh) and fixed 3s/5h runout.
  static const std::array<std::array<int, 3>, 6> flops = {{
      {{card_id("Ks"), card_id("7h"), card_id("2c")}},
      {{card_id("9s"), card_id("8s"), card_id("4h")}},
      {{card_id("Qd"), card_id("Qc"), card_id("6s")}},
      {{card_id("Js"), card_id("9d"), card_id("2h")}},
      {{card_id("7s"), card_id("6h"), card_id("5c")}},
      {{card_id("5s"), card_id("4s"), card_id("3h")}},
  }};

  std::vector<Published> roots;
  std::vector<SupportedRootSpec> specs;
  std::size_t total_sets = 0;
  const auto train_start = Clock::now();
  for (std::size_t i = 0; i < flops.size(); ++i) {
    HeadsUpGame game = canonicalize(base_game(flops[i], {40, 40}, {card_id("3s"), card_id("5h")}));
    Published published = publish_complete(std::move(game), 100, dir, "spr4-" + std::to_string(i));
    total_sets += published.information_sets;
    specs.push_back({published.path, published.digest});
    roots.push_back(std::move(published));
  }
  const double train_ms =
      std::chrono::duration<double, std::milli>(Clock::now() - train_start).count();
  if (total_sets < 100000) {
    std::fprintf(stderr, "aggregate resident sets %zu below 100000\n", total_sets);
    return 2;
  }

  std::vector<RootLoadResult> results;
  const auto cold_start = Clock::now();
  ResidentPolicySet residents = ResidentPolicySet::build(specs, ResidentOptions{}, &results);
  const auto cold_end = Clock::now();
  for (const RootLoadResult& result : results)
    if (result.status != RootStatus::Advertised) {
      std::fprintf(stderr, "aggregate root not advertised: %s (%s)\n", result.path.c_str(),
                   result.detail.c_str());
      return 2;
    }
  const double cold_ms = std::chrono::duration<double, std::milli>(cold_end - cold_start).count();
  const std::size_t resident_bytes = residents.total_resident_bytes();

  // Warm hit queries: EVERY covered hero decision of EVERY supported root,
  // enumerated into independent per-root vectors. The total must equal the
  // resident information-set count, so the timed batch touches the full
  // 115k+ working set rather than the first root.
  std::vector<std::vector<Query>> per_root_queries(roots.size());
  std::size_t distinct_nodes = 0;
  for (std::size_t i = 0; i < roots.size(); ++i) {
    std::set<std::string> seen;
    collect_queries(roots[i].game, i, HeadsUpState(roots[i].game.root), per_root_queries[i], seen);
    distinct_nodes += per_root_queries[i].size();
  }
  std::vector<Query> queries;
  queries.reserve(distinct_nodes);
  for (auto& root_queries : per_root_queries)
    for (Query& query : root_queries)
      queries.push_back(std::move(query));
  per_root_queries.clear();
  per_root_queries.shrink_to_fit();
  if (distinct_nodes != total_sets) {
    std::fprintf(stderr, "enumerated %zu queries but published %zu sets\n", distinct_nodes,
                 total_sets);
    return 2;
  }

  // Miss fixtures.
  const HeadsUpGame small = [] {
    HeadsUpGame game;
    game.root = {{card_id("2c"), card_id("3d"), card_id("7h")}, {2, 2}, {1, 1}, 2, 1, 1};
    game.ranges[0] = {{{card_id("As"), card_id("Ks")}, 2}, {{card_id("Qh"), card_id("Jh")}, 3}};
    game.ranges[1] = {{{card_id("Ac"), card_id("Kc")}, 5}, {{card_id("Qd"), card_id("Jd")}, 7}};
    game.fixed_runout = {card_id("9h"), card_id("8s")};
    return canonicalize(game);
  }();
  const Published zero_pub = publish_zero_prob(dir);
  std::vector<RootLoadResult> with_small_results;
  ResidentPolicySet with_small =
      ResidentPolicySet::build({{roots[0].path, roots[0].digest}, {zero_pub.path, zero_pub.digest}},
                               {}, &with_small_results);

  ResidentOptions starved_options;
  starved_options.budget_bytes = 1;
  std::vector<RootLoadResult> starved_results;
  ResidentPolicySet starved = ResidentPolicySet::build({{roots[0].path, roots[0].digest}},
                                                       starved_options, &starved_results);

  const HeadsUpState root0(roots[0].game.root);
  const HeadsUpState off_amount = root0.after_action(0, {ActionType::Bet, 7});
  HeadsUpState divergent =
      root0.after_action(0, {ActionType::Check}).after_action(1, {ActionType::Check});
  divergent = divergent.after_card(card_id("2h"));
  const HeadsUpState small_root(small.root);
  const HeadsUpState small_after_jam_call =
      small_root.after_action(0, {ActionType::Bet, 2}).after_action(1, {ActionType::Call});

  // Pre-built outside every counter region: the resident query path speaks
  // GameState + PublicAction history, and constructing either inside a timed
  // loop would allocate in the HARNESS rather than the resident lookup.
  const UnifiedNode root0_node = to_unified(root0);
  const UnifiedNode off_amount_node = to_unified(off_amount);
  const UnifiedNode divergent_node = to_unified(divergent);
  const UnifiedNode small_root_node = to_unified(small_root);
  const UnifiedNode small_after_jam_node = to_unified(small_after_jam_call);

  constexpr std::size_t kBatch = 100000;
  constexpr std::size_t kMissBatch = 20000;
  std::vector<double> hit_ns;
  std::vector<double> miss_ns;
  hit_ns.reserve(kBatch);
  miss_ns.reserve(kMissBatch);
  ResidentScratch scratch;
  // Pre-built outside every counter region: 64 hex chars exceed libc++ SSO,
  // so constructing it inside the miss loop would allocate in the HARNESS
  // rather than the resident lookup.
  const std::string bad_pin(64, 'a');
  const std::string_view good_pin(roots[0].sha256_hex);

  // Warmup: one full pass over the working set, untimed and outside every
  // counter region, so branch predictors and caches are hot before timing.
  {
    ResidentScratch pre;
    for (const Query& query : queries)
      if (!residents.hero_decision(query.state, query.history, query.hero, std::nullopt, pre).hit) {
        std::fprintf(stderr, "warmup uncovered query\n");
        return 2;
      }
  }

  // --- Hit batch; the resident contract requires zero allocations here. ---
  const std::uint64_t hit_allocs_before = g_alloc_count.load(std::memory_order_relaxed);
  const auto hit_start = Clock::now();
  for (std::size_t i = 0; i < kBatch; ++i) {
    const Query& query = queries[i % distinct_nodes];
    const auto t0 = Clock::now();
    const ResidentAnswer answer =
        residents.hero_decision(query.state, query.history, query.hero, std::nullopt, scratch);
    const auto t1 = Clock::now();
    if (!answer.hit) {
      std::fprintf(stderr, "expected covered hit at iteration %zu: %s\n", i,
                   to_string(answer.reason));
      return 2;
    }
    hit_ns.push_back(std::chrono::duration<double, std::nano>(t1 - t0).count());
  }
  const std::uint64_t hit_allocs_after = g_alloc_count.load(std::memory_order_relaxed);
  const double hit_ms = std::chrono::duration<double, std::milli>(Clock::now() - hit_start).count();
  const std::uint64_t hit_allocations = hit_allocs_after - hit_allocs_before;

  // --- Miss batch; likewise required to be allocation-free. ---------------
  const std::uint64_t miss_allocs_before = g_alloc_count.load(std::memory_order_relaxed);
  const auto miss_start = Clock::now();
  std::size_t unexpected_miss_hits = 0;
  for (std::size_t i = 0; i < kMissBatch; ++i) {
    const auto t0 = Clock::now();
    ResidentAnswer answer;
    switch (i % 9) {
      case 0:
        answer = residents.public_belief(small_root_node.state, small_root_node.history,
                                         std::nullopt, scratch);  // unsupported
        break;
      case 1:
        answer = residents.public_belief(small_root_node.state, small_root_node.history, good_pin,
                                         scratch);  // pin mismatch
        break;
      case 2:
        answer = starved.public_belief(root0_node.state, root0_node.history, std::nullopt,
                                       scratch);  // over budget
        break;
      case 3:
        answer = residents.public_belief(off_amount_node.state, off_amount_node.history,
                                         std::nullopt, scratch);  // off-tree amount
        break;
      case 4:
        answer = residents.hero_decision(root0_node.state, root0_node.history,
                                         {card_id("2h"), card_id("2d")}, std::nullopt,
                                         scratch);  // untrained combo
        break;
      case 5:
        answer = residents.hero_decision(root0_node.state, root0_node.history,
                                         {card_id("Ks"), card_id("As")}, std::nullopt,
                                         scratch);  // blocked by board
        break;
      case 6:
        answer = residents.public_belief(divergent_node.state, divergent_node.history, std::nullopt,
                                         scratch);  // runout
        break;
      case 7:
        answer = with_small.public_belief(small_after_jam_node.state, small_after_jam_node.history,
                                          std::nullopt, scratch);  // zero prob
        break;
      case 8:
        answer = residents.public_belief(small_root_node.state, small_root_node.history,
                                         std::string_view(bad_pin), scratch);  // bad pin
        break;
    }
    const auto t1 = Clock::now();
    if (answer.hit)
      ++unexpected_miss_hits;
    miss_ns.push_back(std::chrono::duration<double, std::nano>(t1 - t0).count());
  }
  const std::uint64_t miss_allocs_after = g_alloc_count.load(std::memory_order_relaxed);
  const double miss_ms =
      std::chrono::duration<double, std::milli>(Clock::now() - miss_start).count();
  const std::uint64_t miss_allocations = miss_allocs_after - miss_allocs_before;
  if (hit_allocations != 0 || miss_allocations != 0) {
    std::fprintf(stderr, "warm path allocated: hit_batch=%llu miss_batch=%llu (must be zero)\n",
                 static_cast<unsigned long long>(hit_allocations),
                 static_cast<unsigned long long>(miss_allocations));
    return 2;
  }

  std::sort(hit_ns.begin(), hit_ns.end());
  std::sort(miss_ns.begin(), miss_ns.end());
  const double p50 = percentile(hit_ns, 0.50) / 1000.0;
  const double p95 = percentile(hit_ns, 0.95) / 1000.0;
  const double p99 = percentile(hit_ns, 0.99) / 1000.0;
  const double miss_p99 = percentile(miss_ns, 0.99) / 1000.0;
  const bool target_met = p99 <= 10000.0;

  std::printf(
      "resident_lookup_version,roots,information_sets,distinct_nodes,train_ms,cold_ms,"
      "resident_bytes,budget_bytes,peak_rss_bytes,hit_calls,hit_ms,miss_calls,miss_ms,"
      "hit_allocations,miss_allocations,hit_p50_us,hit_p95_us,hit_p99_us,miss_p99_us,"
      "p99_target_met\n");
  std::printf(
      "1,%zu,%zu,%zu,%.3f,%.3f,%zu,%zu,%ld,%zu,%.3f,%zu,%.3f,%llu,%llu,%.3f,%.3f,%.3f,"
      "%.3f,%d\n",
      roots.size(), total_sets, distinct_nodes, train_ms, cold_ms, resident_bytes,
      static_cast<std::size_t>(kDefaultResidentBudgetBytes), static_cast<long>(peak_rss_bytes()),
      kBatch, hit_ms, kMissBatch, miss_ms, static_cast<unsigned long long>(hit_allocations),
      static_cast<unsigned long long>(miss_allocations), p50, p95, p99, miss_p99,
      target_met ? 1 : 0);
  std::printf("\naggregate roots=%zu information_sets=%zu (19176 per SPR-4 root x 6)\n",
              roots.size(), total_sets);
  std::printf("cold construction: %.3f ms; resident %.3f MiB of 256 MiB budget\n", cold_ms,
              resident_bytes / 1048576.0);
  std::printf("warm hits n=%zu  p50=%.3f us p95=%.3f us p99=%.3f us (target p99 <= 10000 us)\n",
              kBatch, p50, p95, p99);
  std::printf("warm misses n=%zu p99=%.3f us; unexpected miss hits=%zu\n", kMissBatch, miss_p99,
              unexpected_miss_hits);
  std::printf("allocations: hit batch %llu, miss batch %llu (contract: zero on a lookup)\n",
              static_cast<unsigned long long>(hit_allocations),
              static_cast<unsigned long long>(miss_allocations));
  std::printf("p99 target %s; a missed target blocks default promotion, not correctness.\n",
              target_met ? "MET" : "MISSED");
  return 0;
}
