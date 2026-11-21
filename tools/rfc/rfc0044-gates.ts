// RFC 0044 reachable convergence certification: frozen CERT/QUAL
// predicates over the extended offline iteration ladder, plus the
// rule-5 additive-floor failure classifier.
//
// Inputs are the campaign ladder CSV rows. The recognized header is
// (columns are read BY NAME; the future freeze addendum may append
// columns without breaking this parser):
//   mode,scheme,seats,street,seed,target_iters,safety_wall_s,completed,
//   infosets,sealed_bytes,joint_deals,max_gap_bb,sum_gap_bb,
//   bucket_sum_gap_bb,wall_observed_s,peak_rss_kb,truncated,note
//
// Two gates (RFC 0044 sections 2-3), both evaluated in full double
// precision and rounded only for display:
//   CERT  five-seed mean within-abstraction bucket gap series g(T):
//         absolute 0.005 BB bar, independent-seed half-width bar,
//         three-rung log-linear tail consistency, and a strictly
//         separate prior-window forward-prediction check.
//   QUAL  at r_cert: five-seed mean exact-hole gap <= 0.03 BB (HU)
//         and strict paired superiority over the incumbent, seed by
//         identical seed.
//
// CLI mode:
//   certification <csv...> [--json]
//         groups ladder/river rows by (seats, scheme) and prints the
//         per-rung series, the r_cert verdict (or rule-5 class), the
//         three-rung tail fit with its 100M projection, and QUAL when
//         a paired incumbent series exists.
import {readFileSync} from 'node:fs';

// --- Frozen predicate constants (RFC 0044 section 2) -----------------------

export const LADDER: readonly number[] = [
  100_000, 300_000, 1_000_000, 3_000_000, 10_000_000,
  30_000_000, 100_000_000, 300_000_000,
];
export const LADDER_CAP = 300_000_000;
export const SEED_COUNT = 5;
export const EPS_GAP = 0.005;        // BB, absolute within-abstraction bar
export const EPS_EXACT_HU = 0.03;    // BB, HU exact-game quality bar
export const HALFWIDTH_BAR = 0.001;  // BB
export const T_CRIT_0_975_4 = 2.7764;  // two-sided Student t, df = n-1 = 4
export const TAIL_WINDOW = 3;
export const EXPONENT_MIN = 0.0;
export const EXPONENT_MAX = 1.5;
export const FIT_REL_TOLERANCE = 0.20;
// Rule-5 additive-floor window. Frozen by the RFC 0044 Stage-2 pre-run
// freeze addendum (2026-10-31, item 1): fit on the LAST FOUR observed rungs
// only, so the 100k/300k transient cannot be misread as a positive floor.
// 'all' (Infinity) remains available as a diagnostics override.
export const RULE5_WINDOW_RUNGS = 4;

// Columns this evaluator reads out of the campaign CSV. Anything else
// present in the header is ignored on purpose so the freeze addendum
// can append columns.
export const RECOGNIZED_COLUMNS = [
  'mode', 'scheme', 'seats', 'street', 'seed', 'target_iters',
  'safety_wall_s', 'completed', 'infosets', 'sealed_bytes', 'joint_deals',
  'max_gap_bb', 'sum_gap_bb', 'bucket_sum_gap_bb', 'wall_observed_s',
  'peak_rss_kb', 'truncated', 'note',
] as const;

export interface LadderRow {
  mode: string;
  scheme: string;
  seats: number;
  street: string;
  seed: number;
  target_iters: number;
  safety_wall_s: number;
  completed: number;
  infosets: number;
  sealed_bytes: number;
  joint_deals: number;
  max_gap_bb: number;
  sum_gap_bb: number;          // exact-hole summed BR gap (QUAL input)
  bucket_sum_gap_bb: number;   // within-abstraction bucket gap (CERT input)
  wall_observed_s: number;
  peak_rss_kb: number;
  truncated: boolean;
  note: string;
}

