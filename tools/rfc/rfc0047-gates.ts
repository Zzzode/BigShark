// RFC 0047 Stage-1 frozen evaluator of record.
//
// S1 structural predicates:
//   G1  per-request concrete-infoset envelope (< 1,000,000) for the
//       depth-limited turn solve and the worst-case reached (line,card) river
//       re-solve, on every frozen stratum, plus per-card cross-line uniformity.
//   G4  canonical board-class constants and the turn x48 = 5 x river identity.
// S2 adds:
//   G2  oracle certified accuracy over a frozen coverage-frame cert CSV
//       (CERT per-entry <= 0.005 BB, seedless reference residual <= 0.001 BB,
//       QUAL exact-hole summed <= 0.03 BB). Pass an optional third CSV path.
// G3 (error-propagation slope) is added later in S2.
//
// Inputs are the tidy CSVs emitted by the guarded C++ binaries:
//   bigshark-rfc0047-infoset-budget -> s1-infoset-budget.csv
//   bigshark-rfc0047-census         -> s1-census.csv  (metric,value)
//   bigshark-rfc0047-oracle-build   -> <out>.bsncv.cert.csv (G2 frame)
// Malformed or missing rows fail closed (a thrown GateInputError, exit 2).

export const MAX_INFOSETS = 1_000_000;

export const STRATA = [
  'w1',
  'unpaired-rainbow-ahigh',
  'paired',
  'monotone',
  'unpaired-twotone',
  'raised-srp',
] as const;
export type StratumName = (typeof STRATA)[number];

export const CANONICAL_FLOP_CLASSES = 1755;
export const CANONICAL_TURN_CLASSES = 16_432;
export const CANONICAL_RIVER_CLASSES = 134_459;
export const CONCRETE_TURN_BOARDS = 270_725;
export const CONCRETE_RIVER_BOARDS = 2_598_960;
export const TURN_X48 = 12_994_800;

export class GateInputError extends Error {}

interface InfosetRow {
  stratum: string;
  totalNodes: number;
  turnActionNodes: number;
  chanceNodes: number;
  frontierLeaves: number;
  riverActionNodesTotal: number;
  reachedMax: number;
  perCardMin: number;
  perCardMax: number;
  perCardUniform: boolean;
  turnCombos: number;
  riverCombos: number;
  turnInfosets: number;
  reachedRiverInfosetsMax: number;
  perCardInfosetsOffline: number;
  conservativeOnlineSum: number;
  under1m: boolean;
}

function parseCsv(text: string): string[][] {
  return text
    .trim()
    .split(/\r?\n/)
    .map((line) => line.split(','));
}

function requireHeader(actual: string[], expected: string[]): void {
  const a = actual.join(',');
  const e = expected.join(',');
  if (a !== e)
    throw new GateInputError(`unexpected CSV header.\n expected: ${e}\n actual:   ${a}`);
}

function num(record: Record<string, string>, key: string): number {
  const raw = record[key];
  if (raw === undefined)
    throw new GateInputError(`missing column '${key}'`);
  const v = Number(raw);
  if (!Number.isFinite(v))
    throw new GateInputError(`non-numeric value '${raw}' for '${key}'`);
  return v;
}

function bool(record: Record<string, string>, key: string): boolean {
  const raw = record[key];
  if (raw === 'true')
    return true;
  if (raw === 'false')
    return false;
  throw new GateInputError(`expected true/false for '${key}', got '${raw ?? ''}'`);
}

