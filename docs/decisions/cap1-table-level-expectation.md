# Decision Support: Cap-1 Table-Level Expected Loss (BB/100)

Status: Current
Date: 2026-11-18
Scope: read-only measurement campaign (D-346) converting the sealed
per-occurrence cap-1 structural residuals into table-level expected loss in
BB/100. No strategy code, cap configuration, training run, or production
behavior was changed; the only new executable material is offline analysis
tooling under `docs/plans/ablation/cap1-table-level/` and transcript-driven
runs of the existing `bigshark-practice` CLI against the existing
`bigshark-engine` binary. Companion to
[cap1-online-quality-and-risk.md](cap1-online-quality-and-risk.md), which
establishes the per-occurrence values this document weights by reach
frequency.

## 1. Question and answer shape

The earlier analysis quantified the cap-1 leak **per occurrence of a deep
check-to-river spot** (about 9.7-15.6 BB per engine seat per occurrence under
the Uniform completion, 76.5-107.7 BB under the BrFavorable bound). A
per-occurrence value is not a session rate: most dealt hands never reach such
a spot. This document measures the missing factor — how often the exposed
root classes are reached per dealt hand — and combines the two:

```
E[loss per dealt hand] = sum over root classes r of P(r reached) x E[loss | r]
table-level BB/100     = 100 x E[loss per dealt hand]
```

The answer is delivered as an **interval plus assumption table**, not a
single point estimate, because one river class (rivers reached after flop or
turn aggression) has no sealed per-occurrence measurement and is carried as
an explicit unknown with bounded treatment.

## 2. Method

### 2.1 Two evidence layers

1. **Per-occurrence severity (sealed, reused unchanged).** The T6
   six-stratum anchor sweep and the RFC 0049 union-reference measurements
   give the expected extra loss conditional on reaching each frozen anchor
   root. Values and receipts are listed in section 4; none were recomputed.
2. **Reach frequencies (newly measured here).** Heads-up 100 BB hands dealt
   by the existing offline practice table, with the bot seat served by the
   real engine process. Every hand is fully dealt to a fold or showdown, so
   street reach is observed rather than assumed.

### 2.2 Why a mounted engine root is required

With no resident blueprint mounted, the engine advertises no capabilities
(`resolvingAdvertised()` is false when zero roots are loaded) and every
served decision is the labeled operational fallback: check if legal,
otherwise call, otherwise fold
(`engine/src/protocol/envelope.cpp`, `operationalFallbackDecision`). A
transcript from that configuration contains zero aggressive bot actions and
is therefore a **mechanical upper-bound distribution**, not an equilibrium
one. It was run anyway as a diagnostic and contrast corpus (section 6.2).

The production-shaped corpus mounts the local heads-up 100 BB preflop
profile (`artifacts/preflop-profile/preflop-profile.db`, schema v2,
`max_raises_per_street = 0`, i.e. the exact cap-0 menu) as a pinned resident
root. With the root advertised, postflop misses are handled by the live
subgame resolver, which produces the bet/raise/jam actions absent from the
fallback corpus. The resulting arrival distribution is the best available
equilibrium-derived distribution on this machine. It is cap-0-conditional;
the implications are treated in section 7.

### 2.3 Root classes

Each dealt hand is classified from its transcript:

| class | definition | matching sealed anchor |
| --- | --- | --- |
| `limp_ctr` | river reached, no preflop raise, no flop/turn aggression (pot-4 check-to-river) | the five limped T6 strata |
| `srp_ctr` | river reached, exactly one preflop raise, no flop/turn aggression (pot-8 check-to-river) | the `raised-srp` T6 stratum |
| `other` | river reached after flop/turn aggression, with money left to bet | no per-occurrence measurement; treated as unknown |
| `allin_runout` | river reached only because an all-in was committed (no remaining decision) | structural zero: no raise decision exists for cap-1 to miss |
| `none` | hand ended before the river | no river residual possible |

The pot values follow deterministically from the action history, so the
class-to-anchor pot identity (4 BB limped, 8 BB single-raised) is structural,
not sampled.

### 2.4 Opponent distribution and scripted seat

The engine always occupies one seat; the other seat is driven by one of
five scripts so that arrival frequencies can be bracketed between
contrasting opponent styles. The first three are purely passive and form the
headline evidence; the last two target conditions the passive scripts cannot
reach:

- **station** — check when legal, otherwise call; never folds, never raises
  (maximally loose-passive; maximizes river reach).
