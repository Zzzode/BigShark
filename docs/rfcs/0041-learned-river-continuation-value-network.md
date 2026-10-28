---
rfc: "0041"
subject: "Learned River Continuation-Value Network for Multiway Turn Strategy"
status: "Accepted"
authors: "Claude (zzzode agent session 2026-10-28)"
created: "2026-10-28"
updated: "2026-10-28"
owners: "Engine resolver (production bounded turn entry + block refinement driver), engine solver (neural ContinuationValueModel + BSNCV01 codec), engine benchmarks (offline label generator and gates), tools (offline trainer), apps/engine-host (default-deny model mount/wiring)"
supersedes: ""
superseded-by: ""
---

# RFC 0041: Learned River Continuation-Value Network for Multiway Turn Strategy

## Summary

RFC 0039 proved the nested range-conditioned river continuation value is
the mathematically correct leaf object, but exact on-demand solving of
that object cannot meet the 1,600 ms decision envelope (one leaf needs
~10M inner iterations, ~138.6 s cold-start; 240/1,344 leaves per 2/3-seat
turn solve => hours to days per solve). This RFC replaces the per-solve
inner MCCFR in the OFFLINE BENCHMARK/MEASUREMENT and FUTURE PRODUCTION
serving paths with a feed-forward neural network trained offline to
produce the same object — the per-seat per-combo range-conditioned
marginal matrix `V_p(c)` — at microsecond cost.

Important scope honesty (corrected after independent review): the
production serving path does not exist today. Production resolvers pass
`nullptr` for the continuation slot
(`engine/src/resolver/subgame_resolver.cpp:720-721`), no production TU
constructs any `ContinuationValueModel`, and RFC 0039 D3 withdrew
bounded-turn enablement, leaving production at the
`turn_root_unserved` all-miss label
(`engine/src/protocol/envelope.cpp:180`); the block policy-iteration
driver (`refine_leaves` plus the outer block loop) and the exact model's
gap walk are benchmark-local code in
`engine/benchmarks/subgame_nash_conv.cpp`. This RFC therefore includes
building the missing production pieces (bounded turn entry, a production
outer refinement driver, model mount/construction) in addition to the
network; the benchmark's hard-typed `NestedRiverValueModel` gap walk is
generalized to an abstract-model gate (a neural model has no inner tree
or policy to walk). The seam's CONSUMER behavior (trainer hook, fail-
closed, equity fallback) is unchanged, but the seam itself gains the
small additive surface enumerated in §3a. Training is offline (C++
label generation using the RFC 0039 exact solver, separate Python
training step with provenance); inference is C++23 in-process f32 with
no external runtime, weights shipped in a new standalone digest-pinned
`BSNCV01` artifact. This is the DeepStack recipe constrained by the
project's offline, fail-closed, default-deny gates. A network is routed
only if pre-declared RFC 0031-style quality, latency, and structural
gates pass; otherwise the engine keeps the current behavior and the RFC
outcome is recorded as failed, exactly as RFC 0039's stop branch was.

## Motivation

### The measured problem

RFC 0039 Stage 1 evidence (`docs/plans/0039-m2.csv`,
`rfc0039-inner-gap-series.csv`, `rfc0039-identity-control.csv`,
`docs/plans/0039-implementation.md`):

- The correct abstract-game inner gap needs ~10M inner iterations per
  river leaf; cumulative cold-start wall is ~138.6 s per leaf on a
  28-core release build (~13.7 us/iter), 87x the 1,600 ms envelope for a
  single leaf.
- A turn solve has 240 (2 seats, 357-node tree) / 1,344 (3 seats,
  1,864-node tree) frontier leaves; exact solving is ~9 hours per
  2-seat solve and an extrapolated ~14-24 days per 3-seat solve.
- Gate-honest exact solving never serves a leaf in the envelope; at
  feasible iteration counts serving under-converged matrices is
  seed-noise dominated (composed deltas 0.103/0.495/0.450 across seeds
  101/202/303, one seed worse than the equity leaf).
- The equity leaf (assume no river betting) carries a composed M2 model
  delta of 0.402 BB heads-up and 6.161 BB at three seats against the
  frozen 0.03 BB gate (RFC 0036 evidence).

### Why a learned estimator, and why this form

The expensive object is smooth and reusable: the value of a joint
(public board, pot geometry, effective stacks, per-seat conditioned
range) at a river boundary varies continuously in its inputs, and
identical public states recur across hands. DeepStack demonstrated
exactly this decomposition at heads-up no-limit (offline-trained
counterfactual-value network evaluated inside a shallow depth-limited
search; human-professional-level play at decision times measured in
hundreds of milliseconds). The RFC 0039 machinery already provides the
exact per-leaf label generator and the consuming seam, so this RFC adds
the estimator, not a new search architecture.

Users: the offline engine build (model training/validation), the host
decision path for shapes that pass the gates, and the offline practice
table. Expected value: 2-seat turn serving inside the envelope with a
composed model delta near the exact nested solution; an evidence basis
for whether 3+ seats is reachable empirically (multiway has no
zero-sum safety theorem).

## Goals

- G1: One network-backed `ContinuationValueModel` implementation
  (`NeuralContinuationValueModel`) that produces the per-seat
  per-combo marginal matrix for a river boundary leaf from the public
  board, the ledger, and the exact conditioned leaf ranges exported by
  `export_leaf_continuation_ranges`.
- G2: An offline C++ label generator that solves river subgames with the
  RFC 0039 exact solver at the bucket-gate iteration count and emits
  versioned, provenance-tagged `(features, V_p(c) labels)` records.
- G3: An offline Python training step with declared, pinned
  dependencies, deterministic seeds, a committed data/model manifest
  (feature-scheme id, architecture id, training-set digest, code
  revision, quality metrics), and no presence on any runtime path.
- G4: A standalone `BSNCV01` model artifact (magic/version/digests,
  mmap-friendly, immutable after load) and a hand-written C++23 f32
  feed-forward inference path in `bigshark_solver` with bounded
  allocation and a declared per-call byte/time budget.
- G5: Pre-declared, falsifiable gates (RFC 0031 pattern): held-out
  per-combo marginal accuracy against the exact nested oracle, the
  composed M2 model delta against the full reference, latency, and
  structural invariants, all with multi-seed intervals committed under
  `docs/plans/ablation/`.
- G6: Default-deny host integration behind the gate table; fail-closed
  to the equity frontier on any model decline/invalid state; zero
  observable behavior change for shapes that do not pass.
