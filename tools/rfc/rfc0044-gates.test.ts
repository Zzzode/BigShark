import assert from 'node:assert/strict';
import {readdirSync, readFileSync, mkdtempSync, writeFileSync} from 'node:fs';
import {tmpdir} from 'node:os';
import {join} from 'node:path';
import {fileURLToPath} from 'node:url';
import {test} from 'node:test';

import {
  LADDER,
  LADDER_CAP,
  SEED_COUNT,
  EPS_GAP,
  T_CRIT_0_975_4,
  RECOGNIZED_COLUMNS,
  aggregateSeries,
  buildRungPoint,
  classifyFloor,
  evaluateCert,
  evaluateQual,
  fitLogLinear,
  floorCrossingIterations,
  floorFit,
  RULE5_WINDOW_RUNGS,
  loadLadderCsv,
  loadLadderCsvs,
  powerPredict,
  parseCertificationArgs,
  type RungPoint,
  type SeedSample,
} from './rfc0044-gates.js';

const SEEDS = [101, 102, 103, 104, 105];

// Construct one rung directly from five independent-seed bucket gaps
// (and, optionally, exact-hole gaps). null seeds means a missing /
// non-observed rung.
function rung(index: number, bucketGaps: number[] | null, exactGaps?: number[]): RungPoint {
  const samples: SeedSample[] = [];
  if (bucketGaps !== null) {
    assert.equal(bucketGaps.length, SEED_COUNT);
    bucketGaps.forEach((bucket, k) => {
      samples.push({seed: SEEDS[k]!, bucket, exact: exactGaps ? exactGaps[k]! : bucket});
    });
  }
  return buildRungPoint(index, samples);
}

function identical(g: number): number[] {
  return Array(SEED_COUNT).fill(g);
}

// g(T) = a*T^-b over the full ladder, with optional per-rung overrides
// and missing rungs.
function powerSeries(
  a: number,
  b: number,
  overrides?: Map<number, number>,
  missing?: ReadonlySet<number>,
): RungPoint[] {
  return LADDER.map((T, i) => {
    if (missing?.has(i))
      return rung(i, null);
    const g = overrides?.get(i) ?? a * T ** -b;
    return rung(i, identical(g));
  });
}

test('smooth power-law tail certifies at the smallest crossing rung', () => {
  const b = 0.6;
  const a = 0.009 * 10_000_000 ** b;  // smooth g(10M) = 0.009 > EPS_GAP
  const verdict = evaluateCert(powerSeries(a, b));
  assert.equal(verdict.certified, true);
  assert.equal(verdict.rCert, 5);  // 30M: g = 0.00437; 10M stays above 0.005
  const at10M = verdict.rungs[4]!;
  assert.equal(at10M.passes.r1, false);
  const cert = verdict.rungs[5]!;
  assert.deepEqual(cert.passes, {r1: true, r2: true, r3: true, r4: true});
  assert.ok(Math.abs(cert.values.tailFit!.b - b) < 1e-9);
  assert.ok((cert.values.priorRelDeviation ?? 1) < 1e-9);
});

test('one-rung -45% dip passes rule 3 but is rejected by rule 4', () => {
  const b = 0.6;
  const a = 0.0085 * 30_000_000 ** b;  // smooth g(30M) = 0.0085
  // The dip is at 30M (index 5): 0.55x the smooth prediction, just
  // below EPS_GAP. Prior window (1M/3M/10M) cannot predict it.
  const series = powerSeries(a, b, new Map([[5, 0.55 * a * LADDER[5]! ** -b]]))
    .slice(0, 6);
  const verdict = evaluateCert(series);
  assert.equal(verdict.certified, false);
  assert.equal(verdict.rCert, null);
  const dipped = verdict.rungs[5]!;
  assert.equal(dipped.passes.r1, true);   // 0.004675 <= EPS_GAP
  assert.equal(dipped.passes.r2, true);   // identical seeds -> zero hw
  assert.equal(dipped.passes.r3, true);   // three-rung fit stays within 20%
  assert.equal(dipped.passes.r4, false);  // prior window predicts no dip
  assert.ok(Math.abs((dipped.values.priorRelDeviation ?? 0) - 0.45) < 1e-9);
  assert.ok(dipped.failures.includes('r4:prior-prediction'));
  assert.ok(verdict.firstFailureReasons.includes('r4:prior-prediction'));
});

