#!/usr/bin/env node

import { readFileSync, readdirSync, writeFileSync } from 'node:fs';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

import {
  buildContext,
  closeEngine,
  decide,
} from '../platforms/river-club/src/engine.js';
import type {
  ExecutableDecision,
  GoldenFixture,
  RiverMe,
  RiverRoom,
  RiverState,
} from '../platforms/river-club/src/types.js';

const root = resolve(dirname(fileURLToPath(import.meta.url)), '../..');
const sessionDirectory = join(root, 'sessions');
const outputPath = join(
  root,
  'platforms',
  'river-club',
  'tests',
  'fixtures',
  'v0-golden.json',
);

const selections: Array<{
  name: string;
  handId: string;
  revision: number;
}> = [
  {
    name: 'preflop-facing-reraise',
    handId: 'hand-1789118465225-489502',
    revision: 34,
  },
  {
    name: 'multiway-flop-check-option',
    handId: 'hand-1789114858081-855209',
    revision: 854,
  },
  {
    name: 'multiway-turn-facing-raise',
    handId: 'hand-1789115319484-460647',
    revision: 984,
  },
  {
    name: 'heads-up-river-check-option',
    handId: 'hand-1789116805259-459304',
    revision: 1316,
  },
  {
    name: 'multiway-river-facing-bet',
    handId: 'hand-1789115319484-460647',
    revision: 991,
  },
  {
    name: 'short-stack-heads-up-river',
    handId: 'hand-1789120907393-378663',
    revision: 410,
  },
];

type FixtureState = RiverState & { me: RiverMe; room: RiverRoom };

function loadSnapshots(): Map<string, FixtureState> {
  const snapshots = new Map<string, FixtureState>();
  for (const file of readdirSync(sessionDirectory).filter(name => name.endsWith('.jsonl')).sort()) {
    const lines = readFileSync(join(sessionDirectory, file), 'utf8').split('\n');
    for (const line of lines) {
      if (!line.trim()) continue;
      let entry: unknown;
      try {
        entry = JSON.parse(line) as unknown;
      } catch {
        continue;
      }
      if (!isSessionEntry(entry)) continue;
      const room = entry.resp.room;
      if (!room?.legal || !room.hero?.cards?.length) continue;
      const key = `${room.handId}:${room.revision}`;
      if (!snapshots.has(key)) snapshots.set(key, entry.resp);
    }
  }
  return snapshots;
}

function sanitizeState(state: FixtureState, fixtureName: string): FixtureState {
  const clone = structuredClone(state);
  const room = clone.room;
  const heroSeat = room.hero.seat;
  const originalNames = new Map<string, string>();

  for (const seat of room.seats || []) {
    if (seat?.name) originalNames.set(seat.name, seat.seat === heroSeat ? 'Hero' : `Player${seat.seat}`);
  }
  if (clone.me?.name) originalNames.set(clone.me.name, 'Hero');

  clone.me = { id: 'hero', name: 'Hero' };
  room.id = `room-${fixtureName}`;
  room.name = `Fixture ${fixtureName}`;
  room.handId = `hand-${fixtureName}`;
  room.timeLeftMs = 15000;

  delete room.hero.name;
  for (const seat of room.seats || []) {
    if (!seat) continue;
    if (seat.name) seat.name = originalNames.get(seat.name) || `Player${seat.seat}`;
    if (seat.action) delete seat.action;
  }

  for (const event of room.events || []) {
    const originalText = event.text || '';
    const actor = [...originalNames.entries()]
      .sort((left, right) => right[0].length - left[0].length)
      .find(([original]) => originalText.startsWith(original))?.[1];
    event.text = actor ? `${actor} ${event.kind}` : `Event ${event.kind}`;
  }

  return clone;
}

function stableDecision(decision: ExecutableDecision): ExecutableDecision {
  return {
    action: decision.action,
    ...(decision.amount === undefined ? {} : { amount: decision.amount }),
    reason: decision.reason,
  };
}

const snapshots = loadSnapshots();
const fixtures: GoldenFixture[] = [];

for (const selection of selections) {
  const key = `${selection.handId}:${selection.revision}`;
  const source = snapshots.get(key);
  if (!source) throw new Error(`Missing source snapshot ${key}`);

  const state = sanitizeState(source, selection.name);
  const context = buildContext(state.room, {
    style: 'tag',
    heroName: 'Hero',
  });
  const decision = await decide(state.room, {
    style: 'tag',
    heroName: 'Hero',
  });

  fixtures.push({
    name: selection.name,
    state,
    context,
    decision: stableDecision(decision),
  });
}

writeFileSync(outputPath, `${JSON.stringify(fixtures, null, 2)}\n`);
process.stdout.write(`Wrote ${fixtures.length} sanitized River v0 fixtures to ${outputPath}\n`);
closeEngine();
process.exit(0);

function isSessionEntry(
  value: unknown,
): value is { resp: FixtureState } {
  if (typeof value !== 'object' || value === null) return false;
  const response = (value as { resp?: unknown }).resp;
  if (typeof response !== 'object' || response === null) return false;
  const candidate = response as { me?: unknown; room?: unknown };
  return typeof candidate.me === 'object'
    && candidate.me !== null
    && typeof candidate.room === 'object'
    && candidate.room !== null;
}
