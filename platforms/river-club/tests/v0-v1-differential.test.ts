import assert from 'node:assert/strict';
import { create } from '@bufbuild/protobuf';
import { readFileSync } from 'node:fs';
import { dirname, join, resolve } from 'node:path';
import test from 'node:test';
import { fileURLToPath } from 'node:url';

import { ProtoEngineProcessClient } from '../../../clients/node/proto-engine-process-client.js';
import { EngineProcessClient } from '../../../clients/node/engine-process-client.js';
import {
  EnvelopeSchema,
  GetCapabilitiesRequestSchema,
  type Envelope,
} from '../../../build/generated/ts/bigshark/engine/v1/engine_pb.js';
import { decide } from '../src/engine.js';
import { buildContext } from '../src/v0-normalizer.js';
import {
  decisionEnvelope,
  fromV1DecisionResponse,
  toV1DecisionRequest,
} from '../src/v1-mapper.js';
import type {
  Action,
  GoldenFixture,
  RawEngineDecision,
  RiverRoom,
  V0DecisionContext,
} from '../src/types.js';

const here = dirname(fileURLToPath(import.meta.url));
const root = resolve(here, '../../../..');
const engineBinary = process.env.BIGSHARK_ENGINE_BINARY
  || join(root, 'bin', 'bigshark-engine');
const fixtures = JSON.parse(readFileSync(
  join(root, 'platforms', 'river-club', 'tests', 'fixtures', 'v0-golden.json'),
  'utf8',
)) as GoldenFixture[];

function v0Client(): EngineProcessClient<V0DecisionContext, RawEngineDecision> {
  return new EngineProcessClient({
    command: engineBinary,
    warmupRequest: {
      handId: 'warmup',
      revision: 0,
      style: 'tag',
      street: 'preflop',
      blinds: [10, 20],
      position: 'BTN',
      playersInHand: 2,
      effectiveStackBb: 100,
      hole: [],
      board: [],
      pot: 0,
      riverGtoOn: false,
      raises: 0,
      openerPosition: '',
      heroWasRaiser: false,
      heroPreflopAggressor: false,
      limpers: 0,
    },
    warmupTimeoutMs: 10_000,
  });
}

function v1Client(): ProtoEngineProcessClient {
  const warmup = create(EnvelopeSchema, { protocolMinor: 0 });
  warmup.payload = {
    case: 'getCapabilitiesRequest',
    value: create(GetCapabilitiesRequestSchema),
  };
  warmup.requestId = 'differential-warmup';
  return new ProtoEngineProcessClient({
    command: engineBinary,
    args: ['--serve-proto'],
    warmupEnvelope: warmup,
    warmupTimeoutMs: 10_000,
  });
}

// Runs both transport paths of the real engine binary against the same frozen
// River state and compares the executable action and exact target total.
for (const fixture of fixtures) {
  test(`v0/v1 decision parity: ${fixture.name}`, async t => {
    const jsonClient = v0Client();
    const protoClient = v1Client();
    t.after(() => {
      jsonClient.stop();
      protoClient.stop();
    });

    const config = {
      style: 'tag',
      heroName: fixture.state.me.name,
      timeoutMs: 10_000,
      engineClient: jsonClient,
    };

    // (i) v0 JSON path against the frozen context.
    const v0Context = buildContext(fixture.state.room, config);
    assert.deepEqual(v0Context, fixture.context);
    const v0Decision = await jsonClient.request(v0Context, 10_000);
    assert.equal(v0Decision.action, fixture.decision.action,
      `${fixture.name}: v0 action matches golden`);
    assert.equal(v0Decision.amount ?? 0, fixture.decision.amount ?? 0,
      `${fixture.name}: v0 target matches golden`);

    // (ii) framed v1 path built from the identical frozen state.
    const request = toV1DecisionRequest(fixture.state.room, config);
    const response = await protoClient.request(
      decisionEnvelope(request),
      10_000,
    ) as Envelope;
    assert.equal(response.payload.case, 'decisionResponse',
      `${fixture.name}: engine returned a decision envelope`);
    if (response.payload.case !== 'decisionResponse') throw new Error('bad payload');
    assert.equal(response.payload.value.result.case, 'strategy',
      `${fixture.name}: valid state yields a strategy, never an error`);

    const v1Decision = fromV1DecisionResponse(response, fixture.state.room);
    assert.equal(v1Decision.action, v0Decision.action,
      `${fixture.name}: action parity (${v0Decision.action} vs ${v1Decision.action})`);
    assert.equal(v1Decision.amount ?? 0, v0Decision.amount ?? 0,
      `${fixture.name}: exact target-total parity`);

    // Selected action membership: the probability-1 policy and the sampled
    // action are validated by the mapper against the platform legal set.
    const executable = await decide(fixture.state.room, {
      ...config,
      proto: true,
      protoEngineClient: {
        request: () => Promise.resolve(response),
      },
    });
    assert.equal(executable.action, fixture.decision.action);
    assert.equal(executable.amount ?? 0, fixture.decision.amount ?? 0);
  });
}