- **folder** — check when legal, otherwise fold; never puts money in
  voluntarily (maximally tight; minimizes river reach).
- **passive-random** — uniform over fold/check/call only; passive but
  stochastic, giving an interior point with natural fold rates.

- **uniform** — uniform over every legal action including raises and jams,
  producing natural preflop aggression. In the mounted configuration about
  45% of its bot decisions replay-miss off-tree aggressive amounts and fall
  back to the passive distribution (600 of 1,342 in the retained transcript),
  so its rates are reported with an explicit contamination caveat rather than
  used as headline evidence.
- **srp-passive** — one smallest preflop raise at its first preflop decision,
  then fold/check/call only. It exists to measure the conditional
  check-to-river rate inside a single-raised pot, which the passive scripts
  structurally cannot reach; its 63% fallback rate is disclosed in section 7.

### 2.5 Reproducibility

All runs reuse existing binaries; nothing was compiled or trained. Commands,
wall times, seeds, and sample sizes are recorded in section 8. Raw
transcripts and machine-readable event sidecars are retained under
`docs/plans/ablation/cap1-table-level/` (`*.log` is git-ignored repository
policy). Two distinct reproducibility levels apply:

- **Exact (parsed evidence).** Every committed CSV is regenerated from a
  retained transcript by the committed parser and calculator with no random
  element; this is byte-verified.
- **Statistical (transcript generation).** Re-running a segment with the
  same seed does not reproduce the transcript bit for bit. The deck and
  solver PRNG streams are seeded deterministically from hand and decision
  indices, but the online resolver stops on a wall-clock deadline
  (`limits.time`, `apps/engine-host/main.cpp`), so completed iteration counts
  vary with scheduling and the sampled actions can diverge, after which hand
  paths differ. The correct re-run test is therefore frequency-level
  replication, performed with independent fresh seeds in section 3.2.

## 3. Reach frequencies

All values are shares of the 400 dealt hands in that corpus (1,200 pooled
per campaign), with binomial standard errors in the summary CSVs. The table
below is campaign 1 (seeds 401-405, retained transcripts); campaign 2 is
compared in section 3.2.

| corpus (scripted seat style) | flop | turn | river | limp_ctr | srp_ctr | other river | all-in runout | fold end |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| station (never folds) | 1.000 | 1.000 | 1.000 | 0.0775 | 0 | 0.6225 | 0.3000 | 0 |
| folder (folds to any bet) | 0.500 | 0.0475 | 0.0475 | 0.0325 | 0 | 0 | 0.0150 | 0.9950 |
| passive-random (passive, stochastic) | 0.745 | 0.450 | 0.450 | 0.0600 | 0 | 0.2500 | 0.1400 | 0.7175 |
| **pooled passive (1,200 hands)** | 0.748 | 0.499 | 0.499 | **0.0567** | 0 | 0.2908 | 0.1517 | 0.5708 |
| uniform (all actions equally) | 0.918 | 0.848 | 0.813 | 0 | 0.0025 (1 hand) | 0.2850 | 0.5250 | 0.2675 |
| srp-passive (one forced preflop raise) | 1.000 | 0.780 | 0.780 | 0 | 0.5100 | 0.1850 | 0.0850 | 0.3200 |

Three structural facts drive everything else:

1. **The three purely passive scripts never raise preflop** (verified from
   the event sidecars: zero aggressive picks), and the bot's first preflop
   decision is always the provenance-gate fallback (check/limp) — exactly 200
   fallbacks per 400-hand passive corpus in both campaigns — so no passive
   corpus can produce an SRP. `srp_ctr = 0` there is a construction property,
   not an estimate.
2. **Limped check-to-river is genuinely rare under equilibrium play:**
   3.25%-7.75% of dealt hands per passive style, 5.67% pooled in campaign 1
   (binomial SE 0.0067, 95% interval 4.36%-6.98%). The same class occurs in
   100% of hands in the unmounted mechanical corpus, which is why that corpus
   is a bias demonstration rather than evidence.
3. **The SRP check-to-river rate is only measurable conditionally.** The
   srp-passive script forces exactly one preflop raise per hand, and 51.0%
   of those hands then check to the river in the single-raised pot. This is
   a conditional rate P(check-to-river | single-raised pot), not a
   per-dealt-hand frequency; the unconditional P(single-raised pot) under a
   realistic style is not measured in this campaign (the only natural
   corpus containing it, uniform, produced one clean instance in 400 hands).
   It also carries a fallback caveat in section 7.

