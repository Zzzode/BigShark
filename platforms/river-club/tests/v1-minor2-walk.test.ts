// RFC 0008 stage 5 real-binary minor-2 walk. Every assertion is gated on the
// client actually negotiating minor 2: a stale published binary (which only
// knows minors 0/1) makes the handshake settle below and the test skips, so
// an unpublished stage-5 binary can never fail npm check. The non-gated
// negotiation/decode/code-9 coverage lives in v1-minor2-guard.test.ts and
// proto-engine-process-client.test.ts against the scripted fake host.
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
  DecisionRequestSchema,
  EnvelopeSchema,
  ForcedContributionType,
  GetCapabilitiesRequestSchema,
  GuaranteeLevel,
  PlayerStatus,
  Rank,
  SolverMode,
  SolverSource,
  Street,
  Suit,
  type DecisionRequest,
  type Envelope,
} from '../../../build/generated/ts/bigshark/engine/v1/engine_pb.js';

const here = dirname(fileURLToPath(import.meta.url));
const root = resolve(here, '../../../../');
const engineBinary = process.env.BIGSHARK_ENGINE_BINARY
  || join(root, 'bin', 'bigshark-engine');
const fixturePublisher = process.env.BS_RESIDENT_FIXTURE_BIN
  || join(root, 'bin', 'bs-resident-fixture');

interface PublishedFixture { path: string; sha256: string }

function publishFixture(): PublishedFixture | null {
  if (!existsSync(engineBinary) || !existsSync(fixturePublisher))
    return null;
  const dir = mkdtempSync(join(tmpdir(), 'bs-stage5-walk-'));
  const out = spawnSync(fixturePublisher, [dir], { encoding: 'utf8' });
  if (out.status !== 0)
    throw new Error(`fixture publisher failed: ${out.stderr}`);
  return JSON.parse(out.stdout.trim()) as PublishedFixture;
}

const card = (rank: Rank, suit: Suit) => ({ rank, suit });
const fixture = publishFixture();

function capabilitiesEnvelope(): Envelope {
  const envelope = create(EnvelopeSchema);
  envelope.protocolMinor = 0;
  envelope.requestId = `warm5-${Math.random()}`;
  envelope.payload = {
    case: 'getCapabilitiesRequest',
    value: create(GetCapabilitiesRequestSchema),
  };
  return envelope;
}

function residentClient(published: PublishedFixture): ProtoEngineProcessClient {
  return new ProtoEngineProcessClient({
    command: engineBinary,
    args: ['--serve-proto', '--resident-root', `${published.path}=${published.sha256}`],
    warmupEnvelope: capabilitiesEnvelope(),
    warmupTimeoutMs: 10_000,
    negotiateMinor1: true,
    negotiateMinor2: true,
  });
}

// Same published-fixture root as the minor-1 blueprint walk: flop 2c 3d 7h,
// heads-up, 5 big blind, pot 20, check/bet(5..40).
function blueprintRequest(floor?: GuaranteeLevel): DecisionRequest {
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
      handId: 'stage5-root',
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
      legalActions: [
        { type: ActionType.CHECK },
        { type: ActionType.BET, minTargetTotal: 5n, maxTargetTotal: 40n },
      ],
      toCall: 0n,
    },
    options: {
      strategyProfile: 'tag',
      solveTimeBudgetMs: 1000,
      seed: 42n,
      includeSampledAction: true,
      solverMode: SolverMode.BLUEPRINT,
      ...(floor !== undefined ? { minimumGuarantee: floor } : {}),
    },
  });
}

function frame(request: DecisionRequest): Envelope {
  const envelope = create(EnvelopeSchema, { protocolMinor: 2 });
  envelope.requestId = `stage5-${Math.random()}`;
  envelope.payload = { case: 'decisionRequest', value: request };
  return envelope;
}

test('real binary: minor-2 walk',
  { skip: fixture === null ? 'engine or fixture binary unavailable' : false },
  async t => {
    if (!fixture) return;
    const client = residentClient(fixture);
    t.after(() => client.stop());
    await client.request(capabilitiesEnvelope(), 10_000);
    if (client.negotiatedProtocolMinor !== 2) {
      t.skip(`published binary negotiated minor ${client.negotiatedProtocolMinor}, not 2`);
      return;
    }

    // Capabilities advertise [0,1,2] at v1.2.0.
    const capsEnvelope = create(EnvelopeSchema, { protocolMinor: 2 });
    capsEnvelope.requestId = `caps5-${Math.random()}`;
    capsEnvelope.payload = {
      case: 'getCapabilitiesRequest',
      value: create(GetCapabilitiesRequestSchema),
    };
    const caps = await client.request(capsEnvelope, 10_000);
    if (caps.payload.case !== 'getCapabilitiesResponse') throw new Error('bad caps');
    assert.deepEqual([...caps.payload.value.supportedProtocolMinors], [0, 1, 2]);
    assert.equal(caps.payload.value.engineBuildVersion, 'bigshark-engine-v1.2.0');

    // Published blueprint row: field 11 approximate, field 10 absent.
    const row = await client.request(frame(blueprintRequest()), 10_000);
    if (row.payload.case !== 'decisionResponse') throw new Error('bad payload');
    const result = row.payload.value.result;
    assert.equal(result.case, 'expandedStrategy');
    if (result.case !== 'expandedStrategy') return;
    assert.equal(result.value.solver?.source, SolverSource.BLUEPRINT);
    assert.equal(result.value.solver?.artifactSha256, fixture.sha256);
    assert.equal(result.value.solver?.guaranteeLevel, 'approximate');
    assert.equal(result.value.solver?.guarantee, undefined, 'field 10 never set at minor 2');
    assert.equal(result.value.solver?.cacheHit, true);

    // Floor approximate: still served.
    const served = await client.request(
      frame(blueprintRequest(GuaranteeLevel.APPROXIMATE)), 10_000);
    if (served.payload.case !== 'decisionResponse') throw new Error('bad payload');
    assert.equal(served.payload.value.result.case, 'expandedStrategy');

    // Floor certified_bound: the cached blueprint is only approximate, so
    // this is a non-retryable code 9 with no strategy payload.
    const refused = await client.request(
      frame(blueprintRequest(GuaranteeLevel.CERTIFIED_BOUND)), 10_000);
    if (refused.payload.case !== 'decisionResponse') throw new Error('bad payload');
    const r = refused.payload.value.result;
    assert.equal(r.case, 'error');
    if (r.case !== 'error') return;
    assert.equal(r.value.code, 9);
    assert.equal(r.value.retryable, false);

    // HEURISTIC on the same node answers field 11 approximate from the policy.
    const heuristicRequest = blueprintRequest();
    heuristicRequest.options!.solverMode = SolverMode.HEURISTIC;
    const heuristic = await client.request(frame(heuristicRequest), 10_000);
    if (heuristic.payload.case !== 'decisionResponse') throw new Error('bad payload');
    const h = heuristic.payload.value.result;
    assert.equal(h.case, 'expandedStrategy');
    if (h.case !== 'expandedStrategy') return;
    assert.equal(h.value.solver?.guaranteeLevel, 'approximate');
    assert.equal(h.value.solver?.guarantee, undefined);
  });
