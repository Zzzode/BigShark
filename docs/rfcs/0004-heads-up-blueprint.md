---
rfc: "0004"
subject: "Heads-Up Multi-Size Blueprint Training"
status: "Implementing"
authors: "BigShark maintainers"
created: "2026-09-14"
updated: "2026-09-14"
owners: "engine poker, engine solver, engine benchmarks"
supersedes: ""
superseded-by: ""
---

# RFC 0004: Heads-Up Multi-Size Blueprint Training

## Summary

Introduce a stack-aware, multi-size heads-up no-limit Hold'em game and an
offline blueprint trainer under `engine/`. A blueprint is an average strategy
for an explicitly identified game, root range pair, and action abstraction.
It is not a claim of full-game equilibrium. Start with exact information
states and small postflop games, then expand measured coverage. Preserve the
existing production policy until separate runtime promotion gates pass.

## Motivation

The repaired multi-street solver provides useful convergence regression
evidence, but its validation game cannot represent real no-limit betting.
Extending that game's fixed node codes and fixed-size arrays would hide
incorrect rules behind additional special cases. A real betting-state
implementation is required for raises, stack limits, all-ins, and reusable
postflop strategies.

## Goals

- Model correct actor order, commitments, minimum raises, and terminal payouts.
- Support multiple bet and raise sizes on every postflop street.
- Produce deterministic, inspectable average policies and training state.
- Preserve information-set consistency and card-removal probabilities.
- Distinguish exact small-game exploitability from large-game estimates.
- Establish resource-bounded training for declared ranges and board families.

## Non-Goals

- Strategy storage or live resolving; RFC 0005 owns those contracts.
- Multiway, rake, side-pot strategy, or tournament utility; RFC 0006 owns them.
- A distributed trainer, GPU backend, neural value model, or plugin framework.
- Solving unrestricted full-game Hold'em on a developer workstation.
- Silently replacing preflop charts or changing existing v0 decisions.

## Current State and Evidence

- `engine/src/gto/multistreet_cfr.h` specifies IP-first action, one fixed bet
  per street, and no raises. `ISet` supports a small fixed action array.
- `multistreet_cfr.cpp` includes weighted private ranges, public chance,
  history keys, average policies, and information-set best-response evaluation.
  Its compact key and bucket model are specific to the validation game.
- `engine/tests/test_multistreet_ref.cpp` provides an independent fixed-run
  oracle. `engine/benchmarks/multistreet_cfr_benchmark.cpp` has three small
  fixture families; two disable buckets and one samples public chance with
  one private combination per side.
- `engine/src/gto/river_game.cc` demonstrates the existing private OpenSpiel
  boundary. Production river and multi-street validation games use different
  specialized trees; neither is a complete no-limit rules implementation.
- `engine/include/bs/decision.hpp` still uses `int` monetary fields.
  RFC 0002 requires exact 64-bit wire amounts; unchecked narrowing is invalid.
