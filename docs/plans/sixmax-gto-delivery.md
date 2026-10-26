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
| ~~Engine practice restricted to heads-up~~ | `engine/benchmarks/practice_simulator.cpp` count gate and single bot injection | **Resolved 2026-10-17:** T7 complete — 2..10-seat CLI with EngineProcess sharing one host across all bot seats; 6-seat engine-served hands verified |

Existing native mapper/validator/minor2/resolving tests passed 4/4; the River
differential suite passed 17/17, including a declared side-pot fallback. Native
practice tests passed for six-seat easy/medium policies, not six-seat GTO.
The engine practice CLI now accepts 2..10 seats with engine-served bots (T7).

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
| T0 | Protocol, solver, product documentation | Whole-feature rules/support matrix, public-data audit, independently reviewed quality/latency/coverage acceptance; no weakened labels | None | In Progress: gaps audited; [threshold proposal](sixmax-gto-thresholds.md) approved by second independent review 2026-10-17 (epsilon_seat=0.05 BB/hand, epsilon_sum=0.15 BB/hand, delta_regression=0.01 BB/hand, family-wise 95% CI, error budget 40% of epsilon_seat); 12 editorial fixes applied; thresholds frozen pending campaign execution |
| T1a | v1 protocol and native tests | Exact minor-2 N-seat preflop replay; frozen minor-0/1 compatibility | RFC 0019 approval | Complete: RFC 0019 implemented, N-seat preflop replay and minor-0/1/2 compatibility tests pass in release |
| T1b | Protocol, River adapter; resident/resolver identity prerequisites | Complete public preflop-to-postflop ledger, folded/dealt status and trusted actor history | T0, T1a, RFCs 0020/0021/0022; follow-up contract approval | In Progress: v2 client/mapper/runner implemented (protocol v3, capabilities handshake, G/F ledger, pot layers, transition path, observation provenance); honest observation provenance via intake ledger implemented (118/118 Node tests pass); supplied-history replay and root identity passed release; RFC 0022 v2 wire/behavioral/vector design Accepted 2026-10-16; protobuf+ASan RepeatedField copy fix committed (Swap instead of copy-assign, closes conformance and node ASan failures); ASan suite re-running with fix |
| T2 | Poker utility, protocol, frontier, artifacts | Unequal-stack and side-pot-safe state/utility/identity; no imaginary `pot / n` contributions | T0, T1b; RFC 0006 follow-up approval | In Progress: side-pot-safe per-layer frontier settlement implemented (RFC 0010 update, independently approved); equal-stack guard removed; utility identifier replaced; 18 native ledger baselines passed focused release/ASan; RFC 0022 v2 wire design Accepted 2026-10-16; six-seat preflop profile trained (1.0 GB, 100K iterations, 100 BB, raise-cap=1); six-seat flop library trained (8 classes, 1.9 GB, 1000 iter/class) |
| T3 | Solver, abstraction, native benchmarks | Independent six-seat quality and normal-stack resource/deadline measurements before scaling | T0 | In Progress: [feasibility + quality measured](t3-six-seat-feasibility.md); raise-cap + depth-limited resolving makes all 15 fixtures feasible; pooled-BR NashConv optimized (fixed-size BRView, multi-traverser walker, OpenMP, direct showdown); 100K-iteration quality campaign: 0.02-0.15 BB exploitability across 2-6 seats, 58-98% reduction vs uniform, 5-6 seat games near-equilibrium at ~0.02 BB; full 15-fixture scan <6 min at 1K iter, ~8 min at 100K iter |
| T4 | Solver, resident, artifacts | Six-position preflop policies with matching multiplayer continuation values and immutable provenance | T1a, T2, T3 | In Progress: N-seat continuation-range export (2..10 seats) with recursive joint-deal enumeration and card-mask filtering; --raise-cap flag wired in both offline builders and recorded in manifests; 2/3/6-seat range tests pass; 6-seat pipeline verified; full 100K-iteration six-seat profile trained (6,424-node tree, 519,168 info sets, 4,073,472 stored rows, 1.0 GB artifact, 430s training, 584s pipeline, 6.4 GB peak RSS, SHA-256 e3d77d55); six-seat flop library trained (8 classes, 1.9 GB). RFC 0037 (Implemented 2026-10-26) completed the full 2..6 dealt-count 100 BB preflop set: added 3-seat 100 BB cap 1 (Complete, 11,154 infosets), 4-seat 100 BB cap 1 (44,616 infosets, 69 s, 67 MB), 5-seat 100 BB cap 1 (157,170 infosets, 174 s, 260 MB); the 2-seat 100 BB profile stays cap 0, and the legacy 3-seat 10 BB cap 0 validation artifact was retired 2026-10-26 (the policy is heads-up cap 0, every 3+ seat shape cap 1 at both layers); runner auto-discovery pins every profile by manifest sha256. Shipped artifacts are standalone preflop identities; independent full-range quality certification is still the campaign |
| T5 | Range propagation, resolver, resident | Action-conditioned 2..6-live-player flop/turn/river strategy coverage preserving dealt-seat identity | T1b, T2, T3, T4 | In Progress: six-way flop libraries COMPLETE. Premium generation: 1,745 trained classes + 10 geometrically undealable JQKA-high classes (=1,755; 21,972/22,100 concrete flops, 99.42%, 334 GB, manifest 2026-10-18). Full-range generation: all 1,755 classes trainable, 22,100/22,100 flops (100%), packed to `artifacts/flop-library-6way-fullrange/library.bin` (179.71 MiB), and exhaustively verified lossless 2026-10-19 (`verify_pack: 1755 classes, 1931640886 stored rows, 12383280 holding/node checks, 0 mismatches`). RFC 0026 BSFLIB02 format Implemented; RFC 0027 direct train-and-pack Implemented 2026-10-19 (a full 1,755-class direct rebuild reproduced the verified `.bin` byte for byte, sha256 `b67e3cb3…`, with no per-class SQLite, 10.1 GB peak, and 3.6 s assembly). The premium generation and the seven obsolete-generation SQLite libraries (372 GiB) were deleted 2026-10-19; the full-range packed library is the only bundled flop library. Turn/river six-way strategy remains open. 2026-10-20: four follow-up RFCs are drafted and under independent review, none yet approved. RFC 0029 makes the flop-terminal library measurable (every stage-6 walker currently blind to `Phase::Frontier`); RFC 0030 pins the raise cap in the action-abstraction identity and adds a tree-shape witness to the sealed checkpoint, which closes a measured silent-resume defect independent of any cap change; RFC 0031 defers finer postflop bucketing until abstraction loss is measured, because the shipped library already over-segments (1.82 distinct vectors/node, 57.6% uniform fallback); RFC 0032 fixes turn/river board addressing and adopts the RFC 0011 frontier-leaf direction instead of building full turn/river class libraries (~1.05 TB projected for river). |
| T6 | Resolver, service, protocol, Node client, River runner | Off-tree handling and receipt-to-action budget including construction/lookup/solve; accurate source and fallback reports | T1b, T2, T5 | In Progress: RFC 0023 startup/deadline repair; solve-budget alignment fixed; receipt-to-action latency measurement implemented (envelope handler stamps solve_time_us on all successful decision paths); AUTOMATIC mode now tries resolver then operational fallback on blueprint miss (was falling through to heuristic); RFC 0025 off-tree decision handling Implemented (flop-rooted depth-limited resolving + off-tree action line tolerance; resolve_flop and off-tree tolerance implemented, native tests passing, Engine Build Contract passing) |
| T7 | Practice leaf and engine process client | All five bot seats served by the same host through complete six-seat, rotating-position hands | T1a, T1b, T2; additive CLI approval | Complete: interactive CLI (`bigshark-practice`) with 2..10-seat support; EngineProcess spawns `bigshark-engine --serve-proto` and shares one host across all bot seats; EngineServedPolicy converts GameState/HandLog to v2 DecisionRequest (G/F ledger, pot layers, transition path with board cards and call amounts, forced contributions, provenance); first-decision MID_HAND_ATTACH fallback; 96/100 engine-served decisions in 5-hand 6-seat test; Engine Build Contract 75/75 passing |
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

