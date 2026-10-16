#!/usr/bin/env node

import { create, fromBinary, toBinary } from '@bufbuild/protobuf';
import {
  ActionType,
  DecisionResponseSchema,
  EngineErrorSchema,
  EnvelopeSchema,
  ExpandedStrategySchema,
  GetCapabilitiesResponseSchema,
  GuaranteeLevel,
  ObservationCoverage,
  ObservationSource,
  PotPhase,
  LayerStatus,
  SettlementRule,
  ReopeningRule,
  RunoutRule,
  SolverMode,
  SolverSource,
  StrategySchema,
  UtilityProfile,
  type Envelope,
} from '../../../../build/generated/ts/bigshark/engine/v2/engine_pb.js';

const CONTINUATION = 0x80;
const PAYLOAD_MASK = 0x7f;
const MAX_FRAME_BYTES = 1 << 20;

function encodeVarint(value: number): Buffer {
  const bytes: number[] = [];
  do {
    let byte = value & PAYLOAD_MASK;
    value >>>= 7;
    if (value !== 0) byte |= CONTINUATION;
    bytes.push(byte);
  } while (value !== 0);
  return Buffer.from(bytes);
}

let accumulated = Buffer.alloc(0);

function responseEnvelope(requestId: string, envelope: Envelope): Buffer {
  envelope.protocolVersion = 3;
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

  if (handId === 'error') {
    decision.result = {
      case: 'error',
      value: create(EngineErrorSchema, {
        code: 13, // OFF_TREE_AMOUNT
        message: 'off-tree amount',
        retryable: false,
      }),
    };
    const envelope = create(EnvelopeSchema);
    envelope.payload = { case: 'decisionResponse', value: decision };
    return envelope;
  }

  if (handId === 'expanded') {
    decision.result = {
      case: 'expandedStrategy',
      value: create(ExpandedStrategySchema, {
        actions: [
          { type: ActionType.CHECK, probability: 0.4 },
          { type: ActionType.BET, targetTotal: 100n, probability: 0.3 },
          { type: ActionType.BET, targetTotal: 200n, probability: 0.2 },
          { type: ActionType.BET, targetTotal: 400n, probability: 0.1 },
        ],
        selectedAction: { type: ActionType.BET, targetTotal: 200n },
        solver: {
          source: SolverSource.BLUEPRINT,
          reasonCode: 'blueprint',
          cacheHit: true,
          artifactSha256:
            '0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef',
          guaranteeLevel: 'approximate',
          utilityProfile: UtilityProfile.CASH_CHIP,
        },
      }),
    };
    const envelope = create(EnvelopeSchema);
    envelope.payload = { case: 'decisionResponse', value: decision };
    return envelope;
  }

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
        : { selectedAction: { type: ActionType.CHECK } }),
      solver: {
        source: SolverSource.POSTFLOP_HEURISTIC,
        reasonCode: 'postflop-heuristic',
        guaranteeLevel: 'approximate',
        utilityProfile: UtilityProfile.CASH_CHIP,
      },
    }),
  };
  const envelope = create(EnvelopeSchema);
  envelope.payload = { case: 'decisionResponse', value: decision };
  return envelope;
}

function capabilitiesEnvelope(): Envelope {
  const capabilities = create(GetCapabilitiesResponseSchema, {
    supportedProtocolVersions: [0, 1, 2, 3],
    engineBuildVersion: 'fake-v2.0',
    capabilities: [
      {
        minDealtPlayers: 2,
        maxDealtPlayers: 2,
        minLivePlayers: 2,
        maxLivePlayers: 2,
        supportedStreets: [1, 2, 3, 4],
        maxStackBb: 100,
        sidePots: false,
        ruleProfile: {
          settlement: SettlementRule.CLOCKWISE_LEFT_OF_BUTTON,
          reopening: ReopeningRule.NEVER_CUMULATIVE,
          runout: RunoutRule.SINGLE,
          dealtSeatCount: 2,
        },
        utilityProfile: UtilityProfile.CASH_CHIP,
        requiredCoverage: ObservationCoverage.FULL,
        solverMode: SolverMode.AUTOMATIC,
        maxGuarantee: GuaranteeLevel.APPROXIMATE,
        qualityScope: 'heads-up-r1',
      },
    ],
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
    process.stdout.write(Buffer.from([0x81, 0x00]));
    return;
  }
  const frame = responseEnvelope(requestId, decisionResponseEnvelope(request));
  if (mode === 'split') {
    const middle = Math.floor(frame.length / 2);
    process.stdout.write(frame.subarray(0, middle));
    setTimeout(() => process.stdout.write(frame.subarray(middle)), 15);
    return;
  }
  process.stdout.write(frame);
}

process.stdin.on('data', (chunk: Buffer) => {
  accumulated = Buffer.concat([accumulated, chunk]);
  pump();
});