- G7: Productionize the three components that exist only in the
  benchmark today, with the engine resolver as owner:
  1. the RFC 0036 bounded cap-1 streets(1) turn resolver entry wired in
     the host behind an absent/default-deny gate entry;
  2. a production outer refinement driver that, for each bounded block,
     exports exact conditioned leaf ranges and refines a
     `ContinuationValueModel` within the request wall budget (the
     production counterpart of the benchmark `refine_leaves` loop);
  3. model mount/construction at resolver setup from a digest-pinned
     `BSNCV01` artifact, with certification-scope checks.
  The benchmark nested-M2 arm is generalized to accept an abstract
  `ContinuationValueModel` plus a model-specific gap/certification probe
  so the same composed-M2 evaluation works for exact and neural
  producers (a neural model supplies an offline artifact certificate;
  it has no per-leaf inner tree/policy for the benchmark to walk).
- G8: Predeclare the full non-inference turn cost — the exact
  conditioned-range export per leaf (`export_leaf_continuation_ranges`,
  currently exercised only on 10-hand fixtures) is a joint-deal
  enumeration that grows as the range product; its full-range (1,326
  combos/seat) cost at 2 and 3 seats is measured in Stage 1 and either
  optimized (incremental/memoized path probabilities, cap on enumerated
  joint deals with a sampled fallback estimator) or included in the
  per-decision latency budget before any enablement gate can pass.

## Non-Goals

- No change to the card or action abstractions the policy rows act on
  (`kNSeatCardKind`, cap menus). A learned hand-strength embedding that
  replaces the 9-category postflop buckets is a separate RFC (it changes
  the information partition the trainer and artifacts key on); the
  ~0.26 BB exact-hand card-abstraction floor documented in RFC 0039
  remains RFC 0031 territory and stays on the deployment record.
- No belief network or public-belief-state search (ReBeL): the leaf
  ranges are computed exactly per solve by
  `export_leaf_continuation_ranges`; the network predicts values
  conditional on given ranges, not beliefs.
- No online/continual learning, no network I/O, no GPU requirement at
  inference, no ONNX/libtorch/any external inference runtime.
- No preflop or flop terminal evaluation by this network in this RFC;
  scope is the turn->river boundary (five-card leaf) only.
- No game-platform client, credentials, or live operation; offline
  engine and practice table only.
- No replacement of exact nested solving for offline ground truth; the
  RFC 0039 path remains the label source and measurement oracle.
- No multi-strategy parallel core; the network is a leaf evaluator
  inside the C++ engine, labeled as approximate in solver metadata.

## Current State and Evidence

### Implemented (verified)

- The `ContinuationValueModel` seam
  (`engine/include/bs/continuation_value.hpp:162-252`): `refine(leaf,
  conditioned_ranges, root_support_ranges, budget)`, sweep-safe
  `lookup_scalar(leaf, traverser, own_cards, value_chips)`,
  Solved/Miss/Decline/Invalid statuses, values in integer-f64 chips.
- `NestedRiverValueModel` (`engine/src/gto/nested_river_model.cpp`)
  builds the river-rooted subgame, trains it via `train_nseat`, and
  extracts `V_p(c)` through `extract_root_value`; warm reuse, fail-closed
  gap certification, root-support matrix coverage.
- Trainer dispatch (`engine/src/gto/nseat_trainer.cpp:403-437`) tries
  the continuation model at five-card frontier leaves, swallows all
  exceptions, and falls back to the injected `FrontierEvaluator`
  (equity). The parallel training route has no continuation slot.
- Exact conditioned leaf ranges
  (`engine/src/gto/continuation_range.cpp:232-398`): per-seat
  independently normalized combo distribution on the deterministic path
  to one leaf.
- Measurement harness (`engine/benchmarks/subgame_nash_conv.cpp`):
  `measure_nested_m2` hard-constructs the concrete `NestedRiverValueModel`
  (around line 1797) and uses concrete-only accessors for its gap walk
  (`inner_tree/inner_policy/inner_ranges/record_inner_gap`); the M1/M2
  entry points take a `FrontierEvaluator*` but the composed nested-M2 arm
  does NOT accept an abstract `ContinuationValueModel` — so a neural
  producer needs a small benchmark generalization (see G7). The
  weighted joint NashConv (`compute_nash_conv_weighted`) supports both
  exact-hole and bucket-restricted BR groupings.
- The block policy-iteration outer driver (`refine_leaves` lambda plus
  the block loop) and `export_leaf_continuation_ranges`' only callers are
  benchmark TUs; there is no production outer refinement loop today.
- Artifact precedents: BSF1 frontier table
  (`engine/include/bs/postflop_frontier.hpp:119-147`, 128-byte magic
  header, LE fixed records, SHA-256 via sidecar) and BSWARM03
  (`engine/include/bs/sealed_checkpoint.hpp`: magic, version, exact
  payload length, payload digest, abstraction digests; atomic publish;
  reject old/foreign magic).
- Host loading precedent: digest-pinned mmap libraries mounted
  read-only (`engine/src/resident/mmap_library.cpp`), resident budget
  256 MiB (`resident_policy.hpp:49`), `--verify-binary` opt-in full
  hash.
- The M1/M2 harnesses take a `FrontierEvaluator*`; the composed nested-M2
  arm is hard-typed to `NestedRiverValueModel`, so abstract model
  injection is work added under G7. RFC 0031 established the pre-declared
  interval adoption rule and committed-CSV evidence convention.

### Absent (must be built)

- No first-party neural/inference code anywhere (repo-wide grep: zero
  onnx/tensor/MLP references outside the retired external-client notes
  in gitignored `/local`).
- No Python data-production or training pipeline (only three stdlib
  offline audit scripts under `tools/artifact-audit/`).
- No f32 tensor/inference precedent on the solver decision path and no
  packed f32 weights anywhere; engine decision-path numerics are f64
  (some experimental/offline TUs use single precision, e.g.
  multistreet_cfr, and range tracking does, but no serving policy or
  packed artifact does).
- No byte budget covers model weights or activation scratch.
- Turn serving: `turn_root_unserved` is only the all-miss label; full
  turn->river MCCFR runs when the deadline allows
  (`engine/src/resolver/subgame_resolver.cpp:650-672`), with no
  continuation model constructed on any production path.

## Design Principles

- **Same object, cheaper producer.** The network outputs the exact
  matrix contract RFC 0039 defined; consumers cannot distinguish an
  oracle matrix from a network matrix by interface, only by measured
  quality. No new search semantics.
- **Ground truth stays exact.** Offline labels come from the converged
  RFC 0039 nested solver; the network is never trained on its own
  outputs or on the equity approximation.
- **Fail-closed by construction.** Decline/Invalid/miss/version
  mismatch/out-of-budget falls back to the equity frontier; routing is
  gated on the measured evidence and default-deny, and the runtime
  fallback to the equity frontier is mandatory, so a damaged or
  out-of-scope model degrades to today's behavior rather than failing a
  decision.