export function loadLadderCsv(path: string): LadderRow[] {
  const lines = readFileSync(path, 'utf8')
    .split('\n')
    .map((line) => line.trim())
    .filter((line) => line.length > 0 && !line.startsWith('#'));
  if (lines.length === 0)
    throw new Error(`${path}: empty CSV`);
  const header = lines[0]!.split(',').map((h) => h.trim());
  const indexOf = new Map<string, number>();
  header.forEach((name, i) => {
    if (indexOf.has(name))
      throw new Error(`${path}: duplicate header column ${name}`);
    indexOf.set(name, i);
  });
  for (const name of RECOGNIZED_COLUMNS) {
    if (!indexOf.has(name))
      throw new Error(`${path}: missing required column ${name}`);
  }
  const at = (fields: string[], name: (typeof RECOGNIZED_COLUMNS)[number]): string => {
    const i = indexOf.get(name);
    const value = i === undefined ? undefined : fields[i];
    if (value === undefined)
      throw new Error(`${path}: row missing field ${name}`);
    return value;
  };
  const num = (fields: string[], name: (typeof RECOGNIZED_COLUMNS)[number]): number => {
    const v = Number(at(fields, name));
    if (!Number.isFinite(v))
      throw new Error(`${path}: non-numeric ${name}`);
    return v;
  };
  const rows: LadderRow[] = [];
  for (const line of lines.slice(1)) {
    const f = line.split(',');
    const target = num(f, 'target_iters');
    const completed = num(f, 'completed');
    // A cell is non-observed when its truncation flag is set OR it did
    // not complete its target iteration count; nothing is imputed.
    const truncated = at(f, 'truncated') !== '0' || completed < target;
    rows.push({
      mode: at(f, 'mode'),
      scheme: at(f, 'scheme'),
      seats: num(f, 'seats'),
      street: at(f, 'street'),
      seed: num(f, 'seed'),
      target_iters: target,
      safety_wall_s: num(f, 'safety_wall_s'),
      completed,
      infosets: num(f, 'infosets'),
      sealed_bytes: num(f, 'sealed_bytes'),
      joint_deals: num(f, 'joint_deals'),
      max_gap_bb: num(f, 'max_gap_bb'),
      sum_gap_bb: num(f, 'sum_gap_bb'),
      bucket_sum_gap_bb: num(f, 'bucket_sum_gap_bb'),
      wall_observed_s: num(f, 'wall_observed_s'),
      peak_rss_kb: num(f, 'peak_rss_kb'),
      truncated,
      note: at(f, 'note'),
    });
  }
  return rows;
}

export function loadLadderCsvs(paths: string[]): LadderRow[] {
  return paths.flatMap((p) => loadLadderCsv(p));
}

// --- Series aggregation ------------------------------------------------------

export interface SeedSample {
  seed: number;
  bucket: number;  // within-abstraction gap for this independent seed
  exact: number;   // exact-hole summed gap for this independent seed
}

export interface RungPoint {
  index: number;
  T: number;
  observed: boolean;                 // exactly five non-truncated seeds
  n: number;
  meanGap: number;                   // five-seed mean bucket gap
  sdN1: number;                      // sample SD, n-1 denominator
  hw: number;                        // t * s / sqrt(5)
  perSeedGap: Map<number, number>;
  meanExact: number;                 // five-seed mean exact gap
  perSeedExact: Map<number, number>;
}

function mean(xs: number[]): number {
  return xs.reduce((a, b) => a + b, 0) / xs.length;
}

function sampleSdN1(xs: number[]): number {
  if (xs.length < 2)
    return 0;
  const m = mean(xs);
  return Math.sqrt(xs.reduce((a, x) => a + (x - m) ** 2, 0) / (xs.length - 1));
}

// Build one ladder rung from its independent-seed samples. Exported so
// tests can synthesize rung arrays directly instead of going through
// the CSV loader.
export function buildRungPoint(
  index: number,
  samples: SeedSample[],
  ladder: readonly number[] = LADDER,
): RungPoint {
  const T = ladder[index]!;
  const perSeedGap = new Map<number, number>();
  const perSeedExact = new Map<number, number>();
  for (const s of samples) {
    if (perSeedGap.has(s.seed))
      throw new Error(`duplicate seed ${s.seed} at rung index ${index}`);
    perSeedGap.set(s.seed, s.bucket);
    perSeedExact.set(s.seed, s.exact);
  }
  const gaps = [...perSeedGap.values()];
  const exacts = [...perSeedExact.values()];
  const sdN1 = sampleSdN1(gaps);
  return {
    index,
    T,
    // Turn rows carry bucket gap -1 ("not applicable"); such a rung is
    // never an observed CERT cell.
    observed: samples.length === SEED_COUNT && gaps.every((g) => Number.isFinite(g) && g >= 0),
    n: samples.length,
    meanGap: gaps.length ? mean(gaps) : NaN,
    sdN1,
    hw: gaps.length ? (T_CRIT_0_975_4 * sdN1) / Math.sqrt(gaps.length) : NaN,
    perSeedGap,
    meanExact: exacts.length ? mean(exacts) : NaN,
    perSeedExact,
  };
}

