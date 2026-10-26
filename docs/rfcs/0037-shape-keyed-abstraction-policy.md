---
rfc: "0037"
subject: "Shape-Keyed Action-Abstraction Selection Policy"
status: "Implemented"
authors: "BigShark maintainers"
created: "2026-10-25"
updated: "2026-10-25"
owners: "engine abstraction, engine resolver, engine flop library, engine benchmarks"
supersedes: ""
superseded-by: ""
---

# RFC 0037: Shape-Keyed Action-Abstraction Selection Policy

> Revision 3 (2026-10-25): round 2 returned Changes Requested with one
> blocker — the activation table omitted the reachable 2-live shallow
> flop cell. Revision 3 makes the heads-up shallow cell the second
> declared, equally spike-gated behavior change (T3 already has direct
> feasibility evidence for two-seat shallow trees), adds it to Stage 0a
> and every affected AC/stop condition, and makes the fixed-cell
> feasibility surprise a machine-emitted checker FAIL (n2) plus a
> three-layer feasibility clarification for the three-seat standard cell
> (n1). Revision 2 corrected the BSFLIB03 identity facts, removed the
> `resolve_uniform` wiring, made the ablation fully pre-registered, added
> the heads-up flop spike, and added explicit profile wiring.

## Summary

Production strategies today run under whichever raise cap their producer
hardcoded at the time it was written: heads-up artifacts and the three-seat
shallow profile are uncapped (cap 0), six-seat artifacts are cap 1,
`resolve_flop` hardcodes cap 1 for every seat count, and the missing
three-seat 100 BB and four/five-seat profiles have no declared value at all.
This RFC replaces that historical distribution for its owned surfaces with
one explicit, reviewed, pre-registered **shape policy**: a single function
in `bs::abstraction` mapping game shape (street-start player count and stack
band) to the production raise cap, over the unchanged rfc0007 size schedule.
The table is frozen from a pre-registered cap ablation (caps 0/1/2 at
2/3/6 seats under the production identity menu, with 4/5 seats assigned by
fixed interpolation), after which `resolve_flop` and the offline builders
consult the policy, shipped artifacts are audited against it, and the
missing preflop profiles are trained under the frozen values. The RFC
changes no wire protocol and no artifact format; the cap token and digest
machinery it relies on is RFC 0030, already implemented.

**Owned surfaces.** The policy governs (a) the standalone preflop profile
artifacts (one SQLite artifact per shape, each carrying its own digest) and
(b) rootless flop resolving (`resolve_flop`) and future on-demand routes.
It does **not** claim mixed-cap support inside one BSFLIB03 flop library:
that format carries one file-level action identity, and under the frozen
table the only roots the existing six-way library can gain are standard-band
3-6-live roots, which are all cap 1. A 2-live (cap 0) or shallow (cap 0)
flop root needs a follow-up format/mount RFC, named below.

## Motivation

The goal is complete six-max cash support (`docs/plans/sixmax-gto-delivery.md`,
register item T4 and the T5 follow-ups), and three work items block on the
same missing decision:

1. **The same parameter has opposite feasibility at different shapes.**
   T3 measured that an uncapped identity tree is feasible for two seats at
   100 BB (5,326 preflop nodes), caps the three-seat identity tree at
   roughly 16 BB (two-million-node limit; `docs/plans/0009-implementation.md`,
   W4b record), and exceeds the 1 GiB builder cap at five/six seats at every
   depth (`docs/plans/t3-six-seat-feasibility.md`, original matrix). Cap 1
   makes tree size stack-independent and builds at six seats in 9,361 nodes
   / 9.1 MiB on the T3 coarse scan. A single global cap is therefore wrong
   in both directions: cap 0 is infeasible at 5/6 seats; cap 1 deletes the
   4-bet/5-bet structure that is the core of heads-up play.
2. **The current values are an accident of chronology, not a policy.** The
   two-seat and three-seat profiles predate the cap (RFC 0009/0010 era); the
   six-seat profile and flop library postdate it (2026-10-16). Nothing
   declares which cap a *new* shape must use, so the missing four/five-seat
   profiles and the three-seat 100 BB profile cannot start without inventing
   a value ad hoc.
3. **The action-abstraction error axis is unmeasured.** RFC 0031 measured
   the card-abstraction axis and set the procedural precedent (pre-registered
   rule, two iteration counts, confidence intervals, keep/adopt/inconclusive).
   Pluribus (Brown & Sandholm, 2019) identifies action abstraction as
   typically the largest approximation error source in large poker games.
   The published cap-1 NashConv figures for multiway play
   (`t3-six-seat-feasibility.md`, 100K campaign, 0.02-0.20 BB) are measured
   **inside the capped, coarse-menu game** and therefore say nothing about
   the error the cap introduces against a richer game; RFC 0036's
   production-menu calibration already found those coarse-menu numbers do
   not transfer (`docs/plans/0036-implementation.md`, production identity
   node table).

Industry practice supports shape-varying granularity under one framework:
Pluribus trains a coarse blueprint and refines sizing in real-time nested
solves; commercial solvers configure per-spot bet-size tables. No strong
system uses one global action parameter for every player count and street.
The project already invested in part of the mechanism — RFC 0030 put the
cap in every artifact's digest. What is missing is the rule that assigns
production caps.

## Goals

- **G1**: One owned policy function `production_raise_cap(shape)` (plus its
  stack-band classifier) in `bs::abstraction`, with a frozen revision
  token, returning the production cap for supported shapes (2..6 players,
  standard and shallow bands).
- **G2**: A pre-registered ablation protocol — fixed production menu,
  translator, fixture set, iteration counts, seeds, estimator,
  infeasibility recording, and a **total** decision function from measured
  outcomes to every table cell — that freezes the table from measurements
  of caps 0/1/2 at 2/3/6 seats.
- **G3**: `resolve_flop` consults the policy at the flop root for supported
  shapes instead of hardcoding cap 1. `resolve_uniform` is explicitly
  untouched (see Non-Goals).
- **G4**: A root-aware local audit proving every shipped artifact agrees
  with the policy for its shape, with three named exit states.
- **G5**: The missing preflop profiles (three-seat 100 BB, four-seat and
  five-seat standard) trained Complete under the frozen values **and
  explicitly registered as resident roots in the runner's runtime
  configuration with SHA-256 pins**, so a dealt 2..6-player hand can match
  a profile. Artifact existence alone does not serve a hand.
- **G6**: An explicit cross-street continuity invariant: a hand that
  changes live-player count between streets is served by each root's exact
  identity, while the cap never changes mid-street.

## Non-Goals

- **No size-schedule change.** Every cell keeps the canonical rfc0007
  pot-fraction schedule (preflop/flop/turn bets 1/3, 3/4, 3/2 and raises
  1/2, 1/1; river bets 3/2, 2/1, 3/1 and raises 3/2, 2/1, 3/1). Bet-size
  granularity per shape is a later, separately ablated proposal.
- **No per-street cap variation in v1.** The policy returns one cap per
  shape used on every street of that root. Street-varying caps are a future
  revision once a use case is measured.
- **No RFC 0036 reversal.** The 5/6-seat turn/river on-demand route stays
  declined: RFC 0036 measured the cap-1 path at 5.7-15.7 BB at the envelope
  ceiling, 38x-105x over the 0.15 BB M1 sum gate, and declined on M1
  envelope convergence. The follow-up M2 measurement (river-betting model
  error, recorded 2026-10-26) then FAILED its 0.03 BB gate by orders of
  magnitude, so the blocker is demonstrably not just the cap. Concretely, `resolve_uniform` keeps its current cap-0
  behavior for every seat count; this RFC does not change which turn/river
  shapes solve. A future re-enabled RFC 0036 route reads the policy, but
  re-enabling stays RFC 0036's own gated decision.
