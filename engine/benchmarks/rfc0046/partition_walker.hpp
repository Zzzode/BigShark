// RFC 0046 Stage-1 measurement-only per-infoset NashConv partition walker.
//
// Exact chance-free enumeration (blocker-aware joint deals, all legal river
// cards) of a two-seat turn-rooted tree, returning the telescoping
// best-response partition from harness section 5.3: for traverser action
// infoset I,
//
//   g(I) = own_prefix_H(I) * (max_a PV_BR(a) - sum_a sigma_H(a) PV_BR(a))
//
// with BR-tail child values pooled over opponent holdings, accumulated with
// the FULL H reach (opponent/chance view reach times the traverser's own
// H-prefix probability). The increments telescope exactly to
// u(BR) - u(H); the walker independently returns the all-H root value and
// the BR root value so callers can assert the identity per player within
// 1e-9 root-pot.
//
// Labeling (harness 5.3 total ordered rule): extension-only nodes
// (cap-1 mapping absent, reached only through a zero-H-probability branch)
// -> T_action with identically zero g; a mapped node whose BR argmax is a
// cap-0-only action -> T_action; otherwise node street selects T_trunk /
// T_boundary / T_river (T_trunk is the pinned zero in a turn-rooted tree).
//
// Benchmark-only.
#pragma once

#include <array>
#include <bs/abstract_tree.hpp>
#include <bs/game_definition.hpp>
#include <bs/heads_up_solver.hpp>
#include <cstdint>
#include <map>
#include <span>
#include <vector>

#include "concrete_keyer.hpp"

namespace bs::rfc0046 {

enum class PartitionTerm { Trunk = 0, Boundary = 1, River = 2, Action = 3 };

// Behavior policy over the EVALUATION tree (tree0). Implementations return a
// probability per menu0 action for the acting seat's concrete holding;
// actions that exist only in tree0 (cap-0 extension branches) must get
// probability 0 from a cap-1 hybrid policy.
class PartitionPolicy {
 public:
  virtual ~PartitionPolicy() = default;
  virtual void probabilities(std::size_t seat, std::size_t node0, std::int64_t node1,
                             const std::array<int, 2>& own, const std::vector<poker::Action>& menu0,
                             std::span<double> out) const = 0;
};

struct PartitionInput {
  const tree::AbstractTree* tree0 = nullptr;  // evaluation game
  // cap-1 reference tree and the node0 -> node1 mapping; when node1_map is
  // null every tree0 node is treated as a cap-1 node.
  const tree::AbstractTree* tree1 = nullptr;
  const std::vector<std::int64_t>* node1_map = nullptr;
  const ConcreteCardKeyer* keyer = nullptr;
  std::array<std::vector<bs::solver::WeightedHand>, 2> ranges;
  const PartitionPolicy* policy = nullptr;
};

struct PartitionResult {
  // SUMMED chance-free NashConv over both players (each normalized by its own
  // deal mass, then summed). nash_conv_bb = nash_conv_chip / big_blind: the
  // frozen summed-BB convention (addendum §5/§8, RFC 0045 gate precedent),
  // NOT the per-live-seat exploitability (no seat divisor).
  double nash_conv_chip = 0.0;
  double nash_conv_bb = 0.0;
  std::array<double, 4> terms_chip{};
  std::array<double, 4> terms_bb{};  // summed-BB: chip term / big_blind
  double extension_mass_chip = 0.0;  // T_action mass at extension-only nodes; pinned 0
  // Per-player identity residual |(BR-H) - sum g| in root-pot units; the
  // gate uses the max over players.
  std::array<double, 2> residual_pot{};
  std::size_t joint_deals = 0;
};

// Builds the node0 -> node1 path mapping (public action/card edges); a tree0
// node absent from the cap-1 tree maps to -1.
std::vector<std::int64_t> map_cap1_nodes(const tree::AbstractTree& tree0,
                                         const tree::AbstractTree& tree1);

// Runs the partition walk for both seats.
PartitionResult partition_nash_conv(const PartitionInput& input);

}  // namespace bs::rfc0046