The river classes partition every river-reached hand: in the campaign-1
pooled passive corpus, 5.67% + 29.08% + 15.17% = 49.92%, equal to total
river reach, with no unclassified remainder.

### 3.2 Independent replication

Because transcript generation is only statistically reproducible (section
2.5), the full five-corpus campaign was re-run unchanged with independent
deck seeds (501-505 against 401-405) on the same binaries and budget. The
replication is an independent sample, not a re-roll of the same decks: no
hand is shared between the two campaigns. The comparison below is therefore
a genuine between-campaign stability test.

Headline rates per passive style and pooled:

| quantity | campaign 1 (seeds 401-403) | campaign 2 (seeds 501-503) |
| --- | --- | --- |
| station limp_ctr per dealt hand | 0.0775 | 0.0675 |
| folder limp_ctr per dealt hand | 0.0325 | 0.0375 |
| passive-random limp_ctr per dealt hand | 0.0600 | 0.0600 |
| pooled limp_ctr (1,200 hands each) | 0.0567 | 0.0550 |
| pooled `other` river per dealt hand | 0.2908 | 0.2858 |
| pooled all-in runout per dealt hand | 0.1517 | 0.1433 |
| pooled lower-branch expectation (BB/100, T6 Uniform) | 56.02 | 54.37 |
| pooled upper-branch expectation (BB/100) | 484.15 | 475.14 |

The two campaigns' pooled lower-branch expectations differ by 1.65 BB/100,
well inside one combined standard error (SE 6.60 and 6.51, combined ~9.27);
every per-style frequency difference is below 1.1 binomial SE. The targeted
corpora replicate too: the conditional SRP check-to-river rate is 51.0%
versus 52.75%, and both independent uniform corpora produced exactly one
clean SRP check-to-river in 400 hands (0.25%), confirming that this class is
genuinely rare under an aggressive style rather than a single-run fluke.

Pooling the two independent campaigns gives 2,400 passive-style hands with
134 limped check-to-river occurrences (5.58%). The 2,400-hand pooled
estimates in section 5 are the primary headline; the per-campaign values are
retained above as the stability evidence. Receipts:
`r2-summary.csv`, `both-summary.csv`, `both-expectation.csv`.

## 4. Per-occurrence severity inputs

All values are BB per engine seat, i.e. the sealed two-seat summed
exploitability divided by two, because the practice table has exactly one
engine seat and only that seat concedes the structural loss.

### 4.1 T6 direct cap-0 reference (Uniform completion and BrFavorable bound)

Per-seat values below are the sealed two-seat summed exploitability divided
by two. The five limped strata share the pot-4 check-to-river root shape;
`raised-srp` is the pot-8 single-raised root.

| anchor stratum | Uniform, per seat (BB) | BrFavorable, per seat (BB) |
| --- | --- | --- |
| w1 (limped) | 9.7411 | 76.4612 |
| unpaired-rainbow-ahigh | 9.7243 | 77.0396 |
| paired | 9.7909 | 77.1303 |
| monotone | 10.4110 | 84.8841 |
| unpaired-twotone | 9.7632 | 76.8908 |
| **limped mean** | **9.8861** (range 9.7243-10.4110) | **78.4812** (range 76.4612-84.8841) |
| raised-srp | 14.7207 | 94.3803 |

Source: T6 SIX-STRATUM ANCHOR SCOPE RESULT in
`docs/plans/0047-stage1-tracker.md` (raw two-seat values 19.4822-29.4414
Uniform and 152.9223-188.7606 BrFavorable).

### 4.2 Union reference (w1 only)

The stricter union reference also prices missing bet-sizing actions. It was
run on the w1 anchor only (`exact_exploitability_bb` in
`docs/plans/ablation/rfc0047/s3/stage2b-union/w1-coarse-300.csv`):

| completion | two-seat sum | per engine seat |
| --- | --- | --- |
| Uniform | 31.24489667 | 15.6224 |
| BrFavorable | 215.34663379 | 107.6733 |

Applying the w1 union value to every limped spot is an extrapolation: no
union measurement exists for the other four limped strata or for the SRP
root, so union-based rows are labeled `union_limp_w1` and kept separate from
the T6-direct rows. The companion analysis's headline range "9.7-15.6 BB
Uniform, 76.5-107.7 BB BrFavorable" spans exactly the T6-to-union per-seat
extrema above.

## 5. Table-level expectation