- **No mixed-cap BSFLIB03 library.** File-level identity and the single
  binary mount make per-root differing caps impossible in the shipped
  format; see Current State. Mixed-cap flop roots require a follow-up RFC.
- **No deep-stack (> 100 BB), ante, rake, ICM, or tournament policy.**
  Shapes outside the supported table keep today's behavior exactly.
- **No multi-root priority decision.** Which postflop root to train next is
  RFC 0035 stages 4-5, gated on live-session histograms. This RFC fixes the
  *parameter* any such root trains under, not the root's identity.
- **No card-abstraction change.** Settled by RFC 0031 (keep tiers).
- **No opponent-adaptive abstraction.**

## Current State and Evidence

### Facts (verified in this repository)

- **The cap exists, is digest-pinned, and is cheap to vary.**
  `ActionAbstraction::identity(max_raises)`
  (`engine/include/bs/abstraction.hpp:256`); the gate suppresses the
  aggressive menu once `raises_this_street() >= cap`, with zero meaning
  uncapped (`engine/src/tree/abstract_tree.cpp:154-156`); the
  `|max-raises=<N>` token participates in the digest and is reconstructed
  from persisted identity (RFC 0030, implemented).
- **Shipped artifact caps (manifests under `artifacts/`, current as of
  2026-10-26 after legacy retirement):**

  | Artifact | Shape | Cap | Notes |
  | --- | --- | --- | --- |
  | `preflop-profile` | 2 seats, 100 BB | 0 | Complete, 1.32 GB |
  | `preflop-profile-3way-100bb` | 3 seats, 100 BB | 1 | Complete, 0.02 GB |
  | `preflop-profile-4way` | 4 seats, 100 BB | 1 | Complete, 0.07 GB |
  | `preflop-profile-5way` | 5 seats, 100 BB | 1 | Complete, 0.26 GB |
  | `preflop-profile-6way` | 6 seats, 100 BB | 1 | Complete, 1.01 GB |
  | `flop-library-6way-fullrange` | 6 live, limped root | 1 | BSFLIB03, 1,755 classes, 188,432,384 B, sha256 `55c3e976…` |
  | `ac1-16w` | 2 seats, 160 flop classes | 0 | narrow early library |

  The former `preflop-profile-3way` (3 seats, 10 BB, cap 0) validation
  artifact was retired on 2026-10-26 and moved to
  `~/.bigshark-backups/retired-2026-10-26/`; nothing in `artifacts/`
  carries a 3-seat shallow cap-0 identity any more.

- **Pre-RFC wiring gap (since resolved by Stage 4).** At authoring time no
  four-seat or five-seat preflop artifact existed, and profiles were not
  auto-discovered: the runner detected only directories named
  `flop-library*`
  (`platforms/river-club/src/engine.ts:158-169`), so a preflop profile
  reached the engine only as an explicit
  `--resident-root <path>=<sha256>` entry from `residentRoots` in
  `.runtime/config.json`
  (`apps/engine-host/main.cpp:6-11,482-503`;
  `apps/river-club-agent/main.ts:13,141,312-343`). The engine host itself
  ships with no default resident root (`main.cpp:9`). Engine-side identity
  matching does include player count, but only among already-loaded roots.
  Stage 4 closed the gap: the 3-6-seat 100 BB profiles shipped and
  `resolveResidentRoots` now auto-discovers every `preflop-profile*`
  directory with manifest sha256 pins (explicit config/env still takes
  precedence).
- **The BSFLIB03 action identity is file-level, and the format is
  single-cap per library.** The file header stores one
  `action_abstraction_digest` plus the full canonical name/version/
  parameters (including the cap token) and one size schedule
  (`engine/include/bs/mmap_library.hpp:30-37`); "the two abstraction
  identities stored in the file header … a class section does not carry
  them" (`:206-213`). `RootDirectoryEntry` carries only root id, line tag,
  pot, stacks, contributions, folded mask, live count, range profile, and
  class count — no action or card identity (`:42-52,186-196`). RFC 0035 is
  explicit: "No per-root distinct action/card abstractions in phase 1.
  Every root uses … `max_raises_per_street=1`"
  (`docs/rfcs/0035-...:106-110`). Independently, the runner rejects mounting
  a second binary library or a binary alongside any other flop source
  (`engine.ts:136-149`). Consequently every root in one `.bin` shares one
  cap, and two cap-differing libraries cannot be served together today.
- **Runtime cap producers are hardcoded in two distinct places with
  different reach:**
  - `resolve_flop` builds `ActionAbstraction::identity(/*max_raises=*/1)`
    for **all** seat counts (`engine/src/resolver/subgame_resolver.cpp`,
    near `:525-529`). It handles board-3 flop nodes only; it is the RFC
    0025 flop-rooted depth-limited path, gated by
    `BIGSHARK_SUBGAME_UNIFORM` (`apps/engine-host/main.cpp:180-230`).
  - `resolve_uniform` mints `ActionAbstraction::identity()` — cap 0 — at
    `engine/src/resolver/subgame_resolver.cpp:466`, and is the first
    blueprint-free attempt for turn/river nodes from
    `main.cpp:191`. At multiway 100 BB shapes the cap-0 production-menu
    tree exceeds the resolver's limits (`ResolveLimits`: 5,000,000 nodes,
    1,000,000 information sets, 1 GiB, `engine/include/bs/resolver.hpp:
    184-187`; RFC 0036 measured production identity turn trees exceeding
    1 GiB from three seats up at 100 BB), so the build fails and the spot
    falls through to the labelled fallback. **Changing this cap would make
    those trees build and publish `approximate`, re-playing precisely the
    route RFC 0036 measured and declined; this RFC therefore does not touch
    it.**
  - The RFC 0036 bounded-resolver benchmark hardcodes cap 1
    (`engine/benchmarks/subgame_nash_conv.cpp:1191,1268`) and the historical
    T3 campaign modes carry their own pinned constants (`:1407,1436`); none
    is routed in production.
- **Builders take an explicit flag.** `preflop_profile_builder.cpp:140-146`
  (`--raise-cap`, default 0); `flop_library_builder.cpp` defaults cap 0 at
  `:75` and parses the flag at `:117`. Nothing validates that a production
  invocation matches any policy; the AC9 audit checks token *presence*, not
  token *correctness for the shape* (`tools/artifact-audit/ac9-token-audit.py`).
- **Feasibility evidence (T3).** The uncapped full turn-to-river tree is
  20,739 nodes / 11.4 MiB at two seats 100 BB but 970,891 nodes / 605 MiB
  at three seats 100 BB (trainer information-set cap hit), and over 1 GiB
  at five/six seats. Depth-limited (`streets(1)`) **cap-1, declared-only**
  node counts are stack-independent: 153 / 505 / 1,457 / 3,809 / 9,361 for
  2..6 seats (turn-rooted, one-bet-one-raise menu). The
  **production identity menu has no equivalent flop-rooted two-seat cap-0
  measurement**; the closest points are a six-seat cap-1 flop-rooted build
  (~466K nodes in the calibration harness, ~10x its 46,585-node turn tree)
  and a production-menu uncapped two-seat **turn** tree of 33,693 nodes
  (`docs/plans/0036-implementation.md:60-63`). The heads-up flop change this
  RFC makes is therefore spike-gated, not assumed feasible.
