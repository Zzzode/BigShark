import assert from 'node:assert/strict';
import {
  create,
  toBinary,
  fromBinary,
  type MessageInitShape,
} from '@bufbuild/protobuf';
import { dirname, join } from 'node:path';
import test from 'node:test';
import { fileURLToPath } from 'node:url';

import {
  ProtoEngineProcessClient,
  warmupEnvelope,
} from '../proto-engine-process-client.js';
import {
  ActionType,
  DecisionRequestSchema,
  EnvelopeSchema,
  GuaranteeLevel,
  ObservationCoverage,
  ObservationSource,
  PotPhase,
  LayerStatus,
  SettlementRule,
  ReopeningRule,
  RunoutRule,
  SolverMode,
  Street,
  UtilityProfile,
  type Envelope,
} from '../../../build/generated/ts/bigshark/engine/engine_pb.js';

const here = dirname(fileURLToPath(import.meta.url));
const fakeEngine = join(here, 'fixtures', 'fake-proto-engine.js');

function decisionEnvelope(handId: string, toCall = 0n): Envelope {
  const envelope = create(EnvelopeSchema, {
    protocolVersion: 3,
    requestId: `${handId}-${Math.random()}`,
  });
  const decisionInit: MessageInitShape<typeof DecisionRequestSchema> = {
    state: {
      game: {
        variant: 1,
        bettingStructure: 1,
        gameType: 1,
        tableCapacity: 2,
        amountUnit: { name: 'chip', decimalPlaces: 0 },
        smallBlind: 10n,
        bigBlind: 20n,
        ruleProfile: {
          settlement: SettlementRule.CLOCKWISE_LEFT_OF_BUTTON,
          reopening: ReopeningRule.NEVER_CUMULATIVE,
          runout: RunoutRule.SINGLE,
          dealtSeatCount: 2,
        },
      },
      handId,
      decisionIndex: 1n,
      street: Street.FLOP,
      buttonSeat: 1,
      heroSeat: 0,
      players: [
        {
          seat: 1,
          startingStack: 2000n,
          stack: 1970n,
          streetCommitted: 30n,
          grossContributed: 30n,
          refunded: 0n,
          status: 1,
        },
        {
          seat: 0,
          startingStack: 2000n,
          stack: 1980n,
          streetCommitted: 20n,
          grossContributed: 20n,
          refunded: 0n,
          status: 1,
        },
      ],
      heroHoleCards: [
        { rank: 13, suit: 1 },
        { rank: 12, suit: 2 },
      ],
      board: [
        { rank: 5, suit: 3 },
        { rank: 8, suit: 1 },
        { rank: 2, suit: 2 },
      ],
      pot: {
        phase: PotPhase.ACTION,
        layers: [
          {
            contributionLevel: 20n,
            amount: 40n,
            eligibleSeats: [0, 1],
            status: LayerStatus.PENDING,
          },
        ],
        potTotal: 40n,
      },
      transitionPath: [],
      legalActions: [{ type: ActionType.CHECK }],
      toCall,
      provenance: {
        source: ObservationSource.OFFICIAL_RESPONSE,
        coverage: ObservationCoverage.FULL,
        controllerEpoch: 'test-epoch',
        observedEventCount: 0,
      },
    },
    options: {
      strategyProfile: 'tag',
      solveTimeBudgetMs: 1000,
      seed: 1n,
      solverMode: SolverMode.AUTOMATIC,
      utilityProfile: UtilityProfile.CASH_CHIP,
    },
  };
  const decisionRequest = create(DecisionRequestSchema, decisionInit);
  envelope.payload = { case: 'decisionRequest', value: decisionRequest };
  return envelope;
}

function client(): ProtoEngineProcessClient {
  return new ProtoEngineProcessClient({
    command: process.execPath,
    args: [fakeEngine],
    warmupEnvelope: warmupEnvelope(),
    warmupTimeoutMs: 1000,
  });
}

test('warmup, capabilities handshake, and echo', async t => {
  const engine = client();
  t.after(() => engine.stop());

  const response = await engine.request(decisionEnvelope('echo'));
  assert.equal(response.payload.case, 'decisionResponse');
  assert.equal(response.protocolVersion, 3);
  if (response.payload.case === 'decisionResponse') {
    const result = response.payload.value.result;
    assert.equal(result.case, 'strategy');
    if (result.case === 'strategy') {
      assert.equal(result.value.selectedAction?.type, ActionType.CHECK);
    }
  }
  // Capabilities were stored during handshake.
  assert.ok(engine.capabilities.length > 0);
  assert.equal(engine.buildVersion, 'fake-test-build');
  assert.equal(engine.protocolVersion, 3);
});

