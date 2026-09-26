// stage6/infoset_id.hpp — the two typed information-set identity domains.
//
// The MCCFR trainer addresses rows on the materialized/streamed ABSTRACT tree
// (AbstractInfosetKey: artifact + node/path + own card bucket). The deviation
// estimator and its frozen best response address rows on the EXACT game tree
// (ExactInfosetKey: public-history hash + the traverser's sorted own pair).
// They never mix; both use a content-derived 128-bit InfosetUuid, never a
// pointer, so artifacts are stable across processes.
#pragma once

#include <array>
#include <bs/abstraction.hpp>
#include <bs/behavior_policy.hpp>
#include <bs/game_definition.hpp>
#include <bs/stage6/geometry.hpp>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace bs::stage6 {

struct InfosetUuid {
  std::uint64_t hi = 0;
  std::uint64_t lo = 0;
  bool operator==(const InfosetUuid&) const = default;
  bool operator<(const InfosetUuid& other) const noexcept {
    return hi != other.hi ? hi < other.hi : lo < other.lo;
  }
  std::string to_string() const;
};

// Abstract-tree address used by the trainer and frozen candidate artifacts.
struct AbstractInfosetKey {
  // Content identity of the artifact this row belongs to (geometry matrix +
  // bucket + row-table hash). Zero for a key not yet sealed into an artifact.
  std::uint64_t artifact_content_hash = 0;
  // Materialized mode: TreeNode index. Streaming mode: kNoNode and the key is
  // carried by PublicPath tokens (hashed into uuid); node_index is still
  // recorded in materialized artifacts for direct lookup.
  std::size_t tree_node_index = 0;
  // Acting seat's own L2 card bucket (CategoryTiersV1).
  std::uint32_t own_card_bucket = 0;
  // Streaming-mode public path hash; ignored in materialized mode.
  std::uint64_t path_hash = 0;

  bool operator==(const AbstractInfosetKey&) const = default;
  bool operator<(const AbstractInfosetKey& o) const noexcept {
    if (artifact_content_hash != o.artifact_content_hash)
      return artifact_content_hash < o.artifact_content_hash;
    if (tree_node_index != o.tree_node_index)
      return tree_node_index < o.tree_node_index;
    if (own_card_bucket != o.own_card_bucket)
      return own_card_bucket < o.own_card_bucket;
    return path_hash < o.path_hash;
  }
  InfosetUuid uuid() const;
};

// Exact-game address used by the deviation estimator / frozen BR.
struct ExactInfosetKey {
  std::uint64_t public_history_hash = 0;
  std::array<int, 2> own_holding_sorted{};  // {min,max}; exact_oracle convention
  std::size_t traverser = 0;

  bool operator==(const ExactInfosetKey&) const = default;
  InfosetUuid uuid() const;
};

// One canonical public-history hash over the logged public actions and the
// board prefix, the same value the estimator Q table, CRN streams and frozen
// BR lookups use.
std::uint64_t public_history_hash(const HandLog& log, std::span<const int> board,
                                  poker::Street street);

}  // namespace bs::stage6
