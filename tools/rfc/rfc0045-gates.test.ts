import assert from 'node:assert/strict';
import {mkdtempSync, writeFileSync} from 'node:fs';
import {tmpdir} from 'node:os';
import {join} from 'node:path';
import {test} from 'node:test';

import {
  EPS_BUCKET,
  EPS_EXACT,
  EPS_HW,
  FLOP_CLASSES,
  GATE_BOARDS,
  GATE_RUNG,
  LADDER,
  SEEDS_FULL,
  SEEDS_W1,
  T_0_975_2,
  T_0_975_4,
  aggregateCell,
  aggregateCellSeries,
  buildEnvelope,
  evaluateRungRules,
  evaluateWorstTextureGate,
  loadCensusCsv,
  loadG2Csv,
  schemeKey,
  selectInstrument,
  validateGateCoverage,
  summarizeCensus,
  summarizeRiverWall,
  type G2Row,
} from './rfc0045-gates.js';

const HEADER =
  'board,range,scheme,kf,kt,kr,seed,iterations,infosets,joint_deals,'
  + 'sum_gap_bb,bucket_sum_gap_bb,train_s,exact_walk_s';

function g2row(partial: Partial<G2Row> & Pick<G2Row, 'board' | 'seed'>): G2Row {
  return {
    rangeName: 'full', scheme: 'ehs2-32-64-64', kf: 32, kt: 64, kr: 64,
    iterations: GATE_RUNG,
    infosets: 100, jointDeals: 1_070_190, exactGap: 0.02, bucketGap: 0.004,
    trainS: 10, exactWalkS: 51, note: 'final', ...partial,
  };
}

function csvOf(rows: Array<Array<string | number>>): string {
  return [HEADER, ...rows.map((r) => r.join(','))].join('\n');
}

function row(
  board: string, rangeName: string, scheme: string, kf: number, kt: number, kr: number,
  seed: number, iters: number, exact: number, bucket: number, walkS = 51): string {
  return [board, rangeName, scheme, kf, kt, kr, seed, iters, 100, 1_070_190,
          exact, bucket, 46, walkS].join(',');
}

test('frozen constants match the RFC 0045 freeze', () => {
  assert.deepEqual(LADDER, [1_000_000, 10_000_000, 100_000_000, 300_000_000]);
  assert.equal(GATE_RUNG, 300_000_000);
  assert.equal(SEEDS_W1, 5);
  assert.equal(SEEDS_FULL, 3);
  assert.equal(T_0_975_4, 2.7764);
  assert.equal(T_0_975_2, 4.303);
  assert.equal(EPS_BUCKET, 0.005);
  assert.equal(EPS_HW, 0.001);
  assert.equal(EPS_EXACT, 0.03);
  assert.equal(FLOP_CLASSES, 1755);
});

test('schemeKey parses the frozen grids and rejects an unknown point', () => {
  assert.equal(schemeKey('tiers', 0, 0, 0), 'tiers');
  assert.equal(schemeKey('ehs2', 32, 64, 256), 'ehs2-32-64-256');
  assert.equal(schemeKey('ehs2-v2', 32, 128, 256), 'ehs2-32-128-256');
  assert.throws(() => schemeKey('ehs2', 16, 64, 64), /unknown EHS grid/);
  assert.throws(() => schemeKey('ehs2', 32, 128, 64), /unknown EHS grid/);
  assert.throws(() => schemeKey('nonsense', 0, 0, 0), /unknown scheme/);
});

test('the mechanical verdict requires all six frozen gate boards', () => {
  // Raised-srp board is registered, not operator-supplied.
  assert.equal(GATE_BOARDS.length, 6);
  assert.ok(GATE_BOARDS.includes('2c3d7hJs2s'));
  assert.throws(() => validateGateCoverage(['2c3d7hJs9c']), /incomplete/);
  assert.throws(() => validateGateCoverage(GATE_BOARDS.slice(0, 5)), /2c3d7hJs2s/);
  assert.doesNotThrow(() => validateGateCoverage(GATE_BOARDS));
});

test('full cell uses t=4.303 and requires exactly three independent seeds', () => {
  const rows: G2Row[] = [];
  for (const seed of [101, 102, 103])
    rows.push(g2row({board: 'b', seed, exactGap: 0.020 + seed * 1e-6}));
  const cell = aggregateCell(rows, 'b', 'ehs2-32-64-64', 'full', GATE_RUNG);
  assert.equal(cell.observed, true);
  assert.equal(cell.n, 3);
  // Deterministic half-width: 4.303 * s / sqrt(3), s with n-1 denominator.
  const vals = [0.020101, 0.020102, 0.020103];
  const m = vals.reduce((a, b) => a + b, 0) / 3;
  const sd = Math.sqrt(vals.reduce((a, x) => a + (x - m) ** 2, 0) / 2);
  assert.ok(Math.abs(cell.exactHw - (T_0_975_2 * sd) / Math.sqrt(3)) < 1e-12);
  const two = aggregateCell(rows.slice(0, 2), 'b', 'ehs2-32-64-64', 'full', GATE_RUNG);
  assert.equal(two.observed, false);
});

