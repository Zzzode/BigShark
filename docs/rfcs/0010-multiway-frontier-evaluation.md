---
rfc: "0010"
subject: "Multiway Frontier Evaluation and N-Way Preflop Training"
status: "Proposed"
authors: "BigShark maintainers"
created: "2026-10-04"
updated: "2026-10-04"
owners: "engine, solver, artifact boundary, benchmarks"
supersedes: ""
superseded-by: ""
---

# RFC 0010: Multiway Frontier Evaluation and N-Way Preflop Training

## Summary

Generalize the flop-terminal frontier evaluator from heads-up to 2..10 seats,
enabling multiway preflop profile training. The `EquityFrontierEvaluator`
computes exact all-in-at-flop equity for N hands by enumerating turn/river
runouts and splitting the pot among tied winners. The `FrontierEvaluator`
interface already accepts a span of hands (N-way in shape); this RFC updates
the contract, generalizes the implementation, relaxes the player-count gates
in the trainer, GameDef validation, and artifact reader/writer, and extends
the offline preflop profile builder to produce N-way artifacts. The
heads-up-only `DeclaredFrontierTable` and the live v1 decision path are
explicitly out of scope.

## Motivation

The published preflop profile (`artifacts/preflop-profile/preflop-profile.db`,
rules_id `rfc0009-unified-preflop-v1`) is heads-up only. Real poker sessions
are multiway: 6-max and 9-max tables routinely produce 3..6-way preflop pots.
A heads-up preflop blueprint cannot inform multiway decisions, and the live
engine currently falls back to the operational policy (check/call/fold) or
the minor-2 resolver on every multiway preflop hand.

The blocker is the frontier evaluator. A flop-terminal game ends at the flop;
the frontier leaf's value is supplied by a `FrontierEvaluator` rather than by
the rules' showdown. RFC 0007 scoped the evaluator to two players. The
`EquityFrontierEvaluator` throws on `hands.size() != 2`, the trainer refuses
multiway flop-terminal roots, and the artifact reader/writer hardcode
`player_count == 2` for preflop flop-terminal artifacts.

The N-way generalization is conceptually straightforward: enumerate the same
turn/river runouts, evaluate all N hands, and split the pot among tied
winners. The computational cost grows linearly with N (N hand evaluations
per runout), and the runout count shrinks as N grows (fewer unseen cards).
This RFC removes the heads-up restriction so a multiway preflop profile can
be trained, measured, and published.

## Goals

- Generalize `EquityFrontierEvaluator` to support 2..10 seats with exact
  all-in-at-flop equity and correct multiway pot splitting.
- Update the `FrontierEvaluator` contract from "heads-up only" to "2..10
  seats"; the interface signature is unchanged.
- Relax the flop-terminal player-count gates in GameDef validation, the
  nseat trainer, and the artifact reader/writer.
- Export the blind-seat helpers (`small_blind_seat`, `big_blind_seat`,
  `preflop_first_actor`) from the poker module so the artifact reader/writer
  derives N-way blinds from a single source of truth.
- Extend the offline preflop profile builder to train and publish an N-way
  preflop artifact (first target: 3-way).
- Measure and report the N-way preflop profile: tree size, information sets,
  artifact bytes, training wall time, and NashConv (via the existing
  `bigshark-preflop-nash-conv` tool, generalized to N-way).

## Non-Goals

- **Live v1 N-way preflop reconstruction.** The live decision path
  (`reconstructPreflop` in the River adapter) builds a 2-seat flop-terminal
  GameDef. Generalizing it to N-way and wiring an N-way artifact into the
  resident layer is a separate change, deferred to a follow-up RFC or stage.
- **Continuation-range N-way export.** `export_continuation_ranges` is
  heads-up only and feeds a heads-up flop class library. An N-way flop class
  library does not exist yet; generalizing the export is deferred until one
  does.
- **DeclaredFrontierTable N-way.** The `DeclaredFrontierTable` (RFC 0007
  option A, nested blueprint lookup) uses fixed-size `std::array<..., 2>`
  entries and is keyed to a 2-seat blueprint. It remains heads-up only.
  N-way declared tables are future work if nested evaluation is revived.
- **N-way flop class library.** The flop class library builder
  (`flop_library.cpp`) trains heads-up flop-rooted games. Extending it to
  N-way is possible with the generalized evaluator but is not part of this
  RFC.
- **Postflop multiway solving.** The heads-up multistreet CFR
  (`solve_heads_up`) remains 2-seat by design; multiway postflop games route
  to the nseat MCCFR trainer, which already supports 2..10 seats for
  river-terminal games.

