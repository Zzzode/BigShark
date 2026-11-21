// RFC 0042 mechanical statistics: paired-difference intervals and the
// frozen gate evaluation for the EquityStrengthV1 card-kind ablation.
//
// Inputs are the benchmark's frozen CSV rows:
//   scheme,seats,street,seed,wall_ms,completed,infosets,sealed_bytes,
//   joint_deals,max_gap_bb,sum_gap_bb,bucket_sum_gap_bb,note
//
// Two RFC 0042 modes:
//   pilot  <csv...>    expected S=32 half-widths per scheme/shape
//                      (paired SD from the 5 pilot seeds)
//   gate   <control-csv> <candidate-csv> [--scheme name] [--bucket-bb B]
//                      the frozen Stage 3 predicates (superiority on
//                      2-seat river, non-inferiority elsewhere,
//                      structural gates, half-width classification)
//
// The interval is the bias-corrected percentile bootstrap of the mean
// paired difference (B=10,000, deterministic SplitMix64 seed), with the
// paired-t interval reported alongside. Everything here is computed
// from the raw rows; there is no post-hoc parameter.
import {readFileSync} from 'node:fs';

export const BOOTSTRAP_REPLICATES = 10_000;
export const BOOTSTRAP_SEED = 0x0042_0042_0042_0042n;
export const PRIMARY_HALFWIDTH_TARGET = 0.05;  // BB, M/2 at M=0.101
export const NONINFERIORITY_HALFWIDTH_TARGET = 0.01;  // BB, delta/2
export const NONINFERIORITY_MARGIN = 0.02;  // BB
export const CONVERGENCE_BUCKET_SUM = 0.05;  // BB, per river shape
export const BYTES_RATIO_LIMIT = 2.0;
export const ITERATIONS_RATIO_LIMIT = 0.5;
export const SUPERIORITY_FLOOR = 0.05;  // BB absolute floor for M

export interface AblationRow {
  scheme: string;
  seats: number;
  street: 'river' | 'turn';
  seed: bigint;
  completed: bigint;
  infosets: number;
  sealed_bytes: number;
  joint_deals: number;
  max_gap_bb: number;
  sum_gap_bb: number;
  bucket_sum_gap_bb: number;  // -1 when not applicable
  note: string;
}

const HEADER =
  'scheme,seats,street,seed,wall_ms,completed,infosets,sealed_bytes,joint_deals,' +
  'max_gap_bb,sum_gap_bb,bucket_sum_gap_bb,note';

export function loadAblationCsv(path: string): AblationRow[] {
  const lines = readFileSync(path, 'utf8')
    .split('\n')
    .map((line) => line.trim())
    .filter((line) => line.length > 0 && !line.startsWith('#'));
  if (lines[0] !== HEADER)
    throw new Error(`${path}: unexpected header; the RFC 0042 CSV header is frozen`);
  const rows: AblationRow[] = [];
  for (const line of lines.slice(1)) {
    const f = line.split(',');
    if (f.length !== 13)
      throw new Error(`${path}: malformed row (${f.length} fields): ${line}`);
    if (f[5] === 'DECLINED')
      continue;  // declined rows carry their reason in the note column position
    const seats = Number(f[1]);
    if (seats !== 2 && seats !== 3)
      throw new Error(`${path}: gate analysis supports seats 2..3 only, got ${seats}`);
    if (f[2] !== 'river' && f[2] !== 'turn')
      throw new Error(`${path}: bad street ${f[2]}`);
    rows.push({
      scheme: f[0]!,
      seats,
      street: f[2] as 'river' | 'turn',
      seed: BigInt(f[3]!),
      completed: BigInt(f[5]!),
      infosets: Number(f[6]),
      sealed_bytes: Number(f[7]),
      joint_deals: Number(f[8]),
      max_gap_bb: Number(f[9]),
      sum_gap_bb: Number(f[10]),
      bucket_sum_gap_bb: Number(f[11]),
      note: f[12]!,
    });
  }
  return rows;
}