### 2026-10-25 status entry

- **Authorization gates checked**: no River Club token exists
  (`~/.config/river-club-agent/config.json` absent, `RIVER_CLUB_TOKEN`
  unset) and the user has not authorized a live data-collection session;
  the bankroll gate therefore cannot be evaluated. Tasks 1 and 4 of the
  25-item work list are open user gates. Every live-corpus task (RFC 0035
  stage 4-5, T-09/T-10/T-11, coverage measurement) stays blocked; the
  single `sessions/2026-10-25.jsonl` row is the token-missing failure.
- **RFC 0037 (shape-keyed action-abstraction selection policy) Implemented
  2026-10-26**: the frozen cap table equals the provisional table (no
  cell promoted); the 72-row pre-registered matrix and the committed
  checker (`tools/artifact-audit/rfc0037-ablation-check.py`) mechanically
  returned KEEP — the one nominally promotable cell (3-seat standard
  1 -> 2) has no non-proxy cap-0 reference in the postflop streets(1)
  geometry (production-menu cap-0 trees exceed 1 GiB at 3+ seats), so it
  stays cap 1 despite a favorable proxy. The policy module, the
  `resolve_flop` activation table (only change: two-live shallow flop
  cap 1 -> 0 after a passing Stage-0a spike), the mid-street-fold
  invariant, the tri-state policy audit (8/8 artifacts pass), the
  cross-cap ablation harness, and the complete 2..6 dealt-count preflop
  profile set with runner auto-discovery all landed. Gates: ctest 79/79,
  npm 133/133, benchmark-multistreet quality PASS, ASan clean on touched
  surfaces, proto:check and replay exit 0. Independent full-range quality
  certification remains the campaign; the cap-selection policy itself is
  settled.