test('multi-word and unmatched actor names map as opponents, not failures', async t => {
  const multiwordFixture = fixtures[1];
  assert.ok(multiwordFixture);
  const baseRoom = multiwordFixture.state.room;
  const room = structuredClone(baseRoom);
  // Give every opponent a multi-word display name the first-token matching in
  // the v0 normalizer tolerates; include one name that matches no seat at all.
  const seats = room.seats.filter((seat): seat is NonNullable<typeof seat> => seat !== null);
  const opponent = seats.find(seat => seat.seat !== room.hero.seat);
  assert.ok(opponent);
  room.events = [
    { kind: 'check', text: `${opponent.name} check` },
    { kind: 'check', text: 'Mysterious Stranger check' },
    { kind: 'check', text: 'Hero check' },
  ];
  const request = toV1DecisionRequest(room, { style: 'tag', heroName: 'Hero' });
  const actors = request.state!.actionHistory.map(event => event.actorPlayerId);
  assert.deepEqual(actors, [
    `seat-${opponent.seat}`,
    `seat-${opponent.seat}`,
    `seat-${room.hero.seat}`,
  ]);
  // The request still reaches a strategy through the real framed binary.
  const protoClient = v1Client();
  t.after(() => protoClient.stop());
  const executable = await decide(room, {
    style: 'tag',
    heroName: 'Hero',
    timeoutMs: 10_000,
    proto: true,
    protoEngineClient: protoClient,
  });
  assert.equal(executable.action, 'check');
});

test('a multi-word hero name keeps hero actions attributed to the hero', () => {
  const multiwordFixture = fixtures[1];
  assert.ok(multiwordFixture);
  const room = structuredClone(multiwordFixture.state.room);
  // The hero and an opponent share a first name token; first-token matching
  // would attribute the hero's raise to the opponent and flip heroWasRaiser.
  // Full-name startsWith matching must distinguish them, mirroring v0.
  for (const seat of room.seats) {
    if (!seat)
      continue;
    if (seat.seat === room.hero.seat)
      seat.name = 'John Doe';
    else
      seat.name = 'John Smith';
  }
  room.events = [
    { kind: 'raise', text: 'John Smith raise' },
    { kind: 'call', text: 'John Doe call' },
  ];
  const request = toV1DecisionRequest(room, { style: 'tag', heroName: 'John Doe' });
  const actors = request.state!.actionHistory.map(event => event.actorPlayerId);
  assert.deepEqual(actors, [
    `seat-${room.seats.find(s => s?.name === 'John Smith')!.seat}`,
    `seat-${room.hero.seat}`,
  ]);
  // Longest-prefix tie-break: a bare "John ..." event cannot match the hero
  // full name, so it goes to the opponent rather than the hero.
  const ambiguous = structuredClone(room);
  ambiguous.events = [{ kind: 'check', text: 'John Q check' }];
  const ambiguousRequest = toV1DecisionRequest(ambiguous, {
    style: 'tag',
    heroName: 'John Doe',
  });
  assert.notEqual(ambiguousRequest.state!.actionHistory[0]?.actorPlayerId,
    `seat-${room.hero.seat}`);
});

