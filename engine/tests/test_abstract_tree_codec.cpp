// RFC 0014: ATF1 tree codec roundtrip test.
//
// Builds a small postflop tree, saves it to a temp file, loads it back, and
// verifies the loaded tree matches the original in structure and content.
// This is a CTest, not a manual benchmark.
#include <zstd.h>

#include <array>
#include <bs/abstract_tree.hpp>
#include <bs/abstraction.hpp>
#include <bs/game_definition.hpp>
#include <bs/postflop_config.hpp>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <string>
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
using namespace bs::tree;
using namespace bs::poker;
using bs::gto::make_postflop_abstraction;
using bs::gto::make_postflop_def;

// Build a small postflop tree (bucket 0 = SPR 0.5, smallest tree).
std::unique_ptr<AbstractTree> build_test_tree() {
  const std::array<int, 3> flop = {0, 1, 2};  // 2c 2d 2h
  const GameDef def = make_postflop_def(flop, 0);
  const auto abstraction = make_postflop_abstraction(0);
  TreeLimits limits;
  limits.max_bytes = std::size_t{1} << 30;  // 1 GiB
  limits.max_nodes = 2'000'000;
  return std::make_unique<AbstractTree>(def, abstraction, limits);
}

// Verify two trees have identical structure. Returns 0 on success, 1 on failure.
int verify_trees_match(const AbstractTree& a, const AbstractTree& b) {
  CHECK(a.size() == b.size());
  CHECK(a.terminals().size() == b.terminals().size());
  CHECK(a.depth() == b.depth());

  const auto& nodes_a = a.nodes();
  const auto& nodes_b = b.nodes();
  CHECK(nodes_a.size() == nodes_b.size());

  for (std::size_t i = 0; i < nodes_a.size(); ++i) {
    const TreeNode& na = nodes_a[i];
    const TreeNode& nb = nodes_b[i];
    CHECK(na.kind == nb.kind);
    CHECK(na.index == nb.index);
    CHECK(na.parent == nb.parent);
    CHECK(na.street == nb.street);
    CHECK(na.actor == nb.actor);
    CHECK(na.children.size() == nb.children.size());
    for (std::size_t c = 0; c < na.children.size(); ++c)
      CHECK(na.children[c] == nb.children[c]);
    CHECK(na.actions.size() == nb.actions.size());
    for (std::size_t ai = 0; ai < na.actions.size(); ++ai) {
      CHECK(na.actions[ai].type == nb.actions[ai].type);
      CHECK(na.actions[ai].target_total == nb.actions[ai].target_total);
    }
    CHECK(na.incoming_action.type == nb.incoming_action.type);
    CHECK(na.incoming_action.target_total == nb.incoming_action.target_total);
    CHECK(na.incoming_card == nb.incoming_card);
    CHECK(na.terminal == nb.terminal);
  }

  // Verify terminal payloads.
  const auto& terms_a = a.terminals();
  const auto& terms_b = b.terminals();
  CHECK(terms_a.size() == terms_b.size());
  for (std::size_t i = 0; i < terms_a.size(); ++i) {
    const TerminalPayload& ta = terms_a[i];
    const TerminalPayload& tb = terms_b[i];
    CHECK(ta.player_count == tb.player_count);
    CHECK(ta.folded == tb.folded);
    CHECK(ta.board_size == tb.board_size);
    for (std::size_t j = 0; j < ta.board_size; ++j)
      CHECK(ta.board[j] == tb.board[j]);
    CHECK(ta.live_count == tb.live_count);
    for (std::size_t j = 0; j < ta.live_count; ++j)
      CHECK(ta.live_order[j] == tb.live_order[j]);
    for (std::size_t s = 0; s < ta.player_count; ++s) {
      CHECK(ta.seats[s].contributed == tb.seats[s].contributed);
      CHECK(ta.seats[s].refunded == tb.seats[s].refunded);
      CHECK(ta.seats[s].stack == tb.seats[s].stack);
      CHECK(ta.seats[s].folded == tb.seats[s].folded);
    }
    if (ta.folded) {
      CHECK(ta.chip_utility.size() == tb.chip_utility.size());
      for (std::size_t j = 0; j < ta.chip_utility.size(); ++j)
        CHECK(ta.chip_utility[j] == tb.chip_utility[j]);
    }
  }
  return 0;
}

