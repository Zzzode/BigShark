// RFC 0047 Stage-1 frozen evaluator of record.
//
// S1 implements only the STRUCTURAL predicates:
//   G1  per-request concrete-infoset envelope (< 1,000,000) for the
//       depth-limited turn solve and the worst-case reached (line,card) river
//       re-solve, on every frozen stratum, plus per-card cross-line uniformity.
//   G4  canonical board-class constants and the turn x48 = 5 x river identity.
// G2/G3 (oracle accuracy, error-propagation slope) are added in S2; until
// then this evaluator cannot emit a Stage PASS, only the S1 structural result.
//
// Inputs are the tidy CSVs emitted by the guarded C++ binaries:
//   bigshark-rfc0047-infoset-budget -> s1-infoset-budget.csv
//   bigshark-rfc0047-census         -> s1-census.csv  (metric,value)
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

// Manual entrypoint: node dist/tools/rfc/rfc0047-gates.js <infoset.csv> <census.csv>
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
  if (!infosetPath || !censusPath) {
    console.error('usage: rfc0047-gates <s1-infoset-budget.csv> <s1-census.csv>');
    process.exit(2);
  }
  try {
    const result = evaluateS1(readFileSync(infosetPath, 'utf8'),
                             readFileSync(censusPath, 'utf8'));
    if (!result.g1 || !result.g4) {
      console.error('RFC 0047 S1 structural gates FAILED:');
      for (const f of result.failures)
        console.error(`  - ${f}`);
      process.exit(1);
    }
    console.log('RFC 0047 S1 structural gates PASS (G1 infoset envelope, G4 census).');
    console.log('G2/G3 oracle accuracy/slope gates are pending S2 (no Stage PASS yet).');
  } catch (err) {
    console.error(`RFC 0047 gate input error: ${(err as Error).message}`);
    process.exit(2);
  }
}
