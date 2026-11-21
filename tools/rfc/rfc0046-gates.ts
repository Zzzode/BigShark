// RFC 0046 Stage-0 pre-frozen gate evaluator (measurement-only).
//
// Frozen sources of truth:
//   docs/plans/0046-addendum-stage0-gates.md  (strata, seeds, envelopes,
//                                             Gates 1-3, section-10 table)
//   docs/plans/0046-stage1-harness.md section 5 (additive partition, label
//                                             rule, variant checks) and
//                                             section 7 (CSV + predicates)
//
// The evaluator emits exactly one typed verdict PASS | REDIRECT | CLOSE.
// Unknown, missing, or inconsistent input aborts with a non-zero exit and
// a machine-readable error instead of producing a verdict. Every
// predicate below is mechanical and pre-frozen; there is no operator
// discretion in the evaluation path.
import {readFileSync} from 'node:fs';

// --- Frozen constants (addendum sections 2,3,4,5,7) ------------------------

export const T_CRIT_FULL = 4.303;   // df = n-1 = 2, full-range n=3
export const T_CRIT_W1 = 2.7764;    // df = n-1 = 4, W1 n=5
export const SEEDS_FULL: readonly number[] = [4320, 4321, 4322];
export const SEEDS_W1: readonly number[] = [4320, 4321, 4322, 4323, 4324];
// Gate-2 timing always samples R=10 fresh cold resolves per seed on the
// three shared timing seeds (addendum section 3: 3 seeds x 10 = 30).
export const TIMING_SEEDS: readonly number[] = [4320, 4321, 4322];
export const RESOLVES_PER_SEED = 10;
export const N_TIMING = 30;
// Hyndman-Fan type 1 (nearest-rank / inverse-ECDF), 1-based ordered ranks.
export const P50_RANK = 15;
export const P95_RANK = 29;
export const MAX_RANK = 30;

export const B0 = 0.010; // BB, six-stratum-max UCB of total hybrid NashConv
export const R0 = 0.002; // BB, six-stratum-max UCB of T_river
export const M_GAP = 0.002; // BB/root-pot, Gate-3 cap-gap UCB

// Production ResolveLimits envelope (addendum section 4).
export const MAX_NODES = 5_000_000;
export const MAX_INFOSETS = 1_000_000;
export const MAX_BYTES = 1_073_741_824; // 1 GiB
export const P95_WALL_S = 10.0;         // 10,000 ms
export const MAX_WALL_S = 12.0;         // 12,000 ms

// One-shot diagnostic profile bounds (addendum section 3).
export const DIAG_MAX_NODES = 10_000_000;
export const DIAG_MAX_INFOSETS = 2_000_000;
export const DIAG_MAX_BYTES = 2_147_483_648; // 2 GiB
export const DIAG_MAX_WALL_S = 30.0;         // 30,000 ms

// Measurement tolerances (harness sections 4,5).
export const CERT_SLACK_TOL = 1e-6;  // root-pot, certification slack
export const MARGIN_DIFF_TOL = 1e-6; // root-pot, responder margin determinism
export const IDENTITY_TOL = 1e-9;    // root-pot, telescoping identity
export const DIRECTIONAL_TOL = 1e-9; // root-pot, variant directional checks
export const TERM_FLOOR = -1e-6;     // root-pot, no term below this
export const STRUCTURAL_ZERO_TOL = 1e-12; // exact pinned-zero columns
export const GAP_ANOMALY = -1e-6;    // root-pot, Gate-3 LCB sign anomaly

export const CAP0REF_OUTER_NODES = 20_000_000; // 8 GiB / 600 s outer budget
export const CAP0REF_OUTER_BYTES = 8_589_934_592; // 8 GiB
export const CAP0REF_OUTER_S = 600.0;

export const HANDSHAKE_ITERATIONS = 2_000_000;
export const HANDSHAKE_SEED = 4320;
export const DEFAULT_HANDPAIR_MANIFEST =
  'docs/plans/ablation/rfc0046/rfc0046-handpair.json';

// --- Frozen strata (addendum section 1) -------------------------------------

export type RangeName = 'full' | 'w1';
export type SchemeName = 'T0' | 'T1' | 'T2t' | 'T2h';
export type PolicyName =
  | 'hybrid'
  | 'rivermerge'
  | 'bucketmargin'
  | 'statictrunk'
  | 'cap0ref';

export const CANDIDATES: readonly SchemeName[] = ['T0', 'T1', 'T2t', 'T2h'];
export const HINT_SCHEMES: readonly SchemeName[] = ['T2t', 'T2h'];
export const VARIANT_POLICIES: readonly PolicyName[] = [
  'rivermerge', 'bucketmargin', 'statictrunk',
];

export interface StratumSpec {
  name: string;
  root: string;
  turnBoard: string;
  riverCard: string;
  range: RangeName;
  seeds: readonly number[]; // UCB seeds
  timingSeeds: readonly number[];
  tCrit: number;
  // Frozen root identity (addendum section 1): BB and pot at the turn root.
  // Used to convert between BB-valued columns (total_nc, terms, Gate-3 gap)
  // and the root-pot units the identity/floor/anomaly thresholds are frozen
  // in (finding M2).
  bigBlind: number;
  pot: number;
}

// Convert a BB-per-hand quantity to a root-pot fraction for one stratum.
export function bbToPotFraction(spec: StratumSpec, bbValue: number): number {
  return bbValue * (spec.bigBlind / spec.pot);
}

export const STRATA: readonly StratumSpec[] = [
  {
    name: 'w1', root: 'limped', turnBoard: '2c3d7hJs', riverCard: '9c',
    range: 'w1', seeds: SEEDS_W1, timingSeeds: TIMING_SEEDS, tCrit: T_CRIT_W1,
    bigBlind: 2, pot: 4,
  },
  {
    name: 'unpaired-rainbow-ahigh', root: 'limped',
    turnBoard: '7sKhAdJs', riverCard: '9c',
    range: 'full', seeds: SEEDS_FULL, timingSeeds: TIMING_SEEDS,
    tCrit: T_CRIT_FULL,
    bigBlind: 2, pot: 4,
  },
  {
    name: 'paired', root: 'limped', turnBoard: '7sKhKdJs', riverCard: '9c',
    range: 'full', seeds: SEEDS_FULL, timingSeeds: TIMING_SEEDS,
    tCrit: T_CRIT_FULL,
    bigBlind: 2, pot: 4,
  },
  {
    name: 'monotone', root: 'limped', turnBoard: 'QsKsAsJs', riverCard: '9c',
    range: 'full', seeds: SEEDS_FULL, timingSeeds: TIMING_SEEDS,
    tCrit: T_CRIT_FULL,
    bigBlind: 2, pot: 4,
  },
  {
    name: 'unpaired-twotone', root: 'limped',
    turnBoard: '7sKhAsJs', riverCard: '9c',
    range: 'full', seeds: SEEDS_FULL, timingSeeds: TIMING_SEEDS,
    tCrit: T_CRIT_FULL,
    bigBlind: 2, pot: 4,
  },
  {
    name: 'raised-srp', root: 'single-raised',
    turnBoard: '2c3d7hJs', riverCard: '2s',
    range: 'full', seeds: SEEDS_FULL, timingSeeds: TIMING_SEEDS,
    tCrit: T_CRIT_FULL,
    bigBlind: 2, pot: 8,
  },
];

export const STRATUM_BY_NAME: ReadonlyMap<string, StratumSpec> = new Map(
  STRATA.map((s) => [s.name, s]),
);
export const MONOTONE_STRATUM = 'monotone';
export const FALLBACK_STRATUM = 'unpaired-twotone';

export const TERMINATIONS: readonly string[] = [
  'Certified',
  'CertificationRejected',
  'SolveDeadline',
  'CertifyDeadline',
  'ResourceExhausted',
  'DiagnosticInconclusive',
];
export const FAILURE_TERMINATIONS: readonly string[] = [
  'CertificationRejected',
  'SolveDeadline',
  'CertifyDeadline',
  'ResourceExhausted',
];
export const DEADLINE_OR_RESOURCE: readonly string[] = [
  'SolveDeadline',
  'CertifyDeadline',
  'ResourceExhausted',
];
export const NOTES: readonly string[] = ['final', 'truncated', 'diagnostic'];

// --- CSV schema (harness section 7; order is frozen) ------------------------

