import { existsSync } from 'node:fs';
import { fileURLToPath } from 'node:url';

import { EngineProcessClient } from '../../../clients/node/engine-process-client.js';
import { ProtoEngineProcessClient } from '../../../clients/node/proto-engine-process-client.js';
import {
  decisionEnvelope,
  fromV1DecisionResponse,
  toV1DecisionRequest,
  V1EngineError,
} from './v1-mapper.js';
import {
  buildContext,
  safeFallback,
  validateEngineDecision,
} from './v0-normalizer.js';
import { create } from '@bufbuild/protobuf';
import {
  EnvelopeSchema,
  ErrorCode,
  GetCapabilitiesRequestSchema,
  SolverMode,
} from '../../../build/generated/ts/bigshark/engine/v1/engine_pb.js';
import type {
  EngineConfig,
  ExecutableDecision,
  RawEngineDecision,
  RiverRoom,
  V0DecisionContext,
  V1EnvelopeClient,
} from './types.js';
import type { Envelope } from '../../../build/generated/ts/bigshark/engine/v1/engine_pb.js';

const defaultBinary = process.env.BIGSHARK_ENGINE_BINARY || fileURLToPath(
  new URL('../../../../bin/bigshark-engine', import.meta.url),
);
let defaultClient: EngineProcessClient<V0DecisionContext, RawEngineDecision> | null = null;
let defaultProtoClient: ProtoEngineProcessClient | null = null;

function engineClient(): EngineProcessClient<V0DecisionContext, RawEngineDecision> | null {
  if (!defaultClient && existsSync(defaultBinary)) {
    defaultClient = new EngineProcessClient({
      command: defaultBinary,
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
    });
  }
  return defaultClient;
}

function protoEngineClient(): ProtoEngineProcessClient | null {
  if (!defaultProtoClient && existsSync(defaultBinary)) {
    const warmup: Envelope = create(EnvelopeSchema, { protocolMinor: 0 });
    warmup.payload = {
      case: 'getCapabilitiesRequest',
      value: create(GetCapabilitiesRequestSchema),
    };
    warmup.requestId = 'warmup-capabilities';
    defaultProtoClient = new ProtoEngineProcessClient({
      command: defaultBinary,
      args: ['--serve-proto'],
      warmupEnvelope: warmup,
      warmupTimeoutMs: 10_000,
      // The framed path stays minor 0 by default; callers must explicitly
      // negotiate a higher minor before its features can be used.
      negotiateMinor1: process.env.BIGSHARK_ENGINE_PROTO_MINOR1 === '1',
      negotiateMinor2: process.env.BIGSHARK_ENGINE_PROTO_MINOR2 === '1',
    });
  }
  return defaultProtoClient;
}

export { buildContext, safeFallback };

function protoEnabled(config: EngineConfig): boolean {
  return config.proto === true || process.env.BIGSHARK_ENGINE_PROTO === '1';
}

async function decideV1(
  room: RiverRoom,
  config: EngineConfig,
): Promise<ExecutableDecision> {
  const client = (config.protoEngineClient as V1EnvelopeClient | undefined)
    ?? protoEngineClient();
  if (!client)
    return safeFallback(room);
  // The concrete process client completes its capability handshake at the end
  // of start(); awaiting it makes the negotiated minor available on the very
  // first decision instead of racing it as 0. Structural fakes omit start().
  await client.start?.();
  // Minor 1+ is used only with a client whose capability handshake succeeded
  // (explicit opt-in at client construction). On minor 1 a forced blueprint
  // request runs in BLUEPRINT mode; otherwise AUTOMATIC tries the resident
  // blueprint and deterministically falls back to the heuristic on any miss.
  const minor: 0 | 1 | 2 = client.negotiatedProtocolMinor ?? 0;
  const solverMode = config.protoBlueprint && minor >= 1
    ? SolverMode.BLUEPRINT
    : SolverMode.AUTOMATIC;
  const request = toV1DecisionRequest(room, {
    ...(config.style !== undefined ? { style: config.style } : {}),
    ...(config.heroName !== undefined ? { heroName: config.heroName } : {}),
    ...(minor >= 1 ? { solverMode } : {}),
    // Field 8 is only valid on minor 2; pass the minor so the mapper gates it.
    ...(minor === 2
      ? {
          negotiatedMinor: 2 as const,
          ...(config.minimumGuaranteeLevel !== undefined
            ? { minimumGuaranteeLevel: config.minimumGuaranteeLevel }
            : {}),
        }
      : {}),
  });
  const envelope = await client.request(
    decisionEnvelope(request, minor),
    config.timeoutMs ?? 2000,
  ) as Envelope;
  return fromV1DecisionResponse(envelope, room, minor);
}

export async function decide(
  room: RiverRoom,
  config: EngineConfig = {},
): Promise<ExecutableDecision> {
  if (!room?.legal)
    return { action: 'fold', reason: 'no-legal', guaranteeLevel: 'operational_fallback' };

  if (protoEnabled(config)) {
    try {
      return await decideV1(room, config);
    } catch (error) {
      // RFC 0008 stage 5: a GUARANTEE_BELOW_REQUEST refusal is a contract
      // response to a caller-declared floor, not an operational outage. It
      // must propagate so the explicit caller can react; it never becomes a
      // strategic safe fallback.
      if (error instanceof V1EngineError
        && error.code === ErrorCode.GUARANTEE_BELOW_REQUEST)
        throw error;
      // Transport, validation, or capability failures are operational errors;
      // the v1 mapper never converts them into a strategic fold.
      return safeFallback(room);
    }
  }

  const client = config.engineClient || engineClient();
  if (!client)
    return safeFallback(room);

  try {
    const context = buildContext(room, config);
    const rawDecision = await client.request(context, config.timeoutMs ?? 2000);
    return validateEngineDecision(room, rawDecision) || safeFallback(room);
  } catch {
    return safeFallback(room);
  }
}

export function closeEngine(): void {
  defaultClient?.stop();
  defaultClient = null;
  defaultProtoClient?.stop();
  defaultProtoClient = null;
}
