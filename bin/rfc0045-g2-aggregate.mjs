#!/usr/bin/env node
// Aggregate RFC 0045 G2 campaign per-cell rows into one CSV and run the
// frozen evaluator. Usage:
//   node bin/rfc0045-g2-aggregate.mjs [ckpt-dir] [--json]
//
// The work-stealing driver writes one `<dir>/rows/cell-N.row` per finished
// cell (plus a `.done` marker); cells without a .row are simply absent.
import {readdirSync, readFileSync, writeFileSync, mkdirSync} from 'node:fs';
import {existsSync} from 'node:fs';

const target = new URL('../dist/tools/rfc/rfc0045-gates.js', import.meta.url);
if (!existsSync(target)) {
  console.error('Run npm run build:ts first.');
  process.exit(1);
}
const gates = await import(target.href);

const args = process.argv.slice(2).filter((a) => a !== '--json');
const asJson = process.argv.includes('--json');
// Diagnostics only: permit an interim verdict on an incomplete campaign.
// Without it the aggregator fails closed (Stage-4 addendum finding N4):
// no verdict is printed and the process exits non-zero until done=368/368
// with zero walkslow rows, so an interim aggregate can never be mistaken
// for the verdict of record.
const allowPartial = process.argv.includes('--partial');
const dir = args.filter((a) => a !== '--partial')[0] ?? 'rfc45-run2';
const rowsDir = `${dir}/rows`;
const files = readdirSync(rowsDir).filter((f) => f.endsWith('.row')).sort(
  (a, b) => parseInt(a.slice(5), 10) - parseInt(b.slice(5), 10),
);
const rows = files.flatMap((f) => gates.loadG2Csv(`${rowsDir}/${f}`));

mkdirSync('docs/plans/ablation', {recursive: true});
const header = 'board,range,scheme,kf,kt,kr,seed,iterations,infosets,joint_deals,sum_gap_bb,'
  + 'bucket_sum_gap_bb,train_s,exact_walk_s,truncated,completed,note';
const out = [header];
for (const f of files) {
  const lines = readFileSync(`${rowsDir}/${f}`, 'utf8').split('\n').map((l) => l.trim());
  for (const l of lines) {
    if (!l || l === header)
      continue;
    out.push(l);
  }
}
writeFileSync('docs/plans/ablation/rfc0045-g2-campaign.csv', out.join('\n') + '\n');

const doneCount = readdirSync(rowsDir).filter((f) => f.endsWith('.done')).length;
console.log(`cells done=${doneCount}/368 final_rows=${rows.length} -> docs/plans/ablation/rfc0045-g2-campaign.csv`);
// Frozen addendum section 6: full walks outside 50.996 s +/-20% must be
// re-run. The driver retries in-cell with backoff and only leaves the
// walkslow note when every attempt failed; the aggregate must not pass.
const slow = rows.filter((r) => r.note === 'walkslow');
if (slow.length > 0) {
  console.error(`G2: ${slow.length} cell(s) breached the exact-walk band and need a `
    + `prefix-resume re-run (note=walkslow):`);
  for (const r of slow)
    console.error(`  walkslow ${r.board} ${r.range} ${r.scheme} seed=${r.seed} `
      + `T=${r.iterations} walk=${r.exact_walk_s}s`);
  if (!allowPartial)
    process.exit(1);
}
const complete = doneCount === 368 && rows.length === 368 && slow.length === 0;
if (!complete && !allowPartial) {
  console.error(`G2: campaign incomplete (done=${doneCount}/368, final_rows=${rows.length}, `
    + `walkslow=${slow.length}); CSV written for monitoring but NO verdict is emitted. `
    + `Pass --partial for a diagnostic interim verdict only.`);
  process.exit(1);
}
if (rows.length > 0) {
  if (!complete)
    console.error('G2: DIAGNOSTIC interim verdict on an INCOMPLETE campaign; not of record.');
  const verdict = gates.selectInstrument(rows);
  if (asJson) {
    console.log(JSON.stringify(verdict,
      (k, v) => (v instanceof Map ? Object.fromEntries(v) : v), 2));
  } else if (verdict.kind === 'uniform-ehs-viable') {
    console.log(`G2: uniform-ehs-viable selected=${verdict.selected} `
      + `worst=${verdict.gate.worstBoard} exact=${verdict.gate.worstExact.toFixed(6)}`);
  } else if (verdict.kind === 'needs-conditional-kt128') {
    console.log('G2: primary axis failed at all rungs so far (conditional arm not run yet).');
    console.log(`  last reasons=[${verdict.gate.reasons.join(';')}]`);
  } else {
    console.log('G2: instrument-inadequate-uniform-ehs');
    for (const g of verdict.gates)
      console.log(`  worst=${g.worstBoard} exact=${g.worstExact.toFixed(6)} `
        + `reasons=[${g.reasons.join(';')}]`);
  }
}
