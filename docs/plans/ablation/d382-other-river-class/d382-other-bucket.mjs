#!/usr/bin node
// D-382 other-river-class BB/100 calculator.
//
// Closes the D-380 proxy band with measured cap-4 per-occurrence severity on
// the betting ledgers that the 692 pooled "other" hands actually reach.
// Inputs are all existing committed evidence plus the four D-382 bench
// receipts (Uniform + BrFavorable rows each):
//   srp-ledger-0 (pot 8, zero passive-corpus weight, required cross-check),
//   limped-ledger-1/2/3 (pots 8/10/16, the aggressive-line family).
// Each observed practice river pot is mapped to its nearest measured limped
// ledger; pots above 16 have no frozen ledger and are disclosed, not hidden.
// Frequency uncertainty is binomial on the 2,400-hand pool; severities are
// treated as fixed, the same convention as table-expectation.mjs.
//
// Usage: node d382-other-bucket.mjs
import { readFileSync, writeFileSync } from 'node:fs';

const DIR = new URL('.', import.meta.url);
const read = (p) => readFileSync(new URL(p, DIR), 'utf8').trim().split('\n').slice(1)
  .map((line) => line.split(','));

// --- 1. measured cap-4 severities (two-seat summed BB), keyed by ledger ---
const receipts = {
  A_srp0_pot8: 'd382-srp-ledger0-pot8-cap4-20000.csv',
  L1_limped1_pot8: 'd382-limped-ledger1-pot8-cap4-20000.csv',
  L2_limped2_pot10: 'd382-limped-ledger2-pot10-cap4-20000.csv',
  L3_limped3_pot16: 'd382-limped-ledger3-pot16-cap4-20000.csv',
};
const ledgerPot = { L1_limped1_pot8: 8, L2_limped2_pot10: 10, L3_limped3_pot16: 16 };
const sev = {};
for (const [tag, path] of Object.entries(receipts)) {
  for (const row of read(path)) {
    const [cap, arm, , , , nash, eEmbed, pure] = row;
    if (cap !== '4') continue;
    (sev[tag] ??= {})[arm] = { e: Number(eEmbed), pure: Number(pure), nc: Number(nash) };
  }
}

// --- 2. empirical pot distribution of the 692 pooled other hands ---
const passive = new Set(['station', 'folder', 'passive', 'r2-station', 'r2-folder', 'r2-passive']);
const potCounts = new Map();
let otherHands = 0;
for (const f of ['../cap1-table-level/mounted-rows.csv', '../cap1-table-level/r2-rows.csv']) {
  for (const line of read(f)) {
    if (!passive.has(line[0]) || line[13] !== 'other') continue;
    const pot = Number(line[9]);
    potCounts.set(pot, (potCounts.get(pot) ?? 0) + 1);
    otherHands++;
  }
}
if (otherHands !== 692) throw new Error(`expected 692 other hands, got ${otherHands}`);

// Nearest measured limped ledger; ties resolve to the smaller pot (the
// higher per-pot-fraction, conservative side).
const measuredPots = [8, 10, 16];
const nearest = (pot) => measuredPots.reduce((best, p) =>
  Math.abs(p - pot) < Math.abs(best - pot) ? p : best, measuredPots[0]);
const tagForPot = { 8: 'L1_limped1_pot8', 10: 'L2_limped2_pot10', 16: 'L3_limped3_pot16' };
const weights = { L1_limped1_pot8: 0, L2_limped2_pot10: 0, L3_limped3_pot16: 0 };
const mappingRows = ['practice_pot,hands,share,mapped_ledger_pot'];
for (const [pot, k] of [...potCounts].sort((a, b) => a[0] - b[0])) {
  const mp = nearest(pot);
  weights[tagForPot[mp]] += k;
  mappingRows.push(`${pot},${k},${(k / otherHands).toFixed(4)},${mp}`);
}
const w = Object.fromEntries(Object.entries(weights).map(([k, v]) => [k, v / otherHands]));