// Deterministic SplitMix64 over [0,1): the same stream every run, so
// the bootstrap intervals are reproducible from the committed CSV.
function makeRng(seed: bigint): () => number {
  let state = seed;
  const MASK = (1n << 64n) - 1n;
  return () => {
    state = (state + 0x9e37_79b9_7f4a_7c15n) & MASK;
    let z = state;
    z = ((z ^ (z >> 30n)) * 0xbf58_476d_1ce4_e5b9n) & MASK;
    z = ((z ^ (z >> 27n)) * 0x94d0_49bb_1331_11ebn) & MASK;
    z = z ^ (z >> 31n);
    return Number(z >> 11n) / Number(1n << 53n);
  };
}

export interface PairedStats {
  n: number;
  mean: number;
  sd: number;  // sample SD of the paired differences
}

function pairedDifferences(
  control: AblationRow[],
  candidate: AblationRow[],
  shape: {seats: number; street: 'river' | 'turn'},
  metric: 'sum_gap_bb' | 'bucket_sum_gap_bb' | 'sealed_bytes' | 'completed',
): number[] {
  const key = (r: AblationRow) => `${r.seats}/${r.street}/${r.seed}`;
  const bySeed = new Map<string, number>();
  for (const r of control)
    if (r.seats === shape.seats && r.street === shape.street)
      bySeed.set(key(r), Number(r[metric]));
  const diffs: number[] = [];
  for (const r of candidate) {
    if (r.seats !== shape.seats || r.street !== shape.street)
      continue;
    const c = bySeed.get(key(r));
    if (c === undefined)
      throw new Error(`paired analysis: candidate seed ${r.seed} missing in control`);
    diffs.push(c - Number(r[metric]));
  }
  if (diffs.length === 0)
    throw new Error(`paired analysis: no rows for seats=${shape.seats} street=${shape.street}`);
  if (diffs.length !== bySeed.size)
    throw new Error('paired analysis: control/candidate seed sets differ');
  return diffs;
}

export function pairedStats(d: number[]): PairedStats {
  const n = d.length;
  const mean = d.reduce((a, b) => a + b, 0) / n;
  const variance = n > 1 ? d.reduce((a, x) => a + (x - mean) ** 2, 0) / (n - 1) : 0;
  return {n, mean, sd: Math.sqrt(variance)};
}

// Bias-corrected percentile bootstrap of the mean. Returns the 95%
// CI plus the observed mean; the paired-t CI is reported for
// transparency (gates use the BCa interval).
export function bcaBootstrapMean(
  d: number[],
  replicates = BOOTSTRAP_REPLICATES,
  seed: bigint = BOOTSTRAP_SEED,
): {mean: number; lower: number; upper: number; tLower: number; tUpper: number} {
  const stats = pairedStats(d);
  const n = d.length;
  const rng = makeRng(seed);
  const means: number[] = [];
  for (let b = 0; b < replicates; ++b) {
    let sum = 0;
    for (let i = 0; i < n; ++i)
      sum += d[Math.floor(rng() * n)]!;
    means.push(sum / n);
  }
  means.sort((a, b) => a - b);
  const quantile = (p: number) => {
    const idx = Math.min(means.length - 1, Math.max(0, Math.floor(p * means.length)));
    return means[idx]!;
  };
  // z0: bias correction from the proportion below the observed mean.
  const below = means.filter((m) => m < stats.mean).length / means.length;
  const z0 = normInv(Math.min(0.999, Math.max(0.001, below)));
  // Acceleration via the jackknife.
  const jack: number[] = [];
  for (let i = 0; i < n; ++i)
    jack.push((d.reduce((a, x, j) => a + (j === i ? 0 : x), 0)) / (n - 1));
  const jackMean = jack.reduce((a, b) => a + b, 0) / n;
  const num = jack.reduce((a, x) => a + (jackMean - x) ** 3, 0);
  const den = 6 * (jack.reduce((a, x) => a + (jackMean - x) ** 2, 0) ** 1.5);
  const a = den === 0 ? 0 : num / den;
  const zLo = normInv(0.025);
  const zHi = normInv(0.975);
  const adjust = (z: number) => {
    const p = normalCdf(z0 + (z0 + z) / (1 - a * (z0 + z)));
    return quantile(p);
  };
  // Two-sided 95% t critical for small paired samples (df=n-1); the
  // bootstrap CI is authoritative, this is the companion interval.
  const tCrit = tCritical975(Math.max(1, n - 1));
  const half = (tCrit * stats.sd) / Math.sqrt(n);
  return {
    mean: stats.mean,
    lower: adjust(zLo),
    upper: adjust(zHi),
    tLower: stats.mean - half,
    tUpper: stats.mean + half,
  };
}

