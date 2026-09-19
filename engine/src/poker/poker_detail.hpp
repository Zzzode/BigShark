// Shared internal helpers for the poker rules translation units.
//
// These three existed as byte-identical copies in heads_up.cpp and
// multiway.cpp, along with two byte-identical `contains` bodies and two
// field-identical action/legal struct pairs. RFC 0008 measured that the rules
// are duplicated rather than divergent, and this header is the smallest step
// that removes the copies without changing a single behavior: every body below
// is moved verbatim from one of the two files.
//
// Deliberately NOT a public header. These helpers throw on programmer error and
// are meaningless outside a transition implementation.
#pragma once

#include <array>
#include <bs/heads_up.hpp>
#include <cstddef>
#include <limits>
#include <stdexcept>

namespace bs::poker::detail {

inline Chips add(Chips left, Chips right) {
  if (right > std::numeric_limits<Chips>::max() - left)
    throw std::overflow_error("chip sum overflow");
  return left + right;
}

inline void require(bool condition, const char* message) {
  if (!condition)
    throw std::invalid_argument(message);
}

inline void use_card(int card, std::array<bool, 52>& used) {
  require(card >= 0 && card < 52, "card ID outside deck");
  require(!used[card], "duplicate card");
  used[card] = true;
}

// Next seat clockwise from `from` (exclusive). Moved from multiway.cpp, where
// it was already the general form; the heads-up rules reach their opponent with
// `1 - player` and do not need it.
inline std::size_t clockwise(std::size_t count, std::size_t from, std::size_t step = 1) {
  return (from + step) % count;
}

}  // namespace bs::poker::detail
