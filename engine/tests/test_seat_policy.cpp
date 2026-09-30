// RFC 0009 W2b Step 4 conformance for the seat-indexed policy export.
//
// Three gates:
//  1. Three-seat export bijection: on a 3-seat river-rooted tree (board_size 5,
//     so no chance nodes and external sampling covers every action node),
//     export_seat_policy emits one concrete row per (node, acting-seat,
//     off-board combo) whose CategoryTiersV1 bucket has a sealed (bucket, node)
//     row, and the emitted row matches the NSeatPolicyRow for that bucket. A
//     tree walk written in this test re-derives the expected row set from the
//     sealed (bucket, node) table and compares it to the export key-by-key and
//     value-by-value.
//  2. Key round-trip: make_information_key round-trips through
//     encode_public_key(key, 2) / decode_public_key(text, 2, ...) for every
//     actor 0..9 and board_count 0/3/4/5, with grammar-consistent
//     public-action paths. The revision-1/revision-2 rejection surfaces are
//     exercised too.
//  3. Two-seat byte identity: make_information_key produces the byte-identical
//     key (and revision-1 encoding) that information_key(HeadsUpState, ...)
//     produces for the same state, at both an empty-path flop root and a
//     check-down river state. A 2-seat export keys its rows by
//     make_information_key, closing the chain export -> make_information_key ->
//     information_key.
#include <array>
#include <bs/abstract_tree.hpp>
#include <bs/abstraction.hpp>
#include <bs/eval.hpp>
#include <bs/game_definition.hpp>
#include <bs/heads_up.hpp>
#include <bs/heads_up_solver.hpp>
#include <bs/nseat_trainer.hpp>
#include <bs/seat_policy.hpp>
#include <bs/strategy_artifact.hpp>
#include <cstdint>
#include <cstdio>
#include <map>
#include <stdexcept>
#include <string>
#include <utility>
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
using namespace bs::poker;
using namespace bs::solver;
using namespace bs::tree;

int card(const char* name) {
  return cardId(std::string(name));
}

// --- shared fixtures ---------------------------------------------------------

// A 3-seat river-rooted fixture: board_size 5 so the tree has no chance nodes
// and external sampling covers every action node. Same chip shape as the
// existing 3-seat flop fixture (1/1/1 stacks, dead {1,1,1}, pot 3).
GameDef three_seat_river_def() {
  GameDef def{};
  def.player_count = 3;
  def.button = 0;
  def.big_blind = 2;
  def.stacks = {1, 1, 1, 0, 0, 0, 0, 0, 0, 0};
  def.contributions = {1, 1, 1, 0, 0, 0, 0, 0, 0, 0};
  def.pot = 3;
  def.board = {card("2c"), card("3d"), card("7h"), card("Ks"), card("9c")};
  def.board_size = 5;
  return def;
}

// Two combos per seat, all off the river board and mutually card-distinct, so
// every joint combination is card-compatible.
std::vector<std::vector<WeightedHand>> three_seat_river_ranges() {
  return {
      {{{card("Ah"), card("Ac")}, 2}, {{card("8h"), card("8c")}, 3}},
      {{{card("As"), card("Ad")}, 5}, {{card("8s"), card("8d")}, 7}},
      {{{card("Qh"), card("Qc")}, 11}, {{card("Jh"), card("Jc")}, 13}},
  };
}

// The flop-rooted heads-up family (test_solve_nseat.cpp:74): top overpairs,
// rooted flop 2c3d7h, 1/1 stacks, dead contributions {1,1}.
HeadsUpGame heads_up_river_game() {
  HeadsUpGame game;
  game.root = {{card("2c"), card("3d"), card("7h")}, {1, 1}, {1, 1}, 2, 1, 1};
  game.ranges[0] = {{{card("Ah"), card("Ac")}, 2}, {{card("8d"), card("8c")}, 3}};
  game.ranges[1] = {{{card("As"), card("Ad")}, 5}, {{card("Qd"), card("Qc")}, 7}};
  game.fixed_runout = {card("Ks"), card("9c")};
  return game;
}

