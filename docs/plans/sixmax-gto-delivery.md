# Complete Six-Max GTO Delivery

Status: Current

User-feature status: In Progress. Complete six-max GTO is not implemented or
qualified. This register records verified gaps and internal work, not a release
claim. The user requires the complete feature; successful internal milestones
must never be presented as a partially completed user feature.

## Intended Outcome

Operate through the official River Club interface at a six-player no-limit
Hold'em table, preserving all six dealt-player positions and public history
through every street. Cover the ordinary 2..6-live-player pots, unequal current
stacks, all-ins, side pots, raise reopening, and legal nonstandard bet sizes.
Strategy provenance, quality, coverage, and complete decision latency must be
independently qualified. A legal operational fallback is not GTO support.

Rules/data availability and quality acceptance must be explicit. A 100 BB
hand-start fixture is an engineering reference, not a restriction excluding
normal later hands or unequal stacks. No task may close by shrinking to
heads-up, tiny stacks, singleton holdings, or a uniform-prior solve and calling
that the complete six-max feature.

## Verified Starting Point (2026-10-14)

The checkout was clean before this work. Rules and several interfaces already
accept 2..10 seats, but this does not establish six-max strategy coverage.
Read-only audits used the actual mapper, validator, envelope, host, resident,
trainer, subgame resolver, native tests, and local artifact manifests.

| Gap | Evidence | Effect |
| --- | --- | --- |
| Multiway preflop reconstruction refused | `engine/src/protocol/v1_resident_mapper.cpp`, `reconstructPreflop` hardcodes two players and two-element ledgers | Six-seat requests miss before existing policy lookup |
| Postflop dead-money reconstruction incomplete | Same mapper assumes equal per-seat flop contributions and does not replay preflop folds into its root | Ordinary preflop-folded six-seat hands are not accurately reconstructed |
| Side-pot utility unavailable over v1 | `engine/src/protocol/v1_semantic_validator.cpp` rejects side pots | Unequal all-in hands safely fail, not GTO support; RFC 0006 requires a coherent follow-up protocol |
| ~~Unequal hand-start stacks refused for flop-terminal games~~ | `engine/src/poker/game_definition.cpp`; RFC 0010 single-pot frontier invariant | **Resolved 2026-10-17:** guard removed; side-pot-safe per-layer settlement supplies correct utility (RFC 0010 update) |
| No six-seat published strategy found | Local artifact manifests have at most four players | Existing two/three/four-player files cannot be relabeled or projected as a six-seat game |
| Practical six-seat online costs and quality unqualified | Native subgame success fixtures cover two/three/four players; full tree construction precedes training wall checks | A six-seat type gate alone does not make a normal-stack decision affordable |
| Nonstandard action coverage missing | Resident action paths require exact targets and return `OffTreeAmount` | A legal off-tree amount can miss policy/range lookup; rounding is not a justified remedy |
| Actor attribution can guess on incomplete River text | `platforms/river-club/src/v1-mapper.ts` compatibility attribution | Precise six-seat replay needs reliable public identity/history, not inferred actors |
| Complete provider ledger/history not guaranteed | Official local `docs/SKILL.md` omits old events; `RiverEvent` has kind/text, not authoritative actor/sequence/street/amount; OpenAPI room is generic | Public cross-snapshot accounting needs explicit completeness; midway attach, lost actions, ambiguous names and rounded amounts cannot be guessed |
| No-root resolving entry gated | Host resolver advertisement requires resident roots | The uniform-no-blueprint implementation does not automatically imply executable no-root support |
| Engine practice restricted to heads-up | `engine/benchmarks/practice_simulator.cpp` count gate and single bot injection | No six-player production-engine whole-hand practice acceptance entry |

Existing native mapper/validator/minor2/resolving tests passed 4/4; the River
differential suite passed 17/17, including a declared side-pot fallback. Native
practice tests passed for six-seat easy/medium policies, not six-seat GTO.
The current engine practice CLI rejected six seats with exit 2.

The official-data audit found no guaranteed hand-start stack, gross/refund
ledger, or complete historical action stream. Optional River `pots` can carry
eligibility for every layer, including the main pot; v1 only carries a scalar
main pot and requires at least two eligible players per encoded side pot.
This is why deleting v1 refusal is not a coherent side-pot implementation.
No authoritative local declaration of provider rake/ante/reopening rules was
found. These are documented-data gaps, not proof that the live provider lacks
the data or uses a particular rule; no live or private endpoint was inspected.

T0/T1b must audit authoritative public fields and observation completeness.
Only uniquely explained public transitions can accumulate a trusted hand
ledger; an unobserved interval, ambiguous actor, or incomplete history must
remain visibly incomplete, not enter exact reconstruction. Preserve any
available public pot-layer eligibility, including single-eligible layers.

## Work Register

