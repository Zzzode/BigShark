// stage6/public_path.hpp — the streaming trainer's public-information cursor.
//
// At live>=3 the coarse tree is too large to materialize, so the trainer
// addresses information sets by a deterministic token path instead of a node
// pointer. The path starts at one geometry token and appends one uint16 per
// public edge: an action edge encodes (acting seat, menu ordinal), a chance
// edge encodes 0x8000 | board-only card ordinal. Trailing action enumeration
// uses push/pop; chance and sampled-opponent edges advance once.
//
// At live=2 the SAME walk resolves against the materialized TreeNode index;
// the dual-cursor alignment gate proves the streaming path and the tree index
// identify the same node. This header carries no tree type.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace bs::stage6 {

class PublicPath {
 public:
  explicit PublicPath(std::uint64_t geometry_token) { reset(geometry_token); }

  void reset(std::uint64_t geometry_token = 0) {
    tokens_.clear();
    tokens_.push_back(static_cast<std::uint16_t>(geometry_token & 0xffffULL));
    tokens_.push_back(static_cast<std::uint16_t>((geometry_token >> 16) & 0xffffULL));
    tokens_.push_back(static_cast<std::uint16_t>((geometry_token >> 32) & 0xffffULL));
    tokens_.push_back(static_cast<std::uint16_t>((geometry_token >> 48) & 0xffffULL));
  }

  // Action edge: seat<=9 (5 bits) and menu ordinal <32 (5 bits).
  void on_action(std::size_t seat, std::size_t menu_ordinal) {
    tokens_.push_back(static_cast<std::uint16_t>((seat << 5) | menu_ordinal));
  }
  // Chance edge: high bit marks it a card, low bits the board-only ordinal
  // (<=51). Distinct from every action token (whose high bit is 0).
  void on_chance(std::size_t board_ordinal) {
    tokens_.push_back(static_cast<std::uint16_t>(0x8000u | board_ordinal));
  }
  // Replays one previously recorded raw edge token (used by iterative tree
  // replayers that snapshot the token vector rather than holding a call stack).
  void append_edge_token(std::uint16_t token) { tokens_.push_back(token); }
  // Pops one edge (used while the traverser enumerates its menu actions).
  void pop() {
    // Never pop the four geometry-prefix tokens.
    if (tokens_.size() > 4)
      tokens_.pop_back();
  }

  const std::vector<std::uint16_t>& tokens() const noexcept { return tokens_; }

  std::uint64_t hash() const noexcept;
  std::string to_string() const;

 private:
  std::vector<std::uint16_t> tokens_;
};

}  // namespace bs::stage6