// Advances a heads-up state from the flop root through a check-down on the
// flop and turn to the river action phase. With 1-chip stacks any flop or turn
// bet is all-in and ends the hand, so check-down is the ONLY history that
// reaches the river with pot 2 and 1/1 stacks. (Same shape as
// test_solve_nseat.cpp:466.)
HeadsUpState checked_down_river(const HeadsUpGame& game) {
  HeadsUpState state(game.root);
  const Action check{ActionType::Check, 0};
  for (int street = 0; street < 2; ++street) {
    if (state.phase() != Phase::Action || state.actor() != 0)
      throw std::runtime_error("checked_down_river: expected seat 0 to act");
    state = state.after_action(0, check);
    if (state.phase() != Phase::Action || state.actor() != 1)
      throw std::runtime_error("checked_down_river: expected seat 1 to act");
    state = state.after_action(1, check);
    if (state.phase() != Phase::Deal)
      throw std::runtime_error("checked_down_river: expected a deal phase");
    state = state.after_card(*game.fixed_runout[street]);
  }
  if (state.phase() != Phase::Action || state.actor() != 0)
    throw std::runtime_error("checked_down_river: expected river action by seat 0");
  if (state.board().size() != 5)
    throw std::runtime_error("checked_down_river: expected a five-card board");
  return state;
}

// --- 1. three-seat export bijection ------------------------------------------

// Re-derives the expected concrete row set from the sealed (bucket, node)
// table, mirroring export_dfs's walk (same child orders, same emplace-keep-
// first) but written independently in this test from the documented contract.
void collect_expected(const AbstractTree& tree, const NSeatPolicy& nseat_policy,
                      std::map<InformationKey, SeatPolicyRow>& expected, std::size_t node_index,
                      GameState& cursor, std::vector<PublicAction>& path) {
  const TreeNode& node = tree.node(node_index);
  if (node.is_terminal())
    return;
  if (node.is_chance()) {
    for (std::size_t ci = 0; ci < node.children.size(); ++ci) {
      const TreeNode& child = tree.node(node.children[ci]);
      GameState next = cursor.after_card(child.incoming_card);
      collect_expected(tree, nseat_policy, expected, node.children[ci], next, path);
    }
    return;
  }

  const std::size_t seat = node.actor;
  const auto board = cursor.board();
  std::array<bool, 52> on_board{};
  for (int c : board)
    on_board[static_cast<std::size_t>(c)] = true;

  for (int c0 = 0; c0 < 52; ++c0) {
    if (on_board[static_cast<std::size_t>(c0)])
      continue;
    for (int c1 = c0 + 1; c1 < 52; ++c1) {
      if (on_board[static_cast<std::size_t>(c1)])
        continue;
      const std::array<int, 2> cards{c0, c1};
      const std::vector<int> board_vec(board.begin(), board.end());
      const std::uint32_t bucket =
          abstraction::card_bucket(abstraction::CardBucketKind::CategoryTiersV1, cards, board_vec);
      const NSeatInformationKey nk{bucket, node_index};
      const auto it = nseat_policy.rows().find(nk);
      if (it == nseat_policy.rows().end())
        continue;  // exact miss: no sealed row at this (bucket, node)
      const NSeatPolicyRow& nrow = it->second;
      SeatPolicyRow srow;
      srow.actions = nrow.actions;
      srow.probabilities = nrow.probabilities;
      srow.visits = nrow.visits;
      expected.emplace(make_information_key(seat, cards, board, path), std::move(srow));
    }
  }

  for (std::size_t ai = 0; ai < node.actions.size(); ++ai) {
    const Action& action = node.actions[ai];
    GameState next = cursor.after_action(seat, action);
    path.push_back(PublicAction{node.street, seat, action});
    collect_expected(tree, nseat_policy, expected, node.children[ai], next, path);
    path.pop_back();
  }
}

