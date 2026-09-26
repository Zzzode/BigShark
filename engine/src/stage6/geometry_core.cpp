// stage6/geometry_core.cpp — the pure-poker half of the R3b geometry surface.
//
// This translation unit depends only on L1 (GameState/GameDef) and the
// poker-only behavior vocabulary (HandLog is unused here; signatures read L1
// chip fields directly). It is compiled into bigshark_stage6_core so both the
// trainer (which must never reach the deployed policy) and the evaluation
// surface share one implementation of GeometrySignature, pot-scale bucketing,
// the reduced representative construction, flop_signature and GeometryCoverage.
//
// The chart/deviation-grid preflop ENUMERATORS (which drive the pinned chart
// through the policy-linked adapter) live in geometry_enumerator.cpp in
// bigshark_stage6_eval.
#include <algorithm>
#include <bs/stage6/geometry.hpp>
#include <cstdint>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace bs::stage6 {

std::string GeometrySignature::to_string() const {
  std::ostringstream os;
  os << "n" << player_count << " pot" << pot << " live{";
  for (std::size_t i = 0; i < live.size(); ++i)
    os << (i ? "," : "") << live[i];
  os << "} st{";
  for (std::size_t i = 0; i < player_count; ++i)
    os << (i ? "," : "") << stacks[i];
  os << "} in{";
  for (std::size_t i = 0; i < player_count; ++i)
    os << (i ? "," : "") << contributed[i];
  os << "}";
  return os.str();
}

// --------------------------------------------------- Revision 3 bucketing

std::string GeometryBucketKey::to_string() const {
  std::ostringstream os;
  os << "n" << player_count << " potbb" << pot_bb << " live" << live_count << " act"
     << acting_count;
  return os.str();
}

GeometryBucketKey bucket_key_for(const GeometrySignature& sig, poker::Chips big_blind) {
  GeometryBucketKey key;
  key.player_count = sig.player_count;
  // Round the exact pot to whole big blinds (nearest), quantizing the ≤1bb
  // pot-rounding confound declared in Revision 3.
  key.pot_bb = (sig.pot + big_blind / 2) / big_blind;
  for (std::size_t s : sig.live)
    if (sig.stacks[s] > 0)
      ++key.acting_count;
  key.live_count = sig.live.size();
  return key;
}

namespace {

// True if a live seat is all-in FOR LESS at the flop (it put in a partial
// stack that ended preflop with chips committed but none behind while others
// continue). At the uniform-100bb fixture the reviewer measured that acting
// members have equal ~97bb stacks and all-in members are full 200-chip jams;
// the no-mix guard rejects any bucket that combines the two classes.
bool has_all_in_for_less(const GeometrySignature& sig) {
  for (std::size_t s : sig.live) {
    const poker::Chips stack = sig.stacks[s];
    const poker::Chips in = sig.contributed[s];
    // A live seat with zero behind that contributed LESS than the deepest
    // live contributor is all-in for less (a shorter stack called/jammed).
    if (stack == 0) {
      poker::Chips max_in = 0;
      for (std::size_t t : sig.live)
        max_in = std::max(max_in, sig.contributed[t]);
      if (in > 0 && in < max_in)
        return true;
    }
  }
  return false;
}

}  // namespace