function normalCdf(z: number): number {
  return 0.5 * (1 + erf(z / Math.SQRT2));
}

function erf(x: number): number {
  // Abramowitz-Stegun 7.1.26, sufficient for the CI quantiles.
  const s = x < 0 ? -1 : 1;
  x = Math.abs(x);
  const t = 1 / (1 + 0.3275911 * x);
  const y =
    1 -
    ((((1.061405429 * t - 1.453152027) * t + 1.421413741) * t - 0.284496736) * t +
      0.254829592) *
      t *
      Math.exp(-x * x);
  return s * y;
}

function normInv(p: number): number {
  // Beasley-Springer-Moro approximation.
  if (p <= 0 || p >= 1)
    throw new Error('normInv: p out of range');
  const a = [
    -3.969683028665376e1, 2.209460984245205e2, -2.759285104469687e2, 1.38357751867269e2,
    -3.066479806614716e1, 2.506628277459239,
  ];
  const b = [
    -5.447609879822406e1, 1.615858368580409e2, -1.556989798598866e2, 6.680131188771972e1,
    -1.328068155288572e1,
  ];
  const c = [
    -7.784894002430293e-3, -3.223964580411365e-1, -2.400758277161838, -2.549732539343734,
    4.374664141464968, 2.938163982698783,
  ];
  const d = [7.784695709041462e-3, 3.224671290700398e-1, 2.445134137142996, 3.754408661907416];
  const plow = 0.02425;
  const phigh = 1 - plow;
  let q: number;
  let r: number;
  if (p < plow) {
    q = Math.sqrt(-2 * Math.log(p));
    return (((((c[0]! * q + c[1]!) * q + c[2]!) * q + c[3]!) * q + c[4]!) * q + c[5]!) /
      ((((d[0]! * q + d[1]!) * q + d[2]!) * q + d[3]!) * q + 1);
  }
  if (p <= phigh) {
    q = p - 0.5;
    r = q * q;
    return ((((((a[0]! * r + a[1]!) * r + a[2]!) * r + a[3]!) * r + a[4]!) * r + a[5]!) * q) /
      (((((b[0]! * r + b[1]!) * r + b[2]!) * r + b[3]!) * r + b[4]!) * r + 1);
  }
  q = Math.sqrt(-2 * Math.log(1 - p));
  return -(((((c[0]! * q + c[1]!) * q + c[2]!) * q + c[3]!) * q + c[4]!) * q + c[5]!) /
    ((((d[0]! * q + d[1]!) * q + d[2]!) * q + d[3]!) * q + 1);
}

// Two-sided 95% Student-t critical values for the pilot/Stage 3 sample
// sizes used (df 4 pilot, 31 and above confirm). Beyond 30 the normal
// 1.96 is within 0.03; exact rows are listed for the frozen sizes.
function tCritical975(df: number): number {
  const table = new Map<number, number>([
    [4, 2.776], [5, 2.571], [6, 2.447], [7, 2.365], [8, 2.306], [9, 2.262], [10, 2.228],
    [15, 2.131], [20, 2.086], [25, 2.060], [30, 2.042], [31, 2.040], [40, 2.021], [60, 2.0],
  ]);
  const exact = table.get(df);
  if (exact !== undefined)
    return exact;
  if (df > 60)
    return 1.96;
  // Interpolate between the bracketing listed rows.
  const keys = [...table.keys()].sort((a, b) => a - b);
  for (let i = 0; i + 1 < keys.length; ++i) {
    const lo = keys[i]!;
    const hi = keys[i + 1]!;
    if (df > lo && df < hi) {
      const w = (df - lo) / (hi - lo);
      return table.get(lo)! * (1 - w) + table.get(hi)! * w;
    }
  }
  return 1.96;
}

