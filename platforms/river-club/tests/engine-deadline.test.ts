// RFC 0023 structural-client deadline boundaries. These are mechanical
// lifecycle tests, not artifact/solver quality or live-platform acceptance.
import assert from 'node:assert/strict';
import { create } from '@bufbuild/protobuf';
import test, { type TestContext } from 'node:test';

import {
  ActionType,
  DecisionResponseSchema,
  EnvelopeSchema,
  ExpandedStrategySchema,
  SolverSource,
  type Envelope,
} from '../../../build/generated/ts/bigshark/engine/v1/engine_pb.js';
import { decide, safeFallback } from '../src/engine.js';
import type { EngineConfig, ExecutableDecision, RiverRoom, V1EnvelopeClient } from '../src/types.js';

const room: RiverRoom = {
  id: 'deadline-offline-fixture',
  handId: 'deadline-hand',
  revision: 1,
  street: 'flop',
  blinds: [10, 20],
  dealer: 1,
  actor: 0,
  hero: {
    seat: 0, name: 'Hero', stack: 1000, bet: 0, status: 'active',
    cards: [{ rank: 'A', suit: 'spades' }, { rank: 'K', suit: 'spades' }],
  },
  board: [{ rank: '2', suit: 'clubs' }, { rank: '3', suit: 'diamonds' }, { rank: '7', suit: 'hearts' }],
  pot: 40,
  seats: [
    { seat: 0, name: 'Hero', stack: 1000, bet: 0, status: 'active', agent: true, blind: 'BB' },
    { seat: 1, name: 'Opponent', stack: 1000, bet: 0, status: 'active', button: true, blind: 'SB' },
  ],
  legal: {
    actions: ['check', 'bet'], call: 0, potOdds: 0, currentBet: 0,
    raiseTo: { min: 20, max: 1000 }, min: 20, max: 1000, amountMeaning: 'target-total',
  },
};

function response(): Envelope {
  const decision = create(DecisionResponseSchema, {
    result: {
      case: 'expandedStrategy',
      value: create(ExpandedStrategySchema, {
        actions: [{ type: ActionType.BET, targetTotal: 20n, probability: 1 }],
        selectedAction: { type: ActionType.BET, targetTotal: 20n },
        solver: {
          source: SolverSource.BLUEPRINT, reasonCode: 'blueprint', cacheHit: true,
          guaranteeLevel: 'approximate', artifactSha256: 'd'.repeat(64),
        },
      }),
    },
  });
  return create(EnvelopeSchema, { protocolMinor: 2, payload: { case: 'decisionResponse', value: decision } });
}

function deferred<T>() {
  let resolve!: (value: T | PromiseLike<T>) => void;
  let reject!: (reason?: unknown) => void;
  const promise = new Promise<T>((yes, no) => { resolve = yes; reject = no; });
  return { promise, resolve, reject };
}

function observe(promise: Promise<ExecutableDecision>) {
  const outcome: { settled: boolean; value?: ExecutableDecision; error?: unknown } = { settled: false };
  void promise.then(value => { outcome.settled = true; outcome.value = value; },
    error => { outcome.settled = true; outcome.error = error; });
  return outcome;
}

async function flushWork() { await new Promise<void>(resolve => setImmediate(resolve)); }

function clock(t: TestContext) {
  let now = 0;
  t.mock.method(performance, 'now', () => now);
  t.mock.timers.enable({ apis: ['setTimeout'] });
  return {
    async advance(ms: number) { now += ms; t.mock.timers.tick(ms); await flushWork(); },
    stallTo(ms: number) { now = ms; },
  };
}

function config(client: V1EnvelopeClient, timeoutMs = 100): EngineConfig {
  return {
    proto: true, protoEngineClient: client, protoBlueprint: true,
    heroName: 'Hero', timeoutMs, solveTimeBudgetMs: 1500,
    residentRoots: [], flopLibraries: [],
  };
}

function assertFallback(outcome: ReturnType<typeof observe>) {
  assert.equal(outcome.settled, true, 'the adapter wait must be bounded even on old/fake clients');
  assert.equal(outcome.error, undefined);
  assert.deepEqual(outcome.value, safeFallback(room));
  assert.equal(outcome.value?.artifactSha256, undefined);
  assert.equal(outcome.value?.guaranteeLevel, 'operational_fallback');
}