test('sub-bar plateau with fitted b = 0 certifies once i >= 3', () => {
  const series = LADDER.map((_, i) => rung(i, identical(0.004)));
  const verdict = evaluateCert(series);
  assert.equal(verdict.certified, true);
  assert.equal(verdict.rCert, 3);
  for (const i of [0, 1, 2])
    assert.equal(verdict.rungs[i]!.eligible, false);
  const cert = verdict.rungs[3]!;
  assert.deepEqual(cert.passes, {r1: true, r2: true, r3: true, r4: true});
  assert.ok(Math.abs(cert.values.tailFit!.b) < 1e-12);
  assert.ok((cert.values.tailMaxRelResidual ?? 1) < 1e-12);
});

test('single-rung >81% crash is rejected', () => {
  const b = 0.6;
  const a = 0.03 * 3_000_000 ** b;  // smooth g(3M) = 0.03
  const series = powerSeries(a, b, new Map([[3, 0.10 * a * LADDER[3]! ** -b]]))
    .slice(0, 4);  // nothing observed after the crashed rung
  const verdict = evaluateCert(series);
  assert.equal(verdict.certified, false);
  const crashed = verdict.rungs[3]!;
  assert.equal(crashed.passes.r1, true);  // 0.003 below the absolute bar
  assert.equal(crashed.passes.r3 === false || crashed.passes.r4 === false, true);
  assert.ok((crashed.values.tailFit?.b ?? 0) > 1.5 || !crashed.passes.r4);
  assert.ok(Math.abs((crashed.values.priorRelDeviation ?? 0) - 0.9) < 1e-9);
});

test('seed noise with half-width above 0.001 BB fails rule 2', () => {
  // Five independent seeds, mean 0.004, sample SD ~0.001186.
  const gaps = [0.0025, 0.00325, 0.004, 0.00475, 0.0055];
  const m = gaps.reduce((x, y) => x + y, 0) / SEED_COUNT;
  const sd = Math.sqrt(gaps.reduce((x, g) => x + (g - m) ** 2, 0) / (SEED_COUNT - 1));
  const hw = (T_CRIT_0_975_4 * sd) / Math.sqrt(SEED_COUNT);
  assert.ok(hw > 0.001);
  const series = LADDER.map((_, i) => rung(i, gaps));
  const verdict = evaluateCert(series);
  assert.equal(verdict.certified, false);
  const at3M = verdict.rungs[3]!;
  assert.equal(at3M.passes.r1, true);
  assert.equal(at3M.passes.r2, false);
  assert.ok(at3M.failures.includes('r2:half-width'));
  assert.ok(Math.abs(at3M.values.hw - hw) < 1e-15);
});

test('converged-but-coarse: CERT pass, QUAL fail on the exact mean', () => {
  const equity = LADDER.map((_, i) => rung(i, identical(0.004), identical(0.05)));
  const tiers = LADDER.map((_, i) => rung(i, identical(0.004), identical(0.32)));
  const cert = evaluateCert(equity);
  assert.equal(cert.certified, true);
  const qual = evaluateQual(equity, tiers, cert.rCert!);
  assert.equal(qual.passes, false);
  assert.equal(qual.meanPasses, false);
  assert.equal(qual.allSeedsStrictlyBetter, true);
  assert.ok(qual.failureReasons.includes('qual:mean-exact-gap'));
});

