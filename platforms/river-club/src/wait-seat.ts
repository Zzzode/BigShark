#!/usr/bin/env node
// wait-seat — poll `rooms` until an agent-enabled active table with an open seat
// exists at/under the given big blind, then join with --buy-in (100BB default).
// Only entry is automated; every gameplay decision stays with the model.
//
// Usage: wait-seat.mjs [--max-bb 50] [--buy-in 0(auto=100BB)] [--max-ms 300000]

import { spawnSync } from 'node:child_process';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

import {
  parseRiverError,
  parseRoomsResponse,
} from './json.js';
import type { RiverRoomSummary } from './types.js';

const here = dirname(fileURLToPath(import.meta.url));
const argv = process.argv.slice(2);
const flag = (name: string, fallback: number): number => {
  const index = argv.indexOf(`--${name}`);
  return index >= 0 ? Number(argv[index + 1]) : fallback;
};
const maxBb = flag('max-bb', 50);
const buyInArg = flag('buy-in', 0);
const maxMs = flag('max-ms', 300_000);

const cli = (args: string[]) => spawnSync(
  process.execPath,
  [join(here, 'journal-cli.js'), ...args],
  { encoding: 'utf8' },
);

const end = Date.now() + maxMs;
const denied = new Set<string>(); // room ids that rejected us (password / agents disabled) this run
let tick = 0;
while (Date.now() < end) {
  let rooms: RiverRoomSummary[];
  try { rooms = parseRoomsResponse(cli(['rooms']).stdout).rooms; }
  catch { await new Promise(r => setTimeout(r, 4000)); continue; }

  const open = rooms
    // Only already-active tables (at least one opponent); skip empty waiting rooms.
    .filter(r => r.allowAgents && r.status === 'playing' && r.seats[0] > 0
      && r.blinds[1] <= maxBb && r.seats[0] < r.seats[1] && !denied.has(r.id))
    // Prefer active tables with players, then lower blinds (bankroll safety).
    .sort((a, b) => (b.seats[0] - a.seats[0]) || (a.blinds[1] - b.blinds[1]));

  if (open.length) {
    const r = open[0];
    if (!r) continue;
    const buyIn = buyInArg || r.blinds[1] * 100;
    console.error(`seat open: ${r.id} ${r.name} ${r.blinds[0]}/${r.blinds[1]} ${r.seats[0]}/${r.seats[1]} → join buyIn=${buyIn}`);
    const res = cli(['join', r.id, '--buy-in', String(buyIn)]);
    let code: string | undefined;
    try { code = parseRiverError(res.stdout)?.code; } catch { /* non-JSON */ }
    if (res.status !== 0) {
      // Remember permanent rejections and keep waiting; surface other failures.
      if (code && ['ROOM_PASSWORD_INVALID', 'AGENTS_DISABLED', 'ROOM_FULL'].includes(code)) {
        denied.add(r.id);
        console.error(`\n${r.name}(${r.id}) rejected: ${code}, skipping it this run`);
        continue;
      }
      process.stdout.write(res.stdout);
      process.exit(res.status ?? 1);
    }
    process.stdout.write(res.stdout);
    process.exit(0);
  }
  process.stderr.write(`waiting for seat (bb<=${maxBb})${'.'.repeat((tick++ % 4) + 1)}   \r`);
  await new Promise(r => setTimeout(r, 5000));
}
console.error('\nwait budget exhausted');
process.exit(2);
