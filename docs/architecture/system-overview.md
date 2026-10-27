# System Architecture

Status: Current

## Purpose

BigShark is a platform-neutral no-limit Texas Hold'em decision engine. The C++
decision process consumes a compact normalized context over a framed Protobuf
protocol served by a local engine host. A game-platform integration used to
live in this repository and was retired by
[RFC 0040](../rfcs/0040-retire-river-club-agent-api.md); only the engine side
exists today.

The repository separates two concerns:

1. Engine process integration: process lifecycle, request framing, timeouts,
   and response correlation (the generic Node client and the C++ engine host).
2. Poker decisions: hand evaluation, range construction, equity estimation,
   policy, resolving, and river equilibrium solving.

The framed Protobuf engine protocol (RFCs 0002 and 0034) is the only engine
protocol. It carries a G/F contribution ledger, phase-tagged pot layers,
observation provenance, and capability tuples. The engine-host accepts
`protocol_version` field 1 == 3 on every frame. The handler replays from the
true hand-start root (no mid-street rebase) and validates the declared G/F
ledger and pot state against the native replay. Capability tuples (`hu-r1`)
are advertised; `sixmax-r1` is gated on T2–T5 completion. The resident layer
serves blueprint decisions (flop-rooted, turn-rooted, river-rooted, and
preflop). The resident mapper rotates actor indices so the button is always
at index 0, matching blueprint root identity for any physical button
position. Preflop decisions build a hand-start root GameState; postflop
decisions build a flop-rooted GameState. Terminal-only resolving runs on an
AUTOMATIC blueprint miss, with a labeled operational fallback
(check/call/fold) when the resolver also misses.

## Runtime Components

```mermaid
flowchart LR
  Consumer[Embedding application] --> Client[clients/node process client]
  Human[Human player] --> Practice[bigshark-practice]
  Client --> Host[apps/engine-host]
  Practice --> Host
  Host --> Proto[bigshark_engine_protocol]
  Proto --> Service[bigshark_service]
  Service --> Policy[bigshark_policy]
  Policy --> Resident[bigshark_resident]
  Resident --> Artifacts[bigshark_artifacts]
  Policy --> Solver[bigshark_solver]
  Policy --> Poker[bigshark_poker]
  Solver --> Tree[bigshark_tree]
  Solver --> Abstraction[bigshark_abstraction]
  Solver --> Poker
  Tree --> Abstraction
  Tree --> Poker
  Abstraction --> Poker
```

## Component Boundaries

| Component | Owns | Must not own |
| --- | --- | --- |
| `engine/` | Poker, solver, abstraction, policy, service, protocol library, resident lookup, artifacts, resolver, and solver-owned benchmarks | Authentication, HTTP, room discovery, platform tokens, or any network I/O |
| `bigshark_abstraction` (RFC 0008 stage 3) | Card bucketing, declared ordered action menus, abstraction identity (`AbstractionId`), and the typed mismatch refusal; depends only on the rules layer | Knowledge of solvers, storage, or IO, and any modification of the rules. The solver's live menu is a thin adapter over the lifted builder (bit-for-bit identical decisions, replay 156/0/0); card buckets and `AbstractionId` are consumed by the flop class library and class-based `resolve_root` (W4a/W4c) |
| `bigshark_tree` (RFC 0008 stage 4 L3) | The seat-count-agnostic abstract public betting tree (`AbstractTree`): abstracted action nodes, probability-free public-card chance nodes, and fold/showdown terminal ledgers built from an L1 `GameDef` and an L2 menu; links only `bigshark_poker` and `bigshark_abstraction` | Regrets, ranges, hole cards, policy, probabilities on chance edges, per-deal conditioning, and any solver, transitional rules adapter (heads-up or multiway), storage/transport, or private-holding evaluator (enforced unconditionally at build time by allowlisting the compiler-resolved include closure — which macro-paste, line-splice, digraph, no-space, and `#import` spellings cannot evade — and at link time by the link-negative test). A build that crosses a deterministic node/depth/byte bound throws the typed `tree_resource_exhausted`, never a truncated tree; the byte bound covers requested retained capacity |
| `bigshark_solver::solve` (RFC 0008 stage 4 L4) | The single unified solver seam: routes a two-seat identity tree to heads-up CFR with the numeric core unchanged (bit-for-bit under both drivers and every runout variant), and explicitly refuses unsupported shapes with the typed `unsupported_tree_shape` | Silent approximation of a model it does not solve, conflating a shape refusal with a malformed request (the exception is not `invalid_argument`), and a self-assigned guarantee level on the result (the source-to-guarantee map is L6). The river LP/DCFR and experimental multistreet solvers are reachable but refuse-only this stage |
| `apps/engine-host` | C++ process composition, framed Protobuf host (`--serve-proto`, the default serving mode) | Platform mapping or strategy rules |
| `clients/node` | Generic engine process lifecycle, framed Protobuf request/response, timeouts, and FIFO correlation | Platform-specific fields or poker strategy |
| `bin/` | Documentation/RFC checker launchers and the published engine executable | Reusable implementation |
| `.codex/skills/` | Human-readable exploitative style guidance | Transport or platform API details |
| `strategy/` | Poker policy baseline and observed table notes | Protocol implementation |

