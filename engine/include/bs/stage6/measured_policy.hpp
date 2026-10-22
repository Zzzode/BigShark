// RFC 0029 stage 2: BehaviorPolicy adapters over the two policy sources the
// stage-6 walks can measure.
//
// The walks speak one interface — `stage6::BehaviorPolicy`, a distribution over
// concrete legal actions at (state, seat, hole). Neither native source speaks
// it: `solver::NSeatPolicy` answers by node index through a trained tree, and
// `resident::ResidentPolicySet` answers by running its full serving path over a
// public-action history. These adapters are the seam, and they are the reason
// the measurement is of the SERVED path rather than of a lookup that happens to
// sit beneath it.
//
// Both adapters FAIL CLOSED. A miss is an exception, never a fallback policy:
// silently substituting uniform or check/call would let a harness publish a
// NashConv figure that measures the fallback while claiming to measure the
// policy. AC4 requires a zero miss count partly because this turns the
// distinction into an error instead of a number.
//
// Offline only: this header lives on the stage-6 eval surface, which the
// offline guard keeps out of the service, both wire protocols, and the host.
#pragma once

#include <array>
#include <bs/behavior_policy.hpp>
#include <bs/nseat_trainer.hpp>
#include <bs/prng.hpp>
#include <bs/resident_policy.hpp>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace bs::gto {
class FrontierEvaluator;
}

namespace bs::tree {
class AbstractTree;
}

namespace bs::stage6 {

// Thrown for any answer the underlying policy source cannot produce. Carries
// the source's own reason where one exists, so a driver can report WHY a
// fixture missed rather than only that it did.
class Stage6PolicyError : public std::runtime_error {
 public:
  explicit Stage6PolicyError(const std::string& what) : std::runtime_error(what) {}
};

// Adapts a trained `solver::NSeatPolicy` to the walk interface.
//
// The tree is carried because node addressing is not derivable from cards: the
// policy's rows are keyed by `NSeatInformationKey`, and `NSeatPolicy::lookup`
// resolves one by walking the tree in child order until a node's observable
// state matches. Without the tree the adapter could not ask the question.
//
// Not thread-safe in the sense the walks need: `lookup` is const and holds no
// mutable state, so one adapter may be shared across threads. The per-thread
// requirement in this RFC belongs to the resident adapter, whose serving path
// writes a scratch buffer.
class NSeatBehaviorPolicy final : public BehaviorPolicy {
 public:
  // Both references must outlive the adapter. `policy.game()` must equal
  // `tree`'s definition and the action identities must agree; `lookup` itself
  // reports a mismatch as an exact miss, which this adapter turns into a throw.
  NSeatBehaviorPolicy(const solver::NSeatPolicy& policy, const tree::AbstractTree& tree);

  std::vector<PolicyAction> distribution(const poker::GameState& state, std::size_t seat,
                                         HoleCards hole,
                                         const PolicyContext& context) const override;

  // Answers served since construction, and the number the source could not
  // answer. A driver reports both; AC3 requires the miss count be zero on the
  // named fixture rather than merely small.
  std::size_t served() const noexcept { return served_; }
  std::size_t misses() const noexcept { return misses_; }

 private:
  const solver::NSeatPolicy& policy_;
  const tree::AbstractTree& tree_;
  mutable std::size_t served_ = 0;
  mutable std::size_t misses_ = 0;
};

// Adapts the resident serving path to the walk interface.
//
// This calls `hero_decision`, NOT `find_bucket` or any narrower index query.
// The distinction is the point of the RFC: an adapter over the index would
// measure blueprint storage, while `hero_decision` runs the public-belief
// propagation, the hero-private blocker filter, and the canonical relabel that
// a live decision actually pays for. A figure produced the other way would not
// describe what the server does.
//
// THREAD SAFETY: `hero_decision` writes a ~340 KiB `ResidentScratch`. The
// driver parallelizes across fixture deals, so each worker thread must own an
// adapter (or at least its own scratch). A shared instance would data-race, and
// the sanitizer build is expected to catch it — that is why the requirement is
// stated here rather than discovered later.
class ResidentBehaviorPolicy final : public BehaviorPolicy {
 public:
  // `resident` must outlive the adapter and must not be moved while it lives:
  // `ResidentAnswer` carries pointers into the set's immutable storage.
  // `pinned_sha256`, when set, restricts answers to that one artifact.
  ResidentBehaviorPolicy(const resident::ResidentPolicySet& resident,
                         std::optional<std::string_view> pinned_sha256 = std::nullopt);

  std::vector<PolicyAction> distribution(const poker::GameState& state, std::size_t seat,
                                         HoleCards hole,
                                         const PolicyContext& context) const override;

  std::size_t served() const noexcept { return served_; }
  std::size_t misses() const noexcept { return misses_; }
  // The last miss reason, as the resident path named it. Meaningful only after
  // a miss; `misses()` counts them.
  resident::MissReason last_miss() const noexcept { return last_miss_; }

 private:
  const resident::ResidentPolicySet& resident_;
  std::optional<std::string_view> pinned_sha256_;
  // One scratch per adapter instance, which is one per worker thread by
  // contract. Mutable because `distribution` is const on the interface.
  mutable resident::ResidentScratch scratch_{};
  mutable std::size_t served_ = 0;
  mutable std::size_t misses_ = 0;
  mutable resident::MissReason last_miss_ = resident::MissReason::None;
};

// Builds the `PublicAction` history `hero_decision` expects from a stage-6
// `HandLog`. The two describe the same public record in different shapes: the
// log is per-street (`preflop`, `flop`, `turn`, `river`) while the resident path
// consumes one ordered span tagged by street.
//
// Exposed because the conversion is specified rather than assumed (RFC 0029):
// the order is streets ascending, and within a street the recorded order.
std::vector<poker::PublicAction> public_action_history(const HandLog& log);

}  // namespace bs::stage6