// Aggregate rows already restricted to one scheme/street/seats group.
// Only ladder cells (mode === 'ladder', target on the frozen ladder,
// non-truncated) contribute.
export function aggregateSeries(
  rows: LadderRow[],
  ladder: readonly number[] = LADDER,
): RungPoint[] {
  return ladder.map((T, index) => {
    const samples: SeedSample[] = [];
    for (const r of rows) {
      if (r.mode !== 'ladder' || r.target_iters !== T || r.truncated)
        continue;
      samples.push({seed: r.seed, bucket: r.bucket_sum_gap_bb, exact: r.sum_gap_bb});
    }
    return buildRungPoint(index, samples, ladder);
  });
}

export interface GroupKey {
  seats: number;
  scheme: string;
  street: string;
}

export function groupKeyString(key: GroupKey): string {
  return `${key.seats}|${key.scheme}|${key.street}`;
}

// Group non-truncated ladder rows by (seats, scheme, street).
export function groupLadderRows(rows: LadderRow[]): Map<string, LadderRow[]> {
  const groups = new Map<string, LadderRow[]>();
  for (const r of rows) {
    if (r.mode !== 'ladder' || r.truncated)
      continue;
    const key = groupKeyString({seats: r.seats, scheme: r.scheme, street: r.street});
    const list = groups.get(key);
    if (list === undefined)
      groups.set(key, [r]);
    else
      list.push(r);
  }
  return groups;
}

// --- Log-linear power-law fit -----------------------------------------------

export interface PowerFit {
  a: number;  // g(T) = a * T^-b
  b: number;
}

export interface FitPoint {
  T: number;
  g: number;
}

// Ordinary least squares on ln g = ln a - b ln T over arbitrary rung
// subsets. Requires at least two points with distinct T and positive g.
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

// --- Rule 5: additive-floor diagnostic g(T) = c + a*T^-b, c >= 0 ------------

export interface FloorFit {
  c: number;
  a: number;
  b: number;
  sse: number;
}

interface ConstrainedLinear {
  c: number;
  a: number;
}

// For a FIXED b, set x_k = T_k^-b and fit g_k = c + a*x_k under
// c >= 0, a >= 0. With one regressor this is a convex quadratic over a
// wedge, so the projected optimum is one of: the interior OLS solution
// when both coefficients are non-negative, the c=0 edge, or the a=0
// edge. Evaluate the feasible candidates and keep the smallest SSE.
function fitLinearNonNeg(pts: Array<{x: number; g: number}>): ConstrainedLinear {
  const n = pts.length;
  const sx = pts.reduce((a, p) => a + p.x, 0);
  const sg = pts.reduce((a, p) => a + p.g, 0);
  const mx = sx / n;
  const mg = sg / n;
  let sxx = 0;
  let sxy = 0;
  for (const p of pts) {
    sxx += (p.x - mx) ** 2;
    sxy += (p.x - mx) * (p.g - mg);
  }
  const candidates: ConstrainedLinear[] = [];
  if (sxx > 0) {
    const aOls = sxy / sxx;
    const cOls = mg - aOls * mx;
    if (aOls >= 0 && cOls >= 0)
      candidates.push({a: aOls, c: cOls});
  }
  const sxxOrigin = pts.reduce((a, p) => a + p.x * p.x, 0);
  const sxg = pts.reduce((a, p) => a + p.x * p.g, 0);
  candidates.push({c: 0, a: sxxOrigin > 0 && sxg > 0 ? sxg / sxxOrigin : 0});  // c = 0 edge
  candidates.push({c: mg, a: 0});                                                 // a = 0 edge
  let best = candidates[0]!;
  let bestSse = Infinity;
  for (const cand of candidates) {
    const sse = pts.reduce((a, p) => a + (p.g - cand.c - cand.a * p.x) ** 2, 0);
    if (sse < bestSse) {
      bestSse = sse;
      best = cand;
    }
  }
  return best;
}