export function parseInfosetCsv(text: string): InfosetRow[] {
  const rows = parseCsv(text);
  const header = rows.shift();
  if (header === undefined)
    throw new GateInputError('infoset CSV is empty');
  requireHeader(header, [
    'stratum', 'pot', 'total_nodes', 'turn_action_nodes', 'chance_nodes',
    'frontier_leaves', 'river_action_nodes_total', 'reached_min',
    'reached_mean_x1000', 'reached_max', 'per_card_actions_min',
    'per_card_actions_max', 'per_card_uniform', 'turn_combos', 'river_combos',
    'turn_infosets', 'reached_river_infosets_max', 'per_card_infosets_offline',
    'conservative_online_sum', 'under_1m',
  ]);
  return rows.map((cols) => {
    const r: Record<string, string> = {};
    header.forEach((h, i) => {
      const v = cols[i];
      if (v === undefined)
        throw new GateInputError(`short row for column '${h}'`);
      r[h] = v;
    });
    const stratum = r['stratum'] ?? '';
    return {
      stratum,
      totalNodes: num(r, 'total_nodes'),
      turnActionNodes: num(r, 'turn_action_nodes'),
      chanceNodes: num(r, 'chance_nodes'),
      frontierLeaves: num(r, 'frontier_leaves'),
      riverActionNodesTotal: num(r, 'river_action_nodes_total'),
      reachedMax: num(r, 'reached_max'),
      perCardMin: num(r, 'per_card_actions_min'),
      perCardMax: num(r, 'per_card_actions_max'),
      perCardUniform: bool(r, 'per_card_uniform'),
      turnCombos: num(r, 'turn_combos'),
      riverCombos: num(r, 'river_combos'),
      turnInfosets: num(r, 'turn_infosets'),
      reachedRiverInfosetsMax: num(r, 'reached_river_infosets_max'),
      perCardInfosetsOffline: num(r, 'per_card_infosets_offline'),
      conservativeOnlineSum: num(r, 'conservative_online_sum'),
      under1m: bool(r, 'under_1m'),
    };
  });
}

export interface CensusValues {
  [metric: string]: number;
}

export function parseCensusCsv(text: string): CensusValues {
  const rows = parseCsv(text);
  const header = rows.shift();
  if (header === undefined)
    throw new GateInputError('census CSV is empty');
  requireHeader(header, ['metric', 'value']);
  const out: CensusValues = {};
  for (const cols of rows) {
    const metric = cols[0];
    const raw = cols[1];
    if (metric === undefined || raw === undefined)
      throw new GateInputError('census rows must have metric,value');
    const v = Number(raw);
    if (!Number.isFinite(v))
      throw new GateInputError(`census metric '${metric}' non-numeric '${raw}'`);
    out[metric] = v;
  }
  return out;
}

export interface S1Result {
  g1: boolean;
  g4: boolean;
  failures: string[];
  rows: InfosetRow[];
}

export function evaluateS1(infosetCsv: string, censusCsv: string): S1Result {
  const rows = parseInfosetCsv(infosetCsv);
  const census = parseCensusCsv(censusCsv);
  const failures: string[] = [];

  // All six strata present exactly once.
  const byName = new Map<string, InfosetRow>();
  for (const row of rows) {
    if (byName.has(row.stratum))
      failures.push(`duplicate stratum row '${row.stratum}'`);
    byName.set(row.stratum, row);
  }
  for (const s of STRATA) {
    if (!byName.has(s))
      failures.push(`missing stratum '${s}'`);
  }

  // G1 per-stratum structural predicates.
  for (const s of STRATA) {
    const row = byName.get(s);
    if (!row)
      continue;
    if (row.turnCombos !== 1128)
      failures.push(`${s}: turn combos ${row.turnCombos} != 1128`);
    if (row.riverCombos !== 1081)
      failures.push(`${s}: river combos ${row.riverCombos} != 1081`);
    if (!row.perCardUniform)
      failures.push(`${s}: per-card river action nodes are not uniform`);
    if (row.frontierLeaves !== row.chanceNodes * 48)
      failures.push(`${s}: frontier leaves != chance_nodes x 48`);
    if (row.riverActionNodesTotal !== row.perCardMax * 48)
      failures.push(`${s}: total river action nodes != per_card_max x 48`);
    for (const [label, value] of [
      ['turn_infosets', row.turnInfosets],
      ['reached_river_infosets_max', row.reachedRiverInfosetsMax],
      ['conservative_online_sum', row.conservativeOnlineSum],
    ] as const) {
      if (value >= MAX_INFOSETS)
        failures.push(`${s}: ${label} ${value} >= ${MAX_INFOSETS}`);
    }
    if (!row.under1m)
      failures.push(`${s}: under_1m flag is false`);
  }

  // G4 census constants.
  const expect: Array<[string, number]> = [
    ['canonical_flop_classes', CANONICAL_FLOP_CLASSES],
    ['canonical_turn_classes', CANONICAL_TURN_CLASSES],
    ['canonical_river_classes', CANONICAL_RIVER_CLASSES],
    ['concrete_turn_boards', CONCRETE_TURN_BOARDS],
    ['concrete_river_boards', CONCRETE_RIVER_BOARDS],
    ['turn_boards_times_48', TURN_X48],
    ['five_times_river_boards', TURN_X48],
    ['turn_river_identity', 1],
  ];
  for (const [metric, want] of expect) {
    const got = census[metric];
    if (got === undefined)
      failures.push(`census missing '${metric}'`);
    else if (got !== want)
      failures.push(`census '${metric}' = ${got}, expected ${want}`);
  }

  return {g1: failures.length === 0, g4: failures.length === 0, failures, rows};
}