// --- 3. bucket expectation ---
const HANDS = 2400;
const fOther = otherHands / HANDS;
const seFreq = Math.sqrt((fOther * (1 - fOther)) / HANDS);
const perSeat = (tag, arm) => sev[tag][arm].e / 2;
const weightedSeverity = (arm) =>
  Object.entries(w).reduce((acc, [tag, wj]) => acc + wj * perSeat(tag, arm), 0);
const sUniform = weightedSeverity('uniform');
const sBrfav = weightedSeverity('br_favorable');
const bucket = (s) => 100 * fOther * s;
const bucketSe = (s) => 100 * seFreq * s;
const ci = (x, se) => [x - 1.96 * se, x + 1.96 * se];

// Fixed-weight alternatives to bracket the ledger-mapping choice.
const altRows = [];
for (const arm of ['uniform', 'br_favorable']) {
  const perSeatVals = Object.keys(weights).map((t) => perSeat(t, arm));
  altRows.push([arm, 'min-ledger', Math.min(...perSeatVals), bucket(Math.min(...perSeatVals))]);
  altRows.push([arm, 'max-ledger', Math.max(...perSeatVals), bucket(Math.max(...perSeatVals))]);
}

// --- 4. output ---
const out = [];
out.push('# D-382 other-bucket weights (shares of 692 pooled other hands)');
for (const [tag, wj] of Object.entries(w))
  out.push(`weight,${tag},pot${ledgerPot[tag]},${wj.toFixed(4)},hands=${weights[tag]}`);
out.push('', ...mappingRows, '');
out.push('arm,basis,per_seat_severity_bb,other_bucket_bb_per_100,ci_low,ci_high');
for (const [arm, s] of [['uniform', sUniform], ['br_favorable', sBrfav]]) {
  const b = bucket(s);
  const [lo, hi] = ci(b, bucketSe(s));
  out.push(`${arm},empirical_weighted,${s.toFixed(6)},${b.toFixed(4)},${lo.toFixed(4)},${hi.toFixed(4)}`);
}
for (const [arm, basis, s, b] of altRows)
  out.push(`${arm},${basis}_fixed_weight,${s.toFixed(6)},${b.toFixed(4)},,`);
out.push('');
const otherUniform = bucket(sUniform);
const [, ciHi] = ci(otherUniform, bucketSe(sUniform));
const w1Lower = 0.2216; // D-380 sealed lower branch, unchanged
out.push(`# total table-level (D-380 lower branch 0.2216 + other bucket)`);
out.push(`total_uniform_bb_per_100,${(w1Lower + otherUniform).toFixed(4)}`);
out.push(`total_uniform_ci_high,${(w1Lower + ciHi).toFixed(4)}`);
out.push(`other_brfav_bb_per_100,${bucket(sBrfav).toFixed(4)}`);
out.push(`f_other,${fOther.toFixed(6)},se,${seFreq.toFixed(6)}`);
out.push('');
out.push('# D-382 pre-registered judgment (Uniform arm)');
out.push(`rule_point_le_1.0,${otherUniform <= 1.0 ? 'PASS' : 'FAIL'},point=${otherUniform.toFixed(4)}`);
out.push(`rule_ci_high_le_1.0,${ciHi <= 1.0 ? 'PASS' : 'FAIL'},ci_high=${ciHi.toFixed(4)}`);
out.push(`rule_point_le_0.5_keep_threshold,${otherUniform <= 0.5 ? 'PASS' : 'FAIL'}`);
out.push('');
out.push('# SRP anchor cross-check (zero passive-corpus weight; not in bucket)');
for (const arm of ['uniform', 'br_favorable'])
  out.push(`srp0_pot8,${arm},e_embed=${sev.A_srp0_pot8[arm].e.toFixed(8)},per_seat=${perSeat('A_srp0_pot8', arm).toFixed(6)}`);

const text = out.join('\n') + '\n';
process.stdout.write(text);
writeFileSync(new URL('./d382-other-bucket.csv', DIR), text);