function floorSseAt(b: number, pts: FitPoint[]): {c: number; a: number; sse: number} {
  const lin = pts.map((p) => ({x: p.T ** -b, g: p.g}));
  // b = 0 makes T^-b identical for every rung, so a is absorbed into c.
  const {c, a} = b === 0 ? {c: mean(pts.map((p) => p.g)), a: 0} : fitLinearNonNeg(lin);
  const sse = pts.reduce((acc, p) => acc + (p.g - c - a * p.T ** -b) ** 2, 0);
  return {c, a, sse};
}

// Fit g(T) = c + a*T^-b with c,a >= 0 and b in [0, 2]. The outer search
// over b is a deterministic coarse grid (step 0.01) followed by a
// golden-section refinement inside the best grid cell; the linear
// sub-problem at each b has the closed/projection form above.
// windowRungs selects the fit window: the last windowRungs observed
// points, or every observed point when Infinity. The freeze addendum
// pins this window; until then RULE5_WINDOW_RUNGS is all-observed.
export function floorFit(points: FitPoint[], windowRungs: number = RULE5_WINDOW_RUNGS): FloorFit {
  const ordered = [...points].filter((p) => Number.isFinite(p.g) && p.g > 0)
    .sort((p, q) => p.T - q.T);
  if (ordered.length < 2)
    throw new Error('floorFit: need at least two observed rungs');
  const window = windowRungs === Infinity ? ordered : ordered.slice(-windowRungs);
  if (window.length < 2)
    throw new Error('floorFit: fit window needs at least two rungs');
  const B_MAX = 2;
  const GRID_STEPS = 200;
  let best: FloorFit = {c: 0, a: 0, b: 0, sse: Infinity};
  for (let k = 0; k <= GRID_STEPS; ++k) {
    const b = (B_MAX * k) / GRID_STEPS;
    const cur = floorSseAt(b, window);
    if (cur.sse < best.sse)
      best = {b, c: cur.c, a: cur.a, sse: cur.sse};
  }
  let lo = Math.max(0, best.b - B_MAX / GRID_STEPS);
  let hi = Math.min(B_MAX, best.b + B_MAX / GRID_STEPS);
  // Golden-section minimization inside the bracketing grid cell. The
  // SSE profile is smooth and locally treated as unimodal there.
  const GR = (Math.sqrt(5) - 1) / 2;
  let cB = hi - GR * (hi - lo);
  let dB = lo + GR * (hi - lo);
  let fc = floorSseAt(cB, window);
  let fd = floorSseAt(dB, window);
  for (let iter = 0; iter < 60; ++iter) {
    if (fc.sse < fd.sse) {
      hi = dB;
      dB = cB;
      fd = fc;
      cB = hi - GR * (hi - lo);
      fc = floorSseAt(cB, window);
    } else {
      lo = cB;
      cB = dB;
      fc = fd;
      dB = lo + GR * (hi - lo);
      fd = floorSseAt(dB, window);
    }
  }
  if (fc.sse < best.sse)
    best = {b: cB, c: fc.c, a: fc.a, sse: fc.sse};
  if (fd.sse < best.sse)
    best = {b: dB, c: fd.c, a: fd.a, sse: fd.sse};
  return best;
}

// Iteration count at which the fitted floor reaches EPS_GAP. Returns
// Infinity whenever the floor dominates (c >= EPS_GAP): no finite
// budget reaches the bar, and a finite number must never be emitted.
export function floorCrossingIterations(fit: FloorFit, eps: number = EPS_GAP): number {
  if (fit.c >= eps)
    return Infinity;
  if (fit.a <= 0 || fit.b <= 0)
    return 0;  // already below eps with no decaying component
  return (fit.a / (eps - fit.c)) ** (1 / fit.b);
}

export type FloorClassKind =
  | 'non-zero-floor'       // c >= EPS_GAP: structural floor for this grid
  | 'compute-ceiling'      // c < EPS_GAP but crossing is beyond 300M
  | 'crossing-below-cap';  // evaluator convenience: projected crossing <= 300M

export interface FloorClassification {
  kind: FloorClassKind;
  fit: FloorFit;
  crossingIterations: number;  // Infinity for non-zero-floor
}

// When the fitted floor sits inside this margin band below EPS_GAP the
// crossing budget is too ill-conditioned to quote; the classification still
// follows c < EPS_GAP (compute ceiling), but no finite budget is reported.
export const NEAR_FLOOR_RATIO = 0.8;