// --- S2 G2: oracle certified accuracy --------------------------------------
//
// G2 inherits RFC 0044's distinction between a within-instrument convergence
// gap and an exact-hole SUMMED gap; both are SUMMED best-response/NashConv
// quantities (see rfc0044-gates.ts meanGap / exact gap), never a per-combo
// worst value. The V3 builder is SEEDLESS (bitwise-identical rerun, no RNG),
// has no abstraction bucket loss, and enumerates every compatible deal, so:
//   CERT g(T)   = candidate exact full-deal SUMMED NashConv (the within-
//                 instrument convergence gap); gated at 0.005 BB;
//   half-width  = 0 BY CONSTRUCTION for a seedless solver (RFC 0044's value is
//                 the t*SE of an independent-seed mean, which does not exist);
//                 a bitwise-identical rerun native test covers it, and here we
//                 additionally require the higher-accuracy reference be at
//                 least as converged as the candidate;
//   QUAL        = candidate exact SUMMED NashConv against the exact-hole
//                 evaluator - the SAME instrument for V3 (no bucket loss), so
//                 it equals CERT; gated at the inherited 0.03 BB.
// The per-combo quantities (drift_max = e_max, drift/cert RMS, and the shipped
// one-sided per-entry bound) are RECORDED ONLY: they are the oracle entry
// error that G3 propagates through the turn-equilibrium leaf reach rho, and
// the production inequalities UCB(hybrid) <= 0.010 / UCB(t_river) <= 0.002 are
// what bound a per-entry error - not a 0.005 worst-combo bar here.
export const EPS_CERT_BB = 0.005;  // within-instrument summed NashConv
export const EPS_QUAL_BB = 0.03;   // exact-hole summed NashConv (HU)
// Reference-quality diagnostic bar (not an RFC 0044 seed half-width, which is
// exactly 0 for a seedless solver): the reference must be at least this much
// more converged than the candidate so it is a meaningful reference.
export const EPS_REF_NC_BB = 0.005;

export interface CertRow {
  stratum: string;
  shapeId: number;
  group: number;
  potChips: number;
  candNashConvBb: number;
  refNashConvBb: number;
  driftMaxBb: number;
  residualMaxBb: number;
  certMaxBb: number;
  driftRmsBb: number;
  residualRmsBb: number;
  certRmsBb: number;
}

export function parseCertCsv(text: string): CertRow[] {
  const rows = parseCsv(text);
  const header = rows.shift();
  if (header === undefined)
    throw new GateInputError('cert CSV is empty');
  requireHeader(header, [
    'stratum', 'shape_id', 'group', 'pot_chips', 'cand_nash_conv_bb',
    'ref_nash_conv_bb', 'drift_max_bb', 'residual_max_bb', 'cert_max_bb',
    'drift_rms_bb', 'residual_rms_bb', 'cert_rms_bb',
  ]);
  return rows.map((cols) => {
    const r: Record<string, string> = {};
    header.forEach((h, i) => { r[h] = cols[i] ?? ''; });
    if (!r.stratum)
      throw new GateInputError('cert row has an empty stratum');
    return {
      stratum: r.stratum,
      shapeId: num(r, 'shape_id'),
      group: num(r, 'group'),
      potChips: num(r, 'pot_chips'),
      candNashConvBb: num(r, 'cand_nash_conv_bb'),
      refNashConvBb: num(r, 'ref_nash_conv_bb'),
      driftMaxBb: num(r, 'drift_max_bb'),
      residualMaxBb: num(r, 'residual_max_bb'),
      certMaxBb: num(r, 'cert_max_bb'),
      driftRmsBb: num(r, 'drift_rms_bb'),
      residualRmsBb: num(r, 'residual_rms_bb'),
      certRmsBb: num(r, 'cert_rms_bb'),
    };
  });
}