int test_export_bijection() {
  const GameDef def = three_seat_river_def();
  const AbstractTree tree(def, abstraction::ActionAbstraction::identity());
  const auto ranges = three_seat_river_ranges();
  const NSeatTrainingResult trained =
      train_nseat(tree, ranges, 200, 20260930, NSeatTrainerLimits{});
  CHECK(trained.termination == NSeatTerminationPhase::Complete);
  CHECK(trained.completed_iterations == 200);
  CHECK(same_game_def(trained.policy.game(), def));
  CHECK(!trained.policy.rows().empty());

  const SeatTrainingResult exported = export_seat_policy(trained, tree, ranges);
  CHECK(same_game_def(exported.policy.game(), def));
  CHECK(exported.policy.action_id() == tree.action_id());
  CHECK(exported.action_id == trained.policy.action_id());
  CHECK(exported.terminal_depth == def.terminal);
  CHECK(exported.completed_iterations == trained.completed_iterations);
  CHECK(exported.seed == trained.seed);
  CHECK(exported.algorithm_revision == trained.algorithm_revision);

  const auto& seat_rows = exported.policy.rows();
  CHECK(!seat_rows.empty());

  // Re-derive the expected row set and compare key-by-key, value-by-value.
  std::map<InformationKey, SeatPolicyRow> expected;
  GameState cursor{def};
  std::vector<PublicAction> path;
  collect_expected(tree, trained.policy, expected, tree.root_index(), cursor, path);
  CHECK(!expected.empty());
  CHECK(seat_rows.size() == expected.size());
  auto eit = expected.begin();
  for (const auto& [key, row] : seat_rows) {
    CHECK(eit != expected.end());
    CHECK(eit->first == key);
    CHECK(eit->second.actions == row.actions);
    CHECK(eit->second.probabilities == row.probabilities);
    CHECK(eit->second.visits == row.visits);
    ++eit;
  }
  CHECK(eit == expected.end());
  std::printf("export bijection: %zu exported rows match the sealed (bucket, node) table\n",
              seat_rows.size());
  return 0;
}

// --- 2. key round-trip -------------------------------------------------------

