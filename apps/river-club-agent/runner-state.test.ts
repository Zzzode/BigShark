import assert from 'node:assert/strict';
import test from 'node:test';

import {
  engineBudgetMs,
  overrideWindowMs,
  parseConfigResidentRoots,
  resultsLogEntry,
  selectCliFailureStep,
  selectPausedStep,
  selectRunnerStep,
  shouldRetryAction,
} from './runner-state.js';
import type {
  RiverRoom,
  RiverState,
} from '../../platforms/river-club/src/types.js';

function room(overrides: Partial<RiverRoom> = {}): RiverRoom {
  return {
    id: 'room-1',
    revision: 1,
    handId: 'hand-1',
    street: 'flop',
    hero: {
      seat: 0,
      name: 'Hero',
      stack: 2_000,
      cards: [
        { rank: 'A', suit: 'spades' },
        { rank: 'K', suit: 'hearts' },
      ],
      status: 'active',
    },
    board: [
      { rank: 'Q', suit: 'spades' },
      { rank: 'J', suit: 'hearts' },
      { rank: '2', suit: 'clubs' },
    ],
    pot: 200,
    seats: [
      {
        seat: 0,
        name: 'Hero',
        stack: 2_000,
        status: 'active',
      },
      {
        seat: 1,
        name: 'Villain',
        stack: 2_000,
        status: 'active',
      },
    ],
    ...overrides,
  };
}

function state(
  currentRoom: Exclude<RiverState['room'], undefined>,
): RiverState {
  return {
    me: { name: 'Hero', wallet: 8_000 },
    room: currentRoom,
  };
}

test('classifies off-table, leaving, and heartbeat states', () => {
  assert.equal(selectRunnerStep(state(null), 'Hero', null, 4_000).kind, 'off-table');
  assert.equal(selectRunnerStep(
    state({ id: 'room-1', revision: 2, mode: 'leaving' }),
    'Hero',
    null,
    4_000,
  ).kind, 'leaving');
  assert.equal(selectRunnerStep(
    state({ id: 'room-1', revision: 3 }),
    'Hero',
    null,
    4_000,
  ).kind, 'heartbeat');
});

test('classifies wait and act states', () => {
  assert.equal(selectRunnerStep(state(room()), 'Hero', null, 4_000).kind, 'wait');
  assert.equal(selectRunnerStep(state(room({
    legal: {
      actions: ['check', 'bet'],
      call: 0,
      potOdds: 0,
      raiseTo: { min: 100, max: 2_000 },
    },
  })), 'Hero', null, 4_000).kind, 'act');
});

test('detects table dissolution and stop-loss', () => {
  const empty = room({ seats: [room().seats[0] ?? null] });
  assert.equal(selectRunnerStep(state(empty), 'Hero', null, 4_000).kind, 'empty-table');

  const losing = state(room({ hero: { ...room().hero, stack: 1_000 } }));
  const step = selectRunnerStep(losing, 'Hero', 13_000, 4_000);
  assert.deepEqual(step.kind === 'stop-loss'
    ? { kind: step.kind, total: step.total }
    : step, { kind: 'stop-loss', total: 9_000 });
});

test('requires explicit resume and recognizes stale action recovery', () => {
  assert.equal(selectPausedStep(false, false), 'wait');
  assert.equal(selectPausedStep(false, true), 'resume');
  assert.equal(selectPausedStep(true, true), 'stop');
  assert.equal(shouldRetryAction('STALE_STATE'), true);
  assert.equal(shouldRetryAction('NOT_YOUR_TURN'), true);
  assert.equal(shouldRetryAction('RATE_LIMITED'), false);
});