- **Offline boundary.** No network I/O, no credentials, no external
  runtime; public cards and hero hole cards only; training never touches
  live state.
- **Evidence over assertion.** Quality is measured against the exact
  oracle with intervals on held-out boards the network never trained on;
  gates are declared before the first training run.
- **One model format, no legacy readers.** First version magic
  `BSNCV01`; any incompatible future format is a new magic and an RFC;
  no dual reader or feature flag.
- **C++ only on the decision path.** Python exists solely as an offline
  training tool and is never required to build or serve the engine.

## Proposed Design

### Data flow

```mermaid
flowchart TD
    subgraph OFFLINE[Offline, never on a decision path]
        A[C++ label generator<br/>benchmark arm --rfc0041-genlabels<br/>exact nested solver at bucket gate] --> B[Versioned records<br/>features + V_p(c) labels<br/>+ manifest + sha256]
        B --> C[Python trainer<br/>pinned deps, deterministic seeds]
        C --> D[Trained f32 weights]
        D --> E[BSNCV01 packer<br/>C++, magic/digests/atomic publish]
        E --> F[model.bsncv + .sha256]
    end
    subgraph ONLINE[Host decision path, C++ only]
        G[Turn request] --> H[cap-1 turn tree<br/>streets(1) shallow MCCFR]
        H -->|river boundary leaf, conditioned ranges| I[NeuralContinuationValueModel<br/>f32 in-process inference]
        F -->|mapped read-only, digest checked at startup| I
        I -->|Solved V_p(c)| H
        I -.->|Miss/Decline/Invalid/budget| J[EquityFrontierEvaluator fallback]
        H --> K[Policy + SolverMetadata<br/>guarantee_level=approximate]
    end
```

### 1. Feature scheme (v1, pinned by digest)

All inputs are derived from public information plus the conditioned
ranges the leaf exporter already computes; hero private cards enter
only through that seat's own range/marginal, exactly as in RFC 0039.

- **Board features (fixed 5-card leaf):** card bitmasks for board;
  rank/suit histograms; evaluator category counts and a bounded set of
  board texture indicators (paired/trip/quota, flush-suit counts,
  straight-window/gap counts, high-card ranks). Deterministic functions
  of the five cards only.
- **Range features per seat:** one f32 weight per canonical combo over
  the 1,326-combo index space (`bs::comboIndex`), zero when absent; the
  vector is the exact normalized conditioned leaf range. A fixed dense
  ordering and normalization (each seat independently sums to 1, as
  exported today) is part of the feature contract.
- **Ledger scalars:** pot, per-seat net contributions, remaining
  stacks, folded flags, button/relative position, live seat count, big
  blind — normalized to pot units or BB (documented per scalar) and
  accompanied by the action-abstraction digest.
- Feature construction is pure, allocation-bounded, and reused-scratch;
  its identity is a declared FNV-1a-64 `feature_scheme_id` stored in the
  artifact and asserted at every load and every `refine()`.

Out of scope in v1: learned hand embeddings, opponent-model history
features, rake/variant-specific inputs.

### 2. Network contract

- Feed-forward dense network with ReLU (final linear output; values in
  chips), architecture hash (layer count, widths, activation set) stored
  in the artifact. Hidden widths are bounded by a declared cap chosen so
  weights + activations fit the model budget (G4); the exact widths are
  fixed in the manifest for v1, not configurable at load.
- Output is the marginal matrix: per seat, one f32 value per supported
  root combo (`ContinuationMatrix`). Sparse support may be encoded with
  the root-support combo list already passed to `refine`; unsupported
  combos are absent (never zero-filled).
- The estimator must satisfy the structural invariants the exact matrix
  has: finiteness everywhere on the declared input domain; distributional
  zero-sum within tolerance (sum over seats of range-probability-weighted
  marginals ~= 0); value bounds inside `[-total_chips_in_play,
  +total_chips_in_play]`. These are checked offline on every validation
  batch AND online on every refine (bounds and the always-on weighted
  zero-sum residual of §3a); there is no optional checking mode.
- Determinism: inference uses no RNG and no concurrency-dependent
  reduction; the same artifact + inputs produce bit-identical f32
  outputs. Training RNG is SplitMix64-equivalent deterministic streams
  with committed master seeds (Python side may use a pinned generator
  implementation reproducing a documented stream).

### 3a. Seam surface changes (additive, fully enumerated)

The abstract `ContinuationValueModel` contract gains a small, explicit,
additive surface; the trainer-side consumer and lookup semantics do not
change:

1. `ContinuationBudget` gains two OPTIONAL fields with safe defaults
   (`max_inference_us_per_refine = 0` meaning "no separate inference
   cap; use wall", `max_model_bytes = 0` meaning "no model-byte charge
   beyond the existing caps"):
   `std::chrono::microseconds max_inference_us_per_refine;` and
   `std::size_t max_model_bytes;`. Existing constructors and the exact
   nested model are unaffected.
2. `ContinuationTermination` gains one enumerator `Inferred` (the exact
   nested values use Complete/WallClock/ResourceLimit only). The single
   switch consumer (`nested_river_model.cpp` result mapping) is updated
   exhaustively; a compile audit verifies no other switch exists, and
   reporting treats `Inferred` as approximate.
3. Certification stays artifact-level for the neural model. The abstract
   seam's per-leaf `record_inner_gap` is a concrete-class-only offline
   driver method and is not part of the model interface; a neural model's
   `refine` serves iff the loaded artifact is certified for the requested
   seat/stack scope AND the per-call checks below pass.

A neural `refine` resolves deterministically, without any per-leaf gap
measurement:

- **Solved**: feature/abstraction/scope identities match; forward pass
  completes inside `wall` and `max_inference_us_per_refine`; every output
  finite; all values within `[-total_chips_in_play, +total_chips_in_play]`;
  root-support coverage filled; and the ALWAYS-ON distributional
  zero-sum residual passes.
- **Invalid**: scope/abstraction/feature-scheme mismatch, malformed
  conditioned ranges (size/non-empty), or an output support/shape
  mismatch.
- **Decline**: wall/inference/byte budget exceeded or the zero-sum
  residual fails.

There is no optional "validation mode". Chip conservation is checked on
every successful refine at near-zero cost: with the conditioned
per-seat range weights already in hand, compute

```text
R = | sum_p sum_c w_p(c) * V_p(c) |     (chips)
```

one dot product per seat plus a sum (the exact model is distributionally
zero-sum by construction; see `continuation_value.hpp` header contract).
`total_pot` is fixed to mean the leaf's pot from the ledger (sum of net
contributions at the boundary, chips) — NOT total chips in play or stack;
the exact numeric `tol_zerosum` is pinned at Stage 2 architecture
finalization (provisional default `R / total_pot <= 1e-3`), after which it
is constant for the v1 model. If exceeded, refine returns Decline. A
learned map that cannot conserve chips is rejected per leaf rather than
accepted offline only; a DeepStack-style explicit zero-sum projection
(center the outputs across seats) is an allowed implementation choice if
raw outputs need it, in
which case the projection is part of the pinned architecture and the
post-projection values are what gets stored and measured.

