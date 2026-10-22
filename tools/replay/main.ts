#!/usr/bin/env node
// Offline replay only. Synthetic engineering fixtures never count as play history.
import { existsSync, readFileSync, readdirSync } from 'node:fs';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { closeEngine, decide } from '../../platforms/river-club/src/engine.js';
import type { Action, RiverState } from '../../platforms/river-club/src/types.js';
import { collectReplaySnapshots, decisionProblem, replayExitCode, type ReplaySource } from './journal.js';

const here = dirname(fileURLToPath(import.meta.url));
const root = resolve(here, '../../..');
const args = process.argv.slice(2);
if (args.some(arg => arg !== '--synthetic')) throw new Error('usage: node bin/replay.mjs [--synthetic]');
const source: ReplaySource = args.includes('--synthetic') ? 'synthetic' : 'historical';
let journals: { name: string; text: string }[];
if (source === 'synthetic') {
  const fixtures = JSON.parse(readFileSync(join(root, 'tools/replay/fixtures/synthetic-rooms.json'), 'utf8')) as { state: RiverState }[];
  journals = [{ name: 'fixed-synthetic-engineering-fixtures', text: fixtures.map(f => JSON.stringify({ resp: f.state })).join('\n') }];
} else {
  const directory = join(root, 'sessions');
  journals = existsSync(directory) ? readdirSync(directory).filter(f => f.endsWith('.jsonl')).sort().map(name => ({ name, text: readFileSync(join(directory, name), 'utf8') })) : [];
}
const { snapshots, stats } = collectReplaySnapshots(journals, source);
console.log(`Replay source: ${source} | Snapshots: ${snapshots.length} | Malformed: ${stats.malformed} | Skipped: ${stats.skipped} | Duplicates: ${stats.duplicates}`);
if (source === 'synthetic') console.log('Historical coverage: NONE (fixed synthetic engineering corpus only)');
else if (snapshots.length === 0) console.log('Historical coverage: NONE (no usable historical snapshots)');

let n = 0, fallback = 0, illegal = 0, totalMs = 0, maxMs = 0, turnUnserved = 0;
const mix: Partial<Record<Action, number>> = {};
const samples: string[] = [];
try {
  for (const snapshot of snapshots) {
    const room = snapshot.room;
    const config = { style: 'tag', heroName: snapshot.heroName, timeoutMs: 10_000 };
    const t0 = process.hrtime.bigint();
    const decision = await decide(room, config);
    const ms = Number(process.hrtime.bigint() - t0) / 1e6;
    totalMs += ms; maxMs = Math.max(maxMs, ms);
    if (decision.reason?.startsWith('safe-fallback')) fallback++;
    // RFC 0032 stage 3: the adapter prefixes engine reasons with `cpp:`, so the
    // counter matches on suffix rather than on the bare token.
    if (decision.reason?.endsWith('turn_root_unserved')) turnUnserved++;
    const problem = decisionProblem(room, decision);
    if (problem) {
      illegal++;
      if (illegal <= 5) console.log(`FAIL ${snapshot.file}:${snapshot.line}: ${problem}`);
      continue;
    }
    n++;
    mix[decision.action] = (mix[decision.action] ?? 0) + 1;
    if (samples.length < 10 && (decision.action === 'raise' || decision.action === 'bet')) samples.push(`${room.street} ${room.solver?.heroPosition ?? ''} ${decision.action} ${decision.amount}`);
  }
} finally { closeEngine(); }
console.log(`Decisions: ${n} | Illegal: ${illegal} | JS fallbacks: ${fallback} | Turn/River roots unserved: ${turnUnserved} | Average ${(totalMs / Math.max(1, snapshots.length)).toFixed(2)}ms | Maximum ${maxMs.toFixed(1)}ms`);
console.log('Action mix:', JSON.stringify(mix));
console.log('Bet and raise samples:'); samples.forEach(s => console.log('  ', s));
process.exit(replayExitCode(illegal, fallback));