struct TestFiles {
  std::filesystem::path dir =
      std::filesystem::temp_directory_path() /
      ("bs_tree_codec_" +
       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  TestFiles() { std::filesystem::create_directory(dir); }
  ~TestFiles() {
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
  }
};

template <typename T>
void put(std::vector<char>& bytes, std::size_t offset, T value) {
  if (offset > bytes.size() || sizeof(T) > bytes.size() - offset)
    throw std::runtime_error("invalid test mutation offset");
  std::memcpy(bytes.data() + offset, &value, sizeof(T));
}

template <typename T>
T get(const std::vector<char>& bytes, std::size_t offset) {
  T value;
  if (offset > bytes.size() || sizeof(T) > bytes.size() - offset)
    throw std::runtime_error("invalid test read offset");
  std::memcpy(&value, bytes.data() + offset, sizeof(T));
  return value;
}

std::vector<char> read_file(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary | std::ios::ate);
  if (!in || in.tellg() < 0)
    throw std::runtime_error("cannot read test file");
  std::vector<char> bytes(static_cast<std::size_t>(in.tellg()));
  in.seekg(0);
  in.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  if (!in)
    throw std::runtime_error("cannot read test bytes");
  return bytes;
}

void write_file(const std::filesystem::path& path, const std::vector<char>& bytes) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  if (!out)
    throw std::runtime_error("cannot write test bytes");
}

std::vector<char> decompress(const std::vector<char>& compressed) {
  std::vector<char> raw(get<std::uint64_t>(compressed, 8));
  const auto size =
      ZSTD_decompress(raw.data(), raw.size(), compressed.data() + 16, compressed.size() - 16);
  if (ZSTD_isError(size) || size != raw.size())
    throw std::runtime_error("cannot decompress test fixture");
  return raw;
}

// Keep malformed-input cases small: a river-rooted multiway tree still has
// action, fold, and showdown nodes, but no large chance expansion.
GameDef multiway_fixture() {
  GameDef def{};
  def.player_count = 3;
  def.button = 0;
  def.big_blind = 2;
  def.stacks = {2, 3, 3};
  def.contributions = {1, 1, 1};
  def.pot = 3;
  def.board = {0, 5, 10, 15, 20};
  def.board_size = 5;
  return def;
}

bool rejects(const std::filesystem::path& path, const GameDef& def,
             const abstraction::ActionAbstraction& action, TreeLimits limits = {},
             bool resource_only = false) {
  try {
    (void)AbstractTree::load(path, def, action, limits);
  } catch (const tree_resource_exhausted&) {
    return true;
  } catch (const std::runtime_error&) {
    return !resource_only;
  }
  return false;
}

