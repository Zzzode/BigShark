import assert from 'node:assert/strict';
import {test} from 'node:test';

import {
  CANONICAL_FLOP_CLASSES,
  CANONICAL_RIVER_CLASSES,
  CANONICAL_TURN_CLASSES,
  CONCRETE_RIVER_BOARDS,
  CONCRETE_TURN_BOARDS,
  EPS_CERT_BB,
  EPS_HYBRID_B0,
  EPS_QUAL_BB,
  EPS_RIVER_R0,
  evaluateS1,
  evaluateS2G2,
  evaluateS2G3,
  GateInputError,
  MAX_INFOSETS,
  operatingEMaxPotFraction,
  operatingEL2PotFraction,
  parseCensusCsv,
  parseCertCsv,
  parseG3Csv,
  parseInfosetCsv,
  T_CRIT_0_95_5,
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

const CERT_HEADER =
  'stratum,shape_id,group,pot_chips,cand_nash_conv_bb,ref_nash_conv_bb,' +
  'drift_max_bb,residual_max_bb,cert_max_bb,' +
  'drift_rms_bb,residual_rms_bb,cert_rms_bb';

function certRow(o: Partial<Record<string, string | number>> = {}): string {
  const d: Record<string, string | number> = {
    stratum: 'w1', shape_id: 7, group: 0, pot_chips: 4,
    cand_nash_conv_bb: 0.002, ref_nash_conv_bb: 0.0002,
    // Per-entry G3-input numbers may be large; they are not G2-gated.
    drift_max_bb: 0.03, residual_max_bb: 0.0008, cert_max_bb: 0.0308,
    drift_rms_bb: 0.007, residual_rms_bb: 0.0004, cert_rms_bb: 0.0074, ...o,
  };
  return Object.values(d).map((v) => String(v)).join(',');
}

test('G2 passes when summed NashConv clears CERT/QUAL despite large e_max', () => {
  const csv = [CERT_HEADER, certRow(), certRow({group: 1, stratum: 'paired'})].join('\n');
  const r = evaluateS2G2(csv);
  assert.equal(r.g2, true, r.failures.join('; '));
  assert.equal(r.rows, 2);
  assert.equal(r.certBb, 0.002);       // CERT summed NashConv
  assert.equal(r.qualBb, 0.002);       // QUAL identical for V3
  assert.ok(r.eMaxBb >= 0.03);         // per-combo worst recorded, not gated
  assert.ok(r.driftRmsMaxBb >= 0.007); // per-combo RMS recorded, not gated
});

test('G2 fails when summed NashConv exceeds the CERT bar', () => {
  const csv = [
    CERT_HEADER,
    certRow({cand_nash_conv_bb: EPS_CERT_BB + 0.001}),
  ].join('\n');
  const r = evaluateS2G2(csv);
  assert.equal(r.g2, false);
  assert.ok(r.failures.some((f) => f.startsWith('CERT')));
});

test('G2 fails when summed NashConv exceeds the QUAL bar (0.03)', () => {
  const csv = [
    CERT_HEADER,
    certRow({cand_nash_conv_bb: EPS_QUAL_BB + 0.01}),
  ].join('\n');
  const r = evaluateS2G2(csv);
  assert.equal(r.g2, false);
  assert.ok(r.failures.some((f) => f.startsWith('QUAL')));
});

test('G2 fails when the reference is not more converged than the candidate', () => {
  const csv = [
    CERT_HEADER,
    certRow({cand_nash_conv_bb: 0.001, ref_nash_conv_bb: 0.002}),
  ].join('\n');
  const r = evaluateS2G2(csv);
  assert.equal(r.g2, false);
  assert.ok(r.failures.some((f) => f.includes('reference NashConv')));
});

test('G2 fails when the reference NashConv itself is above the reference bar', () => {
  const csv = [
    CERT_HEADER,
    certRow({cand_nash_conv_bb: 0.02, ref_nash_conv_bb: 0.01}),
  ].join('\n');
  const r = evaluateS2G2(csv);
  assert.equal(r.g2, false);
  assert.ok(r.failures.some((f) => f.includes('reference NashConv')));
});

test('G2 does not gate per-combo e_max or drift RMS (they feed G3)', () => {
  // Large-pot/indifferent-combo value error is acceptable for G2; the G3 reach
  // envelope is what certifies it does not blow the production inequalities.
  const csv = [
    CERT_HEADER,
    certRow({drift_max_bb: 0.5, cert_max_bb: 0.5,
             drift_rms_bb: 0.05, cert_rms_bb: 0.05}),
  ].join('\n');
  const r = evaluateS2G2(csv);
  assert.equal(r.g2, true, r.failures.join('; '));
});

test('G2 rejects a cert bound below one of its components (integrity)', () => {
  const csv = [
    CERT_HEADER,
    certRow({drift_max_bb: 0.04, cert_max_bb: 0.02, cert_rms_bb: 0.02}),
  ].join('\n');
  const r = evaluateS2G2(csv);
  assert.equal(r.g2, false);
  assert.ok(r.failures.some((f) => f.includes('cert max below')));
});

test('G2 rejects an rms cert bound below one of its rms components', () => {
  const csv = [
    CERT_HEADER,
    certRow({drift_rms_bb: 0.02, cert_rms_bb: 0.005}),
  ].join('\n');
  const r = evaluateS2G2(csv);
  assert.equal(r.g2, false);
  assert.ok(r.failures.some((f) => f.includes('cert rms below')));
});

test('G2 rejects a duplicate (stratum,shape,group) row and a bad header', () => {
  const dup = [CERT_HEADER, certRow(), certRow()].join('\n');
  assert.equal(evaluateS2G2(dup).g2, false);
  assert.throws(() => parseCertCsv('a,b\n1,2\n'), GateInputError);
});

// --- G3 error-propagation slope -------------------------------------------

const G3_STRATA = [
  'w1', 'unpaired-rainbow-ahigh', 'paired', 'monotone',
  'unpaired-twotone', 'raised-srp',
];
const G3_LEVELS = [1e-6, 1e-5, 1e-4, 1e-3, 1e-2, 2e-2];
// Linear per-arm slope delta = slope * e_max; coh is the adversarial worst.
const G3_SLOPE: Record<string, number> = {
  zm: 0.05, coh: 0.5, pat: 0.15, act: 0.02,
};

function g3Row(stratum: string, arm: string, level: number,
               slope: number | null): string {
  if (slope === null)  // identity row
    return `${stratum},identity,0,0,0,0.001,0.0008,0`;
  const eMax = level;
  const delta = slope * level;
  const turnGap = 0.001 + delta;
  return `${stratum},${arm},${level.toExponential()},${eMax},`
    + `${(level * 0.8).toExponential()},${turnGap.toFixed(8)},0.0008,`
    + `${delta.toFixed(8)}`;
}

function g3Frame(): string {
  const lines = ['stratum,arm,level,e_max,e_l2,turn_gap_bb,river_term_bb,'
    + 'delta_hybrid_bb'];
  for (const s of G3_STRATA) {
    lines.push(g3Row(s, 'identity', 0, null));
    for (const arm of ['zm', 'coh', 'pat', 'act'])
      for (const lvl of G3_LEVELS)
        lines.push(g3Row(s, arm, lvl, G3_SLOPE[arm]!));
  }
  return lines.join('\n');
}

function g3Cert(certMaxBb: number, certRmsBb: number, potChips = 400): string {
  const lines = [CERT_HEADER];
  for (const s of G3_STRATA)
    lines.push(certRow({stratum: s, group: 0, pot_chips: potChips,
                        cert_max_bb: certMaxBb, cert_rms_bb: certRmsBb}));
  return lines.join('\n');
}

test('G3 operating points derive from shipped cert bounds over root pot', () => {
  const rows = parseCertCsv(
    [CERT_HEADER, certRow({pot_chips: 4, cert_max_bb: 0.0308,
                           cert_rms_bb: 0.0074})].join('\n'));
  assert.ok(Math.abs(operatingEMaxPotFraction(rows) - 0.0308 * 2 / 4) < 1e-12);
  assert.ok(Math.abs(operatingEL2PotFraction(rows) - 0.0074 * 2 / 4) < 1e-12);
});

test('G3 joint (e_max,e_l2) envelope passes on the real-drift ray', () => {
  // E = 0.0004*2/400 = 2e-6; L = 0.0002*2/400 = 1e-6 (act e_l2@E = 0.8*2e-6
  // = 1.6e-6 >= L, so the ray dominates the production point).
  const cert = g3Cert(0.0004, 0.0002, 400);
  const r = evaluateS2G3(g3Frame(), cert);
  assert.equal(r.rows, (1 + 4 * G3_LEVELS.length) * G3_STRATA.length);
  assert.ok(Math.abs(r.eMaxFrozenPotFraction - 2e-6) < 1e-15);
  assert.ok(Math.abs(r.eL2FrozenPotFraction - 1e-6) < 1e-15);
  assert.equal(r.perStratum.length, 6);
  assert.equal(r.rayDominatesProduction, true);
  for (const p of r.perStratum) {
    assert.ok(Math.abs(p.jointDeltaBb - 0.02 * 2e-6) < 1e-15);
    assert.ok(Math.abs(p.coherentDeltaBb - 0.5 * 2e-6) < 1e-15);
  }
  // Identical strata -> zero SD -> UCB equals the mean.
  assert.ok(Math.abs(r.jointDeltaUcbBb - 0.04e-6) < 1e-15);
  assert.ok(Math.abs(r.coherentUcbBb - 1e-6) < 1e-15);
  assert.ok(Math.abs(r.projectedHeadroomBb
    - (EPS_HYBRID_B0 - EPS_RIVER_R0 - 0.04e-6)) < 1e-12);
  assert.equal(r.g3, true);
  assert.deepEqual(r.failures, []);
});

test('G3 joint gate passes while the single-norm coherent bound is over budget',
     () => {
  // E = 0.0184: real act ray delta .02*.0184 = 3.68e-4 (pass), coherent
  // .5*.0184 = .0092 (recorded, over B0-R0, but NOT the gated coordinate).
  // L = 1.0*2/400 = 0.005 <= act e_l2@E = 0.8*.0184 = .01472.
  const cert = g3Cert(3.68, 1.0, 400);
  const r = evaluateS2G3(g3Frame(), cert);
  assert.ok(Math.abs(r.eMaxFrozenPotFraction - 0.0184) < 1e-12);
  assert.ok(r.coherentUcbBb > EPS_HYBRID_B0 - EPS_RIVER_R0);
  assert.ok(r.jointDeltaUcbBb < EPS_HYBRID_B0 - EPS_RIVER_R0);
  assert.equal(r.g3, true);
});

test('G3 fails closed when the operating e_max exceeds the top injected level',
     () => {
  // E = 1*2/4 = 0.5 >> 2e-2 top level; also L = 0.0002*2/4 = 1e-4.
  const cert = g3Cert(1.0, 0.0002, 4);
  const r = evaluateS2G3(g3Frame(), cert);
  assert.equal(r.g3, false);
  assert.ok(r.failures.some((f) => f.includes('exceeds the largest injected')));
});

test('G3 fails when the real-drift ray is less correlated than production',
     () => {
  // E tiny (2e-6) but L huge (1.0*2/400 = 0.005 > act e_l2@E 1.6e-6): the
  // real ray no longer dominates a shipped node, so a joint certificate is
  // refused rather than silently claimed.
  const cert = g3Cert(0.0004, 1.0, 400);
  const r = evaluateS2G3(g3Frame(), cert);
  assert.equal(r.g3, false);
  assert.equal(r.rayDominatesProduction, false);
  assert.ok(r.failures.some((f) => f.includes('does not dominate')));
});

test('G3 one-sided t constant is the df=5 value and missing strata fail', () => {
  assert.ok(Math.abs(T_CRIT_0_95_5 - 2.0150) < 1e-9);
  const one = [
    'stratum,arm,level,e_max,e_l2,turn_gap_bb,river_term_bb,delta_hybrid_bb',
    g3Row('w1', 'identity', 0, null),
    ...['zm', 'coh', 'pat', 'act'].flatMap((arm) =>
      G3_LEVELS.map((lvl) => g3Row('w1', arm, lvl, G3_SLOPE[arm]!))),
  ].join('\n');
  const r = evaluateS2G3(one, g3Cert(0.0004, 0.0002, 400));
  assert.equal(r.g3, false);
  assert.ok(r.failures.some((f) => f.includes("missing g3 stratum")));
  assert.throws(() => parseG3Csv('a,b\n1,2\n'), GateInputError);
});
