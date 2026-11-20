#!/usr/bin node
// D-390 Scenario A aggregator.
//
// Recomputes the other-river bucket and end-to-end BB/100 under Scenario A
// (L1/L2/L3 all served at cap-6) using the newly measured cap-6 ledger
// receipts, against the frozen D-382 empirical weights and f_other. Every
// cell is labeled measured vs projected. Same Uniform-arm convention and
// per-seat (/2) normalization as d382-other-bucket.mjs.
//
// Usage: node aggregate-scenario-a.mjs
import { readFileSync, writeFileSync } from 'node:fs';

const ROOT = new URL('../../../../', import.meta.url);
const TMP = new URL('.', import.meta.url);

// Frozen D-382 quantities (d382-other-bucket.csv), treated as fixed.
const F_OTHER = 692 / 2400;            // 0.2883
const W1_LOWER = 0.2216;               // D-380 sealed lower branch
const TARGET = 0.5;
const RAIL = 1.0;
const WEIGHTS = { L1: 0.0751, L2: 0.2962, L3: 0.6286 };
const LEDGER = {
  L1: { index: 1, pot: 8, receipt3k: 'l1-cap6-3000.csv', receipt20k: 'l1-cap6-20000.csv' },
  L2: { index: 2, pot: 10, receipt3k: 'l2-cap6-3000.csv', receipt20k: 'l2-cap6-20000.csv' },
  L3: { index: 3, pot: 16, receipt3k: 'l3-cap6-3000.csv', receipt20k: 'l3-cap6-20000.csv' },
};

// L2/L3 3k receipts live in the committed D-382 directory; L1 3k and all 20k
// receipts live in this tmp campaign directory.
const COMMITTED = new URL('docs/plans/ablation/d382-other-river-class/', ROOT);
const readRow = (base, name) => {
  let text;
  try {
    text = readFileSync(new URL(name, base), 'utf8');
  } catch {
    return null;  // receipt not produced yet
  }
  const line = text.trim().split('\n').find((l) => l.startsWith('6,uniform'));
  if (!line) return null;
  const c = line.split(',');
  return {
    cap: Number(c[0]), nc: Number(c[5]), eEmbed: Number(c[6]),
    pureMenu: Number(c[7]), wallMs: Number(c[10]), rssKb: Number(c[12]),
  };
};

const cells = [];
for (const [tag, meta] of Object.entries(LEDGER)) {
  const r3kTmp = readRow(TMP, meta.receipt3k);
  const r3kCommitted = tag === 'L1' ? null
    : readRow(COMMITTED, `rootcause-${tag.toLowerCase()}-cap6-3000-uniform.csv`);
  const r20k = readRow(TMP, meta.receipt20k);
  cells.push({
    tag, index: meta.index, pot: meta.pot, weight: WEIGHTS[tag],
    r3k: r3kTmp ?? r3kCommitted, r3kSource: r3kTmp ? 'tmp' : (r3kCommitted ? 'committed' : 'missing'),
    r20k,
  });
}

const out = [];
out.push('# D-390 Scenario A cap-6 measured receipts (Uniform arm)');
out.push('ledger,pot,weight,iter,source,online_nc,pure_menu,e_embed,wall_ms,rss_peak_kb');
for (const cell of cells) {
  for (const [iter, row, source] of [
    ['3000', cell.r3k, cell.r3kSource],
    ['20000', cell.r20k, cell.r20k ? 'tmp' : 'missing'],
  ]) {
    if (!row) {
      out.push(`${cell.tag},${cell.pot},${cell.weight},${iter},missing,,,,,`);
      continue;
    }
    out.push(`${cell.tag},${cell.pot},${cell.weight},${iter},${source},${row.nc.toFixed(8)},` +
      `${row.pureMenu.toFixed(8)},${row.eEmbed.toFixed(8)},${row.wallMs.toFixed(0)},${row.rssKb}`);
  }
}
out.push('');

const scenarios = [
  ['Scenario A @3000 (L1 measured; L2/L3 measured 3k)', 'r3k'],
  ['Scenario A @20000 (all three measured)', 'r20k'],
];
out.push('# Scenario A aggregation: other bucket = 100 * f_other * sum(w * e_embed/2)');
out.push(`# constants f_other=${F_OTHER.toFixed(6)} w1_lower=${W1_LOWER} target=${TARGET} rail=${RAIL}`);
out.push('scenario,cell_status,weighted_per_seat_bb,other_bucket_bb100,end_to_end_bb100,gap_to_0.5,vs_1.0_rail');
for (const [label, key] of scenarios) {
  const missing = cells.filter((c) => !c[key]);
  const status = missing.length
    ? `projected(missing:${missing.map((m) => m.tag).join('|')})`
    : 'measured';
  let perSeat = 0;
  for (const cell of cells) {
    const row = cell[key];
    if (row) perSeat += cell.weight * (row.eEmbed / 2);
    // Missing cells contribute nothing here; the row is labeled projected.
  }
  const bucket = 100 * F_OTHER * perSeat;
  const endToEnd = W1_LOWER + bucket;
  const gap = Math.max(0, endToEnd - TARGET);
  out.push(`${label},${status},${perSeat.toFixed(6)},${bucket.toFixed(4)},${endToEnd.toFixed(4)},` +
    `${gap.toFixed(4)},${endToEnd <= RAIL ? 'PASS' : 'FAIL'}`);
}
out.push('');

// Projected reference from the memo (L1 by L2's measured 3k ratio) for audit.
out.push('# Memo reference (projection, not measured): other=0.5328 end_to_end=0.7544');
out.push('# D-380 two rails (must stay separate): certification <=0.002 BB per decision;');
out.push('# online serving <=1.0 BB/100 (0.5 keep target). Cap-6 3k e_embed stays >0.002,');
out.push('# so the certification rail is unaffected regardless of the serving-rail result.');

const text = out.join('\n') + '\n';
process.stdout.write(text);
writeFileSync(new URL('scenario-a-summary.csv', TMP), text);