## Current State and Evidence

### Frontier evaluator (heads-up only)

`engine/include/bs/frontier.hpp` defines the `FrontierEvaluator` interface.
The `evaluate()` signature already accepts `std::span<const std::array<int,2>>
hands` — N-way in shape — but the contract comment states "two-player
heads-up only" and implementations must reject `player_count != 2`.

`engine/src/gto/equity_frontier.cpp` implements `EquityFrontierEvaluator`:
- Throws `std::invalid_argument` if `hands.size() != 2` (line 21-22).
- Builds a 45-card pool (52 - 3 flop - 4 hole), enumerates C(45,2) = 990
  turn/river combos.
- For each combo, evaluates both 7-card hands, counts wins/ties, computes
  `equity = (wins + 0.5 * ties) / total`.
- Caches equity per `(flop, hands)` in an `unordered_map<uint64_t,
  array<double,2>>` keyed by a 42-bit packing (7 cards x 6 bits).
- Returns `equity * pot - contributed` per seat.

### Trainer gate

`engine/src/gto/nseat_trainer.cpp` `validate_request()` (line 541-554)
accepts a flop-terminal preflop root only when `player_count == 2`:

```cpp
if (def.board_size == 0 && def.preflop && def.terminal == poker::TerminalDepth::Flop) {
  if (def.player_count != 2)
    throw std::invalid_argument(
        "nseat trainer: frontier evaluation is heads-up only; multiway "
        "flop-terminal is outside RFC 0007's scope");
}
```

The trainer's `walk_flop_deal()` already samples 3 flop cards and calls
`ctx.frontier->evaluate(flop, hands, payload)` with `hands` built from
`*ctx.holes` (N entries). The walk is N-way-ready; only the gate blocks it.

### GameDef validation

`engine/src/poker/game_definition.cpp` (line 72-73) requires
`def.player_count == 2` for all flop-terminal games:

```cpp
require(def.player_count == 2,
        "flop-terminal games are heads-up only; multiway frontier evaluation is out of scope");
```

The root validation already accepts both preflop-rooted (`preflop &&
board_size == 0`) and flop-rooted (`!preflop && board_size == 3`) games.

### Artifact reader/writer

`engine/src/artifacts/strategy_artifact.cpp`:

- **Writer** `validate_game_v2()` (line 1119-1133): for preflop
  flop-terminal games, checks `player_count == 2` and validates
  `blinds_posted` against the hardcoded 2-seat convention (button posts
  `big_blind/2`, other posts `big_blind`).
- **Reader** (line 2048-2049): checks `player_count_raw == 2` for preflop
  artifacts.
- **Reader** (line 2070-2074): derives `blinds_posted` with the hardcoded
  2-seat convention:
  ```cpp
  game.blinds_posted[game.button] = game.big_blind / 2;
  game.blinds_posted[1 - game.button] = game.big_blind;
  ```

The DB-level rules identifier is `kRulesIdentifierPreflop` for all preflop
games regardless of seat count; the reader determines `is_preflop` from it.
No schema change is needed to store N-way preflop games — the `game` table
already stores `player_count` as a column.

### Blind-seat helpers (file-local)

`engine/src/poker/game_definition.cpp` defines `small_blind_seat()`,
`big_blind_seat()`, and `preflop_first_actor()` in an anonymous namespace
(line 45-54). They already handle both conventions:

```cpp
std::size_t small_blind_seat(const GameDef& def) {
  return def.player_count == 2 ? def.button : clockwise(def.player_count, def.button);
}
std::size_t big_blind_seat(const GameDef& def) {
  return def.player_count == 2 ? clockwise(def.player_count, def.button)
                               : clockwise(def.player_count, def.button, 2);
}
std::size_t preflop_first_actor(const GameDef& def) {
  return def.player_count == 2 ? def.button : clockwise(def.player_count, big_blind_seat(def));
}
```

These are not declared in the public header, so the artifact reader/writer
cannot use them and instead hardcodes the 2-seat convention.

### Preflop profile builder (heads-up only)

`engine/benchmarks/preflop_profile_builder.cpp` builds a 2-seat GameDef
(`player_count = 2`, 2 ranges of 1,326 combos each), trains with
`EquityFrontierEvaluator`, and publishes a schema-v2 artifact. The
`make_preflop_def()` and `all_combos()` helpers hardcode 2 seats.

### HU baseline measurements

