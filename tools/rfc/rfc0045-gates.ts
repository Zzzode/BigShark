// RFC 0045 M2 instrument resolution: frozen G2 worst-texture gate,
// Stage-1 river-walk smoke ingestion, G3 root-census ingestion, and the
// G4 bounded envelope. The RFC 0044 evaluator is left unchanged; this
// file only imports its pinned log-linear fit helpers.
//
// G2 campaign CSV columns are read BY NAME (the pre-run freeze addendum
// may append columns):
//   board,range,scheme,kf,kt,kr,seed,iterations,infosets,joint_deals,
//   sum_gap_bb,bucket_sum_gap_bb,train_s,exact_walk_s
//
// `range` is `w1` (five-seed W1 regression, t = 2.7764) or `full`
// (three independent seeds with independent deal batches,
// t = 4.303). The exact gate is evaluated on chance-free RIVER rows
// only; turn rows stay descriptive and never gate.
import {readFileSync} from 'node:fs';
// The RFC 0044 evaluator cannot be imported: its module body runs its own
// CLI dispatch, which would intercept this evaluator's argv. The three
// frozen RFC 0044 math helpers are therefore reproduced verbatim here and
// pinned by the tail-rule tests below.
function mean(xs: number[]): number {
  return xs.reduce((a, b) => a + b, 0) / xs.length;
}

export interface PowerFit {
  a: number; // g(T) = a * T^-b
  b: number;
}

export interface FitPoint {
  T: number;
  g: number;
}

// Ordinary least squares on ln g = ln a - b ln T (identical to the
// RFC 0044 evaluator's fitLogLinear).
export function fitLogLinear(points: FitPoint[]): PowerFit {
  if (points.length < 2)
    throw new Error('fitLogLinear: need at least two points');
  const xs = points.map((p) => Math.log(p.T));
  const ys = points.map((p) => Math.log(p.g));
  const n = points.length;
  const mx = mean(xs);
  const my = mean(ys);
  let num = 0;
  let den = 0;
  for (let i = 0; i < n; ++i) {
    num += (xs[i]! - mx) * (ys[i]! - my);
    den += (xs[i]! - mx) ** 2;
  }
  if (den === 0)
    throw new Error('fitLogLinear: degenerate T coordinates');
  const slope = num / den;
  return {a: Math.exp(my - slope * mx), b: -slope};
}

export function powerPredict(fit: PowerFit, T: number): number {
  return fit.a * T ** -fit.b;
}

export function maxRelResidual(fit: PowerFit, points: FitPoint[]): number {
  let worst = 0;
  for (const p of points) {
    const predicted = powerPredict(fit, p.T);
    worst = Math.max(worst, Math.abs(predicted - p.g) / p.g);
  }
  return worst;
}

// --- Frozen predicate constants (RFC 0045 sections 2,4) --------------------

export const LADDER: readonly number[] = [1_000_000, 10_000_000, 100_000_000, 300_000_000];
export const GATE_RUNG_INDEX = 3; // rule 4 on four rungs: only 300M eligible
export const GATE_RUNG = LADDER[GATE_RUNG_INDEX]!;
export const SEEDS_W1 = 5;
export const SEEDS_FULL = 3;
export const T_0_975_4 = 2.7764; // W1: df = n-1 = 4
export const T_0_975_2 = 4.303;  // full range: df = n-1 = 2
export const EPS_BUCKET = 0.005; // BB within-abstraction bar
export const EPS_HW = 0.001;     // BB independent-seed half-width bar
export const EPS_EXACT = 0.03;   // BB HU worst-stratum exact bar
export const FIT_REL_TOLERANCE = 0.20;
export const FLOP_CLASSES = 1755;

// The frozen full-range texture strata (section 2). W1 anchors the
// RFC 0044 regression; the four canonical boards are each completed by
// the W1 turn Js / river Js9c. The raised-root stratum is added by the
// freeze addendum via the deterministic anti-peek selector and may be
// recorded as absent (never substituted).
export interface TextureStratum {
  name: string;
  flop: string;   // three concatenated card names
  turn: string;   // four cards
  river: string;  // five cards (chance-free gate board)
  // False for the raised-root stratum: it was not a fixed board but the
  // deterministic RFC section-2 selector applied to the committed G3 census.
  // The selection result is itself frozen for this campaign.
  fixed: boolean;
}

