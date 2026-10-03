// RFC 0007: frontier evaluator tests.
//
// Tests the DeclaredFrontierTable (option A): canonical keying, lookup,
// fail-closed missing entries, and construction validation.
#include <bs/frontier.hpp>

#include <cstdio>
#include <stdexcept>
#include <vector>

#define CHECK(cond)                                                         \
  do {                                                                      \
    if (!(cond)) {                                                          \
      std::printf("CHECK failed: %s (%s:%d)\n", #cond, __FILE__, __LINE__); \
      return 1;                                                             \
    }                                                                       \
  } while (0)

namespace {

using namespace bs::gto;
using bs::tree::TerminalPayload;

// Hardcoded card ids matching the engine's encoding (rank*4+suit, 0=2c).
constexpr int k2h = 2, k3h = 6, k4h = 10;
constexpr int k5c = 12, k5d = 13, k5h = 14;
constexpr int kAc = 48, kAh = 50, kAs = 51;
constexpr int kKc = 44, kKh = 46, kKs = 47;

TerminalPayload make_ledger() {
  TerminalPayload tp;
  tp.player_count = 2;
  tp.seats[0].contributed = 2;
  tp.seats[1].contributed = 2;
  tp.seats[0].stack = 48;
  tp.seats[1].stack = 48;
  tp.live_count = 2;
  tp.live_order = {0, 1, 0, 0, 0, 0, 0, 0, 0, 0};
  tp.board = {k2h, k3h, k4h, 0, 0};
  tp.board_size = 3;
  return tp;
}

int test_basic_lookup() {
  // Build a table with one entry: flop 2h3h4h, seat0=AsKs, seat1=AhKh.
  DeclaredFrontierTable::Entry e;
  e.flop = {k2h, k3h, k4h};
  e.hands = {{{kAs, kKs}, {kAh, kKh}}};
  e.values = {1.5, -1.5};
  std::vector<DeclaredFrontierTable::Entry> entries{e};
  DeclaredFrontierTable table(std::move(entries));
  CHECK(table.size() == 1);

  const TerminalPayload ledger = make_ledger();
  const std::array<int, 3> flop = {k2h, k3h, k4h};
  const std::array<std::array<int, 2>, 2> hands = {{{kAs, kKs}, {kAh, kKh}}};
  std::vector<double> result = table.evaluate(flop, hands, ledger);
  CHECK(result.size() == 2);
  CHECK(result[0] == 1.5);
  CHECK(result[1] == -1.5);
  return 0;
}

int test_canonical_keying() {
  // The same (flop, joint deal) with cards in different order must produce
  // the same lookup. The constructor sorts, and evaluate() sorts its input.
  DeclaredFrontierTable::Entry e;
  e.flop = {k4h, k2h, k3h};  // unsorted
  e.hands = {{{kKs, kAs}, {kKh, kAh}}};  // unsorted within seats
  e.values = {2.0, -2.0};
  std::vector<DeclaredFrontierTable::Entry> entries{e};
  DeclaredFrontierTable table(std::move(entries));

  const TerminalPayload ledger = make_ledger();
  // Query with sorted input.
  const std::array<int, 3> flop = {k2h, k3h, k4h};
  const std::array<std::array<int, 2>, 2> hands = {{{kAs, kKs}, {kAh, kKh}}};
  std::vector<double> result = table.evaluate(flop, hands, ledger);
  CHECK(result[0] == 2.0);
  CHECK(result[1] == -2.0);

  // Query with unsorted input (must still hit the same entry).
  const std::array<int, 3> flop_unsorted = {k3h, k2h, k4h};
  const std::array<std::array<int, 2>, 2> hands_unsorted = {
      {{kKs, kAs}, {kAh, kKh}}};
  result = table.evaluate(flop_unsorted, hands_unsorted, ledger);
  CHECK(result[0] == 2.0);
  CHECK(result[1] == -2.0);
  return 0;
}

int test_missing_entry_throws() {
  DeclaredFrontierTable table({});
  const TerminalPayload ledger = make_ledger();
  const std::array<int, 3> flop = {k2h, k3h, k4h};
  const std::array<std::array<int, 2>, 2> hands = {{{kAs, kKs}, {kAh, kKh}}};
  bool threw = false;
  try {
    (void)table.evaluate(flop, hands, ledger);
  } catch (const std::runtime_error&) {
    threw = true;
  }
  CHECK(threw);
  return 0;
}

int test_wrong_seat_count_throws() {
  DeclaredFrontierTable::Entry e;
  e.flop = {k2h, k3h, k4h};
  e.hands = {{{kAs, kKs}, {kAh, kKh}}};
  e.values = {1.0, -1.0};
  DeclaredFrontierTable table({e});

  const TerminalPayload ledger = make_ledger();
  const std::array<int, 3> flop = {k2h, k3h, k4h};
  // 3 seats — frontier evaluation is heads-up only.
  const std::array<std::array<int, 2>, 3> hands3 = {
      {{kAs, kKs}, {kAh, kKh}, {kAc, kKc}}};
  bool threw = false;
  try {
    (void)table.evaluate(flop, hands3, ledger);
  } catch (const std::invalid_argument&) {
    threw = true;
  }
  CHECK(threw);
  return 0;
}

int test_wrong_flop_size_throws() {
  DeclaredFrontierTable::Entry e;
  e.flop = {k2h, k3h, k4h};
  e.hands = {{{kAs, kKs}, {kAh, kKh}}};
  e.values = {1.0, -1.0};
  DeclaredFrontierTable table({e});

  const TerminalPayload ledger = make_ledger();
  const std::array<int, 2> flop2 = {k2h, k3h};
  const std::array<std::array<int, 2>, 2> hands = {{{kAs, kKs}, {kAh, kKh}}};
  bool threw = false;
  try {
    (void)table.evaluate(flop2, hands, ledger);
  } catch (const std::invalid_argument&) {
    threw = true;
  }
  CHECK(threw);
  return 0;
}

int test_duplicate_entry_throws() {
  DeclaredFrontierTable::Entry e1;
  e1.flop = {k2h, k3h, k4h};
  e1.hands = {{{kAs, kKs}, {kAh, kKh}}};
  e1.values = {1.0, -1.0};
  DeclaredFrontierTable::Entry e2 = e1;
  e2.values = {2.0, -2.0};
  bool threw = false;
  try {
    DeclaredFrontierTable table({e1, e2});
    (void)table;
  } catch (const std::invalid_argument&) {
    threw = true;
  }
  CHECK(threw);
  return 0;
}

int test_multiple_entries() {
  // Two different flops with the same hands must not collide.
  DeclaredFrontierTable::Entry e1;
  e1.flop = {k2h, k3h, k4h};
  e1.hands = {{{kAs, kKs}, {kAh, kKh}}};
  e1.values = {1.0, -1.0};
  DeclaredFrontierTable::Entry e2;
  e2.flop = {k5c, k5d, k5h};
  e2.hands = {{{kAs, kKs}, {kAh, kKh}}};
  e2.values = {3.0, -3.0};
  DeclaredFrontierTable table({e1, e2});
  CHECK(table.size() == 2);

  const TerminalPayload ledger = make_ledger();
  const std::array<std::array<int, 2>, 2> hands = {{{kAs, kKs}, {kAh, kKh}}};
  const std::array<int, 3> flop1 = {k2h, k3h, k4h};
  const std::array<int, 3> flop2 = {k5c, k5d, k5h};
  std::vector<double> r1 = table.evaluate(flop1, hands, ledger);
  std::vector<double> r2 = table.evaluate(flop2, hands, ledger);
  CHECK(r1[0] == 1.0);
  CHECK(r2[0] == 3.0);
  return 0;
}

}  // namespace

int main() {
  if (test_basic_lookup() != 0)
    return 1;
  if (test_canonical_keying() != 0)
    return 1;
  if (test_missing_entry_throws() != 0)
    return 1;
  if (test_wrong_seat_count_throws() != 0)
    return 1;
  if (test_wrong_flop_size_throws() != 0)
    return 1;
  if (test_duplicate_entry_throws() != 0)
    return 1;
  if (test_multiple_entries() != 0)
    return 1;
  std::printf("test_frontier_table PASS\n");
  return 0;
}