The published heads-up preflop profile (100 BB, 100K iterations), per
`artifacts/preflop-profile/manifest.json`:
- 5,326 tree nodes (1,776 action), 126,317 information sets.
- 114,656,256 accounted bytes (~109 MiB) trainer memory.
- 1,025,584 stored rows, 265,732,096 bytes (~253 MiB) artifact.
- Training wall time: 111.598 s (virtual flop deal, Preflop169 abstraction).
- NashConv: 99.24 chips (49.62 BB exploitability) at 10K MC deals.

The FlopDeal leaf (virtual flop deal, committed a333bcc) eliminated the
132,600-leaf chance subtree per preflop action line, keeping the tree small
regardless of stack depth. The same mechanism keeps an N-way preflop tree
small: tree size is dominated by action nodes, not chance.

## Design Principles

- **Exact, not approximate.** The frontier value is exact all-in-at-flop
  equity: every turn/river runout is enumerated, every hand evaluated, ties
  split correctly. No Monte Carlo, no equity lookup tables, no
  interpolation.
- **One source of truth for blinds.** The blind-seat derivation lives in
  the poker module and is used by GameDef validation, the GameState
  constructor, and the artifact reader/writer. No duplicated convention
  logic.
- **Fail closed.** Every gate that currently rejects N-way flop-terminal
  games either accepts 2..10 or throws with a typed message. No silent
  acceptance of an unmodeled shape.
- **No schema change.** The schema-v2 artifact format already stores
  `player_count` and derives blinds deterministically. N-way preflop
  artifacts use the same schema with `player_count > 2`; the reader
  generalizes the blind derivation. No migration, no version bump.
- **Cache is an optimization, not a correctness dependency.** The equity
  cache may be absent, capped, or cold; the evaluator recomputes from
  first principles on a miss. A cache collision must never produce a wrong
  equity (the key comparison is exact, not hash-only).

## Proposed Design

### 1. FrontierEvaluator contract update

`engine/include/bs/frontier.hpp`: update the contract comment from
"two-player heads-up only" to "2..10 seats". The `evaluate()` signature is
unchanged (it already takes a span). Add a contract clause:

```
- Scope: 2..10 seats (RFC 0010). Implementations must reject
  player_count < 2 or player_count > kMaxUnifiedSeats.
```

The `DeclaredFrontierTable` class comment is updated to state it remains
heads-up only (its `Entry` struct is fixed-size and keyed to a 2-seat
blueprint).

### 2. EquityFrontierEvaluator generalization

`engine/include/bs/equity_frontier.hpp` and
`engine/src/gto/equity_frontier.cpp`:

**Validation:** replace the `hands.size() != 2` throw with:

```cpp
if (hands.size() < 2 || hands.size() > poker::kMaxUnifiedSeats)
  throw std::invalid_argument("equity frontier evaluator supports 2..10 seats");
if (flop.size() != 3)
  throw std::invalid_argument("equity frontier evaluator requires a 3-card flop");
```

