import assert from 'node:assert/strict';
import {
  create,
  type MessageInitShape,
} from '@bufbuild/protobuf';
import { dirname, join } from 'node:path';
import test from 'node:test';
import { fileURLToPath } from 'node:url';

import {
  ProtoEngineProcessClient,
  MAX_FRAME_BYTES,
  encodeVarint,
} from '../proto-engine-process-client.js';
import {
  DecisionRequestSchema,
  EnvelopeSchema,
  GetCapabilitiesRequestSchema,
  GetCapabilitiesResponseSchema,
  type Envelope,
} from '../../../build/generated/ts/bigshark/engine/v1/engine_pb.js';

const here = dirname(fileURLToPath(import.meta.url));
const fakeEngine = join(here, 'fixtures', 'fake-proto-engine.js');

function capabilitiesEnvelope(): Envelope {
  const envelope = create(EnvelopeSchema, { protocolMinor: 0 });
  envelope.payload = {
    case: 'getCapabilitiesRequest',
    value: create(GetCapabilitiesRequestSchema),
  };
  envelope.requestId = `cap-${Math.random()}`;
  return envelope;
}

function decisionEnvelope(handId: string, toCall = 0n): Envelope {
  const envelope = create(EnvelopeSchema, {
    protocolMinor: 0,
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
      },
      handId,
      decisionIndex: 1n,
      street: 1,
      buttonSeat: 1,
      heroPlayerId: 'hero',
      players: [
        { playerId: 'villain', seat: 1, stack: 2000n, status: 1 },
        { playerId: 'hero', seat: 0, stack: 2000n, status: 1 },
      ],
      heroHoleCards: [
        { rank: 13, suit: 1 },
        { rank: 12, suit: 2 },
      ],
      pot: { potTotal: 30n, mainPot: 30n },
      legalActions: [{ type: 2 }],
      toCall,
    },
    options: {
      strategyProfile: 'tag',
      solveTimeBudgetMs: 1000,
      seed: 1n,
      solverMode: 1,
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
    warmupEnvelope: capabilitiesEnvelope(),
    warmupTimeoutMs: 1000,
  });
}

test('warmup, echo, split frames, and consecutive requests', async t => {
  const engine = client();
  t.after(() => engine.stop());

  const split = await engine.request(decisionEnvelope('split'));
  assert.equal(split.payload.case, 'decisionResponse');
  assert.equal(
    split.payload.case === 'decisionResponse'
      ? split.payload.value.result.case === 'strategy'
        && split.payload.value.result.value.actions[0]?.type === 2
      : false,
    true,
  );

  const second = await engine.request(decisionEnvelope('echo'));
  assert.equal(second.payload.case, 'decisionResponse');
});

test('correlates responses by request id, not arrival order', async t => {
  const engine = client();
  t.after(() => engine.stop());

  const slow = engine.request(decisionEnvelope('reorder-1'), 1000);
  const fast = engine.request(decisionEnvelope('echo'), 1000);
  const fastResponse = await fast;
  const slowResponse = await slow;
  assert.match(fastResponse.requestId, /^echo-/);
  assert.match(slowResponse.requestId, /^reorder-1-/);
});

test('rejects an oversize declared length before allocation', async t => {
  const engine = client();
  t.after(() => engine.stop());

  await assert.rejects(
    engine.request(decisionEnvelope('oversize'), 1000),
    /1 MiB/,
  );
});

test('a truncated frame at process exit rejects outstanding requests', async t => {
  const engine = client();
  t.after(() => engine.stop());

  await assert.rejects(
    engine.request(decisionEnvelope('truncated'), 1000),
    /exited/,
  );
});

test('rejects a malformed varint and restarts', async t => {
  const engine = client();
  t.after(() => engine.stop());

  await assert.rejects(
    engine.request(decisionEnvelope('malformed-varint'), 1000),
    /malformed frame/,
  );
  const recovered = await engine.request(decisionEnvelope('echo'), 1000);
  assert.equal(recovered.payload.case, 'decisionResponse');
});

