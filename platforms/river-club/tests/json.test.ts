import assert from 'node:assert/strict';
import test from 'node:test';

import {
  parseRiverError,
  parseRiverState,
} from '../src/json.js';

test('parses full River state and sparse heartbeats', () => {
  const state = parseRiverState(JSON.stringify({
    changed: true,
    me: { name: 'Hero', wallet: 8_000 },
    room: {
      id: 'room-1',
      revision: 3,
      handId: 'hand-1',
      street: 'flop',
      hero: {
        seat: 0,
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
        { seat: 0, name: 'Hero', stack: 2_000, status: 'active' },
        { seat: 1, name: 'Opponent', stack: 2_000, status: 'all-in' },
      ],
      legal: {
        actions: ['check', 'bet'],
        call: 0,
        potOdds: 0,
        raiseTo: { min: 100, max: 2_000 },
      },
    },
  }));
  assert.equal(state.room?.revision, 3);

  const heartbeat = parseRiverState(JSON.stringify({
    changed: false,
    room: { id: 'room-1', revision: 4 },
  }));
  assert.equal(heartbeat.room?.revision, 4);
});

test('rejects malformed full room state', () => {
  assert.throws(
    () => parseRiverState(JSON.stringify({
      room: {
        id: 'room-1',
        revision: 3,
        handId: 'hand-1',
        street: 'flop',
        hero: null,
        board: [],
        pot: 0,
        seats: [],
      },
    })),
    /room state is invalid/,
  );
});

test('parses CLI errors separately from state', () => {
  const payload = JSON.stringify({
    ok: false,
    error: 'slow down',
    status: 429,
    code: 'RATE_LIMITED',
    retryable: true,
    retryAfterMs: 750,
  });
  assert.throws(() => parseRiverState(payload), /not state/);
  assert.deepEqual(parseRiverError(payload), {
    ok: false,
    error: 'slow down',
    status: 429,
    code: 'RATE_LIMITED',
    retryable: true,
    retryAfterMs: 750,
  });
});
