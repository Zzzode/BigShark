// RFC 0008 stage 5 platform adapter tests against the scripted minor-2 fake
// engine (no published binary required): negotiation plumbing through decide(),
// field-11 decode onto ExecutableDecision, code-9 propagation (never a safe
// fallback), floor encoding, and the operational_fallback label on local
// fallbacks.
import assert from 'node:assert/strict';
import { dirname, join } from 'node:path';
import test from 'node:test';
import { fileURLToPath } from 'node:url';
import { readFileSync } from 'node:fs';

import { create } from '@bufbuild/protobuf';
import { ProtoEngineProcessClient } from '../../../clients/node/proto-engine-process-client.js';
import {
  ActionType,
  DecisionResponseSchema,
  EnvelopeSchema,
  ExpandedStrategySchema,
  GetCapabilitiesRequestSchema,
  SolverSource,
} from '../../../build/generated/ts/bigshark/engine/v1/engine_pb.js';
import { decide, safeFallback } from '../src/engine.js';
import { fromV1DecisionResponse, toV1DecisionRequest, V1EngineError } from '../src/v1-mapper.js';
import type { GoldenFixture } from '../src/types.js';

const here = dirname(fileURLToPath(import.meta.url));
const root = join(here, '../../../../');
const fakeEngine = join(root, 'dist/clients/node/tests/fixtures/fake-proto-engine.js');

const fixtures = JSON.parse(readFileSync(
  join(root, 'platforms/river-club/tests/fixtures/v0-golden.json'),
  'utf8',
)) as GoldenFixture[];

function warmup() {
  const envelope = create(EnvelopeSchema, { protocolMinor: 0 });
  envelope.payload = {
    case: 'getCapabilitiesRequest',
    value: create(GetCapabilitiesRequestSchema),
  };
  envelope.requestId = 'warmup-minor2';
  return envelope;
}

function minor2Client(): ProtoEngineProcessClient {
  return new ProtoEngineProcessClient({
    command: process.execPath,
    args: [fakeEngine],
    warmupEnvelope: warmup(),
    warmupTimeoutMs: 5_000,
    negotiateMinor2: true,
  });
}

test('minor-2 heuristic decision surfaces guaranteeLevel approximate from field 11', async t => {
  const client = minor2Client();
  t.after(() => client.stop());
  // Flop check-option fixture: the fake minor-2 host answers check with
  // guarantee_level approximate.
  const fixture = fixtures.find(f => f.name === 'multiway-flop-check-option')!;
  const decision = await decide(fixture.state.room, {
    style: 'tag',
    heroName: fixture.state.me.name,
    timeoutMs: 5_000,
    proto: true,
    protoEngineClient: client,
  });
  assert.equal(decision.action, 'check');
  assert.equal(decision.guaranteeLevel, 'approximate',
    'the executable decision carries the decoded field-11 level');
});

test('a floor refusal (code 9) propagates as V1EngineError, never a safe fallback', async t => {
  const client = minor2Client();
  t.after(() => client.stop());
  const fixture = fixtures.find(f => f.name === 'multiway-flop-check-option')!;
  await assert.rejects(
    decide(fixture.state.room, {
      style: 'tag',
      heroName: fixture.state.me.name,
      timeoutMs: 5_000,
      proto: true,
      protoEngineClient: client,
      minimumGuaranteeLevel: 'exact_solved',
    }),
    (error: unknown) =>
      error instanceof V1EngineError
      && error.code === 9
      && error.retryable === false
      && error.codeName === 'GUARANTEE_BELOW_REQUEST',
  );
});

test('a floor the host can meet serves the decision with its level', async t => {
  const client = minor2Client();
  t.after(() => client.stop());
  const fixture = fixtures.find(f => f.name === 'multiway-flop-check-option')!;
  const decision = await decide(fixture.state.room, {
    style: 'tag',
    heroName: fixture.state.me.name,
    timeoutMs: 5_000,
    proto: true,
    protoEngineClient: client,
    minimumGuaranteeLevel: 'approximate',
  });
  assert.equal(decision.action, 'check');
  assert.equal(decision.guaranteeLevel, 'approximate');
});