int verify_rejections(const TestFiles& files) {
  const auto def = multiway_fixture();
  const auto action = abstraction::ActionAbstraction::identity();
  const AbstractTree tree(def, action);
  const auto path = files.dir / "multiway.atf";
  tree.save(path);
  const auto compressed = read_file(path);
  const auto raw = decompress(compressed);
  CHECK(verify_trees_match(tree, AbstractTree::load(path, def, action)) == 0);
  write_file(path, raw);
  const auto loaded = AbstractTree::load(path, def, action);
  CHECK(verify_trees_match(tree, loaded) == 0);

  std::vector<std::size_t> node_offsets;
  std::size_t offset = 32;
  std::size_t fold_node = kNoNode;
  std::size_t showdown_node = kNoNode;
  for (const auto& node : tree.nodes()) {
    node_offsets.push_back(offset);
    if (node.kind == NodeKind::TerminalFold && fold_node == kNoNode)
      fold_node = node.index;
    if (node.kind == NodeKind::TerminalShowdown && showdown_node == kNoNode)
      showdown_node = node.index;
    offset += 24 + node.children.size() * 4 + node.actions.size() * 5;
  }
  CHECK(fold_node != kNoNode && showdown_node != kNoNode);
  std::vector<std::size_t> terminal_offsets;
  for (const auto& terminal : tree.terminals()) {
    terminal_offsets.push_back(offset);
    offset += 28 + terminal.player_count * 32 + terminal.live_count * 4 +
              terminal.chip_utility.size() * 8;
  }
  CHECK(offset == raw.size());
  const auto fold = terminal_offsets[tree.node(fold_node).terminal];
  const auto showdown = terminal_offsets[tree.node(showdown_node).terminal];
  const auto live = showdown + 4 + def.player_count * 32;
  const auto board = live + tree.terminal(tree.node(showdown_node).terminal).live_count * 4;
  const auto fold_utilities = fold + 4 + def.player_count * 32 + 4 + 20;
  const auto& fold_payload = tree.terminal(tree.node(fold_node).terminal);
  std::size_t forged_loser = kNoNode;
  for (std::size_t seat = 0; seat < fold_payload.player_count; ++seat) {
    if (fold_payload.seats[seat].folded && fold_payload.seats[seat].stack > 0) {
      forged_loser = seat;
      break;
    }
  }
  CHECK(forged_loser != kNoNode);
  using Mutation = std::pair<const char*, std::function<void(std::vector<char>&)>>;
  const std::vector<Mutation> mutations = {
      {"bad magic", [](auto& b) { b[0] = 'X'; }},
      {"bad version", [](auto& b) { put<std::uint32_t>(b, 4, 1); }},
      {"reserved header", [](auto& b) { put<std::uint32_t>(b, 28, 1); }},
      {"zero nodes", [](auto& b) { put<std::uint32_t>(b, 8, 0); }},
      {"terminal count", [&](auto& b) { put<std::uint32_t>(b, 12, tree.size() + 1); }},
      {"zero depth", [](auto& b) { put<std::uint32_t>(b, 16, 0); }},
      {"wrong depth", [&](auto& b) { put<std::uint32_t>(b, 16, tree.depth() + 1); }},
      {"node kind", [&](auto& b) { b[node_offsets[0]] = 99; }},
      {"node street", [&](auto& b) { b[node_offsets[0] + 1] = 99; }},
      {"wrong street", [&](auto& b) { b[node_offsets[0] + 1] = 0; }},
      {"actor index", [&](auto& b) { put<std::uint32_t>(b, node_offsets[0] + 4, 3); }},
      {"wrong legal seat actor",
       [&](auto& b) {
         put<std::uint32_t>(b, node_offsets[0] + 4, (tree.node(0).actor + 1) % def.player_count);
       }},
      {"action degree", [&](auto& b) { b[node_offsets[0] + 2] = 0; }},
      {"huge child count",
       [&](auto& b) {
         put<std::uint32_t>(b, node_offsets[0] + 16, std::numeric_limits<std::uint32_t>::max());
       }},
      {"child out of bounds",
       [&](auto& b) { put<std::uint32_t>(b, node_offsets[0] + 24, tree.size()); }},
      {"cycle to root", [&](auto& b) { put<std::uint32_t>(b, node_offsets[0] + 24, 0); }},
      {"duplicate child",
       [&](auto& b) { put<std::uint32_t>(b, node_offsets[0] + 28, tree.node(0).children[0]); }},
      {"terminal index",
       [&](auto& b) {
         put<std::uint32_t>(b, node_offsets[fold_node] + 12, tree.terminals().size());
       }},
      {"incoming action type", [&](auto& b) { b[node_offsets[fold_node] + 3] = 99; }},
      {"incoming card", [&](auto& b) { put<std::int32_t>(b, node_offsets[fold_node] + 8, 52); }},
      {"action edge mismatch", [&](auto& b) { b[node_offsets[fold_node] + 3] = 1; }},
      {"player array overflow", [&](auto& b) { b[fold] = 255; }},
      {"wrong player count", [&](auto& b) { b[fold] = 2; }},
      {"live array overflow", [&](auto& b) { b[fold + 1] = 255; }},
      {"zero live count", [&](auto& b) { b[fold + 1] = 0; }},
      {"fold flag", [&](auto& b) { b[fold + 2] = 2; }},
      {"board size overflow", [&](auto& b) { b[fold + 3] = 255; }},
      {"negative contribution", [&](auto& b) { put<std::int64_t>(b, fold + 4, -1); }},
      {"oversized stack",
       [&](auto& b) { put<std::int64_t>(b, fold + 20, std::numeric_limits<std::int64_t>::max()); }},
      {"excess refund", [&](auto& b) { put<std::int64_t>(b, fold + 12, 100); }},
      {"seat flag", [&](auto& b) { b[fold + 28] = 2; }},
      {"live index", [&](auto& b) { put<std::uint32_t>(b, live, 3); }},
      {"duplicate live seat",
       [&](auto& b) { put<std::uint32_t>(b, live + 4, get<std::uint32_t>(b, live)); }},
      {"board card", [&](auto& b) { put<std::int32_t>(b, board, 52); }},
      {"duplicate board card", [&](auto& b) { put<std::int32_t>(b, board + 4, 0); }},
      {"board mismatch", [&](auto& b) { put<std::int32_t>(b, board, 1); }},
      {"utility allocation overflow",
       [&](auto& b) {
         put<std::uint32_t>(b, fold_utilities, std::numeric_limits<std::uint32_t>::max());
       }},
      {"wrong fold payout", [&](auto& b) { put<std::int64_t>(b, fold_utilities + 4, 100); }},
      {"root-conserving forged terminal path",
       [&](auto& b) {
         // The forged seat retains its exact root total, live flags/cards are
         // unchanged, and all utilities match the forged pot. Only replaying
         // the actions reveals that these chips were never contributed.
         const auto seat_offset = fold + 4 + forged_loser * 32;
         put<std::int64_t>(b, seat_offset, fold_payload.seats[forged_loser].contributed + 1);
         put<std::int64_t>(b, seat_offset + 16, fold_payload.seats[forged_loser].stack - 1);
         std::int64_t forged_pot = 0;
         for (std::size_t seat = 0; seat < fold_payload.player_count; ++seat) {
           const auto at = fold + 4 + seat * 32;
           forged_pot += get<std::int64_t>(b, at) - get<std::int64_t>(b, at + 8);
         }
         for (std::size_t seat = 0; seat < fold_payload.player_count; ++seat) {
           const auto at = fold + 4 + seat * 32;
           const auto net = get<std::int64_t>(b, at) - get<std::int64_t>(b, at + 8);
           const auto utility = seat == fold_payload.live_order[0] ? forged_pot - net : -net;
           put<std::int64_t>(b, fold_utilities + 4 + seat * 8, utility);
         }
       }},
      {"truncated nodes", [](auto& b) { b.resize(50); }},
      {"truncated terminal", [](auto& b) { b.pop_back(); }},
      {"trailing bytes", [](auto& b) { b.push_back(0); }},
  };
  for (const auto& [name, mutate] : mutations) {
    auto bytes = raw;
    mutate(bytes);
    write_file(path, bytes);
    if (!rejects(path, def, action)) {
      std::printf("unexpected acceptance: %s\n", name);
      return 1;
    }
  }

  // Every short outer header must fail without underflow or uninitialized reads.
  for (std::size_t length = 0; length <= 16; ++length) {
    write_file(path, std::vector<char>(compressed.begin(), compressed.begin() + length));
    CHECK(rejects(path, def, action));
  }
  auto bytes = compressed;
  put<std::uint32_t>(bytes, 4, 1);
  write_file(path, bytes);
  CHECK(rejects(path, def, action));
  bytes = compressed;
  put<std::uint64_t>(bytes, 8, std::numeric_limits<std::uint64_t>::max());
  write_file(path, bytes);
  CHECK(rejects(path, def, action, {}, true));
  bytes = compressed;
  put<std::uint64_t>(bytes, 8, raw.size() + 1);
  write_file(path, bytes);
  CHECK(rejects(path, def, action));
  bytes = compressed;
  bytes.pop_back();
  write_file(path, bytes);
  CHECK(rejects(path, def, action));
  bytes = compressed;
  bytes.push_back(0);
  write_file(path, bytes);
  CHECK(rejects(path, def, action));

  write_file(path, raw);
  TreeLimits limits;
  limits.max_nodes = tree.size() - 1;
  CHECK(rejects(path, def, action, limits, true));
  limits = {};
  limits.max_depth = tree.depth() - 1;
  CHECK(rejects(path, def, action, limits, true));
  limits = {};
  limits.max_bytes = raw.size() - 1;
  CHECK(rejects(path, def, action, limits, true));
  CHECK(compressed.size() < limits.max_bytes);
  write_file(path, compressed);
  CHECK(rejects(path, def, action, limits, true));
  bytes = raw;
  put<std::uint64_t>(bytes, 20, std::numeric_limits<std::uint64_t>::max());
  write_file(path, bytes);
  CHECK(rejects(path, def, action, {}, true));
  // A forged low accounting header cannot bypass real slab accounting.
  bytes = raw;
  put<std::uint64_t>(bytes, 20, 0);
  write_file(path, bytes);
  limits.max_bytes = loaded.accounted_bytes() - 1;
  CHECK(limits.max_bytes > raw.size());
  CHECK(rejects(path, def, action, limits, true));
  limits.max_bytes = loaded.accounted_bytes();
  CHECK(verify_trees_match(tree, AbstractTree::load(path, def, action, limits)) == 0);
  std::printf("  malformed-input and resource-bound cases: OK (%zu mutations)\n", mutations.size());
  return 0;
}

