import assert from 'node:assert/strict';
import {mkdtempSync, writeFileSync} from 'node:fs';
import {tmpdir} from 'node:os';
import {join} from 'node:path';
import {afterEach, test} from 'node:test';

import {
  bcaBootstrapMean,
  BOOTSTRAP_REPLICATES,
  evaluateCandidate,
  loadAblationCsv,
  pairedStats,
  pilotAnalysis,
  PRIMARY_HALFWIDTH_TARGET,
  NONINFERIORITY_HALFWIDTH_TARGET,
} from './rfc0042-gates.js';

const HEADER =
  'scheme,seats,street,seed,wall_ms,completed,infosets,sealed_bytes,joint_deals,' +
  'max_gap_bb,sum_gap_bb,bucket_sum_gap_bb,note';

function row(
  scheme: string,
  seats: number,
  street: 'river' | 'turn',
  seed: number,
  fields: {completed?: bigint; bytes?: number; sum?: number; bucket?: number},
): string {
  const completed = fields.completed ?? 100000n;
  const bytes = fields.bytes ?? 6740;
  const sum = fields.sum ?? 0.3;
  const bucket = fields.bucket ?? -1;
  return [
    scheme, seats, street, seed, 10000, completed.toString(), 30, bytes, 38,
    sum.toFixed(6), sum.toFixed(6), bucket.toFixed(6), '',
  ].join(',');
}

let dirs: string[] = [];
afterEach(() => {
  dirs = [];
});

function writeCsv(name: string, lines: string[]): string {
  const dir = mkdtempSync(join(tmpdir(), 'rfc0042-'));
  const path = join(dir, name);
  writeFileSync(path, [HEADER, ...lines].join('\n'));
  dirs.push(dir);
  return path;
}

function symmetricCsv(scheme: string, values: number[]): string {
  // All four gated shapes per seed, exact sum = bucket sum = v, equal
  // bytes and iterations so non-inferiority and cost gates pass.
  const shapes: Array<[number, 'river' | 'turn']> = [
    [2, 'river'], [2, 'turn'], [3, 'river'], [3, 'turn'],
  ];
  const lines: string[] = [];
  for (const [seats, street] of shapes)
    values.forEach((v, i) =>
      lines.push(row(scheme, seats, street, 100 + i, {sum: v, bucket: v, bytes: 1000})),
    );
  return writeCsv(`${scheme}.csv`, lines);
}

test('paired stats compute the mean and sample SD', () => {
  const s = pairedStats([1, 2, 3, 4, 5]);
  assert.equal(s.n, 5);
  assert.ok(Math.abs(s.mean - 3) < 1e-12);
  assert.ok(Math.abs(s.sd - Math.sqrt(2.5)) < 1e-12);
});

test('bootstrap interval is deterministic, ordered, and tight on a constant series', () => {
  const d = [0.1, 0.1, 0.1, 0.1, 0.1];
  const a = bcaBootstrapMean(d);
  const b = bcaBootstrapMean(d);
  assert.deepEqual(a, b);
  assert.ok(a.lower <= a.mean && a.mean <= a.upper);
  assert.ok(Math.abs(a.mean - 0.1) < 1e-12);
  assert.ok(Math.abs(a.lower - 0.1) < 1e-12);
  assert.ok(Math.abs(a.upper - 0.1) < 1e-12);
  assert.equal(a.tLower, a.tUpper);
});

test('bootstrap uses exactly 10,000 deterministic replicates', () => {
  assert.equal(BOOTSTRAP_REPLICATES, 10_000);
  const d = Array.from({length: 20}, (_, i) => Math.sin(i));
  const a = bcaBootstrapMean(d);
  const b = bcaBootstrapMean(d);
  assert.equal(a.lower, b.lower);
  assert.equal(a.upper, b.upper);
});

test('a clearly positive paired improvement passes the primary gate', () => {
  const tiers = symmetricCsv('tiers', [0.33, 0.34, 0.32, 0.33, 0.35]);
  const cand = symmetricCsv('equity', [0.13, 0.12, 0.14, 0.13, 0.12]);
  const tiersRows = loadAblationCsv(tiers);
  const candRows = loadAblationCsv(cand);
  // Bucket sums tiny for both so gate 3 passes; supply the other three
  // shapes as equal rows (zero difference) so non-inferiority passes.
  const report = evaluateCandidate(tiersRows, candRows, 'equity');
  assert.ok(report.gate1Primary);
  assert.equal(report.gate1Primary.pass, true);
  assert.ok(report.gate1Primary.mean > 0.19);
  assert.ok(report.gate1Primary.lower > 0);
});

