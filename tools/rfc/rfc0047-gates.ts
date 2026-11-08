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

// --- G3 exact box-corner convexity/tail check (review R1 automation) ------

export interface G3CornerRow {
  stratum: string;
  eMax: number;
  eL2: number;
  measuredDeltaBb: number;
  analyticBoundBb: number;
}

const G3_CORNER_HEADER =
  'stratum,arm,level,e_max,e_l2,turn_gap_bb,river_term_bb,delta_hybrid_bb,'
  + 'analytic_bound_bb';

export function parseG3CornerCsv(text: string): G3CornerRow[] {
  const rows = parseCsv(text);
  const header = rows.shift();
  if (header === undefined)
    throw new GateInputError('g3 corner CSV is empty');
  requireHeader(header, G3_CORNER_HEADER.split(','));
  return rows
    .filter((cols) => cols[1] === 'corner')
    .map((cols) => {
      const r: Record<string, string> = {};
      header.forEach((h, i) => { r[h] = cols[i] ?? ''; });
      if (!r.stratum)
        throw new GateInputError('g3 corner row has empty stratum');
      return {
        stratum: r.stratum,
        eMax: num(r, 'e_max'),
        eL2: num(r, 'e_l2'),
        measuredDeltaBb: num(r, 'delta_hybrid_bb'),
        analyticBoundBb: num(r, 'analytic_bound_bb'),
      };
    });
}

export interface G3CornerResult {
  ok: boolean;
  failures: string[];
  rows: number;
  perStratum: Array<{stratum: string; measuredDeltaBb: number;
                     analyticBoundBb: number; ratio: number}>;
}

// Fail-closed assertion that the exact box-corner optimizer's MEASURED
// propagated delta never exceeds its analytic first-order bound (convexity /
// BR-switching / negative tail cannot break the subgradient certificate at the
// claimed point). One corner row per stratum.
export function evaluateG3Corner(cornerCsv: string): G3CornerResult {
  const rows = parseG3CornerCsv(cornerCsv);
  const failures: string[] = [];
  for (const s of STRATA)
    if (!rows.some((r) => r.stratum === s))
      failures.push(`missing exact-corner row for '${s}'`);
  const perStratum = rows.map((r) => ({
    stratum: r.stratum,
    measuredDeltaBb: r.measuredDeltaBb,
    analyticBoundBb: r.analyticBoundBb,
    ratio: r.analyticBoundBb > 0 ? r.measuredDeltaBb / r.analyticBoundBb : NaN,
  }));
  for (const r of rows)
    if (!(r.measuredDeltaBb <= r.analyticBoundBb))
      failures.push(
        `${r.stratum}: exact-corner measured ${r.measuredDeltaBb.toExponential(3)} `
        + `> analytic bound ${r.analyticBoundBb.toExponential(3)} BB`);
  return {ok: failures.length === 0, failures, rows: rows.length, perStratum};
}

const G3_HEADER =
  'stratum,arm,level,e_max,e_l2,turn_gap_bb,river_term_bb,delta_hybrid_bb';