// The sixth stratum was selected data-blind by the RFC section-2 rule over
// the committed G3 census: smallest-pot raised tuple (root_id 1, button 0,
// SRP, pot 8) completed by the lexicographically smallest non-blocking
// river card (2s). It is registered in the evaluator so a default `g2`
// verdict mechanically covers ALL SIX frozen boards; omitting it makes a
// verdict invalid (see validateGateCoverage).
export const STRATA: readonly TextureStratum[] = [
  {name: 'w1', flop: '2c3d7h', turn: '2c3d7hJs', river: '2c3d7hJs9c', fixed: true},
  {name: 'unpaired-rainbow-ahigh', flop: '7sKhAd', turn: '7sKhAdJs', river: '7sKhAdJs9c', fixed: true},
  {name: 'paired', flop: '7sKhKd', turn: '7sKhKdJs', river: '7sKhKdJs9c', fixed: true},
  {name: 'monotone', flop: 'QsKsAs', turn: 'QsKsAsJs', river: 'QsKsAsJs9c', fixed: true},
  {name: 'unpaired-twotone', flop: '7sKhAs', turn: '7sKhAsJs', river: '7sKhAsJs9c', fixed: true},
  {name: 'raised-srp', flop: '2c3d7h', turn: '2c3d7hJs', river: '2c3d7hJs2s', fixed: false},
];

// The six river boards every mechanical G2 verdict must cover.
export const GATE_BOARDS: readonly string[] = STRATA.map((s) => s.river);

// Tested EHS grids (section 4). K_flop pinned at 32; the primary axis is
// K_river; the K_turn=128 arm runs only if K_river=256 with K_turn=64
// fails G2.
export type SchemeKey =
  | 'tiers'
  | 'ehs2-32-64-64'
  | 'ehs2-32-64-128'
  | 'ehs2-32-64-256'
  | 'ehs2-32-128-256';

export interface GridScheme {
  key: SchemeKey;
  incumbent: boolean;
  kf: number;
  kt: number;
  kr: number;
  conditional: boolean; // only eligible after the primary 256 arm fails
}

export const SCHEMES: readonly GridScheme[] = [
  {key: 'tiers', incumbent: true, kf: 0, kt: 0, kr: 0, conditional: false},
  {key: 'ehs2-32-64-64', incumbent: false, kf: 32, kt: 64, kr: 64, conditional: false},
  {key: 'ehs2-32-64-128', incumbent: false, kf: 32, kt: 64, kr: 128, conditional: false},
  {key: 'ehs2-32-64-256', incumbent: false, kf: 32, kt: 64, kr: 256, conditional: false},
  {key: 'ehs2-32-128-256', incumbent: false, kf: 32, kt: 128, kr: 256, conditional: true},
];

export function schemeKey(scheme: string, kf: number, kt: number, kr: number): SchemeKey {
  if (scheme === 'tiers')
    return 'tiers';
  if (scheme === 'ehs2' || scheme === 'equity' || scheme === 'ehs2-v2') {
    const key = `ehs2-${kf}-${kt}-${kr}`;
    for (const s of SCHEMES)
      if (s.key === key)
        return s.key;
    throw new Error(`unknown EHS grid ${kf}/${kt}/${kr}`);
  }
  throw new Error(`unknown scheme ${scheme}`);
}

// --- Campaign CSV -----------------------------------------------------------

const G2_COLUMNS = [
  'board', 'range', 'scheme', 'kf', 'kt', 'kr', 'seed', 'iterations',
  'infosets', 'joint_deals', 'sum_gap_bb', 'bucket_sum_gap_bb',
  'train_s', 'exact_walk_s',
] as const;

export interface G2Row {
  board: string;
  rangeName: string;
  scheme: SchemeKey;
  kf: number;
  kt: number;
  kr: number;
  seed: number;
  iterations: number;
  infosets: number;
  jointDeals: number;
  exactGap: number;     // chance-free river exact-hole summed gap (BB)
  bucketGap: number;    // within-abstraction gap; -1 = not applicable
  trainS: number;
  exactWalkS: number;
  note: string;  // 'final' | 'walkslow' (full walk breached the frozen band)
}

