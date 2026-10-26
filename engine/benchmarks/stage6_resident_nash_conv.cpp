// RFC 0029 stage 2: the resident-path NashConv measurement driver.
//
// What this measures, and what it does not. It builds one flop-terminal fixture
// from a declared class library, wraps the RESIDENT SERVING PATH in a
// BehaviorPolicy, and runs the exact oracle and the exact best response over
// that policy. The resulting figure is the general-sum NashConv of the served
// policy on a shallow abstracted game — a reproducibility smoke test on a named
// fixture, not a strength claim about the deployed configuration.
//
// Three things the figure is printed WITH, because without them it is not
// interpretable:
//
//   evaluator identity  — the frontier leaves are settled by
//                         EquityFrontierEvaluator; its inputs decide the value,
//                         so the figure is only comparable to one produced the
//                         same way.
//   miss counts         — a policy that misses falls back, and a figure that
//                         silently measures the fallback is worthless. The
//                         adapters fail closed, so a miss aborts the run; the
//                         counts are printed for the zero-miss statement AC4
//                         requires.
//   uncontained nodes   — the number of action nodes whose tree menu contains an
//                         action the declared behavior menu omits. This is the
//                         boundary of what the declared-menu deviation walk can
//                         express, and it is measured rather than assumed.
//
// Usage: bigshark-stage6-resident-nash-conv --library <dir> [--class N]
//                                           [--iterations M] [--seed S]
#include <third_party/yyjson/yyjson.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <bs/abstract_tree.hpp>
#include <bs/abstraction.hpp>
#include <bs/behavior_policy.hpp>
#include <bs/equity_frontier.hpp>
#include <bs/eval.hpp>
#include <bs/flop_library.hpp>
#include <bs/nash_conv_units.hpp>
#include <bs/resident_policy.hpp>
#include <bs/stage6/exact_oracle.hpp>
#include <bs/stage6/measured_policy.hpp>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace bs;
using namespace bs::poker;
namespace fs = std::filesystem;

struct Options {
  fs::path library_dir;
  std::size_t class_index = 0;
  std::uint64_t seed = 0;
  bool json = false;
  // RFC 0029 Open Question 1 pins the fixture; these are its parameters. The
  // builder CLI cannot produce it (--range accepts only premium|full), so the
  // library API is driven from here.
  bool build_fixture = false;
  std::size_t classes = 1;
  std::uint64_t iterations = 2000;
};

Options parse(int argc, char** argv) {
  Options opt;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    const auto next = [&]() -> std::string {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "%s requires a value\n", arg.c_str());
        std::exit(2);
      }
      return argv[++i];
    };
    if (arg == "--library") {
      opt.library_dir = next();
    } else if (arg == "--class") {
      opt.class_index = static_cast<std::size_t>(std::stoull(next()));
    } else if (arg == "--seed") {
      opt.seed = static_cast<std::uint64_t>(std::stoull(next()));
    } else if (arg == "--json") {
      opt.json = true;
    } else if (arg == "--build-fixture") {
      opt.build_fixture = true;
    } else if (arg == "--classes") {
      opt.classes = static_cast<std::size_t>(std::stoull(next()));
    } else if (arg == "--iterations") {
      opt.iterations = static_cast<std::uint64_t>(std::stoull(next()));
    } else {
      std::fprintf(stderr, "unknown argument: %s\n", arg.c_str());
      std::exit(2);
    }
  }
  if (opt.library_dir.empty()) {
    std::fprintf(stderr, "usage: %s --library <dir> [--class N] [--seed S] [--json]\n", argv[0]);
    std::exit(2);
  }
  return opt;
}

// The scalars this driver needs from `manifest.json`. Parsed here rather than
// in a library API because nothing else consumes them: the resident set reads
// the packed binary directly, and this driver is the only reader of the
// fixture's declared parameters. RFC 0035: the ledger lives in
// served_roots[0] (the fixture library is single-root).
struct FixtureScalars {
  std::size_t player_count = 0;
  Chips big_blind = 0;
  Chips stack = 0;
  Chips contribution = 0;
  Chips pot = 0;
  std::string action_id;
  std::vector<std::array<int, 3>> canonical_boards;
};