export const GATE_COLUMNS = [
  'stratum', 'root', 'turn_board', 'river_card', 'range', 'scheme', 'trunk',
  'cap', 'seed', 'resolve_id', 'policy',
  'trunk_rows', 'trunk_bytes',
  'infosets', 'nodes', 'peak_bytes',
  'train_s', 'certify_s', 'resolve_wall_s', 'walk_s',
  'termination',
  'coldcold_bitwise', 'warmhint_bitwise', 'max_margin_diff', 'max_cert_slack',
  'total_nc', 't_trunk', 't_boundary', 't_river', 't_action',
  'extension_mass', 'telescope_resid',
  'nc_endgame_cap1', 'nc_endgame_cap0',
  'river_argmax_diff', 'note',
] as const;

export type GateTermination =
  | 'Certified'
  | 'CertificationRejected'
  | 'SolveDeadline'
  | 'CertifyDeadline'
  | 'ResourceExhausted'
  | 'DiagnosticInconclusive';

export interface GateRow {
  stratum: string;
  root: string;
  turnBoard: string;
  riverCard: string;
  range: RangeName;
  scheme: SchemeName;
  trunk: string;
  cap: number;
  seed: number;
  resolveId: string;
  policy: PolicyName;
  trunkRows: number;
  trunkBytes: number;
  infosets: number;
  nodes: number;
  peakBytes: number;
  trainS: number;
  certifyS: number;
  resolveWallS: number;
  walkS: number;
  termination: GateTermination;
  coldcoldBitwise: boolean | null;
  warmhintBitwise: boolean | null;
  maxMarginDiff: number | null;
  maxCertSlack: number | null;
  totalNc: number | null;
  tTrunk: number | null;
  tBoundary: number | null;
  tRiver: number | null;
  tAction: number | null;
  extensionMass: number | null;
  telescopeResid: number | null;
  ncEndgameCap1: number | null;
  ncEndgameCap0: number | null;
  riverArgmaxDiff: boolean | null;
  note: string;
}

export class GateInputError extends Error {
  code: string;
  constructor(code: string, message: string) {
    super(message);
    this.name = 'GateInputError';
    this.code = code;
  }
}

function parseBool(raw: string, context: string): boolean | null {
  if (raw === '')
    return null;
  if (raw === 'true' || raw === '1')
    return true;
  if (raw === 'false' || raw === '0')
    return false;
 throw new GateInputError('bad-boolean', `${context}: invalid boolean '${raw}'`);
}

function parseNum(raw: string, context: string): number {
  const v = Number(raw);
  if (!Number.isFinite(v))
    throw new GateInputError('non-numeric', `${context}: non-numeric '${raw}'`);
  return v;
}

function parseNumOrNull(raw: string): number | null {
  if (raw === '')
    return null;
  const v = Number(raw);
  return Number.isFinite(v) ? v : null;
}

function parseNonNegInt(raw: string, context: string): number {
  const v = parseNum(raw, context);
  if (!Number.isInteger(v) || v < 0)
    throw new GateInputError('bad-integer', `${context}: expected non-negative integer, got '${raw}'`);
  return v;
}

export function loadGateCsv(path: string): GateRow[] {
  const raw = readFileSync(path, 'utf8');
  const lines = raw
    .split('\n')
    .map((l) => l.trim())
    .filter((l) => l.length > 0 && !l.startsWith('#'));
  if (lines.length === 0)
    throw new GateInputError('empty-input', `${path}: empty CSV`);
  const header = lines[0]!.split(',').map((h) => h.trim());
  if (header.length !== GATE_COLUMNS.length
      || header.some((h, i) => h !== GATE_COLUMNS[i])) {
    throw new GateInputError(
      'header-mismatch',
      `${path}: header must be exactly, in order:\n${GATE_COLUMNS.join(',')}`,
    );
  }
  const out: GateRow[] = [];
  for (let li = 1; li < lines.length; ++li) {
    const f = lines[li]!.split(',');
    if (f.length !== GATE_COLUMNS.length) {
      throw new GateInputError(
        'bad-field-count',
        `${path}:${li + 1}: expected ${GATE_COLUMNS.length} fields, got ${f.length}`,
      );
    }
    out.push(parseGateRow(f, `${path}:${li + 1}`));
  }
  return out;
}

export function loadGateCsvs(paths: string[]): GateRow[] {
  return paths.flatMap((p) => loadGateCsv(p));
}

function at(f: readonly string[], i: number): string {
  return f[i] ?? '';
}

function asEnum<T extends string>(
  raw: string, allowed: readonly T[], context: string,
): T {
  if ((allowed as readonly string[]).includes(raw))
    return raw as T;
  throw new GateInputError(
    'bad-enum', `${context}: '${raw}' not in {${allowed.join(',')}}`);
}

function parseGateRow(f: readonly string[], ctx: string): GateRow {
  const s = at(f, 0);
  const spec = STRATUM_BY_NAME.get(s);
  if (!spec)
    throw new GateInputError('unknown-stratum', `${ctx}: unknown stratum '${s}'`);
  const range = asEnum(at(f, 4), ['full', 'w1'], `${ctx} range`);
  const scheme = asEnum(at(f, 5), CANDIDATES, `${ctx} scheme`);
  const policy = asEnum(
    at(f, 10),
    ['hybrid', 'rivermerge', 'bucketmargin', 'statictrunk', 'cap0ref'],
    `${ctx} policy`,
  );
  const termination = asEnum(
    at(f, 20), TERMINATIONS, `${ctx} termination`,
  ) as GateTermination;
  const note = asEnum(at(f, 35), NOTES, `${ctx} note`);
  const trunk = at(f, 6);
  if (trunk === '')
    throw new GateInputError('missing-field', `${ctx}: empty trunk`);
  const resolveId = at(f, 9);
  if (resolveId === '')
    throw new GateInputError('missing-field', `${ctx}: empty resolve_id`);
  const alwaysNum = (i: number, name: string): number =>
    parseNum(at(f, i), `${ctx} ${name}`);
  return {
    stratum: s,
    root: at(f, 1),
    turnBoard: at(f, 2),
    riverCard: at(f, 3),
    range,
    scheme,
    trunk,
    cap: parseNonNegInt(at(f, 7), `${ctx} cap`),
    seed: parseNum(at(f, 8), `${ctx} seed`),
    resolveId,
    policy,
    trunkRows: parseNonNegInt(at(f, 11), `${ctx} trunk_rows`),
    trunkBytes: parseNonNegInt(at(f, 12), `${ctx} trunk_bytes`),
    infosets: alwaysNum(13, 'infosets'),
    nodes: alwaysNum(14, 'nodes'),
    peakBytes: alwaysNum(15, 'peak_bytes'),
    trainS: alwaysNum(16, 'train_s'),
    certifyS: alwaysNum(17, 'certify_s'),
    resolveWallS: alwaysNum(18, 'resolve_wall_s'),
    walkS: alwaysNum(19, 'walk_s'),
    termination,
    coldcoldBitwise: parseBool(at(f, 21), `${ctx} coldcold_bitwise`),
    warmhintBitwise: parseBool(at(f, 22), `${ctx} warmhint_bitwise`),
    maxMarginDiff: parseNumOrNull(at(f, 23)),
    maxCertSlack: parseNumOrNull(at(f, 24)),
    totalNc: parseNumOrNull(at(f, 25)),
    tTrunk: parseNumOrNull(at(f, 26)),
    tBoundary: parseNumOrNull(at(f, 27)),
    tRiver: parseNumOrNull(at(f, 28)),
    tAction: parseNumOrNull(at(f, 29)),
    extensionMass: parseNumOrNull(at(f, 30)),
    telescopeResid: parseNumOrNull(at(f, 31)),
    ncEndgameCap1: parseNumOrNull(at(f, 32)),
    ncEndgameCap0: parseNumOrNull(at(f, 33)),
    riverArgmaxDiff: parseBool(at(f, 34), `${ctx} river_argmax_diff`),
    note,
  };
}

// --- Statistics --------------------------------------------------------------

export function mean(xs: readonly number[]): number {
  if (xs.length === 0)
    throw new GateInputError('empty-sample', 'mean: no samples');
  return xs.reduce((a, b) => a + b, 0) / xs.length;
}

// Sample standard deviation with n-1 denominator; zero/one sample -> 0.
export function sampleSdN1(xs: readonly number[]): number {
  if (xs.length < 2)
    return 0;
  const m = mean(xs);
  return Math.sqrt(xs.reduce((a, x) => a + (x - m) ** 2, 0) / (xs.length - 1));
}

export interface UcbStats {
  n: number
  mean: number;
  sd: number;
  halfWidth: number;
  ucb: number;
  lcb: number;
}