- **A deterministic action translator already exists and is tested.**
  `bs::stage6` R7 (`engine/include/bs/stage6/translator.hpp`) maps an
  abstract distribution onto concrete legal actions: passives pass
  through, aggression snaps to the nearest legal target with ties to the
  smaller total, masses merge, and untranslatable aggression follows the
  declared call -> check -> fold fallback; `project_exact_to_coarse_index`
  performs the inverse row lookup. It lives in the stage-6 evaluation
  library and is never linked into the service. The ablation reuses this
  exact rule rather than inventing a projection.
- **The mid-street raise counter is seeded and identity-checked** (RFC
  0030): `build_subgame_def_from_node` propagates `raises_this_street`, so a
  policy-selected cap composes correctly with continued streets.

### Assumptions (to be verified in Stage 1)

- A policy trained in a cap-*c* game and evaluated by a best response
  walking a richer same-depth cap-*r* tree, with the stage6 translator
  projecting rows and a pinned default at beyond-cap nodes, prices the cap
  error in BB. The identity negative control (evaluate cap *c* in a cap-*c*
  tree) must reproduce the in-model NashConv; if it does not, the
  translation is defective and the run is invalid by construction.
- At six seats no uncapped reference tree can be built, so the six-seat
  translation cell uses the richest *feasible* reference cap and is labelled
  a proxy, never an absolute cap-error bound.

## Design Principles

- **One framework, shape-keyed parameters.** Same size schedule, same
  digest/identity machinery, same quality thresholds for every shape; only
  the cap varies, and only by a frozen rule.
- **Measure before freezing.** The provisional table is a starting point;
  the shipped table is the ablation outcome recorded against a rule
  declared beforehand. A threshold chosen after seeing results is not a
  measurement.
- **Maximize structure under constraints.** Among caps that pass the
  resource and quality gates, choose the richest (cap 0 uncapped is richer
  than cap 2, which is richer than cap 1). The cap exists only where the
  richer tree is forced out by resource limits.
- **Conservatism on interpolation and proxies.** A shape not directly
  measured (4/5 seats) is fixed by interpolation, never moved by
  diagnostics; a proxy reference cannot promote any cell.
- **No new serving surface by accident.** This RFC changes only the two
  heads-up flop cells (both spike-gated). Declined RFC 0036 shapes and
  unsupported shapes behave exactly as today.
- **No silent shape change.** A policy miss leaves the current path intact
  with a named diagnostic; the resolver never guesses a cap.
- **Policy is identity, not config.** The table is a compiled, test-pinned
  constant with a revision token, not user-editable configuration; changing
  it is an RFC-level event that changes artifact digests.

## Proposed Design

### Shape definition

```cpp
namespace bs::abstraction {

enum class StackBand {
  Shallow,   // max effective stack among live players <= 16 BB
  Standard,  // > 16 BB and <= 100 BB (the 100 BB buy-in operating band)
};

struct GameShape {
  std::uint8_t street_start_players;  // preflop: dealt players; postflop: live at street start
  StackBand band;
};

// Revision token recorded in audit output. Bumped only by a superseding RFC.
constexpr char kShapePolicyRevision[] = "shape-policy-v1";

// nullopt = unsupported shape (player count outside 2..6 or > 100 BB band).
std::optional<std::uint8_t> production_raise_cap(const GameShape&) noexcept;
StackBand stack_band_for(std::uint32_t max_effective_stack_bb) noexcept;

}  // namespace bs::abstraction
```

Player count semantics: preflop artifacts are keyed by **dealt** players
(every seated player has cards); postflop roots are keyed by players **live
at street start**, matching the RFC 0035 root identity (folded mask and
per-seat contributions). `resolve_flop`, which roots at the current flop,
reads live players from the game root and the deepest-cover stack
(`deepest_cover` in `engine/src/tree/abstract_tree.cpp`). The 16 BB
boundary is the measured three-seat identity-tree cliff; above it cap >= 1
makes T3 subgame node counts stack-independent, so v1 needs no finer
partition. Stacks above 100 BB and player counts 7..10 (mechanically
accepted by the engine) return `nullopt`.

### Provisional table (starting point, NOT the frozen outcome)

| Players | Shallow (<=16 BB) | Standard (16-100 BB) |
| --- | --- | --- |
| 2 | 0 | 0 |
| 3 | 1 | 1 (only conditionally promotable to 2, see rule) |
| 4 | 1 | 1 (fixed by interpolation) |
| 5 | 1 | 1 (fixed) |
| 6 | 1 | 1 (fixed) |

Rules of construction:

- Two seats stay uncapped: the certified heads-up path, and full trees are
  feasible at all T3 depths.
- Every three-or-more-seat shape ships cap 1 at every depth: cap 0 is
  builder-infeasible in the postflop resolver geometry, and the project
  operates the 100 BB buy-in, so the legacy 3-seat 10 BB cap-0 validation
  profile was retired on 2026-10-26.
- Four seats inherit cap 1 by conservative interpolation; four-seat
  measurements are diagnostic only.
- Three seats standard is the single cell the ablation may promote from
  cap 1 to cap 2, and only under the conditions the total function states.

The provisional table matches every shipped artifact, so a fully confirmed
ablation outcome invalidates no shipped bytes. The frozen table is the
ablation result recorded in this RFC's Implementation Record.

### Ablation protocol (Stage 1, pre-registered)

**Harness.** Extend `bigshark-subgame-nash-conv` with `--raise-cap N` and
a reference-cap argument (today cap 1 is hardcoded at
`subgame_nash_conv.cpp:1191` in the M1 modes and pinned in the T3 historical
modes at `:1407,1436`, which stay hardcoded to preserve campaign
reproducibility). A new `--ablation` mode runs one cell: train the candidate
at cap *c*, build the same-depth reference tree at cap *r*, and walk the
best response against the translated candidate. Raw output goes to
`docs/plans/ablation/rfc0037-<cell>.csv` (RFC 0031 precedent).

**Pinned game configuration for every cell:**

- Action menu: the **production identity** rfc0007 schedule (preflop/flop/
  turn bets 1/3, 3/4, 3/2 and raises 1/2, 1/1; river bets 3/2, 2/1, 3/1 and
  raises 3/2, 2/1, 3/1) — never the T3 declared-only one-bet-one-raise
  menu. Results are therefore **not** compared numerically to the T3
  0.02-0.20 BB band; that band is coarse-menu evidence.
- Geometry: turn-rooted subgame, `DepthLimit::streets(1)`,
  `EquityFrontierEvaluator`, 100 BB for standard cells and 10 BB for
  shallow cells (same configuration family as the RFC 0036 M1 envelope
  measurements, so in-model figures cross-check those).
- Ranges: the T3 ten-hand uniform board-blocked sub-range; six-seat rows use
  the RFC 0036 twelve-hand river-compatible range where ten hands admit too
  few disjoint holdings. Full-range (1,326-combo) claims are out of scope;
  they belong to the delivery register's independent quality campaign.
- Iterations: two counts, 1,000 and 100,000 (wall-capped completion
  reported as in the 100K campaign). The **100,000-count is the sole
  decisive count**; 1,000 is a convergence diagnostic. Promotion requires
  the high-budget pass; if the two counts disagree, the rule keeps the
  shipped cap.
- Seeds: at least three independent learn/confirm seed groups per point
  (floor given multiway cost; intervals are t-based 95% over seeds).

**Cells.**

