#!/usr/bin/env node
// replay.mjs — run the engine over every actionable state found in the session
// journals. Reports action mix, latency, illegal outputs. Read-only.
import { readFileSync, readdirSync } from 'node:fs';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import {
  buildContext,
  closeEngine,
  decide,
} from '../../platforms/river-club/src/engine.js';
import {
  isRiverRoom,
  type Action,
  type RiverState,
} from '../../platforms/river-club/src/types.js';

const here = dirname(fileURLToPath(import.meta.url));
const root = resolve(here, '../../..');

const seen = new Set<string>();
let n = 0, fallback = 0, illegal = 0, totalMs = 0, maxMs = 0;
const mix: Partial<Record<Action, number>> = {};
const samples: string[] = [];

for (const f of readdirSync(join(root, 'sessions')).filter(f => f.endsWith('.jsonl'))) {
  for (const line of readFileSync(join(root, 'sessions', f), 'utf8').split('\n')) {
    if (!line.trim()) continue;
    let e: { resp?: RiverState };
    try {
      e = JSON.parse(line) as { resp?: RiverState };
    } catch {
      continue;
    }
    const r = e.resp?.room;
    if (!isRiverRoom(r) || !r.legal || !r.hero.cards.length) continue;
    const key = r.handId + ':' + r.revision + ':' + r.street;
    if (seen.has(key)) continue;
    seen.add(key);
    const heroName = e.resp?.me?.name || r.hero?.name || '';
    const ctx = buildContext(r, { style: 'tag', heroName });
    const t0 = process.hrtime.bigint();
    const d = await decide(r, { style: 'tag', heroName });
    const ms = Number(process.hrtime.bigint() - t0) / 1e6;
    totalMs += ms; maxMs = Math.max(maxMs, ms);
    if (d.reason?.startsWith('safe-fallback')) fallback++;
    if (!d || !r.legal.actions.includes(d.action)) { illegal++; if (illegal <= 5) console.log('ILLEGAL:', d); continue; }
    if ((d.action === 'raise' || d.action === 'bet')
      && (d.amount === undefined
        || !r.legal.raiseTo
        || d.amount < r.legal.raiseTo.min
        || d.amount > r.legal.raiseTo.max)) {
      illegal++; console.log('BAD SIZE', d, r.legal.raiseTo); continue;
    }
    n++;
    mix[d.action] = (mix[d.action] ?? 0) + 1;
    if (samples.length < 10 && (d.action === 'raise' || d.action === 'bet'))
      samples.push(`${ctx.street} ${ctx.hole.join('')} ${ctx.position} ${d.action} ${d.amount} :: ${d.reason} (${ctx.raises}r,${ctx.limpers}l,${ctx.playersInHand}w)`);
  }
}
console.log(`\nDecisions: ${n} | Illegal: ${illegal} | JS fallbacks: ${fallback} | Average ${(totalMs / Math.max(1, n)).toFixed(2)}ms | Maximum ${maxMs.toFixed(1)}ms`);
console.log('Action mix:', JSON.stringify(mix));
console.log('Bet and raise samples:'); samples.forEach(s => console.log('  ', s));
closeEngine();
process.exit(0);