export function ucbStats(xs: readonly number[], tCrit: number): UcbStats {
  const n = xs.length;
  const m = mean(xs);
  const sd = sampleSdN1(xs);
  const hw = (tCrit * sd) / Math.sqrt(n);
  return {n, mean: m, sd, halfWidth: hw, ucb: m + hw, lcb: m - hw};
}

// Hyndman-Fan type 1 nearest-rank quantile on ASCENDING-sorted samples:
// rank = ceil(p * n), 1-based.
export function nearestRank(sortedAsc: readonly number[], rank: number): number {
  if (rank < 1 || rank > sortedAsc.length)
    throw new GateInputError('bad-rank', `rank ${rank} outside 1..${sortedAsc.length}`);
  return sortedAsc[rank - 1]!;
}

export function hfQuantile(samples: readonly number[], p: number): number {
  const sorted = [...samples].sort((a, b) => a - b);
  const rank = Math.ceil(p * sorted.length);
  return nearestRank(sorted, rank);
}

// --- Per-infoset telescoping partition (harness section 5.3) ----------------

export type TermLabel = 'T_trunk' | 'T_boundary' | 'T_river' | 'T_action';
export type EndgameStreet = 'turn' | 'river';

export interface ActionValue {
  value: number;
  insideCap1: boolean;
  scheduleIndex: number; // frozen identity-schedule index (lower wins ties)
}

// Deterministic BR argmax BEFORE labeling (harness 5.3 rule 2): among the
// maximum-value actions prefer one inside the cap-1 menu; among still-tied
// actions take the lowest frozen identity-schedule action index.
export function chooseBrAction(actions: readonly ActionValue[]): ActionValue {
  if (actions.length === 0)
    throw new GateInputError('empty-actions', 'chooseBrAction: no actions');
  let best = -Infinity;
  for (const a of actions)
    if (a.value > best)
      best = a.value;
  const tied = actions.filter((a) => a.value === best);
  const inside = tied.filter((a) => a.insideCap1);
  const pool = inside.length > 0 ? inside : tied;
  return pool.reduce((acc, a) => (a.scheduleIndex < acc.scheduleIndex ? a : acc));
}

export interface StreamNode {
  street: EndgameStreet;
  reachH: number;     // full H-reach of the infoset
  g: number;          // telescoping regret increment (>= 0)
  extensionOnly: boolean; // rule 1: reachable only via cap-0-only extension
  brInsideCap1: boolean;  // post-tie BR argmax lies inside the cap-1 menu
}

// Total-ordered label rule, evaluated top to bottom (harness 5.3).
export function labelOf(n: Pick<StreamNode, 'street' | 'extensionOnly' | 'brInsideCap1'>):
    TermLabel {
  if (n.extensionOnly)
    return 'T_action'; // rule 1: cap-0-only extension region
  if (!n.brInsideCap1)
    return 'T_action'; // rule 2: missing-menu regret
  // rule 3: the measurement tree is turn-rooted, so no T_trunk node exists.
  return n.street === 'turn' ? 'T_boundary' : 'T_river';
}

// T_trunk is structurally pinned to the exact constant zero (harness 5.2).
export const TRUNK_TERM: 0 = 0;

export interface Partition {
  total: number;
  extensionMass: number;
  terms: Record<TermLabel, number>;
  // |Total - (T_trunk+T_boundary+T_river+T_action)|; extension g is pinned
  // zero, so a nonzero extension mass shows up here as well.
  residual: number;
}

export function partitionStream(nodes: readonly StreamNode[]): Partition {
  const terms: Record<TermLabel, number> = {
    T_trunk: TRUNK_TERM, T_boundary: 0, T_river: 0, T_action: 0,
  };
  let total = 0;
  let extensionMass = 0;
  for (const n of nodes) {
    total += n.g;
    const label = labelOf(n);
    if (n.extensionOnly)
      extensionMass += n.g;
    else
      terms[label] += n.g;
  }
  const summed = terms.T_trunk + terms.T_boundary + terms.T_river
    + terms.T_action;
  return {total, extensionMass, terms, residual: Math.abs(total - summed)};
}

// --- Hand-pair manifest (harness section 6) ---------------------------------

export interface HandPairManifest {
  stratum: string;
  board: string;
  river: string;
  seed: number;
  iterations: number;
  referenceDigest: string;
  combos: [string, string];
  categoryId: string;
  actionIndices: [number, number];
  valueGap: number;
  fallbackUsed: boolean;
}

export function loadHandPairManifest(path: string): HandPairManifest {
  let json: Record<string, unknown>;
  try {
    json = JSON.parse(readFileSync(path, 'utf8')) as Record<string, unknown>;
  } catch (e) {
    throw new GateInputError(
      'handpair-unreadable', `${path}: ${(e as Error).message}`);
  }
  const ctx = `handpair ${path}`;
  const str = (k: string): string => {
    const v = json[k];
    if (typeof v !== 'string' || v === '')
      throw new GateInputError('handpair-field', `${ctx}: missing string ${k}`);
    return v;
  };
  const num = (k: string): number => {
    const v = json[k];
    if (typeof v !== 'number' || !Number.isFinite(v))
      throw new GateInputError('handpair-field', `${ctx}: missing number ${k}`);
    return v;
  };
  const bool = (k: string): boolean => {
    const v = json[k];
    if (typeof v !== 'boolean')
      throw new GateInputError('handpair-field', `${ctx}: missing boolean ${k}`);
    return v;
  };
  const arr = (k: string, len: number): unknown[] => {
    const v = json[k];
    if (!Array.isArray(v) || v.length !== len)
      throw new GateInputError('handpair-field', `${ctx}: ${k} must be ${len} elements`);
    return v;
  };
  const board = str('board');
  const river = str('river');
  const seed = num('seed');
  const iterations = num('iterations');
  const referenceDigest = str('reference_digest');
  const combosRaw = arr('combos', 2);
  if (!combosRaw.every((c) => typeof c === 'string' && c !== ''))
    throw new GateInputError('handpair-field', `${ctx}: combos must be two nonempty strings`);
  const actionsRaw = arr('action_indices', 2);
  if (!actionsRaw.every((a) => Number.isInteger(a)))
    throw new GateInputError('handpair-field', `${ctx}: action_indices must be two integers`);
  const valueGap = num('value_gap');
  const fallbackUsed = bool('fallback_used');
  if (seed !== HANDSHAKE_SEED)
    throw new GateInputError('handpair-constants', `${ctx}: seed must be ${HANDSHAKE_SEED}`);
  if (iterations !== HANDSHAKE_ITERATIONS)
    throw new GateInputError('handpair-constants',
      `${ctx}: iterations must be ${HANDSHAKE_ITERATIONS}`);
  if (valueGap <= 0)
    throw new GateInputError('handpair-constants', `${ctx}: value_gap must be strictly positive`);
  if (actionsRaw[0] === actionsRaw[1])
    throw new GateInputError('handpair-constants', `${ctx}: action indices must differ`);
  if (combosRaw[0] === combosRaw[1])
    throw new GateInputError('handpair-constants', `${ctx}: combos must differ`);
  let stratum: string;
  if (!fallbackUsed) {
    if (board !== 'QsKsAsJs' || river !== '9c')
      throw new GateInputError('handpair-constants',
        `${ctx}: non-fallback manifest must pin QsKsAsJs / 9c`);
    stratum = MONOTONE_STRATUM;
  } else {
    if (board !== '7sKhAsJs' || river !== '9c')
      throw new GateInputError('handpair-constants',
        `${ctx}: fallback manifest must pin 7sKhAsJs / 9c`);
    stratum = FALLBACK_STRATUM;
  }
  return {
    stratum, board, river, seed, iterations, referenceDigest,
    combos: combosRaw as [string, string],
    categoryId: str('category_id'),
    actionIndices: actionsRaw as [number, number],
    valueGap, fallbackUsed,
  };
}

// --- Evaluation --------------------------------------------------------------

export interface TimingStats {
  n: number;
  p50S: number;
  p95S: number;
  maxS: number;
}

export type Gate2Classification = 'pass' | 'budget-tight' | 'bound-unreachable';

export interface StratumGate2 {
  scheme: SchemeName;
  cap: number;
  stratum: string;
  range: RangeName;
  certifiedResolves: number;
  timingSamples: number;
  p50S: number | null;
  p95S: number | null;
  maxS: number | null;
  envelopePass: boolean;
  classification: Gate2Classification;
  failures: string[];
}

export interface CandidateGate1 {
  scheme: SchemeName;
  trunk: string;
  trunkRows: number;
  trunkBytes: number;
  worstP95S: number;
  perStratum: Array<{
    stratum: string;
    n: number;
    total: UcbStats;
    river: UcbStats;
  }>;
  sixMaxUcbTotal: number;
  sixMaxUcbRiver: number;
  eligible: boolean;
}