| Cell | Candidates trained | Reference cap r | Role |
| --- | --- | --- | --- |
| 2 seats standard | 0, 1, 2 | 0 (feasible) | direct; fixes 2 -> 0 |
| 3 seats shallow | 0, 1, 2 | 0 (T3: 119K nodes / 75 MiB full at 10 BB; same-depth attempt measured) | direct in the original pre-registration; **RETIRED 2026-10-26 with the legacy 3-seat 10 BB cap-0 profile — the cell is frozen at cap 1 by the legacy-retirement amendment below, and its ablation rows are moot** |
| 3 seats standard | 0, 1, 2 in this geometry (see layers note) | attempt 0 on the same `streets(1)` geometry; T3-DL **coarse-menu** evidence builds at 46,267 nodes / 36.5 MiB, so a production-menu FITS verdict is expected but must be recorded as the condition-(i) result | only promotable cell; direct when r = 0 FITS, proxy only if r > 0 |
| 4 seats standard | 1, 2 | as feasible | diagnostic only |
| 5 seats standard | 1, 2 | as feasible | diagnostic only |
| 6 seats standard | 1, 2; cap 0 reference `INFEASIBLE` (>1 GiB; both the T3-DL `streets(1)` geometry at all depths and the full tree) | 2 if it builds, else 1 | proxy; cannot promote |

**Three feasibility layers (kept distinct; n1).** The word "feasible"
has three different meanings in this matrix and an `INFEASIBLE` verdict
must name which layer it is in:

1. **Reference-tree build** — constructing the cap-*r* tree the BR walks.
   For three seats standard at cap 0, T3-DL coarse-menu evidence
   (declared-only one-bet/one-raise menu, NOT the production rfc0007
   schedule) measured the `streets(1)` tree building at 46,267 nodes /
   36.5 MiB; the production-menu build is larger (RFC 0036 measured a
   ~3.7x production/coarse ratio at six seats) but still projects far
   below 1 GiB. That coarse-menu figure supports a FITS expectation only;
   the production-menu build itself is the recorded condition-(i)
   verdict. When that build FITS with r = 0, the identity control
   `E_0(0)` is necessarily computable — the two statements must not be
   read as contradictory.
2. **Candidate-training budget** — completing iterations inside the wall
   limit. A wall-capped completion is an accepted outcome (the six-seat
   100K campaign completed at ~60K of 100K); it is not `INFEASIBLE`.
3. **Standalone-profile full-tree feasibility** — a separate fact about
   the preflop profile builder, not the ablation geometry. The uncapped
   three-seat 100 BB *full* turn-to-river tree accounts 970,891 nodes /
   605 MiB and hits the trainer information-set cap (T3), which is exactly
   why the three-seat standard profile ships cap 1 and why no cap-0
   candidate in the ablation can make a cap-0 *profile* trainable.

An `INFEASIBLE` verdict records the exact cap hit (builder bytes, trainer
bytes, information-set count, or wall) and its layer, in the T3 tradition;
it is never a skipped cell. If six-seat cap 2 does not build, r = 1 and the
cell cannot price any error: the identity control still runs, and the axis
is recorded unpriceable at six seats rather than guessed.

**Translation and evaluation.**

1. The reference tree uses the same geometry and depth as the candidate
   (`streets(1)`); it differs only in cap, so the priced quantity is the
   cap error, not a confounded depth error. A full-depth reference number is
   additionally printed for context and clearly labelled.
2. Reference-tree information sets are partitioned into *cap-reachable*
   nodes (some cap-*c*-legal line reaches them) and *beyond-cap* nodes
   (require more than *c* full raises on a street).
3. At a cap-reachable node the candidate row is projected onto the node's
   concrete legal actions with `translate_coarse_to_exact`
   (`translator.hpp:84-86`): passives pass through, fraction targets map
   identically (same rfc0007 schedule; only all-in clamps snap), ties go to
   the smaller target, masses merge.
4. At beyond-cap nodes no candidate row exists. The pinned primary default
   is **uniform over the legal passive actions** (call, else check; fold
   stays an explicit equal-share member only when it is the passive menu) —
   the candidate never invents aggression past its cap. A sensitivity arm
   fills those nodes **uniform over the full legal menu**. Both arms are
   reported; a promotion must clear the rule on the *worse* arm.
5. The measured quantity is `E_r(c)`: summed best-response gap in BB (BB =
  2 chips) of the translated cap-*c* candidate in the cap-*r* tree, pooled
   walker, with per-seat components. In-model NashConv `I(c)` is printed for
   context.
6. **Identity negative control:** with r = c, `E_c(c)` must equal `I(c)`
   within a declared tolerance (1e-6 relative on the fixture); a mismatch
   invalidates the cell before any decision is taken. Equivalence and
   negative tests pin rules 3-4 in the native suite before Stage 1 runs.

**Pre-registered CSV columns:**
`cell,seats,band_bb,depth,candidate_cap,ref_cap,iterations,seed,nodes,infosets,
bytes,build_ms,completed_iters,feasibility,inmodel_nashconv_bb,
translated_gap_passive_bb,translated_gap_uniformfill_bb,per_seat_gaps_bb,notes`.

**Total decision function (outcomes -> frozen cells).** Let `[lo, hi]` be
the 95% interval of `E_r(c)` and `b` the cell's shipped baseline cap;
`minimum = max(0.01 BB, (1/3) * |E_r(b)|)` (the RFC 0031 effect-size rule).

- **Cells 2 standard, 2 shallow, 3 shallow:** fixed at 0; measurements
  serve as controls. If a fixed cell's required cap-0 feasibility does not
  hold (other than six-seat cap 0, whose infeasibility is an expected
  recorded fact), the checker emits `fail-fixed-cell-surprise` naming the
  cell and cap hit; the cell is not silently assigned another value — the
  failure halts adoption and goes to a superseding revision.
- **Cell 3 standard:** promote 1 -> 2 only if, at the decisive count, (i)
  the reference verdict is `FITS` with r = 0 (non-proxy), (ii) the whole
  interval of cap 2 lies strictly below cap 1's under BOTH fill arms
  (`hi(2) < lo(1)`), and (iii) `E_r(1) - E_r(2) >= minimum`; otherwise keep
  1. A proxy reference, overlapping intervals, an effect below the floor,
  or an infeasible cap-2 tree keeps 1.
- **Cells 4/5/6 (both bands):** fixed at 1 regardless of their diagnostic
  runs; six-seat cap-0 infeasibility is recorded as a feasibility fact, not
  a promotion input.
- **Scope of a 3-standard promotion:** the three-seat 100 BB preflop
  profile trains at cap 2 and rootless three-seat-standard flop resolving
  uses cap 2. A cap-2 flop *library root* cannot join the cap-1 BSFLIB03
  file or share its mount; it waits for the format/mount follow-up RFC.
  Turn/river on-demand stays RFC 0036-governed regardless.
- The committed CSV rule-checker script computes every frozen cell from the
  files; there is no human adjudication branch.

### Runtime wiring (scoped)