test('QUAL fails on one paired seed even when the exact mean passes', () => {
  const equityExact = [0.02, 0.02, 0.02, 0.02, 0.035];  // mean 0.023
  const tiersExact = [0.03, 0.03, 0.03, 0.03, 0.03];
  const equity = LADDER.map((_, i) => rung(i, identical(0.004), equityExact));
  const tiers = LADDER.map((_, i) => rung(i, identical(0.004), tiersExact));
  const qual = evaluateQual(equity, tiers, 3);
  assert.equal(qual.meanPasses, true);
  assert.equal(qual.passes, false);
  assert.equal(qual.allSeedsStrictlyBetter, false);
  assert.deepEqual(qual.paired.map((p) => p.strictlyBetter), [true, true, true, true, false]);
  assert.ok(qual.failureReasons.includes('qual:paired-superiority'));
});

test('rule 4 never certifies when a prior-window rung is missing', () => {
  const b = 0.6;
  const a = 0.03 * 3_000_000 ** b;
  // Rung 2 (1M) is missing; candidate crossings at 30M/100M are clean.
  const series = powerSeries(
    a, b,
    new Map([[4, 0.004], [5, 0.004]]),
    new Set([2]),
  );
  const verdict = evaluateCert(series);
  assert.equal(verdict.certified, false);
  assert.equal(verdict.rCert, null);
  for (const i of [3, 4, 5]) {
    const r = verdict.rungs[i]!;
    assert.equal(r.eligible, false);
    assert.ok(r.failures.includes('r4:prior-window-incomplete'));
  }
});

test('a truncated CSV cell makes its rung non-observed and rule 4 ineligible', () => {
  const dir = mkdtempSync(join(tmpdir(), 'rfc0044-'));
  const values = new Map<number, number>([
    [0, 0.05], [1, 0.03], [3, 0.01], [4, 0.004], [5, 0.004],
  ]);
  const lines: string[] = [];
  const make = (seed: number, index: number, truncated: boolean, completed?: number): string => {
    const T = LADDER[index]!;
    const fields: Record<string, string | number> = {
      mode: 'ladder', scheme: 'syn', seats: 2, street: 'river', seed,
      target_iters: T, safety_wall_s: 3600, completed: completed ?? T,
      infosets: 30, sealed_bytes: 6740, joint_deals: 38,
      max_gap_bb: values.get(index) ?? 0.01,
      sum_gap_bb: values.get(index) ?? 0.01,
      bucket_sum_gap_bb: values.get(index) ?? 0.01,
      wall_observed_s: 1, peak_rss_kb: 1000,
      truncated: truncated ? 1 : 0, note: '',
    };
    return RECOGNIZED_COLUMNS.map((c) => String(fields[c])).join(',');
  };
  // Seed 101's 1M cell is truncated, so the rung has four complete
  // seeds and is non-observed. An under-completed row (completed <
  // target despite truncated=0) is probed separately.
  for (let i = 0; i <= 5; ++i)
    for (let k = 0; k < SEED_COUNT; ++k)
      lines.push(make(SEEDS[k]!, i, i === 2 && k === 0));
  const path = join(dir, 'syn.csv');
  writeFileSync(path, [RECOGNIZED_COLUMNS.join(','), ...lines].join('\n'));
  const series = aggregateSeries(loadLadderCsv(path));
  assert.equal(series[2]!.observed, false);
  assert.equal(series[2]!.n, SEED_COUNT - 1);
  const verdict = evaluateCert(series);
  assert.equal(verdict.certified, false);
  assert.ok(verdict.rungs[3]!.failures.includes('r4:prior-window-incomplete'));

  const probe = join(dir, 'probe.csv');
  writeFileSync(probe, [
    RECOGNIZED_COLUMNS.join(','),
    make(201, 2, false, LADDER[2]! - 1),
  ].join('\n'));
  assert.equal(loadLadderCsv(probe)[0]!.truncated, true);
});

