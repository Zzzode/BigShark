#!/usr/bin/env node
// wait-turn (protocol v2) — block on `river-club next` until our decision is due
// (room.legal present), we're off-table, or the budget expires. Emits ONE compact
// one-line digest so model processing stays fast; the full state is in the journal.
//
// Decide from the digest, then call shoot.mjs ATOMICALLY (it refreshes its own
// snapshot and validates hand/street/legality before acting). Do not call state
// between deciding and shooting.
//
// Usage: wait-turn.mjs [--max-ms 600000] [--until null|legal]

import { spawn } from 'node:child_process';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

import { message, parseRiverState } from './json.js';
import { isRiverRoom, type RiverSeat, type RiverState } from './types.js';

const here = dirname(fileURLToPath(import.meta.url));
const argv = process.argv.slice(2);
const maxMsIndex = argv.indexOf('--max-ms');
const untilIndex = argv.indexOf('--until');
const maxMs = Number(maxMsIndex >= 0 ? argv[maxMsIndex + 1] : undefined) || 600_000;
const until = untilIndex >= 0 ? argv[untilIndex + 1] : 'legal';

function next(): Promise<RiverState> {
  return new Promise<RiverState>((resolve, reject) => {
    const p = spawn(process.execPath, [join(here, 'journal-cli.js'), 'next', '--timeout', '6000'],
      { stdio: ['ignore', 'pipe', 'inherit'] });
    let out = '';
    p.stdout.on('data', (chunk: Buffer) => (out += chunk.toString('utf8')));
    p.on('close', code => (
      code === 0
        ? resolve(parseRiverState(out))
        : reject(new Error(out.trim() || `exit ${code}`))
    ));
  });
}

function emit(s: RiverState): void {
  const r = s.room;
  if (!r) { console.log(JSON.stringify({ offTable: true, control: s.control })); return; }
  if (!isRiverRoom(r)) {
    console.log(JSON.stringify({ heartbeat: true, room: r.id, revision: r.revision }));
    return;
  }
  const h = r.hero;
  const seats = r.seats.filter((seat): seat is RiverSeat => seat !== null).map(x =>
    `${x.seat}:${x.name}${x.button ? '*B' : ''}${x.blind ? '/' + x.blind : ''}${x.status === 'folded' ? '/f' : x.status === 'all-in' ? '/AI' : ''}${x.agent ? '/ai' : ''}${x.bet ? '/b' + x.bet : ''}${x.stack < 800 ? '/s' + x.stack : ''}`).join(' ');
  const out = {
    hand: r.handId, st: r.street, next: r.next, mode: r.mode, t_ms: r.timeLeftMs,
    pos: h?.position, cards: h?.cards.map(card => card.rank + card.suit[0]).join('') || '',
    stackBb: h?.effectiveStackBb, potBb: r.solver?.potBb, spr: r.solver?.spr,
    toCallBb: r.solver?.toCallBb, n: r.solver?.playersInHand,
    legal: r.legal ? { acts: r.legal.actions, call: r.legal.call, odds: r.legal.potOdds, to: r.legal.raiseTo } : null,
    board: r.board.map(card => card.rank + card.suit[0]).join(''),
    seats,
    ev: (r.events || []).slice(-8).map(event => event.text),
    win: r.winners,
  };
  console.log(JSON.stringify(out));
}

const end = Date.now() + maxMs;
while (Date.now() < end) {
  let s: RiverState;
  try { s = await next(); }
  catch (e) {
    process.stderr.write(`wait: ${message(e).slice(0, 120)}\n`);
    await new Promise(r => setTimeout(r, 2500));
    continue;
  }
  if (s.room === null || s.room === undefined) { emit(s); process.exit(0); }
  if (s.room?.mode === 'leaving') continue;
  if (until === 'null') { emit(s); process.exit(0); }
  if (isRiverRoom(s.room) && s.room.legal) { emit(s); process.exit(0); }
  // heartbeat / wait → keep polling
}
process.stderr.write('wait budget exhausted\n');
process.exit(2);