std::vector<GeometryBucket> bucket_geometries(std::size_t player_count, poker::Chips big_blind,
                                              const std::vector<GeometrySignature>& exact) {
  std::vector<GeometryBucket> buckets;
  // Index (not pointer) into `buckets`: emplace_back can reallocate, so a raw
  // pointer captured across the insert would dangle.
  auto find_bucket_index = [&](const GeometryBucketKey& key) -> long {
    for (std::size_t i = 0; i < buckets.size(); ++i)
      if (buckets[i].key == key)
        return static_cast<long>(i);
    return -1;
  };

  for (const GeometrySignature& sig : exact) {
    const GeometryBucketKey key = bucket_key_for(sig, big_blind);
    long index = find_bucket_index(key);
    if (index < 0) {
      GeometryBucket fresh;
      fresh.key = key;
      buckets.push_back(std::move(fresh));
      index = static_cast<long>(buckets.size()) - 1;
    }
    buckets[static_cast<std::size_t>(index)].members.push_back(sig);
  }

  for (GeometryBucket& b : buckets) {
    b.big_blind = big_blind;
    b.actionable = b.key.acting_count >= 1;
    // No-mix guard: an actionable bucket must not contain an all-in-for-less
    // live seat (those form their own acting_count-shaped bucket, but mixing
    // them with continuing members would put illegal targets in the tree).
    for (const GeometrySignature& sig : b.members)
      if (b.actionable && has_all_in_for_less(sig))
        b.mixed_all_in_for_less = true;
    if (b.mixed_all_in_for_less)
      throw std::runtime_error("geometry bucket mixes all-in-for-less with acting members: " +
                               b.key.to_string());
    if (!b.actionable)
      continue;

    // Minimum acting depth and minimum equal acting contribution across
    // members. In every real hand that reaches a flop, each non-folded,
    // non-all-in live seat has called the SAME preflop-close total, so a
    // member's acting contributions are equal; take the shallowest member's
    // total. The reduced game has no folded seat to carry the folded blinds
    // that pad a rounded bucket pot, and fabricating an uneven split (e.g.
    // in{6,7,7}) would put a single contributor on the 6-chip settlement
    // layer, which the rules reject as an unmatched contribution.
    bool first_member = true;
    bool have_min_stack = false;
    poker::Chips min_stack = 0;
    poker::Chips equal_contrib = 0;
    for (const GeometrySignature& member_sig : b.members) {
      poker::Chips member_contrib = 0;
      bool member_first = true;
      for (std::size_t s : member_sig.live) {
        if (member_sig.stacks[s] == 0)
          continue;  // all-in live seat: excluded from the reduced acting game
        if (!have_min_stack || member_sig.stacks[s] < min_stack) {
          min_stack = member_sig.stacks[s];
          have_min_stack = true;
        }
        if (member_first || member_sig.contributed[s] < member_contrib)
          member_contrib = member_sig.contributed[s];
        member_first = false;
      }
      if (member_first)
        throw std::runtime_error("actionable bucket member has no acting seat: " +
                                 b.key.to_string());
      if (first_member || member_contrib < equal_contrib)
        equal_contrib = member_contrib;
      first_member = false;
    }
    b.min_acting_stack = min_stack;

    // Reduced representative: live_count seats, each contributing the same
    // legal total; acting seats carry the minimum stack behind and any fully
    // committed seat keeps zero behind. The pot reconciles EXACTLY to the sum
    // of those equal contributions (a legal, realizable rooted game). It is
    // deliberately NOT forced to the rounded bucket pot: the residual is the
    // folded-dead/blind quantization the Revision-3 bucketed-vs-exact delta
    // test measures, and must never be invented into illegal chip layers.
    const std::size_t live = b.key.live_count;
    const std::size_t acting = b.key.acting_count;
    b.representative_stacks.assign(live, 0);
    b.representative_contrib.assign(live, 0);
    for (std::size_t i = 0; i < live; ++i) {
      if (i < acting)
        b.representative_stacks[i] = min_stack;
      b.representative_contrib[i] = equal_contrib;
    }
    b.representative_pot = equal_contrib * static_cast<poker::Chips>(live);
  }

  std::sort(buckets.begin(), buckets.end(), [](const GeometryBucket& a, const GeometryBucket& c) {
    return a.key.to_string() < c.key.to_string();
  });
  (void)player_count;
  return buckets;
}

GeometrySignature flop_signature(const poker::GameState& state) {
  if (state.board().size() != 3)
    throw std::invalid_argument("flop_signature requires a three-card flop");
  if (state.phase() != poker::Phase::Action && state.phase() != poker::Phase::Deal &&
      state.phase() != poker::Phase::Showdown && state.phase() != poker::Phase::Folded)
    throw std::invalid_argument("flop_signature: unrecognized phase");
  GeometrySignature sig;
  sig.player_count = state.def().player_count;
  sig.pot = state.pot();
  for (std::size_t p = 0; p < sig.player_count; ++p) {
    sig.stacks[p] = state.players()[p].stack;
    sig.contributed[p] = state.players()[p].contributed;
    if (!state.players()[p].folded)
      sig.live.push_back(p);
  }
  return sig;
}

namespace {

// 64-bit FNV-1a over a byte string (independent local copy so the coverage
// hash does not depend on the L2 abstraction digest helper).
std::uint64_t fnv1a_local(const std::string& bytes) {
  std::uint64_t hash = 0xcbf29ce484222325ULL;
  for (unsigned char byte : bytes) {
    hash ^= byte;
    hash *= 0x100000001b3ULL;
  }
  return hash;
}

}  // namespace

GeometryCoverage::GeometryCoverage(std::vector<GeometrySignature> covered)
    : covered_(std::move(covered)) {
  std::sort(covered_.begin(), covered_.end(),
            [](const GeometrySignature& a, const GeometrySignature& b) {
              return a.to_string() < b.to_string();
            });
  covered_.erase(std::unique(covered_.begin(), covered_.end()), covered_.end());
}

bool GeometryCoverage::covers(const GeometrySignature& sig) const {
  const std::string key = sig.to_string();
  const auto it = std::lower_bound(
      covered_.begin(), covered_.end(), key,
      [](const GeometrySignature& s, const std::string& k) { return s.to_string() < k; });
  return it != covered_.end() && it->to_string() == key;
}

GeometrySignature GeometryCoverage::require_covered(const GeometrySignature& sig) const {
  if (!covers(sig))
    throw stage6_geometry_uncovered("simulator reached an uncovered flop geometry: " +
                                    sig.to_string());
  return sig;
}

std::uint64_t GeometryCoverage::content_hash() const noexcept {
  std::string canonical;
  for (const GeometrySignature& sig : covered_)
    canonical += sig.to_string() + '\n';
  return fnv1a_local(canonical);
}

std::uint64_t geometry_bucket_token(const GeometryBucketKey& key) {
  return fnv1a_local(key.to_string());
}

}  // namespace bs::stage6