export interface Gate3Stratum {
  stratum: string;
  n: number;
  nc0Mean: number;
  nc1Mean: number;
  gap: UcbStats;
}

export interface Gate3Result {
  candidate: SchemeName;
  cap: 1 | 2;
  perStratum: Gate3Stratum[];
  anomalyStrata: string[];
  failingAtCap1: string[];
  escalation: {
    attempted: boolean;
    cap2: Gate3Stratum[];
    passed: boolean;
    failingStrata: string[];
  } | null;
  passes: boolean;
  closedReason: string | null;
}

export interface GateVerdict {
  verdict: 'PASS' | 'REDIRECT' | 'CLOSE';
  handpair: {
    stratum: string;
    board: string;
    river: string;
    fallbackUsed: boolean;
  };
  gate2: {
    status: 'PASS' | 'REDIRECT' | 'CLOSE';
    strata: StratumGate2[];
  };
  gate1: {
    candidates: CandidateGate1[];
    eligible: SchemeName[];
    selected: SchemeName | null;
  };
  gate3: Gate3Result | null;
}

interface Indexed {
  rows: GateRow[];
  byKey: Map<string, GateRow>;
}

function rowKey(r: GateRow): string {
  return `${r.scheme}|${r.cap}|${r.stratum}|${r.seed}|${r.resolveId}|${r.policy}`;
}

function indexRows(rows: readonly GateRow[]): Indexed {
  const byKey = new Map<string, GateRow>();
  for (const r of rows) {
    const k = rowKey(r);
    if (byKey.has(k))
      throw new GateInputError('duplicate-row', `duplicate row at ${k}`);
    byKey.set(k, r);
  }
  return {rows: [...rows], byKey};
}

function requireFinite(v: number | null, what: string, ctx: string): number {
  if (v === null || !Number.isFinite(v))
    throw new GateInputError('missing-field', `${ctx}: missing numeric ${what}`);
  return v;
}

// Per-row structural validation against the frozen stratum tuple and the
// note/termination state machine.
function validateRow(r: GateRow): void {
  const spec = STRATUM_BY_NAME.get(r.stratum)!;
  const ctx = `${r.scheme} cap=${r.cap} ${r.stratum} seed=${r.seed} `
    + `${r.resolveId} ${r.policy}`;
  if (r.root !== spec.root)
    throw new GateInputError('stratum-mismatch', `${ctx}: root '${r.root}' != frozen '${spec.root}'`);
  if (r.turnBoard !== spec.turnBoard)
    throw new GateInputError('stratum-mismatch', `${ctx}: turn_board '${r.turnBoard}' != frozen '${spec.turnBoard}'`);
  if (r.riverCard !== spec.riverCard)
    throw new GateInputError('stratum-mismatch', `${ctx}: river_card '${r.riverCard}' != frozen '${spec.riverCard}'`);
  if (r.range !== spec.range)
    throw new GateInputError('range-substitution',
      `${ctx}: range '${r.range}' cannot populate the '${spec.range}' stratum`);
  if (!Number.isInteger(r.seed))
    throw new GateInputError('bad-integer', `${ctx}: seed must be integer`);
  if (r.cap !== 0 && r.cap !== 1 && r.cap !== 2)
    throw new GateInputError('bad-enum', `${ctx}: cap must be 0,1,2`);
  for (const [name, v] of [
    ['trunk_rows', r.trunkRows], ['trunk_bytes', r.trunkBytes],
    ['infosets', r.infosets], ['nodes', r.nodes], ['peak_bytes', r.peakBytes],
    ['train_s', r.trainS], ['certify_s', r.certifyS],
    ['resolve_wall_s', r.resolveWallS], ['walk_s', r.walkS],
  ] as const) {
    if (v < 0)
      throw new GateInputError('bad-number', `${ctx}: ${name} must be non-negative (got ${v})`);
  }

  if (r.policy === 'hybrid') {
    if (r.cap === 0)
      throw new GateInputError('bad-enum', `${ctx}: hybrid rows cannot carry cap=0`);
    if (r.note === 'final' && r.termination !== 'Certified')
      throw new GateInputError('non-certified-final',
        `${ctx}: final gate-input hybrid row must be Certified (got ${r.termination}); `
        + `a failed attempt must be recorded note=truncated`);
    if (r.note === 'truncated'
        && !FAILURE_TERMINATIONS.includes(r.termination))
      throw new GateInputError('bad-termination-note',
        `${ctx}: truncated hybrid row must carry a typed failure termination`);
    if (r.note === 'diagnostic' && r.termination === 'Certified'
        && (r.maxCertSlack === null || r.maxCertSlack > CERT_SLACK_TOL))
      throw new GateInputError('cert-slack', `${ctx}: certified diagnostic exceeds slack`);
  } else if (r.policy === 'cap0ref') {
    if (r.cap !== 0)
      throw new GateInputError('bad-enum', `${ctx}: cap0ref rows must carry cap=0`);
    if (r.note !== 'final')
      throw new GateInputError('bad-termination-note',
        `${ctx}: cap0ref rows must be note=final`);
  } else {
    if (r.cap === 0)
      throw new GateInputError('bad-enum', `${ctx}: variant rows cannot carry cap=0`);
    if (r.note !== 'final')
      throw new GateInputError('bad-termination-note',
        `${ctx}: variant policy rows must be note=final`);
    if (r.termination !== 'Certified')
      throw new GateInputError('non-certified-final',
        `${ctx}: variant gate-input row must be Certified`);
  }

  // Determinism (addendum 6.1) and certification evidence. Every Certified
  // final row is a gate input, so cold-vs-cold bitwise identity is required
  // for all of them; warm-hint bitwise identity is a property of the two
  // hint configurations' served hybrid; the 1e-6 root-pot responder-margin
  // tolerance binds the four gadget-bearing policies; max_cert_slack is the
  // certifier evidence for any Certified profile.
  if (r.note === 'final' && r.termination === 'Certified') {
    if (r.coldcoldBitwise !== true)
      throw new GateInputError('determinism', `${ctx}: coldcold_bitwise must be true for a Certified gate row`);
    if (r.policy === 'hybrid' && HINT_SCHEMES.includes(r.scheme)
        && r.warmhintBitwise !== true)
      throw new GateInputError('determinism',
        `${ctx}: ${r.scheme} hybrid requires warmhint_bitwise true`);
    if (r.policy !== 'cap0ref' && r.maxMarginDiff !== null
        && r.maxMarginDiff > MARGIN_DIFF_TOL)
      throw new GateInputError('determinism',
        `${ctx}: max_margin_diff ${r.maxMarginDiff} exceeds ${MARGIN_DIFF_TOL}`);
    const slack = requireFinite(r.maxCertSlack, 'max_cert_slack', ctx);
    if (slack > CERT_SLACK_TOL)
      throw new GateInputError('cert-slack',
        `${ctx}: max_cert_slack ${slack} exceeds ${CERT_SLACK_TOL} root-pot`);
  }
}

function requirePartition(r: GateRow): void {
  const ctx = `${r.scheme} cap=${r.cap} ${r.stratum} seed=${r.seed} `
    + `${r.resolveId} ${r.policy}`;
  const spec = STRATUM_BY_NAME.get(r.stratum);
  if (!spec)
    throw new GateInputError('unknown-stratum', ctx);
  const total = requireFinite(r.totalNc, 'total_nc', ctx);
  const tTrunk = requireFinite(r.tTrunk, 't_trunk', ctx);
  const tBoundary = requireFinite(r.tBoundary, 't_boundary', ctx);
  const tRiver = requireFinite(r.tRiver, 't_river', ctx);
  const tAction = requireFinite(r.tAction, 't_action', ctx);
  const extension = requireFinite(r.extensionMass, 'extension_mass', ctx);
  const resid = requireFinite(r.telescopeResid, 'telescope_resid', ctx);
  if (Math.abs(tTrunk) > STRUCTURAL_ZERO_TOL)
    throw new GateInputError('trunk-not-zero',
      `${ctx}: t_trunk must be the pinned zero (got ${tTrunk})`);
  if (Math.abs(extension) > STRUCTURAL_ZERO_TOL)
    throw new GateInputError('extension-mass',
      `${ctx}: extension_mass must be identically zero (got ${extension})`);
  // telescope_resid and extension_mass are root-pot fractions; total_nc and
  // the four terms are BB per hand, so the root-pot-frozen floor/identity
  // thresholds are converted per stratum (finding M2).
  if (resid > IDENTITY_TOL)
    throw new GateInputError('identity-residual',
      `${ctx}: telescope_resid ${resid} > ${IDENTITY_TOL} root-pot`);
  const termFloorBb = TERM_FLOOR / (spec.bigBlind / spec.pot);
  const identityBb = IDENTITY_TOL / (spec.bigBlind / spec.pot);
  for (const [name, v] of [
    ['total_nc', total], ['t_boundary', tBoundary],
    ['t_river', tRiver], ['t_action', tAction],
  ] as const) {
    if (v < termFloorBb)
      throw new GateInputError('negative-term',
        `${ctx}: ${name} ${v} below ${TERM_FLOOR} root-pot (${termFloorBb} BB)`);
  }
  const recomputed = Math.abs(
    total - (tTrunk + tBoundary + tRiver + tAction));
  if (recomputed > identityBb)
    throw new GateInputError('identity-residual',
      `${ctx}: |total_nc - sum terms| ${recomputed} > ${IDENTITY_TOL} root-pot `
      + `(${identityBb} BB)`);
}

