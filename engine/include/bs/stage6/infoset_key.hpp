// stage6/infoset_key.hpp — public-history information-set identity for the
// offline stage-6 best-response estimator and MCCFR trainer.
//
// A best response is one action per INFORMATION SET: the traverser's own two
// cards plus the public history (board cards dealt so far and the public
// action log). The key MUST NOT contain an opponent's or folder's hole cards,
// cards not yet dealt, or the random seed — including any of those would let
// the deviation condition on information the player cannot observe, which is
// exactly the omniscient/Jensen bias E_z[max_a Q] >= max_a E_z[Q] that the
// exact best response was corrected to avoid. Views with the same own pair and
// the same public history collapse to ONE key and therefore ONE frozen action.
//
// The key is an explicit canonical token VECTOR (compared lexicographically),
// not a 64-bit hash, so two distinct information sets can never collide and
// pool by accident. The FNV-1a content hash is diagnostic / digest only.
#pragma once

#include <array>
#include <bs/behavior_policy.hpp>
#include <bs/game_definition.hpp>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace bs::stage6 {

// Canonical public-history token stream for a state: length-prefixed so no two
// histories can frame to the same bytes. Layout, in order:
//   [board size][board card id+1 in deal order...]
//   then for street in Preflop, Flop, Turn, River:
//   [street tag][action count][ (seat+1, action-type+1, target_total+1)... ]
// Appending the acting player's OWN sorted hole pair yields the InfosetKey.
std::vector<std::uint64_t> public_history_tokens(const poker::GameState& state, const HandLog& log);

struct InfosetKey {
  // The acting player's own two card ids, sorted ascending. Order-independent
  // (AsKs and KsAs are the same holding).
  std::array<int, 2> own{};
  std::vector<std::uint64_t> public_tokens;

  bool operator==(const InfosetKey&) const = default;
  bool operator<(const InfosetKey& other) const noexcept;
  std::string canonical() const;
  // FNV-1a over the canonical encoding; diagnostics and frozen-table digest
  // only — never used to decide key equality.
  std::uint64_t content_hash() const noexcept;
};

// Builds the information-set key at `state` for `traverser` holding `own`.
// Reads only state.board(), the public `log`, and the traverser's own pair.
InfosetKey make_infoset_key(const poker::GameState& state, const HandLog& log,
                            std::size_t traverser, HoleCards own);

// FNV-1a hash of the length-framed public-history token stream at a node. This
// is the content-address ("public node token") the CRN streams key on: two
// rollout legs on the same public node draw the same value, and legs whose
// public history differs draw independently and re-pair on reconvergence.
// Diagnostic/digest use only; never used for infoset equality.
std::uint64_t public_history_hash(const poker::GameState& state, const HandLog& log);

}  // namespace bs::stage6