test('classifies CLI failures without treating them as table state', () => {
  assert.deepEqual(selectCliFailureStep({
    ok: false,
    error: 'slow down',
    code: 'RATE_LIMITED',
    retryAfterMs: 750,
  }), { kind: 'retry', delayMs: 750 });
  assert.deepEqual(selectCliFailureStep({
    ok: false,
    error: 'gone',
    code: 'NOT_IN_ROOM',
  }), { kind: 'off-table' });
  assert.deepEqual(selectCliFailureStep({
    ok: false,
    error: 'invalid token',
    code: 'TOKEN_INVALID',
  }), { kind: 'fatal' });
});

test('never allocates more decision time than the remaining clock', () => {
  assert.equal(engineBudgetMs(900, 1_200), 0);
  assert.equal(engineBudgetMs(6_020, 6_000), 0);
  assert.equal(engineBudgetMs(8_000, 6_000), 2_000);
  assert.equal(overrideWindowMs('adaptive', 10_000, 2_500, 6_000, 1_200, 1_500), 1_500);
  assert.equal(overrideWindowMs('adaptive', 7_000, 500, 6_000, 1_200, 1_500), 0);
  assert.equal(overrideWindowMs('auto', 7_000, 500, 6_000, 1_200, 1_500), 500);
});

test('resultsLogEntry embeds the engine guarantee level and attaches raw only on failure', () => {
  const ok = resultsLogEntry({
    kind: 'action',
    hand: 'h1',
    street: 'river',
    source: 'engine',
    decision: { action: 'call', reason: 'cpp:gto c', guaranteeLevel: 'exact_solved' },
    ok: true,
    raw: { should: 'be dropped on success' },
  });
  assert.deepEqual(ok, {
    kind: 'action',
    hand: 'h1',
    street: 'river',
    source: 'engine',
    decision: { action: 'call', reason: 'cpp:gto c', guaranteeLevel: 'exact_solved' },
    ok: true,
  });

  const failed = resultsLogEntry({
    kind: 'action',
    hand: 'h2',
    street: 'flop',
    source: 'engine',
    decision: { action: 'fold', reason: 'safe-fallback:fold', guaranteeLevel: 'operational_fallback' },
    ok: false,
    raw: { code: 'STALE_STATE' },
  });
  assert.equal(failed.raw !== undefined && (failed.raw as {code: string}).code, 'STALE_STATE');
  assert.equal(failed.decision.guaranteeLevel, 'operational_fallback');

  const retry = resultsLogEntry({
    kind: 'action-retry',
    hand: 'h3',
    street: 'turn',
    source: 'engine-recovery',
    decision: { action: 'check', reason: 'safe-fallback:check', guaranteeLevel: 'operational_fallback' },
    ok: true,
  });
  assert.equal('raw' in retry, false);
  assert.equal(retry.kind, 'action-retry');
});

// RFC 0009 W1: the runtime config's resident-root validator. It throws on a
// malformed value so the runner's startup preflight exits loudly instead of
// silently discarding the whole config (which the file-parse fallback would
// otherwise do).
test('parseConfigResidentRoots accepts valid specs and rejects malformed ones', () => {
  const pin = 'a'.repeat(64);
  assert.deepEqual(
    parseConfigResidentRoots([{ path: '/x/policy.db', sha256: pin }]),
    [{ path: '/x/policy.db', sha256: pin }],
  );
  assert.deepEqual(parseConfigResidentRoots([]), []);
  assert.throws(() => parseConfigResidentRoots('nope'), /must be an array/);
  assert.throws(() => parseConfigResidentRoots([null]), /entries must be/);
  assert.throws(() => parseConfigResidentRoots([{ path: '', sha256: pin }]), /nonempty|requires/);
  assert.throws(() => parseConfigResidentRoots([{ path: '/x', sha256: 'AB'.repeat(32) }]), /64-lowercase-hex/);
  assert.throws(() => parseConfigResidentRoots([{ path: '/x', sha256: 'abc' }]), /64-lowercase-hex/);
  assert.throws(
    () => parseConfigResidentRoots([{ path: '/x', sha256: pin }, { sha256: pin }]),
    /requires/,
  );
});