export function loadG2Csv(path: string): G2Row[] {
  // A completely empty file (a campaign shard whose header has not been
  // flushed yet) contributes no rows; a malformed/garbage file below still
  // fails validation once any content is present.
  const raw = readFileSync(path, 'utf8');
  if (raw.trim().length === 0)
    return [];
  const lines = raw
    .split('\n')
    .map((l) => l.trim())
    .filter((l) => l.length > 0 && !l.startsWith('#'));
  if (lines.length === 0)
    return [];
  // Header-only shard (a campaign shard that has not produced a cell yet):
  // contribute zero rows rather than fail the whole aggregation.
  if (lines.length === 1)
    return [];
  const header = lines[0]!.split(',').map((h) => h.trim());
  const idx = new Map<string, number>();
  header.forEach((name, i) => {
    if (idx.has(name))
      throw new Error(`${path}: duplicate column ${name}`);
    idx.set(name, i);
  });
  for (const c of G2_COLUMNS) {
    if (!idx.has(c))
      throw new Error(`${path}: missing required column ${c}`);
  }
  const at = (f: string[], c: (typeof G2_COLUMNS)[number]): string => {
    const v = f[idx.get(c)!];
    if (v === undefined)
      throw new Error(`${path}: row missing field ${c}`);
    return v;
  };
  const num = (f: string[], c: (typeof G2_COLUMNS)[number]): number => {
    const v = Number(at(f, c));
    if (!Number.isFinite(v))
      throw new Error(`${path}: non-numeric ${c}`);
    return v;
  };
  const rows: G2Row[] = [];
  const hasTruncated = idx.has('truncated');
  const noteIdx = idx.get('note');
  for (const line of lines.slice(1)) {
    const f = line.split(',');
    // Segment receipts (truncated=1) are not finished cells; the resume
    // protocol emits a `final` row on completion. Ignore the receipts.
    if (hasTruncated && (f[idx.get('truncated')!] ?? '0') !== '0')
      continue;
    const note = noteIdx !== undefined ? (f[noteIdx] ?? '').trim() : 'final';
    // Frozen addendum section 6: a full walk outside 50.996 s +/-20% must be
    // re-run; never score a gate from a cell that exhausted its retries.
    if (note === 'walkslow')
      throw new Error(`${path}: row carries note=walkslow; the exact walk breached the frozen `
        + `band and the cell must be re-run before this CSV can be evaluated`);
    const rangeName = at(f, 'range');
    if (rangeName !== 'full' && rangeName !== 'w1')
      throw new Error(`${path}: range must be full|w1 (got ${rangeName})`);
    const kf = num(f, 'kf');
    const kt = num(f, 'kt');
    const kr = num(f, 'kr');
    rows.push({
      board: at(f, 'board'),
      rangeName,
      scheme: schemeKey(at(f, 'scheme'), kf, kt, kr),
      kf, kt, kr,
      seed: num(f, 'seed'),
      iterations: num(f, 'iterations'),
      infosets: num(f, 'infosets'),
      jointDeals: num(f, 'joint_deals'),
      exactGap: num(f, 'sum_gap_bb'),
      bucketGap: num(f, 'bucket_sum_gap_bb'),
      trainS: num(f, 'train_s'),
      exactWalkS: num(f, 'exact_walk_s'),
      note,
    });
  }
  return rows;
}

export function loadG2Csvs(paths: string[]): G2Row[] {
  return paths.flatMap((p) => loadG2Csv(p));
}

// --- Independent-seed aggregation -------------------------------------------

export interface CellPoint {
  board: string;
  scheme: SchemeKey;
  rangeName: string;
  iterations: number;
  observed: boolean;
  n: number;
  required: number;
  exactMean: number;
  exactHw: number;
  bucketMean: number; // NaN when not applicable
  bucketHw: number;
  perSeedExact: Map<number, number>;
  perSeedBucket: Map<number, number>;
  seeds: number[];
}

function sampleSdN1(xs: number[]): number {
  if (xs.length < 2)
    return 0;
  const m = mean(xs);
  return Math.sqrt(xs.reduce((a, x) => a + (x - m) ** 2, 0) / (xs.length - 1));
}

// Aggregate one (board, scheme, range, rung) cell. Each seed contributes
// once; a duplicate seed is an error (the independent deal-batch
// contract is per seed). `required` selects the t multiplier.
export function aggregateCell(
  rows: G2Row[],
  board: string,
  scheme: SchemeKey,
  rangeName: string,
  iterations: number,
): CellPoint {
  const required = rangeName === 'full' ? SEEDS_FULL : SEEDS_W1;
  const tCrit = rangeName === 'full' ? T_0_975_2 : T_0_975_4;
  const inCell = rows.filter(
    (r) => r.board === board && r.scheme === scheme && r.rangeName === rangeName
      && r.iterations === iterations,
  );
  const perSeedExact = new Map<number, number>();
  const perSeedBucket = new Map<number, number>();
  for (const r of inCell) {
    if (perSeedExact.has(r.seed))
      throw new Error(`duplicate seed ${r.seed} for ${board}|${scheme}|${rangeName}|${iterations}`);
    perSeedExact.set(r.seed, r.exactGap);
    perSeedBucket.set(r.seed, r.bucketGap);
  }
  const seeds = [...perSeedExact.keys()].sort((a, b) => a - b);
  const exacts = seeds.map((s) => perSeedExact.get(s)!);
  const buckets = seeds.map((s) => perSeedBucket.get(s)!).filter((g) => Number.isFinite(g) && g >= 0);
  const exactHw = seeds.length ? (tCrit * sampleSdN1(exacts)) / Math.sqrt(exacts.length) : NaN;
  const bucketDefined = buckets.length === seeds.length && seeds.length > 0;
  const bucketHw = bucketDefined
    ? (tCrit * sampleSdN1(buckets)) / Math.sqrt(buckets.length)
    : NaN;
  return {
    board, scheme, rangeName, iterations,
    observed: seeds.length === required,
    n: seeds.length, required,
    exactMean: seeds.length ? mean(exacts) : NaN,
    exactHw,
    bucketMean: bucketDefined ? mean(buckets) : NaN,
    bucketHw,
    perSeedExact, perSeedBucket, seeds,
  };
}