Lives in `engine/src/gto/neural_continuation_model.cpp` (+ header),
compiled into `bigshark_solver` next to `nested_river_model.cpp`.

- Constructed from a loaded immutable `BSNCV01` artifact and the outer
  turn tree reference (for abstraction digest comparison), mirroring the
  `NestedRiverValueModel` constructor.
- `refine(leaf, conditioned_ranges, root_support_ranges, budget)`:
  builds features for the leaf, runs one forward pass per seat (or one
  batched pass if the bounded implementation supports it), fills
  `ContinuationValues.marginals` over root support, stamps
  `inner_completed_iterations = 0`, `warm = false`, and
  `termination = ContinuationTermination::Inferred` (the additive
  enumerator of §3a; reporting distinguishes exact from inferred
  values). It applies the exact Solved/Invalid/Decline rule of §3a,
  including the always-on zero-sum residual.
- No exception may cross into an MCCFR sweep: internal exceptions map to
  Decline, and the trainer hook's existing try/catch plus equity
  fallback handle the rest.
- `lookup_scalar` reuses the identical fail-closed path as RFC 0039.
  Because a network is a fixed estimator rather than a per-solve gap
  measurement, the model carries an **artifact-level certification**:
  the artifact's manifest includes the offline gate results; the model
  serves only when the loaded artifact is marked certified for the
  requested shape (seats/stack class). Per-leaf `record_inner_gap` is
  not used; the serving predicate is the artifact's certification
  scope, checked at construction.
- Scratch is reusable per instance; weights are mapped read-only; no
  per-leaf heap growth beyond one pre-sized activation buffer.

### 4. Offline label generator (C++)

New benchmark arm `--rfc0041-genlabels` in
`bigshark-subgame-nash-conv.cpp` (or a sibling binary if the driver
grows):

- Samples river-boundary leaves over declared distributions: board
  classes (canonical suit-isomorphism classes reused where applicable),
  betting lines/ledgers reachable in the cap-1 turn tree, stack/SPR
  classes, 2 seats first, 3 seats when the 2-seat model passes.
- For each leaf: exports exact conditioned ranges, solves the river
  subgame with `NestedRiverValueModel`/`train_nseat` to the RFC 0039
  bucket-gate convergence (or a pre-declared tighter offline iteration
  budget with the measured residual recorded), and records features plus
  the exact `V_p(c)` labels and the solver's quality metadata (seed,
  iterations, bucket gap, exact gap).