test('surfaces unknown payload kinds to the caller', async t => {
  const engine = client();
  t.after(() => engine.stop());

  const response = await engine.request(decisionEnvelope('unknown-payload'));
  assert.equal(response.payload.case, 'getCapabilitiesResponse');
  if (response.payload.case !== 'getCapabilitiesResponse') throw new Error('bad payload');
  assert.equal(response.payload.value.engineBuildVersion, 'fake');
});

test('preserves uint64 bigint values end to end', async t => {
  const engine = client();
  t.after(() => engine.stop());

  const aboveSafeInteger = 9_007_199_254_740_993n;  // 2^53 + 1
  const response = await engine.request(decisionEnvelope('bigint', aboveSafeInteger));
  if (response.payload.case !== 'decisionResponse'
    || response.payload.value.result.case !== 'strategy') throw new Error('bad response');
  const strategy = response.payload.value.result.value;
  const target = strategy.actions[0]?.targetTotal;
  assert.equal(typeof target, 'bigint');
  assert.equal(target, aboveSafeInteger);
  assert.equal(strategy.selectedAction?.targetTotal, aboveSafeInteger);
});

test('restarts after a request timeout', async t => {
  const engine = client();
  t.after(() => engine.stop());

  await assert.rejects(
    engine.request(decisionEnvelope('hang'), 15),
    /timed out/,
  );
  const next = await engine.request(decisionEnvelope('echo'), 1000);
  assert.equal(next.payload.case, 'decisionResponse');
});

test('rejects pending requests when the process exits', async t => {
  const engine = client();
  t.after(() => engine.stop());

  await assert.rejects(
    engine.request(decisionEnvelope('exit'), 1000),
    /exited/,
  );
});

test('refuses to encode an oversize local frame', () => {
  assert.throws(() => encodeVarint(MAX_FRAME_BYTES + 1), /Refusing/);
  assert.throws(() => encodeVarint(-1), /Refusing/);
});

// ---------------------------------------------------------------------------
// RFC 0002 Stage 8 minor-1 negotiation matrix against the fake host.
// ---------------------------------------------------------------------------

function minor1Client(): ProtoEngineProcessClient {
  return new ProtoEngineProcessClient({
    command: process.execPath,
    args: [fakeEngine],
    warmupEnvelope: capabilitiesEnvelope(),
    warmupTimeoutMs: 1000,
    negotiateMinor1: true,
  });
}

function minor1DecisionEnvelope(handId: string): Envelope {
  const envelope = decisionEnvelope(handId);
  envelope.protocolMinor = 1;
  return envelope;
}

test('opts into minor 1 only through the explicit handshake', async t => {
  const defaultClient = client();
  t.after(() => defaultClient.stop());
  await defaultClient.request(capabilitiesEnvelope(), 1000);
  assert.equal(defaultClient.negotiatedProtocolMinor, 0,
    'a non-opted-in client stays on minor 0');

  const engine = minor1Client();
  t.after(() => engine.stop());
  await engine.request(capabilitiesEnvelope(), 1000);
  assert.equal(engine.minor1Capable, true);
  assert.equal(engine.negotiatedProtocolMinor, 1);
});

test('new client/old host downgrades to minor 0 when 1 is unsupported', async t => {
  const oldHost = join(here, 'fixtures', 'fake-proto-engine-old.js');
  const engine = new ProtoEngineProcessClient({
    command: process.execPath,
    args: [oldHost],
    warmupEnvelope: capabilitiesEnvelope(),
    warmupTimeoutMs: 1000,
    negotiateMinor1: true,
  });
  t.after(() => engine.stop());
  await engine.request(capabilitiesEnvelope(), 1000);
  assert.equal(engine.minor1Capable, false, 'handshake downgrades to minor 0');
  assert.equal(engine.negotiatedProtocolMinor, 0);
});

