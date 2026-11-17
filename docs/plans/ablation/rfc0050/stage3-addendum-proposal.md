# RFC 0050 Stage 3 Addendum Proposal — Exact-Policy Format and Serving

Status: PROPOSAL ONLY. No code is written or authorized by this
document; RFC 0050 states Stage 3 is an addendum with its own approval
and is not pre-authorized. It is drafted at TechLead D323 direction and
goes to independent review together with the Stage 2 verdict-gap
candidate. Everything proposed is bench-only and changes no frozen
constant, the nine-rung ladder, the 1,800 s wall, or the 0.002 BB gate.

Review history. The first independent review (2026-11-17) returned
Changes Requested with two major findings, both corrected here: the
draft cited a non-existent "1 MiB resident, sub-second" G1 envelope
(the real envelope is 256 MiB resident, 1 GiB RSS, p95 10 s / max 12 s,
1,000,000 infosets; 1 MiB is only the wire frame limit), and it called
single-anchor exact-key serving structurally impossible when it is in
fact resident-feasible (0.155x-0.55x of the 256 MiB budget). The
correction narrows the infeasibility claim from "cannot host an anchor"
to "cannot provide full-campaign 0.002 coverage without a converged
policy and a zero-residual transport". Minors add an explicit
double-failure retarget statement, label the largest-anchor size an
extrapolation, and clarify the 3a/3b numbering. The 22-31 BB embedding
residual, the unmet Stage 1 PASS precondition, and the 134,459-class
coverage scale were independently confirmed and unchanged.

## 0. Authorization precondition that is currently unmet

RFC 0050 says a BSFLIB04 addendum may be drafted "only after a Stage 1
PASS". Stage 1 returned BUDGET_NO_GO, not PASS: the zero pure-menu
identity is proven and the early slope is steep (-1.004), but no rung
reached 0.002 BB inside the wall and the later convergence law is
uncertified. This proposal is therefore drafted under explicit
direction, and it cannot be APPROVED for serving implementation until
either (a) a Stage 1 PASS is produced under an amended budget, or (b)
the RFC's Stage 3 gating sentence is itself amended. The split in
section 3 is designed so that the convergence-independent part can be
reviewed now without prejudging that gate.

## 1. What Stage 3 would deliver

Two decoupled deliverables with separate acceptance gates, so that one
failing does not silently invalidate the other. The 3a/3b names follow
the parent RFC's Stage 3 numbering (3a persistence, 3b serving) rather
than this document's section order.

### 3a — convergence-neutral persistence format (container)

A versioned, checksummed on-disk container for an exact river policy
(the per-node average-strategy tables plus identity metadata), evolved
from the D307 resume codec which already proves the pattern: magic,
format version, full GameDef identity, action-abstraction digest, node
counts, frozen-constant block, payload length, and an FNV-1a checksum,
with fail-closed load and no best-effort recovery.

- Acceptance: bit-exact save/load round-trip and reject-every-corruption
  tests; NOT any e_embed threshold.
- Convergence dependency: none. The container is valid for a policy at
  any iteration; convergence changes the quality of the stored bytes,
  not whether the format works.
- Measured size: the servable average-strategy snapshot is 41.5 MB on
  the smallest anchor (1,600 action nodes) and scales to roughly 148 MB
  on the largest (5,720 nodes), ~3.58x by action-node count. Only the
  41.5 MB figure is a measured receipt; the ~141 MiB (~148 MB)
  largest-anchor
  figure is a linear action-node extrapolation, not a receipt.

### 3b — serving path and its quality gate (the actual hard part)

How an online process consumes a stored exact policy while honoring the
real G1 decision envelope and the 0.002 BB contract. The envelope,
verified against production code rather than an earlier mistaken draft:

- resident policy budget: 256 MiB
  (`resident_policy.hpp:kDefaultResidentBudgetBytes`), with an existing
  mmap + lazy LRU on-demand residency mechanism (RFC 0012/0035);
- per-resolve envelope: p95 10 s / max 12 s, RSS 1 GiB, at most
  1,000,000 information sets. The 10 s p95, 1 GiB RSS, and infoset cap
  are in `resolver.hpp`; the 12 s max is the RFC 0047 G1 figure
  (`rfc0047_convergence_cert_probe.cpp:kG1MaxMs` and the RFC 0047 doc);
- the 1 MiB constant in the codebase is the wire frame limit
  (`protocol.hpp:kMaxFrameBytes`), not a resident budget, and there is no
  sub-second decision requirement.

Acceptance here is a measured e_embed <= 0.002 BB, a different and
stricter gate than 3a. Against the corrected envelope the candidate
mechanisms must be assessed at two distinct scales, not collapsed.

Single-anchor exact-key is resident-feasible, not structurally
impossible. The smallest anchor's 39.55 MiB policy is 0.155x of the 256
MiB budget (6.5x headroom); the extrapolated largest-anchor 141 MiB
policy is 0.55x (1.8x headroom); the full 202.4 MiB working set is
0.20x of the 1 GiB RSS cap. The existing mmap/lazy LRU path means even
that residency need not be eagerly held. What exact-key cannot do is
provide full-campaign coverage (section 3): it serves a stored anchor
only if that anchor was solved and loaded.

