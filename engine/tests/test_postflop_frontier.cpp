// RFC 0013 Stage 1: N-seat declared frontier table + PostflopFrontierEvaluator
// + SPR bucketing + root-value extraction cross-check.
//
// Gates:
//  1. SPR bucketing: boundary values map to the correct bucket.
//  2. N-seat table round-trip: store/load/lookup, missing-entry throw,
//     duplicate-key throw.
//  3. Evaluator scaling: stored values are scaled by
//     pot_actual / pot_representative.
//  4. Root-value cross-check: extract_root_value on a solved river-rooted
//     game agrees with stage6::exact_deal_utility within 1e-9 times the
//     root pot.
#include <algorithm>
#include <array>
#include <bs/abstract_tree.hpp>
#include <bs/abstraction.hpp>
#include <bs/behavior_policy.hpp>
#include <bs/eval.hpp>
#include <bs/game_definition.hpp>
#include <bs/nseat_policy_codec.hpp>
#include <bs/nseat_trainer.hpp>
#include <bs/postflop_config.hpp>
#include <bs/postflop_frontier.hpp>
#include <bs/root_value_extractor.hpp>
#include <bs/stage6/exact_oracle.hpp>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <vector>

#define CHECK(cond)                                                         \
  do {                                                                      \
    if (!(cond)) {                                                          \
      std::printf("CHECK failed: %s (%s:%d)\n", #cond, __FILE__, __LINE__); \
      return 1;                                                             \
    }                                                                       \
  } while (0)

namespace {

using namespace bs;
using namespace bs::gto;
using namespace bs::poker;
using namespace bs::solver;
using namespace bs::tree;
using namespace bs::stage6;

int card(const char* name) {
  return cardId(std::string(name));
}

// The table contract requires per-seat sorted hole cards.
std::array<int, 2> sorted_hand(const char* c1, const char* c2) {
  std::array<int, 2> h = {card(c1), card(c2)};
  std::sort(h.begin(), h.end());
  return h;
}

// ---------------------------------------------------------------------------
// 1. SPR bucketing
// ---------------------------------------------------------------------------

int test_spr_bucketing() {
  // Boundary values: (0,1], (1,2], (2,4], (4,7], (7,12], (12,inf]
  CHECK(SprBucketConfig::bucket_for_spr(0.5) == 0);
  CHECK(SprBucketConfig::bucket_for_spr(1.0) == 0);  // upper bound inclusive
  CHECK(SprBucketConfig::bucket_for_spr(1.5) == 1);
  CHECK(SprBucketConfig::bucket_for_spr(2.0) == 1);  // upper bound inclusive
  CHECK(SprBucketConfig::bucket_for_spr(3.0) == 2);
  CHECK(SprBucketConfig::bucket_for_spr(4.0) == 2);  // upper bound inclusive
  CHECK(SprBucketConfig::bucket_for_spr(5.5) == 3);
  CHECK(SprBucketConfig::bucket_for_spr(7.0) == 3);  // upper bound inclusive
  CHECK(SprBucketConfig::bucket_for_spr(9.5) == 4);
  CHECK(SprBucketConfig::bucket_for_spr(12.0) == 4);  // upper bound inclusive
  CHECK(SprBucketConfig::bucket_for_spr(15.0) == 5);
  CHECK(SprBucketConfig::bucket_for_spr(100.0) == 5);  // deep tail

  // Invalid SPR throws.
  bool threw = false;
  try {
    SprBucketConfig::bucket_for_spr(0.0);
  } catch (const std::invalid_argument&) {
    threw = true;
  }
  CHECK(threw);

  threw = false;
  try {
    SprBucketConfig::bucket_for_spr(-1.0);
  } catch (const std::invalid_argument&) {
    threw = true;
  }
  CHECK(threw);

  std::printf("  spr_bucketing PASS\n");
  return 0;
}

// ---------------------------------------------------------------------------
// 2. N-seat table round-trip
// ---------------------------------------------------------------------------

int test_table_roundtrip() {
  const std::array<int, 3> flop = {card("2c"), card("3d"), card("7h")};
  const std::array<int, 2> h0 = sorted_hand("Ah", "Ac");
  const std::array<int, 2> h1 = sorted_hand("Ks", "Kc");

  NSeatDeclaredFrontierTable::Entry e;
  e.canonical_flop = flop;
  e.spr_bucket = 2;  // (2,4]
  e.hands = {h0, h1};
  e.values = {0.5, -0.5};  // chip values at representative pot

  NSeatDeclaredFrontierTable::Metadata meta;
  meta.representative_pot[2] = 6.0;

  NSeatDeclaredFrontierTable table({e}, meta);
  CHECK(table.size() == 1);

  const std::array<std::array<int, 2>, 2> hands = {h0, h1};

  // Lookup hits.
  const std::vector<double>& v = table.lookup(flop, 2, hands);
  CHECK(v.size() == 2);
  CHECK(std::abs(v[0] - 0.5) < 1e-12);
  CHECK(std::abs(v[1] - (-0.5)) < 1e-12);

  // Missing flop throws.
  const std::array<int, 3> other_flop = {card("2c"), card("3d"), card("8h")};
  bool threw = false;
  try {
    table.lookup(other_flop, 2, hands);
  } catch (const std::invalid_argument&) {
    threw = true;
  }
  CHECK(threw);

  // Missing bucket throws.
  threw = false;
  try {
    table.lookup(flop, 0, hands);
  } catch (const std::invalid_argument&) {
    threw = true;
  }
  CHECK(threw);

  // Missing hands throws.
  const std::array<int, 2> other_h = sorted_hand("Qh", "Qc");
  const std::array<std::array<int, 2>, 2> other_hands = {h0, other_h};
  threw = false;
  try {
    table.lookup(flop, 2, other_hands);
  } catch (const std::invalid_argument&) {
    threw = true;
  }
  CHECK(threw);

  // Duplicate key throws.
  NSeatDeclaredFrontierTable::Entry dup = e;
  threw = false;
  try {
    NSeatDeclaredFrontierTable({e, dup}, meta);
  } catch (const std::invalid_argument&) {
    threw = true;
  }
  CHECK(threw);

  std::printf("  table_roundtrip PASS\n");
  return 0;
}

// ---------------------------------------------------------------------------
// 3. Evaluator scaling
// ---------------------------------------------------------------------------

int test_evaluator_scaling() {
  // Build a one-entry table for flop 2c3d7h, bucket (2,4], deal AhAc vs KsKc.
  // The table stores the CANONICAL flop and canonical-suit-relabeled hands.
  const std::array<int, 3> raw_flop = {card("2c"), card("3d"), card("7h")};
  const abstraction::CanonicalBoard canon = abstraction::canonicalize(raw_flop);
  const std::array<int, 2> raw_h0 = sorted_hand("Ah", "Ac");
  const std::array<int, 2> raw_h1 = sorted_hand("Ks", "Kc");

  // Relabel hands through the canonical relabel.
  std::array<int, 2> h0{}, h1{};
  for (std::size_t c = 0; c < 2; ++c) {
    h0[c] = (raw_h0[c] / 4) * 4 + canon.relabel[raw_h0[c] % 4];
    h1[c] = (raw_h1[c] / 4) * 4 + canon.relabel[raw_h1[c] % 4];
  }
  std::sort(h0.begin(), h0.end());
  std::sort(h1.begin(), h1.end());

  NSeatDeclaredFrontierTable::Entry e;
  e.canonical_flop = canon.board;
  e.spr_bucket = 2;  // (2,4]
  e.hands = {h0, h1};
  e.values = {0.3, -0.3};  // chip values at representative pot

  NSeatDeclaredFrontierTable::Metadata meta;
  meta.representative_pot[2] = 4.0;  // representative pot = 4 chips

  NSeatDeclaredFrontierTable table({e}, meta);
  const PostflopFrontierEvaluator evaluator(table);

  // Ledger: pot = 4 (contributions 2+2), stacks = 10 each.
  // SPR = 10/4 = 2.5 → bucket (2,4].
  TerminalPayload ledger{};
  ledger.player_count = 2;
  ledger.seats[0].contributed = 2;
  ledger.seats[0].stack = 10;
  ledger.seats[1].contributed = 2;
  ledger.seats[1].stack = 10;
  ledger.live_count = 2;
  ledger.live_order = {0, 1};

  // The evaluator takes the RAW flop and hands; it canonicalizes internally.
  const std::vector<int> flop_vec = {raw_flop[0], raw_flop[1], raw_flop[2]};
  const std::vector<std::array<int, 2>> hands = {raw_h0, raw_h1};
  const std::vector<double> result = evaluator.evaluate(flop_vec, hands, ledger);

  // Scale = pot_actual / pot_representative = 4/4 = 1.0.
  CHECK(result.size() == 2);
  CHECK(std::abs(result[0] - 0.3) < 1e-9);
  CHECK(std::abs(result[1] - (-0.3)) < 1e-9);

  // Different pot: pot = 8 (contributions 4+4), stacks = 20.
  // SPR = 20/8 = 2.5 → same bucket. Scale = 8/4 = 2.0.
  TerminalPayload ledger2{};
  ledger2.player_count = 2;
  ledger2.seats[0].contributed = 4;
  ledger2.seats[0].stack = 20;
  ledger2.seats[1].contributed = 4;
  ledger2.seats[1].stack = 20;
  ledger2.live_count = 2;
  ledger2.live_order = {0, 1};

  const std::vector<double> result2 = evaluator.evaluate(flop_vec, hands, ledger2);
  CHECK(std::abs(result2[0] - 0.6) < 1e-9);
  CHECK(std::abs(result2[1] - (-0.6)) < 1e-9);

  std::printf("  evaluator_scaling PASS\n");
  return 0;
}

// ---------------------------------------------------------------------------
// 4. Root-value extraction vs exact_deal_utility cross-check
// ---------------------------------------------------------------------------

// Adapter: wraps a solved NSeatPolicy as a stage6::BehaviorPolicy so the
// exact oracle can walk the L1 tree with it.
class NSeatBehaviorAdapter final : public BehaviorPolicy {
 public:
  NSeatBehaviorAdapter(const NSeatPolicy& policy, const AbstractTree& tree)
      : policy_(policy), tree_(tree) {}

  std::vector<PolicyAction> distribution(const GameState& state, std::size_t seat, HoleCards hole,
                                         const PolicyContext&) const override {
    const NSeatPolicyRow* row = policy_.lookup(tree_, state, seat, hole);
    if (row == nullptr)
      throw std::runtime_error("NSeatBehaviorAdapter: missing policy row");
    std::vector<PolicyAction> result;
    result.reserve(row->actions.size());
    for (std::size_t i = 0; i < row->actions.size(); ++i)
      result.push_back({row->actions[i], row->probabilities[i]});
    return result;
  }

 private:
  const NSeatPolicy& policy_;
  const AbstractTree& tree_;
};

// Small river-rooted fixture: 2 seats, 3-chip starting stacks,
// 1-chip dead contributions, pot 2, board 2c3d7hJs9c. Two combos per seat.
// A river-rooted game has no chance nodes, so the tree is small enough that
// every action node receives a policy row even with modest iterations.
struct SmallRiverGame {
  static GameDef make_def() {
    GameDef def{};
    def.player_count = 2;
    def.button = 1;
    def.big_blind = 2;
    def.stacks = {3, 3};
    def.contributions = {1, 1};
    def.pot = 2;
    def.board = {card("2c"), card("3d"), card("7h"), card("Js"), card("9c")};
    def.board_size = 5;
    def.preflop = false;
    def.terminal = TerminalDepth::River;
    return def;
  }

  static std::vector<WeightedHand> range0() {
    return {{{sorted_hand("Ah", "Ac")}, 1.0}, {{sorted_hand("Kd", "Qd")}, 1.0}};
  }
  static std::vector<WeightedHand> range1() {
    return {{{sorted_hand("Ks", "Kc")}, 1.0}, {{sorted_hand("Th", "9h")}, 1.0}};
  }
};

int test_root_value_crosscheck() {
  const GameDef def = SmallRiverGame::make_def();
  const AbstractTree tree(def, abstraction::ActionAbstraction::identity());
  CHECK(tree.size() > 0);

  const std::vector<std::vector<WeightedHand>> ranges = {SmallRiverGame::range0(),
                                                         SmallRiverGame::range1()};

  // Solve with enough iterations for the policy to converge on this tiny game.
  NSeatTrainerLimits limits{};
  limits.wall = std::chrono::seconds(30);
  const NSeatTrainingResult trained = train_nseat(tree, ranges, 500, 20261009, limits);
  CHECK(trained.termination == NSeatTerminationPhase::Complete);
  CHECK(!trained.policy.rows().empty());

  const NSeatPolicy& policy = trained.policy;
  const GameState root_state(def);

  // Enumerate all 2x2 = 4 joint deals.
  struct Deal {
    std::array<int, 2> h0;
    std::array<int, 2> h1;
  };
  const Deal deals[] = {
      {sorted_hand("Ah", "Ac"), sorted_hand("Ks", "Kc")},
      {sorted_hand("Ah", "Ac"), sorted_hand("Th", "9h")},
      {sorted_hand("Kd", "Qd"), sorted_hand("Ks", "Kc")},
      {sorted_hand("Kd", "Qd"), sorted_hand("Th", "9h")},
  };

  // Adapters for the exact oracle.
  const NSeatBehaviorAdapter adapter0(policy, tree);
  const NSeatBehaviorAdapter adapter1(policy, tree);
  const std::vector<const BehaviorPolicy*> policies = {&adapter0, &adapter1};

  const double tolerance = 1e-9 * static_cast<double>(def.pot);

  for (const Deal& d : deals) {
    const std::array<std::array<int, 2>, 2> deal_arr = {d.h0, d.h1};

    // Root-value extraction (walks the abstract tree).
    const std::vector<double> extracted = extract_root_value(policy, tree, root_state, deal_arr);

    // Exact oracle (walks the L1 tree).
    const std::vector<HoleCards> holes = {d.h0, d.h1};
    const std::array<double, 10> oracle = exact_deal_utility(def, holes, policies);

    for (std::size_t s = 0; s < 2; ++s) {
      const double diff = std::abs(extracted[s] - oracle[s]);
      if (diff > tolerance) {
        std::printf(
            "  cross-check mismatch: deal h0=%d%d h1=%d%d seat=%zu "
            "extracted=%.9f oracle=%.9f diff=%.2e tol=%.2e\n",
            d.h0[0], d.h0[1], d.h1[0], d.h1[1], s, extracted[s], oracle[s], diff, tolerance);
        return 1;
      }
    }
  }

  std::printf("  root_value_crosscheck PASS (4 deals, tol=%.1e)\n", tolerance);
  return 0;
}

// ---------------------------------------------------------------------------
// 5. End-to-end: build a one-config table from solved values and verify
//    the evaluator returns them.
// ---------------------------------------------------------------------------

int test_end_to_end() {
  const GameDef def = SmallRiverGame::make_def();
  const AbstractTree tree(def, abstraction::ActionAbstraction::identity());

  const std::vector<std::vector<WeightedHand>> ranges = {SmallRiverGame::range0(),
                                                         SmallRiverGame::range1()};

  NSeatTrainerLimits limits{};
  limits.wall = std::chrono::seconds(30);
  const NSeatTrainingResult trained = train_nseat(tree, ranges, 500, 20261009, limits);
  CHECK(trained.termination == NSeatTerminationPhase::Complete);

  const NSeatPolicy& policy = trained.policy;
  const GameState root_state(def);

  // Compute SPR from the root state.
  Chips pot = 0;
  Chips eff_stack = 0;
  for (std::size_t s = 0; s < 2; ++s) {
    pot += root_state.players()[s].contributed;
    const Chips stk = root_state.players()[s].stack;
    if (s == 0 || stk < eff_stack)
      eff_stack = stk;
  }
  const double spr = static_cast<double>(eff_stack) / static_cast<double>(pot);
  const std::size_t bucket = SprBucketConfig::bucket_for_spr(spr);

  // Canonicalize the flop.
  const std::array<int, 3> flop_arr = {def.board[0], def.board[1], def.board[2]};
  const abstraction::CanonicalBoard canon = abstraction::canonicalize(flop_arr);

  // Build table entries for all 4 joint deals.
  const std::array<int, 2> combos0[] = {sorted_hand("Ah", "Ac"), sorted_hand("Kd", "Qd")};
  const std::array<int, 2> combos1[] = {sorted_hand("Ks", "Kc"), sorted_hand("Th", "9h")};

  std::vector<NSeatDeclaredFrontierTable::Entry> entries;
  for (const auto& h0 : combos0) {
    for (const auto& h1 : combos1) {
      const std::array<std::array<int, 2>, 2> deal = {h0, h1};
      const std::vector<double> values = extract_root_value(policy, tree, root_state, deal);

      // Relabel hands through the canonical relabel.
      std::array<int, 2> rh0{}, rh1{};
      for (std::size_t c = 0; c < 2; ++c) {
        rh0[c] = (h0[c] / 4) * 4 + canon.relabel[h0[c] % 4];
        rh1[c] = (h1[c] / 4) * 4 + canon.relabel[h1[c] % 4];
      }
      std::sort(rh0.begin(), rh0.end());
      std::sort(rh1.begin(), rh1.end());

      NSeatDeclaredFrontierTable::Entry e;
      e.canonical_flop = canon.board;
      e.spr_bucket = static_cast<std::uint8_t>(bucket);
      e.hands = {rh0, rh1};
      // Store chip values at the representative pot. The evaluator scales by
      // pot_actual / representative_pot (unity in this fixture).
      e.values = {values[0], values[1]};
      entries.push_back(std::move(e));
    }
  }

  NSeatDeclaredFrontierTable::Metadata meta;
  meta.representative_pot[bucket] = static_cast<double>(pot);

  NSeatDeclaredFrontierTable table(std::move(entries), meta);
  CHECK(table.size() == 4);

  // Verify the evaluator returns the correct values for each deal.
  const PostflopFrontierEvaluator evaluator(table);

  TerminalPayload ledger{};
  ledger.player_count = 2;
  for (std::size_t s = 0; s < 2; ++s) {
    ledger.seats[s].contributed = root_state.players()[s].contributed;
    ledger.seats[s].stack = root_state.players()[s].stack;
  }
  ledger.live_count = 2;
  ledger.live_order = {0, 1};

  const std::vector<int> flop_vec = {flop_arr[0], flop_arr[1], flop_arr[2]};
  const double tolerance = 1e-9 * static_cast<double>(pot);

  for (const auto& h0 : combos0) {
    for (const auto& h1 : combos1) {
      const std::array<std::array<int, 2>, 2> deal = {h0, h1};
      const std::vector<double> expected = extract_root_value(policy, tree, root_state, deal);

      const std::vector<std::array<int, 2>> hands = {h0, h1};
      const std::vector<double> actual = evaluator.evaluate(flop_vec, hands, ledger);

      for (std::size_t s = 0; s < 2; ++s) {
        if (std::abs(actual[s] - expected[s]) > tolerance) {
          std::printf("  end-to-end mismatch: seat=%zu actual=%.9f expected=%.9f\n", s, actual[s],
                      expected[s]);
          return 1;
        }
      }
    }
  }

  std::printf("  end_to_end PASS (4 deals, bucket=%zu, spr=%.2f)\n", bucket, spr);
  return 0;
}

// ---------------------------------------------------------------------------
// 6. NSP1 codec roundtrip
// ---------------------------------------------------------------------------

int test_nsp1_codec_roundtrip() {
  namespace fs = std::filesystem;
  const fs::path tmp_dir = fs::temp_directory_path() / "bs_nsp1_test";
  fs::create_directories(tmp_dir);
  const fs::path path = tmp_dir / "test_policy.nsp";

  // Build a small policy with two rows.
  const GameDef def = make_postflop_def({0, 1, 10}, 0);
  const abstraction::ActionAbstraction action_abs = make_postflop_abstraction(0);

  std::map<NSeatInformationKey, NSeatPolicyRow> rows;
  {
    NSeatPolicyRow row;
    row.actions = {poker::Action{poker::ActionType::Check, 0},
                   poker::Action{poker::ActionType::Bet, 50}};
    row.probabilities = {0.7, 0.3};
    rows.emplace(NSeatInformationKey{1, 100}, std::move(row));
  }
  {
    NSeatPolicyRow row;
    row.actions = {poker::Action{poker::ActionType::Fold, 0},
                   poker::Action{poker::ActionType::Call, 50}};
    row.probabilities = {0.4, 0.6};
    rows.emplace(NSeatInformationKey{2, 200}, std::move(row));
  }

  NSeatPolicy policy = NSeatPolicy::from_parts(def, action_abs.id(), std::move(rows));

  // Save and load.
  save_nseat_policy(policy, 0, path);
  CHECK(fs::exists(path));
  CHECK(!fs::exists(path.string() + ".tmp"));  // temp file renamed away

  NSeatPolicy loaded = load_nseat_policy(path, def, action_abs.id(), 0);

  // Verify rows match.
  const auto& orig_rows = policy.rows();
  const auto& loaded_rows = loaded.rows();
  CHECK(loaded_rows.size() == orig_rows.size());

  for (const auto& [key, orig_row] : orig_rows) {
    auto it = loaded_rows.find(key);
    CHECK(it != loaded_rows.end());
    const auto& loaded_row = it->second;
    CHECK(loaded_row.actions.size() == orig_row.actions.size());
    for (std::size_t i = 0; i < orig_row.actions.size(); ++i) {
      CHECK(loaded_row.actions[i].type == orig_row.actions[i].type);
      CHECK(loaded_row.actions[i].target_total == orig_row.actions[i].target_total);
      CHECK(std::abs(loaded_row.probabilities[i] - orig_row.probabilities[i]) < 1e-12);
    }
  }

  // Verify game def matches.
  CHECK(loaded.game().player_count == def.player_count);
  CHECK(loaded.game().board[0] == def.board[0]);
  CHECK(loaded.game().board[1] == def.board[1]);
  CHECK(loaded.game().board[2] == def.board[2]);

  fs::remove_all(tmp_dir);
  std::printf("  nsp1_codec_roundtrip PASS\n");
  return 0;
}

// ---------------------------------------------------------------------------
// 7. NSP1 corrupt file rejection
// ---------------------------------------------------------------------------

int test_nsp1_corrupt_rejection() {
  namespace fs = std::filesystem;
  const fs::path tmp_dir = fs::temp_directory_path() / "bs_nsp1_corrupt";
  fs::create_directories(tmp_dir);

  const GameDef def = make_postflop_def({0, 1, 10}, 0);
  const abstraction::ActionAbstraction action_abs = make_postflop_abstraction(0);

  // Bad magic.
  {
    const fs::path p = tmp_dir / "bad_magic.nsp";
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f << "XXXX";
    f.close();
    bool threw = false;
    try {
      load_nseat_policy(p, def, action_abs.id(), 0);
    } catch (const std::runtime_error&) {
      threw = true;
    }
    CHECK(threw);
  }

  // SPR bucket mismatch.
  {
    // Save with bucket 2, load expecting bucket 3. Bucket 0 is accepted as
    // "not stored" (backward compatibility), so use non-zero buckets.
    std::map<NSeatInformationKey, NSeatPolicyRow> rows;
    NSeatPolicy policy = NSeatPolicy::from_parts(def, action_abs.id(), std::move(rows));
    const fs::path p = tmp_dir / "bucket_mismatch.nsp";
    save_nseat_policy(policy, 2, p);
    bool threw = false;
    try {
      load_nseat_policy(p, def, action_abs.id(), 3);
    } catch (const std::runtime_error&) {
      threw = true;
    }
    CHECK(threw);
  }

  // Truncated file.
  {
    const fs::path p = tmp_dir / "truncated.nsp";
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f.write("NSP1", 4);
    std::uint32_t ver = 1;
    f.write(reinterpret_cast<const char*>(&ver), 4);
    f.close();
    bool threw = false;
    try {
      load_nseat_policy(p, def, action_abs.id(), 0);
    } catch (const std::runtime_error&) {
      threw = true;
    }
    CHECK(threw);
  }

  fs::remove_all(tmp_dir);
  std::printf("  nsp1_corrupt_rejection PASS\n");
  return 0;
}

// ---------------------------------------------------------------------------
// 8. Zero-SPR equity fallback
// ---------------------------------------------------------------------------

int test_zero_spr_equity_fallback() {
  // Build a minimal declared table with one entry so the evaluator can be
  // constructed. The zero-SPR path should bypass the table entirely.
  NSeatDeclaredFrontierTable::Entry entry;
  entry.canonical_flop = {0, 1, 10};
  entry.spr_bucket = 0;
  entry.hands = {sorted_hand("2c", "3c"), sorted_hand("2d", "3d")};
  entry.values = {10.0, -10.0};

  NSeatDeclaredFrontierTable::Metadata metadata{};
  metadata.representative_pot[0] = 100.0;

  std::vector<NSeatDeclaredFrontierTable::Entry> entries;
  entries.push_back(std::move(entry));
  NSeatDeclaredFrontierTable table(std::move(entries), metadata);

  PostflopFrontierEvaluator evaluator(table);

  // All-in ledger: both seats have zero stack (all-in preflop).
  TerminalPayload ledger{};
  ledger.player_count = 2;
  ledger.seats[0].contributed = 50;
  ledger.seats[0].stack = 0;
  ledger.seats[0].folded = false;
  ledger.seats[1].contributed = 50;
  ledger.seats[1].stack = 0;
  ledger.seats[1].folded = false;

  // This must NOT throw. At zero SPR the evaluator delegates to equity.
  std::vector<double> values;
  try {
    values = evaluator.evaluate(
        std::array<int, 3>{0, 1, 10},
        std::array<std::array<int, 2>, 2>{sorted_hand("2c", "3c"), sorted_hand("2d", "3d")},
        ledger);
  } catch (const std::exception& e) {
    std::printf("  zero-SPR threw unexpectedly: %s\n", e.what());
    return 1;
  }

  CHECK(values.size() == 2);
  // Equity values should be finite and sum to ~0 (net chip delta, zero-sum).
  CHECK(std::isfinite(values[0]));
  CHECK(std::isfinite(values[1]));
  CHECK(std::abs(values[0] + values[1]) < 1e-9);

  std::printf("  zero_spr_equity_fallback PASS (v0=%.4f v1=%.4f)\n", values[0], values[1]);
  return 0;
}

}  // namespace

int main() {
  try {
    if (test_spr_bucketing() != 0)
      return 1;
    if (test_table_roundtrip() != 0)
      return 1;
    if (test_evaluator_scaling() != 0)
      return 1;
    if (test_root_value_crosscheck() != 0)
      return 1;
    if (test_end_to_end() != 0)
      return 1;
    if (test_nsp1_codec_roundtrip() != 0)
      return 1;
    if (test_nsp1_corrupt_rejection() != 0)
      return 1;
    if (test_zero_spr_equity_fallback() != 0)
      return 1;
  } catch (const std::exception& e) {
    std::printf("UNCAUGHT EXCEPTION: %s\n", e.what());
    return 1;
  }
  std::printf("test_postflop_frontier PASS\n");
  return 0;
}
