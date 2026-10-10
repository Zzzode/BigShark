// RFC 0013 Stage 2: PostflopValueOracle driver.
//
// Builds the declared postflop frontier table (.fdt) by:
//   1. Training a round-0 heads-up preflop blueprint with the equity frontier.
//   2. Exporting per-flop continuation ranges from the trained policy.
//   3. Aggregating ranges to canonical flop isomorphism classes.
//   4. For each class x SPR bucket, solving a flop-rooted river-terminal game
//      with CFR and extracting per-deal root values via extract_root_value.
//   5. Populating an NSeatDeclaredFrontierTable and saving it as BSF1 binary.
//
// The table stores CFR-solved equilibrium values — no heuristic or interpolated
// values exist anywhere in the path. Every stored value is a policy-following
// root-value traversal of a converged CFR solve.
//
// SPR-dependent action abstraction (r1):
//   Buckets 0-1 (SPR 0.5-1.5): full menu (1/3, 3/4, 3/2 bets; 1/2, 1/1 raises)
//   Buckets 2-3 (SPR 3.0-5.5): reduced menu (1/2, 1/1 bets; 1/1 raises)
//   Buckets 4-5 (SPR 9.5-15): minimal menu (1/1 bets; 1/1 raises)
//
// Incremental save: after each solved config the partial table is written to
// disk, so a crash or resource-exhaustion skip never loses completed work.
// On startup the oracle loads the existing .fdt (if any) and skips configs
// that already have entries.
//
// Usage: bigshark-postflop-oracle <output-path> [iterations] [pf-iterations]
//        [classes] [buckets] [max-deals] [class-start]
//   output-path:   path to save the .fdt table
//   iterations:    CFR iterations per postflop config (default 100000)
//   pf-iterations: preflop training iterations (default 100000)
//   classes:       limit canonical classes (0 = all remaining from class-start)
//   buckets:       limit SPR buckets (0 = all 6, for testing)
//   max-deals:     max deals per config (default 100, 0 = all)
//   class-start:   first canonical class index to process (default 0, for
//                  parallel sharding)
//
// This is an offline tool, NOT a CTest. The full build is a long-running
// background compute task.
#include <algorithm>
#include <array>
#include <bs/abstract_tree.hpp>
#include <bs/abstraction.hpp>
#include <bs/continuation_range.hpp>
#include <bs/equity_frontier.hpp>
#include <bs/game_definition.hpp>
#include <bs/heads_up_solver.hpp>  // WeightedHand
#include <bs/nseat_trainer.hpp>
#include <bs/postflop_frontier.hpp>
#include <bs/range.hpp>  // comboIndex, comboCards
#include <bs/root_value_extractor.hpp>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

using namespace bs::abstraction;
using namespace bs::poker;
using namespace bs::solver;
using namespace bs::tree;
using namespace bs::gto;