// --- CERT-style rung eligibility on the reduced four-rung ladder -----------

// The RFC 0044 tail rules are retained: with four rungs the earliest
// rule-4 eligible rung is index 3 (300M). Evaluated on the full-range
// BUCKET gap series (within-abstraction convergence). Turn rows never
// gate; here the caller restricts to river rows.
export interface RungRules {
  eligible: boolean;
  distance: boolean; // bucket mean <= 0.005
  halfWidth: boolean;
  tailFit: boolean;  // 3-rung log-linear tail
  nonIncreasing: boolean;
  priorPrediction: boolean;
  failures: string[];
}

function nonIncreasing(xs: number[]): boolean {
  for (let i = 1; i < xs.length; ++i) {
    if (xs[i - 1]! + 1e-12 * Math.abs(xs[i - 1]!) < xs[i]!)
      return false;
  }
  return true;
}

export function evaluateRungRules(fullSeries: readonly CellPoint[], index: number): RungRules {
  const failures: string[] = [];
  const point = fullSeries[index];
  if (!point || !point.observed || index !== GATE_RUNG_INDEX)
    return {
      eligible: false, distance: false, halfWidth: false, tailFit: false,
      nonIncreasing: false, priorPrediction: false,
      failures: ['ineligible:not-300M-or-unobserved'],
    };
  const distance = Number.isFinite(point.bucketMean) && point.bucketMean <= EPS_BUCKET;
  if (!distance)
    failures.push('bucket-distance');
  const halfWidth = Number.isFinite(point.bucketHw) && point.bucketHw <= EPS_HW;
  if (!halfWidth)
    failures.push('bucket-half-width');
  const tailPts = fullSeries.slice(index - 2, index + 1)
    .map((p) => ({T: p.iterations, g: p.bucketMean}));
  let tailFit = false;
  let mono = false;
  if (tailPts.every((p) => Number.isFinite(p.g))) {
    const fit = fitLogLinear(tailPts);
    tailFit = fit.b >= 0 && fit.b <= 1.5 && maxRelResidual(fit, tailPts) <= FIT_REL_TOLERANCE;
    mono = nonIncreasing(tailPts.map((p) => p.g));
  }
  if (!tailFit)
    failures.push('tail-fit');
  if (!mono)
    failures.push('not-non-increasing');
  const priorPts = fullSeries.slice(index - 3, index)
    .map((p) => ({T: p.iterations, g: p.bucketMean}));
  let priorPrediction = false;
  if (priorPts.length === 3 && priorPts.every((p) => Number.isFinite(p.g))) {
    const fit = fitLogLinear(priorPts);
    const predicted = powerPredict(fit, point.iterations);
    priorPrediction = Math.abs(point.bucketMean - predicted) / predicted <= FIT_REL_TOLERANCE;
  }
  if (!priorPrediction)
    failures.push('prior-prediction');
  return {
    eligible: true, distance, halfWidth, tailFit, nonIncreasing: mono,
    priorPrediction, failures,
  };
}

// --- Worst-texture QUAL and paired superiority ------------------------------

export interface PairedComparison {
  seed: number;
  candidate: number;
  incumbent: number;
  strictlyBetter: boolean;
}

export interface StratumQual {
  board: string;
  exactMean: number;
  exactHw: number;
  observed: boolean;
  meanPasses: boolean;
  hwPasses: boolean;
  paired: PairedComparison[];
  pairedPasses: boolean;
}

export interface WorstTextureGate {
  rung: number;
  strata: StratumQual[];
  worstBoard: string | null;
  worstExact: number;
  worstPasses: boolean;
  allStrataObserved: boolean;
  passes: boolean;
  reasons: string[];
}