- `resolve_flop` is the only runtime producer changed. For a supported
  shape it consults the policy, with the single supported-shape exception
  named below. The table is exhaustive for every reachable board-3
  `resolve_flop` cell (the resolver accepts 2..10 seats,
  `subgame_resolver.cpp:498-503`):

  | resolve_flop cell | Today | v1 after RFC |
  | --- | --- | --- |
  | 2 live, shallow | 1 | **0 — declared change; Stage-0a PASSED (147,308 nodes / 149 MiB / 133 ms); ACTIVATED** |
  | 2 live, standard | 1 | **0 was gated on the Stage-0a spike; spike DROPPED on the 1 GiB byte cap, so the cell STAYS 1** |
  | 3 live, shallow | 1 | 1 (table and legacy agree; no behavior change) |
  | 4-6 live, shallow | 1 | 1 (table and legacy agree; no behavior change) |
  | 3-6 live, standard | 1 | 1 (table and legacy agree; no behavior change) |
  | nullopt shapes (7-10 players, > 100 BB) | 1 | unchanged, via one named `kUnscopedShapeLegacyCap = 1` constant with a tracking comment |

  The 2-live standard cell is the single board-3 serving/training
  divergence: its training cap is 0 but the resolver serves cap 1 because
  the production-menu Stage-0a spike exceeded the 1 GiB byte cap. Every
  three-or-more-seat cell serves its training cap directly. The 2-live
  shallow cell activates cap 0: heads-up is the uncapped policy family at
  every depth, and T3 has direct shallow feasibility evidence (the
  uncapped full two-seat 10 BB tree builds in 5,739 nodes / 2.9 MiB and
  completes 100K iterations, `t3-six-seat-feasibility.md` results matrix;
  the depth-limited build is 459 nodes). The production identity menu had
  no measured flop-rooted point, so Stage 0a measured both heads-up cells
  and each cell kept cap 1 independently if its own kill criterion
  tripped; the standard cell tripped and the shallow cell passed.
  Three-or-more-seat cells never had a cap-0 row in the operating policy:
  the legacy 3-seat 10 BB cap-0 validation profile was retired
  2026-10-26 (100 BB is the operating buy-in), and every 3+ seat shape
  trains and serves cap 1 uniformly.
- `resolve_uniform` is **not modified**: it keeps cap 0 at every seat
  count, preserving RFC 0036's commitment that the 2/3-seat chains and the
  blueprint-free turn/river ladder behave exactly as today. Existing
  resolver fixtures must pass unchanged; a focused test pins the cap 0 mint
  at `subgame_resolver.cpp:466`.
- A policy `nullopt` never creates a new refusal: the named legacy constant
  keeps the current path, with an `abstraction-policy-unscoped` diagnostic.
  A *supported* shape whose tree build fails keeps the existing
  Unsupported -> fallback ladder.
- Builders keep explicit `--raise-cap` for experiment reproducibility;
  their `--help` names the policy as the production default; the audit
  rejects a production artifact whose flag disagrees with the policy for
  its shape.
- A re-enabled RFC 0036 route reads the policy in its own gated RFC; this
  RFC adds no route.

### Cross-street continuity

- The cap is fixed at **street start** for that street. Mid-street folds
  shrink live count but do not re-run the lookup: the RFC 0030 raise
  counter continues to bound aggressive actions.
- Between streets the shape is re-evaluated from the new root; the served
  artifact is selected by exact root identity (RFC 0035), which carries the
  file's cap. The policy governs training parameters and rootless
  resolving and must agree with the audited artifact set; a disagreement is
  an audit failure.

### Audit (tri-state, root-aware)

`tools/artifact-audit/` gains a policy audit run alongside AC9:

1. **pass (exit 0):** for each standalone preflop profile, the manifest's
   player count/stack maps to a table cap equal to its stored cap token.
   For a BSFLIB03 library: the file header holds one action identity, and
   every root's shape maps to that same cap (a library mixing cap-required
   shapes fails).
2. **superseded-warning (exit 0, named output):** an artifact built under a
   recorded predecessor policy revision, only inside a declared migration
   window recorded with the audit run; the warning names expected and
   actual caps and the revision.
3. **fail (nonzero exit):** a same-revision mismatch, a mixed-cap library,
   an unparseable shape, or a missing cap token.

Like AC9 this is a local operator gate over gitignored `artifacts/`, never
a repository test on artifact bytes; the policy function itself has an
exhaustive native table test. Output always prints
`kShapePolicyRevision`.

### Preflop profile wiring

Training the three missing profiles is not sufficient to serve them: the
runner only loads explicit resident roots. Rollout stage 4 therefore also
pins the full shipped set (2-seat, 3-seat standard, 4-seat, 5-seat,
6-seat) as `residentRoots` entries in the runtime configuration the
runner consumes (`.runtime/config.json` / `BIGSHARK_ENGINE_RESIDENT_ROOTS`,
each `{path, sha256}`), with a launcher-level test that the started engine
advertises every dealt-count root. Where the default runtime configuration
is documented or generated, that documentation is updated in the same
stage.

## Dependency Rules

- `bs::abstraction` owns the policy, band classifier, and revision token.
  Resolvers and builders depend on it; it depends on no solver, resolver,
  service, or artifact module.
- The ablation harness belongs to the engine benchmark/eval surface and
  links `bs::stage6`'s translator (already eval-only); production code
  never depends on the harness or the translator.
- No wire-protocol, protobuf, resident-format, or BSFLIB change.
- RFC 0035 roots train under policy-selected caps only within the format's
  single-cap constraint (standard-band 3-6-live roots at cap 1 in the
  existing library). A root whose shape requires another cap is blocked on
  a follow-up format/mount RFC; it is not silently trained elsewhere.

## Compatibility and Migration

- **No artifact format change.** RFC 0030's token and the BSFLIB03
  file-level identity already carry the cap.
- **One runtime behavior change, heads-up shallow flop only:** the
  two-live shallow-band `resolve_flop` cell solves at cap 0 instead of
  cap 1 after its Stage-0a feasibility spike passed. The two-live
  standard-band spike failed the 1 GiB byte cap, so that cell stays at
  cap 1 and changes nothing at runtime. Everything else is unchanged:
  `resolve_uniform` at every seat count; 3-6-seat flop resolving at every
  band (cap 1, matching the table); 7-10 player and >100 BB shapes (named
  legacy constant, cap 1); and every turn/river path.
- Provisional-table confirmation invalidates no artifact. A promoted
  three-seat-standard cell changes future training; existing artifacts stay
  loadable and the audit reports a predecessor-revision artifact as
  superseded-warning, never refusing to load (identity mismatch already
  routes to the resolver ladder today).
- **New preflop profiles are additive** artifacts plus explicit
  configuration entries; no existing entry changes shape.
- No feature flag, no compatibility shim, no dual tables.

## Security and Operational Impact

Offline training and measurement; no credentials, no live play, no wire
surface. Profile cost does not track subgame node counts — the shipped
2-seat artifact (1.32 GB) is larger than the 6-seat one (1.0 GB) because
tree/menu shape, not seat count, drives stored rows — so profile costs are
expected from manifest evidence, not inferred: the shipped 3-6-seat
100 BB cap-1 builds each record their own trainer limits in their
manifests, and any future rebuild requiring raised limits records them
per profile. The 6-seat profile
recorded 640 s training wall / 828 s pipeline / 6.4 GB peak RSS. Fairness
is unchanged: every decision stays inside legal menus and the shape key
uses only public information. A policy miss is observable via its named
diagnostic.

## Alternatives Considered

1. **Global cap 1 everywhere.** Rejected: it removes 4-bet/5-bet play from
   heads-up, where it matters most, to solve a six-seat feasibility problem
   two-seat trees do not have.
2. **Global cap 0 with runtime fallback on tree exhaustion.** Rejected:
   five/six-seat builds provably exceed limits, making the common multiway
   shape systematically fail into fallback with no quality control.