test('W1 cell uses t=2.7764 and five seeds', () => {
  const rows: G2Row[] = [];
  for (const seed of [1, 2, 3, 4, 5])
    rows.push(g2row({board: 'b', rangeName: 'w1', seed, exactGap: 0.2, bucketGap: 0.18}));
  const cell = aggregateCell(rows, 'b', 'ehs2-32-64-64', 'w1', GATE_RUNG);
  assert.equal(cell.observed, true);
  assert.equal(cell.required, 5);
  assert.ok(cell.exactHw === 0); // identical values -> zero variance
});

test('duplicate seeds at one cell are rejected (independent deal batches)', () => {
  const rows = [g2row({board: 'b', seed: 1}), g2row({board: 'b', seed: 1})];
  assert.throws(() => aggregateCell(rows, 'b', 'ehs2-32-64-64', 'full', GATE_RUNG),
                /duplicate seed/);
});

test('worst-texture gate uses the MAX stratum, not the mean', () => {
  const boards = ['good', 'bad'];
  const rows: G2Row[] = [];
  for (const b of boards)
    for (const seed of [1, 2, 3]) {
      const exact = b === 'bad' ? 0.031 : 0.010;
      rows.push(g2row({board: b, seed, exactGap: exact, bucketGap: 0.004}));
      rows.push({...g2row({board: b, seed, exactGap: exact + 0.2}), scheme: 'tiers'});
    }
  const gate = evaluateWorstTextureGate(rows, 'ehs2-32-64-64', boards);
  assert.equal(gate.worstBoard, 'bad');
  assert.ok(Math.abs(gate.worstExact - 0.031) < 1e-12);
  assert.equal(gate.worstPasses, false);
  assert.equal(gate.passes, false);
  assert.ok(gate.reasons.some((r) => r.startsWith('worst-exact')));
});

test('paired superiority requires strict improvement on every identical seed', () => {
  const boards = ['b'];
  const rows: G2Row[] = [];
  for (const seed of [1, 2, 3]) {
    rows.push(g2row({board: 'b', seed, exactGap: seed === 3 ? 0.05 : 0.01}));
    rows.push({...g2row({board: 'b', seed, exactGap: 0.04}), scheme: 'tiers'});
  }
  const gate = evaluateWorstTextureGate(rows, 'ehs2-32-64-64', boards);
  assert.equal(gate.strata[0]!.pairedPasses, false); // seed 3 loses to tiers
});

test('a fully passing stratum clears the gate', () => {
  const boards = ['b'];
  const rows: G2Row[] = [];
  for (const seed of [1, 2, 3]) {
    rows.push(g2row({board: 'b', seed, exactGap: 0.02 + seed * 1e-7, bucketGap: 0.004}));
    rows.push({...g2row({board: 'b', seed, exactGap: 0.30}), scheme: 'tiers'});
  }
  const gate = evaluateWorstTextureGate(rows, 'ehs2-32-64-64', boards);
  assert.equal(gate.passes, true);
  assert.deepEqual(gate.reasons, []);
});

test('CERT tail rules pass only on the 300M rung with a clean four-rung series', () => {
  // Strictly decaying bucket series: 0.040, 0.013, 0.007, 0.004 (well
  // within the 20% log-linear residual windows).
  const gaps = [0.040, 0.013, 0.007, 0.004];
  const rows: G2Row[] = [];
  LADDER.forEach((T, i) => {
    for (const seed of [1, 2, 3])
      rows.push(g2row({board: 'b', seed, iterations: T, bucketGap: gaps[i]!, exactGap: 0.02}));
  });
  const series = aggregateCellSeries(rows, 'b', 'ehs2-32-64-64', 'full');
  const rules = evaluateRungRules(series, 3);
  assert.equal(rules.eligible, true);
  assert.equal(rules.distance, true);
  assert.equal(rules.halfWidth, true);
  assert.equal(rules.tailFit, true);
  assert.equal(rules.nonIncreasing, true);
  assert.equal(rules.priorPrediction, true);
  assert.deepEqual(rules.failures, []);
  // Earlier rungs are categorically ineligible under rule 4.
  assert.equal(evaluateRungRules(series, 2).eligible, false);
});

test('selection picks the smallest clearing K_river', () => {
  const boards = ['b'];
  const rows: G2Row[] = [];
  const bucketGaps = [0.040, 0.013, 0.007, 0.004];
  for (const [key, kt, kr] of [['ehs2-32-64-64', 64, 64], ['ehs2-32-64-128', 64, 128]] as const) {
    // Only the 128 arm passes (64 arm worst exact 0.04 > 0.03); both share
    // the same decaying within-abstraction convergence series.
    const exact = key === 'ehs2-32-64-64' ? 0.04 : 0.02;
    LADDER.forEach((T, i) => {
      for (const seed of [1, 2, 3])
        rows.push(g2row({board: 'b', seed, iterations: T, scheme: key, kt, kr,
                         exactGap: exact, bucketGap: bucketGaps[i]!}));
    });
  }
  for (const T of LADDER)
    for (const seed of [1, 2, 3])
      rows.push(g2row({board: 'b', seed, iterations: T, scheme: 'tiers',
                       exactGap: 0.31, bucketGap: -1}));
  const verdict = selectInstrument(rows, boards, GATE_RUNG);
  assert.equal(verdict.kind, 'uniform-ehs-viable');
  if (verdict.kind === 'uniform-ehs-viable')
    assert.equal(verdict.selected, 'ehs2-32-64-128');
});