test('minor 1 blueprint hit returns the full expanded distribution', async t => {
  const engine = minor1Client();
  t.after(() => engine.stop());
  const response = await engine.request(minor1DecisionEnvelope('blueprint-hit'), 1000);
  assert.equal(response.payload.case, 'decisionResponse');
  if (response.payload.case !== 'decisionResponse') throw new Error('bad payload');
  const result = response.payload.value.result;
  assert.equal(result.case, 'expandedStrategy');
  if (result.case !== 'expandedStrategy') throw new Error('bad result');
  assert.equal(result.value.actions.length, 4, 'more than five actions supported');
  assert.equal(result.value.solver?.source, 6 /* SOLVER_SOURCE_BLUEPRINT */);
  assert.equal(result.value.solver?.guarantee, 'uncertified');
  assert.ok(/^[a-f0-9]{64}$/.test(result.value.solver?.artifactSha256 ?? ''));
  assert.equal(result.value.selectedAction?.targetTotal, 200n);
});

test('forced blueprint coverage miss surfaces an engine error, not a fold', async t => {
  const engine = minor1Client();
  t.after(() => engine.stop());
  const response = await engine.request(minor1DecisionEnvelope('blueprint-miss'), 1000);
  if (response.payload.case !== 'decisionResponse') throw new Error('bad payload');
  const result = response.payload.value.result;
  assert.equal(result.case, 'error');
  if (result.case !== 'error') throw new Error('bad result');
  assert.equal(result.value.retryable, false);
});

test('a selected action outside the distribution is detectable by the client', async t => {
  const engine = minor1Client();
  t.after(() => engine.stop());
  const response = await engine.request(minor1DecisionEnvelope('blueprint-bad-member'), 1000);
  if (response.payload.case !== 'decisionResponse') throw new Error('bad payload');
  const result = response.payload.value.result;
  assert.equal(result.case, 'expandedStrategy');
  if (result.case !== 'expandedStrategy') throw new Error('bad result');
  const selected = result.value.selectedAction;
  const isMember = result.value.actions.some(action =>
    action.type === selected?.type
    && (action.targetTotal ?? 0n) === (selected?.targetTotal ?? 0n));
  assert.equal(isMember, false, 'the bad member is rejected by membership validation');
});

test('minor-1 automatic fallback is an expanded degenerate heuristic strategy', async t => {
  const engine = minor1Client();
  t.after(() => engine.stop());
  const response = await engine.request(minor1DecisionEnvelope('echo'), 1000);
  if (response.payload.case !== 'decisionResponse') throw new Error('bad payload');
  assert.equal(response.payload.value.result.case, 'expandedStrategy');
  if (response.payload.value.result.case !== 'expandedStrategy') throw new Error('bad result');
  const expanded = response.payload.value.result.value;
  assert.equal(expanded.solver?.source, 2 /* POSTFLOP_HEURISTIC */,
    'heuristic keeps its real source, never BLUEPRINT');
  assert.equal(expanded.solver?.artifactSha256, undefined);
  assert.equal(expanded.solver?.guarantee, undefined);
});

test('old client path still produces a minor-0 strategy with a minor-1 host', async t => {
  const engine = client();
  t.after(() => engine.stop());
  // Even though the host supports minor 1, a non-opted-in client sends minor 0
  // and receives the v1.0 strategy oneof.
  const response = await engine.request(decisionEnvelope('echo'), 1000);
  if (response.payload.case !== 'decisionResponse') throw new Error('bad payload');
  assert.equal(response.protocolMinor, 0);
  assert.equal(response.payload.value.result.case, 'strategy');
});

// ---------------------------------------------------------------------------
// RFC 0008 stage 5 minor-2 negotiation and floor probes against the fake host.
// ---------------------------------------------------------------------------

function minor2Client(opts: { minor1?: boolean } = {}): ProtoEngineProcessClient {
  return new ProtoEngineProcessClient({
    command: process.execPath,
    args: [fakeEngine],
    warmupEnvelope: capabilitiesEnvelope(),
    warmupTimeoutMs: 1000,
    negotiateMinor2: true,
    ...(opts.minor1 ? { negotiateMinor1: true } : {}),
  });
}