test('loader tolerates appended columns and rejects missing ones', () => {
  const dir = mkdtempSync(join(tmpdir(), 'rfc0044-'));
  const base: Record<string, string | number> = {
    mode: 'ladder', scheme: 'syn', seats: 2, street: 'river', seed: 1,
    target_iters: 100_000, safety_wall_s: 3600, completed: 100_000,
    infosets: 30, sealed_bytes: 6740, joint_deals: 38,
    max_gap_bb: 0.1, sum_gap_bb: 0.1, bucket_sum_gap_bb: 0.05,
    wall_observed_s: 1, peak_rss_kb: 1000, truncated: 0, note: '',
  };
  const extended = [...RECOGNIZED_COLUMNS, 'future_freeze_column'].join(',');
  const extendedRow = [...RECOGNIZED_COLUMNS.map((c) => String(base[c])), 'ignored'].join(',');
  const ok = join(dir, 'extended.csv');
  writeFileSync(ok, [extended, extendedRow].join('\n'));
  const rows = loadLadderCsv(ok);
  assert.equal(rows.length, 1);
  assert.equal(rows[0]!.bucket_sum_gap_bb, 0.05);
  const bad = join(dir, 'missing.csv');
  writeFileSync(bad, [RECOGNIZED_COLUMNS.filter((c) => c !== 'note').join(','), 'x'].join('\n'));
  assert.throws(() => loadLadderCsv(bad), /missing required column note/);
});

test('additive-floor fit classifies non-zero-floor and compute-ceiling', () => {
  const pts = LADDER.map((T) => ({T, g: 0.008 + 2.0 * T ** -0.6}));
  const floor = floorFit(pts);
  assert.ok(floor.c >= 0 && floor.a >= 0 && floor.b >= 0);
  assert.ok(Math.abs(floor.c - 0.008) < 1e-6, `c=${floor.c}`);
  assert.ok(Math.abs(floor.a - 2.0) < 1e-6, `a=${floor.a}`);
  assert.ok(Math.abs(floor.b - 0.6) < 1e-3, `b=${floor.b}`);
  const classified = classifyFloor(floor);
  assert.equal(classified.kind, 'non-zero-floor');
  assert.equal(classified.crossingIterations, Infinity);
  assert.equal(floorCrossingIterations({c: 0.01, a: 5, b: 0.6, sse: 0}), Infinity);

  // Slow decay that is still above EPS_GAP at 300M: crossing beyond cap.
  const slow = LADDER.map((T) => ({T, g: 10 * T ** -0.35}));
  const slowFit = floorFit(slow);
  assert.ok(slowFit.c < 1e-6, `c=${slowFit.c}`);
  assert.ok(Math.abs(slowFit.b - 0.35) < 0.01, `b=${slowFit.b}`);
  const slowClass = classifyFloor(slowFit);
  assert.equal(slowClass.kind, 'compute-ceiling');
  assert.ok(Number.isFinite(slowClass.crossingIterations));
  assert.ok(slowClass.crossingIterations > LADDER[LADDER.length - 1]!);

  // Windowed fit (last three observed rungs) stays in the feasible cone.
  const windowed = floorFit(pts, 3);
  assert.ok(windowed.c >= 0 && windowed.a >= 0 && windowed.b >= 0 && windowed.b <= 2);

  // Near-floor band (c in (0.8*EPS_GAP, EPS_GAP)): compute-ceiling class but
  // no finite crossing budget is quoted (the extrapolation is ill-conditioned).
  const nearClass = classifyFloor({c: 0.0045, a: 2, b: 0.6, sse: 0});
  assert.equal(nearClass.kind, 'compute-ceiling');
  assert.equal(nearClass.crossingIterations, Infinity);
  // c at/below 0.8*EPS_GAP quotes the finite crossing budget when it lands
  // beyond the cap; here 2/(0.005-0.004) raised to 1/0.6 is far past 300M.
  const edgeClass = classifyFloor({c: 0.004, a: 200, b: 0.6, sse: 0});
  assert.equal(edgeClass.kind, 'compute-ceiling');
  assert.ok(Number.isFinite(edgeClass.crossingIterations));
  assert.ok(edgeClass.crossingIterations > LADDER_CAP);

  // Default rule-5 window is the frozen four-rung tail.
  assert.equal(RULE5_WINDOW_RUNGS, 4);
});