test('split frames across arrivals', async t => {
  const engine = client();
  t.after(() => engine.stop());

  const split = await engine.request(decisionEnvelope('split'));
  assert.equal(split.payload.case, 'decisionResponse');
});

test('consecutive requests', async t => {
  const engine = client();
  t.after(() => engine.stop());

  const first = await engine.request(decisionEnvelope('first'));
  const second = await engine.request(decisionEnvelope('second'));
  assert.equal(first.payload.case, 'decisionResponse');
  assert.equal(second.payload.case, 'decisionResponse');
});

test('bigint target echo', async t => {
  const engine = client();
  t.after(() => engine.stop());

  const response = await engine.request(decisionEnvelope('bigint', 150n));
  assert.equal(response.payload.case, 'decisionResponse');
  if (response.payload.case === 'decisionResponse') {
    const result = response.payload.value.result;
    if (result.case === 'strategy') {
      assert.equal(result.value.selectedAction?.targetTotal, 150n);
    }
  }
});

test('expanded strategy response', async t => {
  const engine = client();
  t.after(() => engine.stop());

  const response = await engine.request(decisionEnvelope('expanded'));
  assert.equal(response.payload.case, 'decisionResponse');
  if (response.payload.case === 'decisionResponse') {
    const result = response.payload.value.result;
    assert.equal(result.case, 'expandedStrategy');
    if (result.case === 'expandedStrategy') {
      assert.equal(result.value.selectedAction?.type, ActionType.BET);
      assert.equal(result.value.selectedAction?.targetTotal, 200n);
      assert.equal(result.value.solver?.guaranteeLevel, 'approximate');
      assert.equal(result.value.solver?.utilityProfile, UtilityProfile.CASH_CHIP);
    }
  }
});

test('engine error response', async t => {
  const engine = client();
  t.after(() => engine.stop());

  const response = await engine.request(decisionEnvelope('error'));
  assert.equal(response.payload.case, 'decisionResponse');
  if (response.payload.case === 'decisionResponse') {
    const result = response.payload.value.result;
    assert.equal(result.case, 'error');
    if (result.case === 'error') {
      assert.equal(result.value.code, 13); // OFF_TREE_AMOUNT
    }
  }
});

test('frame round-trip encoding', () => {
  const envelope = decisionEnvelope('roundtrip');
  const payload = Buffer.from(toBinary(EnvelopeSchema, envelope));
  const decoded = fromBinary(EnvelopeSchema, payload);
  assert.equal(decoded.protocolVersion, 3);
  assert.equal(decoded.payload.case, 'decisionRequest');
});

test('request timeout', async t => {
  const engine = client();
  t.after(() => engine.stop());

  await assert.rejects(
    () => engine.request(decisionEnvelope('hang'), 100),
    /timed out/,
  );
});

test('oversize frame rejection', async t => {
  const engine = client();
  t.after(() => engine.stop());

  await assert.rejects(
    () => engine.request(decisionEnvelope('oversize'), 500),
    /1 MiB|malformed|undecodable/,
  );
});

test('truncated frame rejection', async t => {
  const engine = client();
  t.after(() => engine.stop());

  await assert.rejects(
    () => engine.request(decisionEnvelope('truncated'), 500),
    /timed out|exited/,
  );
});

test('malformed varint rejection', async t => {
  const engine = client();
  t.after(() => engine.stop());

  await assert.rejects(
    () => engine.request(decisionEnvelope('malformed-varint'), 500),
    /malformed/,
  );
});

test('process exit recovery', async t => {
  const engine = client();
  t.after(() => engine.stop());

  await assert.rejects(
    () => engine.request(decisionEnvelope('exit'), 500),
    /exited/,
  );
  // The client should be able to restart on the next request.
  const response = await engine.request(decisionEnvelope('recovery'));
  assert.equal(response.payload.case, 'decisionResponse');
});

test('duplicate request id rejection', async t => {
  const engine = client();
  t.after(() => engine.stop());

  const envelope = decisionEnvelope('dup');
  envelope.requestId = 'fixed-id';
  // First request starts the process and sends.
  const first = engine.request(envelope, 1000);
  // Second request with the same id should reject immediately.
  await assert.rejects(
    () => engine.request(envelope, 1000),
    /Duplicate/,
  );
  await first;
});