function minor2DecisionEnvelope(handId: string, floor?: number): Envelope {
  const envelope = create(EnvelopeSchema, {
    protocolMinor: 2,
    requestId: `${handId}-${Math.random()}`,
  });
  const request = create(DecisionRequestSchema, {
    state: {
      game: {
        variant: 1,
        bettingStructure: 1,
        gameType: 1,
        tableCapacity: 2,
        amountUnit: { name: 'chip', decimalPlaces: 0 },
        smallBlind: 10n,
        bigBlind: 20n,
      },
      handId,
      decisionIndex: 1n,
      street: 1,
      buttonSeat: 1,
      heroPlayerId: 'hero',
      players: [
        { playerId: 'villain', seat: 1, stack: 2000n, status: 1 },
        { playerId: 'hero', seat: 0, stack: 2000n, status: 1 },
      ],
      heroHoleCards: [
        { rank: 13, suit: 1 },
        { rank: 12, suit: 2 },
      ],
      pot: { potTotal: 30n, mainPot: 30n },
      legalActions: [{ type: 2 }],
    },
    options: {
      strategyProfile: 'tag',
      solveTimeBudgetMs: 1000,
      seed: 1n,
      solverMode: 1,
      ...(floor !== undefined ? { minimumGuarantee: floor } : {}),
    },
  });
  envelope.payload = { case: 'decisionRequest', value: request };
  return envelope;
}

test('minor 2 negotiates and stays above minor 1 when both flags are set', async t => {
  const both = minor2Client({ minor1: true });
  t.after(() => both.stop());
  await both.request(capabilitiesEnvelope(), 1000);
  assert.equal(both.negotiatedProtocolMinor, 2);
  assert.equal(both.minor2Capable, true);
  assert.equal(both.minor1Capable, true);

  const twoOnly = minor2Client();
  t.after(() => twoOnly.stop());
  await twoOnly.request(capabilitiesEnvelope(), 1000);
  assert.equal(twoOnly.negotiatedProtocolMinor, 2, 'minor-2-only opt-in reaches minor 2');
});

test('minor-2 capability probes against an old host fall through to 1 then 0', async t => {
  const oldHost = join(here, 'fixtures', 'fake-proto-engine-old.js');
  const twoThenOne = new ProtoEngineProcessClient({
    command: process.execPath,
    args: [oldHost],
    warmupEnvelope: capabilitiesEnvelope(),
    warmupTimeoutMs: 1000,
    negotiateMinor2: true,
    negotiateMinor1: true,
  });
  t.after(() => twoThenOne.stop());
  await twoThenOne.request(capabilitiesEnvelope(), 1000);
  assert.equal(twoThenOne.negotiatedProtocolMinor, 0, 'old host keeps both probes at minor 0');
});

test('field-11 decode and code 9 against the fake minor-2 host', async t => {
  const engine = minor2Client();
  t.after(() => engine.stop());
  const served = await engine.request(minor2DecisionEnvelope('echo'), 1000);
  if (served.payload.case !== 'decisionResponse') throw new Error('bad payload');
  const servedResult = servedResultUnchecked(served);
  assert.equal(servedResult.solver?.guaranteeLevel, 'approximate');
  assert.equal(servedResult.solver?.guarantee, undefined, 'field 10 is absent');
  assert.equal(served.protocolMinor, 2);

  // Floor exact_solved(4): the fake approximate answer is refused code 9.
  const refused = await engine.request(minor2DecisionEnvelope('echo', 4), 1000);
  if (refused.payload.case !== 'decisionResponse') throw new Error('bad payload');
  const r = refused.payload.value.result;
  assert.equal(r.case, 'error');
  if (r.case !== 'error') throw new Error('bad result');
  assert.equal(r.value.code, 9);
  assert.equal(r.value.retryable, false);

  // Floor approximate(2): served.
  const ok = await engine.request(minor2DecisionEnvelope('echo', 2), 1000);
  if (ok.payload.case !== 'decisionResponse') throw new Error('bad payload');
  assert.equal(ok.payload.value.result.case, 'expandedStrategy');
});

