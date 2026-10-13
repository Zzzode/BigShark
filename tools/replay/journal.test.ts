import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { resolve } from 'node:path';
import test from 'node:test';
import type { GoldenFixture } from '../../platforms/river-club/src/types.js';
import { collectReplaySnapshots, decisionProblem, replayExitCode } from './journal.js';

const fixtures = JSON.parse(readFileSync(resolve('platforms/river-club/tests/fixtures/v0-golden.json'), 'utf8')) as GoldenFixture[];
const first = fixtures[0];
assert.ok(first);
const record = JSON.stringify({ resp: first.state });

test('fixed engineering corpus stays explicitly synthetic and covers all betting streets', () => {
  const result = collectReplaySnapshots([{ name: 'synthetic-golden', text: fixtures.map(f => JSON.stringify({ resp: f.state })).join('\n') }], 'synthetic');
  assert.equal(result.snapshots.length, fixtures.length);
  assert.ok(result.snapshots.length > 0);
  assert.deepEqual(new Set(result.snapshots.map(s => s.room.street)), new Set(['preflop', 'flop', 'turn', 'river']));
  assert.ok(result.snapshots.every(s => s.source === 'synthetic'));
});

test('overlapping journals deduplicate a state but never collide across rooms', () => {
  const other = structuredClone(first.state);
  other.room.id = 'another-table';
  const result = collectReplaySnapshots([{ name: 'a', text: record }, { name: 'b', text: `${record}\n${JSON.stringify({ resp: other })}` }], 'historical');
  assert.equal(result.snapshots.length, 2);
  assert.equal(result.stats.duplicates, 1);
});

test('malformed, empty, waiting and missing-private-card records are accounted separately', () => {
  const waiting = structuredClone(first.state); waiting.room.next = 'wait';
  const noCards = structuredClone(first.state); noCards.room.hero.cards = [];
  const result = collectReplaySnapshots([{ name: 'mixed', text: `\nnot-json\n{}\n${JSON.stringify({ resp: waiting })}\n${JSON.stringify({ resp: noCards })}\n${record}\n` }], 'historical');
  assert.equal(result.snapshots.length, 1);
  assert.deepEqual(result.stats, { lines: 5, malformed: 1, skipped: 3, duplicates: 0 });
});

test('legal amount endpoints pass; missing, nonfinite and out-of-range amounts fail', () => {
  const room = structuredClone(first.state.room);
  room.legal = { actions: ['bet', 'check'], call: 0, potOdds: 0, raiseTo: { min: 20, max: 100 } };
  for (const amount of [20, 100]) assert.equal(decisionProblem(room, { action: 'bet', amount }), null);
  for (const amount of [19, 101, NaN, Infinity]) assert.notEqual(decisionProblem(room, { action: 'bet', amount }), null);
  assert.notEqual(decisionProblem(room, { action: 'bet' }), null);
  assert.notEqual(decisionProblem(room, { action: 'raise', amount: 20 }), null);
  assert.notEqual(decisionProblem(room, undefined), null);
});

test('illegal actions or operational fallback make the regression command fail', () => {
  assert.equal(replayExitCode(0, 0), 0);
  assert.equal(replayExitCode(1, 0), 1);
  assert.equal(replayExitCode(0, 1), 1);
});