Every row is reproducible from the summary CSVs via `table-expectation.mjs`;
the script prints its sealed severity constants to stderr. Standard errors
propagate binomial frequency uncertainty only; the sealed severity inputs
are treated as fixed. BB/100 = 100 x expected loss per dealt hand. Two
poolings are reported: each 1,200-hand campaign separately (section 3.2) and
the primary **2,400-hand pool over both independent campaigns** here.

### 5.1 Conservative certificate (T6 Uniform completion), measured classes only

This branch charges only `limp_ctr` and `srp_ctr` hands their sealed
per-occurrence loss and assumes zero contribution from the unmeasured
`other` class.

Primary 2,400-hand pooled-passive result:

| quantity | value |
| --- | --- |
| expected loss per dealt hand | 0.5520 BB |
| **BB/100** | **55.20** |
| binomial SE (BB/100) | 4.63 |
| 95% interval (BB/100) | **46.1 - 64.3** |

Per-campaign and scenario detail (each 400 hands unless noted):

| corpus | per dealt hand (BB) | BB/100 | 95% interval (BB/100) |
| --- | --- | --- | --- |
| pooled passive, campaign 1 (1,200) | 0.5602 | 56.02 | 43.1 - 68.9 |
| pooled passive, campaign 2 (1,200) | 0.5437 | 54.37 | 41.6 - 67.1 |
| **pooled passive, both campaigns (2,400)** | **0.5520** | **55.20** | **46.1 - 64.3** |
| uniform, each campaign (one SRP hand each) | 0.0368 | 3.68 | driven by one SRP hand; not a rate estimate |
| srp-passive forced-SRP scenario, campaign 1 | 7.5076 | 750.76 | 678.7 - 822.9 |
| srp-passive forced-SRP scenario, campaign 2 | 7.7652 | 776.52 | 704.5 - 848.5 |

The 55.20 BB/100 pooled result is the headline conservative number: it uses
the T6 Uniform per-occurrence values (the standard non-anticipatory
certificate), 2,400 dealt hands across two independent campaigns and three
contrasting passive styles, and charges nothing for the unknown `other`
river class. The interval width is sampling error on reach frequencies only.

### 5.2 Stricter reference and adversarial completion

Same 2,400-hand pooled frequencies, different sealed severity inputs:

| basis | pooled BB/100 | 95% interval |
| --- | --- | --- |
| T6 Uniform (5.1) | 55.20 | 46.1 - 64.3 |
| Union reference, Uniform (w1 extrapolated) | 87.23 | 72.9 - 101.6 |
| T6 BrFavorable adversarial bound | 438.19 | 366.1 - 510.3 |
| Union BrFavorable (w1 extrapolated) | 601.18 | 502.3 - 700.1 |

### 5.3 Band over the unknown `other` river class

The `other` class occurs in 28.83% of the 2,400 pooled dealt hands (692 of
2,400) and has no per-occurrence measurement. The band brackets it honestly:

- **Lower branch** (other contributes zero): the 5.1 value, 55.20 BB/100
  pooled (95% interval 46.1-64.3).
- **Upper branch** (every other-river hand is charged the SRP per-occurrence
  value 14.7207 BB, the more severe measured class under T6 Uniform): pooled
  479.64 BB/100 (95% interval 451.5-507.8); the two campaigns separately give
  484.15 and 475.14.

All-in runouts (14.75% of the 2,400 pooled hands) are excluded from both
branches: with every remaining chip committed, no raise decision exists, so
the cap-1 menu difference is structurally zero there.

### 5.4 What the interval does and does not claim

The defensible headline is therefore **roughly 55 BB/100 under the
conservative Uniform certificate (95% interval 46-64), widening to about
480 BB/100 if the unmeasured river class is as severe as the worst measured
anchor, and to about 438-601 BB/100 under adversarial completion on measured
classes**. These are expected losses of the single engine seat against an
opponent who exploits the missing re-raise category, not realized session
outcomes, and they rest on cap-0-conditional frequencies (section 7).

## 6. Contrast and sanity checks

### 6.1 Why per-occurrence values are not session rates

The companion document's 9.7-15.6 BB Uniform figure is conditional on being
in the deep river spot. The two quantities differ by exactly the reach
factor measured here:

```
BB per 100 hands = 100 x P(spot reached per dealt hand) x BB per occurrence
```

With the mounted equilibrium distribution, the matching limped check-to-river
class is reached on only 3.25%-7.75% of dealt hands across the passive styles
(5.58% in the 2,400-hand pool), so even a 9.9 BB per-occurrence loss becomes
roughly 54-56 BB per 100 hands at the pooled-campaign level rather than 990
BB per 100. Substituting the per-occurrence value for a session mean
overstates the rate by the inverse reach factor, about 18x at the pooled
limp frequency. Both numbers are correct for their own question; they answer
different questions.