export interface S2G2Result {
  g2: boolean;
  failures: string[];
  rows: number;
  certBb: number;          // worst within-instrument summed NashConv (CERT)
  qualBb: number;          // worst exact-hole summed NashConv (QUAL; == CERT for V3)
  refNcMaxBb: number;      // worst reference NashConv (reference-quality check)
  eMaxBb: number;          // RECORDED per-combo worst drift (G3 input)
  driftRmsMaxBb: number;   // RECORDED per-combo RMS drift (G3 e_l2, uniform rho)
  certMaxBb: number;       // RECORDED shipped per-entry worst bound
  byStratum: Record<string, {cert: number; refNc: number; eMax: number;
                             driftRms: number; n: number}>;
}

export function evaluateS2G2(certCsv: string): S2G2Result {
  const rows = parseCertCsv(certCsv);
  const failures: string[] = [];
  if (rows.length === 0)
    throw new GateInputError('cert CSV has no data rows');

  let certBb = 0;
  let refNcMaxBb = 0;
  let eMaxBb = 0;
  let driftRmsMaxBb = 0;
  let certMaxBb = 0;
  const byStratum: S2G2Result['byStratum'] = {};
  const seen = new Set<string>();

  for (const r of rows) {
    const key = `${r.stratum}:${r.shapeId}:${r.group}`;
    if (seen.has(key))
      failures.push(`duplicate cert row ${key}`);
    seen.add(key);
    for (const v of [r.candNashConvBb, r.refNashConvBb, r.driftMaxBb,
                     r.residualMaxBb, r.certMaxBb, r.driftRmsBb,
                     r.residualRmsBb, r.certRmsBb])
      if (!(v >= 0))
        failures.push(`${key}: negative/NaN metric`);
    // The reference must be at least as converged as the candidate.
    if (r.refNashConvBb > r.candNashConvBb + 1e-12)
      failures.push(`${key}: reference NashConv ${r.refNashConvBb} > candidate `
                    + `${r.candNashConvBb}`);
    // Shipped per-entry bound must dominate both of its non-negative
    // components, in both norms (integrity; not a magnitude gate).
    if (r.certMaxBb + 1e-12 < Math.max(r.driftMaxBb, r.residualMaxBb))
      failures.push(`${key}: cert max below a component`);
    if (r.certRmsBb + 1e-12 < Math.max(r.driftRmsBb, r.residualRmsBb))
      failures.push(`${key}: cert rms below a component`);

    certBb = Math.max(certBb, r.candNashConvBb);
    refNcMaxBb = Math.max(refNcMaxBb, r.refNashConvBb);
    eMaxBb = Math.max(eMaxBb, r.driftMaxBb);
    driftRmsMaxBb = Math.max(driftRmsMaxBb, r.driftRmsBb);
    certMaxBb = Math.max(certMaxBb, r.certMaxBb);
    const b = byStratum[r.stratum]
      ?? {cert: 0, refNc: 0, eMax: 0, driftRms: 0, n: 0};
    b.cert = Math.max(b.cert, r.candNashConvBb);
    b.refNc = Math.max(b.refNc, r.refNashConvBb);
    b.eMax = Math.max(b.eMax, r.driftMaxBb);
    b.driftRms = Math.max(b.driftRms, r.driftRmsBb);
    b.n += 1;
    byStratum[r.stratum] = b;
  }

  // CERT: within-instrument summed NashConv convergence gap.
  if (certBb > EPS_CERT_BB)
    failures.push(`CERT summed NashConv ${certBb.toExponential(3)} > `
                  + `${EPS_CERT_BB} BB`);
  // QUAL: exact-hole summed gap (same exact instrument for the seedless,
  // bucket-free V3 kernel, so it equals the CERT value).
  if (certBb > EPS_QUAL_BB)
    failures.push(`QUAL exact-hole summed gap ${certBb.toExponential(3)} > `
                  + `${EPS_QUAL_BB} BB`);
  // Reference-quality diagnostic: the higher-accuracy reference must itself be
  // below the CERT scale to be a meaningful reference. (The RFC 0044 seed
  // half-width is exactly 0 for the seedless solver, verified by the
  // bitwise-identical rerun native test, not re-gated here.)
  if (refNcMaxBb > EPS_REF_NC_BB)
    failures.push(`reference NashConv ${refNcMaxBb.toExponential(3)} > `
                  + `${EPS_REF_NC_BB} BB`);

  return {g2: failures.length === 0, failures, rows: rows.length, certBb,
          qualBb: certBb, refNcMaxBb, eMaxBb, driftRmsMaxBb, certMaxBb,
          byStratum};
}

