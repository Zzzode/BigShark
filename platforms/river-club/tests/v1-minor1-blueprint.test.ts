import assert from 'node:assert/strict';
import { create } from '@bufbuild/protobuf';
import { spawnSync } from 'node:child_process';
import { existsSync, mkdtempSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { dirname, join, resolve } from 'node:path';
import test from 'node:test';
import { fileURLToPath } from 'node:url';

import { ProtoEngineProcessClient } from '../../../clients/node/proto-engine-process-client.js';
import {
  ActionType,
  DecisionResponseSchema,
  DecisionRequestSchema,
  EnvelopeSchema,
  ExpandedStrategySchema,
  ForcedContributionType,
  GetCapabilitiesRequestSchema,
  SelectedActionSchema,
  PlayerStatus,
  Rank,
  SolverMode,
  SolverSource,
  Street,
  Suit,
  type DecisionRequest,
  type Envelope,
} from '../../../build/generated/ts/bigshark/engine/v1/engine_pb.js';
import { fromV1DecisionResponse } from '../src/v1-mapper.js';
import type { RiverRoom } from '../src/types.js';

const here = dirname(fileURLToPath(import.meta.url));
const root = resolve(here, '../../../../');
const engineBinary = process.env.BIGSHARK_ENGINE_BINARY
  || join(root, 'bin', 'bigshark-engine');
const fixturePublisher = process.env.BS_RESIDENT_FIXTURE_BIN
  || join(root, 'bin', 'bs-resident-fixture');

interface PublishedFixture {
  path: string;
  sha256: string;
  rootActionCount: number;
}

function publishFixture(): PublishedFixture | null {
  if (!existsSync(engineBinary) || !existsSync(fixturePublisher))
    return null;
  const dir = mkdtempSync(join(tmpdir(), 'bs-stage8-'));
  const published = spawnSync(fixturePublisher, [dir], { encoding: 'utf8' });
  if (published.status !== 0)
    throw new Error(`fixture publisher failed: ${published.stderr}`);
  return JSON.parse(published.stdout.trim()) as PublishedFixture;
}

const card = (rank: Rank, suit: Suit) => ({ rank, suit });

// A minor-1 BLUEPRINT request at the published fixture root: two players,
// button on seat 1, hero (seat 0, non-button) holds AsKs and acts first on the
// flop. Legal check plus bet targets covering the full abstract window.
function blueprintRequest(mode: SolverMode, seed = 42n): DecisionRequest {
  return create(DecisionRequestSchema, {
    state: {
      game: {
        variant: 1,
        bettingStructure: 1,
        gameType: 1,
        tableCapacity: 2,
        amountUnit: { name: 'chip', decimalPlaces: 0 },
        smallBlind: 2n,
        bigBlind: 5n,
      },
      handId: 'fixture-root',
      decisionIndex: 1n,
      street: Street.FLOP,
      buttonSeat: 1,
      heroPlayerId: 'hero',
      players: [
        { playerId: 'hero', seat: 0, stack: 40n, status: PlayerStatus.ACTIVE },
        { playerId: 'villain', seat: 1, stack: 40n, status: PlayerStatus.ACTIVE },
      ],
      heroHoleCards: [card(Rank.ACE, Suit.SPADES), card(Rank.KING, Suit.SPADES)],
      board: [
        card(Rank.TWO, Suit.CLUBS),
        card(Rank.THREE, Suit.DIAMONDS),
        card(Rank.SEVEN, Suit.HEARTS),
      ],
      pot: { potTotal: 20n, mainPot: 20n },
      forcedContributions: [
        { playerId: 'hero', type: ForcedContributionType.SMALL_BLIND, amount: 2n },
        { playerId: 'villain', type: ForcedContributionType.BIG_BLIND, amount: 5n },
      ],
      actionHistory: [],
      legalActions: [
        { type: ActionType.CHECK },
        { type: ActionType.BET, minTargetTotal: 5n, maxTargetTotal: 40n },
      ],
      toCall: 0n,
    },
    options: {
      strategyProfile: 'tag',
      solveTimeBudgetMs: 1000,
      seed,
      includeSampledAction: true,
      includeFullStrategy: false,
      solverMode: mode,
    },
  });
}

function decisionEnvelope(request: DecisionRequest, minor: 0 | 1): Envelope {
  const envelope = create(EnvelopeSchema, { protocolMinor: minor });
  envelope.payload = { case: 'decisionRequest', value: request };
  return envelope;
}

function capabilitiesEnvelope(minor: 0 | 1): Envelope {
  const envelope = create(EnvelopeSchema, {
    protocolMinor: minor,
    requestId: `caps-${minor}-${Math.random()}`,
  });
  envelope.payload = {
    case: 'getCapabilitiesRequest',
    value: create(GetCapabilitiesRequestSchema),
  };
  return envelope;
}

function residentClient(fixture: PublishedFixture, pin = fixture.sha256):
    ProtoEngineProcessClient {
  return new ProtoEngineProcessClient({
    command: engineBinary,
    args: ['--serve-proto', '--resident-root', `${fixture.path}=${pin}`],
    warmupEnvelope: capabilitiesEnvelope(0),
    warmupTimeoutMs: 10_000,
    negotiateMinor1: true,
  });
}

const fixture = publishFixture();

test('real binary: minor-1 capability handshake advertises BLUEPRINT with a root',
  { skip: fixture === null ? 'engine or fixture binary unavailable' : false },
  async t => {
    if (!fixture) return;
    const client = residentClient(fixture);
    t.after(() => client.stop());
    await client.request(capabilitiesEnvelope(1), 10_000);
    assert.equal(client.minor1Capable, true);
    const caps = await client.request(capabilitiesEnvelope(1), 10_000);
    if (caps.payload.case !== 'getCapabilitiesResponse') throw new Error('bad payload');
    assert.deepEqual(
      [...caps.payload.value.supportedProtocolMinors],
      [0, 1],
    );
    assert.ok(caps.payload.value.solverModes.includes(SolverMode.BLUEPRINT));
    assert.ok(!caps.payload.value.solverModes.includes(SolverMode.RESOLVING),
      'RESOLVING is never advertised');
  });

test('real binary: forced BLUEPRINT hit returns the full (>5) distribution',
  { skip: fixture === null ? 'engine or fixture binary unavailable' : false },
  async t => {
    if (!fixture) return;
    const client = residentClient(fixture);
    t.after(() => client.stop());
    const response = await client.request(
      decisionEnvelope(blueprintRequest(SolverMode.BLUEPRINT), 1),
      10_000,
    );
    assert.equal(response.protocolMinor, 1);
    if (response.payload.case !== 'decisionResponse') throw new Error('bad payload');
    const result = response.payload.value.result;
    assert.equal(result.case, 'expandedStrategy');
    if (result.case !== 'expandedStrategy') throw new Error('bad result');
    const expanded = result.value;
    assert.ok(expanded.actions.length > 5,
      `resident distribution carries ${expanded.actions.length} actions, no five-entry cap`);
    assert.ok(expanded.actions.length <= 32);
    let sum = 0;
    for (const policy of expanded.actions)
      sum += policy.probability;
    assert.ok(Math.abs(sum - 1) <= 1e-9, 'probabilities sum to one');
    assert.equal(expanded.solver?.source, SolverSource.BLUEPRINT);
    assert.equal(expanded.solver?.artifactSha256, fixture.sha256);
    assert.equal(expanded.solver?.guarantee, 'uncertified');
    // Selected action is a member of the full distribution by kind and exact
    // target.
    const selected = expanded.selectedAction;
    assert.ok(selected, 'sampled action present');
    assert.ok(expanded.actions.some(policy =>
      policy.type === selected?.type
      && (policy.targetTotal ?? 0n) === (selected?.targetTotal ?? 0n)),
    'selected action is a distribution member');
    // The jam row is the hero's full stack target and is marked all-in.
    const jam = [...expanded.actions].reverse()
      .find(policy => policy.type === ActionType.BET);
    assert.ok(jam?.allIn, 'maximum bet row is all-in');
  });

test('real binary: sampling changes only with the seed, never the distribution',
  { skip: fixture === null ? 'engine or fixture binary unavailable' : false },
  async t => {
    if (!fixture) return;
    const clientA = residentClient(fixture);
    const clientB = residentClient(fixture);
    t.after(() => {
      clientA.stop();
      clientB.stop();
    });
    const a = await clientA.request(
      decisionEnvelope(blueprintRequest(SolverMode.BLUEPRINT, 1n), 1), 10_000);
    const b = await clientB.request(
      decisionEnvelope(blueprintRequest(SolverMode.BLUEPRINT, 2n), 1), 10_000);
    if (a.payload.case !== 'decisionResponse' || b.payload.case !== 'decisionResponse')
      throw new Error('bad payload');
    if (a.payload.value.result.case !== 'expandedStrategy'
      || b.payload.value.result.case !== 'expandedStrategy')
      throw new Error('bad result');
    const ea = a.payload.value.result.value;
    const eb = b.payload.value.result.value;
    assert.equal(ea.actions.length, eb.actions.length);
    for (let i = 0; i < ea.actions.length; i += 1) {
      assert.equal(ea.actions[i]?.probability, eb.actions[i]?.probability);
      assert.equal(ea.actions[i]?.targetTotal, eb.actions[i]?.targetTotal);
    }
  });

test('real binary: forced BLUEPRINT on an unsupported root is a non-retryable error',
  { skip: fixture === null ? 'engine or fixture binary unavailable' : false },
  async t => {
    if (!fixture) return;
    const client = residentClient(fixture);
    t.after(() => client.stop());
    // Change one flop card: a different root identity -> RootNotSupported.
    const request = blueprintRequest(SolverMode.BLUEPRINT);
    const firstBoardCard = request.state?.board[0];
    assert.ok(firstBoardCard);
    firstBoardCard.rank = Rank.TWO;
    firstBoardCard.suit = Suit.SPADES;
    const response = await client.request(decisionEnvelope(request, 1), 10_000);
    if (response.payload.case !== 'decisionResponse') throw new Error('bad payload');
    assert.equal(response.payload.value.result.case, 'error');
    if (response.payload.value.result.case !== 'error') throw new Error('bad result');
    const error = response.payload.value.result.value;
    assert.equal(error.retryable, false);
  });

test('real binary: a wrong digest pin advertises no blueprint and forces a miss',
  { skip: fixture === null ? 'engine or fixture binary unavailable' : false },
  async t => {
    if (!fixture) return;
    const wrongPin = 'f'.repeat(64);
    const client = residentClient(fixture, wrongPin);
    t.after(() => client.stop());
    await client.request(capabilitiesEnvelope(1), 10_000);
    assert.equal(client.minor1Capable, true, 'minor 1 still negotiates');
    const caps = await client.request(capabilitiesEnvelope(1), 10_000);
    if (caps.payload.case !== 'getCapabilitiesResponse') throw new Error('bad payload');
    assert.ok(!caps.payload.value.solverModes.includes(SolverMode.BLUEPRINT),
      'no advertised roots means BLUEPRINT is hidden');
    const response = await client.request(
      decisionEnvelope(blueprintRequest(SolverMode.BLUEPRINT), 1), 10_000);
    if (response.payload.case !== 'decisionResponse') throw new Error('bad payload');
    assert.equal(response.payload.value.result.case, 'error');
  });

test('real binary: non-opted-in client stays minor 0 even with a blueprint installed',
  { skip: fixture === null ? 'engine or fixture binary unavailable' : false },
  async t => {
    if (!fixture) return;
    const client = new ProtoEngineProcessClient({
      command: engineBinary,
      args: ['--serve-proto', '--resident-root', `${fixture.path}=${fixture.sha256}`],
      warmupEnvelope: capabilitiesEnvelope(0),
      warmupTimeoutMs: 10_000,
      // negotiateMinor1 omitted: stays on minor 0.
    });
    t.after(() => client.stop());
    // BLUEPRINT mode is UNSUPPORTED_FEATURE on minor 0.
    const forced = await client.request(
      decisionEnvelope(blueprintRequest(SolverMode.BLUEPRINT), 0), 10_000);
    if (forced.payload.case !== 'decisionResponse') throw new Error('bad payload');
    assert.equal(forced.payload.value.result.case, 'error');
    // AUTOMATIC is the degenerate v1.0 Strategy, never ExpandedStrategy.
    const automatic = await client.request(
      decisionEnvelope(blueprintRequest(SolverMode.AUTOMATIC), 0), 10_000);
    assert.equal(automatic.protocolMinor, 0);
    if (automatic.payload.case !== 'decisionResponse') throw new Error('bad payload');
    assert.equal(automatic.payload.value.result.case, 'strategy');
  });

test('real binary: RESOLVING (7) is rejected on minor 1',
  { skip: fixture === null ? 'engine or fixture binary unavailable' : false },
  async t => {
    if (!fixture) return;
    const client = residentClient(fixture);
    t.after(() => client.stop());
    const response = await client.request(
      decisionEnvelope(blueprintRequest(SolverMode.RESOLVING), 1), 10_000);
    if (response.payload.case !== 'decisionResponse') throw new Error('bad payload');
    assert.equal(response.payload.value.result.case, 'error');
  });

function expandedEnvelope(source: SolverSource, guarantee?: string): Envelope {
  const envelope = create(EnvelopeSchema, { protocolMinor: 1, requestId: 'mapper' });
  const decision = create(DecisionResponseSchema);
  decision.result = {
    case: 'expandedStrategy',
    value: create(ExpandedStrategySchema, {
      actions: [
        { type: ActionType.CHECK, probability: 0.6 },
        { type: ActionType.BET, targetTotal: 200n, probability: 0.4 },
      ],
      selectedAction: { type: ActionType.BET, targetTotal: 200n },
      solver: {
        source,
        reasonCode: source === SolverSource.BLUEPRINT ? 'blueprint' : 'postflop-heuristic',
        ...(guarantee ? { guarantee } : {}),
      },
    }),
  };
  envelope.payload = { case: 'decisionResponse', value: decision };
  return envelope;
}

const minimalRoom = {
  legal: {
    actions: ['check', 'bet'],
    call: 0,
    potOdds: 0,
    currentBet: 0,
    amountMeaning: 'target-total' as const,
    min: 200,
    max: 400,
    raiseTo: { min: 200, max: 400 },
  },
} as unknown as RiverRoom;

test('mapper executes the sampled action of a full expanded distribution', () => {
  const executable = fromV1DecisionResponse(
    expandedEnvelope(SolverSource.BLUEPRINT, 'uncertified'),
    minimalRoom,
  );
  assert.equal(executable.action, 'bet');
  assert.equal(executable.amount, 200);
  assert.match(executable.reason, /blueprint/);
});

test('mapper rejects an expanded selected action outside its own distribution', () => {
  const envelope = expandedEnvelope(SolverSource.BLUEPRINT, 'uncertified');
  if (envelope.payload.case !== 'decisionResponse') throw new Error('bad payload');
  if (envelope.payload.value.result.case !== 'expandedStrategy') throw new Error('bad result');
  envelope.payload.value.result.value.selectedAction = create(SelectedActionSchema, {
    type: ActionType.BET,
    targetTotal: 999n,
  });
  assert.throws(() => fromV1DecisionResponse(envelope, minimalRoom),
    /not a member/);
});

test('mapper rejects an expanded action outside the platform legal window', () => {
  const room = {
    legal: {
      actions: ['check', 'bet'],
      raiseTo: { min: 300, max: 400 },
    },
  } as unknown as RiverRoom;
  assert.throws(() => fromV1DecisionResponse(
    expandedEnvelope(SolverSource.BLUEPRINT, 'uncertified'), room),
    /legality validation/);
});
