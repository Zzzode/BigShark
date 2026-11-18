#!/usr/bin node
// D-346 table-level expectation campaign driver.
//
// Drives the existing bigshark-practice binary over its stdin/stdout prompt
// protocol with one of five seat-0 scripted policies while the bot seat is
// served by the real bigshark-engine process. It changes no strategy code
// and launches no training: every bot decision is an ordinary engine serve
// under the practice table's offline budget.
//
// Usage:
//   node drive-practice.mjs <station|folder|uniform|passive-random|srp-passive>
//        <hands> <seed> <solveBudgetMs> <rawLogPath> [extra practice args...]
//
// Policies for the human seat (seat 0), chosen from the printed Legal menu:
//   station        check when legal, otherwise call (never folds, never raises)
//   folder         check when legal, otherwise fold (never puts money in
//                  voluntarily)
//   uniform        uniform pick over all numbered legal actions, mulberry32
//   passive-random uniform over fold/check/call only (never aggressive)
//   srp-passive  preflop: smallest available raise/bet once; postflop:
//                uniform over fold/check/call only. Targets the single-raised
//                check-to-river root, which passive scripts never reach (the
//                bot's first preflop decision is a provenance-gate fallback).
//
// The full practice transcript is teed to rawLogPath so every statistic is
// re-derivable from the receipt with ordinary text processing. A sidecar
// <rawLogPath>.events.log records one line per prompt with the legal menu
// and the chosen reply, giving an exact machine-readable action record.
import { spawn } from 'node:child_process';
import { createWriteStream } from 'node:fs';

const [policy, handsArg, seedArg, budgetArg, logPath, ...extra] = process.argv.slice(2);
const policies = ['station', 'folder', 'uniform', 'passive-random', 'srp-passive'];
if (!policies.includes(policy) ||
    !handsArg || !seedArg || !budgetArg || !logPath) {
  process.stderr.write('bad arguments\n');
  process.exit(2);
}

function mulberry32(seed) {
  let a = seed >>> 0;
  return () => {
    a |= 0;
    a = (a + 0x6d2b79f5) | 0;
    let t = Math.imul(a ^ (a >>> 15), 1 | a);
    t = (t + Math.imul(t ^ (t >>> 7), 61 | t)) ^ t;
    return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
  };
}
const rng = mulberry32(Number(seedArg));

const args = [
  '--seats', '2', '--stack-bb', '100',
  '--hands', handsArg, '--seed', seedArg,
  '--engine-path', process.env.ENGINE_PATH ?? 'bin/bigshark-engine',
  '--solve-budget-ms', budgetArg,
  ...extra,
];
const child = spawn('./build/release/engine/bigshark-practice', args, { stdio: ['pipe', 'pipe', 'pipe'] });
const log = createWriteStream(logPath);
const events = createWriteStream(`${logPath}.events.log`);
let buffer = '';
let prompts = 0;

function answer(block) {
  const legal = block.split('\n').find((l) => l.startsWith('Legal:')) ?? '';
  const numbered = block.split('\n').filter((l) => /^\s+\d+\)/.test(l));
  if (policy === 'station') {
    return { reply: legal.includes('check') ? 'x' : 'c',
             legal, pick: legal.includes('check') ? 'checks' : 'calls' };
  }
  if (policy === 'folder') {
    return { reply: legal.includes('check') ? 'x' : 'f',
             legal, pick: legal.includes('check') ? 'checks' : 'folds' };
  }
  if (policy === 'passive-random' || policy === 'srp-passive') {
    // srp-passive opens exactly one preflop raise (smallest aggressive menu
    // entry) when it has the option; every later decision is passive.
    if (policy === 'srp-passive' && block.includes('-- PREFLOP --')) {
      const aggressive = numbered.find((l) => /raises|bets/.test(l));
      if (aggressive) {
        const pick = aggressive.trim().replace(/^\d+\)\s*/, '');
        return { reply: String(numbered.indexOf(aggressive) + 1), legal, pick };
      }
    }
    const passive = numbered.filter((l) => /folds|checks|calls/.test(l));
    const idx = Math.floor(rng() * passive.length);
    const pick = passive[idx].trim().replace(/^\d+\)\s*/, '');
    const reply = String(numbered.indexOf(passive[idx]) + 1);
    return { reply, legal, pick };
  }
  const idx = Math.floor(rng() * numbered.length);
  return { reply: String(idx + 1), legal,
           pick: numbered[idx].trim().replace(/^\d+\)\s*/, '') };
}

child.stdout.on('data', (chunk) => {
  log.write(chunk);
  buffer += chunk.toString();
  for (;;) {
    const at = buffer.indexOf('> ');
    if (at < 0) break;
    const block = buffer.slice(0, at);
    buffer = buffer.slice(at + 2);
    const { reply, legal, pick } = answer(block);
    prompts += 1;
    events.write(`${prompts}\t${legal.trim()}\t=> ${reply} ${pick}\n`);
    child.stdin.write(reply + '\n');
  }
});
child.stderr.on('data', (chunk) => log.write(chunk));
child.on('exit', (code) => {
  log.end();
  events.end();
  process.stderr.write(`exit=${code} prompts=${prompts}\n`);
  process.exit(code ?? 0);
});
