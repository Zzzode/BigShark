# RFC 0009 Implementation Plan

Status: Current

Execution state: RFC 0009 is Implementing (independent approval record in the
RFC's Decision section, 2026-09-29). This plan maps its stages W1-W5 to tasks,
evidence, and gates. Each stage lands as one reviewed commit set with
implementer and reviewer as separate agents (repository convention).

## Stage W1 — Wiring, no default change

Status: implemented and committed (c88dc17), independently reviewed and Approved
(2026-09-29, two rounds; see the review record below).

Delivered (RFC 0009 D1):

- `EngineConfig.residentRoots` and `EngineConfig.solveTimeBudgetMs`
  (`platforms/river-club/src/types.ts`); `ExecutableDecision.artifactSha256`.
- `parseResidentRoots` + `protoEngineLaunchArgs` (`src/engine.ts`): the framed
  child is launched with the host's repeatable
  `--resident-root <path>=<sha256>` argument per configured root; with no
  roots the launch line is byte-identical to the historical
  `['--serve-proto']`. The implicit client is keyed by its launch line and
  negotiates minor 2 only when roots are configured.
- Minor-2 provenance decode: `artifactSha256` (field 9) is attached with the
  guarantee level (field 11) on a negotiated minor-2 strategy response and is
  structurally absent everywhere else (`src/v1-mapper.ts`).
- Budget propagation: `resolveSolveTimeBudgetMs` sanitizes the caller's real
  remaining-time budget into the wire's declared 1..120000 range; the runner
  passes `engineBudgetMs(timeLeftMs, safetyMs)` instead of the pinned 2000.
- Runner wiring (`apps/river-club-agent/main.ts`): `residentRoots` in
  `.runtime/config.json` validated by `parseConfigResidentRoots`
  (`runner-state.ts`); a malformed value fails startup with a loud
  `runner-config-invalid` note and exit 2 (it can never discard sibling
  settings or silently run without roots). With at least one root the runner
  opts into v1; `protoBlueprint` is deliberately NOT set, so AUTOMATIC keeps
  the W1 contract (resident blueprint first, then the engine's own labeled
  fallback). Journal: `guaranteeLevel` and `artifactSha256` ride in the
  results.log decision object, the `acted` stderr note, and pending.json.
- Documentation: `docs/integrations/river-club.md` Local State table.

Gate evidence (this machine, macOS/arm64, working tree on `feat/rfc-0004-0006`):

| Check | Result |
| --- | --- |
| `npm run check` (typecheck, build, source policy, proto, docs, RFC, Node suites) | pass; Node suites 117/117, 0 skipped |
| New tests: `platforms/river-club/tests/w1-resident-wiring.test.ts`, `apps/river-club-agent/runner-state.test.ts` additions | launch-line identity + root append order; root-spec accept/reject; budget sanitizer; fake-host digest/level decode (minor-2 only); REAL BINARY: `--resident-root`-launched host serves the published fixture flop, decision carries the artifact digest, a served (non-fallback) level, and a blueprint reason |
| `node bin/replay.mjs` | `Decisions: 156 / Illegal: 0 / JS fallbacks: 0` (frozen) |
| Reviewer probe (independent) | implicit (non-override) client path serves `bet 40`, `reason cpp:blueprint`, digest `06a6...16aa`; mismatched pin → fail-closed digest-mismatch and heuristic-with-label answers; no-root behavior byte-identical |

Review round 1 (independent reviewer, 2026-09-29): Changes Requested — one
blocking finding (the root-validation throw was swallowed by
`readRunnerConfig`'s fallback catch, silently discarding sibling settings) and
eight non-blocking findings. Dispositions: the blocking finding is fixed with
the startup preflight + outside-the-catch validation + engine-side rethrow;
non-blocking items fixed in place except the D1 no-roots journal wording,
explicitly deferred to W3 when v1 becomes the default without roots.
Review round 2 (same reviewer, 2026-09-29): **Approved** — the blocking finding
verified resolved against the code in both halves (runner preflight path and
engine-side rethrow), the implicit-client coverage gap closed by a new
real-binary test, and the full gate re-run green (118/118, replay 156/0/0).
Carried non-blocking items, all recorded for W3 or noted as cosmetic: the D1
no-roots wording (a W1 note now marks the deferral in the RFC itself); the
runner does not pre-validate the `BIGSHARK_ENGINE_RESIDENT_ROOTS` environment
value (engine-side failure at first decision, documented); `engineBudgetMs`
can return a non-integer that the budget sanitizer pins to 2000 (cosmetic).

## Stage map (remaining)

| Stage | Scope | Status |
| --- | --- | --- |
| W1 | Wiring: root config, v1 enablement, budget, provenance | Committed (c88dc17) |
| W2 | Seat-parameterized trainer behind `solve()`; artifact schema v2; resolver/resident generalization (per-seat certification); host service types | Committed (W2a f05a813; W2b 54a3cf6; W2c-i 883fc72; W2c-ii-a ec1fe6c; W2c-ii-b 4ed6630; W2c-ii-c b88c005) |
| W3 | Promotion + demotion in one change (runner defaults to v1 with roots; minor-2 AUTOMATIC loses the heuristic fallthrough; minor 1 frozen); rollback artifact recorded | Committed (9d41749); rollback artifact = commit b88c005 + pre-W3 release binary; independent review APPROVE |
| W4 | Coverage: suit canonicalization, preflop profile (RFC 0007), first flop library, terminal-only resolving at n >= 2, simulator engine-served tier (D7) | W4a suit canonicalization committed (745a379); W4c-i schema v3 card-abstraction declaration committed (6c88793); W4b preflop profile committed (97d58f6, 6405ab8, aa7f359, 34840b4, efecb17, 8cb16ae) + virtual flop deal (FlopDeal leaf eliminates chance subtree, 25 BB training enabled) + Preflop169 card abstraction (169 preflop buckets replacing CategoryTiersV1's 2, full-range 100 BB training enabled); W4c-ii class-based resolve_root committed (0f58799); W4c-iii first trained class library committed (ebeb32c); W4d terminal-only resolving committed (0844a18); W4e simulator engine-served tier committed (67f3118, d070079) |
| W5 | Documentation freshness (system overview, design doc corrections, protocol references) | Committed |

Open items carried for later stages (from review round 1, non-blocking):
reconcile D1's "journals that state" wording for v1-without-roots at W3;
`game_seats` insert path and schema-test parameterization sequencing at W2;
the W3/stage-7 scoping re-confirmation by that stage's reviewer.
