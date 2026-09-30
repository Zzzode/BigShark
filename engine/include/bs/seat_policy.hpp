// RFC 0009 W2b: the concrete seat-indexed policy (D3).
//
// The v2 artifact's in-memory form. The n-seat trainer (W2a) keys its rows by
// `NSeatInformationKey{own_card_bucket, node_index}`: the bucket abstracts the
// acting seat's two private cards and the node index abstracts the concrete
// board value, so one tree serves every sampled flop of the rooted profile.
// The artifact instead stores CONCRETE combos (a real card0/card1 pair per
// row), so a step must expand each bucket-keyed row into the concrete holdings
// that map to it. That expansion needs the tree (to walk nodes and reconstruct
// each node's board and action path) and the card abstraction, both solver
// domain, so it lives here in the solver rather than at the artifact boundary
// (the deferral recorded at nseat_trainer.hpp:57-60).
//
// `SeatPolicy` is the seat-generic analogue of `HeadsUpPolicy`: it owns the
// game and action-abstraction identity, the per-seat ranges, the size schedule,
// and a map from a concrete `InformationKey` (actor = seat) to a policy row.
// The two are deliberately separate types so the two-seat route's artifacts
// and bytes cannot drift. `SeatTrainingResult` mirrors `NSeatTrainingResult`'s
// scalar report fields and is the value `create_checkpoint` persists as a v2
// artifact.
//
// The information key uses the SAME layout `information_key(HeadsUpState, ...)`
// produces -- [actor, card0, card1, board_count, board_ids..., (street, seat,
// type, target) per observed public action] -- built by `make_information_key`
// from a historyless GameState's components, so an n-seat request and a
// two-seat request for the same state produce the same key.
#pragma once

#include <bs/abstract_tree.hpp>
#include <bs/abstraction.hpp>
#include <bs/game_definition.hpp>
#include <bs/heads_up_solver.hpp>  // InformationKey, WeightedHand
#include <bs/nseat_trainer.hpp>  // NSeatTrainingResult, NSeatTerminationPhase, kNSeatAlgorithmRevision
#include <cstddef>
#include <cstdint>
#include <map>
#include <vector>

// RFC 0009 D3 artifact storage reconstructs an immutable SeatPolicy from a v2
// file through this one sanctioned assembler. The grant adds no mutator to the
// public facade and changes no solver behavior.
namespace bs::artifacts::detail {
class SeatPolicyAssembler;
}

namespace bs::solver {

// One concrete seat-indexed policy row: the ordered menu the tree node
// enumerated, the kFull average probabilities (or the sealed fallback the
// trainer recorded), and the visit count.
struct SeatPolicyRow {
  std::vector<poker::Action> actions;
  std::vector<double> probabilities;
  std::uint64_t visits = 0;
};

struct SeatTrainingResult;

// The concrete seat-indexed policy. Owns its identity and rows; getters return
// const references to the owned copies so a result is self-contained.
class SeatPolicy {
 public:
  const poker::GameDef& game() const noexcept { return game_; }
  const abstraction::SizeSchedule& sizes() const noexcept { return sizes_; }
  const std::vector<std::vector<WeightedHand>>& ranges() const noexcept { return ranges_; }
  const abstraction::AbstractionId& action_id() const noexcept { return action_id_; }
  const std::map<InformationKey, SeatPolicyRow>& rows() const noexcept { return rows_; }

 private:
  friend SeatTrainingResult export_seat_policy(
      const NSeatTrainingResult& result, const tree::AbstractTree& tree,
      const std::vector<std::vector<WeightedHand>>& ranges);
  // The artifact reader (RFC 0009 D3) reconstructs a SeatPolicy from a v2 file;
  // it is the one production consumer granted write access, mirroring
  // PolicyAssembler's relationship to HeadsUpPolicy.
  friend class ::bs::artifacts::detail::SeatPolicyAssembler;
  poker::GameDef game_{};
  abstraction::SizeSchedule sizes_{};
  std::vector<std::vector<WeightedHand>> ranges_{};
  abstraction::AbstractionId action_id_{};
  std::map<InformationKey, SeatPolicyRow> rows_;
};

// Result of exporting a trained n-seat policy to concrete seat-indexed rows.
// Mirrors `NSeatTrainingResult`'s scalar report fields (the export does no
// training, so the counts are the trainer's) and adds the abstraction identity
// and terminal depth the v2 artifact manifest records.
struct SeatTrainingResult {
  NSeatTerminationPhase termination = NSeatTerminationPhase::ResourceLimit;
  std::uint64_t completed_iterations = 0;
  std::size_t nodes = 0;
  std::size_t visits = 0;
  std::size_t information_sets = 0;
  std::size_t accounted_bytes = 0;
  std::uint64_t seed = 0;
  std::uint64_t prng_state = 0;
  std::uint32_t algorithm_revision = kNSeatAlgorithmRevision;
  abstraction::AbstractionId action_id{};
  poker::TerminalDepth terminal_depth = poker::TerminalDepth::River;
  SeatPolicy policy;
};

// Expands the trainer's bucket-keyed rows into concrete seat-indexed rows.
//
// Walks `tree` from the root carrying a `GameState` cursor (internal nodes
// store no board or ledger -- only leaves do -- so the cursor is advanced by
// each edge's incoming_action/incoming_card during the DFS) and the observed
// public-action path. At each Action node with acting seat `s`, for every
// concrete combo (card0, card1) off the node's board it computes the
// `CategoryTiersV1` bucket, looks up the sealed `(bucket, node_index)` row, and
// emits a concrete row keyed by `make_information_key(s, {card0,card1}, board,
// path)`. A bucket with no sealed row at that node is an exact miss (no row
// emitted), matching `NSeatPolicy::lookup`'s no-fill semantics.
//
// `ranges` is the per-seat range vector the trainer was trained with; it is
// carried into the result so the v2 artifact can persist it (the trainer's
// result type does not store ranges -- they are a training input). Throws
// std::invalid_argument if the tree and result disagree on game identity or
// action abstraction, or if a tree/result invariant is violated.
SeatTrainingResult export_seat_policy(const NSeatTrainingResult& result,
                                      const tree::AbstractTree& tree,
                                      const std::vector<std::vector<WeightedHand>>& ranges);

}  // namespace bs::solver