test('every local fallback producer labels operational_fallback', () => {
  const noRoom = safeFallback(null);
  assert.equal(noRoom.guaranteeLevel, 'operational_fallback');
  assert.match(noRoom.reason, /^safe-fallback:/);

  const fixture = fixtures.find(f => f.name === 'multiway-flop-check-option')!;
  const checkRoom = safeFallback(fixture.state.room);
  assert.equal(checkRoom.action, 'check');
  assert.equal(checkRoom.guaranteeLevel, 'operational_fallback');

  const betFacing = fixtures.find(f => f.name === 'multiway-turn-facing-raise')!;
  const callFallback = safeFallback(betFacing.state.room);
  assert.equal(callFallback.action, 'call');
  assert.equal(callFallback.guaranteeLevel, 'operational_fallback');

  // The distinct pre-legal {action:'fold',reason:'no-legal'} site.
  // decide() itself handles a missing legal set before touching the engine.
  const noLegal = { ...fixture.state.room, legal: null };
  return decide(noLegal, { proto: true }).then(decision => {
    assert.equal(decision.action, 'fold');
    assert.equal(decision.reason, 'no-legal');
    assert.equal(decision.guaranteeLevel, 'operational_fallback');
  });
});

// ---- Pure mapper discipline (no process) ---------------------------------

test('toV1DecisionRequest encodes minimum_guarantee only at negotiated minor 2', () => {
  const fixture = fixtures.find(f => f.name === 'multiway-flop-check-option')!;
  const room = fixture.state.room;
  // No minor: field absent.
  assert.equal(
    toV1DecisionRequest(room, { style: 'tag' }).options!.minimumGuarantee,
    undefined,
  );
  // Explicit caller level without the negotiated minor: field still absent.
  assert.equal(
    toV1DecisionRequest(room, {
      style: 'tag',
      minimumGuaranteeLevel: 'certified_bound',
    }).options!.minimumGuarantee,
    undefined,
  );
  // Minor 2 + level: field 8 populated with the CERTIFIED_BOUND ordinal (5).
  const encoded = toV1DecisionRequest(room, {
    style: 'tag',
    negotiatedMinor: 2,
    minimumGuaranteeLevel: 'certified_bound',
  });
  assert.equal(encoded.options!.minimumGuarantee, 5);
  // Minor 2 without a level: absent (accept any).
  assert.equal(
    toV1DecisionRequest(room, { style: 'tag', negotiatedMinor: 2 })
      .options!.minimumGuarantee,
    undefined,
  );
});

test('fromV1DecisionResponse at minor 2 reads field 11 exclusively and never field 10', () => {
  const fixture = fixtures.find(f => f.name === 'multiway-flop-check-option')!;
  const room = fixture.state.room;
  const strategy = create(ExpandedStrategySchema, {
    actions: [{ type: ActionType.CHECK, probability: 1 }],
    selectedAction: { type: ActionType.CHECK },
    solver: {
      source: SolverSource.BLUEPRINT,
      reasonCode: 'blueprint',
      // Hostile/mixed payload: field 10 present as the minor-1 token, but a
      // strict minor-2 reader must take only field 11.
      guarantee: 'uncertified',
      guaranteeLevel: 'approximate',
    },
  });
  const decisionResponse = create(DecisionResponseSchema, {
    result: { case: 'expandedStrategy', value: strategy },
  });
  const envelope = create(EnvelopeSchema, { protocolMinor: 2 });
  envelope.payload = { case: 'decisionResponse', value: decisionResponse };

  const minor2 = fromV1DecisionResponse(envelope, room, 2);
  assert.equal(minor2.guaranteeLevel, 'approximate');

  // Same envelope read as minor 1 yields no level surface at all.
  const minor1 = fromV1DecisionResponse(envelope, room, 1);
  assert.equal(minor1.guaranteeLevel, undefined);
});
