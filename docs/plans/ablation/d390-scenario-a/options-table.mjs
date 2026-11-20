#!/usr/bin node
// D-390 milestone: three-direction options table calculator.
//
// Builds every serving option by deterministic arithmetic on MEASURED cap-4
// and cap-6 20,000-iteration receipts; no new measurement, no projection of
// severity. Cost cells sum measured per-ledger walls (cap-4 receipts were
// taken at 28 threads, cap-6 at 18 threads; the mixed-thread caveat is carried
// into the decision page). Frozen D-380/D-382 inputs: empirical weights over
// the 692 pooled "other" hands, f_other, and the 0.2216 BB/100 check-to-river
// lower branch. Output is the committed options table data.
//
// Usage: node options-table.mjs
import { readFileSync, writeFileSync } from 'node:fs';

const ROOT = new URL('../../../../', import.meta.url);
const D390 = new URL('.', import.meta.url);
const D382 = new URL('../d382-other-river-class/', import.meta.url);
const GATE2 = new URL('../rfc0049/gate2/', import.meta.url);

const F_OTHER = 692 / 2400;            // 0.288333
const W1_LOWER = 0.2216;               // D-380 sealed measured-class branch
const TARGET = 0.5;
const RAIL = 1.0;
const CLASSES = 134459;
const WEIGHTS = { L1: 0.0751, L2: 0.2962, L3: 0.6286 };

const readUniform = (base, name) => {
  const line = readFileSync(new URL(name, base), 'utf8').trim().split('\n')
    .find((l) => l.startsWith('4,uniform') || l.startsWith('6,uniform'));
  const c = line.split(',');
  return { nc: Number(c[5]), eEmbed: Number(c[6]), pureMenu: Number(c[7]),
           wallMs: Number(c[10]), rssKb: Number(c[12]) };
};

const ledgers = ['L1', 'L2', 'L3'];
const cap4File = { L1: 'd382-limped-ledger1-pot8-cap4-20000.csv',
                   L2: 'd382-limped-ledger2-pot10-cap4-20000.csv',
                   L3: 'd382-limped-ledger3-pot16-cap4-20000.csv' };
const cap6File = { L1: 'l1-cap6-20000.csv', L2: 'l2-cap6-20000.csv', L3: 'l3-cap6-20000.csv' };
const data = {};
for (const lg of ledgers) {
  data[lg] = { cap4: readUniform(D382, cap4File[lg]), cap6: readUniform(D390, cap6File[lg]) };
}
// Common check-to-river component: w1 cap-4 20k measured wall (quality enters
// via the frozen W1_LOWER constant, not re-derived here).
const w1Cap4WallMs = readUniform(GATE2, 'd370-w1-identity-cap4-embed-20000.csv').wallMs;

const bucketOf = (choice) => {
  let perSeat = 0, structural = 0, convergence = 0, wallS = 0;
  const detail = [];
  for (const lg of ledgers) {
    const r = data[lg][choice[lg]];
    const w = WEIGHTS[lg];
    const cPerSeat = 100 * F_OTHER * w * (r.pureMenu / 2);
    const nPerSeat = 100 * F_OTHER * w * (r.nc / 2);
    structural += cPerSeat;
    convergence += nPerSeat;
    perSeat += w * (r.eEmbed / 2);
    wallS += r.wallMs / 1000;
    detail.push({ ledger: lg, cap: choice[lg], e: r.eEmbed, pure: r.pureMenu, nc: r.nc,
                  structuralBb100: cPerSeat, convergenceBb100: nPerSeat, wallH: r.wallMs / 3600000 });
  }
  const other = 100 * F_OTHER * perSeat;
  return { detail, other, structural, convergence, endToEnd: W1_LOWER + other,
           aggressiveWallH: wallS / 3600,
           perClassWallH: wallS / 3600 + w1Cap4WallMs / 3600000 };
};

const options = [
  ['full-cap6', { L1: 'cap6', L2: 'cap6', L3: 'cap6' }],
  ['narrow-b-L2L3-cap6', { L1: 'cap4', L2: 'cap6', L3: 'cap6' }],
  ['narrow-a-L3-cap6', { L1: 'cap4', L2: 'cap4', L3: 'cap6' }],
  ['baseline-all-cap4', { L1: 'cap4', L2: 'cap4', L3: 'cap4' }],
];

const out = ['# D-390 options table (all severity cells measured 20k Uniform; costs are measured-wall sums)',
  `# constants f_other=${F_OTHER.toFixed(6)} w1_lower=${W1_LOWER} target=${TARGET} rail=${RAIL} classes=${CLASSES}`,
  `# w1 cap-4 common wall per class ${(w1Cap4WallMs / 3600000).toFixed(4)} h (28 threads)`,
  'option,ledger,cap,e_embed,pure_menu,bounded_nc,structural_bb100,convergence_bb100,wall_h,label'];
const summary = ['', '# option summary',
  'option,other_bucket_bb100,structural_bb100,convergence_bb100,end_to_end_bb100,le_0.5,le_1.0,' +
  'aggressive_wall_h_per_class,total_wall_h_per_class,campaign_machine_h,days_100slots,savings_vs_full_pct,label'];
const full = bucketOf(options[0][1]);
for (const [name, choice] of options) {
  const r = bucketOf(choice);
  for (const d of r.detail)
    out.push(`${name},${d.ledger},${d.cap},${d.e.toFixed(8)},${d.pure.toFixed(8)},${d.nc.toFixed(8)},` +
      `${d.structuralBb100.toFixed(4)},${d.convergenceBb100.toFixed(4)},${d.wallH.toFixed(4)},measured`);
  const campaign = r.perClassWallH * CLASSES;
  const savings = 100 * (1 - r.aggressiveWallH / full.aggressiveWallH);
  summary.push(`${name},${r.other.toFixed(4)},${r.structural.toFixed(4)},${r.convergence.toFixed(4)},` +
    `${r.endToEnd.toFixed(4)},${r.endToEnd <= TARGET ? 'PASS' : 'FAIL'},${r.endToEnd <= RAIL ? 'PASS' : 'FAIL'},` +
    `${r.aggressiveWallH.toFixed(4)},${r.perClassWallH.toFixed(4)},${campaign.toFixed(0)},` +
    `${(campaign / 100 / 24).toFixed(1)},${savings.toFixed(1)},measured-components`);
}
const text = out.concat(summary).join('\n') + '\n';
process.stdout.write(text);
writeFileSync(new URL('./options-table.csv', D390), text);