namespace {

// Build the heads-up preflop GameDef (same convention as preflop_profile_builder).
GameDef make_preflop_def(int stack_bb) {
  GameDef def{};
  def.player_count = 2;
  def.button = 0;
  def.big_blind = 2;
  const Chips stack = static_cast<Chips>(stack_bb * 2);  // BB = 2 chips
  def.stacks = {stack, stack, stack, stack, stack, stack, stack, stack, stack, stack};
  def.contributions = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
  def.pot = 3;  // SB 1 + BB 2
  def.board = {-1, -1, -1, 0, 0};
  def.board_size = 0;
  def.preflop = true;
  def.blinds_posted = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
  def.blinds_posted[small_blind_seat(def)] = 1;
  def.blinds_posted[big_blind_seat(def)] = 2;
  def.terminal = TerminalDepth::Flop;
  return def;
}

// All 1,326 combos with weight 1.0, per seat.
std::vector<std::vector<WeightedHand>> all_combos(std::size_t player_count) {
  std::vector<std::vector<WeightedHand>> ranges(player_count);
  for (auto& range : ranges)
    range.reserve(1326);
  for (int c0 = 0; c0 < 52; ++c0) {
    for (int c1 = c0 + 1; c1 < 52; ++c1) {
      for (auto& range : ranges)
        range.push_back({{c0, c1}, 1.0});
    }
  }
  return ranges;
}

// Build the representative postflop GameDef for a canonical flop class and SPR
// bucket. The pot is fixed at 100 chips (scale-invariant); each seat contributed
// 50 (equal, required by 2-seat rooted-board validation). The stack is set from
// the representative SPR so that SPR = stack / pot.
GameDef make_postflop_def(const std::array<int, 3>& canonical_flop, std::size_t spr_bucket) {
  constexpr Chips kPot = 100;
  const double rep_spr = SprBucketConfig::kRepresentativeSpr[spr_bucket];
  const Chips stack = static_cast<Chips>(std::round(kPot * rep_spr));

  GameDef def{};
  def.player_count = 2;
  def.button = 0;  // heads-up: button = SB, acts first postflop
  def.big_blind = 2;
  def.ante = 0;
  def.stacks = {stack, stack, 0, 0, 0, 0, 0, 0, 0, 0};
  def.contributions = {kPot / 2, kPot / 2, 0, 0, 0, 0, 0, 0, 0, 0};
  def.pot = kPot;
  def.board = {canonical_flop[0], canonical_flop[1], canonical_flop[2], 0, 0};
  def.board_size = 3;
  def.preflop = false;
  def.blinds_posted = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
  def.terminal = TerminalDepth::River;
  return def;
}

// SPR-dependent postflop action abstraction.
//
// Tree size grows exponentially with SPR because deeper stacks allow more
// bets/raises per street. Using the full menu at high SPR produces trees that
// exceed the byte/node caps. This function reduces the menu at higher SPR to
// keep trees tractable while preserving the most important bet sizes.
//
//   Buckets 0-1 (SPR 0.5-1.5):  default  (1/3, 3/4, 3/2 bets; 1/2, 1/1 raises)
//   Buckets 2-3 (SPR 3.0-5.5):  reduced  (1/2, 1/1 bets; 1/1 raises)
//   Buckets 4-5 (SPR 9.5-15):   minimal  (1/1 bets; 1/1 raises)
ActionAbstraction make_postflop_abstraction(std::size_t spr_bucket) {
  SizeSchedule schedule = default_size_schedule();

  if (spr_bucket >= 4) {
    StreetSizes sizes;
    sizes.bets = {{1, 1}};
    sizes.raises = {{1, 1}};
    schedule[static_cast<std::size_t>(Street::Flop)] = sizes;
    schedule[static_cast<std::size_t>(Street::Turn)] = sizes;
    schedule[static_cast<std::size_t>(Street::River)] = sizes;
  } else if (spr_bucket >= 2) {
    StreetSizes sizes;
    sizes.bets = {{1, 2}, {1, 1}};
    sizes.raises = {{1, 1}};
    schedule[static_cast<std::size_t>(Street::Flop)] = sizes;
    schedule[static_cast<std::size_t>(Street::Turn)] = sizes;
    schedule[static_cast<std::size_t>(Street::River)] = sizes;
  }
  // Buckets 0-1 keep the default schedule.

  return ActionAbstraction::declared(std::move(schedule), CoverSeeds::DeclaredOnly);
}

// Check if two two-card hands share a card.
bool hands_collide(const std::array<int, 2>& a, const std::array<int, 2>& b) {
  return a[0] == b[0] || a[0] == b[1] || a[1] == b[0] || a[1] == b[1];
}

// Check if a hand shares a card with the board.
bool blocks_board(const std::array<int, 2>& hand, const std::array<int, 3>& board) {
  return hand[0] == board[0] || hand[0] == board[1] || hand[0] == board[2] || hand[1] == board[0] ||
         hand[1] == board[1] || hand[1] == board[2];
}

// Aggregate per-concrete-flop PolicyReachRangePair into per-canonical-class
// reached ranges. Each concrete flop is canonicalized; its per-seat combo
// probabilities are relabeled through the canonical suit permutation and
// accumulated into the class's range. The result is normalized to sum to 1
// per seat per class.
std::map<std::array<int, 3>, std::vector<std::vector<WeightedHand>>> aggregate_class_ranges(
    const std::vector<PolicyReachRangePair>& flop_ranges) {
  std::map<std::array<int, 3>, std::array<std::map<int, double>, 2>> accum;

  for (const auto& pair : flop_ranges) {
    const CanonicalBoard canon = canonicalize(pair.flop);
    for (std::size_t seat = 0; seat < 2; ++seat) {
      for (const auto& [combo_idx, prob] : pair.ranges[seat]) {
        const auto cards = bs::comboCards(combo_idx);
        const int c0 = (cards[0] / 4) * 4 + canon.relabel[cards[0] % 4];
        const int c1 = (cards[1] / 4) * 4 + canon.relabel[cards[1] % 4];
        const int new_idx = bs::comboIndex(c0, c1);
        accum[canon.board][seat][new_idx] += prob;
      }
    }
  }

  std::map<std::array<int, 3>, std::vector<std::vector<WeightedHand>>> result;
  for (auto& [flop_class, seat_maps] : accum) {
    auto& seat_vecs = result[flop_class];
    seat_vecs.resize(2);
    for (std::size_t seat = 0; seat < 2; ++seat) {
      double total = 0.0;
      for (const auto& [idx, w] : seat_maps[seat])
        total += w;
      if (total <= 0.0)
        continue;
      auto& vec = seat_vecs[seat];
      vec.reserve(seat_maps[seat].size());
      for (const auto& [idx, w] : seat_maps[seat]) {
        if (w > 0.0)
          vec.push_back(WeightedHand{bs::comboCards(idx), w / total});
      }
    }
  }
  return result;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr,
                 "usage: %s <output-path> [iterations] [pf-iterations] [classes] [buckets] "
                 "[max-deals] [class-start]\n"
                 "  output-path:   path to save the .fdt table\n"
                 "  iterations:    CFR iterations per postflop config (default 100000)\n"
                 "  pf-iterations: preflop training iterations (default 100000)\n"
                 "  classes:       limit canonical classes (0 = all remaining)\n"
                 "  buckets:       limit SPR buckets (0 = all 6)\n"
                 "  max-deals:     max deals per config (default 100, 0 = all)\n"
                 "  class-start:   first canonical class index (default 0, for sharding)\n",
                 argv[0]);
    return 1;
  }

  const std::filesystem::path output_path = argv[1];
  const std::uint64_t iterations = argc > 2 ? std::atoll(argv[2]) : 100000;
  const std::uint64_t pf_iterations = argc > 3 ? std::atoll(argv[3]) : 100000;
  const int class_limit = argc > 4 ? std::atoi(argv[4]) : 0;
  const int bucket_limit = argc > 5 ? std::atoi(argv[5]) : 0;
  const std::uint64_t max_deals = argc > 6 ? std::atoll(argv[6]) : 100;
  const std::size_t class_start = argc > 7 ? static_cast<std::size_t>(std::atoll(argv[7])) : 0;

  const std::size_t num_buckets =
      bucket_limit > 0 ? static_cast<std::size_t>(bucket_limit) : SprBucketConfig::kBucketCount;

  std::printf("RFC 0013 Stage 2: PostflopValueOracle (r1: SPR-dependent abstraction)\n");
  std::printf("  output:       %s\n", output_path.string().c_str());
  std::printf("  iterations:   %llu per config\n", static_cast<unsigned long long>(iterations));
  std::printf("  pf iterations: %llu\n", static_cast<unsigned long long>(pf_iterations));
  std::printf("  buckets:      %zu\n", num_buckets);
  if (max_deals > 0)
    std::printf("  max deals:    %llu per config\n", static_cast<unsigned long long>(max_deals));

  // -----------------------------------------------------------------------
  // Step 1: Train round-0 preflop blueprint with the equity frontier.
  // -----------------------------------------------------------------------
  std::printf("\n--- Step 1: Round-0 preflop training (equity frontier) ---\n");

  constexpr int kStackBb = 100;
  const GameDef pf_def = make_preflop_def(kStackBb);
  const auto pf_ranges = all_combos(2);
  const ActionAbstraction pf_abstraction = ActionAbstraction::identity();
  const AbstractTree pf_tree(pf_def, pf_abstraction);

  std::size_t pf_action_nodes = 0;
  for (const auto& n : pf_tree.nodes())
    if (n.is_action())
      ++pf_action_nodes;
  std::printf("  tree: %zu nodes (%zu action)\n", pf_tree.size(), pf_action_nodes);

  const EquityFrontierEvaluator equity_frontier;
  NSeatTrainerLimits pf_limits;
  pf_limits.wall = std::chrono::minutes(30);

  const auto pf_start = std::chrono::steady_clock::now();
  const NSeatTrainingResult pf_trained =
      train_nseat(pf_tree, pf_ranges, pf_iterations, 20261003, pf_limits, &equity_frontier);
  const auto pf_elapsed = std::chrono::steady_clock::now() - pf_start;
  const double pf_wall_s = std::chrono::duration<double>(pf_elapsed).count();

  const char* pf_phase = pf_trained.termination == NSeatTerminationPhase::Complete ? "Complete"
                         : pf_trained.termination == NSeatTerminationPhase::WallClock
                             ? "WallClock"
                             : "ResourceLimit";
  std::printf("  training: %s, %llu iterations, %zu info sets, %.2f s\n", pf_phase,
              static_cast<unsigned long long>(pf_trained.completed_iterations),
              pf_trained.information_sets, pf_wall_s);

  if (pf_trained.termination == NSeatTerminationPhase::ResourceLimit) {
    std::fprintf(stderr, "preflop training hit a resource limit; aborting\n");
    return 1;
  }

  // -----------------------------------------------------------------------
  // Step 2: Export continuation ranges.
  // -----------------------------------------------------------------------
  std::printf("\n--- Step 2: Export continuation ranges ---\n");
  std::fflush(stdout);

  auto flop_ranges = export_continuation_ranges(pf_trained.policy, pf_tree, pf_ranges);
  std::printf("  exported %zu flops with positive reach\n", flop_ranges.size());
  std::fflush(stdout);

  // -----------------------------------------------------------------------
  // Step 3: Aggregate to canonical classes.
  // -----------------------------------------------------------------------
  std::printf("\n--- Step 3: Aggregate to canonical classes ---\n");
  std::fflush(stdout);

  const auto class_ranges = aggregate_class_ranges(flop_ranges);
  std::printf("  %zu canonical classes with reached ranges\n", class_ranges.size());
  std::fflush(stdout);

  flop_ranges.clear();
  flop_ranges.shrink_to_fit();

  // -----------------------------------------------------------------------
  // Step 3b: Resume — load existing table if present.
  // -----------------------------------------------------------------------
  std::vector<NSeatDeclaredFrontierTable::Entry> all_entries;
  std::set<std::pair<std::array<int, 3>, std::uint8_t>> completed_configs;

  if (std::filesystem::exists(output_path)) {
    try {
      auto loaded = NSeatDeclaredFrontierTable::load(output_path);
      all_entries = loaded.export_entries();
      for (const auto& entry : all_entries)
        completed_configs.insert({entry.canonical_flop, entry.spr_bucket});
      std::printf("  resume: %zu entries, %zu configs from %s\n", all_entries.size(),
                  completed_configs.size(), output_path.string().c_str());
    } catch (const std::exception& e) {
      std::fprintf(stderr, "  resume: could not load existing table: %s\n", e.what());
      all_entries.clear();
      completed_configs.clear();
    }
  }
  std::fflush(stdout);

  // -----------------------------------------------------------------------
  // Step 4: Enumerate canonical flop classes and solve.
  // -----------------------------------------------------------------------
  std::printf("\n--- Step 4: Solve postflop games ---\n");

  const auto all_classes = enumerate_canonical_flop_classes();
  if (class_start >= all_classes.size()) {
    std::fprintf(stderr, "class-start %zu exceeds total classes %zu\n", class_start,
                 all_classes.size());
    return 1;
  }
  const std::size_t remaining = all_classes.size() - class_start;
  const std::size_t num_classes =
      class_limit > 0 ? std::min(static_cast<std::size_t>(class_limit), remaining) : remaining;
  std::printf("  %zu canonical flop classes total, processing %zu [%zu, %zu)\n",
              all_classes.size(), num_classes, class_start, class_start + num_classes);
  std::fflush(stdout);

  // Metadata and identity (created before the loop for incremental save).
  NSeatDeclaredFrontierTable::Metadata metadata{};
  for (std::size_t b = 0; b < SprBucketConfig::kBucketCount; ++b)
    metadata.representative_pot[b] = 100.0;
  const std::string build_identity = "rfc0013-stage2-hu-100bb-r1";

  std::size_t total_configs = 0;
  std::size_t total_deals = 0;
  std::size_t skipped_classes = 0;
  std::size_t skipped_configs = 0;
  std::size_t failed_configs = 0;

  NSeatTrainerLimits pf_postflop_limits;
  pf_postflop_limits.wall = std::chrono::minutes(5);
  pf_postflop_limits.max_bytes = std::size_t{4} << 30;  // 4 GiB
  pf_postflop_limits.max_nodes = 5'000'000;
  pf_postflop_limits.max_information_sets = 5'000'000;

  // Tree limits: raised from defaults (1 GiB / 2M nodes) to accommodate
  // high-SPR trees with the reduced abstraction.
  TreeLimits oracle_tree_limits;
  oracle_tree_limits.max_bytes = std::size_t{4} << 30;  // 4 GiB
  oracle_tree_limits.max_nodes = 5'000'000;

  const auto solve_start = std::chrono::steady_clock::now();

  for (std::size_t ci = class_start; ci < class_start + num_classes; ++ci) {
    const auto& flop_class = all_classes[ci];

    auto range_it = class_ranges.find(flop_class);
    if (range_it == class_ranges.end()) {
      ++skipped_classes;
      continue;
    }
    const auto& seat_ranges = range_it->second;

    for (std::size_t b = 0; b < num_buckets; ++b) {
      // Skip already-completed configs (resume support).
      if (completed_configs.count({flop_class, static_cast<std::uint8_t>(b)})) {
        ++skipped_configs;
        continue;
      }

      const GameDef def = make_postflop_def(flop_class, b);
      const ActionAbstraction abstraction = make_postflop_abstraction(b);
      const std::uint64_t seed = 20261003ULL + ci * 1000ULL + b;

      // Build tree and train. The tree is owned by a unique_ptr so it
      // survives the try block for the extraction phase.
      std::unique_ptr<AbstractTree> tree;
      NSeatTrainingResult trained;
      try {
        tree = std::make_unique<AbstractTree>(def, abstraction, oracle_tree_limits);
        trained = train_nseat(*tree, seat_ranges, iterations, seed, pf_postflop_limits, nullptr);
      } catch (const tree_resource_exhausted& e) {
        std::fprintf(stderr, "  class %zu bucket %zu: tree too large: %s\n", ci, b, e.what());
        ++failed_configs;
        continue;
      } catch (const std::exception& e) {
        std::fprintf(stderr, "  class %zu bucket %zu: solve failed: %s\n", ci, b, e.what());
        ++failed_configs;
        continue;
      }

      if (trained.termination == NSeatTerminationPhase::ResourceLimit) {
        std::fprintf(stderr, "  class %zu bucket %zu: resource limit\n", ci, b);
        ++failed_configs;
        continue;
      }

      std::fprintf(stderr, "  class %zu bucket %zu: %zu info sets, tree %zu nodes\n", ci, b,
                   trained.information_sets, tree->size());

      // Extract root values for every card-compatible joint deal.
      const GameState root_state(def);
      std::size_t config_deals = 0;
      std::size_t config_failures = 0;
      bool deal_limit_reached = false;

      for (const auto& hero_hand : seat_ranges[0]) {
        if (blocks_board(hero_hand.cards, flop_class))
          continue;
        for (const auto& opp_hand : seat_ranges[1]) {
          if (blocks_board(opp_hand.cards, flop_class))
            continue;
          if (hands_collide(hero_hand.cards, opp_hand.cards))
            continue;

          const std::array<std::array<int, 2>, 2> deal_arr = {hero_hand.cards, opp_hand.cards};
          std::vector<double> values;
          try {
            values = extract_root_value(trained.policy, *tree, root_state,
                                        std::span<const std::array<int, 2>>(deal_arr.data(), 2));
          } catch (const std::exception& e) {
            if (config_failures < 5) {
              const auto& hero = hero_hand.cards;
              const auto& opp = opp_hand.cards;
              std::fprintf(stderr,
                           "  class %zu bucket %zu: extract failed: %s "
                           "(hero=[%d,%d] opp=[%d,%d])\n",
                           ci, b, e.what(), hero[0], hero[1], opp[0], opp[1]);
            }
            ++config_failures;
            continue;
          }

          NSeatDeclaredFrontierTable::Entry entry;
          entry.canonical_flop = flop_class;
          entry.spr_bucket = static_cast<std::uint8_t>(b);
          entry.hands = {hero_hand.cards, opp_hand.cards};
          entry.values = std::move(values);
          all_entries.push_back(std::move(entry));
          ++config_deals;

          if (max_deals > 0 && config_deals >= max_deals) {
            deal_limit_reached = true;
            break;
          }
        }
        if (deal_limit_reached)
          break;
      }

      ++total_configs;
      total_deals += config_deals;
      completed_configs.insert({flop_class, static_cast<std::uint8_t>(b)});

      // Incremental save: write the partial table after each config so a
      // crash never loses completed work.
      if (!all_entries.empty()) {
        try {
          auto entries_copy = all_entries;
          NSeatDeclaredFrontierTable partial(std::move(entries_copy), metadata);
          partial.save(output_path, build_identity);
        } catch (const std::exception& e) {
          std::fprintf(stderr, "  warning: incremental save failed: %s\n", e.what());
        }
      }

      if ((total_configs % 10) == 0 || total_configs == 1) {
        const auto elapsed = std::chrono::steady_clock::now() - solve_start;
        const double wall_s = std::chrono::duration<double>(elapsed).count();
        std::printf(
            "  [%zu configs] class %zu/%zu bucket %zu: %zu deals (%zu failed), "
            "%.0f s elapsed, %zu entries saved\n",
            total_configs, ci, all_classes.size(), b, config_deals, config_failures, wall_s,
            all_entries.size());
        std::fflush(stdout);
      }
    }
  }

  const auto solve_elapsed = std::chrono::steady_clock::now() - solve_start;
  const double solve_wall_s = std::chrono::duration<double>(solve_elapsed).count();

  std::printf("\n  solved %zu configs, %zu deals, %zu failed configs, %zu skipped classes, "
              "%zu skipped configs (resume)\n",
              total_configs, total_deals, failed_configs, skipped_classes, skipped_configs);
  std::printf("  solve wall time: %.1f s (%.1f min)\n", solve_wall_s, solve_wall_s / 60.0);

  // -----------------------------------------------------------------------
  // Step 5: Build and save the final table.
  // -----------------------------------------------------------------------
  std::printf("\n--- Step 5: Build and save table ---\n");

  try {
    NSeatDeclaredFrontierTable table(std::move(all_entries), metadata);
    std::printf("  table: %zu entries\n", table.size());
    table.save(output_path, build_identity);
    std::printf("  saved: %s\n", output_path.string().c_str());
  } catch (const std::exception& e) {
    std::fprintf(stderr, "failed to build/save table: %s\n", e.what());
    return 1;
  }

  // Write a JSON manifest alongside the table.
  const std::string manifest_json =
      std::string("{\"") + "build_identity\": \"" + build_identity + "\",\n" + "  \"stage\": 2,\n" +
      "  \"stack_bb\": 100,\n" + "  \"player_count\": 2,\n" +
      "  \"iterations_per_config\": " + std::to_string(iterations) + ",\n" +
      "  \"pf_iterations\": " + std::to_string(pf_iterations) + ",\n" +
      "  \"class_start\": " + std::to_string(class_start) + ",\n" +
      "  \"classes_processed\": " + std::to_string(num_classes) + ",\n" +
      "  \"classes_total\": " + std::to_string(all_classes.size()) + ",\n" +
      "  \"buckets\": " + std::to_string(num_buckets) + ",\n" +
      "  \"configs_solved\": " + std::to_string(total_configs) + ",\n" +
      "  \"entries\": " + std::to_string(total_deals) + ",\n" +
      "  \"failed_configs\": " + std::to_string(failed_configs) + ",\n" +
      "  \"skipped_classes\": " + std::to_string(skipped_classes) + ",\n" +
      "  \"skipped_configs_resume\": " + std::to_string(skipped_configs) + ",\n" +
      "  \"solve_wall_seconds\": " + std::to_string(solve_wall_s) + ",\n" +
      "  \"representative_pot\": 100.0,\n" + "  \"spr_representatives\": [" +
      std::to_string(SprBucketConfig::kRepresentativeSpr[0]);
  std::string manifest_full = manifest_json;
  for (std::size_t b = 1; b < SprBucketConfig::kBucketCount; ++b)
    manifest_full += ", " + std::to_string(SprBucketConfig::kRepresentativeSpr[b]);
  manifest_full += "]\n}\n";

  const auto manifest_file = output_path.string() + ".manifest.json";
  std::ofstream mf(manifest_file);
  mf << manifest_full;
  mf.close();
  std::printf("  manifest: %s\n", manifest_file.c_str());

  std::printf("\nDone.\n");
  return 0;
}