- Train/validation split is by CANONICAL BOARD CLASS, not by record, so
  held-out validation tests unseen boards (prevents memorization of the
  2,598 flop classes' river descendants).
- Emits binary/JSONL shards with a manifest: format version, feature
  scheme id, generator code revision, seed, record counts, split digest,
  per-shard SHA-256. Raw shards are gitignored; the manifest and
  aggregate statistics are committed.

### 5. Offline trainer (Python)

New `tools/neural-cv/` (offline only; not built by CMake, not installed
on any runtime path):

- `requirements.txt` with hash-pinned versions (`--require-hashes`
  discipline); stdlib + one declared tensor framework (PyTorch CPU) is the
  initial choice, documented as offline-only. This is the project's first
  such dependency, so its boundary is written down in the tool README:
  the directory is excluded from CMake, ctest, and npm CI (added to no
  build graph), is never installed into the host or practice binaries,
  and a framework swap or major version change forces a new
  feature_scheme/architecture review and a fresh manifest.
- Loads the manifest-verified shards, trains the v1 architecture under a
  committed hyperparameter file, checkpoints only for resumability,
  exports raw weights in a fixed framework-neutral binary form the C++
  packer reads (the tensor order of §6).
- Emits a training report JSON: seeds, split sizes, train/validation
  loss curves, held-out per-combo quantile errors, zero-sum residual,
  framework/library version, commit hash of label data and code;
  validation metrics are reproducible from the manifest + code revision
  within AC3 tolerances.
- No network calls: dependencies vendored or fetched by explicit
  developer action into a documented location; no telemetry, no remote
  dataset access.

### 6. `BSNCV01` artifact (C++ codec)

Standalone codec in `engine/src/gto/` + `engine/include/bs/` (new
`neural_cv_codec` TU in `bigshark_solver`):

All integer fields are little-endian natural alignment; the fixed header
is exactly 128 bytes with every byte covered by a named field; the sole
explicit reserved region is the `reserved2` u32 at offset 124, which must
be zero and is checked by the reader. The 7-character magic is
NUL-terminated inside `char[8]` (`"BSNCV01\0"`). Byte-offset table:

```text
offset size field
0      8    char[8]  magic = {'B','S','N','C','V','0','1',0}
8      4    u32      version = 1
12     4    u32      header_len = 128
16     1    u8       board_size = 5
17     1    u8       dtype = 1 (f32 LE throughout; no mixed precision)
18     1    u8       n_layers (dense weight matrices; biases see below)
19     1    u8       activation = 1 (ReLU; terminal layer always linear)
20     8    u64      feature_scheme_id (FNV-1a-64, pinned in feature code)
28     8    u64      architecture_id (FNV-1a-64 over the exact ordered
                    sequence of layer widths AND activations)
36     8    u64      action_abstraction_digest (0 = scope does not bind one;
                    a river-value model normally binds the cap-1 digest)
44     8    u64      card_abstraction_digest (0 = not bound; for this RFC
                    the model acts under kNSeatCardKind and binds it)
52     8    u64      numeric_profile_id (chips/BB profile, nonzero)
60     8    u64      scope_digest (FNV-1a-64 over the SORTED certified
                    scope table, see "Certification scope" below)
68     8    u64      training_manifest_digest (sha256-derived 64-bit tag of
                    the training manifest: data-split digest, generator
                    code revision, label scheme, training report incl.
                    gate results; full 256-bit sha lives in the sidecar)
76     4    u32      layer_dims[n_layers+1] FOLLOW INLINE in the payload,
                    not the header; header stores u32 io_width[2]:
76     4    u32      input_dim
80     4    u32      output_dim (= seats_block * 1326 for this v1)
84     4    u32      weight_bytes (exact blob length)
88     4    u32      activation_scratch_bytes (largest layer width, f32)
92     32   u8[32]   payload_sha256 (over the complete weight blob)
124    4    u32      reserved2 (must be 0)
128         --      end of header (payload begins, 4096-aligned on disk)
```

Weight blob serialization (the contract the Python exporter and C++
reader share; architecture_id is a checksum over this exact order, not a
substitute for it):

```text
for layer k in 0 .. n_layers-1, in order:
  u32 rows_k, cols_k           # W_k shape: y[row] = sum_c W[row,c] x[c] + b
  f32[rows_k * cols_k] W_k     # row-major, IEEE-754 LE; rows_k = dims[k+1],
                               # cols_k = dims[k]; contiguous, no padding
  f32[rows_k] b_k              # bias vector, immediately after its W
```

- The layer dimension array is reconstructed by the reader from
  `input_dim`, the per-layer `rows_k/cols_k`, and `output_dim`; any
  inconsistency with architecture_id or between adjacent layers
  (cols_{k+1} != rows_k) rejects the file.
- On-disk layout: the 128-byte header is followed by zero-filled padding
  up to a 4096-byte boundary, then the weight blob at that aligned offset
  (and any further inter-/post-blob padding is likewise required to be
  zero; a nonzero padding byte rejects the file). `payload_sha256` spans
  ONLY the weight blob beginning at its 4096-aligned offset for its
  declared `weight_bytes` length (not the padding); the `.sha256` sidecar
  covers the ENTIRE file including header and zero padding.
- The reader rejects: wrong/old magic or trailing NUL mismatch,
  version != 1, header_len != 128, nonzero `reserved2` or any nonzero
  padding byte, any board/dtype/activation value outside the enum,
  truncation (file shorter than aligned offset + declared weight_bytes or
  blob misalignment), payload sha256 mismatch or whole-file sidecar
  mismatch, any non-finite weight or |w| above a declared sanitizer bound
  (the training report pins the bound; weights outside reject), shape
  inconsistent with architecture_id, abstraction/profile digest mismatch
  against the turn tree, or scope_digest mismatch against the requested
  shape. u64 digest fields use 0 ONLY for fields whose semantics above
  explicitly allow it (action/card, with their intended binding stated
  per artifact); feature/architecture/numeric/scope/manifest are required
  nonzero.

Certification scope: a separate small JSON table (committed beside the
artifact and listed in the training manifest) enumerates certified
classes as rows `{seats, stack_bb_class, spr_class, board_scope,
line_scope}`; `scope_digest` is FNV-1a-64 over the canonical (sorted,
newline-joined) serialization. A request outside any row is out of scope
=> construction/serve declines. v1 scopes are restricted to
`board_scope="river-from-turn-streets1"`, `line_scope="cap1-all-lines"`
(bet/raised lines must be present — checked-through-only is rejected at
packing), and seats in {2} initially; 3 requires its own certified table.

- Publish is temp-file + fsync + atomic rename + directory fsync, with a
  `<file>.sha256` sidecar carrying the full-file SHA-256 and the
  256-bit training-manifest digest (BSWARM/BSFLIB discipline).
- Load is read-only mmap or a bounded malloc in constrained
  environments; the loaded object is immutable and const-thread-safe.
- The artifact is NOT a SQLite strategy artifact and does not enter that
  schema.

### 7. Search and budget placement

- The outer solve remains the bounded cap-1 turn tree with streets(1);
  the network replaces the ENTIRE river subtree at each frontier leaf,
  which is the dominant pruning win (the river subtree is what RFC 0039
  timed at ~minutes-to-hours).
- A declared model budget extends `ContinuationBudget`:
  `max_inference_us_per_refine` and `max_model_bytes`. The v1 predeclared
  ceiling is `max_model_bytes <= 16 MiB` (weights + largest activation
  buffer, measured after Stage 2 architecture pinning and recorded in the
  artifact header; this sits inside the 256 MiB resident budget with room
  for libraries) and `max_inference_us_per_refine` chosen in Stage 3 from
  the call-count arithmetic of AC5 (it bounds ONE refine, not a decision).
  Exceeding either is Decline. The actual widths are pinned before the
  gated training run and must fit this ceiling; a design needing more
  requires an RFC amendment, not a silent budget bump.
- Model bytes are charged to a new explicit category reported alongside
  the resident/table budgets, never silently added to the 1 GiB tree
  cap.

### 8. Host integration and metadata

- The model is mounted like a digest-pinned library; a gate-table entry
  per shape (initially all absent/default-deny) decides whether the host
  constructs `NeuralContinuationValueModel` instead of leaving the
  continuation slot null.
- Fallback order: certified network leaf -> equity frontier -> existing
  resolver fallback labels; every fallback path and its frequency is
  observable in metadata.
- SolverMetadata for network-served decisions: `source`/`guarantee_level`
  map to approximate (never `exact_solved`); `artifact_sha256` carries
  the model digest; `reason_code` distinguishes model-served from
  model-declined; no new proto fields are required (existing
  `SolverMetadata` fields are used).

## Dependency Rules

- New code: `engine/include/bs/neural_continuation_model.hpp`,
  `engine/include/bs/neural_cv_codec.hpp`, corresponding
  `engine/src/gto/` TUs in `bigshark_solver`; benchmark arm;
  `tools/neural-cv/` Python (offline).
- The inference TUs may include eval/abstraction/continuation headers and
  the codec; they must not include service/protocol headers, network
  headers, Python, or the training framework. The stage-6 offline guard
  is extended to forbid the inference path depending on behavior/service
  layers beyond what the solver already does.
- `bigshark_solver` gains no new external link dependency; f32 inference
  uses compiler builtins/standard library only (SIMD may be added later
  behind the same architecture id; v1 is portable scalar f32).
- Python tooling depends on nothing in the engine build; the engine build
  depends on nothing Python.
- The model object depends on immutable weights; nothing the host does
  can mutate an artifact.

## Compatibility and Migration

- No wire, persisted-policy, or protocol change; `SolverMetadata` fields
  already exist.
- Current behavior (null continuation, equity frontier / full solve when
  the deadline allows) is unchanged until a gate-table entry enables a
  certified model for a shape.
- Migration stages are independently reversible: removing the mounted
  model or its gate-table entry restores today's path byte-for-byte at
  the receipt level except metadata source labels.
- Label shard and model formats are versioned independently with their
  own magics/ids; incompatible training data requires a new generator
  version, incompatible weights a new magic.

## Security and Operational Impact

- Offline boundary preserved: inference reads only public board,
  ledger, and conditioned ranges derived from declared root ranges;
  hero private cards enter via the same per-seat mechanism as exact
  solving; no unrevealed-card inference is added.
- Artifact integrity: SHA-256 payload + sidecar, digest check at mount,
  optional full verify; a corrupt/tampered model fails closed to equity.
- Resource limits: bounded weights, pre-sized activations, per-call
  wall budget, explicit model byte budget; a model that cannot meet the
  budget for a shape is not certified for it.
- Fairness: deterministic inference; model digests appear in receipts so
  decisions are reproducible/auditable; the model cannot condition on
  opponent identity or history beyond what ranges encode.
- No credentials, no network, no telemetry.

## Alternatives Considered

1. **Persisted exact CV table (RFC 0039 Stage 3).** Offline batch solves
   packed for reuse. Exactly correct (within the abstraction), but
   storage grows with board class x ledger x stack x range combinations
   and 3+ seats has 1,344 leaves/solve with days of recomputation; a
   table does not generalize to unseen boards. Kept as the designated
   fallback/supplement: a packed table can serve the commonest 2-seat
   spots while the network covers the long tail; Stage 3 needs its own
   RFC regardless.
2. **ReBeL-style belief-state value network.** More general (learns the
   belief distribution too), but the ranges at the leaf are already
   computed exactly here, so it pays for a belief net and recursive
   public-state search complexity the problem does not need; ReBeL was
   only validated on small games.
3. **Keep solving online with more compute / better hardware.**
   87x-per-leaf miss and days-per-solve at 3 seats are not closable by
   cores alone under a 1,600 ms receipt-to-action budget; also unusable
   for the offline practice table's bulk hands.
4. **Equity frontier plus richer rollout (betting-aware heuristic).**
   This is a heuristic leaf core of the kind the project bars; RFC 0036
   measured it failing (0.402/6.161 BB), and it has no learning path.
5. **Pluribus-style supercomputed blueprint only.** Proven at six seats
   but is exactly the Stage 3 table approach at a scale this project
   cannot run per board class, and it does not solve turn leaf values
   generically.
6. **External ONNX/libtorch runtime.** Rejected: new heavy runtime
   dependency, supply-chain surface, and startup cost, for a network
   small enough for a fixed scalar f32 evaluator.

## Risks

- **The network does not reach the frozen M2 band (principal risk).**
  Mitigation: pre-declared gates and honest failure recording; the RFC
  is allowed to end with "infeasible at v1" and the engine unchanged,
  as RFC 0039 did. DeepStack's published result is evidence the target
  is plausible heads-up, not a guarantee.
- **Generalization to unseen board textures/ledgers.** Mitigation:
  board-class-disjoint validation split, held-out stack/SPR classes,
  quantile error gates; scope certification per seat/stack class.
- **f32 vs f64 numeric drift.** Mitigation: labels/contract values
  documented in chips, f32 error included in the per-combo gate, bitwise
  determinism asserted, f64 reference comparisons in tests.
- **Training pipeline provenance/bit-rot.** Mitigation: manifest with
  data digest, code revision, pinned hashed requirements, reproducible
  seeds; the trained artifact cannot be regenerated silently.
- **Silent fallback storms at runtime.** Mitigation: serve/decline
  counters in metadata and an offline envelope run reporting fallback
  frequency; a model that declines often is a failed deployment even
  when each fallback is safe.
- **Multiway equilibrium ambiguity (3+ seats).** No zero-sum theorem;
  quality established empirically by the same frozen gates, and 3-seat
  certification is a separate decision after 2-seat evidence.
- **False precision in receipts.** A network is approximate; metadata
  must never claim exact solving, and the RFC 0039 exact/bucket quality
  numbers stay on record.
- **Scope creep toward a learned card abstraction.** Explicitly a
  separate RFC; v1 outputs values under the existing abstraction.

## Verification Plan

- Unit/native tests: feature construction determinism and normalization;
  codec round-trip, rejection of every corruption/truncation/digest
  mismatch/scope mismatch; inference bitwise determinism and
  allocation-bounded scratch; structural invariants (finite, bounded,
  zero-sum residual tolerance) on synthetic weights;
  Solved/Miss/Decline/Invalid and exception-to-fallback paths; correct
  sparse combo coverage (zero-mass combos absent).
- Exact-oracle conformance: on pinned fixtures, network marginals vs
  exact nested solver marginals, per-combo quantile errors with
  three-seed intervals, board-class-disjoint.
- Composed M2: after the G7 benchmark generalization lands, evaluate the
  neural model against the full reference alongside the equity frontier
  and exact nested composed policies, 2 seats, seeds 101/202/303 (and
  3 seats when attempted), with the AC4 strict interval protocol and
  production-semantics mixed arm; CSV under
  `docs/plans/ablation/rfc0041-*.csv`.
- Latency: host timing harness `solve_time_us` distribution on the
  envelope; inference-only microbenchmark (per-refine mean/p95/p99),
  fallback frequency; practice-table `--hands N` smoke runs.
- Full Engine Build Contract: release + asan presets, full ctest,
  format, benchmark-multistreet, npm checks, proto check; the Python
  tool has its own pinned lockfile and a documented reproduction command.
- Evidence is measured, never asserted zero.

## Rollout Plan

Sequencing note: Stage 1 entry depends on RFC 0039 E4's D1 bucket-predicate
switch in the benchmark and the converged 100 BB per-combo oracle row (they
share `subgame_nash_conv.cpp`); this RFC's label generation starts from that
base rather than forking the driver.

1. **Generator and labels (no serving).** Before generation, commit the
   fixed label-cell plan (RFC 0031 discipline): target board-class count,
   line quotas incl. bet/raised lines, stack/SPR grid, records per class,
   seeds, and the per-class compute/storage budget, with the measured
   138.6 s/checked-through-leaf cost and the harder bet lines as the
   planning anchor. `--rfc0041-genlabels` for 2 seats; manifest + label
   statistics committed; per-combo label noise and bucket/exact gaps
   recorded per shard. Shard retention location is fixed in the manifest
   (gitignored bulk storage, committed manifests). Stop: reproducible
   labels whose measured per-combo noise is <= 0.003 BB.
2. **Trainer and codec/inference (offline only).** Python trainer under
   `tools/neural-cv/` + `BSNCV01` packer/reader + C++ scalar f32
   inference; ctests; determinism. Architecture widths and the byte
   budget are pinned here before any gated run. Stop: a reproducible
   artifact round-trips and passes structural ctests with no host change.
3. **Offline gates.** Held-out marginal accuracy (AC4 statistics),
   production-semantics composed M2 with the exact-hand D1 column, and
   the AC5 aggregate latency/full-range-export/serve-rate cells.
   Decision point against the predeclared gates. Stop: either documented
   pass or documented failure (RFC closes, engine unchanged).
4. **Default-deny host instrumentation and shadow.** Gate-table plumbing
   with no enabled shape; a SHADOW run on the offline practice table
   (`bigshark-practice --hands N`, N >= 10,000 decisions predeclared) that
   computes what the model would serve without changing actions, and
   records serve rate, declines, per-share timings, and zero-sum
   residuals. Stop: zero observable behavior change AND shadow metrics
   consistent with the Stage 3 sample.
5. **Conditional enablement.** Enable exactly the seat/stack classes
   whose evidence passes, each with its own CSV row and digest, after a
   fresh independent review confirmation that the gates are met for that
   class. 3+ seats only after their own evidence and review.

## Rollback Plan

Rollback removes the NEW code this RFC adds: the model gate-table entry,
the mounted `BSNCV01` artifact, the bounded-turn entry, and the
production outer refinement driver (none of which exists today — the
continuation slot is currently null in production, so the "optional seam
argument already present" framing applies to the trainer signature only,
not to any wired host path). With those removed the resolver resumes
today's exact continuation-null behavior. Rollback needs no data
migration and changes no wire format; artifacts are immutable files
deleted independently. Because every enabled stage is reversible on its
own, rollback can be per-shape.

## Open Questions

- Exact hidden-layer widths (the byte ceiling is fixed at <= 16 MiB;
  widths are chosen in Stage 2 from the latency/accuracy frontier and
  pinned in the architecture id before the gated training run).
- Whether one model covers 2 and 3 seats via a seat-dimension input or
  seat-count-specific artifacts are trained; default assumption is
  separate certification scopes, 2 seats first.
- Final framework version pin (PyTorch CPU initially) and the bulk shard
  storage path, resolved in Stage 1 within the committed label-cell plan.
- The stack/SPR class grid the 2-seat certification covers, declared in
  the Stage 1 scope table.
- None of these block acceptance of the RFC; they gate individual
  rollout stages.

## Acceptance Criteria

- AC1: `NeuralContinuationValueModel` implements `ContinuationValueModel`
  with the additive §3a surface only; native ctests cover
  Solved/Miss/Decline/Invalid, sparse coverage, exception-to-fallback,
  finiteness, bounds, the always-on zero-sum residual and its Decline
  path, the `Inferred` termination, and bitwise deterministic inference.
- AC2: `BSNCV01` codec round-trips and rejects corrupt/truncated/
  wrong-magic/wrong-version/digest/abstraction/scope mismatches in
  ctests; artifact is immutable, mmap/read-only loadable, with sidecar
  digest; no SQLite/wire change.
- AC3: The offline C++ generator emits provenance-tagged,
  board-class-disjoint label shards from the exact RFC 0039 solver; the
  Python trainer reproduces the shipped artifact's validation metrics
  within predeclared tolerances from the manifest + hash-pinned
  requirements + committed seeds (bitwise cross-platform weight
  reproduction is NOT required and not claimed — pinned PyTorch CPU is
  deterministic per environment; C++ inference bitwise determinism is
  what AC1 pins). The training report records code/data revisions,
  framework version, seeds, and quality metrics.
- AC4 (quality gates; statistical protocol declared here BEFORE the
  first gated training run and committed to the CSV header):
  - **Independent unit and weighting.** Combos sharing a board are
    correlated and are not independent samples. The resampling unit is
    the held-out CANONICAL BOARD CLASS crossed with training seed
    (>= 60 disjoint held-out classes per gate run, >= 3 seeds). For one
    (class, seed), the statistic is computed over its seat/combo/leaf
    records with each combo weighted by its conditioned leaf range
    probability mass (range-mass-weighted error is primary; a
    uniform-per-combo number is reported alongside); bootstrap
    resampling is then over (class, seed) units (10k replicates) to get a
    95% t/percentile interval. A per-combo pooled interval is also shown
    but never used for the gate.
  - **Per-combo gate:** the (class,seed)-bootstrap 95% UPPER bound of
    the range-mass-weighted mean |network - exact oracle| marginal
    error <= 0.01 BB/combo, AND the mass-weighted 95th-percentile error
    point estimate <= 0.05 BB.
  - **Label-noise floor.** Oracle labels are MCCFR outputs; their own
    per-combo noise must be materially below the 0.01 BB gate. Labels
    are generated at a predeclared scheme (fixed high iteration count
    plus >= 3 independent-seed averaged solutions per leaf class) whose
    measured per-combo disagreement (max seed-pair spread / 2) is
    <= 0.003 BB on the held-out classes — one third of the gate, matching
    the RFC 0031 minimum-effect discipline. Bet/raised-line leaves are
    mandatory in the label distribution (checked-through leaves alone
    cannot certify; the M2 driver measured 37-46 BB exact worst-leaf gaps
    at 40k inner), with predeclared class/line quotas in Stage 1.
  - **Composed-M2 gate (strict interval orientation, RFC 0031 rule).**
    Neural and equity-frontier composed policies are each evaluated
    against the SAME full reference on seeds 101/202/303; adopt only if
    the neural model-delta's whole 95% interval lies strictly below the
    equity delta's: `hi(neural) < lo(equity)`, AND improvement
    `mean(equity) - mean(neural) >= max(0.01 BB, (1/3)*|mean(equity)|)`,
    AND `hi(neural) < 0.03 BB` summed. Point estimates do not suffice;
    intervals overlapping => recorded Inconclusive (a valid outcome).
  - **D1 exact-hand deployment column (mandatory).** Because the network
    acts through the same 9-bucket policy rows it cannot remove the
    ~0.26 BB exact-hand floor; every composed-M2 row additionally
    reports the EXACT-hole pooled-BR NashConv (the arm already computes
    it), and the CSV cross-references RFC 0031 as the owner of that
    floor. Enablement requires the exact number to be recorded and
    interval-defined as no worse than the RFC 0039 measured baseline for
    the same shape: the Stage 3 CSV header pins the exact-hole columns
    (neural exact-hole composed NC with its (class,seed) upper CI, the
    RFC 0039 baseline exact-hole NC and its pinned 0.259-0.295 BB leaf
    floor / reference-NC columns), and the rule is
    `lo(neural_exact) <= hi(baseline_exact)` on the composed policy — the
    network may not hide or increase real-game exploitability outside
    interval uncertainty.
  - **Production-semantics arm.** The composed-M2 evaluation runs with
    artifact certification/decline semantics exactly as production will
    (certified leaves served by the network, declined leaves filled by
    the equity frontier, mixed), never an all-leaves force-serve run;
    serve/decline decisions are logged per leaf.
  - If any gate is not met the result is recorded as failure/inconclusive
    in `docs/plans/ablation/rfc0041-m2.csv` and no shape is enabled.
- AC5 (latency/budget; aggregate protocol declared before measurement):
  - Reference hardware is named in the CSV header (the project's
    28-core release-build box class used by RFC 0039, with core count and
    CPU model recorded); timings are release build, >= 200 sampled turn
    decisions per cell, reporting mean/p50/p95/p99.
  - **The binding number is whole-decision cost**, not one refine:
    `T_decision = range_export_total + feature_build_total +
    inference_total + outer_mccfr`, summed over every block refinement
    of every reachable leaf (240/1,344 at 2/3 seats in the measured
    cadence, with the block count fixed in the cell). The CSV breaks
    these four shares out separately. p95 `T_decision` <= 1,600 ms.
  - A separate predeclared inference subtotal: p95 of
    `inference_total + feature_build_total` <= 400 ms at 2 seats
    (leaves x blocks, not per call); the per-call mean is reported for
    scaling but is not the gate.
  - The non-inference range-export share is measured on the FULL
    1,326-combo ranges (G8); if the exact joint enumeration breaches the
    envelope, an optimized/memoized exporter (or a declared bounded
    sampled estimator with its own CI) must land and be measured before
    enablement — the network cannot be certified on a 10-hand fixture
    cost.
  - Model bytes <= 16 MiB (§7).
  - **Serve-rate gate:** on the same >= 200-decision sample, the fraction
    of reachable leaves the certified artifact serves (Solved vs
    Decline/Invalid) must be >= 99% for the certified seat/stack scope;
    below that the shape is not enabled even if served leaves are
    accurate (decline storms are a failed deployment, not just a
    reportable metric).
- AC6: default-deny integration preserves `turn_root_unserved` /
  fallback receipts for every non-enabled shape; network-served
  decisions carry `SolverSource.SOLVER_SOURCE_RESOLVING`,
  `guarantee_level = approximate` (never exact_solved),
  `artifact_sha256` = the model digest, and fixed <=64-char
  `reason_code` tokens `neural_cv_served` / `neural_cv_declined`;
  conformance tests pin the unchanged non-enabled behavior; no new proto
  fields.
- AC7: full Engine Build Contract passes (release + asan on touched
  surfaces, format, full ctest, benchmark-multistreet, npm check, proto
  check); raw rows committed under `docs/plans/ablation/` with versioned
  headers, seeds, and CI declarations; documentation updated in the same
  change (design docs, README indexes, RFC 0036/0039 cross-references).

## Decision

Author agent: Claude (zzzode agent session, RFC author 2026-10-28)
Approved by: rfc0041-approval-1 (independent approval agent, auto_model/alwaysday1_max; did not author the RFC, code, or measurements)
Decision date: 2026-10-28
Review outcome: Approved
Amendment round: one Changes Requested round (five blocking findings); all five resolved and re-verified, and six editorial errata (NB1-NB6) applied in this acceptance commit
Review summary: Round 1 returned Changes Requested — Draft/Proposed status mismatch; overstated reuse of a production serving path that does not exist; an "unchanged seam" claim contradicted by budget/enum/certification edits and a nonexistent validation mode; gates that were not yet falsifiable (pseudo-replicated CIs, point-estimate comparisons, a missing mandatory exact-hand D1 column, an undefined latency aggregate and byte ceiling, unmeasured full-range export cost, no label-noise floor or serve-rate gate); and a BSNCV01 header sketch that could not be independently implemented. The revision: corrects the status and names the engine resolver plus the three components that must be built (bounded turn entry, production block refinement driver, model mount); adds §3a enumerating the additive seam surface (two defaulted budget fields, one Inferred termination enumerator, artifact-scope certification, deterministic Solved/Invalid/Decline rule, always-on per-refine weighted zero-sum check with Decline); defines AC4 as a complete predeclared statistical protocol ((board class x seed) resampling unit, >=60 held-out classes, >=3 seeds, range-mass weighting/bootstrap, 0.01 BB mean CI + 0.05 BB p95, <=0.003 BB label-noise floor with mandatory bet lines, strict RFC 0031 whole-interval composed-M2 rule, mandatory exact-hole D1 column, production-semantics mixed arm, 99% serve-rate); defines AC5 as the whole-decision aggregate latency over leaves x blocks (p95 <= 1,600 ms, named 28-core reference hardware, >=200 decisions, inference+features <=400 ms, full-range export measured before enablement, model <=16 MiB); and specifies BSNCV01 as a complete 128-byte offset table with explicit f32 row-major tensor layout, a digest-bound certification scope table, and a training-manifest digest. Round 2 verified every change against source and the CSV evidence, independently re-summed the header to 128 bytes, re-ran check-rfcs (41) and check-docs (392), and confirmed the enum has exactly one switch consumer; verdict Approved with no remaining blocking findings.

Reviewed scope: full revised RFC 0041 (911+ lines), the RFC 0039 parent/amended gate semantics and Stage 4 trigger, the ContinuationValueModel seam and trainer hook, artifact precedents (BSF1/BSWARM03/BSFLIB03), the subgame_nash_conv M2 harnesses, RFC 0031's adoption-rule section, CMake/stage6 dependency boundaries (grep-verified zero existing neural code and no Python data pipeline), the host/envelope/proto receipt surfaces, and both index rows.

Approved scope: the offline C++ label generator, the offline-only Python trainer under tools/neural-cv/, the BSNCV01 codec, the in-process f32 NeuralContinuationValueModel in bigshark_solver, the bounded turn entry and production outer refinement driver behind a default-deny gate table, the model mount/digest machinery, the predeclared AC4/AC5 gates, and the staged rollout. The approval does not cover any actual enablement (each seat/stack class needs its own evidence CSV and a fresh independent review confirmation), persistence beyond the model artifact itself, any wire/proto change, live operation, credentials, or spending. Conditions before Implementing: produce the implementation plan; Stage 1 label generation starts only after RFC 0039 E4's bucket-predicate switch and the converged 100 BB per-combo oracle row land; gates are frozen in CSV headers before the first gated training run; failure or Inconclusive is recorded and leaves the engine unchanged; the full Engine Build Contract is required before Implemented.

Residual risks accepted: the label campaign's compute economics at bet/raised lines (anchored on 138.6 s per checked-through leaf), whether a <=16 MiB MLP reaches 0.01 BB on board-class-disjoint data, the unmeasured full-range joint-deal export cost gated by G8/AC5, and the absence of a multiplayer zero-sum theorem at 3+ seats (empirical gates only, 3-seat certification is a separate decision). Each is a predeclared honest-failure escape hatch.