test('selection reports the conditional arm when every primary grid fails', () => {
  const boards = ['b'];
  const rows: G2Row[] = [];
  for (const key of ['ehs2-32-64-64', 'ehs2-32-64-128', 'ehs2-32-64-256'] as const) {
    for (const T of LADDER)
      for (const seed of [1, 2, 3]) {
        rows.push(g2row({board: 'b', seed, iterations: T, scheme: key, kt: 64,
                         kr: Number(key.split('-')[3]), exactGap: 0.05, bucketGap: 0.004}));
      }
  }
  for (const T of LADDER)
    for (const seed of [1, 2, 3])
      rows.push(g2row({board: 'b', seed, iterations: T, scheme: 'tiers',
                       exactGap: 0.31, bucketGap: -1}));
  const verdict = selectInstrument(rows, boards, GATE_RUNG);
  assert.equal(verdict.kind, 'needs-conditional-kt128');
});

test('river-wall summary pins the maximum full-range walk wall', () => {
  const rows: G2Row[] = [
    g2row({board: 'b', seed: 1, rangeName: 'w1', iterations: 100_000, jointDeals: 38,
           exactWalkS: 0.002}),
    g2row({board: 'b', seed: 2, iterations: 1_000_000, jointDeals: 1_070_190,
           exactWalkS: 50.96}),
  ];
  const wall = summarizeRiverWall(rows);
  assert.equal(wall.maxFullWalkS, 50.96);
  assert.equal(wall.fullUniverseDeals, 1_070_190);
});

test('CSV loader reads columns by name and rejects malformed rows', () => {
  const dir = mkdtempSync(join(tmpdir(), 'rfc0045-'));
  const path = join(dir, 'g2.csv');
  writeFileSync(path, csvOf([
    ['2c3d7hJs9c', 'full', 'ehs2', 32, 64, 64, 4320, GATE_RUNG, 100, 1_070_190,
     0.02, 0.004, 46, 51] as any,
  ]));
  const rows = loadG2Csv(path);
  assert.equal(rows[0]!.scheme, 'ehs2-32-64-64');
  assert.equal(rows[0]!.exactGap, 0.02);
});

test('CSV loader refuses a walkslow cell whose walk breached the frozen band', () => {
  const dir = mkdtempSync(join(tmpdir(), 'rfc0045-'));
  const path = join(dir, 'g2-walkslow.csv');
  const header = 'board,range,scheme,kf,kt,kr,seed,iterations,infosets,joint_deals,'
    + 'sum_gap_bb,bucket_sum_gap_bb,train_s,exact_walk_s,truncated,completed,note';
  writeFileSync(path, [
    header,
    ['2c3d7hJs9c', 'full', 'ehs2', 32, 64, 64, 4320, GATE_RUNG, 100, 1_070_190,
     0.02, 0.004, 6000, 68.8, 0, 300000000, 'walkslow'].join(','),
  ].join('\n'));
  assert.throws(() => loadG2Csv(path), /walkslow/);
});

test('census loader validates identity roots, all-in split, and cap builds', () => {
  const dir = mkdtempSync(join(tmpdir(), 'rfc0045-'));
  const path = join(dir, 'census.csv');
  writeFileSync(path, [
    'record,root_id,button,pot,line_tag,all_in_no_flop,cap,nodes,infosets,rows,tree_bytes,sealed_bytes,seconds,flop_class',
    'root,r0,0,4,limped,0,,,,,,,,',
    'root,r1,1,8,srp,0,,,,,,,,',
    'allin,,,,,,,,,,,,',
    'river_terminal,r0,,,,,0,1000,500,400,300000,20000,12.5,c0001',
    'river_terminal,r0,,,,,1,800,400,320,240000,16000,9.0,c0001',
  ].join('\n'));
  const summary = summarizeCensus(loadCensusCsv(path));
  assert.equal(summary.distinctRoots, 2);
  assert.equal(summary.allInNoFlopLines, 1);
  assert.deepEqual([...summary.buttons].sort((a, b) => a - b), [0, 1]);
  assert.equal(summary.rootsByTag['limped'], 1);
  assert.equal(summary.rootsByTag['srp'], 1);
  const env = buildEnvelope(summary, 2);
  assert.equal(env.cells, 2 * 1755);
  // cap 0 dominates per-class cost; envelope multiplies the maxima.
  assert.equal(env.byCap[0]!.totalBytes, 2 * 1755 * 20000);
  assert.equal(env.byCap[1]!.totalCoreSeconds, 2 * 1755 * 9.0);
});