// The gate uses the WORST observed full-range exact mean across strata
// (not an average), per-seed half-width, and strict paired superiority
// over the nine-tier incumbent at every identical seed.
export function evaluateWorstTextureGate(
  rows: G2Row[],
  candidate: SchemeKey,
  strataBoards: readonly string[],
  rung: number = GATE_RUNG,
): WorstTextureGate {
  const strata: StratumQual[] = [];
  const reasons: string[] = [];
  for (const board of strataBoards) {
    const cand = aggregateCell(rows, board, candidate, 'full', rung);
    const inc = aggregateCell(rows, board, 'tiers', 'full', rung);
    const observed = cand.observed && inc.observed;
    const paired: PairedComparison[] = [];
    let pairedPasses = false;
    if (observed) {
      for (const seed of cand.seeds) {
        const c = cand.perSeedExact.get(seed)!;
        const t = inc.perSeedExact.get(seed);
        paired.push({
          seed, candidate: c, incumbent: Number.isFinite(t) ? t! : NaN,
          strictlyBetter: Number.isFinite(t) && c < t!,
        });
      }
      pairedPasses = paired.length > 0
        && paired.every((p) => Number.isFinite(p.incumbent) && p.strictlyBetter);
    }
    const meanPasses = observed && cand.exactMean <= EPS_EXACT;
    const hwPasses = observed && cand.exactHw <= EPS_HW;
    strata.push({
      board, exactMean: cand.exactMean, exactHw: cand.exactHw, observed,
      meanPasses, hwPasses, paired, pairedPasses,
    });
  }
  const observedStrata = strata.filter((s) => s.observed);
  const allObserved = observedStrata.length === strata.length;
  let worstBoard: string | null = null;
  let worstExact = -Infinity;
  for (const s of observedStrata) {
    if (s.exactMean > worstExact) {
      worstExact = s.exactMean;
      worstBoard = s.board;
    }
  }
  if (!allObserved)
    reasons.push('missing-strata');
  if (!observedStrata.every((s) => s.hwPasses))
    reasons.push('exact-half-width');
  const worstPasses = allObserved && Number.isFinite(worstExact) && worstExact <= EPS_EXACT;
  if (!worstPasses)
    reasons.push(`worst-exact:${Number.isFinite(worstExact) ? worstExact.toFixed(6) : 'NA'}`);
  if (!observedStrata.every((s) => s.pairedPasses))
    reasons.push('paired-superiority');
  return {
    rung, strata, worstBoard,
    worstExact: Number.isFinite(worstExact) ? worstExact : NaN,
    worstPasses, allStrataObserved: allObserved,
    passes: reasons.length === 0, reasons,
  };
}

// --- Mechanical grid selection (section 4) ----------------------------------

export type InstrumentVerdict =
  | {kind: 'uniform-ehs-viable'; selected: SchemeKey; gate: WorstTextureGate}
  | {kind: 'needs-conditional-kt128'; failed: SchemeKey; gate: WorstTextureGate}
  | {kind: 'instrument-inadequate-uniform-ehs'; gates: WorstTextureGate[]};

const PRIMARY_AXIS: SchemeKey[] = ['ehs2-32-64-64', 'ehs2-32-64-128', 'ehs2-32-64-256'];

export function selectInstrument(
  rows: G2Row[],
  strataBoards: readonly string[] = GATE_BOARDS,
  rung: number = GATE_RUNG,
): InstrumentVerdict {
  const gates: WorstTextureGate[] = [];
  for (const key of PRIMARY_AXIS) {
    // The RFC 0044 CERT rules apply per stratum on the full-range bucket
    // series; every stratum must converge before the exact gate qualifies.
    const ruleFailures = new Set<string>();
    let rulesObserved = true;
    for (const board of strataBoards) {
      const series = aggregateCellSeries(rows, board, key, 'full');
      const rules = evaluateRungRules(series, GATE_RUNG_INDEX);
      if (!rules.eligible)
        rulesObserved = false;
      for (const f of rules.failures)
        ruleFailures.add(`${board}:${f}`);
    }
    const gate = evaluateWorstTextureGate(rows, key, strataBoards, rung);
    gates.push(gate);
    if (rulesObserved && ruleFailures.size === 0 && gate.passes)
      return {kind: 'uniform-ehs-viable', selected: key, gate};
  }
  // The conditional K_turn=128 arm is a separate decision only if the
  // 256/64 primary failed; report it as "needs" unless its own rows also
  // pass (then it is the viable selection).
  const condKey: SchemeKey = 'ehs2-32-128-256';
  const condRows = rows.filter((r) => r.scheme === condKey);
  if (condRows.length > 0) {
    const condGate = evaluateWorstTextureGate(rows, condKey, strataBoards, rung);
    if (condGate.passes)
      return {kind: 'uniform-ehs-viable', selected: condKey, gate: condGate};
    gates.push(condGate);
    return {kind: 'instrument-inadequate-uniform-ehs', gates};
  }
  return {kind: 'needs-conditional-kt128', failed: 'ehs2-32-64-256', gate: gates[gates.length - 1]!};
}

// Full four-rung series for one board/scheme/range, in ladder order.
export function aggregateCellSeries(
  rows: G2Row[],
  board: string,
  scheme: SchemeKey,
  rangeName: string,
): CellPoint[] {
  return LADDER.map((T) => aggregateCell(rows, board, scheme, rangeName, T));
}

// The mechanical verdict may not silently drop a frozen gate board. The
// default selectInstrument() path passes all six; an explicit --boards
// subset must opt into diagnostics via --allow-partial.
export function validateGateCoverage(boards: readonly string[]): void {
  const given = new Set(boards);
  const missing = GATE_BOARDS.filter((b) => !given.has(b));
  if (missing.length > 0)
    throw new Error(`gate board set is incomplete; missing frozen strata: ${missing.join(',')}`);
}

// --- Stage-1 river-wall ingestion -------------------------------------------

export interface RiverWallRecord {
  board: string;
  rangeName: string;
  iterations: number;
  jointDeals: number;
  exactGap: number;
  trainS: number;
  exactWalkS: number;
}