export function classifyFloor(
  fit: FloorFit,
  eps: number = EPS_GAP,
  cap: number = LADDER_CAP,
  nearFloorRatio: number = NEAR_FLOOR_RATIO,
): FloorClassification {
  if (fit.c >= eps)
    return {kind: 'non-zero-floor', fit, crossingIterations: Infinity};
  // c in (ratio*eps, eps): floor-dominated near the bar; the crossing
  // estimate is numerically unstable, so quote no finite budget.
  if (fit.c > eps * nearFloorRatio)
    return {kind: 'compute-ceiling', fit, crossingIterations: Infinity};
  const crossing = floorCrossingIterations(fit, eps);
  return {
    kind: crossing > cap ? 'compute-ceiling' : 'crossing-below-cap',
    fit,
    crossingIterations: crossing,
  };
}

// --- CERT evaluation (RFC 0044 section 2) -----------------------------------

export interface RungValues {
  g: number;
  hw: number;
  tailFit?: PowerFit;
  tailMaxRelResidual?: number;
  priorFit?: PowerFit;
  priorPredicted?: number;
  priorRelDeviation?: number;
}

export interface RungEvaluation {
  index: number;
  T: number;
  observed: boolean;
  eligible: boolean;  // i >= 3 and the rung and its prior window exist
  passes: {r1: boolean; r2: boolean; r3: boolean; r4: boolean};
  values: RungValues;
  failures: string[];
}

function isNonIncreasing(gs: number[]): boolean {
  // Machine-epsilon relative tolerance so equal plateau values count.
  for (let i = 1; i < gs.length; ++i) {
    if (gs[i - 1]! + 1e-12 * Math.abs(gs[i - 1]!) < gs[i]!)
      return false;
  }
  return true;
}

export function evaluateRung(index: number, series: readonly RungPoint[]): RungEvaluation {
  const point = series[index];
  const failures: string[] = [];
  const passes = {r1: false, r2: false, r3: false, r4: false};
  const values: RungValues = {g: NaN, hw: NaN};
  if (point && point.observed) {
    values.g = point.meanGap;
    values.hw = point.hw;
  }
  if (index < TAIL_WINDOW) {
    return {index, T: point ? point.T : LADDER[index]!, observed: !!point?.observed,
      eligible: false, passes, values, failures: ['ineligible:index<3']};
  }
  if (!point || !point.observed) {
    return {index, T: point ? point.T : LADDER[index]!, observed: false,
      eligible: false, passes, values, failures: ['not-observed']};
  }

  // Rule 1: absolute distance.
  passes.r1 = point.meanGap <= EPS_GAP;
  if (!passes.r1)
    failures.push('r1:absolute-distance');

  // Rule 2: independent-seed half-width.
  passes.r2 = point.hw <= HALFWIDTH_BAR;
  if (!passes.r2)
    failures.push('r2:half-width');

  // Rule 3: tail regime consistency on (i-2, i-1, i).
  const tail = [series[index - 2], series[index - 1], point];
  if (tail.some((p) => !p || !p.observed)) {
    failures.push('r3:tail-window-incomplete');
  } else {
    const tailPts = tail.map((p) => ({T: p!.T, g: p!.meanGap}));
    const fit = fitLogLinear(tailPts);
    const residual = maxRelResidual(fit, tailPts);
    values.tailFit = fit;
    values.tailMaxRelResidual = residual;
    const bOk = fit.b >= EXPONENT_MIN && fit.b <= EXPONENT_MAX;
    const residualOk = residual <= FIT_REL_TOLERANCE;
    const monotoneOk = isNonIncreasing(tailPts.map((p) => p.g));
    passes.r3 = bOk && residualOk && monotoneOk;
    if (!bOk)
      failures.push('r3:exponent-out-of-range');
    if (!residualOk)
      failures.push('r3:residual');
    if (!monotoneOk)
      failures.push('r3:not-non-increasing');
  }

  // Rule 4: prior-window forward prediction from (i-3, i-2, i-1);
  // every prior rung must be observed, nothing is imputed.
  const prior = [series[index - 3], series[index - 2], series[index - 1]];
  const eligible = prior.every((p) => p !== undefined && p.observed);
  if (!eligible) {
    failures.push('r4:prior-window-incomplete');
  } else {
    const priorPts = prior.map((p) => ({T: p!.T, g: p!.meanGap}));
    const fit = fitLogLinear(priorPts);
    const predicted = powerPredict(fit, point.T);
    const deviation = Math.abs(point.meanGap - predicted) / predicted;
    values.priorFit = fit;
    values.priorPredicted = predicted;
    values.priorRelDeviation = deviation;
    passes.r4 = deviation <= FIT_REL_TOLERANCE;
    if (!passes.r4)
      failures.push('r4:prior-prediction');
  }

  return {index, T: point.T, observed: true, eligible, passes, values, failures};
}