test('deadline: never-ready structural startup expires and sends no request', async t => {
  const timer = clock(t);
  const starting = deferred<void>();
  let requests = 0;
  const client: V1EnvelopeClient = {
    negotiatedProtocolMinor: 2,
    start: () => starting.promise,
    async request() { requests += 1; return response(); },
  };
  t.after(() => starting.resolve());
  const result = observe(decide(room, config(client)));
  await timer.advance(100);
  assertFallback(result);
  assert.equal(requests, 0);
  starting.resolve();
  await flushWork();
  assert.equal(requests, 0, 'late readiness must not resume an expired adapter decision');
});

test('deadline: rejected structural readiness uses legal recovery without an RPC', async () => {
  let requests = 0;
  const client: V1EnvelopeClient = {
    start: async () => { throw new Error('startup rejected'); },
    async request() { requests += 1; return response(); },
  };
  assert.deepEqual(await decide(room, config(client)), safeFallback(room));
  assert.equal(requests, 0);
});

test('deadline: rejection after startup expiration remains handled', async t => {
  const timer = clock(t);
  const starting = deferred<void>();
  let requests = 0;
  const result = observe(decide(room, config({
    start: () => starting.promise,
    async request() { requests += 1; return response(); },
  })));
  t.after(() => starting.reject(new Error('cleanup')));
  await timer.advance(100);
  assertFallback(result);
  starting.reject(new Error('background startup eventually failed'));
  await flushWork();
  assert.equal(requests, 0);
  assertFallback(result);
});

test('deadline: startup completion after the monotonic deadline cannot begin an RPC', async t => {
  const timer = clock(t);
  let requests = 0;
  const client: V1EnvelopeClient = {
    negotiatedProtocolMinor: 2,
    start: async () => { timer.stallTo(101); },
    async request() { requests += 1; return response(); },
  };
  const result = observe(decide(room, config(client)));
  await flushWork();
  assertFallback(result);
  assert.equal(requests, 0);
});

test('deadline: transport receives remaining time, while requested solver fields are unchanged', async t => {
  const timer = clock(t);
  const starting = deferred<void>();
  let budget = 0;
  let request: Envelope | undefined;
  const client: V1EnvelopeClient = {
    negotiatedProtocolMinor: 2,
    start: () => starting.promise,
    async request(value, timeoutMs) {
      request = value as Envelope;
      budget = timeoutMs!;
      return response();
    },
  };
  t.after(() => starting.resolve());
  const pending = observe(decide(room, config(client)));
  await timer.advance(60);
  starting.resolve();
  await flushWork();
  assert.equal(pending.settled, true);
  assert.equal(pending.value?.action, 'bet');
  assert.ok(Number.isFinite(budget) && budget > 0 && budget <= 40,
    `remaining transport budget must be positive and at most40, got ${budget}`);
  assert.equal(request?.protocolMinor, 2);
  assert.equal(request?.payload.case, 'decisionRequest');
  if (request?.payload.case !== 'decisionRequest') throw new Error('missing actual mapped request');
  assert.equal(request.payload.value.options?.solveTimeBudgetMs, 1500);
  assert.equal(request.payload.value.options?.solverMode, 6 /* BLUEPRINT */);
});

test('deadline: request-internal restart readiness is covered by the outer adapter deadline', async t => {
  const timer = clock(t);
  const restarting = deferred<Envelope>();
  let requests = 0;
  const client: V1EnvelopeClient = {
    negotiatedProtocolMinor: 2,
    start: async () => {},
    request() { requests += 1; return restarting.promise; },
  };
  t.after(() => restarting.resolve(response()));
  const pending = observe(decide(room, config(client)));
  await flushWork();
  assert.equal(requests, 1, 'entered structural request before its hidden readiness wait');
  await timer.advance(100);
  assertFallback(pending);
  restarting.resolve(response());
  await flushWork();
  assertFallback(pending);
});

test('deadline: late response is refused even without a scheduled timeout callback', async t => {
  const timer = clock(t);
  let requests = 0;
  const client: V1EnvelopeClient = {
    negotiatedProtocolMinor: 2,
    start: async () => {},
    async request() { requests += 1; timer.stallTo(101); return response(); },
  };
  const pending = observe(decide(room, config(client)));
  await flushWork();
  assert.equal(requests, 1);
  assertFallback(pending);
});

for (const timeoutMs of [NaN, Infinity, -Infinity, 0, -1]) {
  test(`deadline: invalid adapter budget ${String(timeoutMs)} recovers before startup`, async () => {
    let starts = 0;
    let requests = 0;
    const client: V1EnvelopeClient = {
      negotiatedProtocolMinor: 2,
      start: async () => { starts += 1; },
      async request() { requests += 1; return response(); },
    };
    assert.deepEqual(await decide(room, config(client, timeoutMs)), safeFallback(room));
    assert.equal(starts, 0);
    assert.equal(requests, 0);
  });
}