- **Task 10 (RFC 0036 M2) MEASURED 2026-10-26 — FAIL, Phase C required**:
  the `--rfc0036-m2` harness (full cap-1 turn->river reference vs the
  cap-1 streets(1) frontier candidate; same cap, so it isolates the
  river leaf-model error) gives a 100K, three-seed model delta of 0.402
  BB (CI [0.18, 0.62]) at two seats and 6.161 BB (CI [4.79, 7.53]) at
  three seats — both fail the summed 0.03 BB gate by 1-2 orders of
  magnitude; the uniform-fill arm is ~28/42 BB. Even heads-up deep
  fails. Any turn/river on-demand routing (task 11) now requires a new
  Phase C RFC supplying a real river continuation-value model (nested
  depth-limited resolve with cached CV, or a neural estimator), not more
  iterations/budget. Rows: `docs/plans/ablation/rfc0036-m2.csv`; design
  and verdict in `docs/plans/0036-implementation.md`. Supersedes the
  earlier design-only note.

### 2026-10-24 status entry

- **RFC 0030 (raise cap + checkpoint witness) Implemented (2026-10-25)**:
  identity digest carries `|max-raises=N|`, the flop builder refuses
  witness/digest mismatches before rows are adopted, mid-street raise
  counter plumbed, and the checkpoint witness lineage continues in
  BSWARM03 under RFC 0035. Stage 3 is complete: all service artifacts and
  packed libraries were rebuilt pack-direct with explicit tokens (identity
  family), pass the AC9 local audit (5 pass / 0 fail) and the bundled
  reachability test, and carry matching SHA-256 sidecars; tokenless
  predecessors are preserved under `~/.bigshark-backups/`. The RFC 0029
  validation artifact awaits the T-09 replay program.
- **RFC 0031 (card-abstraction error) Implemented (2026-10-25)**: AC1
  baseline statistics reproduce exactly on the pinned `b67e3cb3` artifact
  (1,642,680 nodes, 57.60% untrained, 2.9319, 1.5237); Stage 1 identity
  pinning and Stage 2 `TiersDrawV1` landed earlier; AC5 cost is below the
  150 ns/call rejection gate at every street (129-135 ns flop/turn,
  79-95 ns river); the RFC 0029 candidate adapter and the stage-6 driver
  gained scheme selection (`--card-kind tiers|tiers-draw`). The Stage-3
  two-scheme ablation on the declared n=2 exact fixture at 1,000 and
  10,000 iterations (raw CSV in `docs/plans/ablation/`) gives negative
  improvement with overlapping intervals at both counts, so the pre-declared
  rule resolves to **keep tiers**; no artifact adopts TiersDrawV1.
- **RFC 0030 stage 3 executing (2026-10-25)**: all service artifacts are
  rebuilt with explicit cap tokens under the correct **identity**
  (`rfc0007-pot-fractions`) family — the earlier coarse-labeled attempt was
  discarded. `ac1-16w`, the 2p/3p/6p preflop profiles, and the six-way
  full-range library rebuilds run pack-direct; tokenless predecessors are
  preserved under `~/.bigshark-backups/`. The AC9 local audit
  (`tools/artifact-audit/ac9-token-audit.py`) gates the result. The
  RFC 0029 validation artifact awaits the T-09 replay program.
