#!/usr/bin node
// D-346 campaign metadata extractor.
//
// Pulls the engine process accounting line ("Engine tier: ... decisions
// served, N fallbacks") out of each retained practice transcript and pairs
// it with a wall-time ledger, emitting one CSV row per segment. These are
// the receipts behind the fallback-rate statements in the decision doc.
//
// Usage:
//   node extract-meta.mjs <label>=<rawLogPath> [<label>=...] \
//        --timing <ledgerPath> --out <meta.csv>
//
// Ledger lines look like: "station seed401 wall=673.9448" or
// "station 400 seed401 wall=633.3449"; matching is by policy token.
import { readFileSync, writeFileSync } from 'node:fs';

const args = process.argv.slice(2);
const inputs = [];
let timingPath = '';
let outPath = '';
for (let i = 0; i < args.length; ++i) {
  if (args[i] === '--timing') timingPath = args[++i];
  else if (args[i] === '--out') outPath = args[++i];
  else inputs.push(args[i]);
}
if (!inputs.length || !outPath) {
  process.stderr.write('bad arguments\n');
  process.exit(2);
}

const wallBySeed = new Map();
if (timingPath) {
  for (const line of readFileSync(timingPath, 'utf8').split('\n')) {
    const m = line.match(/seed(\d+)\s+wall=([0-9.]+)/);
    if (m) wallBySeed.set(Number(m[1]), Number(m[2]));
  }
}

const rows = [
  'run,log,seed,hands,served,fallbacks,fallback_rate,wall_s',
];
for (const input of inputs) {
  const eq = input.indexOf('=');
  const label = input.slice(0, eq);
  const logPath = input.slice(eq + 1);
  const text = readFileSync(logPath, 'utf8');
  const tier = text.match(/(\d+) decisions served, (\d+) fallbacks/);
  const hands = Number((text.match(/^=== Hand #(\d+)/gm) ?? []).pop()?.replace(/\D/g, '') ?? 0);
  const seed = Number((logPath.match(/seed(\d+)/) ?? [])[1] ?? 0);
  if (!tier) throw new Error(`no engine tier line in ${logPath}`);
  const served = Number(tier[1]);
  const fallbacks = Number(tier[2]);
  const wall = wallBySeed.get(seed);
  rows.push([
    label, logPath.split('/').pop(), seed, hands, served, fallbacks,
    (fallbacks / (served + fallbacks)).toFixed(4),
    wall === undefined ? '' : wall.toFixed(1),
  ].join(','));
}
writeFileSync(outPath, rows.join('\n') + '\n');
process.stdout.write(rows.join('\n') + '\n');