**Equity computation:** generalize the win/tie counting to N-way, with
explicit folded-seat handling. In a multiway flop-terminal game the frontier
is routinely reached after preflop folds: the TreeBuilder emits a `FlopDeal`
leaf at any Deal phase, and `walk_flop_deal` passes every seat's hole cards
to `evaluate()`, including folded seats. The evaluator must exclude folded
seats from pot competition. A seat is live iff
`ledger.seats[s].folded == false`. Only live seats' hands are evaluated and
compared; folded seats receive equity 0. Folded seats' contributed chips
remain in the pot (dead money) — the pot sum already includes them, so no
special handling is needed. The runout pool blocks all dealt cards (board
plus every seat's hole cards, including folded seats), matching the existing
sampler's physical-deck interpretation.

For each turn/river combo, evaluate all live seats' seven-card hands, find
the maximum score among live seats, count the number of live seats tied at
the maximum (K), and award each tied live seat a `1.0 / K` share of that
runout. A single-pass variant stores scores in a local array to avoid
redundant evaluations:

```cpp
const std::size_t n = hands.size();
// Determine live seats from the ledger.
std::size_t live_count = 0;
for (std::size_t s = 0; s < n; ++s)
  if (!ledger.seats[s].folded) ++live_count;
if (live_count == 0)
  throw std::invalid_argument("equity frontier evaluator: no live seats at frontier");

std::vector<double> equity_sum(n, 0.0);
for (int i = 0; i < pool_size; ++i) {
  for (int j = i + 1; j < pool_size; ++j) {
    std::uint32_t scores[kMaxUnifiedSeats] = {};
    std::uint32_t best = 0;
    for (std::size_t s = 0; s < n; ++s) {
      if (ledger.seats[s].folded) continue;  // folded seats do not compete
      const int cards[7] = {hands[s][0], hands[s][1], flop[0], flop[1], flop[2], pool[i], pool[j]};
      scores[s] = bs::evaluate(cards, 7).score;
      best = std::max(best, scores[s]);
    }
    int tied = 0;
    for (std::size_t s = 0; s < n; ++s)
      if (!ledger.seats[s].folded && scores[s] == best) ++tied;
    const double share = 1.0 / tied;
    for (std::size_t s = 0; s < n; ++s)
      if (!ledger.seats[s].folded && scores[s] == best) equity_sum[s] += share;
  }
}
const int total = pool_size * (pool_size - 1) / 2;  // C(pool,2)
for (std::size_t s = 0; s < n; ++s)
  equity[s] = equity_sum[s] / total;
```

In heads-up, a fold ends the hand at a `TerminalFold` leaf, so the frontier
is only reached with both seats live. The folded-seat path is therefore a
multiway-only concern; the HU behavior is unchanged.

**Single-pot settlement precondition:** the `equity * pot - contributed`
formula settles a single pot with no side pots. Side pots form only when a
player is all-in for less than another live player's contribution. With
equal stacks (the builder's target), an all-in player commits the full
stack and every caller matches it, so no side pot can form. To enforce this
precondition, GameDef validation for flop-terminal games requires all seats
to have equal stacks (see section 4). The evaluator documents this
precondition; unequal-stack flop-terminal games are rejected at GameDef
validation, never reaching the evaluator.

**Runout count:** the pool is `52 - 3 - 2N` cards. For N=2: 45 cards, 990
combos. For N=3: 43 cards, 903 combos. For N=6: 37 cards, 666 combos. For
N=10: 29 cards, 406 combos. The total runout count shrinks as N grows; the
per-runout cost grows linearly with N. The net cost is bounded.

**Cache key:** replace the `uint64_t` 42-bit packing with an exact,
collision-free key. The key is the sorted card vector: the 3 flop cards
(ascending, as dealt) followed by all hole cards (sorted within seat, seats
in order). Use `std::vector<int>` as the `unordered_map` key with a custom
hash (FNV-1a over the card bytes). The map's `operator==` on vectors is
exact, so a hash collision cannot return a wrong equity — it only degrades
to a bucket collision resolved by equality.

**Cache value:** replace `std::array<double, 2>` with `std::vector<double>`
(N equity fractions).

**Cache cap:** the cache grows unboundedly for N-way preflop training (the
joint deal space is 1,326^N, so revisits are rare). Add a capacity cap
(default 1,000,000 entries). When the cap is reached, stop inserting new
entries; existing entries remain valid and lookups still hit. This bounds
memory while preserving correctness. The cap is a constructor parameter
for testability.

**Pot and utility:** the pot computation already iterates
`ledger.player_count` and sums `ledger.seats[s].contributed` — it
generalizes to N without change. The utility is
`equity[s] * pot - ledger.seats[s].contributed` per seat, generalized from
2 to N return values.

### 3. Blind-seat helper export

`engine/include/bs/game_definition.hpp`: declare `small_blind_seat()`,
`big_blind_seat()`, and `preflop_first_actor()` as public free functions in
`bs::poker`. Move their definitions out of the anonymous namespace in
`game_definition.cpp` (or keep them in the .cpp with external linkage).

The GameDef validation and GameState constructor continue to call these
helpers; the artifact reader/writer now calls them too, replacing the
hardcoded 2-seat convention.

### 4. GameDef validation relaxation

`engine/src/poker/game_definition.cpp`: replace the flop-terminal
player-count gate:

```cpp
// Before:
require(def.player_count == 2,
        "flop-terminal games are heads-up only; multiway frontier evaluation is out of scope");

// After:
require(def.player_count >= 2 && def.player_count <= kMaxUnifiedSeats,
        "flop-terminal games support 2..10 seats");
```

The root validation (`preflop && board_size == 0` or `!preflop &&
board_size == 3`) is unchanged.

Add an equal-stack precondition for flop-terminal games. The frontier
evaluator settles a single pot with no side pots; side pots form only when
a player is all-in for less than another live player's contribution, which
cannot happen with equal stacks. Reject unequal stacks at validation so the
evaluator never sees a side-pot-shaped ledger:

```cpp
if (def.terminal == TerminalDepth::Flop) {
  const Chips stack0 = def.stacks[0];
  for (std::size_t s = 1; s < def.player_count; ++s)
    require(def.stacks[s] == stack0,
            "flop-terminal games require equal stacks (single-pot settlement)");
}
```

### 5. Trainer gate relaxation

`engine/src/gto/nseat_trainer.cpp` `validate_request()`: remove the
`if (def.player_count != 2) throw` inside the flop-terminal preflop branch.
The outer check (`player_count < 2 || player_count > kMaxUnifiedSeats`)
already enforces the valid range. The branch becomes:

```cpp
if (def.board_size == 0 && def.preflop && def.terminal == poker::TerminalDepth::Flop) {
  // Flop-terminal preflop root: accepted for 2..10 seats (RFC 0010).
  // The frontier evaluator supplies leaf values.
} else {
  throw std::invalid_argument(
      "nseat trainer requires a postflop root (3..5 public cards) or a "
      "flop-terminal preflop root");
}
```

The `walk_flop_deal()` path is unchanged — it already passes N hands to
`evaluate()`.

### 6. Artifact writer generalization

`engine/src/artifacts/strategy_artifact.cpp` `validate_game_v2()`:

- Replace `check(game.player_count == 2, ...)` with
  `check(game.player_count >= 2 && game.player_count <= poker::kMaxUnifiedSeats, ...)`.
- Replace the hardcoded 2-seat blinds validation with the exported helpers.
  GameDef validation caps the small blind at the seat's stack after ante
  (`std::min(small_blind, stack_after_ante)`); the writer mirrors this rule
  so a capped-blind game is accepted consistently rather than rejected by
  the writer after passing GameDef validation:

```cpp
const Chips sb = game.big_blind / 2;
const std::size_t sb_seat = poker::small_blind_seat(game);
const std::size_t bb_seat = poker::big_blind_seat(game);
const Chips sb_capped = std::min(sb, game.stacks[sb_seat]);  // mirror GameDef cap
check(game.blinds_posted[sb_seat] == sb_capped, ...);
check(game.blinds_posted[bb_seat] == game.big_blind, ...);
// All other seats post 0.
for (std::size_t s = 0; s < game.player_count; ++s)
  if (s != sb_seat && s != bb_seat)
    check(game.blinds_posted[s] == 0, ...);
```

### 7. Artifact reader generalization

`engine/src/artifacts/strategy_artifact.cpp` reader:

- Replace `check(player_count_raw == 2, ...)` with
  `check(player_count_raw >= 2 && player_count_raw <= poker::kMaxUnifiedSeats, ...)`.
- Replace the hardcoded 2-seat blind derivation with the exported helpers:

```cpp
const std::size_t sb_seat = poker::small_blind_seat(game);
const std::size_t bb_seat = poker::big_blind_seat(game);
game.blinds_posted[sb_seat] = game.big_blind / 2;
game.blinds_posted[bb_seat] = game.big_blind;
```

For 2-seat games this produces the identical result (button = SB, other =
BB), so existing HU artifacts round-trip byte-identically.

### 8. Preflop profile builder extension

`engine/benchmarks/preflop_profile_builder.cpp`:

- Add a `player_count` parameter (usage: `bigshark-preflop-profile-builder
  <output-dir> [iterations] [stack-bb] [player-count]`, default 2).
- `make_preflop_def(stack_bb, player_count)`: set `player_count`, distribute
  stacks and `blinds_posted` using the exported helpers, set `pot = SB + BB`.
- `all_combos(player_count)`: return `player_count` ranges of 1,326 combos
  each.
- The manifest records `player_count` and a `rules_id` of
  `rfc0010-multiway-preflop-v1` for N-way artifacts (the HU artifact keeps
  `rfc0009-unified-preflop-v1`).

The builder remains an offline tool, not a CTest.

**Rules identifier divergence.** The DB-level rules identifier is
`kRulesIdentifierPreflop` for all preflop games regardless of seat count;
the reader determines `is_preflop` from it, not from seat count. The
manifest's `rules_id` is a separate label: the HU artifact uses
`rfc0009-unified-preflop-v1` and the N-way artifact uses
`rfc0010-multiway-preflop-v1`. This is workable because readers key off the
DB identifier plus the `player_count` column. It also provides
forward-compatibility: an old reader that has not been generalized fails
closed on an N-way artifact via its `player_count_raw == 2` check
(strategy_artifact.cpp:2048), rejecting the artifact rather than
misinterpreting it.

### 9. NashConv tool generalization

`engine/benchmarks/preflop_nash_conv.cpp`: the walker already handles
FlopDeal leaves and N-way tree structures. The following changes generalize
it to N seats:

- **Joint deal sampling:** `sample_joint_deal()` currently hardcodes 4
  cards (2 per seat for 2 seats). Generalize to deal `2 * player_count`
  cards from the shuffled deck.
- **Value and aggregation arrays:** the per-seat value and gap arrays are
  `std::array<double, 2>` and the aggregation loops are `p < 2`.
  Generalize to `std::vector<double>` sized by `player_count`.
- **Exploitability formula:** N-way NashConv sums N per-seat best-response
  gaps; exploitability in BB is `NashConv / player_count` (each seat's
  average incentive to deviate).
- **Folded-seat handling:** the walker's FlopDeal leaf constructs the
  payload via `make_frontier_payload(frontier_state)`, which carries
  per-seat `folded` flags, and passes all seats' hole cards to
  `evaluate()`. The folded-seat competition rule from section 2 is
  inherited automatically — the evaluator excludes folded seats via the
  ledger. No separate fix is needed in the walker, but a regression test
  (below) pins the behavior.
- **Fallback policy:** the `check > call > fold` fallback at uncovered nodes
  is seat-agnostic and works unchanged for N seats.

## Dependency Rules

- `bs::poker` (game_definition) gains no new dependencies; the blind-seat
  helpers are moved from an anonymous namespace to the public header.
- `bs::gto` (equity_frontier) depends on `bs::poker` (for `kMaxUnifiedSeats`)
  and `bs::eval` (hand evaluation) — unchanged direction.
- `bs::artifacts` (strategy_artifact) gains a dependency on the
  newly-exported `bs::poker` blind-seat helpers. It already depends on
  `bs::poker` for `GameDef`, `TerminalDepth`, etc., so no new module
  dependency is introduced.
- `bs::solver` (nseat_trainer) depends on `bs::gto` (frontier evaluator) —
  unchanged.
- No dependency from `bs::poker` or `bs::gto` toward `bs::artifacts` or
  `bs::solver`.

## Compatibility and Migration

**Existing HU artifacts are unaffected.** The reader's generalized blind
derivation produces the identical result for `player_count == 2` (button =
SB, other = BB). The `same_game_def` round-trip is byte-identical for
existing HU preflop artifacts. A conformance test pins this: load the
published HU artifact, reconstruct the GameDef, and verify
`same_game_def` against the original.

**No schema change.** The schema-v2 `game` table already stores
`player_count`. N-way preflop artifacts use the same schema with
`player_count > 2` and `rules_id = kRulesIdentifierPreflop`. The reader
determines `is_preflop` from the rules identifier, not from seat count.

**No migration.** Existing artifacts are not rewritten. New N-way artifacts
are published alongside the HU one.

**Gate relaxation is backward-compatible.** Every gate that changes from
"reject N != 2" to "accept 2..10" is a strict superset: 2-seat behavior is
unchanged, 3..10-seat behavior changes from rejection to acceptance.

## Security and Operational Impact

- **Credentials:** no change. The frontier evaluator and trainer are offline
  components with no credential access.
- **Fairness:** the frontier evaluator uses only public information (board
  and hole cards passed by the trainer). It does not infer unrevealed cards
  or use hidden information. The N-way generalization preserves this.
- **Resource limits:** the equity cache cap (1M entries) bounds memory. The
  per-evaluation cost is bounded (max 406 runouts x 10 hand evaluations for
  N=10). The trainer's existing node/information-set/wall-time caps apply.
- **Observability:** the preflop profile builder manifest already records
  tree size, info sets, bytes, and wall time. The manifest gains
  `player_count`. The NashConv tool reports exploitability in chips and BB.
- **Deployment:** offline tooling only. No live engine, service, or protocol
  change. The live v1 decision path is explicitly out of scope.

## Alternatives Considered

**Monte Carlo equity.** Instead of enumerating all turn/river runouts, sample
a subset. Rejected: the exact enumeration is fast enough (max ~4,000 hand
evaluations per `evaluate()` call, microseconds) and introduces no sampling
variance. The heads-up evaluator already enumerates exactly; the N-way
generalization preserves this property.

**Precomputed equity lookup table.** Precompute a table of equities for all
(flop, joint deal) tuples. Rejected: the joint deal space is 1,326^N —
infeasible to precompute for N > 2. The per-call enumeration is cheap and
the cache handles repeated (flop, hands) pairs.

**Keep DeclaredFrontierTable and generalize it too.** The
`DeclaredFrontierTable` uses fixed-size `std::array<..., 2>` entries.
Generalizing it to N-way requires a variable-size entry and key. Rejected
for this RFC: the table is not used in the preflop path (it is RFC 0007
option B, deferred). Generalizing it is dead code until nested evaluation is
revived.

**Separate N-way evaluator class.** Add a `MultiwayEquityFrontierEvaluator`
alongside the heads-up one. Rejected: the evaluation logic is identical
(enumerate runouts, evaluate hands, split ties); only the seat count
changes. Two classes would duplicate the enumeration and caching logic. One
generalized class is simpler and the HU path is a strict subset.

**Schema version bump to v3.** Bump the artifact schema to distinguish
N-way preflop artifacts. Rejected: the schema already stores `player_count`
and the reader derives blinds deterministically. No new columns are needed.
A version bump would require a migration for no benefit.

## Risks

- **N-way equity correctness.** A bug in the multiway pot-splitting logic
  (e.g., wrong tie counting, wrong pot share, folded seats competing) would
  produce wrong frontier values and a wrong policy. Mitigation: unit tests
  pin equity for known scenarios (all-in with N players, split pots,
  folded-seat exclusion, single-live-seat). The folded-seat rule is
  explicit in the design (section 2) and covered by a regression test.
- **Side-pot mis-settlement.** The `equity * pot - contributed` formula
  settles a single pot. With unequal stacks, an all-in player could create
  a side pot that the formula mis-settles. Mitigation: GameDef validation
  requires equal stacks for flop-terminal games (section 4), so the
  evaluator never sees a side-pot-shaped ledger. The precondition is
  documented and tested.
- **Cache key collision.** A hash collision in the cache key could
  theoretically return a wrong equity. Mitigation: the key is a
  `std::vector<int>` with exact `operator==` comparison in the
  `unordered_map`; a hash collision degrades to a bucket collision resolved
  by equality, never a wrong value. A test pins that two different (flop,
  hands) pairs with the same hash (forced via a bad hash) still return
  correct equities.
- **Artifact round-trip drift.** The generalized blind derivation might
  produce a different `blinds_posted` than the hardcoded 2-seat convention
  for edge cases. Mitigation: a conformance test loads the published HU
  artifact and verifies `same_game_def` round-trips exactly. The 2-seat
  branch of `small_blind_seat`/`big_blind_seat` is identical to the
  hardcoded convention.
- **N-way tree size.** An N-way preflop tree has more action nodes than a
  heads-up tree (more players, more action sequences). The FlopDeal leaf
  keeps chance out of the tree, but the action subtree could still exceed
  the 1 GiB cap for deep stacks and large N. Mitigation: the builder
  reports tree size and the trainer's existing caps apply. The first target
  is N=3 at 100 BB, which is expected to fit comfortably (the HU tree is
  5,326 nodes; a 3-way tree is roughly 3x the action sequences).
- **Training convergence at N-way.** MCCFR convergence may be slower for
  N-way games (more information sets, sparser visits per set). Mitigation:
  the builder reports termination phase and completed iterations; the
  NashConv tool measures exploitability. If convergence is insufficient,
  increase iterations or reduce the stack depth.

## Verification Plan

### Unit tests

- `test_equity_frontier.cpp` (new or extended):
  - HU equity matches the existing heads-up evaluator (regression).
  - 3-way equity on a fixed flop and deal: pin exact equity fractions.
  - Split pot: three players with identical hands (e.g., same pair on a
    paired board) each get 1/3 equity.
  - Two players tie, third has worse: the two tied players each get 0.5
    equity, third gets 0.
  - **Folded-seat exclusion:** 3-way flop-terminal game where one seat
    folds preflop. The frontier value excludes the folded hand: the folded
    seat gets equity 0, and the two live seats' equities sum to 1 (their
    contributions stay in the pot as dead money). Pin exact fractions.
  - **Single live seat:** all but one seat fold; the live seat gets equity
    1.0 (guards the degenerate frontier case).
  - Cache: repeated calls with the same (flop, hands) return identical
    values; calls with different hands return different values.
  - Cache cap: after the cap, new entries are not inserted but existing
    entries still hit.
  - Reject N < 2 and N > 10 with `std::invalid_argument`.
- `test_game_definition.cpp`: flop-terminal games with player_count 3, 6,
  10 validate; player_count 1 and 11 reject. Flop-terminal games with
  unequal stacks reject (single-pot settlement precondition).
- `test_solve_nseat.cpp` (or `test_nseat_trainer.cpp`): train a 3-way
  flop-terminal game; verify it completes and produces a policy. Include a
  case where one seat folds preflop and the frontier is reached with two
  live seats.
- `test_strategy_artifact.cpp`: write and read a 3-way preflop flop-terminal
  artifact; verify `same_game_def` round-trips. Verify the published HU
  artifact still round-trips byte-identically.

### Conformance and regression

- Full verification matrix after every C++ change:
  `cmake --preset release`, format, build, `ctest --preset release`,
  `benchmark-multistreet`, `npm run check`, `npm run proto:check`,
  `node bin/replay.mjs`.
- `debug` and `asan` presets for memory/lifetime/bounds coverage.
- `node bin/check-docs.mjs` and `node bin/check-rfcs.mjs` after documentation
  changes.

### Measurements

- **N-way equity cost:** measure `evaluate()` wall time for N=2, 3, 6, 9
  with and without a warm cache. Report microseconds per call.
- **3-way preflop profile:** train at 100 BB with 100K iterations (matching
  the HU profile). Report tree nodes, action nodes, information sets,
  artifact bytes, training wall time, and termination phase.
- **3-way NashConv:** run the generalized `bigshark-preflop-nash-conv` tool
  on the 3-way artifact. Report exploitability in chips and BB at 10K MC
  deals.
- **HU regression:** re-run the HU NashConv tool on the existing HU artifact
  and verify the measured exploitability is unchanged (99.24 chips / 49.62
  BB within MC noise).

## Rollout Plan

1. **Stage 1 — Evaluator + gates.** Generalize `EquityFrontierEvaluator`,
   export blind-seat helpers, relax GameDef/trainer/artifact gates. Add unit
   tests. Full matrix green. No artifact published yet.
2. **Stage 2 — Builder + measurement.** Extend the preflop profile builder
   to N-way. Train a 3-way 100 BB profile. Generalize the NashConv tool and
   measure 3-way exploitability. Report all measurements.
3. **Stage 3 — Documentation.** Update `docs/design/gto-engine.md` (frontier
   evaluator section), RFC 0007 (scope note pointing to RFC 0010), and the
   RFC 0009 implementation plan. Run doc/RFC checks.
4. **Stage 4 — Review.** Independent approval-agent review of the full
   change. Resolve blocking findings. Mark RFC 0010 `Implemented`.

Each stage is independently committable and verifiable. The live v1
decision path is not changed in any stage.

## Rollback Plan

- **Stage 1 rollback:** revert the evaluator/gate commits. The gates return
  to rejecting N-way flop-terminal games. No persisted data is affected.
- **Stage 2 rollback:** delete the 3-way artifact and revert the builder
  extension. The HU artifact is untouched.
- **Stage 3 rollback:** revert documentation commits.
- **No data migration to roll back.** N-way artifacts use the same schema;
  no existing data is rewritten.

## Open Questions

- **First N-way target.** This RFC proposes N=3 as the first measured
  multiway profile. Should the builder also produce N=6 (6-max) in the same
  pass, or is N=3 sufficient for the first iteration? The implementation
  supports 2..10; the question is which artifacts to publish.
- **N-way artifact storage budget.** The HU artifact is 266 MB. A 3-way
  artifact is expected to be larger (more information sets). The storage
  budget for published artifacts is not yet defined. This is a publishing
  decision, not a design blocker.

## Acceptance Criteria

- `EquityFrontierEvaluator` supports 2..10 seats with exact all-in-at-flop
  equity, correct multiway pot splitting, and folded-seat exclusion,
  verified by unit tests.
- The flop-terminal player-count gates in GameDef validation, the nseat
  trainer, and the artifact reader/writer accept 2..10 seats. Flop-terminal
  games with unequal stacks are rejected (single-pot settlement
  precondition).
- The blind-seat helpers are exported from `bs::poker` and used by the
  artifact reader/writer; the published HU preflop artifact round-trips
  byte-identically.
- A 3-way 100 BB preflop profile is trained and published, with reported
  tree size, information sets, artifact bytes, wall time, and termination
  phase.
- The generalized NashConv tool measures 3-way exploitability; the HU
  NashConv regression is within MC noise of the baseline (99.24 chips).
- Full verification matrix green: release, debug, asan; benchmark-multistreet
  PASS; npm check 0 fail; proto check 0 fail; replay clean; docs check 0
  fail; RFC check 0 fail.
- Independent approval-agent review: Approved, with all blocking findings
  resolved.

## Decision

Pending independent approval-agent review.
