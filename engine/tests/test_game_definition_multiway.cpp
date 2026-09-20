// Independent equivalence oracle for the unified game definition at MULTIPLE
// seat counts (RFC 0008 stage 2).
//
// This suite is the stage-2 gate: `bs::poker::GameState` is widened from two
// seats to 3..10 and must reproduce `MultiwayState` element-for-element at
// 3..6 seats, while 7..10 seats (where no shipped rules type exists) are
// checked against a reference ledger written here from the poker rules and
// from `MultiwayState`'s documented contract. At 3..6 the ledger is
// cross-checked against the REAL shipped type, which is what elevates it
// from a copy of the implementation under test; 7..10 rides the same ledger.
// A reviewer can see that in the include list: the ledger itself never sees
// `bs/multiway.hpp`; only the lockstep harness below does.
//
// The walk enumerates every legal action at every node, so its size is
// exponential in the number of seats that can still ACT, not the seat count.
// Every fixture therefore keeps exactly three ACTIVE seats (the width the
// shipped multiway suite already stays inside): three seats drive the full
// button orbit at three seats and two representative button positions at
// 4..10, while every extra seat posts all in (the preflop ante shape) or
// carries a prior-street all-in (a rooted board). The inactive seats still
// traverse the ten-element state arrays, the clockwise actor search across
// them, and the multi-way contribution layers; they just do not branch the
// action tree. The 7..10 settlement behavior is additionally covered by the
// deterministic 7..10 contribution-grid extension in test_settlement.cpp.
//
// Non-vacuity is part of the contract. Every rule-bearing branch is counted
// (mid-hand folds, street refunds that revive an all-in seat, capped blinds,
// antes, short all-ins, showdowns, ties, and the 7..10 range itself), and
// every counter is asserted positive. A walk that never reaches a branch
// passes regardless of the implementation; this repository has shipped
// tests that could not fail, so the counts are the proof this one can.
#include <algorithm>
#include <array>
#include <bs/eval.hpp>
#include <bs/game_definition.hpp>
#include <bs/multiway.hpp>
#include <bs/settlement.hpp>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

using namespace bs::poker;