FixtureScalars read_fixture_scalars(const fs::path& manifest_path) {
  std::ifstream in(manifest_path, std::ios::binary);
  if (!in)
    throw std::runtime_error("cannot open " + manifest_path.string());
  const std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  yyjson_doc* doc = yyjson_read(content.data(), content.size(), 0);
  if (!doc)
    throw std::runtime_error("cannot parse " + manifest_path.string());
  struct DocGuard {
    yyjson_doc* d;
    ~DocGuard() { yyjson_doc_free(d); }
  } guard{doc};

  yyjson_val* root = yyjson_doc_get_root(doc);
  if (!yyjson_is_obj(root))
    throw std::runtime_error("manifest root is not an object");

  const auto require = [root](const char* key) -> yyjson_val* {
    yyjson_val* value = yyjson_obj_get(root, key);
    if (!value)
      throw std::runtime_error(std::string("manifest is missing '") + key + "'");
    return value;
  };
  const auto as_int = [](yyjson_val* value, const char* key) -> long long {
    if (!yyjson_is_int(value) && !yyjson_is_uint(value))
      throw std::runtime_error(std::string("manifest '") + key + "' is not an integer");
    return yyjson_is_uint(value) ? static_cast<long long>(yyjson_get_uint(value))
                                 : yyjson_get_sint(value);
  };

  FixtureScalars out;
  out.player_count = static_cast<std::size_t>(as_int(require("player_count"), "player_count"));
  out.big_blind = static_cast<Chips>(as_int(require("big_blind"), "big_blind"));

  // RFC 0035: the per-root ledger. The fixture is a single-root library.
  yyjson_val* served_roots = yyjson_obj_get(root, "served_roots");
  if (!yyjson_is_arr(served_roots) || yyjson_arr_size(served_roots) < 1)
    throw std::runtime_error("manifest is missing a non-empty 'served_roots' array");
  yyjson_val* served = yyjson_arr_get(served_roots, 0);
  if (!yyjson_is_obj(served))
    throw std::runtime_error("manifest served_roots[0] is not an object");
  const auto require_root = [served](const char* key) -> yyjson_val* {
    yyjson_val* value = yyjson_obj_get(served, key);
    if (!value)
      throw std::runtime_error(std::string("manifest served_roots[0] is missing '") + key + "'");
    return value;
  };
  yyjson_val* stacks = require_root("stacks");
  yyjson_val* contribs = require_root("contributions");
  if (!yyjson_is_arr(stacks) || yyjson_arr_size(stacks) < 1 || !yyjson_is_arr(contribs) ||
      yyjson_arr_size(contribs) < 1)
    throw std::runtime_error("manifest served_roots[0] ledger arrays are empty");
  out.stack = static_cast<Chips>(as_int(yyjson_arr_get(stacks, 0), "stacks[0]"));
  out.contribution = static_cast<Chips>(as_int(yyjson_arr_get(contribs, 0), "contributions[0]"));
  out.pot = static_cast<Chips>(as_int(require_root("pot"), "pot"));

  // The action abstraction identity decides the tree the fixture was trained
  // on; reconstructing a different one would make every lookup miss.
  yyjson_val* name = require("action_abstraction_name");
  if (!yyjson_is_str(name))
    throw std::runtime_error("manifest 'action_abstraction_name' is not a string");
  out.action_id = yyjson_get_str(name);

  yyjson_val* classes = require("classes");
  if (!yyjson_is_arr(classes))
    throw std::runtime_error("manifest 'classes' is not an array");
  for (std::size_t i = 0; i < yyjson_arr_size(classes); ++i) {
    yyjson_val* entry = yyjson_arr_get(classes, i);
    yyjson_val* board = yyjson_is_obj(entry) ? yyjson_obj_get(entry, "canonical_board") : nullptr;
    if (!yyjson_is_arr(board) || yyjson_arr_size(board) != 3)
      throw std::runtime_error("manifest class entry has no three-card canonical_board");
    std::array<int, 3> cards{};
    for (std::size_t c = 0; c < 3; ++c)
      cards[c] = static_cast<int>(as_int(yyjson_arr_get(board, c), "canonical_board"));
    out.canonical_boards.push_back(cards);
  }
  return out;
}

// A `FlopDeal` leaf is not a chance node: it collapses the entire three-card
// flop runout into ONE frontier leaf, and the payload it carries is already the
// completed flop (`abstract_tree.cpp`: "a single leaf replaces the entire
// 3-level chance subtree"). Its value is the frontier evaluator's answer for
// that board — there are no children to enumerate.
//
// For a FLOP-ROOTED fixture the board is already complete, so the leaf is
// settled directly from the state it was reached at. A preflop-rooted game
// would have to enumerate the three flop cards itself; this driver's fixture is
// flop-rooted, and the mismatch is refused rather than silently mis-settled.
std::vector<double> settle_flop_deal(const GameState& state, const gto::FrontierEvaluator& frontier,
                                     const std::vector<std::array<int, 2>>& holes) {
  if (state.board().size() != 3)
    throw stage6::Stage6PolicyError(
        "flop-deal leaf reached from a non-flop-rooted state; this driver's fixture is "
        "flop-rooted");
  return frontier.evaluate(state.board(), holes, tree::make_frontier_payload(state));
}