interface HybridSet {
  // The 30 timing slots (seeds 4320..4322 x R=10), each with exactly one
  // row: either a Certified final resolve or a typed truncated failure.
  timing: GateRow[];
  // Offline UCB walks on the non-timing seeds (w1 4323/4324); these exist
  // only for a block that passes Gate 2.
  extraUcb: GateRow[];
  truncated: GateRow[]; // subset of timing carrying note=truncated
  diagnostic: GateRow | null; // the single section-3 diagnostic profile
}

function collectHybrid(
  idx: Indexed, scheme: SchemeName, cap: number, stratum: StratumSpec,
): HybridSet {
  const rows = idx.rows.filter(
    (r) => r.policy === 'hybrid' && r.scheme === scheme && r.cap === cap
      && r.stratum === stratum.name);
  const set: HybridSet = {
    timing: [], extraUcb: [], truncated: [], diagnostic: null,
  };
  for (const r of rows) {
    if (r.note === 'diagnostic') {
      if (set.diagnostic !== null)
        throw new GateInputError('diagnostic-limit',
          `${scheme} cap=${cap} ${stratum.name}: at most one diagnostic profile row per stratum`);
      set.diagnostic = r;
    } else if (r.note === 'truncated') {
      if (!stratum.timingSeeds.includes(r.seed))
        throw new GateInputError('bad-seed',
          `${scheme} ${stratum.name}: a truncated cold resolve must occupy one of the `
          + `R=10 timing slots on seeds {${stratum.timingSeeds.join(',')}}, got ${r.seed}`);
      set.truncated.push(r);
      set.timing.push(r);
    } else {
      if (!stratum.seeds.includes(r.seed))
        throw new GateInputError('bad-seed',
          `${scheme} ${stratum.name}: final row seed ${r.seed} not in {${stratum.seeds.join(',')}}`);
      if (stratum.timingSeeds.includes(r.seed))
        set.timing.push(r);
      else
        set.extraUcb.push(r);
    }
  }
  return set;
}

function joinedRow(
  idx: Indexed, scheme: SchemeName, cap: number, stratum: string,
  seed: number, resolveId: string, policy: PolicyName,
): GateRow {
  const k = `${scheme}|${cap}|${stratum}|${seed}|${resolveId}|${policy}`;
  const r = idx.byKey.get(k);
  if (!r)
    throw new GateInputError('missing-policy-row', `missing joined row ${k}`);
  return r;
}

// Phase-A structure: exactly 30 cold resolves per stratum, R=10 per timing
// seed, one row per resolve slot (a typed truncated failure REPLACES the
// slot instead of being retried into a 31st resolve).
function checkTimingStructure(
  set: HybridSet, spec: StratumSpec, scheme: SchemeName, cap: number,
): void {
  const bySeed = new Map<number, Set<string>>();
  for (const r of set.timing) {
    let ids = bySeed.get(r.seed);
    if (!ids) {
      ids = new Set();
      bySeed.set(r.seed, ids);
    }
    if (ids.has(r.resolveId))
      throw new GateInputError('duplicate-resolve',
        `${scheme} cap=${cap} ${spec.name} seed=${r.seed}: duplicate resolve_id ${r.resolveId}`);
    ids.add(r.resolveId);
  }
  for (const seed of spec.timingSeeds) {
    const n = bySeed.get(seed)?.size ?? 0;
    if (n !== RESOLVES_PER_SEED)
      throw new GateInputError('timing-count',
        `${scheme} cap=${cap} ${spec.name} seed=${seed}: `
        + `${n} cold resolves, frozen R=${RESOLVES_PER_SEED}`);
  }
  if (bySeed.size !== spec.timingSeeds.length
      || set.timing.length !== N_TIMING)
    throw new GateInputError('timing-count',
      `${scheme} cap=${cap} ${spec.name}: ${set.timing.length} timing resolves, frozen ${N_TIMING}`);
  if (set.diagnostic && !spec.seeds.includes(set.diagnostic.seed))
    throw new GateInputError('bad-seed',
      `${scheme} ${spec.name}: diagnostic seed ${set.diagnostic.seed} not in frozen seeds`);
  // Non-timing UCB walks must not appear as timing slots.
  for (const r of set.extraUcb)
    if (spec.timingSeeds.includes(r.seed))
      throw new GateInputError('bad-seed',
        `${scheme} ${spec.name}: timing seed ${r.seed} row outside the 30 timing slots`);
}

// Phase-B structure, run only after Gate 2 passes: all 30 resolves
// Certified, the non-timing UCB seeds carry exactly one walk each, and
// every walk joins its three variants and the cap0ref reference.
function checkMeasurementStructure(
  idx: Indexed, set: HybridSet, spec: StratumSpec, scheme: SchemeName,
  cap: number,
): GateRow[] {
  if (set.truncated.length > 0 || set.diagnostic)
    throw new GateInputError('gate2-not-pass',
      `${scheme} cap=${cap} ${spec.name}: measurement joins require Gate-2 pass`);
  const certified = set.timing;
  const extraBySeed = new Map<number, number>();
  for (const r of set.extraUcb)
    extraBySeed.set(r.seed, (extraBySeed.get(r.seed) ?? 0) + 1);
  const extraSeeds = spec.seeds.filter((s) => !spec.timingSeeds.includes(s));
  for (const seed of extraSeeds) {
    if ((extraBySeed.get(seed) ?? 0) !== 1)
      throw new GateInputError('ucb-seed-count',
        `${scheme} cap=${cap} ${spec.name}: seed ${seed} needs exactly one `
        + `final UCB walk row (got ${extraBySeed.get(seed) ?? 0})`);
  }
  if (extraBySeed.size !== extraSeeds.length)
    throw new GateInputError('ucb-seed-count',
      `${scheme} cap=${cap} ${spec.name}: missing extra UCB seeds`);
  const walks = [...certified, ...set.extraUcb];
  for (const h of walks)
    checkJoinedWalk(idx, h, scheme, cap, spec);
  return walks;
}