// The Stage-1 smoke pins the per-full-range river exact-walk wall so the
// freeze addendum can cost full-range walks. Gap fields are reported but
// never enter G2 selection from this mode.
export function summarizeRiverWall(rows: G2Row[]): {
  records: RiverWallRecord[];
  maxFullWalkS: number;
  fullUniverseDeals: number;
} {
  const records = rows.map((r) => ({
    board: r.board, rangeName: r.rangeName, iterations: r.iterations,
    jointDeals: r.jointDeals, exactGap: r.exactGap,
    trainS: r.trainS, exactWalkS: r.exactWalkS,
  }));
  const full = records.filter((r) => r.rangeName === 'full');
  return {
    records,
    maxFullWalkS: full.reduce((m, r) => Math.max(m, r.exactWalkS), 0),
    fullUniverseDeals: full.reduce((m, r) => Math.max(m, r.jointDeals), 0),
  };
}

// --- G3 census ingestion -----------------------------------------------------

// Census CSV (one row per River-terminal build or distinct root tuple):
//   record,button,pot,line_tag,live_count,sequences,cap,nodes,infosets,
//   bytes,rows,seconds,all_in_no_flop,flop_class
// Root-identity tuples additionally carry per-seat ledger columns; the
// evaluator validates the structural contract and renders the envelope
// inputs, it never recomputes the tree.
export interface CensusRow {
  record: string;
  fields: Readonly<Map<string, string>>;
}

export function loadCensusCsv(path: string): CensusRow[] {
  const lines = readFileSync(path, 'utf8')
    .split('\n')
    .map((l) => l.trim())
    .filter((l) => l.length > 0 && !l.startsWith('#'));
  if (lines.length === 0)
    throw new Error(`${path}: empty census CSV`);
  const header = lines[0]!.split(',').map((h) => h.trim());
  return lines.slice(1).map((line) => {
    const f = line.split(',');
    const fields = new Map<string, string>();
    header.forEach((name, i) => fields.set(name, (f[i] ?? '').trim()));
    if (!fields.has('record'))
      throw new Error(`${path}: census row missing record column`);
    return {record: fields.get('record')!, fields};
  });
}

function censusNum(r: CensusRow, key: string): number {
  const v = r.fields.get(key);
  if (v === undefined || v === '')
    throw new Error(`census ${r.record}: missing numeric field ${key}`);
  const n = Number(v);
  if (!Number.isFinite(n))
    throw new Error(`census ${r.record}: non-numeric ${key}=${v}`);
  return n;
}

export interface CensusSummary {
  distinctRoots: number;
  allInNoFlopLines: number;
  rootsByTag: Record<string, number>;
  buttons: Set<number>;
  capBuilds: Array<{cap: number; nodes: number; infosets: number; rows: number;
                    treeBytes: number; sealedBytes: number; seconds: number;
                    flopClass: string; root: string}>;
  // Per cap, builds the tree materializer or trainer refused (resource
  // exhaustion). A cap with any exhausted build cannot be envelope-projected
  // from its successful maxima alone.
  exhaustedByCap: Record<number, number>;
  trainErrorByCap: Record<number, number>;
}

// Structural validation: the census must enumerate buttons explicitly,
// split all-in-no-flop lines out, and carry cap-0/cap-1 River-terminal
// builds for the stratified tuples.
export function summarizeCensus(rows: CensusRow[]): CensusSummary {
  const rootIds = new Set<string>();
  const tags: Record<string, number> = {};
  const buttons = new Set<number>();
  let allIn = 0;
  const capBuilds: CensusSummary['capBuilds'] = [];
  const exhaustedByCap: Record<number, number> = {};
  const trainErrorByCap: Record<number, number> = {};
  for (const r of rows) {
    if (r.record === 'root') {
      const id = r.fields.get('root_id') ?? r.fields.get('tuple_key');
      if (!id)
        throw new Error('census root row lacks root_id/tuple_key');
      rootIds.add(id);
      buttons.add(censusNum(r, 'button'));
      const tag = r.fields.get('line_tag') ?? 'unknown';
      tags[tag] = (tags[tag] ?? 0) + 1;
      if ((r.fields.get('all_in_no_flop') ?? '0') !== '0')
        ++allIn;
    } else if (r.record === 'allin') {
      ++allIn;
    } else if (r.record === 'river_terminal') {
      const cap = censusNum(r, 'cap');
      const status = r.fields.get('status') ?? '';
      if (status.startsWith('resource-exhausted')) {
        exhaustedByCap[cap] = (exhaustedByCap[cap] ?? 0) + 1;
        continue;
      }
      if (status.startsWith('train-error')) {
        trainErrorByCap[cap] = (trainErrorByCap[cap] ?? 0) + 1;
        continue;
      }
      capBuilds.push({
        cap,
        nodes: censusNum(r, 'nodes'),
        infosets: censusNum(r, 'infosets'),
        rows: censusNum(r, 'rows'),
        treeBytes: censusNum(r, 'tree_bytes'),
        sealedBytes: censusNum(r, 'sealed_bytes'),
        seconds: censusNum(r, 'seconds'),
        flopClass: r.fields.get('flop_class') ?? '',
        root: r.fields.get('root_id') ?? '',
      });
    }
  }
  return {
    distinctRoots: rootIds.size,
    allInNoFlopLines: allIn,
    rootsByTag: tags,
    buttons,
    capBuilds,
    exhaustedByCap,
    trainErrorByCap,
  };
}