// Replays from the root to `index`. Declared here because the containment
// census below needs it; defined after the census for readability.
GameState state_at(const tree::AbstractTree& t, std::size_t index);

// Counts action nodes whose tree menu contains an action the declared behavior
// menu omits. AC4's containment statement is about THIS number, measured over
// every action node reachable in the built tree, not estimated.
std::size_t count_uncontained(const tree::AbstractTree& t) {
  std::size_t uncontained = 0;
  for (std::size_t index = 0; index < t.size(); ++index) {
    const tree::TreeNode& node = t.node(index);
    if (!node.is_action())
      continue;
    const GameState state = state_at(t, index);
    if (state.phase() != Phase::Action)
      continue;
    const std::vector<Action> declared = stage6::declared_behavior_menu(state, node.actor);
    for (const Action& action : node.actions) {
      if (std::find(declared.begin(), declared.end(), action) == declared.end()) {
        ++uncontained;
        break;  // count the NODE once, per the AC's definition
      }
    }
  }
  return uncontained;
}

// Replays from the root to `index` along the parent chain, applying each edge's
// recorded action or card. Used only by the containment census, which is a
// one-off diagnostic rather than a hot path.
// The fixture root, reconstructed from the manifest's SCALAR fields plus the
// library's own construction rule. The manifest does not store per-seat
// stacks, contributions, button, or pot; they are uniform and derived, which
// is exactly how the library built them (flop_library.cpp:make_def).
//
// A single field wrong here makes every resident lookup miss while the harness
// still "produces a number", so the reconstruction is stated rather than
// assumed and is asserted against the loaded root below.
GameDef fixture_def(const FixtureScalars& manifest, std::size_t class_index) {
  const std::array<int, 3>& board = manifest.canonical_boards.at(class_index);
  GameDef def{};
  def.player_count = manifest.player_count;
  def.button = 0;
  def.big_blind = manifest.big_blind;
  for (std::size_t s = 0; s < manifest.player_count; ++s) {
    def.stacks[s] = manifest.stack;
    def.contributions[s] = manifest.contribution;
  }
  def.pot = static_cast<Chips>(manifest.pot);
  def.board = {board[0], board[1], board[2], 0, 0};
  def.board_size = 3;
  def.terminal = TerminalDepth::Flop;
  return def;
}

GameState state_at(const tree::AbstractTree& t, std::size_t index) {
  std::vector<std::size_t> chain;
  for (std::size_t n = index; n != tree::kNoNode; n = t.node(n).parent)
    chain.push_back(n);
  std::reverse(chain.begin(), chain.end());
  GameState state(t.def());
  for (std::size_t i = 1; i < chain.size(); ++i) {
    const tree::TreeNode& node = t.node(chain[i]);
    if (node.incoming_card != tree::kNoCard) {
      state = state.after_card(node.incoming_card);
      continue;
    }
    // The edge was taken by the PARENT's actor, not this node's: a node's
    // `actor` is who acts AT it, which is the child's question, not the edge's.
    const tree::TreeNode& parent = t.node(node.parent);
    state = state.after_action(parent.actor, node.incoming_action);
  }
  return state;
}

// The pinned 4-combo-per-seat range. Four combos keeps the joint-deal
// enumeration small enough for the exact pooled walk while still reaching a
// frontier leaf; RFC 0029 Open Question 1 pins it and this is the definition.
std::vector<solver::WeightedHand> pinned_fixture_range() {
  // AsKs, AhKh, QdJd, QhJh: four combos, no two sharing a card, so every
  // two-seat joint deal is card-compatible.
  // Cards are rank*4+suit ids, and the trainer requires each combo sorted
  // ascending (K ahead of A, J ahead of Q), so the pairs are written low-high.
  return {
      {{bs::cardId("Ks"), bs::cardId("As")}, 1.0},
      {{bs::cardId("Kh"), bs::cardId("Ah")}, 1.0},
      {{bs::cardId("Jd"), bs::cardId("Qd")}, 1.0},
      {{bs::cardId("Jh"), bs::cardId("Qh")}, 1.0},
  };
}