int test_key_round_trip() {
  // {Ah, Ac} = {49, 51}, sorted ascending and off every board below.
  const std::array<int, 2> cards{card("Ah"), card("Ac")};
  const std::vector<int> board0{};
  const std::vector<int> board3{card("2c"), card("3d"), card("7h")};
  const std::vector<int> board4{card("2c"), card("3d"), card("7h"), card("Ks")};
  const std::vector<int> board5{card("2c"), card("3d"), card("7h"), card("Ks"), card("9c")};
  const std::vector<PublicAction> no_path;
  const Action check{ActionType::Check, 0};
  // The decode grammar reconstructs each event's street from the round-end rule
  // (a call or a check-check advances the street, AFTER pushing the event), so
  // a check that closes a street is tagged with the street it closes -- exactly
  // PublicAction.street semantics. board_count 4 needs a flop check-check;
  // board_count 5 needs a flop and a turn check-check.
  const std::vector<PublicAction> flop_check_check{
      {Street::Flop, 0, check},
      {Street::Flop, 1, check},
  };
  const std::vector<PublicAction> flop_and_turn_check_check{
      {Street::Flop, 0, check},
      {Street::Flop, 1, check},
      {Street::Turn, 0, check},
      {Street::Turn, 1, check},
  };

  for (int actor = 0; actor <= 9; ++actor) {
    // board_count 0: the preflop form (revision 2 only).
    {
      const InformationKey key = make_information_key(actor, cards, board0, no_path);
      const std::string text = artifacts::encode_public_key(key, 2);
      const InformationKey back = artifacts::decode_public_key(text, 2, actor, cards[0], cards[1]);
      CHECK(back == key);
    }
    // board_count 3: flop, empty path.
    {
      const InformationKey key = make_information_key(actor, cards, board3, no_path);
      const std::string text = artifacts::encode_public_key(key, 2);
      const InformationKey back = artifacts::decode_public_key(text, 2, actor, cards[0], cards[1]);
      CHECK(back == key);
    }
    // board_count 4: turn, flop check-check.
    {
      const InformationKey key = make_information_key(actor, cards, board4, flop_check_check);
      const std::string text = artifacts::encode_public_key(key, 2);
      const InformationKey back = artifacts::decode_public_key(text, 2, actor, cards[0], cards[1]);
      CHECK(back == key);
    }
    // board_count 5: river, flop and turn check-check.
    {
      const InformationKey key =
          make_information_key(actor, cards, board5, flop_and_turn_check_check);
      const std::string text = artifacts::encode_public_key(key, 2);
      const InformationKey back = artifacts::decode_public_key(text, 2, actor, cards[0], cards[1]);
      CHECK(back == key);
    }
  }

  // True iff the call throws an ArtifactError of the expected kind.
  auto expect_artifact = [](auto&& call, artifacts::ArtifactErrorKind kind) {
    try {
      call();
    } catch (const artifacts::ArtifactError& e) {
      return e.kind() == kind;
    }
    return false;
  };

  // Revision 1 rejects the preflop form (board_count 0) at encode.
  CHECK(expect_artifact(
      [&] {
        (void)artifacts::encode_public_key(make_information_key(0, cards, board0, no_path), 1);
      },
      artifacts::ArtifactErrorKind::InvalidArgument));
  // Revision 1 rejects player > 1 at decode.
  {
    const std::string text =
        artifacts::encode_public_key(make_information_key(0, cards, board3, no_path), 1);
    CHECK(
        expect_artifact([&] { (void)artifacts::decode_public_key(text, 1, 2, cards[0], cards[1]); },
                        artifacts::ArtifactErrorKind::InvalidSchema));
  }
  // Revision 2 rejects player > 9 at decode.
  {
    const std::string text =
        artifacts::encode_public_key(make_information_key(0, cards, board3, no_path), 2);
    CHECK(expect_artifact(
        [&] { (void)artifacts::decode_public_key(text, 2, 10, cards[0], cards[1]); },
        artifacts::ArtifactErrorKind::InvalidSchema));
  }
  // Revision 2 rejects board_count 1 at encode (build the key by hand since
  // make_information_key validates board_count in {0,3,4,5}).
  {
    const InformationKey bad{0, static_cast<std::uint64_t>(cards[0]),
                             static_cast<std::uint64_t>(cards[1]), 1,
                             static_cast<std::uint64_t>(board3[0])};
    CHECK(expect_artifact([&] { (void)artifacts::encode_public_key(bad, 2); },
                          artifacts::ArtifactErrorKind::InvalidArgument));
  }
  // Decode rejects card0 >= card1.
  {
    const std::string text =
        artifacts::encode_public_key(make_information_key(0, cards, board3, no_path), 2);
    CHECK(
        expect_artifact([&] { (void)artifacts::decode_public_key(text, 2, 0, cards[1], cards[0]); },
                        artifacts::ArtifactErrorKind::InvalidSchema));
  }
  // An unsupported grammar revision is an InvalidArgument.
  CHECK(expect_artifact(
      [&] {
        (void)artifacts::encode_public_key(make_information_key(0, cards, board3, no_path), 99);
      },
      artifacts::ArtifactErrorKind::InvalidArgument));

  std::printf("key round-trip: 40 actor/board pairs + 6 rejections PASS\n");
  return 0;
}

// --- 3. two-seat byte identity ------------------------------------------------