### 6.2 Mechanical unmounted corpus

Running the same three passive scripts with NO resident root mounted (every
bot decision the check/call/fold operational fallback) produces 1,000 hands
per script in about 1.3 seconds total. Because the bot never bets or raises,
limped check-to-river reach is 50%-100% and the implied mechanical rate is
494-989 BB/100 — roughly an order of magnitude above the equilibrium-derived
54-56 BB/100 pooled lower-branch values. This corpus is retained only as a
contrast and a diagnostic:
it proves that assuming a passive distribution inflates the table-level
number, and it is never used as an input to the headline intervals. Receipts:
`fb-{station,folder,passive-random}-1000.log`, parsed in
`fb-summary.csv` and `fb-expectation.csv`.

## 7. Uncertainties and unknowns

- **Cap-0-conditional frequencies.** The mounted resolver plays the exact
  cap-0 menu. A shipped cap-1 engine would generate its own arrival
  distribution; it was not measured because producing it requires a cap-1
  resolver configuration outside this campaign's authorized scope.
  Directional reasoning (qualitative, not measured): removing re-raise
  weapons removes fold pressure, so hands that cap-0 play would end in folds
  more often continue to later streets, which pushes river reach upward; the
  sign of the net bias on the specific check-to-river classes is not
  determinable without a cap-1 distribution. Treat the mounted frequencies
  as the best available estimate, not a cap-1-certified one.
- **The `other` river class has no severity measurement.** Its treatment is
  an explicit band (zero lower branch; srp-equivalent upper proxy), never a
  hidden point estimate. All-in terminal rivers, reported separately, cannot
  carry a menu residual because no raise decision remains.
- **Single machine and binary.** Frequencies come from one deterministic
  build on one host; they are reproducible bitwise from the recorded seeds
  but not transferable to other hardware without rerunning.
- **Heads-up only.** Severity receipts are two-seat measurements. No
  multiway BB/100 figure is produced or implied.