export interface CertVerdict {
  certified: boolean;
  rCert: number | null;  // smallest certifying ladder index
  rungs: RungEvaluation[];
  // Why no certification exists: failures at the first observed rung
  // that crossed the absolute EPS_GAP bar; if the series never crosses,
  // the single absolute-distance reason with the minimum observed gap.
  firstFailureReasons: string[];
}

export function evaluateCert(
  series: readonly RungPoint[],
  ladder: readonly number[] = LADDER,
): CertVerdict {
  const rungs = series.map((_, i) => evaluateRung(i, series));
  const certifiedRung = rungs.find(
    (r) => r.eligible && r.passes.r1 && r.passes.r2 && r.passes.r3 && r.passes.r4,
  );
  if (certifiedRung)
    return {certified: true, rCert: certifiedRung.index, rungs, firstFailureReasons: []};

  const crossing = rungs.find((r) => r.observed && r.passes.r1);
  let reasons: string[];
  if (crossing) {
    reasons = crossing.failures;
  } else {
    const gaps = rungs.filter((r) => r.observed).map((r) => r.values.g);
    const gMin = gaps.length ? Math.min(...gaps) : NaN;
    reasons = [`r1:absolute-distance:min-g=${gMin.toFixed(6)}`];
  }
  return {certified: false, rCert: null, rungs, firstFailureReasons: reasons};
}

// --- QUAL evaluation (RFC 0044 section 3) -----------------------------------

export interface QualSeedComparison {
  seed: number;
  equityExact: number;
  incumbentExact: number;
  strictlyBetter: boolean;
}

export interface QualVerdict {
  atRung: number;
  observed: boolean;
  meanExact: number;
  epsExact: number;
  meanPasses: boolean;
  paired: QualSeedComparison[];
  allSeedsStrictlyBetter: boolean;
  passes: boolean;
  failureReasons: string[];
}

// At r_cert the candidate grid must beat the paired incumbent control
// on every identical seed AND meet the five-seed mean exact bar.
export function evaluateQual(
  equitySeries: readonly RungPoint[],
  incumbentSeries: readonly RungPoint[],
  rCertIndex: number,
  epsExact: number = EPS_EXACT_HU,
): QualVerdict {
  const equity = equitySeries[rCertIndex];
  const incumbent = incumbentSeries[rCertIndex];
  const base: QualVerdict = {
    atRung: rCertIndex, observed: false, meanExact: NaN, epsExact,
    meanPasses: false, paired: [], allSeedsStrictlyBetter: false,
    passes: false, failureReasons: [],
  };
  if (!equity || !equity.observed || !incumbent || !incumbent.observed) {
    base.failureReasons.push('qual:rung-not-observed');
    return base;
  }
  base.observed = true;
  base.meanExact = equity.meanExact;
  base.meanPasses = equity.meanExact <= epsExact;
  const reasons: string[] = [];
  if (!base.meanPasses)
    reasons.push('qual:mean-exact-gap');
  const paired: QualSeedComparison[] = [];
  for (const seed of [...equity.perSeedExact.keys()].sort((a, b) => a - b)) {
    const e = equity.perSeedExact.get(seed)!;
    const t = incumbent.perSeedExact.get(seed);
    if (t === undefined) {
      paired.push({seed, equityExact: e, incumbentExact: NaN, strictlyBetter: false});
      continue;
    }
    paired.push({seed, equityExact: e, incumbentExact: t, strictlyBetter: e < t});
  }
  const missing = paired.filter((p) => !Number.isFinite(p.incumbentExact));
  const allBetter = paired.length > 0 &&
    paired.every((p) => Number.isFinite(p.incumbentExact) && p.strictlyBetter);
  base.paired = paired;
  base.allSeedsStrictlyBetter = allBetter;
  if (!allBetter)
    reasons.push(missing.length ? 'qual:unpaired-seeds' : 'qual:paired-superiority');
  base.passes = base.meanPasses && allBetter;
  base.failureReasons = reasons;
  return base;
}