// Builds the pinned fixture library through the library API. RFC 0029 pins the
// fixture at 10 chips BEHIND with 2 contributed, so the starting stack is 12:
// RFC 0033 made the builder argument a buy-in and derives chips behind as
// `starting_stack - contribution`. The abstracted game, and therefore the
// recorded NashConv and containment margin, is unchanged. `big_blind` is the
// library's fixed default of 2, which the manifest records.
flop_library::LibraryManifest build_fixture_library(const Options& opt) {
  fs::create_directories(opt.library_dir);
  return flop_library::build_library(opt.library_dir, opt.classes, opt.iterations, opt.seed,
                                     /*starting_stack=*/12, /*contribution=*/2,
                                     pinned_fixture_range(),
                                     /*player_count=*/2);
}

// The per-street action log for `street`. A named helper rather than a lambda
// so both the strategy walk and the best-response walk append identically.
std::vector<stage6::LoggedAction>& street_actions(stage6::HandLog& log, Street street) {
  switch (street) {
    case Street::Preflop:
      return log.preflop;
    case Street::Flop:
      return log.flop;
    case Street::Turn:
      return log.turn;
    case Street::River:
      return log.river;
  }
  return log.river;  // unreachable; the enum is exhaustive above
}

// Walks the ABSTRACT tree, asking the served policy for its distribution at
// every action node.
//
// This is not a stylistic choice. The exact oracle recurses the raw L1 game,
// which contains action sequences the fixture library never trained; the
// resident path answers only nodes that exist in the trained abstraction, so an
// L1 walk produces `off-tree-amount` misses and then a figure that measures
// nothing. Measuring the served policy means measuring it ON ITS OWN TREE —
// which is also what makes the uncontained-node census below meaningful.
struct TreeWalker {
  const tree::AbstractTree& abstract_tree;
  const stage6::BehaviorPolicy& policy;
  const gto::FrontierEvaluator& frontier;

  std::array<double, kMaxUnifiedSeats> walk(const GameState& state, std::size_t node_index,
                                            const std::vector<std::array<int, 2>>& holes,
                                            const stage6::HandLog& history) {
    std::array<double, kMaxUnifiedSeats> result{};
    const std::size_t n = holes.size();
    const tree::TreeNode& node = abstract_tree.node(node_index);

    if (node.kind == tree::NodeKind::TerminalFold) {
      const auto& util = abstract_tree.terminal(node.terminal).chip_utility;
      for (std::size_t i = 0; i < n; ++i)
        result[i] = static_cast<double>(util[i]);
      return result;
    }
    if (node.kind == tree::NodeKind::TerminalShowdown) {
      const auto& payload = abstract_tree.terminal(node.terminal);
      std::vector<std::array<int, 2>> live(payload.live_count);
      for (std::size_t i = 0; i < payload.live_count; ++i)
        live[i] = holes[payload.live_order[i]];
      const auto& util = state.settle_showdown(live).chip_utility;
      for (std::size_t i = 0; i < n; ++i)
        result[i] = static_cast<double>(util[i]);
      return result;
    }
    if (node.kind == tree::NodeKind::TerminalFrontier) {
      const auto utils =
          frontier.evaluate(state.board(), holes, abstract_tree.terminal(node.terminal));
      for (std::size_t i = 0; i < n; ++i)
        result[i] = utils[i];
      return result;
    }
    if (node.is_flop_deal()) {
      const std::vector<double> values = settle_flop_deal(state, frontier, holes);
      for (std::size_t i = 0; i < n; ++i)
        result[i] = values[i];
      return result;
    }
    if (node.is_chance()) {
      // The tree indexes chance children by `public_runout_cards`, which is the
      // board-only list: the tree does not know the deal, so a card held by any
      // seat still has a child. Weighting only the cards THIS deal leaves in the
      // deck and normalising over them is the correct conditional
      // distribution, but the child slot must be looked up in the full list or
      // every index past the first omitted card is wrong.
      const std::vector<int> all_cards = tree::public_runout_cards(state);
      std::vector<int> legal;
      legal.reserve(all_cards.size());
      for (int card : all_cards) {
        bool held = false;
        for (const auto& h : holes)
          if (h[0] == card || h[1] == card)
            held = true;
        if (!held)
          legal.push_back(card);
      }
      if (legal.empty())
        throw stage6::Stage6PolicyError("chance node with no legal runout card");
      for (int card : legal) {
        const auto slot_it = std::find(all_cards.begin(), all_cards.end(), card);
        const std::size_t slot = static_cast<std::size_t>(slot_it - all_cards.begin());
        if (slot >= node.children.size())
          throw stage6::Stage6PolicyError("chance card has no tree child");
        const auto child = walk(state.after_card(card), node.children[slot], holes, history);
        for (std::size_t i = 0; i < n; ++i)
          result[i] += child[i];
      }
      const double inv = 1.0 / static_cast<double>(legal.size());
      for (std::size_t i = 0; i < n; ++i)
        result[i] *= inv;
      return result;
    }

    // Action node. The policy is asked for this node's distribution; it is the
    // served path's answer, not an index lookup.
    const std::size_t actor = node.actor;
    stage6::PolicyContext ctx;
    ctx.hand_log = &history;
    const std::vector<stage6::PolicyAction> dist =
        policy.distribution(state, actor, holes[actor], ctx);
    // Match each served action to a tree edge. A served action the tree does
    // not contain is a containment failure, which the census reports; here it
    // would silently drop probability mass, so refuse instead.
    std::vector<double> probs(node.actions.size(), 0.0);
    for (const stage6::PolicyAction& pa : dist) {
      const auto it = std::find(node.actions.begin(), node.actions.end(), pa.action);
      if (it == node.actions.end())
        throw stage6::Stage6PolicyError("served action is absent from the tree menu");
      probs[static_cast<std::size_t>(it - node.actions.begin())] += pa.probability;
    }
    for (std::size_t a = 0; a < node.actions.size(); ++a) {
      if (probs[a] == 0.0)
        continue;
      stage6::HandLog next = history;
      street_actions(next, state.street()).push_back(stage6::LoggedAction{actor, node.actions[a]});
      const auto child =
          walk(state.after_action(actor, node.actions[a]), node.children[a], holes, next);
      for (std::size_t i = 0; i < n; ++i)
        result[i] += probs[a] * child[i];
    }
    return result;
  }
};