// --- G4 envelope -------------------------------------------------------------

// Total cells = distinct non-all-in roots x 1,755 classes. Per-class
// cost uses the census MAX within a stratum (RFC section 6); cap 0 and
// cap 1 are reported separately because the fork is unresolved.
export interface Envelope {
  roots: number;
  classes: number;
  cells: number;
  byCap: Record<number, {
    feasible: boolean;           // false when any sampled build exhausted resources
    exhaustedBuilds: number;
    trainErrors: number;
    successfulBuilds: number;
    maxPerClassRows: number;
    maxPerClassBytes: number;
    maxPerClassSeconds: number;
    totalRows: number;
    totalBytes: number;
    totalCoreSeconds: number;
  }>;
}

export function buildEnvelope(summary: CensusSummary, blueprintRoots: number): Envelope {
  const byCap: Envelope['byCap'] = {};
  for (const cap of [0, 1]) {
    const builds = summary.capBuilds.filter((b) => b.cap === cap);
    const exhausted = summary.exhaustedByCap[cap] ?? 0;
    const trainErrors = summary.trainErrorByCap[cap] ?? 0;
    const maxRows = builds.reduce((m, b) => Math.max(m, b.rows), 0);
    const maxBytes = builds.reduce((m, b) => Math.max(m, b.sealedBytes), 0);
    const maxSeconds = builds.reduce((m, b) => Math.max(m, b.seconds), 0);
    const cells = blueprintRoots * FLOP_CLASSES;
    byCap[cap] = {
      feasible: exhausted === 0,
      exhaustedBuilds: exhausted,
      trainErrors,
      successfulBuilds: builds.length,
      maxPerClassRows: maxRows,
      maxPerClassBytes: maxBytes,
      maxPerClassSeconds: maxSeconds,
      totalRows: cells * maxRows,
      totalBytes: cells * maxBytes,
      totalCoreSeconds: cells * maxSeconds,
    };
  }
  return {roots: blueprintRoots, classes: FLOP_CLASSES, cells: blueprintRoots * FLOP_CLASSES, byCap};
}

// --- CLI ---------------------------------------------------------------------

function fmt(x: number, digits = 6): string {
  return Number.isFinite(x) ? x.toFixed(digits) : 'NA';
}

function usage(): never {
  console.error(
    'usage:\n'
    + '  rfc0045-gates wall <g2-csv...>\n'
    + '  rfc0045-gates g2 <g2-csv...> [--json] [--boards b1,b2,...]\n'
    + '  rfc0045-gates census <census-csv...>\n'
    + '  rfc0045-gates envelope <census-csv> <blueprint-roots>',
  );
  process.exit(2);
}

function wallCli(paths: string[]): void {
  const wall = summarizeRiverWall(loadG2Csvs(paths));
  console.log('board,range,iterations,joint_deals,sum_gap_bb,train_s,exact_walk_s');
  for (const r of wall.records)
    console.log(
      `${r.board},${r.rangeName},${r.iterations},${r.jointDeals},${fmt(r.exactGap)},`
      + `${fmt(r.trainS, 3)},${fmt(r.exactWalkS, 3)}`,
    );
  console.log(`# max full-range river exact walk: ${fmt(wall.maxFullWalkS, 3)} s over`
    + ` ${wall.fullUniverseDeals.toLocaleString('en-US')} deals`);
}

function g2Cli(paths: string[], asJson: boolean, boards: readonly string[],
               allowPartial: boolean): void {
  if (!allowPartial)
    validateGateCoverage(boards);
  const rows = loadG2Csvs(paths);
  const verdict = selectInstrument(rows, boards);
  if (asJson) {
    console.log(JSON.stringify(verdict, (k, v) => (v instanceof Map ? Object.fromEntries(v) : v), 2));
    return;
  }
  if (verdict.kind === 'uniform-ehs-viable') {
    console.log(`G2: uniform-ehs-viable selected=${verdict.selected} worst=`
      + `${verdict.gate.worstBoard} exact=${fmt(verdict.gate.worstExact)}`);
  } else if (verdict.kind === 'needs-conditional-kt128') {
    console.log(`G2: primary axis failed (${verdict.failed}); conditional K_turn=128 arm not run`);
    console.log(`  reasons=[${verdict.gate.reasons.join(';')}]`);
  } else {
    console.log('G2: instrument-inadequate-uniform-ehs');
    for (const g of verdict.gates)
      console.log(`  worst=${g.worstBoard} exact=${fmt(g.worstExact)} reasons=[${g.reasons.join(';')}]`);
  }
}

