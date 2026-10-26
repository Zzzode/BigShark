# RFC 0035 Implementation Plan — Multi-Root Flop Library (BSFLIB03)

Status: Stages 1–3 implemented (2026-10-25); stages 4–5 remain gated
Created: 2026-10-24
Owner: Engine flop library, engine resident, engine host
RFC: [0035](../rfcs/0035-multi-root-flop-library.md) (Accepted 2026-10-24)

## Implementation status (2026-10-25)

Stages 1–3 are complete and verified under the full Engine Build Contract
(76/76 ctest, `npm run check`, `npm run proto:check`, `node bin/replay.mjs`,
check-docs/check-rfcs green):

- **BSFLIB03 codec** (`engine/src/resident/mmap_library.{cpp,hpp}`): magic
  bump with no BSFLIB02 reader; fixed header with the full RFC 0030 action
  identity and strict digest/cap-token validation (a recognized tokenless
  line fails closed); dense `index[root_count][1755]`; 44-byte logical class
  header padded to 48 with a leading root_id; duplicate-ledger, truncation,
  out-of-file/misaligned-index, misindexed-section, and bad-header-padding
  refusals all covered by `test_mmap_library`.
- **Builder** (`engine/src/flop_library/`): `BuildParams.roots` declares the
  ordered root table; per-(root,class) sections and checkpoints stride
  independently (`root-NNNN-class-NNNN.bsf`); ordered FNV-1a root-table hash
  in `build.json`; BSWARM03 checkpoints (200-byte header) add the root-table
  witness and refuse a mismatched table by name; reserved range profile 2 is
  refused; `served_roots[]` replaces the single `served_root` manifest block.
- **Resident/host/protocol**: one record per (root, present class); exact
  ledger match before board lookup; `ClassNotTrainedForRoot` threaded through
  the resident enum/to_string, `BlueprintMiss`, `frame_stream`, the envelope
  (mapped to `ERROR_CODE_INCOMPATIBLE_ROOT`), and the host mapMiss; the host
  prints one `resident-served-root` line per directory root and reconciles
  the manifest against the mounted directory. Two-root
  ResidentPolicySet round-trip coverage lives in `test_resident_policy`;
  builder multi-root/resume coverage in `test_flop_library` and
  `test_flop_library_parallel`; host report/reconcile coverage in
  `test_flop_library_loading`.

Stages 4 (session histogram instrumentation) and 5 (the first real gated
multi-root BSFLIB03 build) are unchanged and remain gated on measurement and
compute authorization; no artifact was trained or published by this work. The
shipped one-root artifact on this branch is BSFLIB03 and verified reachable.

## Intended outcome

One packed library file answers several exact preflop-line ledgers through a
BSFLIB03 root directory; live flop decisions resolve exact ledgers without
quantization. No artifact is trained or published under BSFLIB03 until the
measurement gate and compute authorization are satisfied.

## Owning module

`engine/src/flop_library/` (build identity, per-(root,class) stride/resume),
`engine/src/resident/mmap_library.{cpp,hpp}` (BSFLIB03 codec; it owns the
format), `engine/src/resident/resident_policy.cpp` (exact root matching and
the new miss code), `apps/engine-host/main.cpp` (multi-root report).

## Stages and gates

1. **Format: BSFLIB03 codec in mmap code (no artifact, no production
   artifact reader changes beyond what the header requires).** Magic
   `BSFLIB03`; fixed file header retains player count, big blind, button,
   the two abstraction digests, codebook layout, cap, and card bucket kind,
   and gains the full action identity (name/version/canonical
   parameters/`|max-raises=` token/digest) the reader assigns to
   `game.action_id`; root directory (`root_count`, one `root[]` per trained
   root with line tag/pot/contributions/folded/live count/range profile,
   dense `index[root_count][1755]` by global class id, zero absent);
   class header 40 -> 44 padded to the 8-byte section boundary (48
   effective) with `root_id` cross-checked against the addressing index.
   BSFLIB02 is not read after the bump; duplicate root ledgers fail at
   open as mount errors.