function checkJoinedWalk(
  idx: Indexed, h: GateRow, scheme: SchemeName, cap: number, spec: StratumSpec,
): void {
  const river = joinedRow(idx, scheme, cap, spec.name, h.seed, h.resolveId, 'rivermerge');
  const bucket = joinedRow(idx, scheme, cap, spec.name, h.seed, h.resolveId, 'bucketmargin');
  const stattr = joinedRow(idx, scheme, cap, spec.name, h.seed, h.resolveId, 'statictrunk');
  const ref = joinedRow(idx, scheme, 0, spec.name, h.seed, h.resolveId, 'cap0ref');
  for (const v of [river, bucket, stattr, ref]) {
    if (v.range !== h.range || v.root !== h.root || v.turnBoard !== h.turnBoard)
      throw new GateInputError('join-mismatch', `joined row ${rowKey(v)} mismatches hybrid tuple`);
  }
  for (const v of [river, bucket, stattr])
    requirePartition(v);
  // The offline cap-0 reference is exempt from the online wall but bounded
  // by the frozen outer budget (20M nodes / 8 GiB / 600 s); a Certified row
  // outside it is inconsistent input, and a failed build drives Gate 3.
  if (ref.termination === 'Certified') {
    if (ref.nodes > CAP0REF_OUTER_NODES
        || ref.peakBytes > CAP0REF_OUTER_BYTES
        || ref.resolveWallS > CAP0REF_OUTER_S)
      throw new GateInputError('cap0ref-outer-budget',
        `${rowKey(ref)}: Certified cap0ref outside the 20M/8GiB/600s outer budget`);
  } else if (!FAILURE_TERMINATIONS.includes(ref.termination)) {
    throw new GateInputError('bad-termination-note',
      `${rowKey(ref)}: cap0ref termination must be Certified or a typed build failure`);
  }
  const hTRiver = requireFinite(h.tRiver, 't_river', rowKey(h));
  const hTBound = requireFinite(h.tBoundary, 't_boundary', rowKey(h));
  // Harness 5.4 directional checks (tol = 1e-9 root-pot); the term columns are
  // BB per hand, so the tolerance is converted per stratum (finding M2).
  const directionalBb = DIRECTIONAL_TOL / (spec.bigBlind / spec.pot);
  const vTRiver = requireFinite(river.tRiver, 't_river', rowKey(river));
  if (spec.name !== MONOTONE_STRATUM) {
    if (vTRiver < hTRiver - directionalBb)
      throw new GateInputError('directional-rivermerge',
        `${rowKey(river)}: river-bucket mass ${vTRiver} < T_river ${hTRiver} - tol`);
  } else {
    // Strictly larger on the monotone stratum (gap > 1e-9 root-pot).
    if (!(vTRiver - hTRiver > directionalBb))
      throw new GateInputError('directional-rivermerge-strict',
        `${rowKey(river)}: monotone stratum requires rivermerge river mass `
        + `strictly above T_river by > ${DIRECTIONAL_TOL} root-pot`);
  }
  for (const v of [bucket, stattr]) {
    const vTBound = requireFinite(v.tBoundary, 't_boundary', rowKey(v));
    if (vTBound < hTBound - directionalBb)
      throw new GateInputError('directional-turn',
        `${rowKey(v)}: turn-bucket mass ${vTBound} < T_boundary ${hTBound} - tol`);
  }
}

function checkDiagnosticBounds(r: GateRow): boolean {
  return r.nodes <= DIAG_MAX_NODES
    && r.infosets <= DIAG_MAX_INFOSETS
    && r.peakBytes <= DIAG_MAX_BYTES
    && r.resolveWallS <= DIAG_MAX_WALL_S;
}

// Addendum 6.2 typed status mapping. The 30-slot timing structure has
// already been enforced; this maps the typed failures plus the one-shot
// diagnostic profile to pass / budget-tight / bound-unreachable.
function classifyGate2(
  set: HybridSet, scheme: SchemeName, spec: StratumSpec, cap: number,
): {classification: Gate2Classification; failures: string[]; stats: TimingStats | null;
    envelopePass: boolean} {
  const failures: string[] = [];
  // A truncated slot is a resolve that did not certify inside the frozen
  // section-4 envelope/deadline, so the stratum does not pass the envelope
  // even if all recorded Certified rows stayed within bounds.
  let envelopePass = set.truncated.length === 0;
  for (const r of set.timing) {
    if (r.note !== 'final') {
      failures.push(`resolve:${r.seed}/${r.resolveId}:${r.termination}`);
      continue;
    }
    if (r.nodes > MAX_NODES || r.infosets > MAX_INFOSETS
        || r.peakBytes > MAX_BYTES) {
      envelopePass = false;
      failures.push(`envelope:${r.seed}/${r.resolveId}`);
    }
  }
  const certified = set.timing.filter((r) => r.note === 'final');
  const stats: TimingStats | null = certified.length === N_TIMING
    ? {
        n: N_TIMING,
        p50S: nearestRank(certified.map((r) => r.resolveWallS).sort((a, b) => a - b), P50_RANK),
        p95S: nearestRank(certified.map((r) => r.resolveWallS).sort((a, b) => a - b), P95_RANK),
        maxS: nearestRank(certified.map((r) => r.resolveWallS).sort((a, b) => a - b), MAX_RANK),
      }
    : null;
  if (stats && (stats.p95S > P95_WALL_S || stats.maxS > MAX_WALL_S))
    failures.push('wall-p95-or-max');

  if (set.truncated.length === 0 && failures.length === 0 && stats)
    return {classification: 'pass', failures, stats, envelopePass};

  // Any typed/envelope/wall failure requires the one-shot diagnostic
  // profile; without it the stratum cannot be mapped and evaluation aborts.
  if (!set.diagnostic)
    throw new GateInputError('diagnostic-required',
      `${scheme} cap=${cap} ${spec.name}: failed resolve(s) and no diagnostic profile row`);
  const diag = set.diagnostic;
  const diagCertifies = diag.termination === 'Certified' && checkDiagnosticBounds(diag);
  // CertificationRejected means the exact bound fails even on the enlarged
  // diagnostic profile (addendum 6.2): a certifying diagnostic contradicts
  // that typed outcome, and an inconclusive/out-of-bounds one confirms it.
  if (set.truncated.some((r) => r.termination === 'CertificationRejected')) {
    if (diagCertifies)
      throw new GateInputError('inconsistent-classification',
        `${scheme} ${spec.name}: CertificationRejected attempt but the diagnostic profile certified`);
    return {classification: 'bound-unreachable', failures, stats, envelopePass};
  }
  if (!set.truncated.every((r) => DEADLINE_OR_RESOURCE.includes(r.termination)))
    throw new GateInputError('bad-termination-note',
      `${scheme} ${spec.name}: unclassified truncated termination set`);
  // Deadline/resource failures: certified-under-enlarged-profile classifies
  // budget-tight; anything else is bound-unreachable.
  if (!diagCertifies)
    return {classification: 'bound-unreachable', failures, stats, envelopePass};
  return {classification: 'budget-tight', failures, stats, envelopePass};
}

function perSeedMeans(rows: readonly GateRow[], pick: (r: GateRow) => number):
    Map<number, number> {
  const bySeed = new Map<number, number[]>();
  for (const r of rows) {
    const arr = bySeed.get(r.seed) ?? [];
    arr.push(pick(r));
    bySeed.set(r.seed, arr);
  }
  const out = new Map<number, number>();
  for (const [seed, xs] of bySeed)
    out.set(seed, mean(xs));
  return out;
}

function perSeedUcb(
  rows: readonly GateRow[], spec: StratumSpec,
  pick: (r: GateRow) => number,
): UcbStats {
  const means = perSeedMeans(rows, pick);
  const ordered = spec.seeds.map((s) => {
    const v = means.get(s);
    if (v === undefined)
      throw new GateInputError('ucb-seed-count',
        `missing per-seed value for ${spec.name} seed ${s}`);
    return v;
  });
  return ucbStats(ordered, spec.tCrit);
}

function gate3StatsFor(
  idx: Indexed, scheme: SchemeName, cap: number, strata: readonly StratumSpec[],
): {perStratum: Gate3Stratum[]; anomaly: string[]; buildFailures: string[]} {
  const perStratum: Gate3Stratum[] = [];
  const anomaly: string[] = [];
  const buildFailures: string[] = [];
  for (const spec of strata) {
    const set = collectHybrid(idx, scheme, cap, spec);
    if (set.truncated.length > 0 || set.diagnostic)
      throw new GateInputError('gate2-not-pass',
        `${scheme} cap=${cap} ${spec.name}: Gate 3 runs only after Gate-2 pass`);
    checkTimingStructure(set, spec, scheme, cap);
    const walks = checkMeasurementStructure(idx, set, spec, scheme, cap);
    for (const h of walks) {
      const ref = joinedRow(idx, scheme, 0, spec.name, h.seed, h.resolveId, 'cap0ref');
      if (ref.termination !== 'Certified')
        buildFailures.push(spec.name);
    }
    if (buildFailures.includes(spec.name)) {
      perStratum.push({
        stratum: spec.name, n: spec.seeds.length,
        nc0Mean: NaN, nc1Mean: NaN,
        gap: {n: spec.seeds.length, mean: NaN, sd: 0, halfWidth: 0,
               ucb: NaN, lcb: NaN},
      });
      continue;
    }
    const gaps = perSeedMeans(walks, (r) => {
      const nc1 = requireFinite(r.ncEndgameCap1, 'nc_endgame_cap1', rowKey(r));
      const nc0 = requireFinite(r.ncEndgameCap0, 'nc_endgame_cap0', rowKey(r));
      return nc1 - nc0;
    });
    const nc1Means = perSeedMeans(walks, (r) =>
      requireFinite(r.ncEndgameCap1, 'nc_endgame_cap1', rowKey(r)));
    const nc0Means = perSeedMeans(walks, (r) =>
      requireFinite(r.ncEndgameCap0, 'nc_endgame_cap0', rowKey(r)));
    const gapStats = ucbStats(spec.seeds.map((s) => gaps.get(s)!), spec.tCrit);
    // The gap LCB is in BB per hand; the frozen sign-anomaly floor is
    // -1e-6 root-pot, converted per stratum (finding M2).
    const gapAnomalyBb = GAP_ANOMALY / (spec.bigBlind / spec.pot);
    if (gapStats.lcb < gapAnomalyBb)
      anomaly.push(spec.name);
    perStratum.push({
      stratum: spec.name, n: spec.seeds.length,
      nc0Mean: mean(spec.seeds.map((s) => nc0Means.get(s)!)),
      nc1Mean: mean(spec.seeds.map((s) => nc1Means.get(s)!)),
      gap: gapStats,
    });
  }
  return {perStratum, anomaly, buildFailures: [...new Set(buildFailures)]};
}

