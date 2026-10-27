# RFC 0030 Implementation Plan

Status: Historical

Retired as an operational document on 2026-10-27 by [RFC 0040](../rfcs/0040-retire-river-club-agent-api.md): its references to River bin launchers (e.g. journal replay) describe deleted commands. The RFC 0030 implementation evidence below is retained verbatim as a dated record.

Execution state: RFC 0030 is Accepted (independent approval record in the RFC's
Decision section, `rfc0030-approval-1`, 2026-10-24, approved on round 2 with
editorial corrections applied). This plan maps its three rollout stages to
tasks, evidence, and gates. Stages 1-2 are pure repository work. Stage 3
rebuilds local artifacts and needs an explicit training/compute authorization
before it runs; the RFC names that boundary and it is not bypassed here.

## Stage 1 — Identity minting and mid-street counter (one commit)

Tasks:

1. `engine/include/bs/abstraction.hpp` / `engine/src/abstraction/abstraction.cpp`
   - `ActionAbstraction(SizeSchedule, std::uint8_t max_raises = 0)`,
     `identity(std::uint8_t = 0)`, `declared(schedule, seeds, std::uint8_t = 0)`;
     delete `set_max_raises_per_street`.
   - `make_id` / `make_declared_id` append the canonical token
     `|max-raises=<unsigned decimal>` with no trailing newline; cast through
     `unsigned` (never stream the `uint8_t`).
   - Add `parse_cap_token(const std::string& parameters) -> std::optional or
     named error`, `is_identity_action_family(const AbstractionId&)`
     (name + canonical schedule + version + cap + recomputed digest), and
     apply the same strict recompute/cap requirement to the declared-coarse
     family at the gates.
   - `reconstruct_action_abstraction` keeps its lenient contract: empty id and
     unrecognized name/version fall back to `ActionAbstraction(sizes)` cap 0;
     recognized family parses the cap; recognized tokenless/malformed throws.
2. Gate replacements in `engine/src/gto/nseat_trainer.cpp:870-873` (call
   `:929`) and `engine/src/gto/solve.cpp:26-29,82-85` with the family
   predicates.
3. Producer call sites pass the cap via constructor:
   `flop_library_builder.cpp:192`, `preflop_profile_builder.cpp:207`,
   `subgame_resolver.cpp:505`, `subgame_nash_conv.cpp:1166,1195`,
   `test_flop_library.cpp:222`; `preflop_nash_conv.cpp:29-64` passes the
   recovered cap and refuses a tokenless input by name.
4. Golden digest pins in `engine/tests/test_abstraction.cpp:214,556` updated
   to the new canonical values.
5. Mid-street counter: `GameDef::raises_this_street`, `GameState(GameDef const&)`
   assignment, `build_subgame_def_from_node` seeds it
   (`subgame_resolver.cpp:340-377`), `same_game_def` compares it,
   `validate(GameDef)` rejects a nonzero count at a street-start root.

Gates: identity/admissibility/mid-street native tests; `resolve_flop` solves a
capped fixture; `--raise-cap 1` flop-library build of the test fixture
completes; zero `set_max_raises_per_street` hits; full Engine Build Contract;
ASan on resolver/codec surfaces.

## Stage 2 — BSWARM02 tree-shape witness

Tasks:

1. `engine/include/bs/sealed_checkpoint.hpp`: magic `BSWARM02`, version field
   2, `kSealedCheckpointHeaderBytes = 192`, `tree_shape_digest u64` at offset
   184; no old magic codec (bad magic follows the corruption-discard path).
2. One shared witness function (tree owner module) implementing the RFC hash;
   golden stored-constant test; target/kind/size mutation tests.
3. Seal writes the witness over the materialized tree; `train_one_class`
   recomputes and hard-refuses on mismatch BEFORE rows are adopted
   (`flop_library.cpp` adoption point), preserving files; the check precedes
   the stale-but-complete branch.
4. Tests: AC4.1 direct `train_nseat` shifted-row acceptance; AC4.2 tampered
   witness refusal with present matching `build.json`; AC4.3 clean rerun;
   AC4.4 legacy `BSWARM01` file discarded as corrupt and retrained.
5. `static_assert` at `sealed_checkpoint.cpp:83` updated to 192.

Gates: warm-start/resume tests unchanged for torn/incompatible/stale
dispositions; witness tests pass; Engine Build Contract; ASan.

## Stage 3 — Rebuild and local audit (executed 2026-10-25)

Status: executing with local compute (offline training, no live play).

1. Current `artifacts/**/library.bin` and the capped `.db` artifacts were
   preserved outside the repository under `~/.bigshark-backups/`
   (`ac1-16w-pre0030`, `profiles-pre0030/`,
   `flop-library-6way-fullrange-pre0030`) at Stage-3 time; the whole
   backup tree was deleted on 2026-10-26 by maintainer direction.
2. Rebuilds, all with explicit `|max-raises=` tokens:
   - `ac1-16w`: identity (`rfc0007-pot-fractions`) cap 0, full range, 160
     classes, 2 seats, 100 BB, 1,000 iterations; rebuilt pack-direct
     (RFC 0027). Retired from `artifacts/` on 2026-10-26 as the narrow
     early library with no mount consumer; this line is the historical
     Stage-3 build record.
   - `preflop-profile` (2p, cap 0), `preflop-profile-3way` (3p 10 BB,
     identity; needs `--max-infosets 2000000 --train-bytes 8589934592`
     because the Preflop169-retaining trainer needs ~1.58M information
     sets on the three-seat identity tree, above the library default;
     retired 2026-10-26 under RFC 0037's legacy-retirement amendment —
     this line is the historical Stage-3 build record),
     `preflop-profile-6way` (6p, identity cap 1).
   - `flop-library-6way-fullrange`: identity cap 1, full range, 1,755
     classes, 6 seats, 200-chip start / 2 contribution, 1,000 iterations.
   - The `preflop-profile-validation` artifact is an RFC 0029 measurement
     artifact whose generation procedure (postflop `.nsp` policy
     directory) is not pinned; it was preserved in the backup (deleted
     2026-10-26) and is regenerated with the T-09 replay program rather
     than guessed.
3. The AC9 local audit runs `python3 -I tools/artifact-audit/ac9-token-audit.py
   artifacts`, which opens every `.db` manifest table and every
   `manifest.json` carrying `action_abstraction_parameters`, and fails on a
   tokenless parameter line. Result recorded after the six-way rebuild.
4. New digests are recorded in the RFC 0028/0029/0031 erratum notes; the
   packed library's continuation menu now keeps its trained cap through the
   BSFLIB03 full-action header (that gap is closed by RFC 0035); the
   BSFLIB02 form retains the documented continuation gap until replaced.

## Verification commands

```bash
cmake --preset release
cmake --build --preset release --target format
cmake --build --preset release
ctest --preset release
cmake --build --preset release --target benchmark-multistreet
npm run check
npm run proto:check
node bin/replay.mjs
node bin/check-rfcs.mjs
node bin/check-docs.mjs
```

ASan is required for the GameDef lifetime change (stage 1) and the codec
header change (stage 2).