export interface PilotCell {
  scheme: string;
  seats: number;
  street: 'river' | 'turn';
  n: number;
  mean_sum_gap_bb: number;
  pairedSd: number;
  projectedHalfWidth32: number;
  target: number;
  passesAtS32: boolean;
}

// Pilot power check: project the 95% half-width at S=32 from the
// pilot paired SD (the SD is computed against the `tiers` control
// when a tiers CSV is present, else against the scheme's own seed
// spread on sum_gap_bb). All four shapes are reported for every
// candidate scheme; the worst non-inferiority shape sets S.
export function pilotAnalysis(
  tiersPath: string | null,
  candidatePaths: {scheme: string; path: string}[],
): PilotCell[] {
  const tiersRows = tiersPath ? loadAblationCsv(tiersPath) : null;
  const out: PilotCell[] = [];
  for (const {scheme, path} of candidatePaths) {
    const rows = loadAblationCsv(path);
    for (const seats of [2, 3]) {
      for (const street of ['river', 'turn'] as const) {
        const shape = {seats, street};
        let sd: number;
        let mean: number;
        const sameScheme = rows.filter((r) => r.seats === seats && r.street === street);
        if (tiersRows && scheme !== 'tiers') {
          const d = pairedDifferences(tiersRows, rows, shape, 'sum_gap_bb');
          const st = pairedStats(d);
          sd = st.sd;
          mean = st.mean;
        } else {
          const vals = sameScheme.map((r) => r.sum_gap_bb);
          sd = pairedStats(vals.map((_, i) => vals[i]! - pairedStats(vals).mean)).sd;
          mean = pairedStats(vals).mean;
        }
        const target =
          seats === 2 && street === 'river'
            ? PRIMARY_HALFWIDTH_TARGET
            : NONINFERIORITY_HALFWIDTH_TARGET;
        const hw = (tCritical975(31) * sd) / Math.sqrt(32);
        out.push({
          scheme,
          seats,
          street,
          n: sameScheme.length,
          mean_sum_gap_bb: mean,
          pairedSd: sd,
          projectedHalfWidth32: hw,
          target,
          passesAtS32: hw < target,
        });
      }
    }
  }
  return out;
}

// ---------------------------------------------------------------------------
// Frozen Stage 3 gate evaluation (all predicates from RFC 0042 Section 3)
// ---------------------------------------------------------------------------

export interface ShapeInterval {
  seats: number;
  street: 'river' | 'turn';
  n: number;
  mean: number;
  lower: number;
  upper: number;
  tLower: number;
  tUpper: number;
}

export function pairedIntervalForMetric(
  control: AblationRow[],
  candidate: AblationRow[],
  shape: {seats: number; street: 'river' | 'turn'},
  metric: 'sum_gap_bb' | 'bucket_sum_gap_bb' | 'sealed_bytes' | 'completed',
): ShapeInterval {
  const d = pairedDifferences(control, candidate, shape, metric);
  const ci = bcaBootstrapMean(d);
  return {
    seats: shape.seats,
    street: shape.street,
    n: d.length,
    mean: ci.mean,
    lower: ci.lower,
    upper: ci.upper,
    tLower: ci.tLower,
    tUpper: ci.tUpper,
  };
}

function meanOf(rows: AblationRow[], shape: {seats: number; street: string}, field: keyof AblationRow): number {
  const xs = rows.filter((r) => r.seats === shape.seats && r.street === shape.street)
    .map((r) => Number(r[field]));
  if (xs.length === 0)
    throw new Error(`meanOf: no rows ${shape.seats}/${shape.street}`);
  return xs.reduce((a, b) => a + b, 0) / xs.length;
}

export interface GateReport {
  candidate: string;
  m: number;
  gate1Primary?: {pass: boolean; mean: number; lower: number; m: number};
  gate2NonInferiority: {shape: string; pass: boolean; upper: number; margin: number; halfWidth: number; target: number}[];
  gate3Convergence: {shape: string; pass: boolean; bucketSum: number; limit: number}[];
  gate4Cost: {shape: string; pass: boolean; bytesRatio: number; iterationsRatio: number}[];
  gates1to4: boolean;
  hingeFailuresWide: {shape: string; observedHalfWidth: number; target: number}[];
}