// --- Certification report (CLI) ---------------------------------------------

export interface RungReport {
  index: number;
  T: number;
  observed: boolean;
  n: number;
  g: number | null;
  hw: number | null;
}

export interface TailFitReport {
  fromIndex: number;
  toIndex: number;
  fit: PowerFit;
  maxRelResidual: number;
  projection100M: number;
}

export interface RuleFiveReport {
  windowRungs: number;
  fit: FloorFit;
  classification: FloorClassification;
}

export interface CertificationGroup {
  seats: number;
  scheme: string;
  street: string;
  cert: CertVerdict;
  rungs: RungReport[];
  tailFit: TailFitReport | null;
  rule5: RuleFiveReport | null;
  qual: QualVerdict | null;
}

// Fit the three-rung tail over the last three observed rungs and
// project to 100M (the AC3 retrospective comparison).
function trailingTailFit(series: readonly RungPoint[]): TailFitReport | null {
  const observed = series.filter((p) => p.observed);
  if (observed.length < TAIL_WINDOW)
    return null;
  const window = observed.slice(-TAIL_WINDOW);
  const pts = window.map((p) => ({T: p.T, g: p.meanGap}));
  const fit = fitLogLinear(pts);
  return {
    fromIndex: window[0]!.index,
    toIndex: window[window.length - 1]!.index,
    fit,
    maxRelResidual: maxRelResidual(fit, pts),
    projection100M: powerPredict(fit, 100_000_000),
  };
}

export function buildCertificationReport(
  rows: LadderRow[],
  ladder: readonly number[] = LADDER,
  rule5Window: number = RULE5_WINDOW_RUNGS,
): CertificationGroup[] {
  // CERT is the river predicate.
  const river = rows.filter((r) => r.street === 'river');
  const groups = groupLadderRows(river);
  const seriesByGroup = new Map<string, {key: GroupKey; series: RungPoint[]}>();
  for (const [keyString, groupRows] of groups) {
    const [seatsRaw, scheme, street] = keyString.split('|');
    const key = {seats: Number(seatsRaw), scheme: scheme!, street: street!};
    seriesByGroup.set(keyString, {key, series: aggregateSeries(groupRows, ladder)});
  }
  const reports: CertificationGroup[] = [];
  for (const {key, series} of [...seriesByGroup.values()]
    .sort((x, y) => x.key.seats - y.key.seats || x.key.scheme.localeCompare(y.key.scheme))) {
    const cert = evaluateCert(series, ladder);
    const rungs: RungReport[] = series.map((p) => ({
      index: p.index, T: p.T, observed: p.observed, n: p.n,
      g: p.observed ? p.meanGap : null,
      hw: p.observed ? p.hw : null,
    }));
    let rule5: RuleFiveReport | null = null;
    if (!cert.certified) {
      const pts = series.filter((p) => p.observed).map((p) => ({T: p.T, g: p.meanGap}));
      if (pts.length >= 2) {
        const fit = floorFit(pts, rule5Window);
        rule5 = {
          windowRungs: rule5Window,
          fit,
          classification: classifyFloor(fit),
        };
      }
    }
    let qual: QualVerdict | null = null;
    if (cert.certified && cert.rCert !== null && key.scheme !== 'tiers') {
      const incumbent = seriesByGroup.get(groupKeyString({...key, scheme: 'tiers'}));
      if (incumbent)
        qual = evaluateQual(series, incumbent.series, cert.rCert);
    }
    reports.push({
      seats: key.seats, scheme: key.scheme, street: key.street,
      cert, rungs, tailFit: trailingTailFit(series), rule5, qual,
    });
  }
  return reports;
}

function fmt(x: number, digits = 6): string {
  return Number.isFinite(x) ? x.toFixed(digits) : 'NA';
}

function fmtIter(x: number): string {
  return Number.isFinite(x) ? Math.round(x).toLocaleString('en-US') : 'Infinity';
}

