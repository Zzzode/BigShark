#!/usr/bin/env node
// shoot — atomic "refresh snapshot → validate preconditions → act" in one process,
// so the model's tool-roundtrip latency never burns the CLI's cached decision snapshot.
//
// The model decides from wait-turn's digest, then calls shoot ONCE. Shoot re-pulls
// the live state itself and refuses to fire unless the live state still matches
// the decision (same hand/street, action legal, amount in range, call price sane).
// Any divergence → safe abort, no action sent, fresh state printed for a new decision.
//
// Usage:
//   shoot.mjs fold|check|call [--hand ID] [--street S]
//   shoot.mjs bet|raise --amount TOTAL [--hand ID] [--street S]
//   shoot.mjs call --max-call-bb N      (abort if call price exceeds N big blinds)
//   (global) --allow-different-street   (debug; normally street mismatch aborts)

import { spawnSync } from 'node:child_process';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

import { parseRiverState } from './json.js';
import { isRiverRoom, type Action, type RiverState } from './types.js';

const here = dirname(fileURLToPath(import.meta.url));
const cli = (args: string[]) => spawnSync(
  process.execPath,
  [join(here, 'journal-cli.js'), ...args],
  { encoding: 'utf8', maxBuffer: 4 * 1024 * 1024 },
);

const argv = process.argv.slice(2);
const rawAction = argv[0];
const flag = (name: string): string | undefined => {
  const index = argv.indexOf(`--${name}`);
  return index >= 0 ? argv[index + 1] : undefined;
};
const amount = flag('amount') !== undefined ? Number(flag('amount')) : null;
const expectHand = flag('hand');
const expectStreet = flag('street');
const maxCallBb = flag('max-call-bb') !== undefined ? Number(flag('max-call-bb')) : null;

if (!rawAction || !['fold', 'check', 'call', 'bet', 'raise'].includes(rawAction)) {
  console.error('usage: shoot <fold|check|call|bet|raise> [--amount N] ...'); process.exit(1);
}
const action = rawAction as Action;

function abort(why: string, state?: RiverState): never {
  console.log(JSON.stringify({ aborted: true, reason: why, state: state ? digest(state) : null }));
  process.exit(2);
}

function digest(s: RiverState) {
  const r = s.room;
  if (!r) return { notInRoom: true, control: s.control };
  if (!isRiverRoom(r)) {
    return { heartbeat: true, room: r.id, revision: r.revision };
  }
  const h = r.hero;
  return {
    hand: r.handId, street: r.street, next: r.next, t: r.timeLeftMs,
    pos: h?.position, cards: h?.cards?.map(c => c.rank + c.suit[0]),
    stackBb: h?.effectiveStackBb, potBb: r.solver?.potBb, toCallBb: r.solver?.toCallBb,
    legal: r.legal?.actions, call: r.legal?.call, raiseTo: r.legal?.raiseTo,
    events: r.events?.slice(-6).map(e => e.text),
    folded: h?.status === 'folded', winners: r.winners,
  };
}

// 1. Fresh snapshot (this is what `act` will bind to).
const st = cli(['state']);
if (st.status !== 0) { console.log(st.stdout); process.exit(st.status ?? 1); }
const s = parseRiverState(st.stdout);
const r = s.room;
if (!r) abort('not-in-room', s);
if (!isRiverRoom(r)) abort('incomplete-snapshot', s);
if (r.mode === 'leaving') abort('room-leaving', s);
if (r.next !== 'act' || !r.legal) abort(`not-our-turn(next=${r.next})`, s);
if (r.hero?.status === 'folded') abort('already-folded', s);
if (expectHand && r.handId !== expectHand) abort('hand-changed', s);
if (expectStreet && r.street !== expectStreet && !argv.includes('--allow-different-street')) abort('street-changed', s);
if (!r.legal.actions.includes(action)) abort(`illegal-action(${action}); legal=${r.legal.actions.join('/')}`, s);

const bb = r.blinds?.[1] ?? 20;
if (action === 'call' && maxCallBb !== null) {
  const callBb = (r.legal.call ?? 0) / bb;
  if (callBb > maxCallBb) abort(`call ${callBb.toFixed(1)}bb > max ${maxCallBb}bb`, s);
}
if (action === 'bet' || action === 'raise') {
  const range = r.legal.raiseTo;
  if (!range || amount === null || !Number.isInteger(amount) || amount < range.min || amount > range.max) {
    abort(`amount ${amount} outside raiseTo ${range?.min}..${range?.max}`, s);
  }
}

// 2. Fire immediately — snapshot was cached by the `state` above, milliseconds ago.
const actArgs = ['act', action];
if (action === 'bet' || action === 'raise') actArgs.push('--amount', String(amount));
const res = cli(actArgs);
process.stdout.write(res.stdout || JSON.stringify({ ok: false, raw: res.stderr }));
process.exit(res.status ?? 0);