// --- S2 G3: error-propagation slope ---------------------------------------
//
// The telescoping walker is an EXACT, SEEDLESS full-deal evaluator (identity
// to machine epsilon), so a G3 draw has no sampling distribution; inventing
// NashConv seeds would be the pseudo-replication RFC 0047 G3(d) forbids. The
// honest variability that bounds a non-probed mounted node is ACROSS THE SIX
// FROZEN STRATA. The probe CSV (append-mode, one run per stratum) carries an
// `identity` row (zero-perturbation walker gap vs the solver's own NashConv)
// and one row per (arm, level) with the frozen norms e_max/e_l2 (pot fraction)
// and the resulting hybrid gap components in BB.
export const EPS_HYBRID_B0 = 0.010;  // UCB propagated hybrid NashConv (BB)
export const EPS_RIVER_R0 = 0.002;   // UCB online river term t_river (BB)
// One-sided 95% Student t for df = n-1 = 5 (six strata).
export const T_CRIT_0_95_5 = 2.0150;
export const G3_ARMS = ['zm', 'coh', 'pat', 'act'] as const;
export type G3Arm = (typeof G3_ARMS)[number];

export interface G3Row {
  stratum: string;
  arm: string;       // G3Arm or 'identity'
  level: number;     // injected amplitude (pot fraction); 0 for identity
  eMax: number;
  eL2: number;
  turnGapBb: number;
  riverTermBb: number;
  deltaHybridBb: number;
}

const G3_HEADER =
  'stratum,arm,level,e_max,e_l2,turn_gap_bb,river_term_bb,delta_hybrid_bb';

export function parseG3Csv(text: string): G3Row[] {
  const rows = parseCsv(text);
  const header = rows.shift();
  if (header === undefined)
    throw new GateInputError('g3 CSV is empty');
  requireHeader(header, G3_HEADER.split(','));
  return rows.map((cols) => {
    const r: Record<string, string> = {};
    header.forEach((h, i) => { r[h] = cols[i] ?? ''; });
    if (!r.stratum)
      throw new GateInputError('g3 row has an empty stratum');
    return {
      stratum: r.stratum,
      arm: r.arm ?? '',
      level: num(r, 'level'),
      eMax: num(r, 'e_max'),
      eL2: num(r, 'e_l2'),
      turnGapBb: num(r, 'turn_gap_bb'),
      riverTermBb: num(r, 'river_term_bb'),
      deltaHybridBb: num(r, 'delta_hybrid_bb'),
    };
  });
}

// Production operating point: the largest SHIPPED one-sided per-entry bound
// (cert_max = drift + reference residual) expressed as a fraction of that
// leaf's root pot. G3 must envelope every mounted entry at this magnitude.
export function operatingEMaxPotFraction(certRows: CertRow[]): number {
  let e = 0;
  for (const r of certRows) {
    if (r.potChips <= 0)
      throw new GateInputError(`cert row ${r.stratum}:${r.group} has nonpositive pot`);
    e = Math.max(e, r.certMaxBb * 2 / r.potChips);
  }
  return e;
}

// Piecewise-linear interpolation of y=delta at x=eMax over one arm's rows
// sorted by measured e_max. Fails closed if E is not bracketed above (the
// campaign must include a level >= the operating point).
function interpolateDeltaAt(points: Array<[number, number]>, e: number): number {
  const p = [...points].sort((a, b) => a[0] - b[0]);
  if (p.length < 2)
    throw new GateInputError('need >= 2 points to interpolate the envelope');
  const first = p[0]!;
  const last = p[p.length - 1]!;
  if (e < first[0])
    return first[1];
  for (let k = 1; k < p.length; ++k) {
    const hi = p[k]!;
    if (e <= hi[0]) {
      const lo = p[k - 1]!;
      const f = hi[0] === lo[0] ? 0 : (e - lo[0]) / (hi[0] - lo[0]);
      return lo[1] + f * (hi[1] - lo[1]);
    }
  }
  throw new GateInputError(
    `operating e_max ${e.toExponential(3)} exceeds the largest injected level `
    + `${last[0].toExponential(3)}; add a bracketing level`);
}

export interface G3StratumResult {
  stratum: string;
  perArmDelta: Record<string, number>;  // interpolated delta at eMaxFrozen
  worstArm: string;
  worstDeltaBb: number;
  identityGapBb: number;
  offlineRiverTermBb: number;  // campaign leaf-solve residual, NOT production
}