export function evaluateCandidate(
  tiers: AblationRow[],
  candidate: AblationRow[],
  candidateName: string,
): GateReport {
  const shapes = [
    {seats: 2, street: 'river' as const},
    {seats: 2, street: 'turn' as const},
    {seats: 3, street: 'river' as const},
    {seats: 3, street: 'turn' as const},
  ];
  const report: GateReport = {
    candidate: candidateName,
    m: 0,
    gate2NonInferiority: [],
    gate3Convergence: [],
    gate4Cost: [],
    gates1to4: true,
    hingeFailuresWide: [],
  };

  // Gate 1: superiority on 2-seat river.
  const primary = shapes[0]!;
  const tiersExact = meanOf(tiers, primary, 'sum_gap_bb');
  const tiersBucket = meanOf(tiers, primary, 'bucket_sum_gap_bb');
  report.m = Math.max(SUPERIORITY_FLOOR, (tiersExact - tiersBucket) / 3);
  const g1 = pairedIntervalForMetric(tiers, candidate, primary, 'sum_gap_bb');
  report.gate1Primary = {
    pass: g1.lower > 0 && g1.mean >= report.m,
    mean: g1.mean,
    lower: g1.lower,
    m: report.m,
  };

  // Gate 2: non-inferiority on the other three shapes; the interval is
  // computed on (new - tiers), i.e. the negative of d.
  for (const shape of shapes.slice(1)) {
    const d = pairedDifferences(tiers, candidate, shape, 'sum_gap_bb');  // tiers - new
    const ci = bcaBootstrapMean(d);
    const upperNewMinusTiers = -ci.lower;
    const halfWidth = (ci.upper - ci.lower) / 2;
    const pass = upperNewMinusTiers <= NONINFERIORITY_MARGIN;
    report.gate2NonInferiority.push({
      shape: `${shape.seats}p-${shape.street}`,
      pass,
      upper: upperNewMinusTiers,
      margin: NONINFERIORITY_MARGIN,
      halfWidth,
      target: NONINFERIORITY_HALFWIDTH_TARGET,
    });
    if (!pass)
      report.hingeFailuresWide.push({
        shape: `${shape.seats}p-${shape.street}`,
        observedHalfWidth: halfWidth,
        target: NONINFERIORITY_HALFWIDTH_TARGET,
      });
  }

  // Gate 3: per-river-shape bucket-constrained seed mean.
  for (const shape of shapes.filter((s) => s.street === 'river')) {
    const bucketSum = meanOf(candidate, shape, 'bucket_sum_gap_bb');
    const pass = bucketSum <= CONVERGENCE_BUCKET_SUM;
    report.gate3Convergence.push({
      shape: `${shape.seats}p-river`,
      pass,
      bucketSum,
      limit: CONVERGENCE_BUCKET_SUM,
    });
  }

  // Gate 4: cost ratios per shape.
  for (const shape of shapes) {
    const cBytes = meanOf(candidate, shape, 'sealed_bytes');
    const tBytes = meanOf(tiers, shape, 'sealed_bytes');
    const cIter = meanOf(candidate, shape, 'completed');
    const tIter = meanOf(tiers, shape, 'completed');
    const bytesRatio = cBytes / tBytes;
    const iterationsRatio = cIter / tIter;
    report.gate4Cost.push({
      shape: `${shape.seats}p-${shape.street}`,
      pass: bytesRatio <= BYTES_RATIO_LIMIT && iterationsRatio >= ITERATIONS_RATIO_LIMIT,
      bytesRatio,
      iterationsRatio,
    });
  }

  report.gates1to4 =
    (report.gate1Primary?.pass ?? false) &&
    report.gate2NonInferiority.every((g) => g.pass) &&
    report.gate3Convergence.every((g) => g.pass) &&
    report.gate4Cost.every((g) => g.pass);
  return report;
}