| ID | Owning modules | Required internal outcome | Dependencies | Status |
| --- | --- | --- | --- | --- |
| T0 | Protocol, solver, product documentation | Whole-feature rules/support matrix, public-data audit, independently reviewed quality/latency/coverage acceptance; no weakened labels | None | In Progress: gaps audited; quality reference/threshold still to be specified |
| T1a | v1 protocol and native tests | Exact minor-2 N-seat preflop replay; frozen minor-0/1 compatibility | RFC 0019 approval | Complete: RFC 0019 implemented, N-seat preflop replay and minor-0/1/2 compatibility tests pass in release |
| T1b | Protocol, River adapter; resident/resolver identity prerequisites | Complete public preflop-to-postflop ledger, folded/dealt status and trusted actor history | T0, T1a, RFCs 0020/0021/0022; follow-up contract approval | In Progress: honest observation provenance via intake ledger implemented (118/118 Node tests pass); supplied-history replay and root identity passed release; RFC 0022 v2 wire/behavioral/vector design Accepted 2026-10-16; ASan suite 69/72 (3 pre-existing failures: protobuf+ASan annotation, stale resident temp dir) |
| T2 | Poker utility, protocol, frontier, artifacts | Unequal-stack and side-pot-safe state/utility/identity; no imaginary `pot / n` contributions | T0, T1b; RFC 0006 follow-up approval | In Progress: side-pot-safe per-layer frontier settlement implemented (RFC 0010 update, independently approved); equal-stack guard removed; utility identifier replaced; 18 native ledger baselines passed focused release/ASan; RFC 0022 v2 wire design Accepted 2026-10-16; six-seat preflop profile trained (1.0 GB, 100K iterations, 100 BB, raise-cap=1); six-seat flop library trained (8 classes, 1.9 GB, 1000 iter/class) |
| T3 | Solver, abstraction, native benchmarks | Independent six-seat quality and normal-stack resource/deadline measurements before scaling | T0 | In Progress: [feasibility + quality measured](t3-six-seat-feasibility.md); raise-cap + depth-limited resolving makes all 15 fixtures feasible; pooled-BR NashConv optimized (fixed-size BRView, multi-traverser walker, OpenMP, direct showdown); 100K-iteration quality campaign: 0.02-0.15 BB exploitability across 2-6 seats, 58-98% reduction vs uniform, 5-6 seat games near-equilibrium at ~0.02 BB; full 15-fixture scan <6 min at 1K iter, ~8 min at 100K iter |
| T4 | Solver, resident, artifacts | Six-position preflop policies with matching multiplayer continuation values and immutable provenance | T1a, T2, T3 | In Progress: N-seat continuation-range export (2..10 seats) with recursive joint-deal enumeration and card-mask filtering; --raise-cap flag wired in both offline builders and recorded in manifests; 2/3/6-seat range tests pass; 6-seat pipeline verified; full 100K-iteration six-seat profile trained (6,424-node tree, 519,168 info sets, 4,073,472 stored rows, 1.0 GB artifact, 430s training, 584s pipeline, 6.4 GB peak RSS, SHA-256 e3d77d55); six-seat flop library trained (8 classes, 1.9 GB) |
| T5 | Range propagation, resolver, resident | Action-conditioned 2..6-live-player flop/turn/river strategy coverage preserving dealt-seat identity | T1b, T2, T3, T4 | In Progress: 1,755-class flop library training in progress (16 workers, 1000 iter/class, 100 BB, raise-cap=1, 1,872 info sets/class); 86/1,755 classes published; turn/river strategy remain open |
| T6 | Resolver, service, protocol, Node client, River runner | Off-tree handling and receipt-to-action budget including construction/lookup/solve; accurate source and fallback reports | T1b, T2, T5 | In Progress: RFC 0023 startup/deadline repair; solve-budget alignment fixed; receipt-to-action latency measurement implemented (envelope handler stamps solve_time_us on all successful decision paths); off-tree handling remains open (needs RFC) |
| T7 | Practice leaf and engine process client | All five bot seats served by the same host through complete six-seat, rotating-position hands | T1a, T1b, T2; additive CLI approval | Pending |
| T8 | All owning modules, native/Node acceptance | Complete support matrix, independent quality evidence, all positions/streets/resources/variants, immutable release and rollback validation | All earlier tasks | Pending |

The work register is not a second strategy implementation or new runtime
framework. Existing modules retain ownership. Shared/protocol/model changes
must identify why local closure is insufficient and receive independent RFC
approval before implementation. Open questions in future tasks are not silently
authorized by approving an earlier prerequisite.

The user requested an independently reviewed standard rather than supplying a
reference or error threshold. The [qualification proposal](sixmax-gto-qualification.md)
defines the separate coverage, quality, and resource gates. It remains Proposed:
guarantee-to-evidence justification, numerical thresholds and complete campaign
budgets still need independent approval before a passing claim is possible.

## Internal Checkpoint and Verification Policy