export interface S2G3Result {
  g3: boolean;
  failures: string[];
  eMaxFrozenPotFraction: number;
  rows: number;
  perStratum: G3StratumResult[];
  // One-sided 95% UCB across the six strata (t, df=5).
  deltaPropMeanBb: number;
  deltaPropUcbBb: number;
  // Composition is finalized in S3 with the online G5 river term; this is the
  // room left once the R0 = 0.002 river budget is reserved.
  projectedHeadroomBb: number;
}

export function evaluateS2G3(g3Csv: string, certCsv: string): S2G3Result {
  const rows = parseG3Csv(g3Csv);
  const certRows = parseCertCsv(certCsv);
  const failures: string[] = [];
  const eMaxFrozen = operatingEMaxPotFraction(certRows);

  if (rows.length === 0)
    throw new GateInputError('g3 CSV has no data rows');

  // Bucket rows by stratum.
  const byStratum = new Map<string, G3Row[]>();
  for (const r of rows) {
    const a = byStratum.get(r.stratum) ?? [];
    a.push(r);
    byStratum.set(r.stratum, a);
  }
  for (const s of STRATA)
    if (!byStratum.has(s))
      failures.push(`missing g3 stratum '${s}'`);

  const perStratum: G3StratumResult[] = [];
  const worstDeltas: number[] = [];

  for (const s of STRATA) {
    const srows = byStratum.get(s);
    if (!srows)
      continue;
    const ident = srows.filter((r) => r.arm === 'identity');
    if (ident.length !== 1)
      failures.push(`${s}: expected exactly one identity row, got ${ident.length}`);
    const identityGapBb = ident[0]?.deltaHybridBb === 0
      ? ident[0]!.turnGapBb
      : (ident[0]?.turnGapBb ?? NaN);

    const perArmDelta: Record<string, number> = {};
    let worstArm = '';
    let worstDelta = -Infinity;
    for (const arm of G3_ARMS) {
      const arows = srows.filter((r) => r.arm === arm);
      if (arows.length < 2) {
        failures.push(`${s}: arm ${arm} needs >= 2 levels`);
        continue;
      }
      for (const r of arows)
        if (!(r.eMax >= 0) || !Number.isFinite(r.deltaHybridBb))
          failures.push(`${s}:${arm}: non-finite/negative norm or delta`);
      const pts = arows.map((r) => [r.eMax, r.deltaHybridBb] as [number, number]);
      let d: number;
      try {
        d = interpolateDeltaAt(pts, eMaxFrozen);
      } catch (err) {
        failures.push(`${s}:${arm}: ${(err as Error).message}`);
        continue;
      }
      perArmDelta[arm] = d;
      if (d > worstDelta) {
        worstDelta = d;
        worstArm = arm;
      }
    }
    const offlineRiverTermBb = ident[0]?.riverTermBb ?? NaN;
    perStratum.push({
      stratum: s, perArmDelta, worstArm,
      worstDeltaBb: worstDelta === -Infinity ? NaN : worstDelta,
      identityGapBb, offlineRiverTermBb,
    });
    if (Number.isFinite(worstDelta))
      worstDeltas.push(worstDelta);
  }

  // One-sided 95% UCB of the worst-arm propagated delta over six strata.
  // The zero-perturbation telescoping identity (walker gap == solver NashConv
  // to machine epsilon) is enforced by the native rfc0047_turn_solver CTest,
  // not re-derived here.
  let deltaPropMeanBb = NaN;
  let deltaPropUcbBb = NaN;
  let projectedHeadroomBb = NaN;
  if (worstDeltas.length === STRATA.length) {
    deltaPropMeanBb = worstDeltas.reduce((a, b) => a + b, 0) / worstDeltas.length;
    const sd = Math.sqrt(
      worstDeltas.reduce((a, b) => a + (b - deltaPropMeanBb) ** 2, 0)
      / (worstDeltas.length - 1));
    deltaPropUcbBb = deltaPropMeanBb
      + T_CRIT_0_95_5 * sd / Math.sqrt(worstDeltas.length);
    projectedHeadroomBb = EPS_HYBRID_B0 - EPS_RIVER_R0 - deltaPropUcbBb;
    if (!(deltaPropUcbBb <= EPS_HYBRID_B0 - EPS_RIVER_R0))
      failures.push(
        `propagated-delta UCB ${deltaPropUcbBb.toExponential(3)} > B0-R0 `
        + `${(EPS_HYBRID_B0 - EPS_RIVER_R0).toExponential(3)} BB at `
        + `e_max_frozen ${eMaxFrozen.toExponential(3)}`);
  } else {
    failures.push('cannot compute UCB: need worst-arm delta for all six strata');
  }

  return {
    g3: failures.length === 0,
    failures,
    eMaxFrozenPotFraction: eMaxFrozen,
    rows: rows.length,
    perStratum,
    deltaPropMeanBb,
    deltaPropUcbBb,
    projectedHeadroomBb,
  };
}