int test_two_seat_byte_identity() {
  const std::array<int, 2> own{card("Ah"), card("Ac")};
  const std::vector<int> flop_board{card("2c"), card("3d"), card("7h")};
  const std::vector<int> river_board{card("2c"), card("3d"), card("7h"), card("Ks"), card("9c")};
  const std::vector<PublicAction> no_path;

  // (a) Empty-path flop root: the heads-up root has no history, so
  //     make_information_key and information_key must agree byte-for-byte.
  {
    HeadsUpRoot root{{card("2c"), card("3d"), card("7h")}, {1, 1}, {1, 1}, 2, 1, 1};
    HeadsUpState state(root);
    CHECK(state.phase() == Phase::Action);
    CHECK(state.actor().has_value() && *state.actor() == 0);
    CHECK(state.board().size() == 3);
    CHECK(state.history().empty());

    const InformationKey hu_key = information_key(state, own);
    const InformationKey seat_key = make_information_key(0, own, flop_board, no_path);
    CHECK(hu_key == seat_key);
    CHECK(artifacts::encode_public_key(hu_key, 1) == artifacts::encode_public_key(seat_key, 1));
  }

  // (b) Check-down river state: the heads-up history carries the flop and turn
  //     check-check, and make_information_key carries the same path, so the two
  //     keys (and their revision-1 encodings) are byte-identical.
  {
    const HeadsUpGame game = heads_up_river_game();
    const HeadsUpState river_state = checked_down_river(game);
    CHECK(river_state.history().size() == 4);
    const Action check{ActionType::Check, 0};
    const std::vector<PublicAction> check_down_path{
        {Street::Flop, 0, check},
        {Street::Flop, 1, check},
        {Street::Turn, 0, check},
        {Street::Turn, 1, check},
    };
    const InformationKey hu_key = information_key(river_state, own);
    const InformationKey seat_key = make_information_key(0, own, river_board, check_down_path);
    CHECK(hu_key == seat_key);
    CHECK(artifacts::encode_public_key(hu_key, 1) == artifacts::encode_public_key(seat_key, 1));
  }

  // (c) A 2-seat river-rooted export keys its rows by make_information_key: the
  //     root row for seat 0's combo sits at make_information_key(0, combo,
  //     board, {}). With (a)/(b) establishing make_information_key ==
  //     information_key, this closes the chain export -> make_information_key
  //     -> information_key.
  {
    GameDef def{};
    def.player_count = 2;
    def.button = 1;
    def.big_blind = 1;
    def.stacks = {1, 1, 0, 0, 0, 0, 0, 0, 0, 0};
    def.contributions = {1, 1, 0, 0, 0, 0, 0, 0, 0, 0};
    def.pot = 2;
    def.board = {card("2c"), card("3d"), card("7h"), card("Ks"), card("9c")};
    def.board_size = 5;
    const AbstractTree tree(def, abstraction::ActionAbstraction::identity());
    const std::vector<std::vector<WeightedHand>> ranges{
        {{{card("Ah"), card("Ac")}, 2}, {{card("8d"), card("8c")}, 3}},
        {{{card("As"), card("Ad")}, 5}, {{card("Qd"), card("Qc")}, 7}},
    };
    const NSeatTrainingResult trained = train_nseat(tree, ranges, 100, 4242, NSeatTrainerLimits{});
    CHECK(trained.termination == NSeatTerminationPhase::Complete);
    const SeatTrainingResult exported = export_seat_policy(trained, tree, ranges);
    const InformationKey root_key = make_information_key(0, own, river_board, no_path);
    CHECK(exported.policy.rows().count(root_key) == 1);
  }

  std::printf("two-seat byte identity: flop root, check-down river, export root PASS\n");
  return 0;
}

}  // namespace

int main() {
  if (test_export_bijection() != 0)
    return 1;
  if (test_key_round_trip() != 0)
    return 1;
  if (test_two_seat_byte_identity() != 0)
    return 1;
  std::printf("test_seat_policy PASS\n");
  return 0;
}