Start with T1a: the mapper barrier is proven, small, and blocks every later
six-seat preflop artifact. RFC 0019 confines that fix to its owner and preserves
legacy protocol behavior. Its new support still uses the existing equal-stack,
no-side-pot profile and therefore cannot close the user feature.

At each checkpoint retain all project-native regression and quality gates.
Protocol changes require full native release and true ASan/UBSan verification,
Node/protocol conformance, multistreet benchmarks, replay and documentation
checks. Empty genuine journals establish no historical coverage. Fake host
rows and synthetic snapshots establish engineering behavior, not poker quality.

Six-max promotion requires a separately reviewed test matrix and independent
evidence per actual supported game. Deviation estimates are not exact best
response bounds; low measured gain or self-play wins alone cannot establish
the required whole-game GTO quality. If quality cannot be established, retain
In Progress and disclose the research/implementation gap.

RFC 0023's runtime prerequisite passed independent implementation review and
156/156 Node tests against each of
the real Release and true Debug ASan engine targets, with zero skipped cases;
protocol tests passed 25/25. It separates administrative root loading from
active decision time and suppresses expired request dispatch, without changing
the training algorithm or any wire/artifact format. It does not provide off-tree
strategy, native-work cancellation, complete six-seat policy or quality evidence.
The original full sanitizer command still has one resident target running and
an original failed Node entry; the successful fixed Node rerun is recorded
separately, not presented as a new full-suite pass. T1/T2 gates remain open.

On 2026-10-14 a four-agent independent audit found RFCs 0019/0020/0021
conformant with no blocker or major findings, and two minor hardening items
were added (symmetric preflop payment caps; an optional-ledger positive
replay). Final-source gates passed: release format/build, full release CTest
91/91 in 363.84 s, `npm run check` 156/156, direct `npm run proto:check`
25/25, `node bin/replay.mjs` exit 0 with zero usable historical snapshots,
and `benchmark-multistreet` with all three quality gates passing. The true
Debug ASan/UBSan build compiled and linked the final source. The full ASan
CTest suite was stopped at the user's request after roughly 85 minutes with
`heads_up_solver_sampled` and `resident_policy` still running; no sanitizer
error was reported by any completed test. The full-suite sanitizer gate
remains open; T1a/T1b stay In Progress and the RFCs stay Implementing.

On 2026-10-14 the T3 six-seat feasibility scan completed all 15 fixtures
(2..6 seats x 10/25/100 BB, turn-rooted, declared-only 1/2-pot bet + 1/2-pot
raise, 30 s solve budget). The current coarse abstraction is infeasible for
5+ seats at every stack (tree exceeds the 1 GiB builder cap), 4-seat solves
exceed the trainer byte cap at 10 BB and the builder cap at 25+ BB, and
3-seat solves are trainer-cap constrained at 100 BB. Only 2-seat solves
complete at all stacks. The [T3 report](t3-six-seat-feasibility.md) records
the full matrix, growth analysis and the recommended coarser multiway
schedule / depth-limited resolving direction.

On 2026-10-15 depth-limited resolving (RFC 0024) extended feasibility to
5 seats at 10/25 BB and 4 seats at 25 BB. On 2026-10-16 a per-street
raise-count cap (`max_raises_per_street=1`) combined with depth-limited
resolving made **all 15 fixtures feasible**, including 6-seat at every
stack depth. The largest tree (6-seat, any stack) is only 9,361 nodes /
9.1 MiB, down from >1 GiB. Tree size is independent of stack depth with
the cap. All fixtures achieve 100% action-node coverage. T3 stays In
Progress: this is resource/deadline evidence, not the independent quality
gate the qualification proposal requires.

## Operational Boundaries

This development work does not authorize tokens, hidden information, external
compute spending, real-money play, live joining/action submission, destructive
artifact replacement, commits, pushes, or deployment. Bounded synthetic/native
checks may proceed; large training/publication and live acceptance retain their
separate resource and operating gates. Preserve existing immutable generations.

## Whole-Feature Completion Gate

The feature stays In Progress unless all of the following are verified:

- Every required six-position and street scenario has a real qualified strategy
  path, not a mock response, heuristic rename, or routine operational fallback.
- Public-state reconstruction preserves contributions, refunds, eligibility,
  statuses, action rights, actual stacks, and reliable history across streets.
- Normal unequal-stack/all-in/side-pot and legal nonstandard-bet scenarios are
  included; no hidden-information shortcut or incompatible root substitution.
- Independent quality/abstraction evidence matches the actual policy, ranges,
  rules, game identity, and full six-max support claim.
- Measured end-to-end time/memory and error/restart behavior satisfy the real
  platform budget; deployment/loading/version/rollback checks pass.
- All native, sanitizer, Node, protocol, and complete six-seat acceptance gates
  pass, and an independent reviewer approves the full evidence.

No partial feature-complete percentage is inferred from finished task counts.