- **RFC 0033 (flop library root reachability) Implemented**: the shipped
  six-way library now answers the real limped 200-chip/2-contribution root
  (188,411,904 bytes, sha256 b5a480af…, reachable per RFC 0035's measurement
  gate; the superseded b67e3cb3 backup stays outside the repository).
- **RFC 0034 (unversioned engine protocol) Implemented (2026-10-25)**: single
  `bigshark.engine` package, `engine.binpb` baseline, descriptor diff limited
  to package/file/FQN, the two-TU fuzz harness restored with a 100,000-input
  clean Clang-14 campaign and a linking libFuzzer configuration, six-exhaust
  token inventory residual empty, and 124/124 compiled Node tests including
  both real-binary w1 cases non-skipped against the republished engine. The
  full ASan/UBSan suite is green on the final source (77/77; the
  `stage6_measured_policy` watchdog was raised to 5,400 s under sanitizers
  only after a 900 s wall-timeout on a still-advancing run, and the isolated
  rerun passed in 2,905 s), and the final release Engine Build Contract is
  green (ctest 79/79, benchmark quality gates PASS, npm check 124/124,
  proto:check, replay).
- **RFC 0035 (multi-root flop library) Implemented for stages 1-3
  (2026-10-25)**: the BSFLIB03 root directory, full RFC 0030 action
  identity in the header, per-(root,class) builder/resume, BSWARM03
  checkpoints, exact-ledger resident matching, the ClassNotTrainedForRoot
  miss through all six sites, and per-root host reporting are implemented
  and tested (AC1-AC7, release ctest 79/79, ASan codec subset 10/10).
  Stages 4-5 — the live exact-ledger histogram over `sessions/` and the
  first real multi-root training build — remain the explicit gate; no
  second root is trained until that threshold and the data exist.
- **RFC 0036 (six-way turn/river on-demand) Accepted (design only)**: bounded
  cap-1 river (seats 5/6) and street-start turn (seats 4-6) resolves with a
  three-stage serve/decline gate; host routing stays unrouted until the
  stage-0 node/iteration/visits calibration evidence lands. This is the T5
  turn/river direction replacing class-library expansion. The stage-0
  behavior-preserving build/train split of `solve_rooted_subgame` landed
  2026-10-24, pinned by the release resolver/subgame/conformance suites;
  the calibration measurements themselves remain gated on compute
  authorization. **Update 2026-10-25: stage 0 measured, result is no-go
  for seats 5/6.** Production identity cap-1 shapes are small (six-seat
  river 1,897 nodes, six-seat turn streets(1) 46,585 nodes; all clear the
  1 GiB builder cap) and the 1,600 ms envelope affords 1,510 (river) / 970
  (turn) iterations at six seats, but in-model pooled NashConv there is
  9.3 BB (six-seat river), 15.7 BB (six-seat turn), and 5.7-7.1 BB at five
  seats — 38x-105x above the 0.15 BB sum gate. No minimum-iteration
  constant both fits the envelope and passes M1, so both bounded paths
  default to decline; host routing stays off and the labeled fallback
  remains. The seam is retained for a future budget change. M2/Phase C is
  not reached. Evidence in `docs/plans/0036-implementation.md` and the
  `--rfc0036-m1*` harnesses.
- **T-08 coverage instrumentation landed**: `platforms/river-club/src/
  coverage.ts` plus `bin/coverage.mjs` tally the `.runtime/results.log`
  seam into resident/resolved/fallback buckets with per-reason and
  per-street distributions; analyzer unit tests pass. Real hit-rate numbers
  await a live session corpus.
- **T-11 provider gate (static half) audited 2026-10-24**:
  `docs/integrations/provider-public-field-audit.md` maps every
  reconstruction input against the authoritative v2.1 contract. Settled:
  revision spine, legality, current state, and the old-events-omitted
  guarantee. Undocumented/unsettled: `room.pots`, `winners` shape, the
  event-kind catalog, rake/ante/refund/runout rules, and window retention.
  The host still emits only `hu-r1` with `side_pots=false`; the v2 side-pot
  end-to-end remains blocked on the five enumerated authoritative-data
  items plus real captured vectors (live-play authorization). T-09 (RFC
  0029 stage 4) and T-10 (RFC 0032 stage 4) remain documented as blocked
  on live-play authorization; `sessions/` is empty.