test('short heads-up effective stack jams identically on both transports', async t => {
  // Adversarial room from review: hero BB holds AA 1980 behind, SB limped and
  // has only 180 behind (200 total = 10bb). The server effectiveStackBb is 10;
  // the v1 mapper must reconstruct that from structured players so both
  // transports jam to the 200 target rather than treat hero as 100bb deep.
  const room: GoldenFixture['state']['room'] = {
    id: 'room-short',
    revision: 2,
    handId: 'hand-short-eff',
    street: 'preflop',
    blinds: [10, 20],
    hero: {
      seat: 4,
      name: 'Hero Long Name',
      stack: 1980,
      effectiveStack: 1980,
      effectiveStackBb: 10,
      position: 'BB',
      cards: [{ rank: 'A', suit: 'spades' }, { rank: 'A', suit: 'hearts' }],
      status: 'active',
      bet: 20,
    },
    dealer: 5,
    board: [],
    pot: 40,
    pots: [{ size: 40, eligible: [4, 5] }],
    seats: [
      { seat: 4, name: 'Hero Long Name', stack: 1980, status: 'active', bet: 20, blind: 'BB', position: 'BB' },
      { seat: 5, name: 'Short Stack Opp', stack: 180, status: 'active', bet: 20, blind: 'SB', position: 'BTN' },
    ],
    events: [
      { kind: 'call', text: 'Short Stack Opp call' },
    ],
    solver: {
      format: 'river-gto-v1',
      game: 'NLHE',
      street: 'preflop',
      heroPosition: 'BB',
      playersInHand: 2,
      potBb: 2,
      effectiveStackBb: 10,
      spr: 10,
      toCallBb: 0,
      potOdds: 0,
    },
    legal: {
      actions: ['fold', 'check', 'raise'],
      call: 0,
      potOdds: 0,
      currentBet: 20,
      amountMeaning: 'target-total',
      min: 40,
      max: 200,
      raiseTo: { min: 40, max: 200 },
    },
  };
  const config = { style: 'tag' as const, heroName: 'Hero Long Name', timeoutMs: 10_000 };
  const v0Context = buildContext(room, config);
  assert.equal(v0Context.effectiveStackBb, 10, 'v0 server effective stack is 10bb');

  const v1Request = toV1DecisionRequest(room, config);
  const actors = v1Request.state!.actionHistory.map(event => event.actorPlayerId);
  assert.deepEqual(actors, ['seat-5'], 'multi-word opponent limp attributed to seat 5');

  // Real binary on both transports.
  const jsonClient = v0Client();
  const protoClient = v1Client();
  t.after(() => {
    jsonClient.stop();
    protoClient.stop();
  });
  const v0Decision = await jsonClient.request(v0Context, 10_000);
  assert.equal(v0Decision.action, 'raise');
  assert.equal(v0Decision.amount ?? 0, 200);
  const response = await protoClient.request(
    decisionEnvelope(v1Request), 10_000,
  ) as Envelope;
  assert.equal(response.payload.case, 'decisionResponse');
  if (response.payload.case !== 'decisionResponse') throw new Error('bad payload');
  assert.equal(response.payload.value.result.case, 'strategy',
    'short effective stack is a valid jam, not an error');
  const v1Decision = fromV1DecisionResponse(response, room);
  assert.equal(v1Decision.action, 'raise');
  assert.equal(v1Decision.amount, 200);
});

import type { TestContext } from 'node:test';