// Frozen Gate-1 total order: rows, bytes, smallest cold-resolve p95, name.
export interface Gate1OrderEntry {
  scheme: string;
  trunkRows: number;
  trunkBytes: number;
  worstP95S: number;
  eligible: boolean;
}

export function orderKey(c: Pick<Gate1OrderEntry,
  'trunkRows' | 'trunkBytes' | 'worstP95S' | 'scheme'>):
    [number, number, number, string] {
  return [c.trunkRows, c.trunkBytes, c.worstP95S, c.scheme];
}

export function selectGate1(candidates: readonly Gate1OrderEntry[]):
    string | null {
  const eligible = candidates.filter((c) => c.eligible);
  if (eligible.length === 0)
    return null;
  return [...eligible].sort((a, b) => {
    const ka = orderKey(a);
    const kb = orderKey(b);
    for (let i = 0; i < 4; ++i) {
      if (ka[i]! < kb[i]!)
        return -1;
      if (ka[i]! > kb[i]!)
        return 1;
    }
    return 0;
  })[0]!.scheme;
}

// Phase A: Gate-2 structure and typed classification for one block.
function gate2Block(
  idx: Indexed, scheme: SchemeName, cap: number,
): StratumGate2[] {
  const out: StratumGate2[] = [];
  for (const spec of STRATA) {
    const set = collectHybrid(idx, scheme, cap, spec);
    checkTimingStructure(set, spec, scheme, cap);
    const classified = classifyGate2(set, scheme, spec, cap);
    out.push({
      scheme, cap, stratum: spec.name, range: spec.range,
      certifiedResolves: set.timing.filter((r) => r.note === 'final').length,
      timingSamples: N_TIMING,
      p50S: classified.stats?.p50S ?? null,
      p95S: classified.stats?.p95S ?? null,
      maxS: classified.stats?.maxS ?? null,
      envelopePass: classified.envelopePass,
      classification: classified.classification,
      failures: classified.failures,
    });
  }
  return out;
}

// Phase B: Gate-1 measurement for one candidate. Runs only after Gate 2 has
// passed globally, so every walk joins its variants and the reference.
function gate1Block(
  idx: Indexed, scheme: SchemeName, handpair: HandPairManifest,
): {perStratum: CandidateGate1['perStratum']; worstP95S: number} {
  const perStratum: CandidateGate1['perStratum'] = [];
  let worstP95S = 0;
  for (const spec of STRATA) {
    const set = collectHybrid(idx, scheme, 1, spec);
    checkTimingStructure(set, spec, scheme, 1);
    const walks = checkMeasurementStructure(idx, set, spec, scheme, 1);
    for (const h of walks) {
      requirePartition(h);
      requireFinite(h.ncEndgameCap1, 'nc_endgame_cap1', rowKey(h));
      requireFinite(h.ncEndgameCap0, 'nc_endgame_cap0', rowKey(h));
      if (spec.name === handpair.stratum && h.riverArgmaxDiff !== true)
        throw new GateInputError('handpair-argmax',
          `${rowKey(h)}: behavioral hand pair served argmax must differ on ${handpair.stratum}`);
    }
    const classified = classifyGate2(set, scheme, spec, 1);
    if (classified.classification !== 'pass' || !classified.stats)
      throw new GateInputError('gate2-not-pass',
        `${scheme} ${spec.name}: Gate-1 measurement requires Gate-2 pass`);
    worstP95S = Math.max(worstP95S, classified.stats.p95S);
    const total = perSeedUcb(walks, spec, (r) =>
      requireFinite(r.totalNc, 'total_nc', rowKey(r)));
    const river = perSeedUcb(walks, spec, (r) =>
      requireFinite(r.tRiver, 't_river', rowKey(r)));
    perStratum.push({stratum: spec.name, n: spec.seeds.length, total, river});
  }
  return {perStratum, worstP95S};
}

export function evaluateGates(
  input: readonly GateRow[], handpair: HandPairManifest,
): GateVerdict {
  const rows = [...input];
  for (const r of rows)
    validateRow(r);

  // Addendum section 3 freezes exactly ONE diagnostic profile PER STRATUM IN
  // TOTAL across every candidate block (no rerun under any outcome), so the
  // quota is global over the whole CSV, not per (candidate, stratum).
  const diagPerStratum = new Map<string, number>();
  for (const r of rows)
    if (r.note === 'diagnostic')
      diagPerStratum.set(r.stratum, (diagPerStratum.get(r.stratum) ?? 0) + 1);
  for (const [stratum, count] of diagPerStratum)
    if (count > 1)
      throw new GateInputError('diagnostic-limit',
        `${stratum}: at most one diagnostic profile row per stratum in total, got ${count}`);

  const idx = indexRows(rows);

  // Every frozen candidate must be present for every stratum; no other
  // scheme/cap/stratum tokens survive parse. Reject stray cap=0 hybrids and
  // cap=2 cap0ref rows structurally.
  for (const scheme of CANDIDATES) {
    const schemeRows = rows.filter((r) => r.scheme === scheme);
    const trunks = new Set(schemeRows.map((r) => `${r.trunk}`));
    if (trunks.size === 0)
      throw new GateInputError('missing-candidate', `candidate ${scheme} has no rows`);
    if (trunks.size !== 1)
      throw new GateInputError('trunk-inconsistency',
        `${scheme}: trunk label must be constant, got {${[...trunks].join(',')}}`);
    const consts = new Map<string, [number, number]>();
    for (const r of schemeRows) {
      const k = `${r.trunk}`;
      const prev = consts.get(k);
      const pair: [number, number] = [r.trunkRows, r.trunkBytes];
      if (prev && (prev[0] !== pair[0] || prev[1] !== pair[1]))
        throw new GateInputError('trunk-inconsistency',
          `${scheme}/${r.trunk}: trunk_rows/trunk_bytes must be constant `
          + `(${prev.join('/')} vs ${pair.join('/')})`);
      consts.set(k, pair);
    }
  }

  // Phase A: Gate 2 over every (candidate x stratum) cap-1 block. Gate 2
  // CLOSE/REDIRECT is total and stops Gates 1/3 (addendum sections 7,10),
  // so measurement joins are neither required nor inspected on failure.
  const gate2Strata: StratumGate2[] = [];
  for (const scheme of CANDIDATES)
    gate2Strata.push(...gate2Block(idx, scheme, 1));

  let gate2Status: 'PASS' | 'REDIRECT' | 'CLOSE' = 'PASS';
  if (gate2Strata.some((s) => s.classification === 'bound-unreachable'))
    gate2Status = 'CLOSE';
  else if (gate2Strata.some((s) => s.classification === 'budget-tight'))
    gate2Status = 'REDIRECT';

  const emptyGate1 = {candidates: [] as CandidateGate1[],
    eligible: [] as SchemeName[], selected: null as SchemeName | null};
  if (gate2Status !== 'PASS') {
    return {
      verdict: gate2Status,
      handpair: {
        stratum: handpair.stratum, board: handpair.board,
        river: handpair.river, fallbackUsed: handpair.fallbackUsed,
      },
      gate2: {status: gate2Status, strata: gate2Strata},
      gate1: emptyGate1,
      gate3: null,
    };
  }

  // Phase B: Gate-1 measurement, only with Gate 2 passing everywhere.
  const candidates: CandidateGate1[] = [];
  for (const scheme of CANDIDATES) {
    const block = gate1Block(idx, scheme, handpair);
    const sixMaxUcbTotal = Math.max(
      ...block.perStratum.map((s) => s.total.ucb));
    const sixMaxUcbRiver = Math.max(
      ...block.perStratum.map((s) => s.river.ucb));
    const schemeRows = rows.filter((r) => r.scheme === scheme);
    candidates.push({
      scheme,
      trunk: schemeRows[0]!.trunk,
      trunkRows: schemeRows[0]!.trunkRows,
      trunkBytes: schemeRows[0]!.trunkBytes,
      worstP95S: block.worstP95S,
      perStratum: block.perStratum,
      sixMaxUcbTotal,
      sixMaxUcbRiver,
      eligible: sixMaxUcbTotal <= B0 && sixMaxUcbRiver <= R0,
    });
  }

  const eligible = candidates.filter((c) => c.eligible).map((c) => c.scheme);
  const selectedName = selectGate1(candidates);
  const selected = (CANDIDATES as readonly string[]).includes(selectedName ?? '')
    ? (selectedName as SchemeName)
    : null;

  let verdict: 'PASS' | 'REDIRECT' | 'CLOSE';
  let gate3: Gate3Result | null = null;
  if (selected === null) {
    verdict = 'CLOSE';
  } else {
    gate3 = evaluateGate3(idx, selected);
    verdict = gate3.passes ? 'PASS' : 'CLOSE';
  }

  return {
    verdict,
    handpair: {
      stratum: handpair.stratum,
      board: handpair.board,
      river: handpair.river,
      fallbackUsed: handpair.fallbackUsed,
    },
    gate2: {status: gate2Status, strata: gate2Strata},
    gate1: {candidates, eligible, selected},
    gate3,
  };
}

