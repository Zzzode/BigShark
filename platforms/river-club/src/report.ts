#!/usr/bin/env node
// report.mjs — one-line session report: total bankroll vs baseline, table,
// action mix and failures. Safe for a periodic monitor.
import { spawnSync } from 'node:child_process';
import { existsSync, readFileSync } from 'node:fs';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

import { parseRiverState } from './json.js';
import {
  isRiverRoom,
  type Action,
  type ExecutableDecision,
} from './types.js';

const here = dirname(fileURLToPath(import.meta.url));
const root = resolve(here, '../../../..');
const cli = (args: string[]) => spawnSync(
  process.execPath,
  [join(here, 'journal-cli.js'), ...args],
  { encoding: 'utf8' },
);

let baseline: number | null = null;
let since = 0;
const bf = join(root, '.runtime', 'baseline.json');
if (existsSync(bf)) {
  try {
    const value = JSON.parse(readFileSync(bf, 'utf8')) as {
      baseline?: number;
      at?: number;
    };
    baseline = value.baseline ?? null;
    since = value.at ?? 0;
  } catch { /* */ }
}

let total: number | null = null;
let table = '?';
try {
  const s = parseRiverState(cli(['state']).stdout);
  const r = s.room;
  if (s.me?.wallet !== undefined) {
    total = s.me.wallet + (isRiverRoom(r) ? r.hero.stack : 0);
  }
  table = isRiverRoom(r) ? `${r.name} ${r.street} st=${r.hero.stack}` : 'offtable';
} catch { /* */ }

interface ResultEntry {
  ts?: number;
  source?: string;
  hand?: string;
  ok?: boolean;
  decision?: ExecutableDecision;
}

const mix: Partial<Record<Action, number>> = {};
let fail = 0, modelN = 0, engineN = 0, actedHands = new Set();
const rf = join(root, '.runtime', 'results.log');
if (existsSync(rf)) {
  for (const line of readFileSync(rf, 'utf8').split('\n')) {
    if (!line.trim()) continue;
    let d: ResultEntry;
    try { d = JSON.parse(line) as ResultEntry; } catch { continue; }
    if (d.decision?.action && (d.ts ?? 0) >= since) {
      mix[d.decision.action] = (mix[d.decision.action] ?? 0) + 1;
      if (d.source === 'model') modelN++; else engineN++;
      if (d.hand) actedHands.add(d.hand);
      if (d.ok === false) fail++;
    }
  }
}

const pnl: number | '?' = total !== null && baseline !== null ? total - baseline : '?';
const ts = new Date().toTimeString().slice(0, 5);
console.log(`${ts} | total=${total ?? '?'} baseline=${baseline ?? '?'} PnL=${pnl} (${typeof pnl === 'number' ? (pnl / 20).toFixed(1) : '?'}BB) | ${table} | acts=${JSON.stringify(mix)} engine=${engineN} model=${modelN} fail=${fail}`);