// Runs one room through BOTH real transports and asserts action kind and
// exact target agree. Used for the skeptic's cross-gate effective-stack
// rooms: the physical stack straddles 12bb but the server hint pins it at 12.
async function assertDualTransportParity(
  t: TestContext,
  room: GoldenFixture['state']['room'],
  heroName: string,
  expectedAction: Action | null,
  expectedAmount: number | null,
): Promise<{ action: Action; amount: number }> {
  const config = { style: 'tag' as const, heroName, timeoutMs: 10_000 };
  const v0Context = buildContext(room, config);
  const v1Request = toV1DecisionRequest(room, config);
  assert.ok(typeof v1Request.options?.preflopEffectiveStackBb === 'number'
    && Number.isFinite(v1Request.options.preflopEffectiveStackBb)
    && v1Request.options.preflopEffectiveStackBb > 0,
    'v1 carries a finite positive effective-stack hint (or the 100 bb default)');
  const jsonClient = v0Client();
  const protoClient = v1Client();
  t.after(() => {
    jsonClient.stop();
    protoClient.stop();
  });
  const v0 = await jsonClient.request(v0Context, 10_000);
  const response = await protoClient.request(
    decisionEnvelope(v1Request), 10_000,
  ) as Envelope;
  if (response.payload.case !== 'decisionResponse') throw new Error('bad payload');
  assert.equal(response.payload.value.result.case, 'strategy');
  const v1 = fromV1DecisionResponse(response, room);
  assert.equal(v1.action, v0.action,
    `dual-transport action parity (v0 ${v0.action} vs v1 ${v1.action})`);
  assert.equal(v1.amount ?? 0, v0.amount ?? 0,
    `dual-transport target parity (v0 ${v0.amount ?? 0} vs v1 ${v1.amount ?? 0})`);
  if (expectedAction !== null)
    assert.equal(v0.action, expectedAction, `v0 action ${v0.action}`);
  if (expectedAmount !== null) {
    assert.equal(v0.amount ?? 0, expectedAmount);
    assert.equal(v1.amount, expectedAmount);
  }
  return { action: v0.action, amount: v0.amount ?? 0 };
}

test('cross-gate effective stack hint: HU 260 total, server 12bb', async t => {
  await assertDualTransportParity(
    t,
    {
      id: 'room-gate-hu260',
      revision: 1,
      handId: 'hand-gate-hu260',
      street: 'preflop',
      blinds: [10, 20],
      hero: {
        seat: 1,
        name: 'Hero',
        stack: 240,
        effectiveStackBb: 12,
        position: 'BB',
        cards: [{ rank: 'A', suit: 'spades' }, { rank: 'J', suit: 'hearts' }],
        status: 'active',
        bet: 20,
      },
      dealer: 0,
      board: [],
      pot: 40,
      pots: [{ size: 40, eligible: [0, 1] }],
      seats: [
        { seat: 0, name: 'Villain', stack: 240, status: 'active', bet: 20, blind: 'SB', position: 'BTN' },
        { seat: 1, name: 'Hero', stack: 240, status: 'active', bet: 20, blind: 'BB', position: 'BB' },
      ],
      events: [{ kind: 'call', text: 'Villain call' }],
      solver: {
        format: 'river-gto-v1',
        game: 'NLHE',
        street: 'preflop',
        heroPosition: 'BB',
        playersInHand: 2,
        potBb: 2,
        effectiveStackBb: 12,
        spr: 12,
        toCallBb: 0,
        potOdds: 0,
      },
      legal: {
        actions: ['fold', 'check', 'raise'],
        call: 0,
        potOdds: 0,
        currentBet: 20,
        amountMeaning: 'target-total',
        min: 40,
        max: 260,
        raiseTo: { min: 40, max: 260 },
      },
    },
    'Hero', null, null,
  );
});

test('multi-word opener parity: 4-way CO opener defaults like v0', async t => {
  await assertDualTransportParity(
    t,
    {
      id: 'room-mw-opener',
      revision: 1,
      handId: 'hand-mw-opener',
      street: 'preflop',
      blinds: [10, 20],
      hero: {
        seat: 4,
        name: 'Hero',
        stack: 1980,
        effectiveStackBb: 99,
        position: 'HJ',
        cards: [{ rank: 'A', suit: 'clubs' }, { rank: 'J', suit: 'spades' }],
        status: 'active',
      },
      dealer: 5,
      board: [],
      pot: 80,
      pots: [{ size: 80, eligible: [1, 3, 4, 5] }],
      seats: [
        { seat: 1, name: 'Sir Lancelot', stack: 1940, status: 'active', bet: 60, position: 'CO' },
        { seat: 3, name: 'Folded MP', stack: 2000, status: 'folded' },
        { seat: 4, name: 'Hero', stack: 1980, status: 'active', position: 'HJ' },
        { seat: 5, name: 'Button Player', stack: 1980, status: 'folded', position: 'BTN' },
      ],
      // v0 first-token opener "Sir" matches no seat -> openerPosition "" -> HJ;
      // both transports therefore evaluate AJs as vs HJ open (fold), not CO.
      events: [{ kind: 'raise', text: 'Sir Lancelot raise' }],
      solver: {
        format: 'river-gto-v1',
        game: 'NLHE',
        street: 'preflop',
        heroPosition: 'HJ',
        playersInHand: 4,
        potBb: 4,
        effectiveStackBb: 99,
        spr: 99,
        toCallBb: 3,
        potOdds: 0.1,
      },
      legal: {
        actions: ['fold', 'call', 'raise'],
        call: 60,
        potOdds: 0.1,
        currentBet: 60,
        amountMeaning: 'target-total',
        min: 60,
        max: 1980,
        raiseTo: { min: 120, max: 1980 },
      },
    },
    'Hero', 'fold', null,
  );
});