- Upstream [OpenSpiel external-sampling MCCFR](https://github.com/google-deepmind/open_spiel/blob/master/open_spiel/algorithms/external_sampling_mccfr.cc)
  samples chance and opponent actions and enumerates traverser actions.
  The local vendored subset does not include that algorithm.

The upstream source is a design reference, not a pinned implementation
dependency or evidence that the new trainer already works.

## Design Principles

1. Poker rules belong to `bigshark_poker`; abstractions and training belong to
   `bigshark_solver`.
2. All strategies use the same legal transition function.
3. Sampling hidden cards for training never makes them policy inputs.
4. Information keys retain observed history and the acting player's private
   observations. No bit truncation or hash-only equality is permitted.
5. An unsupported state is explicit; nearest-neighbor matching is not implicit.
6. Exact benchmarks remain independent of the production trainer.

## Proposed Design

### Ownership and interfaces

Add focused poker state and transition types under `engine/include/bs/` and
`engine/src/poker/`. Add a public heads-up solving facade under
`engine/include/bs/`, backed by private `engine/src/gto/` implementation.
Keep source lists and tests in `engine/CMakeLists.txt`.

The facade accepts a root game, two weighted ranges, an action-size schedule,
and iteration, memory, node, and deadline limits. It returns training status,
completed iterations, a queryable immutable average policy, and metrics.
Training state and immutable policy are distinct domain objects.

A solver-owned native executable in `engine/benchmarks/` exercises the facade
without platform dependencies. A later TypeScript solver CLI may orchestrate
native training; it cannot implement strategy.

### Root and amount semantics

The first profile is heads-up, cash, no ante, no rake, flop-rooted NLHE.
The root identifies button, board, big blind, each player's stack behind and
total contribution before the root, and the pot. Street commitments are zero
at this root. Root contributions must sum to the pot.

Amounts and sums are checked `uint64_t`; calculations reject overflow.
Expected utility uses double precision only after integer transitions.
Training profiles whose maximum total chips exceed `2^53 - 1` are unsupported
until a documented higher-precision numeric backend exists. The wire mapper
must report this limitation, not narrow into the old `Ctx` or alter units.

At each postflop street the out-of-position player acts first. Terminal
accounting awards pots and returns unmatched wagers before computing each
player's net utility relative to their chips and contributions at hand start.
For the two-player no-rake profile, utilities sum to zero.

### Legal transitions and action abstraction

State includes actor, street, board, stacks, street and total commitments,
last full raise size, and whether each player retains raise rights.

For current pot `P` (including all wagers already made), call cost `C`, and
acting player's street commitment `H`, a rational sizing fraction `n/d`
produces:

- open bet: `H + ceil(P * n / d)`;
- raise: `H + C + ceil((P + C) * n / d)`.

Use checked integer arithmetic. Intersect generated targets with rule-derived
legal target totals, include the minimum full raise and effective all-in,
and deduplicate after clipping. Fractions are positive reduced rationals,
not floating-point identifiers. Order actions by fold, check, call, then
ascending aggressive target total. Limit the abstract action list to 32.

The first schedule uses `1/3`, `3/4`, and `3/2` pot bets; `1/2` and `1/1`
pot raises; and an all-in target. Schedules are part of game identity.
No arbitrary raise-count cap is needed: full raises consume finite stacks.
Node and memory limits may stop training but cannot silently prune legal
branches or substitute heuristic terminal values.

A short all-in does not restore previously exhausted raise rights.
Checks or a completed call close the street only when no action remains.
An all-in call deals the remaining public cards and reaches showdown with no
additional betting. A fold returns unmatched excess before payout.

### Information states and chance

Initial private deals follow
`w0(i) * w1(j) / Z`, restricted to compatible cards and the root board.
Future public cards are sampled from the remaining deck of that same deal.
A private deal remains fixed until terminal.

An exact information state identifies the game, acting player, own two
cards, ordered public runout, full public action sequence including target
totals, and any earlier private abstraction observations. The opponent's
private hand is never included. Hash tables compare full keys.

Version 1 uses exact cards and public histories. Suit isomorphism may reduce
states only when the permutation preserves both weighted ranges, board,
seats, and every observed action. Do not merge boards solely by equity.
Lossy card buckets require a subsequent design and measured abstraction error.
This deliberately trades storage for a defensible first correctness baseline.

### Training

Implement one trainer for the new game, with full traversal on small fixtures
and external sampling on larger ones. Both traverse the same rules and
information keys. Use established external-sampling MCCFR (Monte Carlo
counterfactual regret minimization): sample chance and non-traverser actions,
enumerate traverser actions, and update regrets from action value minus node
value. At each visit copy the current policy before recursion.

Use ordinary regret matching and the two-player `AverageType::kSimple` rule
in [OpenSpiel revision 48401890](https://github.com/google-deepmind/open_spiel/blob/48401890ee9857e611678302371378175a8e4c6b/open_spiel/algorithms/external_sampling_mccfr.cc).
Do not combine discounting, exploration mixing, or pruning with this baseline.
Positive regrets normalize to the current policy; all-zero regrets initialize
uniformly. Run traverser 0, then traverser 1; complete one traversal before
starting the other. At a visit to information set `I`, freeze local policy
`sigma` before recursion. At chance/opponent nodes sample from the actual
chance/current-policy distribution. At traverser nodes enumerate all actions.

For traverser `t`, compute sampled child values `v(a)` and
`v = sum_a sigma(a) * v(a)`. After recursion update
`R(I,a) += v(a) - v` only when actor equals `t`. When actor equals `1-t`,
update `S(I,a) += sigma(a)` on each sampled visit using the pre-recursion
snapshot. Do not multiply these sampled updates by private-deal probability
or own reach again: their visit sampling already supplies those factors.
Normalize `S` only when exporting behavior. The full traversal path explicitly
weights regret by opponent/chance reach and averages by own reach; it visits
each information set once for average accumulation, avoiding repeated hidden
histories. At a fixed information set, sampled unnormalized averages may
differ by a constant chance factor; compare normalized expected averages.

PRNG revision 1 is SplitMix64 with a single 64-bit state initialized to seed:
increment by `0x9e3779b97f4a7c15`; apply the standard xor-shift/multiply
mix with `0xbf58476d1ce4e5b9` and `0x94d049bb133111eb`, shifts 30, 27,
and 31, using modulo-2^64 arithmetic. Unit doubles use the top 53 output bits
times `2^-53`. Bounded integer sampling rejects values below
`(0 - bound) % bound` before taking modulo. Weighted sampling visits combos
in ascending ID and uses cumulative positive weights; public cards ascend
by ID and player/action order is fixed. Never traverse an unordered map to
consume randomness.

Record algorithm revision, PRNG state, traversal order, and iteration count.
An interrupted iteration restores the PRNG, regrets, averages, and newly
allocated information states to their iteration-start state. Both traversals
and any average update must complete before publishing that iteration.
Per-iteration deltas include updates visible to later traversal visits.

The old multi-street implementation remains a frozen regression reference
during validation, with no live routing. Remove its duplicate training path
only after equivalent legacy-game fixtures can be evaluated by the new
implementation and the independent oracle remains intact. The river solver
continues serving its current callers.

### Coverage and evaluation

A blueprint covers exactly its root board, range pair, stacks, rules, and
action schedule plus their reachable continuations. No silent stack scaling,
range truncation, uniform fill for untrained states, or board substitution.
Publish a coverage miss for incomplete/unvisited information states.

Small fixtures report the existing convention:
`exploitability_pot = sum of both best-response gains / root pot`.
This is normalized NashConv, not the half-NashConv convention used by some
libraries. Exact best response aggregates hidden opponent reach before
choosing one action per information state.

Large-game reports label sampled deviation measurements as estimates or
lower bounds, never certified exact exploitability. Record seeds, iterations,
policy revision, range sizes, node count, peak resident memory, elapsed time,
and missing-policy coverage. A frozen release suite uses at least dry,
two-tone connected, paired, and monotone flops; equal and asymmetric stacks;
and stack-to-pot ratios 1, 4, and 10.

Preflop replacement is a later stage using the same rules and trainer:
the button/small blind acts first preflop, the big blind retains its option
after a limp, and postflop order reverses. Export solved continuation ranges
from actual policy reach. Keep current charts wherever the trained profile
does not cover the game. Six-max charts are not replaced by a heads-up model.

## Dependency Rules

- `bigshark_solver -> bigshark_poker`; no transport, SQLite, or provider types.
- Policy consumes public solver results; it never includes private headers.
- Native benchmarks may include solver-private test hooks under `engine/`.
- External CLI orchestration consumes public native interfaces only.
- RFC 0002 owns wire-to-domain mapping; this RFC adds no wire fields.

## Compatibility and Migration

The shared poker module gains additive rules types because chips and legal
transitions are invariants required by both training and later utility models.
Solver-local rules or adapter-side rules would duplicate those invariants.
Existing `Ctx`, v0 entry points, specialized river behavior, and session files
remain unchanged during offline implementation.

This is an intentional strategy-model change, so eventual live differences
must be separated from v0/v1 protocol-parity work. No new solver source is
advertised before runtime integration in RFC 0005.

## Security and Operational Impact

Training uses synthetic deals or reviewed public records. Never use hidden
live opponent cards or unreviewed session data for player profiling.
No network access is needed for training. Default local runs stop at 1 GiB
solver allocations, one million information sets, or 60 seconds; explicit
limits are required for larger runs. Allocation limits include temporary
iteration deltas. An estimate is checked before starting.

Exhaustion yields incomplete training status and the last complete policy,
not a success flag or a production-ready artifact. Larger compute jobs need
an explicit resource budget; this RFC does not authorize cloud spending.

## Alternatives Considered

- Extend the validation tree: rejected because its actor and commitment model
  is incompatible with real betting.
- Generalize all existing river solvers at once: rejected due to production
  parity risk; the new offline game is a bounded migration.
- Import the entire OpenSpiel tree: unnecessary for a focused poker game.
  Use its published algorithm as a reference and retain the current private
  dependency boundary.
- Begin with lossy buckets or a neural solver: deferred until exact small-game
  correctness and capacity evidence expose a concrete scaling requirement.

## Risks

- Exact-card storage grows rapidly. Sparse coverage is reported explicitly;
  failing capacity goals requires a new abstraction decision, not a GTO claim.
- Full no-limit raises expand traversal cost. Resource aborts must be bounded
  within an iteration and must not corrupt resumable state.
- Numerical and library differences prevent bitwise cross-platform promises.
  Require same-build repeatability and tolerance-based cross-platform checks.
- Existing legacy CFR evidence does not validate new rules or sampled averages.
  Independent transition, reach, and best-response tests are mandatory.

## Verification Plan

Use the complete `AGENTS.md` C++ build contract plus ASan/UBSan.
Add project-native tests for actor order, min-raise/reopening, uncalled bets,
short calls, all-in runouts, exact chip conservation, overflow, and illegal
transitions. Compare tiny-game terminal distributions and BR values against
an independent enumerator.

Keep all three existing benchmark gates unchanged. Add multi-size fixed-run
and sampled-chance fixtures with at least two combinations per side and
seeds 1, 17, and 43. Proposed release gates are normalized NashConv at most
0.002 for fixed small games and 0.02 for sampled small games by one million
iterations; final values must improve on the first checkpoint. Same-build
repeats must match within `1e-12`. Missing coverage is a failure in these
quality fixtures.

Before convergence tests, independently enumerate one-step sample outcomes
and their probabilities for fixed policies. Compare expected regret updates
and normalized average updates against full traversal, including unequal
range weights, overlapping cards, repeated hidden histories, and zero-policy
actions. Verify interrupted-and-resumed training against uninterrupted runs,
including PRNG state and information-set allocation.

Introduce new benchmark output with its own version; do not reinterpret the
existing CSV. Separate longer capacity runs from default CTest. Record actual
hardware and results before claiming broader coverage.

## Rollout Plan

1. Rules and independent terminal tests, with no live policy change.
2. Full-traversal trainer and exact small-game oracle.
3. Sampled traversal, average-policy tests, and resource interruption tests.
4. Multi-size benchmark matrix and measured postflop coverage.
5. RFC 0005 storage and opt-in runtime integration.
6. Heads-up preflop roots and policy-derived continuation ranges after
   postflop gates pass; separate training and quality evidence.

Stop on legality, hidden-information, conservation, oracle, or convergence
failures. Do not weaken thresholds merely to finish a stage.

## Rollback Plan

Disable selection of the new offline/runtime profile; production retains the
existing policy. Keep regression tests and artifacts isolated by algorithm
and game version. No old model is overwritten or reinterpreted.

## Open Questions

- What compute budget should fund training beyond the bounded local suite?
  Until specified, only the local limits above are authorized.
- Broader six-max preflop equilibrium coverage depends on RFC 0006 evidence;
  a heads-up artifact must not claim that coverage.

## Acceptance Criteria

- Correct real heads-up betting rules pass independent native tests.
- Full and sampled traversal pass the declared quality and repeatability gates.
- Coverage, cost, incomplete training, and unsupported profiles are explicit.
- All published actions obey the same native legal transition model.
- Heads-up preflop and postflop training have distinct coverage evidence.
- Current production decisions remain unchanged until separately promoted.
- Documentation reports measured coverage without full-game GTO claims.

## Implementation Evidence

Stage 1 is complete on 2026-09-14:

- `engine/include/bs/heads_up.hpp` and `engine/src/poker/heads_up.cpp`
  implement immutable flop-rooted heads-up rules in `bigshark_poker`.
- `engine/tests/test_heads_up.cpp` adds release-active fixed scenarios and
  independent exhaustive legality, ledger, and settlement checks over 150
  small-stack roots: 12,226 nodes, 2,852 folds, and 3,002 showdowns.
- An independent implementation reviewer found no blocking static issues.
  A separate test author supplied the reference oracle; the main agent
  reviewed and ran it through the public API.
- Release CTest passed 17/17; ASan/UBSan passed 16/16; Node passed 32/32;
  Protobuf passed 7/7; the three existing benchmark families passed.
  Replay covered 156 decisions with zero illegal actions and zero JS fallbacks.
- Existing production routing, v0/v1 protocol, and specialized river solver
  behavior remain unchanged.

The new full/sampled trainer, multi-size abstraction, blueprint, and preflop
training remain pending. This evidence does not complete the RFC as a whole.
This change was verified on macOS; its new Linux build has not yet been run.

Stages 2 and 3 are complete on 2026-09-14 (working-tree checkpoint):

- The full-traversal facade, exact information keys, joint chance model, and
  independent modeled best response are in `heads_up_solver.{hpp,cpp}` with
  independent pure-response and allocation-fault tests.
- External sampling implements the pinned two-player kSimple rule and
  SplitMix64 revision 1. The full average keeps the pinned
  once-per-information-set own-reach convention; sampled and full averages
  match only after normalization, and sampled regrets match as raw values.
- Enumerated expected-update tests use hand-derived literals under
  non-uniform weights, a non-fixed public card, repeated hidden histories,
  and a real three-action multi-size fixture. Repeatability and iteration/
  PRNG rollback are tested under every resource cap.
- Version-2 default and manual capacity benchmarks pass the pinned
  `0.002` fixed and `0.02` sampled gates at 1,000,000 iterations for seeds
  1, 17, and 43, with per-policy-row repeat delta zero. Release CTest was
  23/23 and ASan/UBSan 21/21; legacy benchmark values are unchanged.
  Full numbers and the reference machine are recorded in the
  RFC 0004-0006 implementation plan.

The frozen Stage 4 matrix, durable artifacts, resident lookup, production
wiring, and preflop training remain pending. This still does not complete
the RFC as a whole; the new targets are verified on macOS only.

## Decision

Author agent: /root
Approved by: 7f86ec31-e5b6-4753-a478-a48366ac2edd
Decision date: 2026-09-14
Review outcome: Approved
Reviewed scope: Complete proposal, including staged heads-up preflop work; pre-decision SHA-256 7af01d6a0f48c56e7496f41517ca4e86710a47b825222b30a5896b8f75eb5a00.
Review summary: Formal independent review found coherent ownership, information states, dealing, amount semantics, sampling, rollback, and quality gates. The pinned OpenSpiel kSimple rule was checked. Exact-state capacity, convergence cost, and numeric reproducibility remain implementation gates; postflop evidence alone does not complete the preflop acceptance criterion.

Design approval only. No training, live play, cloud expenditure, or completion
of acceptance criteria is implied.