export function formatCertificationText(report: CertificationGroup[]): string {
  const lines: string[] = [];
  for (const g of report) {
    lines.push(`group seats=${g.seats} scheme=${g.scheme} street=${g.street}`);
    for (const r of g.rungs) {
      const body = r.observed
        ? `g=${fmt(r.g!)} hw=${fmt(r.hw!)}`
        : `n=${r.n} non-observed`;
      lines.push(`  rung[${r.index}] T=${r.T.toLocaleString('en-US')} ${r.observed ? 'observed ' : ''}${body}`);
    }
    if (g.cert.certified) {
      lines.push(`  CERT: CERTIFIED r_cert=${g.cert.rCert} (T=${g.rungs[g.cert.rCert!]?.T.toLocaleString('en-US')})`);
    } else {
      lines.push(`  CERT: NOT_CERTIFIED reasons=[${g.cert.firstFailureReasons.join(';')}]`);
    }
    if (g.tailFit) {
      const t = g.tailFit;
      lines.push(`  tail-fit rungs[${t.fromIndex}..${t.toIndex}] a=${fmt(t.fit.a, 4)} b=${fmt(t.fit.b, 4)} maxRelResidual=${fmt(t.maxRelResidual, 4)} g(100000000)=${fmt(t.projection100M)}`);
    }
    if (g.rule5) {
      const cls = g.rule5.classification;
      lines.push(`  rule5 window=${g.rule5.windowRungs === Infinity ? 'all-observed' : String(g.rule5.windowRungs)} c=${fmt(cls.fit.c)} a=${fmt(cls.fit.a, 4)} b=${fmt(cls.fit.b, 4)} class=${cls.kind} crossing=${fmtIter(cls.crossingIterations)} iterations`);
    }
    if (g.qual) {
      const q = g.qual;
      if (!q.observed) {
        lines.push('  QUAL: not-evaluable (r_cert rung missing in candidate or incumbent)');
      } else {
        lines.push(`  QUAL: ${q.passes ? 'PASS' : 'FAIL'} meanExact=${fmt(q.meanExact)} eps=${q.epsExact} allPairedStrict=${q.allSeedsStrictlyBetter} reasons=[${q.failureReasons.join(';')}]`);
        for (const p of q.paired)
          lines.push(`    seed=${p.seed} equity=${fmt(p.equityExact)} incumbent=${fmt(p.incumbentExact)} strictBetter=${p.strictlyBetter}`);
      }
    }
  }
  return lines.join('\n');
}

export interface CertificationArgs {
  paths: string[];
  asJson: boolean;
  rule5Window: number;
}

// Pure CLI argument parse: CSV paths, --json, and --rule5-window N|all.
// Exists as an exported function so path dropping is regression-tested.
export function parseCertificationArgs(args: string[]): CertificationArgs {
  const asJson = args.includes('--json');
  let rule5Window: number = RULE5_WINDOW_RUNGS;
  const winIdx = args.indexOf('--rule5-window');
  if (winIdx !== -1) {
    const v = args[winIdx + 1];
    if (v === undefined || (v !== 'all' && (!/^\d+$/.test(v) || Number(v) < 2)))
      throw new Error('usage: --rule5-window <N>=2..8 | all');
    rule5Window = v === 'all' ? Infinity : Number(v);
  }
  const paths = args.filter((a, i) =>
    a !== '--json' && !(winIdx !== -1 && (i === winIdx || i === winIdx + 1)));
  return {paths, asJson, rule5Window};
}

function certificationCli(paths: string[], asJson: boolean, rule5Window: number): void {
  const rows = loadLadderCsvs(paths);
  const report = buildCertificationReport(rows, LADDER, rule5Window);
  if (asJson) {
    console.log(JSON.stringify(report, null, 2));
  } else {
    console.log(formatCertificationText(report));
  }
}

const mode = process.argv[2];
if (mode === 'certification') {
  let parsed: CertificationArgs;
  try {
    parsed = parseCertificationArgs(process.argv.slice(3));
  } catch (e) {
    console.error((e as Error).message);
    process.exit(2);
  }
  if (parsed.paths.length === 0) {
    console.error('usage: rfc0044-gates certification <csv...> [--json] [--rule5-window N|all]');
    process.exit(2);
  }
  certificationCli(parsed.paths, parsed.asJson, parsed.rule5Window);
} else if (mode !== undefined) {
  console.error(`unknown mode ${mode}; use 'certification'`);
  process.exit(2);
}