3. **Wire `resolve_uniform` to the policy too.** Rejected in review: it
   would make cap-1 multiway turn/river trees build and publish where RFC
   0036 measured and declined them, re-opening the route without its gates.
4. **Per-spot operator-configurable caps.** Rejected: production strategy
   identity must be reproducible and digest-pinned; revision changes go
   through RFCs.
5. **Freeze the provisional table without an ablation.** Rejected: it
   codifies the chronological accident and leaves the action-abstraction
   error axis unmeasured.
6. **Vary bet-size schedules per shape immediately.** Deferred: cap is the
   binding feasibility variable, and changing both axes at once would
   confound the ablation.
7. **Fold this into RFC 0036.** Rejected: RFC 0036 owns the declined
   on-demand route; offline profiles and flop-root training need the policy
   regardless.
8. **Assume mixed-cap roots fit BSFLIB03.** Rejected by the format: one
   file-level identity and one mounted binary. A mixed-cap future is a
   named follow-up, not a silent assumption.

## Risks

1. **A heads-up cap-0 production-menu flop tree is unexpectedly large.**
   Stage 0a measures the shallow and standard cells separately before
   wiring; if one crosses a ResolveLimits bound (5M nodes / 1M infosets /
   1 GiB) or its envelope projection, that cell alone stays at cap 1 and
   the finding is recorded — the RFC then ships with the other cell (or
   with neither if both trip).
2. **The ablation is inconclusive everywhere (wide t intervals from three
   seeds).** The total function then keeps the provisional table; mechanism,
   audit, and profile training still land, and the CSV is evidence for a
   later higher-sample campaign (the RFC 0031 n=3 precedent).
3. **The three-seat cap-0 reference does not fit.** The 3-standard cell
   becomes proxy-only and cannot promote; cap 1 stays.
4. **Six-seat cap 2 is infeasible.** The axis is unpriceable at six seats;
   recorded honestly; cap 1 stays and no upper bound is claimed.
5. **A table revision strands built profiles.** Artifacts pin their cap in
   digest and manifest; a revision rebuilds like the RFC 0030 stage-3
   rebuild, predecessors preserved outside the repository.
6. **Mid-street fold count tempts re-lookup.** Forbidden by the
   street-start invariant and pinned by a test that folds a player after
   the first bet and asserts the menu does not widen.
7. **New profiles need raised trainer caps.** Disposition is explicit
   limit flags per build (as RFC 0030 stage 3 did), recorded in manifests;
   never a global limit bump.

## Verification Plan

- **Policy unit tests:** exhaustive 2..6 x bands table pinned to the frozen
  values; unknown band/7-10 players return nullopt; classifier boundaries
  at exactly 16 BB, 100 BB, 101 BB; revision token present.
- **Translator tests:** projection equivalence for a cap-reachable node;
  pinned passive default and uniform-fill arm at beyond-cap nodes; the
  identity negative control `E_c(c) == I(c)` within tolerance.
- **Ablation evidence:** declared CSVs with per-cell intervals and the
  scripted total-function application; no hand adjudication.
- **Runtime tests:** one menu-width assertion per activation-table row —
  two-live shallow serves cap 0 after its spike passed; two-live standard
  stays at cap 1 after its spike was dropped; three-to-six-live shallow
  and three-to-six-live standard serve cap 1 directly from the table; the
  nullopt legacy path serves cap 1 with its diagnostic; `resolve_uniform`
  cap-0 fixtures for a multiway and a heads-up shape pass unchanged; the
  mid-street-fold non-widening test.
- **Audit:** passes over the shipped set; fails on a miscapped fixture and
  on a synthetic mixed-cap library; prints the revision and tri-state.
- **Profiles and wiring:** the three new profiles Complete at 100K
  iterations with full ranges, frozen cap tokens, SHA-256 sidecars; a
  launcher-level test confirms every dealt count 2..6 advertises its root
  and resident lookup resolves a representative hand per count.
- **Project gates:** full Engine Build Contract, ASan on
  resolver/abstraction/translator surfaces, npm/proto/replay,
  `node bin/check-rfcs.mjs`, `node bin/check-docs.mjs`.

## Rollout Plan

0. **Stage 0a heads-up flop spikes (before any runtime change).** With the
   existing resolver harness, build BOTH two-live, flop-rooted,
   production-identity-menu, cap-0, `streets(1)` trees: the shallow-band
   (10 BB) and the standard-band (100 BB) shape. For each record nodes,
   information sets, bytes, build ms, and envelope-projected iterations;
   each cell has its own pre-declared kill at any ResolveLimits bound, and
   a shallow failure does not block the standard cell or vice versa. Also
   build the 3-live shallow flop tree as a diagnostic (no activation
   follows from it); the measured build exceeded the 1 GiB byte cap at
   this menu/depth, corroborating cap 1 for every 3+ seat shape. Gate:
   both measured tables recorded; each heads-up cell activates
   independently only on its own pass. **Result: shallow PASSED and
   activated; standard and the 3-live diagnostic exceeded the byte cap
   and were dropped.**
1. **Ablation harness and run (no production change).** Parameterize caps,
   add the translation evaluation and CSV rule-checker, execute the matrix
   as background offline training, record every cell's value or
   `INFEASIBLE` verdict and the frozen table here.
2. **Policy module.** `GameShape`, bands, revision token, compiled table
   initialized to the frozen outcome, exhaustive tests.
3. **Scoped runtime wiring and audit.** Convert `resolve_flop` per the
   activation table; leave `resolve_uniform` untouched; add the tri-state
   root-aware audit. Gate: the diff against current behavior is exactly
   the one spike-gated heads-up shallow cell, activated only after its
   Stage-0a spike passed (the standard cell's spike failed, so it is a
   no-op).
4. **Profiles and wiring.** Train the three missing standard-band profiles
   (and only raise trainer limits per-build as required), publish with
   sidecars, and pin the complete 2..6 dealt-count set as resident roots in
   the runtime configuration with the launcher-level advertising test.
5. **Documentation.** Policy, revision, and frozen table in
   `docs/design/gto-engine.md`; update the six-max delivery register (T4
   preflop coverage) and `docs/README.md`; name the mixed-cap follow-up in
   the RFC 0035 plan.

Stop conditions: any ablation outcome the checker cannot classify; any
serving path changing outside the declared heads-up shallow cell (a new
refusal, a fallback -> solve transition, or any cell absent from the
activation table); any profile not terminating Complete; any Stage-0a
spike that trips its kill (that heads-up cell is dropped, not routed
around, while the other cell may still activate).

## Rollback Plan

- Stages 0-2 are additive measurements/constants.
- Stage 3 is a source change; revert restores the cap-1 flop resolver and
  the current ladder. No persisted state is created.
- Stage 4 adds artifacts and configuration entries; removing both restores
  the prior set. No in-place transformation or data migration exists.
- A rejected frozen cell keeps its provisional value; measurements need no
  rollback.

## Open Questions

1. **Stage 0a result (resolved 2026-10-26):** the shallow 10 BB heads-up
   production-menu flop tree PASSED (147,308 nodes / 149 MiB / 133 ms) and
   its cell activated at cap 0; the standard 100 BB heads-up tree EXCEEDED
   the 1 GiB byte cap and its cell stayed at cap 1. The 3-live shallow
   diagnostic build also exceeded the byte cap, so the question of a later
   cap-0 activation for 3+ seat shapes is moot in v1 — the unified table
   trains and serves them at cap 1, and the legacy 3-seat 10 BB cap-0
   validation profile was retired.