## Decision Flow

1. The embedding application assembles an authoritative decision context:
   hole cards, board, stacks, blinds, action history, and the legal action
   set.
2. It builds a `DecisionRequest` with G/F contribution ledger, phase-tagged
   pot layers, TransitionEvent path, observation provenance, and rule
   profile.
3. `clients/node/proto-engine-process-client.ts` sends the framed request
   to the persistent C++ host.
4. The protocol handler validates the G/F ledger and phase-tagged pot layers,
   replays the TransitionEvent path from the true hand-start root, and routes
   by solver_mode to the service, blueprint, resolving, or heuristic paths.
   On the resident path, the policy layer first looks up a blueprint in
   `bigshark_resident`; on an AUTOMATIC blueprint miss the bounded
   terminal-only resolver runs; if the node is turn/river the blueprint-
   derived subgame resolver runs next; if the node is on the flop the
   flop-rooted depth-limited resolver (RFC 0025) runs with uniform priors;
   if all resolver paths miss, a labeled operational fallback
   (check/call/fold) is served. Off-tree action lines are tolerated: the
   blueprint replay stops at the first off-tree action and uniform priors
   are used for post-off-tree seats.
5. The host returns the selected action, amount, guarantee label, and
   provenance in one framed response; legality against the embedding
   application's table state remains the caller's responsibility.

## Safety Invariants

- The embedding context is authoritative for cards, turn ownership, legal
  actions, and stack movement; the engine never fetches game state.
- No component may infer hidden cards or make network calls.
- The engine may choose only from the legal action set supplied in its input.
- A bet or raise amount is a target total for the current street.
- A stale state causes a fresh decision. An old action is never replayed.
- When every resolver path misses, the engine itself serves the labeled
  operational fallback (check/call/fold); it never invents an aggressive
  action.
- No credentials exist in the repository; local journals stay local and
  gitignored.

## Remaining Coupling

- The framed Protobuf protocol (RFCs 0002 and 0034) is the only engine
  protocol. Its IDL and generated-code checks exist under `proto/`.
- Supplied action history is replayed as a TransitionEvent path; past-street
  rounded display text remains text-only input.
- The offline practice simulator spawns the engine host as a local child
  process.
- No game-platform adapter currently validates the embedding boundary; a
  future integration would be a new external module with its own RFC.

See [RFC 0001](../rfcs/0001-engineering-architecture.md) for the repository
and module architecture.

## Deployment Constraints

- The engine is built as C++23.
- The current exact LP path optionally uses HiGHS.
- BigShark has no direct CoreFoundation dependency; vendored dependencies
  select their own platform libraries.
- The `linux-release` preset is verified with GCC 12 on Debian bookworm,
  without HiGHS, through `tools/ci/Dockerfile.linux`.
- `release` is the only preset that publishes `bin/bigshark-engine`.
- `debug` and `asan` keep their binaries inside their preset build directories.

## Capability Boundary

The engine is a hybrid poker decision engine:

- Preflop: a trained 6-max preflop profile (flop-terminal, full-range
  100 BB) is published as an artifact and served through the resident
  layer. The resident mapper rotates actor indices so the button is at
  index 0, matching the blueprint root for any physical button position.
  Other table sizes and stack depths fall back to the charts or
  operational fallback.
- Flop and turn: deterministic Monte Carlo equity plus heuristics;
  resident blueprint libraries serve flop-rooted decisions at the
  `approximate` guarantee level when a configured root matches. Flop
  libraries may be a packed multi-root `BSFLIB03` file written directly by
  training (RFC 0027, RFC 0035) or a directory of schema-v3 SQLite class
  artifacts; the packed form is the canonical artifact, its root directory
  names every trained ledger, and a launch may use only one binary library.
- River: exact sequence-form LP when available and within budget, otherwise
  bounded DCFR; resident blueprints serve river-rooted decisions at the
  `certified_bound` guarantee level.
- Terminal-only resolving: on an AUTOMATIC blueprint miss, the
  bounded terminal-only resolver runs; if it also misses, the turn/river
  subgame resolver (blueprint-derived or uniform-prior) runs; if the node
  is on the flop, the flop-rooted depth-limited resolver (RFC 0025) runs
  with uniform priors and the equity frontier evaluator; if all resolver
  paths miss, a labeled operational fallback (check/call/fold) is served.
- Engine-served practice tier: the practice simulator forwards bot
  decisions to the engine via framed Protobuf.
- Experimental multi-street CFR: offline and test-only.

The current implementation must not be represented as a complete equilibrium
solver for every street, stack depth, table size, betting structure, or poker
variant. The preflop profile is a declared-profile coverage result, not a
full-game GTO solution.
