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
import { resolveSolveTimeBudgetMs } from './v1-mapper.js';
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
  ResidentRootSpec,
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
let defaultProtoClientKey = '';

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

const SHA256_HEX = /^[a-f0-9]{64}$/;

/**
 * RFC 0009 W1: parse the resident-root configuration surface. The explicit
 * `config.residentRoots` wins over the `BIGSHARK_ENGINE_RESIDENT_ROOTS`
 * environment variable (a JSON array of `{path, sha256}` objects). Malformed
 * entries are configuration errors: the engine child would refuse the launch
 * line anyway, so silently dropping one would hide a typo behind an
 * uncovered-decision fallback. An unparseable environment value yields an
 * empty list (and therefore the declared fallback path, never a crash).
 */
export function parseResidentRoots(
  config: EngineConfig,
): ResidentRootSpec[] {
  if (config.residentRoots !== undefined)
    return config.residentRoots.map(validateResidentRoot);
  const raw = process.env.BIGSHARK_ENGINE_RESIDENT_ROOTS;
  if (!raw)
    return [];
  let value: unknown;
  try {
    value = JSON.parse(raw);
  } catch {
    return [];
  }
  if (!Array.isArray(value))
    return [];
  return value.map(entry => {
    if (typeof entry !== 'object' || entry === null)
      throw new Error('resident root entries must be {path, sha256} objects');
    const record = entry as { path?: unknown; sha256?: unknown };
    return validateResidentRoot({
      path: String(record.path ?? ''),
      sha256: String(record.sha256 ?? ''),
    });
  });
}

function validateResidentRoot(spec: ResidentRootSpec): ResidentRootSpec {
  if (!spec.path)
    throw new Error('resident root requires a nonempty path');
  if (!SHA256_HEX.test(spec.sha256)) {
    throw new Error(
      `resident root ${spec.path} requires a 64-lowercase-hex sha256 pin`,
    );
  }
  return { path: spec.path, sha256: spec.sha256 };
}

function residentRootArgs(roots: readonly ResidentRootSpec[]): string[] {
  return roots.flatMap(root => ['--resident-root', `${root.path}=${root.sha256}`]);
}

/**
 * RFC 0009 W1: the framed child's launch line. With no configured roots this
 * is exactly the historical `['--serve-proto']`; each root appends the host's
 * documented repeatable `--resident-root <path>=<sha256>` argument, in the
 * configuration's order. Exported so the launch line is asserted directly by
 * the wiring tests.
 */
export function protoEngineLaunchArgs(roots: readonly ResidentRootSpec[]): string[] {
  return ['--serve-proto', ...residentRootArgs(roots)];
}

function protoEngineClient(roots: readonly ResidentRootSpec[]): ProtoEngineProcessClient | null {
  // The implicit client is keyed by its launch line: configuration is fixed
  // for a runner's lifetime, and a different root set must not silently reuse
  // the previous child. Default (no roots) keeps the historical bare
  // `--serve-proto` launch line.
  const key = JSON.stringify(roots);
  if (defaultProtoClient && defaultProtoClientKey === key)
    return defaultProtoClient;
  if (!existsSync(defaultBinary))
    return null;
  defaultProtoClient?.stop();
  defaultProtoClient = null;
  const warmup: Envelope = create(EnvelopeSchema, { protocolMinor: 0 });
  warmup.payload = {
    case: 'getCapabilitiesRequest',
    value: create(GetCapabilitiesRequestSchema),
  };
  warmup.requestId = 'warmup-capabilities';
  defaultProtoClient = new ProtoEngineProcessClient({
    command: defaultBinary,
    args: protoEngineLaunchArgs(roots),
    warmupEnvelope: warmup,
    warmupTimeoutMs: 10_000,
    // The framed path stays minor 0 by default; callers must explicitly
    // negotiate a higher minor before its features can be used. RFC 0009 W1:
    // a configured resident root set is that explicit opt-in to the minor-2
    // guarantee-and-digest surface, so the runner's served decisions carry
    // their provenance; without roots nothing changes.
    negotiateMinor1: roots.length > 0
      || process.env.BIGSHARK_ENGINE_PROTO_MINOR1 === '1',
    negotiateMinor2: roots.length > 0
      || process.env.BIGSHARK_ENGINE_PROTO_MINOR2 === '1',
  });
  defaultProtoClientKey = key;
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
  // A malformed programmatic root spec is a configuration error, not an
  // operational outage: surface it instead of silently serving every turn
  // from the fallback with the fault hidden.
  let roots: ResidentRootSpec[];
  try {
    roots = parseResidentRoots(config);
  } catch (error) {
    throw new Error(
      `invalid resident root configuration: ${error instanceof Error ? error.message : String(error)}`,
    );
  }
  const client = (config.protoEngineClient as V1EnvelopeClient | undefined)
    ?? protoEngineClient(roots);
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
    // RFC 0009 W1: the runner's real remaining-time budget reaches the
    // resolver instead of the historical pinned 2000 ms.
    ...(config.solveTimeBudgetMs !== undefined
      ? { solveTimeBudgetMs: resolveSolveTimeBudgetMs(config.solveTimeBudgetMs) }
      : {}),
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
      // RFC 0009 W1: a malformed resident-root configuration is likewise a
      // caller error, not an outage; it must be visible rather than becoming
      // a per-turn silent fallback.
      if (error instanceof Error
        && error.message.startsWith('invalid resident root configuration'))
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
  // Resets the implicit-client key too: a later decide() re-evaluates the
  // launch line instead of reusing the stopped client's key. Any future reset
  // path must clear both fields together.
  defaultClient?.stop();
  defaultClient = null;
  defaultProtoClient?.stop();
  defaultProtoClient = null;
  defaultProtoClientKey = '';
}