// Limps before the opener advance the v1 replay group; the opener shim must
// still select the first preflop raise independent of group. Verified for 2 and
// 3 limpers (0/1 limp are covered by the existing opener and frozen tests).
function limpOpenerRoom(limpers: number): GoldenFixture['state']['room'] {
  // 6-max: UTG/HJ hero, multi-word CO opener after N limpers.
  const seats: GoldenFixture['state']['room']['seats'] = [
    { seat: 0, name: 'Folded UTG', stack: 2000, status: 'folded' },
    { seat: 1, name: 'Hero', stack: 1980, status: 'active', position: 'HJ' },
  ];
  const limperSeats = [2, 3, 4] as const;
  const limperPositions = ['CO', 'BTN', 'SB'] as const;
  for (let i = 0; i < limpers; i++) {
    const limperSeat = limperSeats[i]!;
    seats.push({
      seat: limperSeat,
      name: `Limper Number ${i + 1}`,
      stack: 1980,
      status: 'folded',
      bet: 20,
      position: limperPositions[i]!,
    });
  }
  seats.push(
    { seat: 5, name: 'Chen Meng', stack: 1940, status: 'active', bet: 60, position: 'CO' },
  );
  const events: GoldenFixture['state']['room']['events'] = [];
  for (let i = 0; i < limpers; i++)
    events.push({ kind: 'call', text: `Limper Number ${i + 1} call` });
  events.push({ kind: 'raise', text: 'Chen Meng raise' });
  return {
    id: `room-limp${limpers}-opener`,
    revision: 1,
    handId: `hand-limp${limpers}-opener`,
    street: 'preflop',
    blinds: [10, 20],
    hero: {
      seat: 1,
      name: 'Hero',
      stack: 1980,
      effectiveStackBb: 99,
      position: 'HJ',
      cards: [{ rank: 'A', suit: 'clubs' }, { rank: 'J', suit: 'spades' }],
      status: 'active',
    },
    dealer: 5,
    board: [],
    pot: 60 + limpers * 20,
    seats,
    events,
    solver: {
      format: 'river-gto-v1',
      game: 'NLHE',
      street: 'preflop',
      heroPosition: 'HJ',
      playersInHand: limpers + 2,
      potBb: 3 + limpers,
      effectiveStackBb: 99,
      spr: 99,
      toCallBb: 3,
      potOdds: 0.1,
    },
    legal: {
      actions: ['fold', 'call', 'raise'],
      call: 60,
      potOdds: 0.1,
      currentBet: 60,
      amountMeaning: 'target-total',
      min: 60,
      max: 1980,
      raiseTo: { min: 120, max: 1980 },
    },
  };
}

for (const limpers of [2, 3]) {
  test(`multi-word opener after ${limpers} limpers keeps v0 HJ parity`, async t => {
    // v0 first-token "Chen" matches no seat -> no opener -> HJ default on both.
    await assertDualTransportParity(t, limpOpenerRoom(limpers), 'Hero', null, null);
  });
}

test('server effectiveStackBb 0 and missing both sanitize to 100 on both transports', async t => {
  // A deep (99bb-structured) room whose server reports 0 must behave like v0's
  // sanitized 100bb, not like an invalid hint (which would force a fallback).
  const base = limpOpenerRoom(0);
  for (const [label, eff] of [['zero', 0], ['missing', undefined]] as const) {
    const room = structuredClone(base);
    room.solver = { ...(room.solver as NonNullable<typeof room.solver>) };
    room.hero = { ...room.hero };
    if (eff === undefined) {
      delete (room.solver as { effectiveStackBb?: number }).effectiveStackBb;
      delete room.hero.effectiveStackBb;
    } else {
      room.solver.effectiveStackBb = eff;
      room.hero.effectiveStackBb = eff;
    }
    const request = toV1DecisionRequest(room, { style: 'tag', heroName: 'Hero' });
    assert.equal(request.options?.preflopEffectiveStackBb, 100,
      `${label} effective stack sanitizes to 100 hint`);
    await assertDualTransportParity(t, room, 'Hero', null, null);
  }
});