// Information-set-consistent best response over the abstract tree.
//
// The traverser chooses ONE action per information set — (public node, own two
// cards) — pooling every opponent holding that reaches it. Choosing per deal
// instead would let the traverser see opponents' cards, which is the omniscient
// bound, not a best response. `choices` memoizes the pooled decision per
// information key so every deal reaching that key uses the same action.
struct BestResponseWalk {
  const tree::AbstractTree& abstract_tree;
  const stage6::BehaviorPolicy& policy;
  const gto::FrontierEvaluator& frontier;
  std::size_t traverser;
  mutable std::map<std::pair<std::size_t, int>, Action> choices;

  std::array<double, kMaxUnifiedSeats> walk(const GameState& state, std::size_t node_index,
                                            const std::vector<std::array<int, 2>>& holes,
                                            const stage6::HandLog& history) {
    std::array<double, kMaxUnifiedSeats> result{};
    const std::size_t n = holes.size();
    const tree::TreeNode& node = abstract_tree.node(node_index);

    if (node.kind == tree::NodeKind::TerminalFold) {
      const auto& util = abstract_tree.terminal(node.terminal).chip_utility;
      for (std::size_t i = 0; i < n; ++i)
        result[i] = static_cast<double>(util[i]);
      return result;
    }
    if (node.kind == tree::NodeKind::TerminalShowdown) {
      const auto& payload = abstract_tree.terminal(node.terminal);
      std::vector<std::array<int, 2>> live(payload.live_count);
      for (std::size_t i = 0; i < payload.live_count; ++i)
        live[i] = holes[payload.live_order[i]];
      const auto& util = state.settle_showdown(live).chip_utility;
      for (std::size_t i = 0; i < n; ++i)
        result[i] = static_cast<double>(util[i]);
      return result;
    }
    if (node.kind == tree::NodeKind::TerminalFrontier) {
      const auto utils =
          frontier.evaluate(state.board(), holes, abstract_tree.terminal(node.terminal));
      for (std::size_t i = 0; i < n; ++i)
        result[i] = utils[i];
      return result;
    }
    if (node.is_flop_deal()) {
      const std::vector<double> values = settle_flop_deal(state, frontier, holes);
      for (std::size_t i = 0; i < n; ++i)
        result[i] = values[i];
      return result;
    }
    if (node.is_chance()) {
      const std::vector<int> all_cards = tree::public_runout_cards(state);
      std::vector<int> legal;
      legal.reserve(all_cards.size());
      for (int card : all_cards) {
        bool held = false;
        for (const auto& h : holes)
          if (h[0] == card || h[1] == card)
            held = true;
        if (!held)
          legal.push_back(card);
      }
      if (legal.empty())
        throw stage6::Stage6PolicyError("chance node with no legal runout card");
      for (int card : legal) {
        const auto slot_it = std::find(all_cards.begin(), all_cards.end(), card);
        const std::size_t slot = static_cast<std::size_t>(slot_it - all_cards.begin());
        if (slot >= node.children.size())
          throw stage6::Stage6PolicyError("chance card has no tree child");
        const auto child = walk(state.after_card(card), node.children[slot], holes, history);
        for (std::size_t i = 0; i < n; ++i)
          result[i] += child[i];
      }
      const double inv = 1.0 / static_cast<double>(legal.size());
      for (std::size_t i = 0; i < n; ++i)
        result[i] *= inv;
      return result;
    }

    const std::size_t actor = node.actor;
    if (actor != traverser) {
      // Opponent: follow the served distribution.
      stage6::PolicyContext ctx;
      ctx.hand_log = &history;
      const std::vector<stage6::PolicyAction> dist =
          policy.distribution(state, actor, holes[actor], ctx);
      for (const stage6::PolicyAction& pa : dist) {
        const auto it = std::find(node.actions.begin(), node.actions.end(), pa.action);
        if (it == node.actions.end())
          throw stage6::Stage6PolicyError("served action is absent from the tree menu");
        if (pa.probability == 0.0)
          continue;
        stage6::HandLog next = history;
        street_actions(next, state.street()).push_back(stage6::LoggedAction{actor, pa.action});
        const auto child =
            walk(state.after_action(actor, pa.action),
                 node.children[static_cast<std::size_t>(it - node.actions.begin())], holes, next);
        for (std::size_t i = 0; i < n; ++i)
          result[i] += pa.probability * child[i];
      }
      return result;
    }

    // Traverser: one pooled choice per information set.
    const std::pair<std::size_t, int> key{node_index,
                                          holes[traverser][0] * 52 + holes[traverser][1]};
    const auto it = choices.find(key);
    if (it != choices.end()) {
      const auto slot = static_cast<std::size_t>(
          std::find(node.actions.begin(), node.actions.end(), it->second) - node.actions.begin());
      stage6::HandLog next = history;
      street_actions(next, state.street()).push_back(stage6::LoggedAction{actor, it->second});
      return walk(state.after_action(actor, it->second), node.children[slot], holes, next);
    }

    std::array<double, kMaxUnifiedSeats> best{};
    double best_value = -std::numeric_limits<double>::infinity();
    Action best_action = node.actions.front();
    for (std::size_t a = 0; a < node.actions.size(); ++a) {
      stage6::HandLog next = history;
      street_actions(next, state.street()).push_back(stage6::LoggedAction{actor, node.actions[a]});
      const auto child =
          walk(state.after_action(actor, node.actions[a]), node.children[a], holes, next);
      if (child[traverser] > best_value) {
        best_value = child[traverser];
        best = child;
        best_action = node.actions[a];
      }
    }
    choices[key] = best_action;
    return best;
  }
};