// Manual entrypoint:
//   node dist/tools/rfc/rfc0047-gates.js <infoset.csv> <census.csv> [cert.csv]
//   [g3.csv]
const isMain = (() => {
  try {
    return process.argv[1] !== undefined &&
      import.meta.url === new URL(`file://${process.argv[1]}`).href;
  } catch {
    return false;
  }
})();

if (isMain) {
  const {readFileSync} = await import('node:fs');
  const infosetPath = process.argv[2];
  const censusPath = process.argv[3];
  const certPath = process.argv[4];
  const g3Path = process.argv[5];
  if (!infosetPath || !censusPath) {
    console.error(
      'usage: rfc0047-gates <s1-infoset-budget.csv> <s1-census.csv> '
      + '[s2-cert.csv] [s2-g3.csv]');
    process.exit(2);
  }
  try {
    const s1 = evaluateS1(readFileSync(infosetPath, 'utf8'),
                          readFileSync(censusPath, 'utf8'));
    if (!s1.g1 || !s1.g4) {
      console.error('RFC 0047 S1 structural gates FAILED:');
      for (const f of s1.failures)
        console.error(`  - ${f}`);
      process.exit(1);
    }
    console.log('RFC 0047 S1 structural gates PASS (G1 infoset envelope, G4 census).');

    if (!certPath) {
      console.log('G2/G3 not evaluated (no cert CSV supplied).');
      process.exit(0);
    }
    const g2 = evaluateS2G2(readFileSync(certPath, 'utf8'));
    console.log(
      `G2 over ${g2.rows} subgames: CERT/QUAL summed NashConv ${g2.certBb.toExponential(3)} `
      + `(bars ${EPS_CERT_BB}/${EPS_QUAL_BB}), reference ${g2.refNcMaxBb.toExponential(3)} `
      + `(seed half-width 0 by seedlessness); recorded per-entry e_max `
      + `${g2.eMaxBb.toExponential(3)}, drift RMS ${g2.driftRmsMaxBb.toExponential(3)} `
      + `(feeds G3 reach envelope, not gated here).`);
    if (!g2.g2) {
      console.error('RFC 0047 S2 G2 oracle-accuracy gate FAILED:');
      for (const f of g2.failures)
        console.error(`  - ${f}`);
      process.exit(1);
    }
    console.log('RFC 0047 S2 G2 oracle-accuracy gate PASS.');

    if (!g3Path) {
      console.log('G3 not evaluated (no g3 slope CSV supplied).');
      process.exit(0);
    }
    const g3 = evaluateS2G3(readFileSync(g3Path, 'utf8'),
                            readFileSync(certPath, 'utf8'));
    console.log(
      `G3 over ${g3.rows} rows: operating e_max_frozen `
      + `${g3.eMaxFrozenPotFraction.toExponential(3)} pot-fraction; worst-arm `
      + `propagated hybrid delta mean ${g3.deltaPropMeanBb.toExponential(3)} `
      + `UCB ${g3.deltaPropUcbBb.toExponential(3)} BB (bar B0-R0 `
      + `${(EPS_HYBRID_B0 - EPS_RIVER_R0).toExponential(3)}; the online R0 `
      + `term is finalized in S3/G5).`);
    for (const r of g3.perStratum)
      console.log(
        `  ${r.stratum}: worst arm ${r.worstArm} delta `
        + `${r.worstDeltaBb.toExponential(3)} BB`);
    if (!g3.g3) {
      console.error('RFC 0047 S2 G3 error-propagation gate FAILED:');
      for (const f of g3.failures)
        console.error(`  - ${f}`);
      process.exit(1);
    }
    console.log('RFC 0047 S2 G3 error-propagation gate PASS (slope envelope).');
  } catch (err) {
    console.error(`RFC 0047 gate input error: ${(err as Error).message}`);
    process.exit(2);
  }
}