2. **Three-seat standard reference:** does the cap-0 `streets(1)` reference
   fit, making the cell directly measurable, or is it permanently proxy?
   The total function states either consequence.
3. **Six-seat cap 2 feasibility:** decides whether any six-seat error bound
   exists; a no leaves the axis unpriceable.
4. **Mixed-cap flop roots (2-live or shallow cap-0 roots in a cap-1 world):**
   require a BSFLIB/format-mount follow-up RFC; this RFC blocks them rather
   than guessing a storage path.
5. **Deep (>100 BB) band policy:** deferred; needs its own evidence and a
   superseding RFC.

## Acceptance Criteria

- **AC1**: The ablation harness accepts candidate and reference caps under
  the pinned production identity menu, uses the stage6 translator with the
  pinned passive/uniform-fill defaults, writes the declared CSV columns,
  and passes the identity negative control (`E_c(c) == I(c)`); the full
  cell matrix executes or records `INFEASIBLE`, naming BOTH the layer
  (reference-tree build for the BR walk, candidate-training budget, or
  standalone-profile full-tree feasibility) and the exact cap hit.
- **AC2**: The committed rule-checker maps CSVs to every frozen table cell
  via the total decision function (fixed cells, the one promotable cell,
  proxy/infeasible/interpolation handling), at the decisive 100K count, and
  emits a named machine verdict for every cell including `promote`, `keep`,
  `infeasible`, and a **`fail-fixed-cell-surprise`** verdict (with cell and
  cap hit) when a fixed cell's required feasibility does not hold; no human
  adjudication branch exists. The frozen table and outcomes are recorded in
  this RFC before stage 2 lands.
- **AC3**: `production_raise_cap` / `stack_band_for` exist in
  `bs::abstraction` with a revision token; exhaustive table, boundary, and
  nullopt tests pass.
- **AC4**: Runtime scope is exactly per the activation table, and every
  reachable board-3 cell in 2..10 seats is enumerated by it: `resolve_flop`
  serves cap 0 for the two-live shallow cell (its spike passed), cap 1 for
  the two-live standard cell through
  `kHeadsUpStandardFlopSpikeDroppedCap` (its spike failed the byte cap),
  cap 1 for every 3-6-live cell at either band directly from the unified
  table, and cap 1 for nullopt shapes through `kUnscopedShapeLegacyCap`.
  `resolve_uniform` is verifiably unchanged
  (existing fixtures pass unmodified, cap-0 mint pinned). A grep over
  `engine/src`, `apps`, `engine/benchmarks`, and `bin` enumerates every
  surviving cap literal as policy call, one of the two named legacy
  constants, a pinned historical T3 mode, or test.
- **AC5**: The tri-state root-aware policy audit passes on the shipped set,
  warns (exit 0, named) on a predecessor-revision artifact, and fails
  nonzero on a same-revision mismatch or a synthetic mixed-cap library,
  printing the policy revision.
- **AC6**: Three-seat 100 BB, four-seat, and five-seat standard preflop
  profiles are Complete (100K iterations, full ranges, frozen cap tokens,
  SHA-256 sidecars), the legacy 10 BB three-seat cap-0 artifact is retired
  (moved out of the shipped set on 2026-10-26), and the
  complete 2..6 dealt-count set is served at 100 BB: runner auto-discovery
  (`resolveResidentRoots`) enumerates every `preflop-profile*` directory,
  pins each db by its manifest sha256 (explicit config/env still takes
  precedence and an empty array still opts out), and the node wiring test
  asserts the resolved launch line advertises every root in player order.
- **AC7**: The street-start invariant is tested (mid-street fold does not
  widen the menu; between-street re-root selects the new root identity),
  and unscoped shapes produce the named diagnostic without new refusals.
- **AC8**: Stage 0a records TWO heads-up flop tables (two-live shallow
  10 BB and two-live standard 100 BB), each with nodes/information
  sets/bytes/build ms, envelope projection, and an independent activate/
  drop kill verdict, plus the three-live shallow diagnostic build; no
  runtime change for a cell precedes that cell's recorded pass.
- **AC9**: Engine Build Contract release gates, ASan on touched surfaces,
  npm/proto/replay, `check-rfcs`, and `check-docs` pass;
  `docs/design/gto-engine.md` documents the policy, revision, activation
  table, and frozen table, and the RFC 0035 plan names the mixed-cap
  follow-up.

## Implementation Record

Status: **Implemented (2026-10-26)**.

### Frozen table (mechanical checker verdict: KEEP)

The full pre-registered matrix ran (`docs/plans/ablation/rfc0037-matrix.csv`,
72 rows, 24 distinct cells, two iteration counts, seeds 101/202/303,
production identity menu, turn-rooted streets(1)). The committed checker
(`tools/artifact-audit/rfc0037-ablation-check.py`) returned no
fail-fixed-cell-surprise and **KEEP** for the sole promotable cell. The
operating frozen table, after the 2026-10-26 legacy-retirement amendment
recorded below, is:

| Players | Shallow | Standard |
| --- | --- | --- |
| 2 | 0 | 0 |
| 3 | 1 | 1 |
| 4 | 1 | 1 |
| 5 | 1 | 1 |
| 6 | 1 | 1 |

**Legacy-retirement amendment (2026-10-26).** The checker's original
verdict froze the three-seat shallow cell at cap 0, carried solely by the
shipped Complete `preflop-profile-3way` artifact (TerminalDepth::Flop
geometry, 10 BB). The project operates the 100 BB buy-in and keeps no
legacy implementations; on explicit maintainer direction the 3-seat 10 BB
cap-0 validation profile was retired (moved out of `artifacts/` into the
local backup `~/.bigshark-backups/retired-2026-10-26/`) and the cell was
re-frozen at cap 1, unifying the policy as "heads-up uncapped; every
3+ seat shape cap 1 at both layers and both bands". The checker's
`PREFLOP_PROVEN` set dropped `(3, 10, 0)` in the same change, so no
resolver INFEASIBILITY can ever re-establish the cell; no shipped or
operating artifact carried cap 0 for that shape after the move, so the
policy revision token stays `shape-policy-v1` (no released artifact exists
under the withdrawn value). The ablation matrix rows for the cell remain on
disk as the historical pre-registered record and are no longer decision
inputs.

No cell promoted. The three-seat standard 1 -> 2 promotion failed the
non-proxy condition: its cap-0 reference exceeds the 1 GiB builder byte cap
in the resolver geometry, so only a cap-2 proxy reference was available.
Under that proxy the cap-2 policy's translated gap (1.8 BB in-model) looks
far better than the cap-1 policy's gap in the cap-2 tree (~34-63 BB), but a
proxy cannot promote by the pre-registered rule — the apparent improvement
is recorded as evidence for a possible future revision, not used to change
the frozen value. The proxy cap-1 gap grows with seat count (3p 34-63, 4p
62-117, 5p 83-156 BB), documenting that the one-raise coarsening is most
distorting multiway; six-seat exact evaluation is combinatorially
intractable and stays a sampled-estimator question. The two-seat standard
direct cap-0 comparison at the decisive 100K count measured the cap-1
policy at 9.1-16.6 BB gap (passive fill) versus the cap-0 policy's
1.56-1.68 BB in the uncapped tree — the direct evidence for shipping cap 0
heads-up deep.

### Stage 0a measured

