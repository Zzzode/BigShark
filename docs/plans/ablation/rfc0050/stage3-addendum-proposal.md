# RFC 0050 Stage 3 Addendum Proposal — Exact-Policy Format and Serving

Status: PROPOSAL ONLY. No code is written or authorized by this
document; RFC 0050 states Stage 3 is an addendum with its own approval
and is not pre-authorized. It is drafted at TechLead D323 direction and
goes to independent review together with the Stage 2 verdict-gap
candidate. Everything proposed is bench-only and changes no frozen
constant, the nine-rung ladder, the 1,800 s wall, or the 0.002 BB gate.

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
failing does not silently invalidate the other.

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
  on the largest (5,720 nodes), ~3.58x by action-node count.

### 3b — serving path and its quality gate (the actual hard part)

How an online process consumes a stored exact policy while honoring the
G1 decision envelope (1 MiB resident, sub-second per decision) and the
0.002 BB contract. Acceptance here is a measured e_embed <= 0.002 BB,
which is a different and stricter gate than 3a. The two RFC-named
candidate mechanisms, with current evidence:

- Exact-key match: structurally impossible inside G1. One exact anchor's
  policy is ~40-148 MB against the 1 MiB resident cap, and the online
  exact tree carries the same ~200 MB+ working set the offline gate
  measured. This fails on footprint before convergence is even
  considered.
- Embedding exact -> bounded online tree: reintroduces the menu-mapping
  pure-menu residual that RFC 0049 measured at 22-31 BB on the cap-1
  family. That residual is the structural quantity RFC 0050 proved is
  zero only when the menus match; embedding re-creates the mismatch and
  cannot clear 0.002. The addendum must not re-test the known-failing
  cap-1 mapping.
- Margin-based safe subgame gadget fed by offline per-combo
  continuation values: the only candidate that does not embed menus, but
  its certification path was previously blocked on the deep anchor and
  is untested in this RFC; it would need its own pre-registered residual
  gate.

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
exact menu, which G1 cannot host. The dependency is cut at the format
layer only; the serving-quality gate remains honestly dependent on both
a converged policy AND a zero-residual transport, neither of which
exists today.

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
2. If a converged policy exists but neither exact-key (G1 footprint) nor
   a zero-residual transport can serve it, 3b closes as serving-
   infeasible even though the offline solve succeeded.
3. Only the conjunction of a Stage 1 PASS AND a measured zero-residual
   transport within G1 opens a production serving design, which would
   still require its own addendum and production review beyond this
   bench-only RFC.

Either FAIL is a strategic result for the project to act on, not a
reason to relax the 0.002 BB gate or to count a working container as a
serving solution.

## 6. Relation to the Stage 2 verdict-gap review

The same review covers
`stage2-verdict-gap-amendment-candidate.md`. The recommended Route 1
(probe/receipt-layer sub-reason, no frozen classifier change) is the
mechanism Stage 3 reporting would use to distinguish a budget-no-rung
outcome from an instrumentation failure in any future serving
validation, keeping the shared RFC 0049 classifier untouched.