int verify_chance_and_frontier(const TestFiles& files) {
  const auto action = abstraction::ActionAbstraction::identity();
  GameDef def{};
  def.player_count = 2;
  def.button = 1;
  def.big_blind = 1;
  def.contributions = {1, 1};
  def.pot = 2;
  def.board = {0, 1, 2};
  def.board_size = 3;
  // Zero stacks force a two-street runout with 49 then 48 chance children.
  const AbstractTree chance_tree(def, action);
  CHECK(chance_tree.node(0).is_chance());
  const auto path = files.dir / "chance.atf";
  chance_tree.save(path);
  CHECK(verify_trees_match(chance_tree, AbstractTree::load(path, def, action)) == 0);
  const auto raw = decompress(read_file(path));
  std::vector<std::size_t> offsets;
  std::size_t offset = 32;
  for (const auto& node : chance_tree.nodes()) {
    offsets.push_back(offset);
    offset += 24 + node.children.size() * 4 + node.actions.size() * 5;
  }
  const auto first = chance_tree.node(0).children[0];
  const auto second = chance_tree.node(0).children[1];
  auto bytes = raw;
  put<std::int32_t>(bytes, offsets[first] + 8, 0);
  write_file(path, bytes);
  CHECK(rejects(path, def, action));
  bytes = raw;
  put<std::int32_t>(bytes, offsets[second] + 8, chance_tree.node(first).incoming_card);
  write_file(path, bytes);
  CHECK(rejects(path, def, action));
  bytes = raw;
  put<std::uint32_t>(bytes, 32 + 16, 47);
  write_file(path, bytes);
  CHECK(rejects(path, def, action));
  bytes = raw;
  bytes[offsets[first] + 3] = 1;
  write_file(path, bytes);
  CHECK(rejects(path, def, action));

  def.stacks = {4, 4};
  def.terminal = TerminalDepth::Flop;
  const AbstractTree frontier(def, action);
  CHECK(frontier.node(0).is_action());
  bool saw_rooted_flop_deal = false;
  for (const auto& node : frontier.nodes())
    saw_rooted_flop_deal |= node.is_flop_deal();
  CHECK(saw_rooted_flop_deal);
  frontier.save(path);
  CHECK(verify_trees_match(frontier, AbstractTree::load(path, def, action)) == 0);
  def.preflop = true;
  def.board_size = 0;
  def.board = {};
  def.big_blind = 2;
  def.blinds_posted = {2, 1};
  def.contributions = {};
  def.pot = 3;
  const AbstractTree preflop(def, action);
  bool saw_flop_deal = false;
  for (const auto& node : preflop.nodes())
    saw_flop_deal |= node.is_flop_deal();
  CHECK(saw_flop_deal);
  preflop.save(path);
  CHECK(verify_trees_match(preflop, AbstractTree::load(path, def, action)) == 0);
  std::printf("  chance, frontier, and virtual flop-deal coverage: OK\n");
  return 0;
}

}  // namespace

