// RFC 0046 Stage-1: exact per-combo ConcreteCardKeyer tests.
//
// Pins the measurement-only dense key: bijection of live holdings onto
// 0..C(48,2)-1 on a four-card turn board, blocker semantics, board4-only
// keying (the river card never changes a bucket), and the seam cap. This is
// a native engine test; it does not grep source text.
#include <array>
#include <bs/eval.hpp>
#include <cstdio>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include "../benchmarks/rfc0046/concrete_keyer.hpp"

#define CHECK(cond)                                                         \
  do {                                                                      \
    if (!(cond)) {                                                          \
      std::printf("CHECK failed: %s (%s:%d)\n", #cond, __FILE__, __LINE__); \
      return false;                                                         \
    }                                                                       \
  } while (0)

namespace {
using bs::rfc0046::ConcreteCardKeyer;
using bs::rfc0046::kConcreteBucketCap;
using bs::rfc0046::live_unblocked_holdings;

int card(const char* name) {
  return bs::cardId(std::string(name));
}

bool test_cap_and_density() {
  const std::array<int, 4> board{card("2c"), card("3d"), card("7h"), card("Js")};
  ConcreteCardKeyer keyer(board);
  CHECK(keyer.bucket_cap() == kConcreteBucketCap);
  CHECK(kConcreteBucketCap == 1128);
  CHECK(keyer.live_cards().size() == 48);

  const auto holdings = live_unblocked_holdings(board);
  CHECK(holdings.size() == 1128);

  std::set<std::uint32_t> seen;
  std::uint32_t expected = 0;
  for (const auto& h : holdings) {
    const std::uint32_t b = keyer.bucket(h, board);
    CHECK(b < kConcreteBucketCap);
    CHECK(seen.insert(b).second);
    // live_unblocked_holdings enumerates the same dense order.
    CHECK(b == expected);
    // Inverse map round-trips.
    const auto back = keyer.holding(b);
    CHECK(back[0] == h[0] && back[1] == h[1]);
    ++expected;
  }
  CHECK(seen.size() == 1128);
  CHECK(expected == 1128);
  return true;
}

bool test_injectivity_and_blockers() {
  const std::array<int, 4> board{card("Qs"), card("Ks"), card("As"), card("Js")};
  ConcreteCardKeyer keyer(board);
  // Every distinct live holding maps to a distinct bucket (exact keying).
  const auto holdings = live_unblocked_holdings(board);
  CHECK(holdings.size() == 1128);
  for (std::size_t i = 0; i < holdings.size(); ++i)
    for (std::size_t j = i + 1; j < holdings.size(); ++j)
      CHECK(keyer.bucket(holdings[i], board) != keyer.bucket(holdings[j], board));

  // Holdings sharing a board card are not live and the keyer refuses them.
  bool threw = false;
  try {
    (void)keyer.bucket({card("Qs"), card("2c")}, board);
  } catch (const std::invalid_argument&) {
    threw = true;
  }
  CHECK(threw);

  // Duplicate board cards are rejected at construction (49 "live" cards).
  threw = false;
  try {
    ConcreteCardKeyer bad(std::array<int, 4>{card("Qs"), card("Qs"), card("As"), card("Js")});
  } catch (const std::invalid_argument&) {
    threw = true;
  }
  CHECK(threw);

  // Out-of-range inverse.
  threw = false;
  try {
    (void)keyer.holding(1128);
  } catch (const std::invalid_argument&) {
    threw = true;
  }
  CHECK(threw);
  return true;
}

bool test_board4_only_and_known_values() {
  const std::array<int, 4> board4{card("Qs"), card("Ks"), card("As"), card("Js")};
  ConcreteCardKeyer keyer(board4);

  // With the river added, every live holding keeps its exact turn-board id.
  const std::array<int, 5> board5{card("Qs"), card("Ks"), card("As"), card("Js"), card("9c")};
  const auto turn_holdings = live_unblocked_holdings(board4);
  const auto river_holdings = live_unblocked_holdings(board5);
  CHECK(river_holdings.size() == 47 * 46 / 2);  // C(47,2) = 1081
  for (const auto& h : river_holdings) {
    const std::uint32_t at_turn = keyer.bucket(h, board4);
    const std::uint32_t at_river = keyer.bucket(h, board5);
    CHECK(at_turn == at_river);
  }
  // The river card blocks one extra holding set, but surviving ids are
  // unchanged (they index the four-card board universe).
  CHECK(turn_holdings.size() - river_holdings.size() == 47);

  // Known first live pair: suit order is s=0,h=1,d=2,c=3, so the smallest
  // cards on the spade-heavy board are 2s(id 0) and 2h(id 1); they carry
  // dense id 0.
  CHECK(card("2s") == 0);
  CHECK((keyer.holding(0) == std::array<int, 2>{card("2s"), card("2h")}));
  CHECK(keyer.dense_for_live_pair({card("2s"), card("2h")}) == 0);

  // Mismatched board prefix is rejected.
  bool threw = false;
  try {
    const std::array<int, 4> other{card("2c"), card("3d"), card("7h"), card("Js")};
    (void)keyer.bucket({card("2c"), card("3c")}, other);
  } catch (const std::invalid_argument&) {
    threw = true;
  }
  CHECK(threw);
  return true;
}

bool test_seam_interface() {
  const std::array<int, 4> board{card("7s"), card("Kh"), card("Ad"), card("Js")};
  const ConcreteCardKeyer keyer(board);
  // Usable through the measurement seam interface.
  const bs::solver::NSeatCardKeyerSeam& seam = keyer;
  CHECK(seam.bucket_cap() == 1128);
  const std::array<int, 2> h{card("2c"), card("3c")};
  CHECK(seam.bucket(h, board) == keyer.bucket(h, board));
  return true;
}

}  // namespace

int main() {
  const std::vector<std::pair<const char*, bool (*)()>> cases{
      {"cap_and_density", test_cap_and_density},
      {"injectivity_and_blockers", test_injectivity_and_blockers},
      {"board4_only_and_known_values", test_board4_only_and_known_values},
      {"seam_interface", test_seam_interface},
  };
  for (const auto& [name, fn] : cases) {
    if (!fn()) {
      std::printf("rfc0046_concrete_keyer: FAIL %s\n", name);
      return 1;
    }
    std::printf("rfc0046_concrete_keyer: pass %s\n", name);
  }
  std::printf("rfc0046_concrete_keyer: all tests passed\n");
  return 0;
}