- **Sampling error.** Frequency standard errors are binomial (independent
  dealt hands under the table's seeded CRN deck) and propagated linearly;
  95% intervals use +/- 1.96 SE. Severity inputs are treated as fixed sealed
  measurements; their own uncertainty is not resampled.
- **srp-passive fallback caveat.** The corpus recorded 1,000 fallback
  decisions against 594 served in campaign 1 (62.7%; 61.9% on the same
  corpus in campaign 2, 1,000 against 616): after the scripted preflop
  raise, many bot decisions miss the resident/replay path and return the
  passive check/call/fold distribution. Passive responses make a
  check-to-river more likely, so the 51.0%/52.75% conditional SRP
  check-to-river rates are best read as upper-ish conditional estimates
  under passive defense, not equilibrium conditional rates. The
  pooled-passive headline does not use this corpus.
- **Uniform-corpus replay caveat.** The uniform style picks off-tree
  aggressive amounts; 44.7% of its bot decisions fell back in campaign 1
  (47.3% in campaign 2), and each 400-hand campaign produced exactly one
  clean SRP check-to-river. Its rows are retained for completeness but are
  not used as headline rates.

## 8. Commands, receipts, and wall times

All runs are local, single-machine, using the pre-existing
`build/release/engine/bigshark-practice` and `bin/bigshark-engine` binaries.
No training, compilation, or strategy-code change was involved. Total
mounted-engine compute across the first campaign, the same-seed regeneration
run, and the independent replication was about 92 minutes (5,516 s of
segment wall time), plus about three minutes of probes and the 1.25 s
unmounted control — inside the 120-minute local-compute budget. The wall
clock from first launch to final replication result was longer because
analysis and parsing ran between segments; that interleaving adds no engine
compute.

| step | command (repo-root cwd) | wall |
| --- | --- | --- |
| unmounted control, 3 x 1,000 hands | `node docs/plans/ablation/cap1-table-level/drive-practice.mjs {station,folder,passive-random} 1000 501 300 <log>` | 1.25 s total |
| campaign 1, five segments (seeds 401-405) | `ENGINE_PATH=.../engine-hu-root.sh node ... drive-practice.mjs <policy> 400 <seed> 300 ... --engine-timeout-ms 120000` | 1,949 s |
| same-seed regeneration run | `run-regen.sh` (seeds 401-405) | 1,815 s |
| independent replication | `run-replication.sh` (seeds 501-505) | 1,752 s |
| parsing | `node parse-hands.mjs label=log ... --rows ... --summary ...` | <1 s each |
| scoring | `node table-expectation.mjs both-summary.csv both-expectation.csv --pool=pooled-passive-2400=station,folder,passive,r2-station,r2-folder,r2-passive` | <1 s |
| metadata | `node extract-meta.mjs label=log ... --timing run2.timing --out ...` | <1 s |

The mounted engine is `engine-hu-root.sh`, which execs the published engine
with `--resident-budget 4096 --resident-root artifacts/preflop-profile/preflop-profile.db=<pinned sha256>`; the 4 GiB budget is required because the
root's resident footprint is 413,380,538 bytes against the 256 MiB default.
Every segment exited 0 (driver-err sidecars). Timing ledgers:
`campaign.timing`, `regen.timing`, `run2.timing`; per-hand transcripts and
event sidecars: `hu-*-seed40*.log(.events.log)` and
`r2-*-seed50*.log(.events.log)` (git-ignored `*.log`); committed parsed
evidence: `mounted-{rows,summary,expectation}.csv`,
`r2-{rows,summary}.csv`, `both-{summary,expectation}.csv`,
`fb-{rows,summary,expectation}.csv`.

## 9. How this changes the A/B/C decision inputs

Only option A is named in the repository's decision records: ship the
bounded cap-1 online policy with its structural residual documented
([cap1-online-quality-and-risk.md](cap1-online-quality-and-risk.md)). The
exact definitions of B and C belong to the product decision owner; the
comparison below uses the natural framings "do not ship / keep the engine
offline-only" and "fund a new certification campaign under the R1-R4 restart
criteria" ([m2-certification-terminal-state.md](m2-certification-terminal-state.md)).
If B or C are defined differently, the numbers below still apply unchanged;
only their mapping to option labels needs adjusting.

- **Cost of A (ship cap-1), now in table units.** Under the conservative
  Uniform certificate, the measured check-to-river classes alone imply about
  54-56 BB/100 per passive-style campaign, or 55.20 BB/100 in the 2,400-hand
  two-campaign pool (95% interval 46.1-64.3). The aggressive-style uniform
  corpus shows only 3.7 BB/100 on measured classes, but that value rests on a
  single SRP hand per 400 and is not a rate estimate; its check-to-river
  exposure is largely replaced by all-in runouts. The BrFavorable
  adversarial completion scales the pooled frequencies to 366-510 BB/100 on
  measured classes, and the unmeasured `other` river class — 28.8% of pooled
  dealt hands — can widen the Uniform band to 451-508 BB/100 (pooled upper
  branch) if it were as severe as the SRP anchor. For scale, ordinary
  winning poker edges are single-digit BB/100, so even the lower branch is
  many times any plausible table win rate.
- **Cost of B (do not ship).** Unchanged by this measurement: zero online
  leak, zero online product, continued offline-only status. The new evidence
  does not make B cheaper; it quantifies what B avoids in table units.
- **Cost of C (restart campaign).** Unchanged in direct terms, but the
  reach analysis shows where a future campaign's measurement budget has the
  highest value: per-occurrence severity is already sealed, so the remaining
  first-order unknown is the `other` river class (28.8% of pooled dealt
  hands, 58.6% of river hands) together with an unconditional
  single-raised-pot frequency and any cap-1-conditional distribution. A
  restart RFC that re-measures those items would collapse the widest part of
  the band in section 5 without re-running any existing anchor.
- **What is NOT decided here.** No option is selected. The BB/100 figures are
  expected-value inputs for an informed A/B/C choice, alongside latency,
  stability, and contractual considerations already documented elsewhere.

## 10. Evidence index

- Per-occurrence residuals and node allocation:
  `docs/plans/ablation/rfc0047/s3/stage2b-union/w1-coarse-300.csv`,
  `docs/plans/ablation/rfc0047/s3/pathb-reach/w1-coarse-300.txt`,
  `docs/plans/0047-stage1-tracker.md` (T6 sections).
- Online timing and iteration curve:
  `docs/plans/ablation/rfc0047/s3/online-river-w1-t24.csv`,
  `docs/plans/ablation/rfc0049/gate2/w1-control-cap1.csv`.
- This campaign's tooling and parsed evidence:
  `docs/plans/ablation/cap1-table-level/`.
- Companion analysis: [cap1-online-quality-and-risk.md](cap1-online-quality-and-risk.md),
  [m2-certification-terminal-state.md](m2-certification-terminal-state.md).