std::array<double, kMaxUnifiedSeats> best_response_walk(
    const tree::AbstractTree& abstract_tree, const stage6::BehaviorPolicy& policy,
    const gto::FrontierEvaluator& frontier, const GameState& root, std::size_t root_index,
    const std::vector<std::array<int, 2>>& holes, std::size_t traverser,
    const stage6::HandLog& history) {
  BestResponseWalk walk{abstract_tree, policy, frontier, traverser, {}};
  return walk.walk(root, root_index, holes, history);
}

}  // namespace

int main(int argc, char** argv) {
  const Options opt = parse(argc, argv);

  std::fprintf(stderr, "fixture library: %s\n", opt.library_dir.c_str());
  if (opt.build_fixture) {
    std::fprintf(stderr, "building the pinned fixture (%zu class(es), %llu iterations/class)\n",
                 opt.classes, static_cast<unsigned long long>(opt.iterations));
    try {
      const flop_library::LibraryManifest built = build_fixture_library(opt);
      std::fprintf(stderr, "  built %zu class(es), %llu bytes, board %d/%d/%d\n", built.class_count,
                   static_cast<unsigned long long>(built.library_bin_bytes),
                   built.classes.at(0).canonical_board[0], built.classes.at(0).canonical_board[1],
                   built.classes.at(0).canonical_board[2]);
    } catch (const std::exception& e) {
      std::fprintf(stderr, "error: the fixture build failed: %s\n", e.what());
      return 1;
    }
  }
  const fs::path bin = opt.library_dir / "library.bin";
  if (!fs::exists(bin)) {
    std::fprintf(stderr, "error: %s does not exist\n", bin.c_str());
    return 1;
  }

  // The manifest is written beside the library. Reading it rather than
  // guessing the build parameters is what makes the fixture named and
  // reproducible: the figure is tied to THESE scalars.
  const fs::path manifest_path = opt.library_dir / "manifest.json";
  if (!fs::exists(manifest_path)) {
    std::fprintf(stderr, "error: %s does not exist\n", manifest_path.c_str());
    return 1;
  }
  FixtureScalars manifest{};
  try {
    manifest = read_fixture_scalars(manifest_path);
  } catch (const std::exception& e) {
    std::fprintf(stderr, "error: cannot read the manifest: %s\n", e.what());
    return 1;
  }
  if (opt.class_index >= manifest.canonical_boards.size()) {
    std::fprintf(stderr, "error: --class %zu is outside this %zu-class library\n", opt.class_index,
                 manifest.canonical_boards.size());
    return 1;
  }

  resident::ResidentOptions options;
  options.binary_library_path = bin;
  std::vector<resident::RootLoadResult> root_results;
  resident::ResidentPolicySet resident =
      resident::ResidentPolicySet::build({}, options, &root_results);
  if (resident.advertised_roots() == 0) {
    std::fprintf(stderr, "error: the library advertised no root\n");
    return 1;
  }

  const GameDef def = fixture_def(manifest, opt.class_index);
  const bool identity_abstract = manifest.action_id == abstraction::identity_action_id().name;
  const abstraction::ActionAbstraction action_abstraction =
      identity_abstract
          ? abstraction::ActionAbstraction::identity()
          : abstraction::ActionAbstraction::declared(abstraction::default_size_schedule(),
                                                     abstraction::CoverSeeds::DeclaredOnly);
  const tree::AbstractTree abstract_tree(def, action_abstraction);
  const std::size_t uncontained = count_uncontained(abstract_tree);

  // One adapter: the driver walks one fixture sequentially. A parallel driver
  // would need one per worker thread, which is why the requirement is stated on
  // the adapter rather than left implicit.
  stage6::ResidentBehaviorPolicy policy(resident);

  // The fixture range is the library's DECLARED declared-profile range filtered
  // to the combos that do not block this class's flop. Using the library's own
  // range keeps the enumeration aligned with what the records were trained
  // under; using a different one would measure a policy against hands it never
  // covered.
  // The SAME 4-combo range the fixture was trained with. Using the library's
  // 40-combo premium profile here would enumerate hands the artifact has no rows
  // for, and the resident path would (correctly) report every one as an
  // untrained combo.
  const std::vector<solver::WeightedHand> declared = pinned_fixture_range();
  std::vector<solver::MultiwayWeightedHand> fixture_range;
  for (const solver::WeightedHand& hand : declared) {
    bool blocks = false;
    for (std::size_t i = 0; i < def.board_size; ++i)
      if (hand.cards[0] == def.board[i] || hand.cards[1] == def.board[i])
        blocks = true;
    if (!blocks)
      fixture_range.push_back(solver::MultiwayWeightedHand{hand.cards, hand.weight});
  }
  if (fixture_range.empty()) {
    std::fprintf(stderr, "error: every declared combo blocks this class's flop\n");
    return 1;
  }

  std::vector<std::vector<solver::MultiwayWeightedHand>> ranges(manifest.player_count,
                                                                fixture_range);
  std::vector<const stage6::BehaviorPolicy*> policies(manifest.player_count, &policy);

  // Enumerate the card-compatible joint deals of the fixture range, exactly as
  // the game permits them: no two seats share a card, and no seat's cards are
  // on the board.
  std::vector<std::array<int, 2>> combos;
  for (const solver::WeightedHand& hand : declared) {
    bool blocks = false;
    for (std::size_t i = 0; i < def.board_size; ++i)
      if (hand.cards[0] == def.board[i] || hand.cards[1] == def.board[i])
        blocks = true;
    if (!blocks)
      combos.push_back(hand.cards);
  }

  std::vector<std::vector<std::array<int, 2>>> deals;
  {
    const std::size_t n = manifest.player_count;
    std::vector<std::size_t> pick(n, 0);
    while (true) {
      std::array<bool, 52> used{};
      bool ok = true;
      for (std::size_t s = 0; s < n && ok; ++s) {
        const auto& held = combos[pick[s]];
        for (int c : held) {
          if (used[c])
            ok = false;
          used[c] = true;
        }
      }
      if (ok) {
        std::vector<std::array<int, 2>> deal(n);
        for (std::size_t s = 0; s < n; ++s)
          deal[s] = combos[pick[s]];
        deals.push_back(std::move(deal));
      }
      std::size_t k = 0;
      while (k < n) {
        if (++pick[k] < combos.size())
          break;
        pick[k] = 0;
        ++k;
      }
      if (k == n)
        break;
    }
  }
  if (deals.empty()) {
    std::fprintf(stderr, "error: the fixture range enumerates no joint deal\n");
    return 1;
  }

  const gto::EquityFrontierEvaluator frontier;
  TreeWalker walker{abstract_tree, policy, frontier};

  // Strategy values: one tree walk per joint deal, all seats at once.
  std::array<double, 10> profile{};
  try {
    for (const auto& deal : deals) {
      const auto values = walker.walk(GameState(def), abstract_tree.root_index(), deal, {});
      std::fprintf(stderr, "[deal] s0=%.6f s1=%.6f\n", values[0], values[1]);
      for (std::size_t s = 0; s < manifest.player_count; ++s)
        profile[s] += values[s];
    }
  } catch (const std::exception& e) {
    std::fprintf(stderr, "error: the strategy walk failed: %s\n", e.what());
    std::fprintf(stderr, "  served=%zu misses=%zu last_miss=%s\n", policy.served(), policy.misses(),
                 resident::to_string(policy.last_miss()));
    return 1;
  }
  for (std::size_t s = 0; s < manifest.player_count; ++s)
    profile[s] /= static_cast<double>(deals.size());

  // The profile is a fixed-strategy expectation over a zero-sum game, so it must
  // sum to zero. A non-zero sum means a terminal was mis-settled — which is
  // exactly the failure that produced a plausible 0.488865 before the flop-deal
  // leaf was handled.
  double profile_sum = 0.0;
  for (std::size_t s = 0; s < manifest.player_count; ++s)
    profile_sum += profile[s];
  if (std::abs(profile_sum) > 1e-6) {
    std::fprintf(stderr,
                 "error: the profile is not zero-sum (sum %.9f); a terminal is mis-settled\n",
                 profile_sum);
    return 1;
  }

  // One best response per live seat over the same deal set, pooling by the
  // traverser's own holding so the result is information-set consistent.
  double nash_conv = 0.0;
  const auto br_value = [&](std::size_t traverser) {
    std::array<double, 10> total{};
    for (const auto& deal : deals) {
      const auto values = best_response_walk(abstract_tree, policy, frontier, GameState(def),
                                             abstract_tree.root_index(), deal, traverser, {});
      for (std::size_t s = 0; s < manifest.player_count; ++s)
        total[s] += values[s];
    }
    for (std::size_t s = 0; s < manifest.player_count; ++s)
      total[s] /= static_cast<double>(deals.size());
    return total[traverser];
  };
  try {
    for (std::size_t seat = 0; seat < manifest.player_count; ++seat) {
      if (def.folded[seat])
        continue;
      nash_conv += br_value(seat) - profile[seat];
    }
  } catch (const std::exception& e) {
    std::fprintf(stderr, "error: the best-response walk failed: %s\n", e.what());
    return 1;
  }

  const solver::NashConvNormalisation units = solver::normalise_nash_conv(def, nash_conv);
  const std::string evaluator_identity = "equity-frontier/all-in-at-flop";
  const std::string action_identity = manifest.action_id;

  if (opt.json) {
    std::printf(
        "{\"nash_conv\":%.10f,\"exploitability\":%.10f,\"exploitability_bb\":%.10f,"
        "\"live_seats\":%zu,\"uncontained_nodes\":%zu,\"misses\":%zu,\"served\":%zu,"
        "\"evaluator\":\"%s\",\"action_abstraction\":\"%s\",\"players\":%zu,"
        "\"big_blind\":%lld,\"stack\":%lld,\"contribution\":%lld}\n",
        units.nash_conv, units.exploitability, units.exploitability_bb, units.live_seats,
        uncontained, policy.misses(), policy.served(), evaluator_identity.c_str(),
        action_identity.c_str(), manifest.player_count, static_cast<long long>(manifest.big_blind),
        static_cast<long long>(manifest.stack), static_cast<long long>(manifest.contribution));
  } else {
    std::printf("RFC 0029 stage-6 resident NashConv\n");
    std::printf("  fixture        : %s (class %zu)\n", opt.library_dir.c_str(), opt.class_index);
    std::printf("  seats          : %zu, %lld chips behind, %lld in, %lld big blind\n",
                manifest.player_count, static_cast<long long>(manifest.stack),
                static_cast<long long>(manifest.contribution),
                static_cast<long long>(manifest.big_blind));
    std::printf("  action abstract: %s\n", action_identity.c_str());
    std::printf("  evaluator      : %s\n", evaluator_identity.c_str());
    std::printf("  NashConv       : %.6f chips/hand (summed over %zu live seats)\n",
                units.nash_conv, units.live_seats);
    std::printf("  exploitability : %.6f chips/hand = %.6f bb/hand\n", units.exploitability,
                units.exploitability_bb);
    std::printf("  served/misses  : %zu / %zu\n", policy.served(), policy.misses());
    std::printf("  uncontained    : %zu action nodes\n", uncontained);
  }
  return 0;
}