test('gate 3 fails when the 3-seat river bucket sum exceeds 0.05', () => {
  const tiersRows: string[] = [];
  const candRows: string[] = [];
  for (let i = 0; i < 5; ++i) {
    tiersRows.push(row('tiers', 2, 'river', 100 + i, {sum: 0.33, bucket: 0.03}));
    tiersRows.push(row('tiers', 2, 'turn', 100 + i, {sum: 0.30}));
    tiersRows.push(row('tiers', 3, 'river', 100 + i, {sum: 0.71, bucket: 0.14}));
    tiersRows.push(row('tiers', 3, 'turn', 100 + i, {sum: 0.80}));
    candRows.push(row('equity', 2, 'river', 100 + i, {sum: 0.13, bucket: 0.097}));
    candRows.push(row('equity', 2, 'turn', 100 + i, {sum: 0.19}));
    candRows.push(row('equity', 3, 'river', 100 + i, {sum: 0.38, bucket: 0.376}));
    candRows.push(row('equity', 3, 'turn', 100 + i, {sum: 0.81}));
  }
  const report = evaluateCandidate(
    loadAblationCsv(writeCsv('tiers.csv', tiersRows)),
    loadAblationCsv(writeCsv('equity.csv', candRows)),
    'equity',
  );
  const g3river = report.gate3Convergence.find((g) => g.shape === '2p-river');
  const g3three = report.gate3Convergence.find((g) => g.shape === '3p-river');
  assert.equal(g3river?.pass, false);
  assert.equal(g3three?.pass, false);
  assert.equal(report.gates1to4, false);
});

test('pilot analysis projects the frozen half-width targets', () => {
  const tiers: string[] = [];
  const equity: string[] = [];
  for (let i = 0; i < 5; ++i) {
    tiers.push(row('tiers', 2, 'river', 200 + i, {sum: 0.33}));
    tiers.push(row('tiers', 2, 'turn', 200 + i, {sum: 0.30}));
    tiers.push(row('tiers', 3, 'river', 200 + i, {sum: 0.71}));
    tiers.push(row('tiers', 3, 'turn', 200 + i, {sum: 0.80}));
    equity.push(row('equity', 2, 'river', 200 + i, {sum: 0.13}));
    equity.push(row('equity', 2, 'turn', 200 + i, {sum: 0.19}));
    equity.push(row('equity', 3, 'river', 200 + i, {sum: 0.38}));
    equity.push(row('equity', 3, 'turn', 200 + i, {sum: 0.80 + 0.001 * (i - 2)}));
  }
  const cells = pilotAnalysis(writeCsv('tiers.csv', tiers), [
    {scheme: 'equity', path: writeCsv('equity.csv', equity)},
  ]);
  assert.equal(cells.length, 4);
  const primary = cells.find((c) => c.seats === 2 && c.street === 'river');
  assert.equal(primary?.target, PRIMARY_HALFWIDTH_TARGET);
  for (const c of cells.filter((x) => !(x.seats === 2 && x.street === 'river')))
    assert.equal(c.target, NONINFERIORITY_HALFWIDTH_TARGET);
  assert.equal(cells.every((c) => Number.isFinite(c.projectedHalfWidth32)), true);
});

test('loadAblationCsv rejects a drifted header and a bad street', () => {
  const dir = mkdtempSync(join(tmpdir(), 'rfc0042-'));
  const badHeaderPath = join(dir, 'bad.csv');
  writeFileSync(badHeaderPath, 'not,the,header,,,,,,,,,,\n');
  dirs.push(dir);
  assert.throws(() => loadAblationCsv(badHeaderPath), /unexpected header/);
  const badRow = writeCsv('bad2.csv', [row('tiers', 2, 'flop' as never, 1, {})]);
  assert.throws(() => loadAblationCsv(badRow), /bad street/);
});