test('an out-of-band negotiation probe failure settles at 0 and respawns', async t => {
  // The warmup envelope succeeds; the very next frame (the minor-2 probe)
  // times out, killing the child. Negotiation must not reject: the client
  // settles at minor 0 and a subsequent request transparently respawns.
  const { spawn } = await import('node:child_process');
  const { EventEmitter } = await import('node:events');
  const { toBinary } = await import('@bufbuild/protobuf');
  let launches = 0;
  let warmupAnswered = false;
  const warmup = capabilitiesEnvelope();
  const deadChild = (): unknown => {
    const child = new EventEmitter();
    const stdin = new EventEmitter() as unknown as {
      writable: boolean;
      write: (chunk: unknown, cb?: () => void) => boolean;
    };
    stdin.writable = true;
    stdin.write = (_chunk: unknown, cb?: () => void) => {
      setImmediate(() => cb?.());
      return true;
    };
    const stdout = new EventEmitter();
    Object.assign(child, { stdin, stdout, pid: 1, kill: () => {} });
    // Answer exactly one capabilities envelope (the warmup), then go silent
    // so the probe timer rejects and the child is marked dead.
    queueMicrotask(() => {
      const caps = create(EnvelopeSchema, { protocolMinor: 0 });
      caps.requestId = warmup.requestId;
      caps.payload = {
        case: 'getCapabilitiesResponse',
        value: create(GetCapabilitiesResponseSchema),
      };
      const payload = Buffer.from(toBinary(EnvelopeSchema, caps));
      const len: number[] = [];
      let v = payload.length;
      do {
        let byte = v & 0x7f;
        v >>>= 7;
        if (v !== 0) byte |= 0x80;
        len.push(byte);
      } while (v !== 0);
      stdout.emit('data', Buffer.concat([Buffer.from(len), payload]));
      warmupAnswered = true;
    });
    return child;
  };
  const spawnOverride = ((command: string, args: readonly string[], options: Parameters<typeof spawn>[2]) => {
    launches += 1;
    return launches === 1
      ? deadChild() as ReturnType<typeof spawn>
      : spawn(command, args, options);
  }) as NonNullable<
      ConstructorParameters<typeof ProtoEngineProcessClient>[0]['spawnProcess']
    >;
  const client = new ProtoEngineProcessClient({
    command: process.execPath,
    args: [fakeEngine],
    warmupEnvelope: warmup,
    warmupTimeoutMs: 200,
    negotiateMinor2: true,
    spawnProcess: spawnOverride,
  });
  t.after(() => client.stop());
  await client.start();
  assert.equal(client.negotiatedProtocolMinor, 0,
    'a timed-out probe settles at minor 0 rather than rejecting start()');
  assert.equal(launches, 1, 'only the warmup child spawned before settle');
  assert.ok(warmupAnswered, 'warmup was answered before the silent probe');

  // The next request respawns through the real fake host and succeeds.
  const recovered = await client.request(decisionEnvelope('echo'), 5_000);
  assert.equal(launches, 2, 'respawn happened on the post-probe request');
  assert.equal(recovered.protocolMinor, 0);
  assert.equal(recovered.payload.case, 'decisionResponse');
});

test('minor-2 certified scenario decodes field 11 certified_bound', async t => {
  const engine = minor2Client();
  t.after(() => engine.stop());
  const response = await engine.request(minor2DecisionEnvelope('certified'), 1000);
  if (response.payload.case !== 'decisionResponse') throw new Error('bad payload');
  const result = response.payload.value.result;
  assert.equal(result.case, 'expandedStrategy');
  if (result.case !== 'expandedStrategy') throw new Error('bad result');
  assert.equal(result.value.solver?.source, 7 /* RESOLVING */);
  assert.equal(result.value.solver?.guaranteeLevel, 'certified_bound');
  assert.equal(result.value.solver?.guarantee, undefined);
});

function servedResultUnchecked(envelope: Envelope) {
  if (envelope.payload.case !== 'decisionResponse') throw new Error('bad payload');
  const result = envelope.payload.value.result;
  if (result.case !== 'expandedStrategy') throw new Error('bad result');
  return result.value;
}