- **T-04 verification debt closed (2026-10-25)**: the full ASan/UBSan
  preset is green at 77/77 on the final source, including
  `heads_up_solver_sampled` (3,696 s) and `resident` (12,044 s, no
  watchdog). One wall-timeout on `stage6_measured_policy` (900 s) was
  resolved with a sanitizer-only watchdog raise to 5,400 s following the
  `postflop_templates` precedent; the isolated rerun passed in 2,905 s.
  The final release Engine Build Contract passed end to end (ctest 79/79,
  benchmark-multistreet quality gates PASS, npm check 124/124,
  proto:check, replay, docs, RFC checks).

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

On 2026-10-17 a read-only audit of the qualification measurement code
confirmed `exact_oracle_utility` pools opponent worlds correctly at the
information-set level (one action per own-holding group, counterfactual reach
excluding the traverser, folded cards retained as runout blockers) and
`br_estimator.cpp` uses disjoint learn/confirm seeds, freezes before confirm,
CRN-pairs runouts, and reports per-replica summed gains. Three remediation
items landed the same day: (1) the defective per-deal `NashConvWalker` in
`subgame_nash_conv.cpp` (dead since the pooled walk landed 2026-10-16) was
deleted; (2) the folded-seat `{-1,-1}` runout sentinel is now documented as an
explicit analytic marginalization under the uniform unconditioned fold-range
model with an exchangeability proof (a conditioned fold-range still requires
explicit range enumeration and is not used by these fixtures); (3) the
qualification proposal's exact information-restriction negative control was
added to the native oracle test (`test_information_restriction_negative_control`,
fold/call payoffs -2/-4 vs -2/+4 = the proposal's -1/-2 vs -1/+1 at 2x chip
scale: pooled BR 0, world-wise 1, gains 2 vs 3), pinned under both seat
orientations. The six-dealt-player and conditioned-fold-range extensions of
the control remain open.

Also on 2026-10-17 the flop-library training run surfaced two independent
resume/dealing defects and both were fixed: (1) the scalable rejection joint
dealer burned its 100k-attempt budget on ~140 high-card flop classes where six
seats must draw 12 distinct cards from the 16-card premium range after the
board blocks three of them (acceptance ~2e-5). A new `JointDealDealer`
probes acceptance once per fixed (ranges, board) game and uses plain rejection
on high-acceptance games but enumerates the exact support and cumulative-picks
on narrow games — the exact table is correspondingly small precisely where
rejection failed, with a 2M-deal cap and enlarged rejection fallback. The
nseat trainer now constructs one dealer per training run, shared read-only
across worker threads; both regimes draw from the identical
product-conditional distribution, verified distributionally by
`test_adaptive_dealer_regimes_and_distribution`. (2) Worker SIGKILLs left
torn `-checkpoint.db` files (bad SQLite header / stale rollback journal) that
made every later `--resume` abort before spawning workers; both the builder
parent and the in-worker preflight now discard a checkpoint that fails to
load/validate (plus its journal/wal/shm) and retrain the class, while a valid
checkpoint with an incompatible algorithm revision remains a hard error.

Three further flop-library fixes landed the same evening: (3) eight canonical
classes are geometrically undealable for six seats against the J+ premium
range (the board leaves no six pairwise vertex-disjoint combos, e.g.
KsQsJs-type boards). An exact bounded depth-first edge packing
(`class_is_dealable`) decides this at worker startup; undealable classes are
skipped and recorded in the manifest's `undealable_boards`, and
`merge_fragments` enforces `built + undealable == class_count`. A first
resume run still crashed on the undealable class because the builder binary
predated the skip — building only unit-test targets does not relink the
builder, so the builder must be rebuilt before every `--resume` after a
library/builder source change. (4) Resuming over already-published classes
made merge re-hash every multi-hundred-MB artifact to relearn immutable
digests (hours of sequential I/O). `merge_fragments` and the serial builder
now fill skipped-class records from the existing `manifest.json`
(artifact-immutable, canonical board cross-checked) and probe only classes
with no prior record; `test_merge_reuses_prior_manifest` pins the behavior.
(5) The resume/merge path's stale Edmonds-blossom comments and an obsolete
forward-declaration shim were removed; the matcher is the exact DFS above.

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