function censusCli(paths: string[]): void {
  const summary = summarizeCensus(paths.flatMap(loadCensusCsv));
  console.log(`distinct identity-complete roots: ${summary.distinctRoots}`);
  console.log(`all-in-no-flop lines: ${summary.allInNoFlopLines}`);
  console.log(`buttons enumerated: ${[...summary.buttons].sort((a, b) => a - b).join(',')}`);
  for (const [tag, n] of Object.entries(summary.rootsByTag))
    console.log(`  ${tag}: ${n}`);
  for (const cap of [0, 1]) {
    const builds = summary.capBuilds.filter((b) => b.cap === cap);
    const exhausted = (summary.exhaustedByCap[cap] ?? 0);
    const trainErrors = (summary.trainErrorByCap[cap] ?? 0);
    if (builds.length === 0 && exhausted === 0 && trainErrors === 0)
      continue;
    const maxNodes = Math.max(0, ...builds.map((b) => b.nodes));
    const maxInfosets = Math.max(0, ...builds.map((b) => b.infosets));
    const maxBytes = Math.max(0, ...builds.map((b) => b.sealedBytes));
    console.log(
      `cap ${cap}: successful=${builds.length} resource-exhausted=${exhausted} `
      + `train-errors=${trainErrors} maxNodes=${maxNodes} maxInfosets=${maxInfosets} `
      + `maxBytes=${maxBytes}${exhausted > 0 ? ' (cap NOT feasible for the sampled bounds)' : ''}`,
    );
  };
}

function envelopeCli(censusPath: string, rootsArg: string): void {
  const roots = Number(rootsArg);
  if (!Number.isInteger(roots) || roots <= 0)
    usage();
  const summary = summarizeCensus(loadCensusCsv(censusPath));
  const env = buildEnvelope(summary, roots);
  console.log(`cells=${env.cells.toLocaleString('en-US')} (${env.roots} roots x ${env.classes} classes)`);
  for (const cap of [0, 1]) {
    const e = env.byCap[cap]!;
    if (!e.feasible) {
      console.log(`cap ${cap}: INFEASIBLE for ${e.exhaustedBuilds} sampled build(s) `
        + `— envelope not projected (train-errors=${e.trainErrors}, `
        + `successful=${e.successfulBuilds})`);
      continue;
    }
    console.log(`cap ${cap}: totalRows=${e.totalRows.toLocaleString('en-US')} `
      + `totalBytes=${e.totalBytes.toLocaleString('en-US')} `
      + `coreHours=${(e.totalCoreSeconds / 3600).toFixed(1)} `
      + `(max per-class rows=${e.maxPerClassRows} bytes=${e.maxPerClassBytes} s=${e.maxPerClassSeconds})`);
  }
}

// The CLI dispatch runs only when invoked as the evaluator entry point
// (the compiled module directly, or the bin/rfc0045-gates.mjs launcher);
// importing the evaluator from another tool (the campaign aggregator) must
// not parse that tool's argv.
const invokedDirectly = (() => {
  const entry = process.argv[1] ?? '';
  return /rfc0045-gates(\.[a-f0-9]+)?\.(m?js)$/.test(entry);
})();
if (invokedDirectly) {
const mode = process.argv[2];
if (mode === 'wall') {
  const paths = process.argv.slice(3);
  if (paths.length === 0)
    usage();
  wallCli(paths);
} else if (mode === 'g2') {
  const args = process.argv.slice(3);
  const asJson = args.includes('--json');
  // The mechanical verdict defaults to ALL SIX frozen gate boards
  // (including the census-selected raised-srp board); an explicit
  // --boards subset is diagnostics and needs --allow-partial.
  let boards: readonly string[] = GATE_BOARDS;
  let allowPartial = false;
  const bi = args.indexOf('--boards');
  if (bi !== -1) {
    const v = args[bi + 1];
    if (!v)
      usage();
    boards = v.split(',').map((s) => s.trim()).filter(Boolean);
  }
  if (args.includes('--allow-partial'))
    allowPartial = true;
  const paths = args.filter((a, i) =>
    a !== '--json' && a !== '--allow-partial'
    && !(bi !== -1 && (i === bi || i === bi + 1)));
  if (paths.length === 0)
    usage();
  try {
    g2Cli(paths, asJson, boards, allowPartial);
  } catch (e) {
    console.error((e as Error).message);
    process.exit(2);
  }
} else if (mode === 'census') {
  const paths = process.argv.slice(3);
  if (paths.length === 0)
    usage();
  censusCli(paths);
} else if (mode === 'envelope') {
  const p = process.argv[3];
  const roots = process.argv[4];
  if (!p || !roots)
    usage();
  envelopeCli(p, roots);
} else if (mode !== undefined) {
  console.error(`unknown mode ${mode}`);
  usage();
}
}  // end invokedDirectly