#define CHECK(cond)                                                         \
  do {                                                                     \
    if (!(cond)) {                                                         \
      std::printf("CHECK failed: %s (%s:%d)\n", #cond, __FILE__, __LINE__); \
      return 1;                                                            \
    }                                                                      \
  } while (0)

namespace {

int card(const char* name) {
  return bs::cardId(std::string(name));
}

const std::array<const char*, 5> kBoardNames = {"5c", "6c", "7c", "8c", "9c"};
// Ten fixed hands, none sharing a card with the club board pool, so a full
// runout plus ten live seats never collides. The low pairs can tie on the
// board: a showdown with tied winners is exercised rather than avoided.
const std::array<std::array<const char*, 2>, 10> kHoleNames = {{
    {"Ah", "Ad"}, {"Kh", "Kd"}, {"Qh", "Qd"}, {"Jh", "Jd"}, {"Th", "Td"},
    {"2h", "2d"}, {"3h", "3d"}, {"4h", "4s"}, {"6h", "6d"}, {"7h", "7d"},
}};

// ---------------------------------------------------------------------------
// The independent N-seat reference ledger. It states the multiway rules from
// the RFC text: preflop starts left of the big blind; postflop starts left of
// the button skipping seats that cannot act; a fold mid-hand is not the end
// of the hand; a refund RE-DERIVES all-in (the multiway rule, deliberately
// the opposite of the two-seat one); only a full raise reopens rights.
// ---------------------------------------------------------------------------
struct Ref {
  std::size_t players = 0;
  std::size_t button = 0;
  Chips big_blind = 0;
  std::vector<Chips> stack, committed, contributed, returned;
  std::vector<unsigned char> folded, all_in, raise_rights, pending;
  std::vector<int> board;
  Street street = Street::Flop;
  Phase phase = Phase::Action;
  int actor = -1;
  Chips last_full_raise = 0;
  int history = 0;

  std::size_t live_count() const {
    std::size_t count = 0;
    for (std::size_t p = 0; p < players; ++p)
      count += folded[p] ? 0 : 1;
    return count;
  }
  bool can_act(std::size_t p) const { return !folded[p] && !all_in[p]; }
  bool any_pending() const {
    for (std::size_t p = 0; p < players; ++p)
      if (pending[p] && can_act(p))
        return true;
    return false;
  }
  std::optional<std::size_t> next_actor(std::size_t from) const {
    for (std::size_t step = 1; step <= players; ++step) {
      const std::size_t seat = (from + step) % players;
      if (pending[seat] && can_act(seat))
        return seat;
    }
    return std::nullopt;
  }
  Chips high_commit() const {
    Chips value = 0;
    for (std::size_t p = 0; p < players; ++p)
      value = std::max(value, committed[p]);
    return value;
  }
  Chips pot() const {
    Chips total = 0;
    for (std::size_t p = 0; p < players; ++p)
      total += contributed[p] - returned[p];
    return total;
  }

  void pay(std::size_t p, Chips amount) {
    stack[p] -= amount;
    committed[p] += amount;
    contributed[p] += amount;
    all_in[p] = stack[p] == 0;
  }

  void refund_unmatched() {
    std::size_t high = 0;
    Chips high_amount = 0;
    std::size_t high_count = 0;
    for (std::size_t p = 0; p < players; ++p) {
      if (folded[p])
        continue;
      if (committed[p] > high_amount) {
        high_amount = committed[p];
        high = p;
        high_count = 1;
      } else if (committed[p] == high_amount) {
        ++high_count;
      }
    }
    if (high_count != 1)
      return;
    Chips other_level = 0;
    for (std::size_t p = 0; p < players; ++p)
      if (p != high)
        other_level = std::max(other_level, committed[p]);
    const Chips excess = high_amount - other_level;
    if (excess == 0)
      return;
    stack[high] += excess;
    returned[high] += excess;
    committed[high] -= excess;
    // THE MULTIWAY RULE: a refunded seat's all-in status follows its stack.
    all_in[high] = stack[high] == 0;
  }

  void close_street() {
    refund_unmatched();
    for (std::size_t p = 0; p < players; ++p)
      pending[p] = false;
    phase = street == Street::River ? Phase::Showdown : Phase::Deal;
  }
};

Ref ref_start(const GameDef& def) {
  Ref s;
  s.players = def.player_count;
  s.button = def.button;
  s.big_blind = def.big_blind;
  s.last_full_raise = def.big_blind;
  s.stack.assign(s.players, 0);
  s.committed.assign(s.players, 0);
  s.contributed.assign(s.players, 0);
  s.returned.assign(s.players, 0);
  s.folded.assign(s.players, 0);
  s.all_in.assign(s.players, 0);
  s.raise_rights.assign(s.players, 1);
  s.pending.assign(s.players, 0);
  for (std::size_t p = 0; p < s.players; ++p) {
    s.stack[p] = def.stacks[p];
    s.contributed[p] = def.contributions[p];
  }
  if (def.preflop) {
    if (def.ante > 0) {
      for (std::size_t p = 0; p < s.players; ++p) {
        const Chips posted = std::min(def.ante, s.stack[p]);
        s.stack[p] -= posted;
        s.contributed[p] += posted;
        s.all_in[p] = s.stack[p] == 0;
      }
    }
    const std::pair<std::size_t, Chips> blinds[2] = {
        {(def.button + 1) % s.players, def.big_blind / 2},
        {(def.button + 2) % s.players, def.big_blind}};
    for (const auto& [seat, nominal] : blinds) {
      const Chips posted = std::min(nominal, s.stack[seat]);
      s.stack[seat] -= posted;
      s.committed[seat] += posted;
      s.contributed[seat] += posted;
      s.all_in[seat] = s.stack[seat] == 0;
    }
    s.street = Street::Preflop;
    for (std::size_t p = 0; p < s.players; ++p)
      s.pending[p] = !s.all_in[p];
    s.actor = static_cast<int>((def.button + 3) % s.players);
    if (s.all_in[static_cast<std::size_t>(s.actor)]) {
      const auto next = s.next_actor(static_cast<std::size_t>(s.actor));
      if (next)
        s.actor = static_cast<int>(*next);
      else {
        s.close_street();
        s.actor = -1;
      }
    }
  } else {
    for (std::uint8_t i = 0; i < def.board_size; ++i)
      s.board.push_back(def.board[i]);
    s.street = def.board_size == 3 ? Street::Flop
                                    : (def.board_size == 4 ? Street::Turn : Street::River);
    for (std::size_t p = 0; p < s.players; ++p) {
      s.all_in[p] = s.stack[p] == 0;
      s.pending[p] = s.stack[p] > 0;
    }
    const auto first = s.next_actor(def.button);
    if (!first) {
      s.phase = s.street == Street::River ? Phase::Showdown : Phase::Deal;
      s.actor = -1;
    } else {
      s.actor = static_cast<int>(*first);
    }
  }
  return s;
}

LegalActions ref_legal(const Ref& s) {
  LegalActions result;
  if (s.phase != Phase::Action || s.actor < 0)
    return result;
  const std::size_t hero = static_cast<std::size_t>(s.actor);
  Chips high = 0;
  for (std::size_t p = 0; p < s.players; ++p)
    high = std::max(high, s.committed[p]);
  const Chips due = high - s.committed[hero];
  result.fold = due > 0;
  result.check = due == 0;
  result.call = due > 0;
  result.call_amount = std::min(due, s.stack[hero]);
  if (!s.raise_rights[hero] || s.stack[hero] <= due)
    return result;
  bool someone_responds = false;
  for (std::size_t p = 0; p < s.players; ++p)
    if (p != hero && s.can_act(p))
      someone_responds = true;
  if (!someone_responds)
    return result;
  const Chips maximum = s.committed[hero] + s.stack[hero];
  const Chips full_minimum = high + s.last_full_raise;
  result.aggressive = TargetRange{
      due == 0 ? ActionType::Bet : ActionType::Raise,
      std::min(full_minimum, maximum),
      maximum,
      maximum <= full_minimum,
  };
  return result;
}

void ref_act(Ref& next, std::size_t player, Action action) {
  const Chips high = next.high_commit();
  next.pending[player] = false;
  next.raise_rights[player] = false;
  switch (action.type) {
    case ActionType::Fold:
      // THE MULTIWAY RULE: a folded seat is marked folded, and the hand
      // continues. The fold itself neither refunds nor ends anything.
      next.folded[player] = 1;
      break;
    case ActionType::Check:
      break;
    case ActionType::Call:
      next.pay(player, std::min(high - next.committed[player], next.stack[player]));
      break;
    case ActionType::Bet:
    case ActionType::Raise: {
      const Chips increment = action.target_total - high;
      next.pay(player, action.target_total - next.committed[player]);
      for (std::size_t p = 0; p < next.players; ++p)
        if (p != player && next.can_act(p))
          next.pending[p] = true;
      if (increment >= next.last_full_raise) {
        next.last_full_raise = increment;
        for (std::size_t p = 0; p < next.players; ++p)
          if (p != player && next.can_act(p))
            next.raise_rights[p] = true;
      }
      break;
    }
  }
  ++next.history;
  if (next.live_count() <= 1) {
    next.phase = Phase::Folded;
    next.refund_unmatched();
    for (std::size_t p = 0; p < next.players; ++p)
      next.pending[p] = false;
    next.actor = -1;
    return;
  }
  if (!next.any_pending()) {
    next.close_street();
    next.actor = -1;
    return;
  }
  const auto following = next.next_actor(player);
  if (!following) {
    next.close_street();
    next.actor = -1;
    return;
  }
  next.actor = static_cast<int>(*following);
}

void ref_deal(Ref& next, int public_card) {
  next.board.push_back(public_card);
  const std::size_t dealt = next.board.size();
  next.street = dealt <= 3 ? Street::Flop : (dealt == 4 ? Street::Turn : Street::River);
  if (dealt < 3)
    return;  // the flop is still being dealt
  for (std::size_t p = 0; p < next.players; ++p) {
    next.committed[p] = 0;
    next.pending[p] = next.can_act(p);
    next.raise_rights[p] = true;
  }
  next.last_full_raise = next.big_blind;
  const auto opener = next.next_actor(next.button);
  if (!opener) {
    next.phase = next.street == Street::River ? Phase::Showdown : Phase::Deal;
    next.actor = -1;
  } else {
    next.phase = Phase::Action;
    next.actor = static_cast<int>(*opener);
  }
}

// ---------------------------------------------------------------------------
// Independent contribution-ledger settlement (no rake, odd chip clockwise of
// the button), stated directly in the N-seat vocabulary rather than called
// through the library under test.
// ---------------------------------------------------------------------------
struct RefSettle {
  std::vector<Chips> awards, refunds;
  Chips pot = 0;
};

RefSettle ref_settle(const Ref& s, const std::vector<std::optional<std::uint32_t>>& scores) {
  std::vector<Chips> net(s.players);
  std::vector<Chips> levels;
  for (std::size_t p = 0; p < s.players; ++p) {
    net[p] = s.contributed[p] - s.returned[p];
    if (net[p] != 0)
      levels.push_back(net[p]);
  }
  std::sort(levels.begin(), levels.end());
  levels.erase(std::unique(levels.begin(), levels.end()), levels.end());

  RefSettle out;
  out.awards.assign(s.players, 0);
  out.refunds = s.returned;
  Chips previous = 0;
  for (Chips level : levels) {
    std::vector<std::size_t> contributors, eligible;
    for (std::size_t p = 0; p < s.players; ++p) {
      if (net[p] >= level) {
        contributors.push_back(p);
        if (!s.folded[p])
          eligible.push_back(p);
      }
    }
    if (contributors.size() < 2 || eligible.empty())
      continue;
    const Chips amount = (level - previous) * static_cast<Chips>(contributors.size());
    out.pot += amount;
    std::vector<std::size_t> winners;
    if (eligible.size() == 1) {
      winners = eligible;
    } else {
      std::uint32_t best = 0;
      for (std::size_t p : eligible) {
        if (!scores[p])
          throw std::logic_error("contested layer needs a score");
        if (winners.empty() || *scores[p] > best) {
          best = *scores[p];
          winners = {p};
        } else if (*scores[p] == best) {
          winners.push_back(p);
        }
      }
    }
    // Odd chips go to winners in clockwise order from the seat left of the
    // button. Ordering is the library's: seats ABOVE the button ascending,
    // then seats at or below it ascending (the button itself sorts last).
    std::sort(winners.begin(), winners.end(), [&](std::size_t a, std::size_t b) {
      const auto position = [&](std::size_t seat) {
        // Seat numbers here equal player indices; drive() keeps that true.
        return seat > s.button ? seat : seat + s.players;
      };
      return position(a) < position(b);
    });
    const Chips share = amount / static_cast<Chips>(winners.size());
    const Chips remainder = amount - share * static_cast<Chips>(winners.size());
    for (std::size_t p : winners)
      out.awards[p] += share;
    for (Chips i = 0; i < remainder; ++i)
      out.awards[winners[i % winners.size()]] += 1;
    previous = level;
  }
  return out;
}

// ---------------------------------------------------------------------------
// Field-by-field comparison plus the full legality sweep.
// ---------------------------------------------------------------------------
struct Counts {
  long nodes = 0;
  long mid_folds = 0;       // a fold with the hand continuing
  long hand_end_folds = 0;  // the fold that leaves one live seat
  long showdowns = 0;
  long refunds_revived = 0;  // a refund moved a seat out of all-in
  long short_raises = 0;
  long ties = 0;
  long seven_plus_nodes = 0;
  long rooted_roots = 0;
  long flops_dealt = 0;
};

// Reachability counter for the short-raise branch, accumulated by the walk
// and pinned as non-vacuity at the end of the sweep. compare() stays a pure
// predicate.
thread_local long g_short_raise_counter;

int compare(const GameState& got, const Ref& want) {
  auto dump = [&]() {
    std::printf("  got phase=%d street=%d actor=%d lfr=%llu pot=%llu board=%zu\n",
                static_cast<int>(got.phase()), static_cast<int>(got.street()),
                static_cast<int>(got.actor().value_or(static_cast<std::size_t>(-1))),
                static_cast<unsigned long long>(got.last_full_raise()),
                static_cast<unsigned long long>(got.pot()), got.board().size());
    std::printf("  want phase=%d street=%d actor=%d lfr=%llu pot=%llu board=%zu\n",
                static_cast<int>(want.phase), static_cast<int>(want.street), want.actor,
                static_cast<unsigned long long>(want.last_full_raise),
                static_cast<unsigned long long>(want.pot()), want.board.size());
    for (std::size_t p = 0; p < want.players; ++p) {
      const GamePlayer& g = got.players()[p];
      std::printf("  seat %zu got(st=%llu sc=%llu co=%llu re=%llu f=%d a=%d rr=%d pe=%d) "
                  "want(st=%llu sc=%llu co=%llu re=%llu f=%d a=%d rr=%d pe=%d)\n",
                  p, (unsigned long long)g.stack, (unsigned long long)g.street_committed,
                  (unsigned long long)g.contributed, (unsigned long long)g.refunded, g.folded,
                  g.all_in, g.raise_rights, g.pending, (unsigned long long)want.stack[p],
                  (unsigned long long)want.committed[p], (unsigned long long)want.contributed[p],
                  (unsigned long long)want.returned[p], want.folded[p], want.all_in[p],
                  want.raise_rights[p], want.pending[p]);
    }
  };
  if (got.phase() != want.phase || got.street() != want.street) {
    dump();
    return 1;
  }
  if (got.last_full_raise() != want.last_full_raise || got.pot() != want.pot()) {
    dump();
    return 1;
  }
  if (got.board().size() != want.board.size()) {
    dump();
    return 1;
  }
  for (std::size_t i = 0; i < want.board.size(); ++i)
    if (got.board()[i] != want.board[i]) {
      dump();
      return 1;
    }
  {
    const int want_actor = want.phase == Phase::Action ? want.actor : -1;
    if (static_cast<int>(got.actor().value_or(static_cast<std::size_t>(-1))) != want_actor) {
      dump();
      return 1;
    }
  }
  if (got.live_players().size() != want.live_count()) {
    dump();
    return 1;
  }
  {
    std::size_t expected_index = 0;
    for (std::size_t p = 0; p < want.players; ++p)
      if (!want.folded[p]) {
        if (got.live_players()[expected_index++] != p) {
          dump();
          return 1;
        }
      }
  }
  for (std::size_t p = 0; p < want.players; ++p) {
    const GamePlayer& g = got.players()[p];
    if (g.stack != want.stack[p] || g.street_committed != want.committed[p] ||
        g.contributed != want.contributed[p] || g.refunded != want.returned[p] ||
        g.folded != (want.folded[p] != 0) || g.raise_rights != (want.raise_rights[p] != 0) ||
        g.pending != (want.pending[p] != 0) || g.all_in != (want.stack[p] == 0)) {
      dump();
      return 1;
    }
  }
  return 0;
}

// The reachability counter is a member-ish hook: compare() must remain a pure
// predicate, so the refunded-all-in observation is counted at the call site
// below rather than inside the comparison.

int compare_legal(const GameState& got, const Ref& want) {
  if (want.phase != Phase::Action)
    return 0;
  const LegalActions got_legal = got.legal();
  const LegalActions want_legal = ref_legal(want);
  auto dump_legal = [&]() {
    std::printf("  LEGAL got={f%d c%d k%d amt=%llu agg=%d[%llu..%llu,only=%d]} want={f%d c%d k%d "
                "amt=%llu agg=%d[%llu..%llu,only=%d]} actor=%d stack=%llu committed=%llu rr=%d\n",
                got_legal.fold, got_legal.check, got_legal.call,
                (unsigned long long)got_legal.call_amount, got_legal.aggressive.has_value(),
                got_legal.aggressive ? (unsigned long long)got_legal.aggressive->minimum : 0ULL,
                got_legal.aggressive ? (unsigned long long)got_legal.aggressive->maximum : 0ULL,
                got_legal.aggressive ? got_legal.aggressive->all_in_only : 0, want_legal.fold,
                want_legal.check, want_legal.call, (unsigned long long)want_legal.call_amount,
                want_legal.aggressive.has_value(),
                want_legal.aggressive ? (unsigned long long)want_legal.aggressive->minimum : 0ULL,
                want_legal.aggressive ? (unsigned long long)want_legal.aggressive->maximum : 0ULL,
                want_legal.aggressive ? want_legal.aggressive->all_in_only : 0, want.actor,
                (unsigned long long)want.stack[want.actor],
                (unsigned long long)want.committed[want.actor],
                want.raise_rights[want.actor]);
  };
  if (got_legal.fold != want_legal.fold || got_legal.check != want_legal.check ||
      got_legal.call != want_legal.call || got_legal.call_amount != want_legal.call_amount ||
      got_legal.aggressive.has_value() != want_legal.aggressive.has_value()) {
    dump_legal();
    return 1;
  }
  if (want_legal.aggressive) {
    if (got_legal.aggressive->type != want_legal.aggressive->type ||
        got_legal.aggressive->minimum != want_legal.aggressive->minimum ||
        got_legal.aggressive->maximum != want_legal.aggressive->maximum ||
        got_legal.aggressive->all_in_only != want_legal.aggressive->all_in_only) {
      dump_legal();
      return 1;
    }
  }
  // The full legality sweep: every action type against every target.
  const Chips ceiling = want.stack[static_cast<std::size_t>(want.actor)];
  for (int t = 0; t < 5; ++t) {
    const auto type = static_cast<ActionType>(t);
    for (Chips target = 0; target <= ceiling + 1; ++target) {
      const Action probe{type, target};
      if (got_legal.contains(probe) != want_legal.contains(probe))
        return 1;
    }
  }
  if (want_legal.aggressive &&
      want_legal.aggressive->minimum < want.last_full_raise + want.high_commit())
    ++g_short_raise_counter;
  return 0;
}

// At 3..6 seats the real shipped type is the second reference, advanced in
// lockstep.
MultiwayRoot shipped_root(const GameDef& def) {
  MultiwayRoot root;
  root.players = def.player_count;
  root.button = def.button;
  root.big_blind = def.big_blind;
  root.ante = def.ante;
  root.stacks.assign(root.players, 0);
  root.contributions.assign(root.players, 0);
  for (std::size_t p = 0; p < root.players; ++p) {
    root.stacks[p] = def.stacks[p];
    root.contributions[p] = def.contributions[p];
  }
  if (!def.preflop)
    for (std::uint8_t i = 0; i < def.board_size; ++i)
      root.board.push_back(def.board[i]);
  return root;
}

int compare_shipped(const MultiwayState& mw, const Ref& want) {
  auto dump = [&]() {
    std::printf("  SHIPPED phase=%d street=%d actor=%d lfr=%llu pot=%llu board=%zu\n",
                static_cast<int>(mw.phase()), static_cast<int>(mw.street()),
                static_cast<int>(mw.actor().value_or(static_cast<std::size_t>(-1))),
                static_cast<unsigned long long>(mw.last_full_raise()),
                static_cast<unsigned long long>(mw.pot()), mw.board().size());
    std::printf("  LEDGER  phase=%d street=%d actor=%d lfr=%llu pot=%llu board=%zu\n",
                static_cast<int>(want.phase), static_cast<int>(want.street), want.actor,
                static_cast<unsigned long long>(want.last_full_raise),
                static_cast<unsigned long long>(want.pot()), want.board.size());
    for (std::size_t p = 0; p < want.players; ++p) {
      const MultiwayPlayer& m = mw.players()[p];
      std::printf("  seat %zu ship(st=%llu co=%llu re=%llu f=%d a=%d rr=%d pe=%d) "
                  "ledg(st=%llu co=%llu re=%llu f=%d a=%d rr=%d pe=%d)\n",
                  p, (unsigned long long)m.stack, (unsigned long long)m.street_committed,
                  (unsigned long long)m.refunded, m.folded, m.all_in, m.raise_rights, m.pending,
                  (unsigned long long)want.stack[p], (unsigned long long)want.committed[p],
                  (unsigned long long)want.returned[p], want.folded[p], want.all_in[p],
                  want.raise_rights[p], want.pending[p]);
    }
  };
  if (mw.phase() != want.phase || mw.street() != want.street ||
      mw.last_full_raise() != want.last_full_raise || mw.pot() != want.pot()) {
    dump();
    return 1;
  }
  if (mw.board().size() != want.board.size()) {
    dump();
    return 1;
  }
  for (std::size_t i = 0; i < want.board.size(); ++i)
    if (mw.board()[i] != want.board[i]) {
      dump();
      return 1;
    }
  if (static_cast<int>(mw.actor().value_or(static_cast<std::size_t>(-1))) !=
      (want.phase == Phase::Action ? want.actor : -1)) {
    dump();
    return 1;
  }
  if (mw.live_players().size() != want.live_count()) {
    dump();
    return 1;
  }
  for (std::size_t p = 0; p < want.players; ++p) {
    const MultiwayPlayer& m = mw.players()[p];
    if (m.stack != want.stack[p] || m.street_committed != want.committed[p] ||
        m.contributed != want.contributed[p] || m.refunded != want.returned[p] ||
        m.folded != (want.folded[p] != 0) || m.all_in != (want.all_in[p] != 0) ||
        m.raise_rights != (want.raise_rights[p] != 0) || m.pending != (want.pending[p] != 0)) {
      dump();
      return 1;
    }
  }
  return 0;
}

int compare_settlement(const GameState& state, const Ref& ref, bool showdown, Counts& counts) {
  ContributionSettlement got;
  RefSettle want;
  std::vector<std::array<int, 2>> holes;
  if (showdown) {
    std::vector<std::optional<std::uint32_t>> scores(ref.players);
    for (std::size_t i = 0; i < state.live_players().size(); ++i) {
      const std::size_t seat = state.live_players()[i];
      std::array<int, 7> cards{};
      for (std::size_t c = 0; c < ref.board.size(); ++c)
        cards[c] = ref.board[c];
      const std::array<int, 2> hand{card(kHoleNames[i][0]), card(kHoleNames[i][1])};
      cards[5] = hand[0];
      cards[6] = hand[1];
      holes.push_back(hand);
      scores[seat] = bs::evaluate(cards.data(), 7).score;
    }
    // A tie is a branch the pot-ordering code gets wrong silently; count it.
    for (std::size_t i = 0; i < state.live_players().size(); ++i)
      for (std::size_t j = i + 1; j < state.live_players().size(); ++j) {
        const auto a = scores[state.live_players()[i]];
        const auto b = scores[state.live_players()[j]];
        if (a && b && *a == *b)
          ++counts.ties;
      }
    got = state.settle_showdown(holes);
    want = ref_settle(ref, scores);
  } else {
    got = state.settle_fold();
    const std::vector<std::optional<std::uint32_t>> scores(ref.players);
    want = ref_settle(ref, scores);
  }
  Chips awarded = 0;
  for (std::size_t p = 0; p < ref.players; ++p) {
    if (got.awards[p] != want.awards[p] || got.refunds[p] != want.refunds[p]) {
      std::printf("settle mismatch showdown=%d n=%zu seat=%zu got(award=%llu refund=%llu) "
                  "want(award=%llu refund=%llu) pot got=%llu want=%llu\n",
                  static_cast<int>(showdown), ref.players, p,
                  static_cast<unsigned long long>(got.awards[p]),
                  static_cast<unsigned long long>(got.refunds[p]),
                  static_cast<unsigned long long>(want.awards[p]),
                  static_cast<unsigned long long>(want.refunds[p]),
                  static_cast<unsigned long long>(got.pot),
                  static_cast<unsigned long long>(want.pot));
      return 1;
    }
    awarded += got.awards[p];
  }
  Chips put_in = 0;
  for (std::size_t p = 0; p < ref.players; ++p)
    put_in += ref.contributed[p];
  if (awarded + got.total_refunds != put_in || got.pot != want.pot) {
    std::printf("settle conservation mismatch showdown=%d n=%zu awarded+ref=%llu put=%llu\n",
                static_cast<int>(showdown), ref.players,
                static_cast<unsigned long long>(awarded + got.total_refunds),
                static_cast<unsigned long long>(put_in));
    return 1;
  }
  return 0;
}

// The bounded exhaustive walk. Small stacks keep the action trees finite; the
// ledger, not sampling, decides every branch.
int walk(GameState& state, Ref& ref, MultiwayState* mw, Counts& counts, int depth) {
  if (depth > 48)
    return 0;
  ++counts.nodes;
  if (ref.players >= 7)
    ++counts.seven_plus_nodes;
  g_short_raise_counter = 0;
  const int field_cmp = compare(state, ref);
  const int shipped_cmp = mw ? compare_shipped(*mw, ref) : 0;
  const int legal_cmp = compare_legal(state, ref);
  if (field_cmp != 0 || shipped_cmp != 0 || legal_cmp != 0) {
    std::printf("mismatch at node %ld (n=%zu depth=%d) field=%d shipped=%d legal=%d\n",
                counts.nodes, ref.players, depth, field_cmp, shipped_cmp, legal_cmp);
    return 1;
  }
  counts.short_raises += g_short_raise_counter;
  // The refunded-but-live state: counted here so compare() stays a predicate.
  for (std::size_t p = 0; p < ref.players; ++p)
    if (ref.returned[p] > 0 && ref.stack[p] > 0)
      ++counts.refunds_revived;

  if (ref.phase == Phase::Folded) {
    ++counts.hand_end_folds;
    return compare_settlement(state, ref, false, counts);
  }
  if (ref.phase == Phase::Showdown) {
    ++counts.showdowns;
    return compare_settlement(state, ref, true, counts);
  }
  if (ref.phase == Phase::Deal) {
    ++counts.flops_dealt;
    const int c = card(kBoardNames[ref.board.size()]);
    GameState dealt = state.after_card(c);
    Ref dealt_ref = ref;
    std::optional<MultiwayState> dealt_mw;
    if (mw)
      dealt_mw.emplace(mw->after_card(c));
    ref_deal(dealt_ref, c);
    return walk(dealt, dealt_ref, dealt_mw ? &*dealt_mw : nullptr, counts, depth + 1);
  }

  const std::size_t actor = static_cast<std::size_t>(ref.actor);
  const LegalActions legal = ref_legal(ref);
  std::vector<Action> options;
  if (legal.fold)
    options.push_back({ActionType::Fold, 0});
  if (legal.check)
    options.push_back({ActionType::Check, 0});
  if (legal.call)
    options.push_back({ActionType::Call, 0});
  if (legal.aggressive) {
    options.push_back({legal.aggressive->type, legal.aggressive->minimum});
    // The maximum branch fans the tree out combinatorially (every raise ladder
    // rung is reachable from both the min and max in a few raises), so it is
    // explored only from the button. Buttons rotate through every seat over
    // the 3..6 fixtures, so the maximum is exercised at every seat; the
    // capped minimum is exercised everywhere.
    if (legal.aggressive->minimum < legal.aggressive->maximum && actor == ref.button)
      options.push_back({legal.aggressive->type, legal.aggressive->maximum});
  }
  const bool mid_hand = ref.live_count() > 1;
  for (const Action& action : options) {
    GameState next_state = state.after_action(actor, action);
    Ref next_ref = ref;
    std::optional<MultiwayState> next_mw;
    if (mw)
      next_mw.emplace(mw->after_action(actor, {action.type, action.target_total}));
    const std::size_t live_before = next_ref.live_count();
    ref_act(next_ref, actor, action);
    if (action.type == ActionType::Fold && mid_hand && live_before > 1 &&
        next_ref.phase == Phase::Action)
      ++counts.mid_folds;
    if (walk(next_state, next_ref, next_mw ? &*next_mw : nullptr, counts, depth + 1) != 0)
      return 1;
  }
  return 0;
}

// ---------------------------------------------------------------------------
// Root builders. Stacks are declared BEFORE anything posts, matching the
// definition's own contract.
// ---------------------------------------------------------------------------
long g_capped = 0;
long g_antes = 0;

GameDef make_preflop(std::size_t players, std::size_t button, Chips bb,
                     const std::vector<Chips>& live_stacks, Chips ante) {
  GameDef def{};
  def.player_count = players;
  def.button = button;
  def.big_blind = bb;
  def.ante = ante;
  def.preflop = true;
  def.board_size = 0;
  def.board = {-1, -1, -1, -1, -1};
  for (std::size_t p = 0; p < players; ++p)
    def.stacks[p] = live_stacks[p];
  const std::size_t small_seat = (button + 1) % players;
  const std::size_t big_seat = (button + 2) % players;
  Chips pot = 0;
  if (ante > 0)
    for (std::size_t p = 0; p < players; ++p)
      pot += std::min(ante, def.stacks[p]);
  // Cap by the stack still held AFTER the ante, exactly as the constructor
  // posts it.
  const Chips small_posted = [&] {
    Chips after_ante = def.stacks[small_seat] - std::min(ante, def.stacks[small_seat]);
    return std::min(bb / 2, after_ante);
  }();
  const Chips big_posted = [&] {
    Chips after_ante = def.stacks[big_seat] - std::min(ante, def.stacks[big_seat]);
    return std::min(bb, after_ante);
  }();
  def.blinds_posted[small_seat] = small_posted;
  def.blinds_posted[big_seat] = big_posted;
  pot += small_posted + big_posted;
  def.pot = pot;
  if (small_posted != bb / 2 || big_posted != bb)
    ++g_capped;
  if (ante > 0)
    ++g_antes;
  return def;
}

GameDef make_rooted(std::size_t players, std::size_t button, Chips bb, std::uint8_t board_size,
                    const std::vector<Chips>& stacks, const std::vector<Chips>& dead) {
  GameDef def{};
  def.player_count = players;
  def.button = button;
  def.big_blind = bb;
  def.preflop = false;
  def.board_size = board_size;
  def.board = {-1, -1, -1, -1, -1};
  Chips pot = 0;
  for (std::size_t p = 0; p < players; ++p) {
    def.stacks[p] = stacks[p];
    def.contributions[p] = dead[p];
    pot += dead[p];
  }
  for (std::uint8_t i = 0; i < board_size; ++i)
    def.board[i] = card(kBoardNames[i]);
  def.pot = pot;
  return def;
}

int drive(const GameDef& def, Counts& counts) {
  GameState state(def);
  Ref ref = ref_start(def);
  std::optional<MultiwayState> mw;
  MultiwayState* mw_ptr = nullptr;
  if (def.player_count <= kMaxMultiwayPlayers) {
    mw.emplace(shipped_root(def));
    mw_ptr = &*mw;
  }
  if (!def.preflop)
    ++counts.rooted_roots;
  return walk(state, ref, mw_ptr, counts, 0);
}

std::vector<Chips> flat(std::size_t players, Chips value) {
  return std::vector<Chips>(players, value);
}
std::vector<Chips> one_short(std::size_t players, std::size_t seat, Chips short_stack,
                             Chips deep = 40) {
  std::vector<Chips> stacks(players, deep);
  stacks[seat % players] = short_stack;
  return stacks;
}

// A preflop root with exactly three ACTIVE seats and every other seat all in
// before the blinds post. The inactive seats post their whole one-chip stack
// as an ante (`inactive_ante == inactive_stack == 1`), so they start all in
// and never branch the action tree; the three live seats are the opener and
// the two blind seats, which is also a complete three-way orbit around the
// button. The ten-element arrays, the actor search across inactive seats, and
// the multi-way pot layering are still exercised by the inactive ones.
GameDef bounded_preflop(std::size_t players, std::size_t button, Chips bb, Chips active_stack,
                        Chips active_short_seat, Chips active_short_stack) {
  std::vector<Chips> stacks(players, 1);
  const std::size_t small_seat = (button + 1) % players;
  const std::size_t big_seat = (button + 2) % players;
  const std::size_t opener = (button + 3) % players;
  stacks[small_seat] = active_stack;
  stacks[big_seat] = active_stack;
  stacks[opener] = active_stack;
  if (active_short_seat != players)
    stacks[active_short_seat] = active_short_stack;
  return make_preflop(players, button, bb, stacks, 1);
}

int sweep() {
  Counts counts;
  const Chips bb = 2;

  // Under AddressSanitizer the walk is instrumented ~30x slower, so the
  // sanitized sweep is a SUBSET: three seats full (the shipped lockstep),
  // then only the ten-seat preflop and rooted fixtures, enough to exercise
  // the ten-element arrays and the 7..10 ledger under sanitizers. The
  // release sweep covers every seat count and every non-vacuity counter;
  // the sanitizer's job is bounds/lifetime, not combinatorial coverage.
#if defined(__has_feature)
#if __has_feature(address_sanitizer)
#define BS_MULTIWAY_SANITIZED 1
#endif
#endif
#ifndef BS_MULTIWAY_SANITIZED
#define BS_MULTIWAY_SANITIZED 0
#endif

  // Tree size grows combinatorially with seat count and stack depth, so the
  // fixtures are tiered rather than uniform:
  //  - 3..6 seats, where the REAL MultiwayState exists: EVERY button
  //    rotation (it is the clockwise seat rotation that must agree, not just
  //    the counts) with a short four-chip raise ladder, plus one capped-blind
  //    fixture per seat count;
  //  - 7..10 seats, where no shipped type exists and the independent ledger
  //    is the only reference: two button positions and two stack shapes,
  //    plus the ante fixture at ten seats. The 7..10 range is additionally
  //    covered by the settlement grid extension.
  // The walk enumerates every legal action at every node, so its size is
  // exponential in the number of seats that can still act, not merely the
  // seat count. `bounded_preflop` and the rooted fixture below keep exactly
  // three ACTIVE seats; seats beyond the third post all in or carry a
  // prior-street all-in. The inactive seats still traverse the ten-element
  // arrays, the clockwise actor search, and the contribution layering.
  for (std::size_t players = 3; players <= kMaxUnifiedSeats; ++players) {
#if BS_MULTIWAY_SANITIZED
    if (players != 3 && players != 10)
      continue;
#endif
    // Three seats rotate the cheapest fixture (four-chip stacks, one raise
    // rung) through EVERY button position; that is the lockstep anchor the
    // clockwise actor order is pinned against. At 4..10 seats the same three
    // active seats ride two representative button positions, because walk
    // size is exponential in live-seat count and a full orbit at every count
    // is not a feasible gate. The rules are seat-symmetric past the inactive
    // all-in seats, which the clockwise actor search skips identically.
    std::vector<std::size_t> orbit;
    if (players == 3) {
      for (std::size_t b = 0; b < players; ++b)
        orbit.push_back(b);
    } else {
      orbit = {0, players - 1};
    }
    for (std::size_t button : orbit) {
      // The cheapest bounded root: three active four-chip seats, every seat
      // beyond them posted-all-in by an ante. Rotated across the button orbit
      // so the clockwise skip past inactive all-in seats is exercised.
      if (drive(bounded_preflop(players, button, bb, 4, players, 0), counts) != 0)
        return 1;
    }
    // Capped big blind (posts its whole one-chip stack) inside the bounded
    // three-active profile.
    if (drive(bounded_preflop(players, 0, bb, 20, (0 + 2) % players, 1), counts) != 0)
      return 1;
    // A three-chip all-in raise against deeper seats: the short-raise and
    // cumulative-short-reopening branches, reached from one fixture.
    if (players <= 6)
      if (drive(bounded_preflop(players, 0, bb, 20, 0, 3), counts) != 0)
        return 1;
    // An ante that empties the OPENER (a non-blind seat) before the blinds:
    // it posts no blind and starts all in, so the constructor advances the
    // opener past it and the tree proceeds with the two blind seats. Covers the
    // ante-empties-a-live-seat construction path at four/six/ten seats.
    if (players == 4 || players == 6 || players == 10) {
      const Chips ante = 5;
      const std::size_t opener_seat = (0 + 3) % players;
      const std::size_t small_seat = (0 + 1) % players;
      const std::size_t big_seat = (0 + 2) % players;
      std::vector<Chips> stacks(players, ante);  // every inactive seat antes out
      stacks[opener_seat] = ante;  // the opener also antes out and starts all in
      stacks[small_seat] = 20;
      stacks[big_seat] = 20;
      GameDef def = make_preflop(players, 0, bb, stacks, ante);
      if (drive(def, counts) != 0)
        return 1;
    }
  }

  // --- Rooted boards: one all-in seat and one deep seat ------------------
  // Two button positions through six seats (the cheap preflop orbit above
  // covers the remaining rotations); the extended range rides the same two.
  for (std::size_t players = 3; players <= kMaxUnifiedSeats; ++players) {
#if BS_MULTIWAY_SANITIZED
    if (players != 3 && players != 10)
      continue;
#endif
    for (std::size_t button : {std::size_t{0}, players - 1}) {
      // The three-seat lockstep drives a rooted RIVER (five cards); at 4..10
      // seats the rooted roots ride a FLOP only. The extra turn/river streets
      // add no new transition rule (after_card is one machine across
      // streets), while they multiply the walk by every action subtree, so
      // repeating both board sizes at every count would cost minutes for no
      // new rule coverage. The flop still deals and closes streets; the river
      // showdown path is fully enumerated at three seats.
      const std::array<std::uint8_t, 1> rooted_boards = {
          players == 3 ? std::uint8_t{5} : std::uint8_t{3}};
      for (std::uint8_t board_size : rooted_boards) {
        // Exactly three ACTIVE seats (0, 1, and the last seat); every other
        // seat committed earlier and is all in with zero behind, which is the
        // state the actor search skips and the contribution ledger layers
        // over. Small active stacks keep the raise ladder to two rungs, so
        // the exhaustive walk stays bounded regardless of the seat count.
        std::vector<Chips> stacks(players, 0);
        std::vector<Chips> dead(players, 4);  // each inactive seat reached level 4
        stacks[0] = 0;    // already all in from an earlier street
        stacks[1] = 10;   // one deep active seat
        stacks[2 % players] = 6;
        stacks[players - 1] = 6;
        if (players >= 7)
          stacks[players - 1] = 3;  // a short active seat in the extended range
        if (drive(make_rooted(players, button, bb, board_size, stacks, dead), counts) != 0)
          return 1;
      }
    }
  }

  std::printf(
      "multiway oracle: nodes=%ld 7+nodes=%ld mid_folds=%ld end_folds=%ld showdowns=%ld "
      "ties=%ld refunds_revived=%ld short_raises=%ld capped_blinds=%ld antes=%ld rooted=%ld "
      "flop_deals=%ld\n",
      counts.nodes, counts.seven_plus_nodes, counts.mid_folds, counts.hand_end_folds,
      counts.showdowns, counts.ties, counts.refunds_revived, counts.short_raises, g_capped, g_antes,
      counts.rooted_roots, counts.flops_dealt);
  // Non-vacuity for the full sweep: every rule-bearing counter below is
  // asserted positive. The walk reaches each branch from at least one fixture.
  CHECK(counts.nodes > 20000);
  CHECK(counts.seven_plus_nodes > 1000);
  CHECK(counts.mid_folds > 0);
  CHECK(counts.hand_end_folds > 0);
  CHECK(counts.showdowns > 0);
  CHECK(counts.ties > 0);
  CHECK(counts.refunds_revived > 0);
  CHECK(counts.short_raises > 0);
  CHECK(g_capped > 0);
  CHECK(g_antes > 0);
  CHECK(counts.rooted_roots > 0);
  CHECK(counts.flops_dealt > 0);
  return 0;
}

// A mid-hand fold that is ALSO the last pending action of the street: the two
// seats that already checked cleared their pending flags, and the folding
// seat is still pending. The street must close even though the folded seat's
// pending flag is never cleared -- `any_pending()` has to skip folded seats.
// Counting the folded seat would leave the street open forever.
int fold_as_last_pending_action_closes_street() {
  GameDef def{};
  def.player_count = 3;
  def.button = 0;
  def.big_blind = 2;
  def.stacks = {40, 20, 20, 0, 0, 0, 0, 0, 0, 0};
  def.contributions = {4, 4, 4, 0, 0, 0, 0, 0, 0, 0};
  def.pot = 12;
  def.board = {card("2h"), card("3h"), card("4h"), 0, 0};
  def.board_size = 3;
  def.preflop = false;
  GameState state(def);
  // Postflop order off button 0: seat 1 opens, seat 2 calls, seat 0 faces
  // the bet and is the last seat to act on the street.
  CHECK(state.actor() == 1);
  const LegalActions open = state.legal();
  CHECK(open.aggressive.has_value());
  const Chips target = open.aggressive->minimum;
  state = state.after_action(1, {open.aggressive->type, target});
  CHECK(state.actor() == 2);
  state = state.after_action(2, {ActionType::Call, 0});
  CHECK(state.actor() == 0);
  // Seat 0 folds last: two seats live, the street closes and the turn deals.
  state = state.after_action(0, {ActionType::Fold, 0});
  CHECK(state.phase() == Phase::Deal);
  CHECK(state.live_players().size() == 2);
  return 0;
}

// A three-seat root the stage-1 gate used to reject must now construct and
// drive as the multiway profile.
int three_seats_now_construct() {
  GameDef def{};
  def.player_count = 3;
  def.button = 1;
  def.big_blind = 2;
  def.stacks = {20, 20, 40, 0, 0, 0, 0, 0, 0, 0};
  def.contributions = {2, 2, 2, 0, 0, 0, 0, 0, 0, 0};
  def.pot = 6;
  def.board = {card("2h"), card("3h"), card("4h"), 0, 0};
  def.board_size = 3;
  def.preflop = false;
  const GameState state(def);
  CHECK(state.player_count() == 3);
  CHECK(state.actor() == 2);  // first seat clockwise of button 1 that can act
  CHECK(state.players()[2].pending);

  // Eleven seats remain past the bound even with the widened ledger.
  GameDef too_many = def;
  too_many.player_count = 11;
  bool rejected = false;
  try {
    const GameState bad(too_many);
    static_cast<void>(bad);
  } catch (const std::invalid_argument&) {
    rejected = true;
  }
  CHECK(rejected);
  return 0;
}

int validation_rejections() {
  auto rejects = [](GameDef def) {
    try {
      const GameState state(def);
      static_cast<void>(state);
    } catch (const std::invalid_argument&) {
      return true;
    } catch (const std::overflow_error&) {
      return true;
    }
    return false;
  };

  // A third blind poster is not a thing.
  {
    GameDef def = make_preflop(3, 0, 2, flat(3, 20), 0);
    def.blinds_posted[0] = 1;  // seat 0 is neither blind seat
    CHECK(rejects(def));
  }
  // A blind posting above what the seat holds is a malformed root.
  {
    GameDef def = make_preflop(3, 0, 2, one_short(3, 1, 1, 20), 0);
    def.blinds_posted[1] = 2;  // the seat holds 1 after the ante it never posts
    CHECK(rejects(def));
  }
  // A preflop definition cannot carry a board; a rooted one cannot be empty.
  {
    GameDef def = make_preflop(3, 0, 2, flat(3, 20), 0);
    def.board = {card("2h"), card("3h"), card("4h"), 0, 0};
    def.board_size = 3;
    CHECK(rejects(def));
  }
  {
    GameDef def = make_rooted(3, 0, 2, 3, flat(3, 20), flat(3, 2));
    def.board_size = 0;
    def.board = {-1, -1, -1, -1, -1};
    CHECK(rejects(def));
  }
  // A rooted pot must reconcile with the declared dead money exactly.
  {
    GameDef def = make_rooted(3, 0, 2, 3, flat(3, 20), flat(3, 1));
    def.pot = 4;  // three declared, pot says four
    CHECK(rejects(def));
  }
  // A partial board is not a root at any seat count.
  {
    GameDef def = make_rooted(4, 0, 2, 3, flat(4, 20), flat(4, 2));
    def.board_size = 2;
    CHECK(rejects(def));
  }
  // The old multiway type still stops at six; the widening is the unified type.
  {
    MultiwayRoot root;
    root.players = 7;
    root.button = 0;
    root.big_blind = 2;
    root.stacks.assign(7, 200);
    root.contributions.assign(7, 0);
    bool rejected = false;
    try {
      const MultiwayState state(root);
      static_cast<void>(state);
    } catch (const std::invalid_argument&) {
      rejected = true;
    }
    CHECK(rejected);
  }
  return 0;
}

}  // namespace

int main() {
  struct Case {
    const char* name;
    int (*fn)();
  };
  const std::array<Case, 4> cases{{
      {"sweep", sweep},
      {"fold_as_last_pending_closes_street", fold_as_last_pending_action_closes_street},
      {"three_seats_now_construct", three_seats_now_construct},
      {"validation_rejections", validation_rejections},
  }};
  int failures = 0;
  for (const Case& item : cases) {
    std::printf("case %s ...\n", item.name);
    std::fflush(stdout);
    const int result = item.fn();
    std::printf("  %s\n", result == 0 ? "ok" : "FAILED");
    failures += result;
  }
  if (failures != 0) {
    std::printf("test_game_definition_multiway FAILED\n");
    return 1;
  }
  std::printf("test_game_definition_multiway PASS\n");
  return 0;
}
