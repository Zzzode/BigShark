import assert from 'node:assert/strict';
import {test} from 'node:test';

import {
  CANONICAL_FLOP_CLASSES,
  CANONICAL_RIVER_CLASSES,
  CANONICAL_TURN_CLASSES,
  CONCRETE_RIVER_BOARDS,
  CONCRETE_TURN_BOARDS,
  evaluateS1,
  GateInputError,
  MAX_INFOSETS,
  parseCensusCsv,
  parseInfosetCsv,
  TURN_X48,
} from './rfc0047-gates.js';

const INFOSET_HEADER =
  'stratum,pot,total_nodes,turn_action_nodes,chance_nodes,frontier_leaves,' +
  'river_action_nodes_total,reached_min,reached_mean_x1000,reached_max,' +
  'per_card_actions_min,per_card_actions_max,per_card_uniform,turn_combos,' +
  'river_combos,turn_infosets,reached_river_infosets_max,' +
  'per_card_infosets_offline,conservative_online_sum,under_1m';

function infosetRow(stratum: string,
                    o: Partial<Record<string, string | number | boolean>> = {}): string {
  const d: Record<string, string | number | boolean> = {
    stratum,
    pot: 4, total_nodes: 10923, turn_action_nodes: 10, chance_nodes: 9,
    frontier_leaves: 432, river_action_nodes_total: 3936, reached_min: 0,
    reached_mean_x1000: 9.111, reached_max: 12, per_card_actions_min: 82,
    per_card_actions_max: 82, per_card_uniform: true, turn_combos: 1128,
    river_combos: 1081, turn_infosets: 12408, reached_river_infosets_max: 14053,
    per_card_infosets_offline: 89723, conservative_online_sum: 26461,
    under_1m: true, ...o,
  };
  return Object.values(d).map((v) => String(v)).join(',');
}

const SIX = [
  'w1', 'unpaired-rainbow-ahigh', 'paired', 'monotone', 'unpaired-twotone',
];
function validInfosetCsv(): string {
  const limped = SIX.map((s) => infosetRow(s));
  const srp = infosetRow('raised-srp', {
    pot: 8, total_nodes: 14385, turn_action_nodes: 12,
    chance_nodes: 11, frontier_leaves: 528, river_action_nodes_total: 5184,
    reached_mean_x1000: 9.818, per_card_actions_min: 108,
    per_card_actions_max: 108, turn_infosets: 14664,
    per_card_infosets_offline: 117829, conservative_online_sum: 28717,
  });
  return [INFOSET_HEADER, ...limped, srp].join('\n');
}

function validCensusCsv(): string {
  return [
    'metric,value',
    `concrete_flop_boards,22100`,
    `canonical_flop_classes,${CANONICAL_FLOP_CLASSES}`,
    `concrete_turn_boards,${CONCRETE_TURN_BOARDS}`,
    `canonical_turn_classes,${CANONICAL_TURN_CLASSES}`,
    `concrete_river_boards,${CONCRETE_RIVER_BOARDS}`,
    `canonical_river_classes,${CANONICAL_RIVER_CLASSES}`,
    `turn_boards_times_48,${TURN_X48}`,
    `five_times_river_boards,${TURN_X48}`,
    `turn_river_identity,1`,
  ].join('\n');
}

test('S1 structural gates pass on the measured CSV', () => {
  const r = evaluateS1(validInfosetCsv(), validCensusCsv());
  assert.deepEqual(r.failures, []);
  assert.equal(r.g1, true);
  assert.equal(r.g4, true);
});

test('parse rejects a wrong header', () => {
  assert.throws(() => parseInfosetCsv('a,b\n1,2\n'), GateInputError);
  assert.throws(() => parseCensusCsv('x\n'), GateInputError);
});

test('G1 fails when an online footprint reaches the envelope', () => {
  const bad = validInfosetCsv()
    .split('\n')
    .map((line) => line.startsWith('monotone,')
      ? line.replace('14053,89723,26461', '14053,89723,1000001')
      : line)
    .join('\n');
  const r = evaluateS1(bad, validCensusCsv());
  assert.equal(r.g1, false);
  assert.ok(r.failures.some((f) => f.includes('conservative_online_sum')));
});

test('G1 fails on non-uniform per-card river nodes', () => {
  const bad = validInfosetCsv()
    .split('\n')
    .map((l) => l.startsWith('w1,') ? l.replace('82,82,true', '80,82,false') : l)
    .join('\n');
  const r = evaluateS1(bad, validCensusCsv());
  assert.equal(r.g1, false);
  assert.ok(r.failures.some((f) => f.includes('not uniform')));
});

test('missing stratum fails closed', () => {
  const rows = validInfosetCsv().split('\n').filter((l) => !l.startsWith('paired,'));
  const r = evaluateS1(rows.join('\n'), validCensusCsv());
  assert.ok(r.failures.some((f) => f.includes("missing stratum 'paired'")));
});

test('G4 fails on a wrong river class constant', () => {
  const bad = validCensusCsv().replace(String(CANONICAL_RIVER_CLASSES), '134458');
  const r = evaluateS1(validInfosetCsv(), bad);
  assert.equal(r.g4, false);
  assert.ok(r.failures.some((f) => f.includes('canonical_river_classes')));
});

test('envelope constant is the frozen one million', () => {
  assert.equal(MAX_INFOSETS, 1_000_000);
});