export function parseG3Csv(text: string): G3Row[] {
  const rows = parseCsv(text);
  const header = rows.shift();
  if (header === undefined)
    throw new GateInputError('g3 CSV is empty');
  // Accept the current 8-column schema and the earlier 9-column schema that
  // also carried a per-injection analytic upper-bound column.
  const base = G3_HEADER.split(',');
  if (header.length === base.length + 1) {
    if (header.join(',') !== [...base, 'analytic_bound_bb'].join(','))
      throw new GateInputError('unexpected 9-column g3 header');
  } else {
    requireHeader(header, base);
  }
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

// Second operating coordinate: the largest shipped RMS per-entry bound
// (cert_rms) over root pot. RFC 0047 G3(a) defines BOTH e_max and e_l2; the
// deterministic exact solver's residual is diffuse mixed-sign, so the RMS is
// far below the max and is what makes a mounted node realizable.
export function operatingEL2PotFraction(certRows: CertRow[]): number {
  let l = 0;
  for (const r of certRows) {
    if (r.potChips <= 0)
      throw new GateInputError(`cert row ${r.stratum}:${r.group} has nonpositive pot`);
    l = Math.max(l, r.certRmsBb * 2 / r.potChips);
  }
  return l;
}

// Interpolate one arm's (e_l2, delta) at a target e_max along its amplitude
// ray. Fails closed if E is not bracketed above (the campaign must include a
// level whose measured e_max is >= the operating point).
interface ArmPoint {
  eL2: number;
  deltaBb: number;
}

function interpolateArmAt(arows: G3Row[], eMax: number): ArmPoint {
  const p = arows.map((r) => [r.eMax, r.eL2, r.deltaHybridBb] as const)
    .sort((a, b) => a[0] - b[0]);
  if (p.length < 2)
    throw new GateInputError('need >= 2 points to interpolate the envelope');
  const first = p[0]!;
  const last = p[p.length - 1]!;
  const at = (row: readonly [number, number, number]): ArmPoint =>
    ({eL2: row[1], deltaBb: row[2]});
  if (eMax < first[0])
    return at(first);
  for (let k = 1; k < p.length; ++k) {
    const hi = p[k]!;
    if (eMax <= hi[0]) {
      const lo = p[k - 1]!;
      const f = hi[0] === lo[0] ? 0 : (eMax - lo[0]) / (hi[0] - lo[0]);
      return {
        eL2: lo[1] + f * (hi[1] - lo[1]),
        deltaBb: lo[2] + f * (hi[2] - lo[2]),
      };
    }
  }
  throw new GateInputError(
    `operating e_max ${eMax.toExponential(3)} exceeds the largest injected `
    + `level ${last[0].toExponential(3)}; add a bracketing level`);
}

function oneSidedUcb(values: number[]): {mean: number; sd: number; ucb: number} {
  const n = values.length;
  const mean = values.reduce((a, b) => a + b, 0) / n;
  const sd = Math.sqrt(
    values.reduce((a, b) => a + (b - mean) ** 2, 0) / (n - 1));
  return {mean, sd, ucb: mean + T_CRIT_0_95_5 * sd / Math.sqrt(n)};
}

// The arm that carries the REAL signed per-combo G2 drift shape; the joint
// (e_max,e_l2) certificate is read along this ray. The synthetic arms (zm,
// coh, pat) are single-norm / correlation stress patterns reported alongside.
export const G3_REAL_ARM = 'act';

export interface G3StratumResult {
  stratum: string;
  identityGapBb: number;
  offlineRiverTermBb: number;  // loose campaign leaf residual, NOT production
  perArmDelta: Record<string, number>;  // interpolated delta at eMaxFrozen
  // Joint certificate along the real-drift ray at E: its e_l2 and propagated
  // delta, plus the conservative single-norm coherent-arm delta.
  jointEL2AtE: number;
  jointDeltaBb: number;
  coherentDeltaBb: number;
}

export interface S2G3Result {
  g3: boolean;
  failures: string[];
  eMaxFrozenPotFraction: number;
  eL2FrozenPotFraction: number;
  rows: number;
  perStratum: G3StratumResult[];
  // One-sided 95% UCB across the six strata (t, df=5) of the propagated TURN
  // delta along the real signed-drift ray - the gated joint certificate.
  jointDeltaMeanBb: number;
  jointDeltaUcbBb: number;
  // True when the real ray's e_l2 at E is >= the shipped L on every stratum,
  // i.e. the certified point is no less correlated than any mounted node.
  rayDominatesProduction: boolean;
  // Recorded (NOT gated) single-norm e_max-only coherent worst case: shows why
  // a max-only bound is insufficient and why the RFC's e_l2 is load-bearing.
  coherentMeanBb: number;
  coherentUcbBb: number;
  // Room left once the R0 = 0.002 online river budget is reserved; the R0
  // term itself is certified separately in S3/G5 (this campaign uses loose
  // river solves, so its t_river is a diagnostic, not the production R0).
  projectedHeadroomBb: number;
}

export function evaluateS2G3(g3Csv: string, certCsv: string): S2G3Result {
  const rows = parseG3Csv(g3Csv);
  const certRows = parseCertCsv(certCsv);
  const failures: string[] = [];
  const eMaxFrozen = operatingEMaxPotFraction(certRows);
  const eL2Frozen = operatingEL2PotFraction(certRows);

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
  const jointDeltas: number[] = [];
  const coherentDeltas: number[] = [];
  let rayDominates = true;

  for (const s of STRATA) {
    const srows = byStratum.get(s);
    if (!srows)
      continue;
    const ident = srows.filter((r) => r.arm === 'identity');
    if (ident.length !== 1)
      failures.push(`${s}: expected exactly one identity row, got ${ident.length}`);
    const identityGapBb = ident[0]?.turnGapBb ?? NaN;
    const offlineRiverTermBb = ident[0]?.riverTermBb ?? NaN;

    const perArmDelta: Record<string, number> = {};
    let jointEL2 = NaN;
    let jointDelta = NaN;
    let coherentDelta = NaN;
    for (const arm of G3_ARMS) {
      const arows = srows.filter((r) => r.arm === arm);
      if (arows.length < 2) {
        failures.push(`${s}: arm ${arm} needs >= 2 levels`);
        continue;
      }
      for (const r of arows)
        if (!(r.eMax >= 0) || !Number.isFinite(r.deltaHybridBb)
            || !Number.isFinite(r.eL2))
          failures.push(`${s}:${arm}: non-finite/negative norm or delta`);
      let pt: ArmPoint;
      try {
        pt = interpolateArmAt(arows, eMaxFrozen);
      } catch (err) {
        failures.push(`${s}:${arm}: ${(err as Error).message}`);
        continue;
      }
      perArmDelta[arm] = pt.deltaBb;
      if (arm === G3_REAL_ARM) {
        jointEL2 = pt.eL2;
        jointDelta = pt.deltaBb;
      }
      if (arm === 'coh')
        coherentDelta = pt.deltaBb;
    }
    // INFORMATIONAL only. The reviewer showed exploitability is not monotone
    // in e_l2, so a ray-dominance rule cannot bound the norm box; the spike
    // box-envelope gate (evaluateS2G3Spike, reach-weighted L*) is authoritative.
    if (!(jointEL2 >= eL2Frozen))
      rayDominates = false;
    perStratum.push({
      stratum: s, identityGapBb, offlineRiverTermBb, perArmDelta,
      jointEL2AtE: jointEL2, jointDeltaBb: jointDelta,
      coherentDeltaBb: coherentDelta,
    });
    if (Number.isFinite(jointDelta))
      jointDeltas.push(jointDelta);
    if (Number.isFinite(coherentDelta))
      coherentDeltas.push(coherentDelta);
  }

  // One-sided 95% UCB of the propagated TURN delta. The zero-perturbation
  // telescoping identity (walker gap == solver NashConv to machine epsilon)
  // is enforced by the native rfc0047_turn_solver CTest, not re-derived here.
  let jointMean = NaN, jointUcb = NaN, cohMean = NaN, cohUcb = NaN;
  let headroom = NaN;
  if (jointDeltas.length === STRATA.length) {
    const j = oneSidedUcb(jointDeltas);
    jointMean = j.mean;
    jointUcb = j.ucb;
    headroom = EPS_HYBRID_B0 - EPS_RIVER_R0 - jointUcb;
    if (!(jointUcb <= EPS_HYBRID_B0 - EPS_RIVER_R0))
      failures.push(
        `joint (e_max,e_l2) propagated-delta UCB ${jointUcb.toExponential(3)} `
        + `> B0-R0 ${(EPS_HYBRID_B0 - EPS_RIVER_R0).toExponential(3)} BB at `
        + `e_max_frozen ${eMaxFrozen.toExponential(3)}`);
  } else {
    failures.push('cannot compute joint UCB: need real-arm delta for all six strata');
  }
  if (coherentDeltas.length === STRATA.length) {
    const c = oneSidedUcb(coherentDeltas);
    cohMean = c.mean;
    cohUcb = c.ucb;
    // Reported, never gated: documents the single-norm conservative bound.
  }

  return {
    g3: failures.length === 0,
    failures,
    eMaxFrozenPotFraction: eMaxFrozen,
    eL2FrozenPotFraction: eL2Frozen,
    rows: rows.length,
    perStratum,
    jointDeltaMeanBb: jointMean,
    jointDeltaUcbBb: jointUcb,
    rayDominatesProduction: rayDominates,
    coherentMeanBb: cohMean,
    coherentUcbBb: cohUcb,
    projectedHeadroomBb: headroom,
  };
}

// --- S2 G3 spike-frontier box envelope (independent-review closure) -------
//
// The act ray alone cannot bound the 2-D norm box. The spike campaign places a
// coherent +E payoff shift on the minimal top-fraction of combos ranked by the
// independent walker's BR-minus-sigma gap weight w (water-filling w.r.t. rho),
// tracing the worst shape at each reach-weighted e_l2. The production e_l2
// coordinate L* is reach-weighted (NOT the unweighted per-group cert RMS),
// measured in the same campaign from the exported rho and signed drift.
export const SPIKE_E_POT_FRACTION = 0.0184;  // amplitude at the operating E

export interface NormsRow {
  stratum: string;
  eMaxReal: number;   // full-census measured per-combo |drift|/pot envelope
  lStarReal: number;  // reach-weighted e_l2 of the real signed drift
  analyticBoxBb: number;  // shape-free first-order box bound at (eMaxReal,L*)
}

export function parseNormsCsv(text: string): NormsRow[] {
  const rows = parseCsv(text);
  const header = rows.shift();
  if (header === undefined)
    throw new GateInputError('norms CSV is empty');
  requireHeader(header, ['stratum', 'emax_real', 'lstar_real', 'analytic_box_bb']);
  return rows.map((cols) => {
    if (cols[0] === undefined || !cols[0])
      throw new GateInputError('norms row has empty stratum');
    const v: Record<string, string> = {};
    ['stratum', 'emax_real', 'lstar_real', 'analytic_box_bb'].forEach((h, i) => {
      v[h] = cols[i] ?? '';
    });
    const eMax = Number(v.emax_real);
    const lStar = Number(v.lstar_real);
    const box = Number(v.analytic_box_bb);
    if (![eMax, lStar, box].every(Number.isFinite))
      throw new GateInputError('norms row non-numeric');
    if (box < 0)
      throw new GateInputError('norms analytic box bound negative');
    return {stratum: v.stratum!, eMaxReal: eMax, lStarReal: lStar,
            analyticBoxBb: box};
  });
}

export interface SpikeStratumResult {
  stratum: string;
  eMaxReal: number;
  lStar: number;
  // Empirical: interpolated/clamped measured spike delta at L*.
  empiricalBoxDeltaBb: number;
  clampedBelowFinest: boolean;
  spikePoints: Array<[number, number]>;
  // Shape-free analytic first-order box bound at this stratum's OWN measured
  // full-census envelope (e_max_real, L*); the gated coordinate.
  analyticEnvBoxBb: number;
}

export interface S2G3SpikeResult {
  g3: boolean;
  failures: string[];
  rows: number;
  perStratum: SpikeStratumResult[];
  // FINDING 4: the non-probed-node envelope is the frozen-frame MAX (not a
  // mean confidence bound). Descriptive t statistics are reported alongside.
  envelopeMaxBb: number;
  meanBb: number;
  sdBb: number;
  meanUcbBb: number;
  barBb: number;
  eEnvPotFraction: number;  // full-census amplitude envelope (F3)
}

export function evaluateS2G3Spike(spikeCsv: string, normsCsv: string):
    S2G3SpikeResult {
  const spikeRows = parseG3Csv(spikeCsv);
  const norms = parseNormsCsv(normsCsv);
  const failures: string[] = [];
  const nByStratum = new Map(norms.map((n) => [n.stratum, n]));
  for (const s of STRATA)
    if (!nByStratum.has(s))
      failures.push(`missing norms row for '${s}'`);

  // F3: the cross-stratum amplitude maximum E_env = max full-census measured
  // per-combo envelope. It is REPORTED only; each stratum is gated at its own
  // e_max_real (re-applying E_env to a low-emax stratum would assume an error
  // amplitude its own measured envelope rules out).
  const eEnv = norms.reduce((m, n) => Math.max(m, n.eMaxReal), 0);
  if (!(eEnv > 0))
    failures.push('non-positive full-census e_max envelope');

  const byStratum = new Map<string, G3Row[]>();
  for (const r of spikeRows) {
    const a = byStratum.get(r.stratum) ?? [];
    a.push(r);
    byStratum.set(r.stratum, a);
  }
  for (const s of STRATA)
    if (!byStratum.has(s))
      failures.push(`missing spike stratum '${s}'`);

  const perStratum: SpikeStratumResult[] = [];
  const analyticEnv: number[] = [];

  for (const s of STRATA) {
    const srows = byStratum.get(s);
    const nr = nByStratum.get(s);
    if (!srows || !nr)
      continue;
    const lStar = nr.lStarReal;
    // Empirical spike points at amplitude SPIKE_E, ascending in measured
    // reach-weighted e_l2.
    const pts = srows
      .filter((r) => r.arm.startsWith('spike')
              && Math.abs(r.eMax - SPIKE_E_POT_FRACTION) < 1e-9)
      .map((r) => [r.eL2, r.deltaHybridBb] as [number, number])
      .sort((a, b) => a[0] - b[0]);
    if (pts.length < 2) {
      failures.push(`${s}: need >= 2 spike points at E`);
      continue;
    }
    for (const [x, y] of pts)
      if (!(x >= 0) || !Number.isFinite(y))
        failures.push(`${s}: non-finite spike point`);
    // Reviewer condition (iii): refuse to chord a non-monotone frontier.
    for (let k = 1; k < pts.length; ++k)
      if (pts[k]![1] + 1e-12 < pts[k - 1]![1])
        failures.push(`${s}: spike delta not monotone in e_l2 at index ${k}`);

    const finest = pts[0]!;
    const coarsest = pts[pts.length - 1]!;
    let empirical: number;
    let clamped = false;
    if (lStar <= finest[0]) {
      empirical = finest[1];  // most concentrated measured point bounds it
      clamped = true;
    } else if (lStar > coarsest[0]) {
      failures.push(`${s}: L* ${lStar.toExponential(3)} above coarsest spike `
        + `e_l2 ${coarsest[0].toExponential(3)}; add a broader q`);
      continue;
    } else {
      empirical = NaN;
      for (let k = 1; k < pts.length; ++k) {
        const hi = pts[k]!;
        if (lStar <= hi[0]) {
          const lo = pts[k - 1]!;
          const f = hi[0] === lo[0] ? 0 : (lStar - lo[0]) / (hi[0] - lo[0]);
          empirical = lo[1] + f * (hi[1] - lo[1]);
          break;
        }
      }
    }

    // Shape-free first-order box bound at THIS stratum's own measured
    // full-census envelope (e_max_real, L*). A mounted node built from these
    // leaves has per-combo error <= the stratum emax_real and reach-weighted
    // e_l2 <= L*, so the own-envelope bound is a valid (conservative) node
    // certificate; re-applying the GLOBAL E_env to a low-emax stratum would
    // assume an error amplitude its measured envelope rules out. E_env is
    // reported separately as the cross-stratum amplitude maximum (F3).
    const aOwn = nr.analyticBoxBb;
    perStratum.push({
      stratum: s, eMaxReal: nr.eMaxReal, lStar,
      empiricalBoxDeltaBb: empirical!, clampedBelowFinest: clamped,
      spikePoints: pts, analyticEnvBoxBb: aOwn,
    });
    analyticEnv.push(aOwn);
  }

  let envelopeMax = NaN, mean = NaN, sd = NaN, meanUcb = NaN;
  const bar = EPS_HYBRID_B0 - EPS_RIVER_R0;
  if (analyticEnv.length === STRATA.length) {
    envelopeMax = Math.max(...analyticEnv);
    const u = oneSidedUcb(analyticEnv);
    mean = u.mean;
    sd = u.sd;
    meanUcb = u.ucb;
    if (!(envelopeMax <= bar))
      failures.push(`spike box-corner envelope MAX ${envelopeMax.toExponential(3)} `
        + `> B0-R0 ${bar.toExponential(3)} BB`);
  } else {
    failures.push('cannot compute spike envelope: need all six strata');
  }

  return {
    g3: failures.length === 0,
    failures,
    rows: spikeRows.length,
    perStratum,
    envelopeMaxBb: envelopeMax,
    meanBb: mean,
    sdBb: sd,
    meanUcbBb: meanUcb,
    barBb: bar,
    eEnvPotFraction: eEnv,
  };
}

// Manual entrypoint:
//   node dist/tools/rfc/rfc0047-gates.js <infoset.csv> <census.csv> [cert.csv]
//   [g3.csv] [spike.csv] [norms.csv]
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
  const spikePath = process.argv[6];
  const normsPath = process.argv[7];
  const cornerPath = process.argv[8];
  if (!infosetPath || !censusPath) {
    console.error(
      'usage: rfc0047-gates <s1-infoset-budget.csv> <s1-census.csv> '
      + '[s2-cert.csv] [s2-g3.csv] [s2-g3-spike.csv] [s2-g3-norms.csv] '
      + '[s2-g3-corner.csv]');
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
      `G3 over ${g3.rows} rows: operating (e_max,e_l2) = (`
      + `${g3.eMaxFrozenPotFraction.toExponential(3)}, `
      + `${g3.eL2FrozenPotFraction.toExponential(3)}) pot-fraction; JOINT `
      + `real-signed-drift propagated turn delta mean `
      + `${g3.jointDeltaMeanBb.toExponential(3)} UCB `
      + `${g3.jointDeltaUcbBb.toExponential(3)} BB (bar B0-R0 `
      + `${(EPS_HYBRID_B0 - EPS_RIVER_R0).toExponential(3)}; ray dominates `
      + `production e_l2: ${g3.rayDominatesProduction}).`);
    console.log(
      `  single-norm e_max-only coherent bound UCB `
      + `${g3.coherentUcbBb.toExponential(3)} BB (RECORDED, not gated; `
      + `unreachable by a diffuse mixed-sign deterministic residual).`);
    for (const r of g3.perStratum)
      console.log(
        `  ${r.stratum}: joint delta ${r.jointDeltaBb.toExponential(3)} `
        + `(e_l2@E ${r.jointEL2AtE.toExponential(3)}), coherent `
        + `${r.coherentDeltaBb.toExponential(3)} BB`);
    if (!g3.g3) {
      console.error('RFC 0047 S2 G3 act-ray coordinate FAILED:');
      for (const f of g3.failures)
        console.error(`  - ${f}`);
      process.exit(1);
    }
    console.log('RFC 0047 S2 G3 act real-drift ray coordinate PASS '
      + '(PROVISIONAL component; the box envelope is the spike gate below).');

    if (spikePath && normsPath) {
      const sp = evaluateS2G3Spike(readFileSync(spikePath, 'utf8'),
                                   readFileSync(normsPath, 'utf8'));
      console.log(
        `G3 shape-free analytic box (global pooling, both w tails, each stratum `
        + `at its own full-census envelope; cross-stratum amplitude max E_env `
        + `${sp.eEnvPotFraction.toExponential(3)}): frozen-frame MAX `
        + `${sp.envelopeMaxBb.toExponential(3)} BB (bar B0-R0 `
        + `${sp.barBb.toExponential(3)}); mean ${sp.meanBb.toExponential(3)}.`);
      for (const r of sp.perStratum)
        console.log(
          `  ${r.stratum}: analytic(own envelope) `
          + `${r.analyticEnvBoxBb.toExponential(3)} `
          + `BB (own e_max ${r.eMaxReal.toExponential(3)}, L* `
          + `${r.lStar.toExponential(3)}); measured-spike corner `
          + `${r.empiricalBoxDeltaBb.toExponential(3)}`
          + (r.clampedBelowFinest ? ' [finest-spike clamp]' : ''));
      if (!sp.g3) {
        console.error('RFC 0047 S2 G3 spike box-envelope gate FAILED:');
        for (const f of sp.failures)
          console.error(`  - ${f}`);
        process.exit(1);
      }
      console.log('RFC 0047 S2 G3 spike box-envelope gate PASS '
        + '(non-probed mounted-node bound).');
      if (cornerPath) {
        const cn = evaluateG3Corner(readFileSync(cornerPath, 'utf8'));
        console.log(
          `G3 exact box-corner convexity check over ${cn.rows} rows: measured `
          + `<= analytic bound on every stratum (max ratio `
          + `${Math.max(...cn.perStratum.map((p) => p.ratio)).toFixed(3)}).`);
        if (!cn.ok) {
          console.error('RFC 0047 S2 G3 exact-corner check FAILED:');
          for (const f of cn.failures)
            console.error(`  - ${f}`);
          process.exit(1);
        }
        console.log('RFC 0047 S2 G3 exact-corner check PASS.');
      }
    } else {
      console.log('G3 spike box envelope not evaluated '
        + '(provide spike.csv and norms.csv); non-probed nodes not certified.');
    }
  } catch (err) {
    console.error(`RFC 0047 gate input error: ${(err as Error).message}`);
    process.exit(2);
  }
}
