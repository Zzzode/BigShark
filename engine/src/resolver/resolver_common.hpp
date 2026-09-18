// Internal shared primitives for the RFC 0005 Stage 9 resolver. Private to
// bigshark_resolver; nothing here crosses the public resolver boundary.
#pragma once

#include <array>
#include <bs/heads_up.hpp>
#include <bs/heads_up_solver.hpp>
#include <bs/resolver.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace bs::resolver::detail {

using poker::Action;
using poker::ActionType;
using poker::Chips;
using poker::HeadsUpRoot;
using poker::HeadsUpState;
using poker::Phase;
using poker::Settlement;
using poker::Street;
using solver::HeadsUpGame;
using solver::InformationKey;
using solver::PolicyRow;
using solver::WeightedHand;

struct RequireFailure : std::invalid_argument {
  using std::invalid_argument::invalid_argument;
};

inline void require(bool ok, const char* message) {
  if (!ok)
    throw RequireFailure(message);
}

struct Exhausted {};

// RFC 0004-style bounded budget: node, infoset, depth, byte, and wall-clock
// limits with per-node cancellation. The deadline is checked inside traversal,
// not only between iterations.
struct Budget {
  ResolveLimits limits;
  std::chrono::steady_clock::time_point end;
  std::size_t nodes = 0;
  std::size_t bytes = 0;
  std::size_t peak_bytes = 0;

  explicit Budget(const ResolveLimits& value) : limits(value) {
    require(value.max_nodes > 0 && value.max_information_sets > 0 && value.max_depth > 0 &&
                value.max_bytes > 0 && value.time.count() > 0,
            "positive resolver resource limits required");
    end = std::chrono::steady_clock::now() + value.time;
  }

  void visit() {
    if (nodes == limits.max_nodes || std::chrono::steady_clock::now() >= end)
      throw Exhausted{};
    ++nodes;
  }
  void allocate(std::size_t count) {
    if (bytes > limits.max_bytes || count > limits.max_bytes - bytes)
      throw Exhausted{};
    bytes += count;
    peak_bytes = std::max(peak_bytes, bytes);
  }
};

// RFC 0004 PRNG revision 1 SplitMix64, with a fresh Stage-9 domain constant.
// Terminal-only resolves perform a deterministic full traversal and consume no
// entropy; the stream exists for sampled extensions and, more importantly, to
// pin a public-context-derived identity that can never read the actual hero
// hand. The domain constant separates resolver draws from trainer draws.
struct SplitMix64 {
  std::uint64_t state = 0;
  explicit SplitMix64(std::uint64_t seed) : state(seed ^ 0x52534c5652392d31ULL) {}
  std::uint64_t next_u64() {
    state += 0x9e3779b97f4a7c15ULL;
    std::uint64_t z = state;
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
  }
};

// One weighted joint private deal at the resolve node. Weight is the
// UNNORMALIZED counterfactual weight w(hero,responder); zero-weight pairs are
// not represented as deals.
struct GadgetDeal {
  std::array<int, 2> hero{};
  std::array<int, 2> responder{};
  double weight = 0;
};

// A responder "-x" gadget infoset: the responder's own sorted cards, its total
// counterfactual mass, the centered baseline margin, and the weighted deals
// that share the infoset (each a distinct hero holding).
struct ResponderInfoset {
  std::array<int, 2> cards{};
  double mass = 0;
  double baseline = 0;
  std::vector<std::size_t> deals;  // indices into the model deal vector
};

// Whether two cards share a rank/suit identity.
inline bool cards_conflict(std::array<int, 2> a, std::array<int, 2> b) {
  return a[0] == b[0] || a[0] == b[1] || a[1] == b[0] || a[1] == b[1];
}

inline bool hand_blocks_board(const std::array<int, 2>& hand, const std::vector<int>& board) {
  for (int card : board)
    if (card == hand[0] || card == hand[1])
      return true;
  return false;
}

// The cards the normal game would offer at a Deal phase for one deal, matching
// solver::public_cards exactly: a fixed slot is a singleton; otherwise every
// card not on the board, in either hand, or reserved for a later fixed slot.
std::vector<int> continuation_cards(const HeadsUpGame& game, const HeadsUpState& state,
                                    const std::array<std::array<int, 2>, 2>& hands);

// Expected RESPONDER net chip utility of the subtree AFTER the hero's current
// action, averaging only the remaining public chance. Fold/showdown use exact
// engine settlement. An action node anywhere below means the terminal-only
// precondition was violated; the caller reports ineligibility rather than
// consulting a heuristic leaf.
double continuation_utility_responder(const HeadsUpGame& game, const HeadsUpState& after,
                                      const std::array<std::array<int, 2>, 2>& hands,
                                      std::size_t responder, Budget& budget);

// True when every branch below `node` under the node's ordered abstract
// actions reaches Folded/Showdown through public cards alone, with no further
// action by either player. This is the RFC 0005 terminal-only graph property
// (lines 198-200), evaluated with one exact structural trace per action.
bool is_terminal_only(const HeadsUpGame& game, const HeadsUpState& node,
                      const std::vector<Action>& node_actions);

}  // namespace bs::resolver::detail
