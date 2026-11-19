#!/usr/bin node
// D-380 cap-3/cap-4 table-level expectation calculator.
//
// Reuses the D-346 reach-frequency machine unchanged: the pooled 2,400-hand
// passive frequencies from both-summary.csv (six runs: station, folder,
// passive and their r2 replications) and the identity
//   per-hand loss = f_limp * per-seat per-occurrence loss
// (pooled passive has zero SRP check-to-river hands by construction),
//   BB/100 = 100 * per-hand loss.
// Severity inputs are the D-370/D-371 cap-3/cap-4 w1 embed receipts, halved
// to per-engine-seat values exactly as table-expectation.mjs halves the
// two-seat T6 values. No engine code or sealed receipt is modified; this is
// offline analysis tooling, not a test and not part of the engine build.
//
// Usage: node cap34-table-expectation.mjs
import { writeFileSync } from 'node:fs';

// Sealed pooled frequencies (counts over 2,400 dealt hands; both-summary.csv,
// the same constants table-expectation.mjs pools via --pool).
const HANDS = 2400;
const F = {
  limp_ctr: 134 / HANDS,       // 0.0558333
  srp_ctr: 0 / HANDS,          // construction property of passive scripts
  other_river: 692 / HANDS,    // 0.2883333
  allin_runout: 354 / HANDS,   // 0.1475000, structural-zero menu residual
};
const se = (p) => Math.sqrt((p * (1 - p)) / HANDS);

// D-370/D-371 w1 anchor receipts, two-seat summed BB (gate2 d370/d371 CSVs).
// per-seat = /2, matching the one-engine-seat practice-table convention.
const builds = [
  { tag: 'cap4-20k', e_uniform: 0.07938816, e_brfav: 4.72611640 },
  { tag: 'cap4-6k', e_uniform: 0.07802624, e_brfav: 4.76210834 },
  { tag: 'cap3-20k', e_uniform: 0.37750969, e_brfav: 15.85009114 },
];

// D-346 reference points kept for traceability (per engine seat).
const CAP1_T6U_PER_SEAT = 9.8861;   // five-limped-stratum mean
const CAP1_UNION_PER_SEAT = 15.62244834; // 31.24489667 / 2, w1 union

const rows = [
  'build,arm,branch,per_occurrence_per_seat_bb,per_hand_bb,bb_per_100,se_bb_per_100',
];
const push = (b, arm, branch, s) => {
  const perHand = F.limp_ctr * s; // fSrp = 0 in the pooled passive corpus
  const perHandSe = se(F.limp_ctr) * s;
  rows.push([b, arm, branch, s.toFixed(8), perHand.toFixed(6),
             (100 * perHand).toFixed(4), (100 * perHandSe).toFixed(4)].join(','));
};
for (const b of builds) {
  const sU = b.e_uniform / 2;
  const sB = b.e_brfav / 2;
  // Lower branch: only limp_ctr charged; other class contributes zero,
  // identical discipline to cap1-table-level-expectation.md section 5.1.
  push(b.tag, 'uniform', 'lower_other_zero', sU);
  push(b.tag, 'br_favorable', 'lower_other_zero', sB);
  // Proxy upper band: the unmeasured other-river class (28.83%) is charged
  // the SAME w1 per-seat severity. No pot-8/aggressive-line cap-3/4 receipt
  // exists, so this is a labeled proxy, not a measurement.
  const proxyPerHandU = (F.limp_ctr + F.other_river) * sU;
  const proxySeU = Math.hypot(se(F.limp_ctr), se(F.other_river)) * sU;
  rows.push([b.tag, 'uniform', 'upper_other_eq_w1_proxy', sU.toFixed(8),
             proxyPerHandU.toFixed(6), (100 * proxyPerHandU).toFixed(4),
             (100 * proxySeU).toFixed(4)].join(','));
}

// D-380 crude factor under audit: 55.20 / 31.19 applied to two-seat e_embed.
const crudeFactor = 55.20 / 31.19;
const audit = [
  '',
  '# crude-factor audit (not deliverable numbers)',
  'build,two_seat_e_embed,crude_bb_per_100,precise_lower_bb_per_100,ratio_precise_over_crude',
];
for (const b of builds) {
  const crude = crudeFactor * b.e_uniform;
  const precise = 100 * F.limp_ctr * (b.e_uniform / 2);
  audit.push([b.tag, b.e_uniform.toFixed(8), crude.toFixed(4), precise.toFixed(4),
              (precise / crude).toFixed(4)].join(','));
}
audit.push('', `# reference: cap-1 sealed table-level 55.1974 BB/100`,
           `# cap-1 T6U per-seat ${CAP1_T6U_PER_SEAT}, union per-seat ${CAP1_UNION_PER_SEAT}`,
           `# frequencies: limp ${F.limp_ctr.toFixed(6)} other ${F.other_river.toFixed(6)} ` +
           `allin ${F.allin_runout.toFixed(6)} srp ${F.srp_ctr.toFixed(6)}`);

const out = rows.join('\n') + '\n' + audit.join('\n') + '\n';
process.stdout.write(out);
writeFileSync(new URL('./cap34-table-expectation.csv', import.meta.url), out);