// Gate 1 hinge-width check for the Keep/Inconclusive predicate.
export function primaryHinge(report: {gate1Primary?: {pass: boolean; lower: number; mean: number}},
                             tiers: AblationRow[], candidate: AblationRow[]): void {
  const shape = {seats: 2, street: 'river' as const};
  const ci = pairedIntervalForMetric(tiers, candidate, shape, 'sum_gap_bb');
  const halfWidth = (ci.upper - ci.lower) / 2;
  if (!(report.gate1Primary?.pass ?? false)) {
    (report as unknown as {hinge?: unknown}).hinge = {
      shape: '2p-river',
      observedHalfWidth: halfWidth,
      target: PRIMARY_HALFWIDTH_TARGET,
      lowerPositive: ci.lower > 0,
      meanAboveM: ci.mean,
    };
  }
}

function fmt(x: number, digits = 4): string {
  return x.toFixed(digits);
}

function printReport(report: GateReport): void {
  const g1 = report.gate1Primary!;
  console.log(`candidate=${report.candidate}`);
  console.log(`gate1 superiority 2p-river: mean(d)=${fmt(g1.mean)} LB=${fmt(g1.lower)} M=${fmt(report.m, 3)} -> ${g1.pass ? 'PASS' : 'FAIL'}`);
  for (const g of report.gate2NonInferiority)
    console.log(`gate2 NI ${g.shape}: UB(new-tiers)=${fmt(g.upper)} margin=${g.margin} halfwidth=${fmt(g.halfWidth, 4)} target=${g.target} -> ${g.pass ? 'PASS' : 'FAIL'}`);
  for (const g of report.gate3Convergence)
    console.log(`gate3 convergence ${g.shape}: bucket_sum=${fmt(g.bucketSum)} limit=${g.limit} -> ${g.pass ? 'PASS' : 'FAIL'}`);
  for (const g of report.gate4Cost)
    console.log(`gate4 cost ${g.shape}: bytes x${fmt(g.bytesRatio, 3)} (<=2.0), iters x${fmt(g.iterationsRatio, 3)} (>=0.5) -> ${g.pass ? 'PASS' : 'FAIL'}`);
  console.log(`gates 1-4 -> ${report.gates1to4 ? 'ALL PASS' : 'FAIL'}`);
}

// Pilot projection: the expected S=32 half-width for the deciding
// predicates, computed from the 5 pilot paired seeds.
function pilotCli(paths: string[]): void {
  const tiersPath = paths.find((p) => p.includes('tiers.csv') && !p.includes('tiersdraw') && !p.includes('tiers-draw')) ?? null;
  const candPaths = paths
    .filter((p) => p !== tiersPath)
    .map((p) => ({scheme: p.includes('draw') ? 'tiers-draw' : 'equity', path: p}));
  const cells = pilotAnalysis(tiersPath, candPaths);
  console.log('scheme,shape,n,mean_d_or_sum,paired_sd,projected_hw_S32,target,passes');
  for (const c of cells)
    console.log([c.scheme, `${c.seats}p-${c.street}`, c.n, fmt(c.mean_sum_gap_bb), fmt(c.pairedSd),
      fmt(c.projectedHalfWidth32), fmt(c.target, 2), c.passesAtS32 ? 'yes' : 'NO'].join(','));
}

function gateCli(tiersPath: string, candidatePath: string, name: string): void {
  const tiers = loadAblationCsv(tiersPath);
  const candidate = loadAblationCsv(candidatePath);
  const report = evaluateCandidate(tiers, candidate, name);
  printReport(report);
  if (name !== 'tiers-draw')
    primaryHinge(report, tiers, candidate);
}

const mode = process.argv[2];
if (mode === 'pilot') {
  pilotCli(process.argv.slice(3));
} else if (mode === 'gate') {
  const [tiersPath, candidatePath, name] = process.argv.slice(3);
  if (!tiersPath || !candidatePath || !name) {
    console.error('usage: rfc0042-gates gate <tiers.csv> <candidate.csv> <name>');
    process.exit(2);
  }
  gateCli(tiersPath, candidatePath, name);
} else if (mode !== undefined) {
  console.error(`unknown mode ${mode}; use 'pilot' or 'gate'`);
  process.exit(2);
}