- Embedding exact -> bounded online tree: reintroduces the menu-mapping
  pure-menu residual that RFC 0049 measured at 22.13-31.42 BB across the
  cap-1 family. That residual is the structural quantity RFC 0050 proved
  is zero only when the menus match; embedding re-creates the mismatch
  and cannot clear 0.002. The addendum must not re-test the known-failing
  cap-1 mapping. This is the strongest serving-infeasibility evidence
  and is independent of both footprint and convergence.
- Margin-based safe subgame gadget fed by offline per-combo
  continuation values: the only candidate that does not embed menus, but
  its certification path was previously blocked on the deep anchor and
  is untested in this RFC; it would need its own pre-registered residual
  gate.

So even a perfectly resident single-anchor exact policy still depends on
(a) a converged policy and (b) a zero-residual transport; resident
feasibility removes only the footprint objection, not either of those
gates.

## 2. The hard question — can this be designed without assuming convergence succeeds?

Answer: partially, and the boundary must be stated precisely.

The FORMAT (3a) is fully convergence-independent: its dependency on the
solve is cut at the container boundary. A failed, plateaued, or
abandoned solve still round-trips correctly, and the storage work
retains standalone value as durable, auditable state (the same property
the D307 checkpoint already provides for interruption).

The SERVING QUALITY (3b) cannot be made convergence-independent, and no
design choice removes that fact. If the exact policy's NashConv never
reaches 0.002 BB (the 10^6-10^7 iteration decayed branch or the RFC
0043 plateau), then serving it — by any format, key, or mapping — yields
an e_embed above the contract. A container cannot improve the policy it
holds, and an embedding cannot recover the residual without matching the
exact menu, which would defeat the bounded-tree representation the
online path relies on. The dependency is cut at the format layer only;
the serving-quality gate remains honestly dependent on both a converged
policy AND a zero-residual transport, neither of which exists today.
Resident footprint is no longer the binding objection after correcting
the envelope to 256 MiB / 1 GiB; the two binding objections are
convergence and the missing zero-residual transport.

The design consequence is that Stage 3 must be two gates, not one: 3a
may succeed and ship on its own evidence regardless of convergence; 3b
is permitted to fail independently, and its failure must be reported as
a serving infeasibility rather than hidden behind a working container.

## 3. A coverage gap independent of convergence

Even a fully converged, perfectly stored policy on the six Stage 2
anchors is not an online-served library. An online turn reaches many
distinct river subgames — 48 cards per betting line and, at full scale,
134,459 canonical classes. The six anchors are representative betting
shapes, not complete coverage. A serving proposal must choose explicitly
between:

- sparse deployment: ship solved anchors only and fall back to the
  existing bounded policy elsewhere (the contract then holds only on the
  solved subset; the fallback path keeps its current, non-0.002
  guarantee); or
- full campaign: solve the full frontier, which at the measured
  multi-hour-per-anchor cost and the D303 2.26x parallel ceiling is
  infeasible at current compute and is exactly the scale RFC 0043/0047
  could not close.

This coverage limit holds whether or not convergence succeeds and must
not be conflated with it.

## 4. Proposed budget cap

- Engineering for 3a: no new long solve. Reuse and harden the D307 codec
  behind the link guard; work is bounded implementation plus native
  round-trip and corruption tests, bench-only.
- No new compute is authorized inside this addendum. Any solve that
  produces a policy intended for 3b is a separately funded decision
  bounded by the measured envelope: ~43 h single-thread for the smallest
  anchor under the optimistic -1.0 law, a 2.26x multi-thread ceiling
  (D303), resume-checkpoint durability (D307), and an unknown but
  possibly multi-year cost under the decayed/plateau branches.
- 3b, if attempted, reuses existing sealed mapping instruments for one
  short residual validation and explicitly does not re-run known-failing
  mappings; the margin gadget needs its own pre-registered gate before
  any spend.

## 5. Pre-registered FAIL conditions and the strategic conclusion

The addendum commits to reporting either outcome, never forcing a path:

1. If no Stage 1 PASS is ever produced (plateau or 10^6-10^7 branch),
   3a may still deliver persistence, but 3b closes as serving-
   infeasible: a conformant exact policy does not exist.
2. If a converged policy exists but no zero-residual transport can
   serve it, 3b closes as serving-infeasible even though the offline
   solve succeeded. Exact-key remains available only as sparse
   single-anchor or subset deployment under the 256 MiB / 1 GiB
   envelope, never as full-campaign 0.002 coverage.
3. Only the conjunction of a Stage 1 PASS AND a measured zero-residual
   transport within the real envelope opens a production serving
   design, which would still require its own addendum and production
   review beyond this bench-only RFC.

Either FAIL is a strategic result for the project to act on, not a
reason to relax the 0.002 BB gate or to count a working container as a
serving solution. Explicit terminal statement: if BOTH a Stage 1 PASS is
unattainable AND no zero-residual transport exists, the online 0.002 BB
exact-policy serving line is closed and the project must retarget — for
example to the bounded policy with its current guarantee, a different
quality contract, or an offline-only tool. This document must not be
read as predicting that retarget; it pre-registers the condition under
which it becomes mandatory.

## 6. Relation to the Stage 2 verdict-gap review

The same review covers
`stage2-verdict-gap-amendment-candidate.md`. The recommended Route 1
(probe/receipt-layer sub-reason, no frozen classifier change) is the
mechanism Stage 3 reporting would use to distinguish a budget-no-rung
outcome from an instrumentation failure in any future serving
validation, keeping the shared RFC 0049 classifier untouched.