test('certification CLI argument parsing keeps every CSV path', () => {
  const paths = ['a.csv', 'b.csv', 'c.csv'];
  const plain = parseCertificationArgs(paths);
  assert.deepEqual(plain.paths, paths);
  assert.equal(plain.asJson, false);
  assert.equal(plain.rule5Window, RULE5_WINDOW_RUNGS);
  const withJson = parseCertificationArgs(['--json', ...paths]);
  assert.deepEqual(withJson.paths, paths);
  assert.equal(withJson.asJson, true);
  const withWindow = parseCertificationArgs([...paths, '--rule5-window', 'all', '--json']);
  assert.deepEqual(withWindow.paths, paths);
  assert.equal(withWindow.rule5Window, Infinity);
  const withNum = parseCertificationArgs(['--rule5-window', '3', ...paths]);
  assert.deepEqual(withNum.paths, paths);
  assert.equal(withNum.rule5Window, 3);
  assert.throws(() => parseCertificationArgs(['--rule5-window', '1', ...paths]));
  assert.throws(() => parseCertificationArgs(['--rule5-window']));
});

// --- AC3 regression on the 30 retained RFC 0043 ladder CSVs -----------------

const ablationDir = new URL('../../../docs/plans/ablation/', import.meta.url);

function retainedLadderPaths(): string[] {
  const dir = fileURLToPath(ablationDir);
  return readdirSync(dir)
    .filter((f) => /^rfc0043-ladder-seats[234]-shard\d\.csv$/.test(f))
    .sort()
    .map((f) => join(dir, f));
}

test('AC3: six retained river series are NOT CERTIFIED at 10M and fits reproduce', () => {
  const paths = retainedLadderPaths();
  assert.equal(paths.length, 30);
  const rows = loadLadderCsvs(paths);
  for (const seats of [2, 3, 4]) {
    for (const scheme of ['tiers', 'equity']) {
      const groupRows = rows.filter((r) =>
        r.seats === seats && r.scheme === scheme && r.street === 'river');
      const series = aggregateSeries(groupRows);
      assert.equal(series.length, LADDER.length);
      const observed = series.filter((p) => p.observed).map((p) => p.index);
      assert.deepEqual(observed, [0, 1, 2, 3, 4]);
      const verdict = evaluateCert(series);
      assert.equal(verdict.certified, false, `${seats}p ${scheme} must not certify`);
      assert.equal(verdict.rCert, null);
      const at10M = verdict.rungs[4]!;
      assert.equal(at10M.observed, true);
      assert.equal(at10M.passes.r1, false);  // every retained 10M gap > 0.005 BB
    }
  }
  // Hand-computed RFC 0044 values: HU tail fits on 1M/3M/10M and the
  // 100M projections (section "Evidence from the retained RFC 0043 CSVs").
  const expected = new Map([
    ['tiers', {b: 0.570, projection: 0.00172}],
    ['equity', {b: 0.541, projection: 0.00471}],
  ]);
  for (const [scheme, want] of expected) {
    const series = aggregateSeries(
      rows.filter((r) => r.seats === 2 && r.scheme === scheme && r.street === 'river'));
    const tailPts = [2, 3, 4].map((i) => ({T: series[i]!.T, g: series[i]!.meanGap}));
    const fit = fitLogLinear(tailPts);
    assert.ok(Math.abs(fit.b - want.b) < 0.02, `${scheme} b=${fit.b}`);
    const projection = powerPredict(fit, 100_000_000);
    assert.ok(
      Math.abs(projection - want.projection) / want.projection < 0.10,
      `${scheme} projection=${projection}`,
    );
  }
});

test('AC3 data anchoring resolves regardless of process cwd', () => {
  const paths = retainedLadderPaths();
  assert.equal(paths.length, 30);
  assert.ok(readFileSync(paths[0]!, 'utf8').startsWith('mode,scheme,seats,street,seed'));
});