int main() {
  std::printf("=== RFC 0014: ATF1 tree codec roundtrip ===\n");

  // Build a test tree.
  auto original = build_test_tree();
  std::printf("  built tree: %zu nodes, %zu terminals, depth %zu\n", original->size(),
              original->terminals().size(), original->depth());
  CHECK(original->size() > 0);

  // Save to a temp file.
  TestFiles files;
  const std::filesystem::path tmp_path = files.dir / "hu.atf";
  original->save(tmp_path);
  std::printf("  saved to %s\n", tmp_path.string().c_str());

  // Load it back.
  const std::array<int, 3> flop = {0, 1, 2};
  const GameDef def = make_postflop_def(flop, 0);
  const auto abstraction = make_postflop_abstraction(0);
  TreeLimits limits;
  limits.max_bytes = std::size_t{1} << 30;
  limits.max_nodes = 2'000'000;
  auto loaded = AbstractTree::load(tmp_path, def, abstraction, limits);
  std::printf("  loaded: %zu nodes, %zu terminals, depth %zu\n", loaded.size(),
              loaded.terminals().size(), loaded.depth());

  // Verify structure.
  if (verify_trees_match(*original, loaded) != 0)
    return 1;
  std::printf("  structure match: OK\n");
  CHECK(verify_rejections(files) == 0);
  CHECK(verify_chance_and_frontier(files) == 0);

  // Clean up.
  std::error_code ec;
  std::filesystem::remove(tmp_path, ec);

  std::printf("  ALL CHECKS PASSED\n");
  return 0;
}