The `--flop-spike` mode of
  `bigshark-rfc0036-calibration` builds the production-menu, cap-0,
  flop-rooted, `streets(1)` shapes and prints a per-cell verdict against
  `ResolveLimits` and the 1,600 ms envelope; raw output is archived at
  `docs/plans/ablation/rfc0037-stage0a.txt`. Outcomes: two-live shallow
  (10 BB) PASSES — 147,308 nodes, 149.0 MiB accounted, 132.6 ms median
  build, 480 information sets (bound 480), 0.314 ms/iteration, roughly
  4,700 iterations projected inside the envelope — and ACTIVATES.
  Two-live standard (100 BB) and the three-live shallow diagnostic both
  fail the tree builder with "abstract tree exceeded the byte cap"
  (> 1 GiB). Per the pre-declared independent kill, the standard
  heads-up cell stays cap 1 (no serving change at the 100 BB operating
  band), the 3-live diagnostic corroborates cap 1 for every 3+ seat
  shape, and the only runtime change Stage 3 makes is two-live shallow
  flop resolving cap 1 -> 0.
  The result also explains why T3's small two-seat node counts do not
  apply: those were declared-only coarse-menu trees; the uncapped
  production identity sizing chain crosses 1 GiB at the flop-rooted
  depth-limited shape even heads-up at 100 BB.
- **Stages 2 and 3 landed (2026-10-25).** `bs::abstraction` now owns
  `GameShape`, `StackBand` (Shallow/Standard/Deep), `stack_band_for_bb`,
  `production_raise_cap`, `production_flop_resolve_cap`, the revision
  token, and the two named constants (`kUnscopedShapeLegacyCap`,
  `kHeadsUpStandardFlopSpikeDroppedCap`); an exhaustive native test pins
  the table, band boundaries (16/100/101 BB), nullopt shapes, and the
  one spike-drop. `resolve_flop` builds its cap from the policy
  via the public pure helper
  `SubgameResolver::flop_shape_at_street_start`, which fixes the shape at
  street start (flop fold events restore seats folded this street;
  preflop folds are excluded; street-start stack = current +
  street_committed) so a mid-street fold cannot re-run the lookup. The
  tri-state root-aware audit
  `tools/artifact-audit/rfc0037-policy-audit.py` passes on all five
  shipped artifacts, fails nonzero on a miscapped profile and a
  synthetic mixed-cap library, and warns (exit 0) on a
  predecessor-revision manifest. Resolver tests pin per-row node-count
  equality (2-live shallow resolves a 63,804-node cap-0 tree versus the
  prior 5,068-node cap-1 tree; 2-standard, 3-shallow, 6-standard are
  byte-identical to before) and the mid-street-fold invariant.
  `docs/design/gto-engine.md` documents the policy; the RFC 0035 plan
  names the mixed-cap format/mount follow-up. `resolve_uniform` is
  unchanged.
- **Legacy retirement landed (2026-10-26).** The training table's
  three-seat shallow cell moved 0 -> 1 per the amendment above; the
  resolver's `kDeferredThreeShallowFlopCap` was deleted (3-live shallow
  now reads cap 1 directly from the unified table), the 3-seat 10 BB
  artifact was moved out of `artifacts/`, and both audit scripts and the
  exhaustive table tests were updated in the same change.
- Stages 1, 4, and 5 are complete: the full ablation matrix ran and is
  recorded above, the 3-6-seat 100 BB cap-1 profiles shipped Complete
  with runner auto-discovery, and the documentation set is current. See
  `docs/plans/0037-implementation.md` for the stage log.

### Evidence split by geometry (measured 2026-10-25)

The cap serves two consumers with DIFFERENT tree geometries, and the
ablation directly measures only one; the other's feasibility is an
observed artifact fact. After the 2026-10-26 legacy retirement the two
layers carry the same cap for every operating shape except one.

- **Standalone preflop profiles** train a `TerminalDepth::Flop` tree with
  the exact all-in-at-flop frontier: only the preflop betting street is
  expanded, and any flop reached settles by showdown equity. Here cap 0
  is cheap at shallow stacks — the retired three-seat 10 BB profile was
  Complete at cap 0 with 23,404 tree nodes — because the short jam chain
  contains little structure to delete. That historical observation
  explains why the legacy artifact could exist; it no longer sets any
  operating cell, because the 100 BB buy-in is the operating band and the
  artifact was retired.
- **Postflop rootless resolving** builds a `streets(1)` subtree that
  expands a full flop/turn betting street plus 48/49 board-outcome chance
  nodes. This is the geometry the Stage-1 ablation and the Stage-0a spike
  measure. Under the production identity menu its cap-0 tree exceeds the
  1 GiB builder byte cap at three or more seats at every depth (the T3
  small node counts used the declared-only coarse menu and do not
  transfer). The resolver's independently measured activation table
  (`production_flop_resolve_cap`, with its one named spike-drop constant)
  carries this geometry; the single training cap in `production_raise_cap`
  carries the preflop identity. The only shape where the two layers
  diverge in the operating set is two-live standard flop resolving:
  preflop profiles for heads-up train at cap 0, while the resolver serves
  cap 1 through `kHeadsUpStandardFlopSpikeDroppedCap` because the
  production-menu cap-0 flop tree crossed the 1 GiB byte cap. Every
  3+ seat shape now trains AND serves cap 1 in both geometries.
- The one nominally promotable cell (three-seat standard 1 -> 2) has no
  non-proxy reference in the resolver geometry (cap 0 exceeds the byte
  cap) and is fixed at cap 1 either way. Hence no frozen value depends on
  a geometry the ablation did not measure, and no preflop-terminal
  ablation cell is required to freeze the table.

## Decision

```text
Author agent: BigShark maintainers
Approved by: rfc0037-approval-1 (independent approval agent, auto_model/alwaysday1_max;
              same agent across all three rounds)
Decision date: 2026-10-25
Review outcome: Approved
  Required editorial corrections applied at the status-flip edit: E1 the
  stage-3 gate singular "one cell" becomes two cells; E2 the coarse-menu
  caveat on the 46,267-node T3-DL citation.
Reviewed scope: Round-3 narrow re-review of revision 3 — activation-table
  exhaustiveness against board-3 2..10-seat reach, Stage-0a two-cell
  spikes, Compatibility, stop conditions, AC4/AC7/AC8, Verification, the
  n1 three-layer feasibility distinction, and the n2
  fail-fixed-cell-surprise machine verdict; cross-checked the six-max
  delivery register and docs/README.md; check-rfcs (37) and check-docs
  (429) green. Rounds 1-2 verified the full RFC against source, the five
  shipped manifests, the RFC 0030/0031/0035/0036 contracts, BSFLIB03
  header/root records, the host ladder, both builders, artifact
  auto-discovery, and the stage6 translator.
Review summary: Three rounds. Round 1 returned Changes Requested with
  three blockers (a false BSFLIB03 per-root-cap claim — the action
  identity is file-level and phase-1 libraries are single-cap;
  resolve_uniform wiring that would have re-opened RFC 0036's declined
  multiway turn/river route; and a non-pre-registered ablation protocol),
  two majors (an unmeasured heads-up production-menu flop tree assumed
  feasible; preflop profiles not auto-discovered), and five minors.
  Round 2 returned one blocker (the activation table omitted the
  reachable 2-live shallow flop cell) plus two minors. Revision 3
  activates both heads-up flop cells (shallow and standard) behind
  independent Stage-0a spike gates, enumerates every reachable board-3
  cell, separates reference-build/training-wall/profile-full-tree
  feasibility, and makes fixed-cell surprises a machine verdict. The
  approval is design approval only; it authorizes no spending,
  credentials, live play, or destructive operations.
```