function evaluateCap2Escalation(
  idx: Indexed, selected: SchemeName, failing: readonly string[],
): {cap2: Gate3Stratum[]; passed: boolean} {
  const failingSpecs = STRATA.filter((s) => failing.includes(s.name));
  // The cap-2 remeasure keeps the unchanged section-3/4 wall/node/infoset/
  // byte envelope; Gate 3 changes only the action cap.
  for (const spec of failingSpecs) {
    const set = collectHybrid(idx, selected, 2, spec);
    if (set.truncated.length > 0 || set.diagnostic)
      return {cap2: [], passed: false};
    checkTimingStructure(set, spec, selected, 2);
    const classified = classifyGate2(set, selected, spec, 2);
    if (classified.classification !== 'pass')
      return {cap2: [], passed: false};
    const walks = checkMeasurementStructure(idx, set, spec, selected, 2);
    for (const h of walks) {
      requirePartition(h);
      requireFinite(h.ncEndgameCap1, 'nc_endgame_cap1', rowKey(h));
      requireFinite(h.ncEndgameCap0, 'nc_endgame_cap0', rowKey(h));
    }
  }
  const {perStratum, anomaly, buildFailures} =
    gate3StatsFor(idx, selected, 2, failingSpecs);
  if (anomaly.length > 0 || buildFailures.length > 0)
    return {cap2: perStratum, passed: false};
  const passed = perStratum.every((s) => s.gap.ucb <= M_GAP);
  return {cap2: perStratum, passed};
}

function evaluateGate3(idx: Indexed, selected: SchemeName): Gate3Result {
  const {perStratum, anomaly, buildFailures} =
    gate3StatsFor(idx, selected, 1, STRATA);
  if (anomaly.length > 0)
    throw new GateInputError('gap-sign-anomaly',
      `Gate 3 estimator anomaly: cap-gap LCB < ${GAP_ANOMALY} root-pot at `
      + anomaly.join(','));
  if (buildFailures.length > 0) {
    return {
      candidate: selected, cap: 1, perStratum, anomalyStrata: [],
      failingAtCap1: buildFailures,
      escalation: null, passes: false,
      closedReason: 'cap0ref-build-failure',
    };
  }
  const failing = perStratum.filter((s) => s.gap.ucb > M_GAP).map((s) => s.stratum);
  if (failing.length === 0) {
    return {
      candidate: selected, cap: 1, perStratum, anomalyStrata: [],
      failingAtCap1: [], escalation: null, passes: true, closedReason: null,
    };
  }
  // Sole pre-authorized escalation: one cap-2 remeasure under the unchanged
  // section-3/4 envelope, required only on the failing strata.
  const hasCap2Rows = idx.rows.some(
    (r) => r.scheme === selected && r.cap === 2 && r.policy === 'hybrid');
  if (!hasCap2Rows) {
    return {
      candidate: selected, cap: 1, perStratum, anomalyStrata: [],
      failingAtCap1: failing, escalation: {
        attempted: false, cap2: [], passed: false, failingStrata: failing,
      },
      passes: false, closedReason: 'gap-exceeds-m-no-escalation',
    };
  }
  const esc = evaluateCap2Escalation(idx, selected, failing);
  return {
    candidate: selected, cap: esc.passed ? 2 : 1, perStratum,
    anomalyStrata: [], failingAtCap1: failing,
    escalation: {
      attempted: true, cap2: esc.cap2, passed: esc.passed,
      failingStrata: failing,
    },
    passes: esc.passed,
    closedReason: esc.passed ? null : 'cap2-escalation-failed',
  };
}

// --- CLI --------------------------------------------------------------------

function fmt(x: number | null | undefined, digits = 6): string {
  return x !== null && x !== undefined && Number.isFinite(x)
    ? x.toFixed(digits) : 'NA';
}

function usage(): never {
  console.error(
    'usage:\n'
    + '  rfc0046-gates <gate-csv...> [--json] [--handpair manifest.json]\n'
    + '\n'
    + `Hand-pair manifest defaults to ${DEFAULT_HANDPAIR_MANIFEST}.`);
  process.exit(2);
}

function printText(v: GateVerdict): void {
  console.log(`RFC 0046 Stage-0 gates: ${v.verdict}`);
  console.log(`hand-pair: ${v.handpair.stratum} ${v.handpair.board}/${v.handpair.river}`
    + `${v.handpair.fallbackUsed ? ' (fallback)' : ''}`);
  console.log(`Gate 2: ${v.gate2.status}`);
  for (const s of v.gate2.strata) {
    if (s.cap !== 1)
      continue;
    console.log(`  ${s.scheme} ${s.stratum} [${s.range}] ${s.classification} `
      + `p50=${fmt(s.p50S, 3)}s p95=${fmt(s.p95S, 3)}s max=${fmt(s.maxS, 3)}s`
      + `${s.failures.length ? ` failures=[${s.failures.join(';')}]` : ''}`);
  }
  for (const c of v.gate1.candidates) {
    console.log(`Gate 1 ${c.scheme}: eligible=${c.eligible} rows=${c.trunkRows} `
      + `bytes=${c.trunkBytes} worstP95=${fmt(c.worstP95S, 3)}s `
      + `maxUcbTotal=${fmt(c.sixMaxUcbTotal)} maxUcbRiver=${fmt(c.sixMaxUcbRiver)}`);
  }
  console.log(`Gate 1 selected: ${v.gate1.selected ?? 'none'}`);
  if (v.gate3) {
    console.log(`Gate 3: ${v.gate3.passes ? 'PASS' : 'CLOSE'} `
      + `(candidate ${v.gate3.candidate}, cap ${v.gate3.cap}`
      + `${v.gate3.closedReason ? `, ${v.gate3.closedReason}` : ''})`);
    for (const s of v.gate3.perStratum)
      console.log(`  ${s.stratum}: gap mean=${fmt(s.gap.mean)} `
        + `UCB=${fmt(s.gap.ucb)} LCB=${fmt(s.gap.lcb)} `
        + `NC0=${fmt(s.nc0Mean)} NC1=${fmt(s.nc1Mean)}`);
  }
}

const invokedDirectly = (() => {
  const entry = process.argv[1] ?? '';
  return /rfc0046-gates(\.[a-f0-9]+)?\.(m?js)$/.test(entry);
})();

if (invokedDirectly) {
  const args = process.argv.slice(2);
  const asJson = args.includes('--json');
  let handpairPath = DEFAULT_HANDPAIR_MANIFEST;
  const hi = args.indexOf('--handpair');
  if (hi !== -1) {
    const p = args[hi + 1];
    if (!p)
      usage();
    handpairPath = p;
  }
  const paths = args.filter((a, i) =>
    a !== '--json' && !(hi !== -1 && (i === hi || i === hi + 1)));
  if (paths.length === 0)
    usage();
  try {
    const handpair = loadHandPairManifest(handpairPath);
    const verdict = evaluateGates(loadGateCsvs(paths), handpair);
    if (asJson)
      console.log(JSON.stringify(verdict, null, 2));
    else
      printText(verdict);
  } catch (e) {
    const err = e as GateInputError;
    if (asJson || process.env.RFC0046_JSON_ERROR === '1')
      console.error(JSON.stringify({error: err.code ?? 'error', message: err.message}));
    else
      console.error(`gate evaluation aborted: [${err.code ?? 'error'}] ${err.message}`);
    process.exit(2);
  }
}