test('a side-pot hand fails closed with the operational fallback', async t => {
  const sidePotFixture = fixtures[1];
  assert.ok(sidePotFixture);
  const room = structuredClone(sidePotFixture.state.room);
  room.pots = [
    { size: 80, eligible: [4, 5] },
    { size: 40, eligible: [4, 5] },
  ];
  room.pot = 120;
  const protoClient = v1Client();
  t.after(() => protoClient.stop());
  const executable = await decide(room, {
    style: 'tag',
    heroName: 'Hero',
    timeoutMs: 10_000,
    proto: true,
    protoEngineClient: protoClient,
  });
  assert.match(executable.reason, /^safe-fallback:/);
});

test('unopened big blind option with raise legal survives strict validation', async t => {
  // Structural heads-up preflop big-blind option: hero posted BB, nothing more
  // owed, no voluntary raise yet, legal fold/check/raise with an absolute
  // target. The strict validator must accept it and the engine must raise.
  const room = {
    id: 'room-bb',
    revision: 2,
    handId: 'hand-bb-option',
    street: 'preflop' as const,
    blinds: [10, 20] as [number, number],
    hero: {
      seat: 1,
      name: 'Hero',
      stack: 1980,
      effectiveStack: 1980,
      effectiveStackBb: 99,
      position: 'BB',
      cards: [{ rank: 'A', suit: 'spades' }, { rank: 'K', suit: 'hearts' }],
      status: 'active' as const,
      bet: 20,
    },
    dealer: 0,
    board: [],
    pot: 30,
    pots: [{ size: 30, eligible: [0, 1] }],
    seats: [
      { seat: 0, name: 'BTN', stack: 1990, status: 'active' as const, position: 'BTN', blind: 'SB', bet: 10 },
      { seat: 1, name: 'Hero', stack: 1980, status: 'active' as const, position: 'BB', blind: 'BB', bet: 20 },
    ],
    events: [],
    legal: {
      actions: ['fold', 'check', 'raise'] as Action[],
      call: 0,
      potOdds: 0,
      currentBet: 20,
      min: 20,
      max: 2000,
      amountMeaning: 'target-total' as const,
      raiseTo: { min: 40, max: 2000 },
    },
  };
  const request = toV1DecisionRequest(room as unknown as GoldenFixture['state']['room'], {
    style: 'tag',
    heroName: 'Hero',
  });
  assert.equal(request.state!.legalActions.length, 3);
  const protoClient = v1Client();
  t.after(() => protoClient.stop());
  const envelope = await protoClient.request(decisionEnvelope(request), 10_000) as Envelope;
  assert.equal(envelope.payload.case, 'decisionResponse');
  if (envelope.payload.case !== 'decisionResponse') throw new Error('bad payload');
  assert.equal(envelope.payload.value.result.case, 'strategy',
    'big blind option is valid, not an engine error');
});

test('v1 transport errors fail closed through the runner fallback', async t => {  const invalidRangeFixture = fixtures[0];
  assert.ok(invalidRangeFixture);
  const room = structuredClone(invalidRangeFixture.state.room);
  // An impossible target makes the structured request invalid; the runner
  // surfaces the operational safe fallback rather than a fold-shaped engine
  // strategy.
  if (room.legal?.raiseTo) {
    const raiseTo = room.legal.raiseTo;
    room.legal.raiseTo = { min: raiseTo.min, max: raiseTo.min - 10 };
  }
  const protoClient = v1Client();
  t.after(() => protoClient.stop());
  const decision = await decide(room, {
    style: 'tag',
    heroName: 'Hero',
    timeoutMs: 10_000,
    proto: true,
    protoEngineClient: protoClient,
  });
  assert.match(decision.reason, /^safe-fallback:/);
});