2. **Builder/residency: per-(root,class) stride and resume.**
   `BuildParams` gains the declared root table; `build.json` records the
   ordered root-table hash; `.bsf` sections stride independently; the
   sealed checkpoint already carries the tree witness and card kind, and
   gains/refuses any root-table mismatch; the manifest gains
   `served_roots[]`.
3. **Resident and host.** `MmapLibrary` exposes root/count/index;
   `ResidentPolicySet::build` serves two `SupportedRootSpec` from one file;
   `resolve_root` resolves an exact ledger before a board lookup; the host
   prints one `resident-served-root` line per root; capabilities do not
   enumerate roots. One new miss code, `ClassNotTrainedForRoot`, mirrors
   through all six sites and maps to `ERROR_CODE_INCOMPATIBLE_ROOT`;
   `RootNotSupported` stays the existing code.
4. **Measurement instrumentation (instrument landed 2026-10-25; awaiting
   corpus).** `platforms/river-club/src/root-histogram.ts` plus
   `node bin/root-histogram.mjs` read the `sessions/` journals, keep one flop
   street-start snapshot per hand, and build the EXACT full-ledger tuple
   (player count, big blind, pot, SB-first per-position net/stack/folded
   mask) through the production `toDecisionRequest` mapper — the same
   identity `same_root` compares. It emits family and
   `(live,SB-live,BB-live)` rollups, distinct-tuple shares, stack dispersion
   and open sizes in BB, and counts amount-incomplete snapshots as
   `ledgerIncomplete` by reason instead of fabricating a key. Eight tests
   pin it (`root-histogram.test.ts`). On the current empty corpus it reports
   zero mappable rows and states the gate is unmet. The threshold (share and
   absolute minimum, 95% CI) is pre-declared from the REAL histogram once a
   live corpus exists; if no finite exact-root set reaches it, no K-root
   build runs and a separate quantization RFC is opened.
5. **First real BSFLIB03 build — gated.** Only after stage 4's real-corpus
   histogram and an explicit compute authorization; the phase-1 set always
   includes the limped six-live root and is published only with measured
   cost and per-root coverage.

## Verification

- Mmap round-trips: BSFLIB03 magic/header/directory; bad and old magic,
  truncation, header/directory/section truncation, duplicate root ledger,
  index offset outside the file, section root_id disagreeing with the
  index path, and non-8-byte-aligned padded class headers all refuse at
  open.
- Builder: two-root synthetic library round-trips exact hits for each
  ledger, non-matching ledgers and wrong-pot trained-root misses with the
  correct reason; build-identity root-table hash rejects a different root
  set; per-(root,class) resume independent.
- Host and resident: `ResidentPolicySet::build` two-root round-trip;
  `served_roots[]` reconciles; one startup report line per root.
- Cap-gap continuation test: a cap-1 BSFLIB03 file reconstructs cap 1.
- Engine Build Contract and ASan pass; BSFLIB02 has no reader in the tree.

## Rollback

Stages 1-3 are source changes behind a format bump with no published
artifact; revert is source-only. Stage 5 keeps the BSFLIB02 binary until
the BSFLIB03 build is verified. No wire-protocol, persisted-wire, or
established external contract changes.

## Explicitly out of scope

- Quantization or nearest-root approximation.
- Turn/river roots with non-flop-terminal trees (RFC 0036 owns on-demand).
- Per-position line-conditioned ranges (reserved profile 2, unrepresentable
  in BSFLIB03; needs its own section revision).
- Neural estimators.
- Training, artifact publication, or any compute spend before stage 4 and
  the explicit authorization.

## Follow-up required by RFC 0037 (2026-10-25)

The shape-keyed abstraction policy assigns caps by street-start shape, but
the BSFLIB03 format stores ONE file-level action identity and the runner
mounts only ONE binary flop library, so one library cannot serve roots that
require different caps. The existing six-way library's roots (all
standard-band 3-6-live) are uniformly cap 1 and remain compatible. A future
root that the policy assigns cap 0 — a two-live postflop root or a
shallow-band root — needs a follow-up RFC that either (a) moves the action
identity into per-root headers, or (b) defines a supported multi-binary
mount set. Until then such roots are blocked rather than trained into the
cap-1 file; `tools/artifact-audit/rfc0037-policy-audit.py` fails on a
synthetic mixed-cap library.
