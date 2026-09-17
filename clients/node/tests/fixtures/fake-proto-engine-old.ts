#!/usr/bin/env node

// Stage-7 (minor-0-only) host behavior for the new-client/old-host matrix
// case: capability queries always return the minor-0 advertisement regardless
// of the advertised request minor, and any minor-1 decision request is
// rejected with UNSUPPORTED_PROTOCOL.
import { create, fromBinary, toBinary } from '@bufbuild/protobuf';
import {
  ActionType,
  DecisionResponseSchema,
  EngineErrorSchema,
  EnvelopeSchema,
  GetCapabilitiesResponseSchema,
  SolverSource,
  StrategySchema,
  type Envelope,
} from '../../../../build/generated/ts/bigshark/engine/v1/engine_pb.js';

const CONTINUATION = 0x80;
const PAYLOAD_MASK = 0x7f;

function encodeVarint(value: number): Buffer {
  const bytes: number[] = [];
  do {
    const byte = value & PAYLOAD_MASK;
    value >>>= 7;
    bytes.push(value ? byte | CONTINUATION : byte);
  } while (value !== 0);
  return Buffer.from(bytes);
}

let accumulated = Buffer.alloc(0);

function write(envelope: Envelope): void {
  const payload = Buffer.from(toBinary(EnvelopeSchema, envelope));
  process.stdout.write(Buffer.concat([encodeVarint(payload.length), payload]));
}

function capabilitiesResponse(requestId: string): Envelope {
  const envelope = create(EnvelopeSchema, { protocolMinor: 0, requestId });
  envelope.payload = {
    case: 'getCapabilitiesResponse',
    value: create(GetCapabilitiesResponseSchema, {
      supportedProtocolMinors: [0],
      engineBuildVersion: 'old-stage7',
      solverModes: [1],
      maximumRequestBytes: BigInt(1 << 20),
    }),
  };
  return envelope;
}

function decisionResponse(requestId: string, rejected: boolean): Envelope {
  const envelope = create(EnvelopeSchema, { protocolMinor: rejected ? 0 : 0, requestId });
  const decision = create(DecisionResponseSchema);
  if (rejected) {
    decision.result = {
      case: 'error',
      value: create(EngineErrorSchema, {
        code: 2,
        message: 'only protocol minor 0 is supported',
        retryable: false,
      }),
    };
  } else {
    decision.result = {
      case: 'strategy',
      value: create(StrategySchema, {
        actions: [{ type: ActionType.CHECK, probability: 1 }],
        selectedAction: { type: ActionType.CHECK },
        solver: {
          source: SolverSource.POSTFLOP_HEURISTIC,
          reasonCode: 'postflop-heuristic',
        },
      }),
    };
  }
  envelope.payload = { case: 'decisionResponse', value: decision };
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
      header += 1;
      shift += 7;
      if ((byte & CONTINUATION) === 0) break;
    }
    if (accumulated.length - header < length) return;
    const payload = accumulated.subarray(header, header + length);
    accumulated = accumulated.subarray(header + length);
    const request = fromBinary(EnvelopeSchema, payload) as Envelope;

    if (request.payload.case === 'getCapabilitiesRequest') {
      write(capabilitiesResponse(request.requestId));
      continue;
    }
    write(decisionResponse(request.requestId, request.protocolMinor !== 0));
  }
}

process.stdin.on('data', (chunk: Buffer) => {
  accumulated = Buffer.concat([accumulated, chunk]);
  pump();
});
