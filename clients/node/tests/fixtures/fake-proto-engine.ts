#!/usr/bin/env node

import { create, fromBinary, toBinary } from '@bufbuild/protobuf';
import {
  ActionType,
  DecisionResponseSchema,
  EnvelopeSchema,
  GetCapabilitiesResponseSchema,
  SolverSource,
  StrategySchema,
  type Envelope,
} from '../../../../build/generated/ts/bigshark/engine/v1/engine_pb.js';

const CONTINUATION = 0x80;
const PAYLOAD_MASK = 0x7f;
const MAX_FRAME_BYTES = 1 << 20;

function encodeVarint(value: number): Buffer {
  const bytes: number[] = [];
  do {
    let byte = value & PAYLOAD_MASK;
    value >>>= 7;  // fake never emits lengths beyond uint32
    if (value !== 0) byte |= CONTINUATION;
    bytes.push(byte);
  } while (value !== 0);
  return Buffer.from(bytes);
}

let accumulated = Buffer.alloc(0);

function responseEnvelope(requestId: string, envelope: Envelope): Buffer {
  envelope.protocolMinor = 0;
  envelope.requestId = requestId;
  const payload = Buffer.from(toBinary(EnvelopeSchema, envelope));
  return Buffer.concat([encodeVarint(payload.length), payload]);
}

function decisionResponseEnvelope(request: Envelope): Envelope {
  const state = request.payload.case === 'decisionRequest'
    ? request.payload.value.state
    : undefined;
  const handId = state?.handId ?? '';
  const toCall = state?.toCall ?? 0n;
  const bigintEcho = handId === 'bigint';
  const decision = create(DecisionResponseSchema);
  decision.result = {
    case: 'strategy',
    value: create(StrategySchema, {
      actions: [{
        type: bigintEcho ? ActionType.FOLD : ActionType.CHECK,
        ...(bigintEcho ? { targetTotal: toCall } : {}),
        probability: 1,
      }],
      ...(bigintEcho
        ? { selectedAction: { type: ActionType.FOLD, targetTotal: toCall } }
        : {}),
      solver: {
        source: SolverSource.POSTFLOP_HEURISTIC,
        reasonCode: 'postflop-heuristic',
      },
    }),
  };
  const envelope = create(EnvelopeSchema);
  envelope.payload = { case: 'decisionResponse', value: decision };
  return envelope;
}

function capabilitiesEnvelope(): Envelope {
  const capabilities = create(GetCapabilitiesResponseSchema, {
    supportedProtocolMinors: [0],
    engineBuildVersion: 'fake',
    supportedGameVariants: [1],
    supportedBettingStructures: [1],
    minimumPlayers: 2,
    maximumPlayers: 10,
    supportedStreets: [1, 2, 3, 4],
    supportedActions: [1, 2, 3, 4, 5],
    amountSemantics: 1,
    solverModes: [1],
    strategyProfiles: ['tag'],
    exactLp: 3,
    dcfr: 3,
    multistreet: 2,
    sidePots: 1,
    rake: 1,
    tournamentIcm: 1,
    maximumRequestBytes: BigInt(MAX_FRAME_BYTES),
    maximumSolveTimeMs: 120000,
  });
  const envelope = create(EnvelopeSchema);
  envelope.payload = { case: 'getCapabilitiesResponse', value: capabilities };
  return envelope;
}

function pump(): void {
  for (;;) {
    let length = 0;
    let shift = 0;
    let header = 0;
    while (header < 5) {
      if (header >= accumulated.length) return;
      const byte = accumulated[header] ?? 0;
      length |= (byte & PAYLOAD_MASK) << shift;
      header++;
      shift += 7;
      if ((byte & CONTINUATION) === 0) break;
    }
    if (accumulated.length - header < length) return;
    const payload = accumulated.subarray(header, header + length);
    accumulated = accumulated.subarray(header + length);

    const request = fromBinary(EnvelopeSchema, payload) as Envelope;
    handle(request);
  }
}

function handle(request: Envelope): void {
  const requestId = request.requestId;

  if (request.payload.case === 'getCapabilitiesRequest') {
    process.stdout.write(responseEnvelope(requestId, capabilitiesEnvelope()));
    return;
  }
  if (request.payload.case !== 'decisionRequest'
    || request.payload.value.state === undefined) return;
  const mode = request.payload.value.state.handId || 'echo';

  if (mode === 'exit') process.exit(7);
  if (mode === 'hang') return;
  if (mode === 'oversize') {
    process.stdout.write(
      Buffer.concat([encodeVarint(MAX_FRAME_BYTES + 1), Buffer.from([1])]),
    );
    return;
  }
  if (mode === 'truncated') {
    const frame = responseEnvelope(requestId, decisionResponseEnvelope(request));
    process.stdout.write(frame.subarray(0, Math.min(frame.length, 6)));
    setTimeout(() => process.exit(0), 30);
    return;
  }
  if (mode === 'malformed-varint') {
    // Value 1 with a trailing zero continuation group is non-canonical.
    process.stdout.write(Buffer.from([0x81, 0x00]));
    return;
  }
  if (mode === 'unknown-payload') {
    process.stdout.write(responseEnvelope(requestId, capabilitiesEnvelope()));
    return;
  }
  const frame = responseEnvelope(requestId, decisionResponseEnvelope(request));
  if (mode === 'split') {
    const middle = Math.floor(frame.length / 2);
    process.stdout.write(frame.subarray(0, middle));
    setTimeout(() => process.stdout.write(frame.subarray(middle)), 15);
    return;
  }
  if (mode === 'reorder-1') {
    setTimeout(() => process.stdout.write(frame), 60);
    return;
  }
  process.stdout.write(frame);
}

process.stdin.on('data', (chunk: Buffer) => {
  accumulated = Buffer.concat([accumulated, chunk]);
  pump();
});
