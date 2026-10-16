import { existsSync, readdirSync } from 'node:fs';
import { totalmem } from 'node:os';
import { join } from 'node:path';
import { performance } from 'node:perf_hooks';
import { fileURLToPath } from 'node:url';

import { create } from '@bufbuild/protobuf';

import { ProtoEngineProcessClient } from '../../../clients/node/proto-engine-process-client.js';
import {
  EnvelopeSchema,
  ErrorCode,
  GetCapabilitiesRequestSchema,
  type Envelope,
} from '../../../build/generated/ts/bigshark/engine/v2/engine_pb.js';
import {
  decisionEnvelope,
  EngineError,
  fromDecisionResponse,
  resolveSolveTimeBudgetMs,
  safeFallback,
  toDecisionRequest,
} from './mapper.js';
import type {
  EngineClient,
  EngineConfig,
  ExecutableDecision,
  ResidentRootSpec,
  RiverRoom,
} from './types.js';

const defaultBinary = process.env.BIGSHARK_ENGINE_BINARY || fileURLToPath(
  new URL('../../../../bin/bigshark-engine', import.meta.url),
);
// Repo root relative to the COMPILED module (dist/platforms/river-club/src/),
// the same depth defaultBinary uses. Auto-detection looks for bundled flop
// class libraries under <repo>/artifacts/flop-library*/manifest.json.
const repoRoot = fileURLToPath(new URL('../../../../', import.meta.url));
// The full library takes ~55 s to probe and longer to load; the default
// 10 s warmup would expire mid-handshake. Use the same per-phase allowance
// for standalone roots: 300 s for one artifact source, 600 s for two or more.
export const WARMUP_TIMEOUT_WITH_LIBRARY_MS = 300_000;
const WARMUP_TIMEOUT_MULTI_LIBRARY_MS = 600_000;
const WARMUP_TIMEOUT_DEFAULT_MS = 10_000;

/**
 * The resident budget for auto-detected flop libraries, in MiB.
 *
 * Defaults to 1/4 of total system RAM (12 GiB on a 48 GB machine,
 * 4 GiB on 16 GB). The engine loads classes that fit and reports the
 * rest OverBudget, so a smaller budget means partial coverage, not a
 * failure. Override with BIGSHARK_ENGINE_RESIDENT_BUDGET_MIB (a
 * positive integer in MiB).
 */
export function residentBudgetMiB(): number {
  const raw = process.env.BIGSHARK_ENGINE_RESIDENT_BUDGET_MIB;
  if (raw !== undefined) {
    const parsed = Number(raw);
    if (Number.isFinite(parsed) && parsed > 0) return Math.floor(parsed);
  }
  return Math.floor(totalmem() / 4 / (1024 * 1024));
}
let defaultClient: ProtoEngineProcessClient | null = null;
let defaultClientKey = '';

const SHA256_HEX = /^[a-f0-9]{64}$/;

/**
 * Parse the resident-root configuration surface. The explicit
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

/**
 * All bundled flop class library directories with a manifest.json,
 * sorted by name for deterministic launch order. Returns an empty array
 * when none are present. Injectable so the wiring tests are
 * deterministic on machines without the 3 GB checkout.
 */
export function autoDetectedFlopLibraries(root: string = repoRoot): string[] {
  const artifactsDir = join(root, 'artifacts');
  if (!existsSync(artifactsDir)) return [];
  const found: string[] = [];
  for (const entry of readdirSync(artifactsDir, { withFileTypes: true })) {
    if (!entry.isDirectory() || !entry.name.startsWith('flop-library')) continue;
    const manifest = join(artifactsDir, entry.name, 'manifest.json');
    if (existsSync(manifest))
      found.push(join(artifactsDir, entry.name));
  }
  return found.sort();
}

/**
 * The first auto-detected flop library, or null when none are present.
 * Kept for backward compatibility with callers that expect a single path.
 */
export function autoDetectedFlopLibrary(root: string = repoRoot): string | null {
  const all = autoDetectedFlopLibraries(root);
  return all.length > 0 ? all[0]! : null;
}

export interface ResolvedFlopLibraries {
  libraries: string[];
  // Defined only when the set is the launcher's auto-detected default;
  // the engine child is then launched with --resident-budget.
  residentBudgetMiB?: number;
}

/**
 * Flop class library directories the engine child is launched with.
 *
 * Precedence:
 * 1. `config.flopLibraries` (explicit). An empty array is the config
 *    opt-out: it wins and carries no budget.
 * 2. `BIGSHARK_ENGINE_FLOP_LIBRARIES` env var (JSON array of directory
 *    paths). A set-but-garbage value suppresses auto-detection (declared
 *    fallback, never a crash). An empty array is the env opt-out.
 * 3. Auto-detection: every directory under `<repo>/artifacts/` whose
 *    name starts with `flop-library` and contains a `manifest.json`
 *    (e.g. `flop-library/` for heads-up, `flop-library-3p/` for
 *    3-way). All detected libraries are loaded with an auto-detected
 *    resident budget.
 *
 * Each entry becomes a repeatable `--flop-library <dir>` child argument;
 * the engine expands it into one resident root per class via
 * `<dir>/manifest.json`. The engine's root identity matching selects
 * the right artifact per decision (2-seat queries match 2-seat
 * artifacts, 3-seat queries match 3-seat artifacts).
 */
export function resolveFlopLibraries(
  config: EngineConfig,
  autoDetect: () => string[] = () => autoDetectedFlopLibraries(),
): ResolvedFlopLibraries {
  if (config.flopLibraries !== undefined) {
    return {
      libraries: config.flopLibraries.map(lib => {
        if (typeof lib !== 'string' || lib.length === 0)
          throw new Error('flop library entries must be non-empty strings');
        return lib;
      }),
    };
  }
  const raw = process.env.BIGSHARK_ENGINE_FLOP_LIBRARIES;
  if (raw !== undefined) {
    let value: unknown;
    try {
      value = JSON.parse(raw);
    } catch {
      return { libraries: [] };
    }
    if (!Array.isArray(value))
      return { libraries: [] };
    return {
      libraries: value.map(entry => {
        if (typeof entry !== 'string' || entry.length === 0)
          throw new Error('flop library entries must be non-empty strings');
        return entry;
      }),
    };
  }
  const detected = autoDetect();
  return detected.length > 0
    ? { libraries: detected, residentBudgetMiB: residentBudgetMiB() }
    : { libraries: [] };
}

/**
 * Flop class library directories (backward-compatible delegate to
 * resolveFlopLibraries). New code should use resolveFlopLibraries to
 * also receive the auto-detected resident budget.
 */
export function parseFlopLibraries(config: EngineConfig): string[] {
  return resolveFlopLibraries(config).libraries;
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
 * The engine child's launch line. With no configured roots or
 * libraries this is exactly the bare `['--serve-proto']`; each root
 * appends the host's documented repeatable `--resident-root <path>=<sha256>`
 * argument, each flop library appends `--flop-library <dir>`, and a
 * residentBudgetMiB appends `--resident-budget <MiB>`, in the
 * configuration's order. Exported so the launch line is asserted directly by
 * the wiring tests.
 */
export function protoEngineLaunchArgs(
  roots: readonly ResidentRootSpec[],
  libraries: readonly string[] = [],
  residentBudgetMiB?: number,
): string[] {
  return [
    '--serve-proto',
    ...residentRootArgs(roots),
    ...libraries.flatMap(lib => ['--flop-library', lib]),
    ...(residentBudgetMiB !== undefined
      ? ['--resident-budget', String(residentBudgetMiB)]
      : []),
  ];
}

/**
 * The warmup timeout for the engine child. A flop library loads ~1,755
 * SQLite DBs before the engine's first frame (~55 s probe, longer to
 * load), so the default 10 s would expire mid-handshake. A standalone root
 * also validates and indexes before the first frame. One configured source
 * gets 300 s; two or more get 600 s. These are per-phase startup/negotiation
 * allowances, never active decision budgets. Library-only callers retain
 * their previous results. Exported for the wiring tests.
 */
export function warmupTimeoutMsFor(
  libraries: readonly string[],
  roots: readonly ResidentRootSpec[] = [],
): number {
  const sourceCount = libraries.length + roots.length;
  if (sourceCount === 0) return WARMUP_TIMEOUT_DEFAULT_MS;
  return sourceCount === 1
    ? WARMUP_TIMEOUT_WITH_LIBRARY_MS
    : WARMUP_TIMEOUT_MULTI_LIBRARY_MS;
}

function engineClient(
  roots: readonly ResidentRootSpec[],
  libraries: readonly string[],
  residentBudgetMiB?: number,
): ProtoEngineProcessClient | null {
  // The implicit client is keyed by its launch line: configuration is fixed
  // for a runner's lifetime, and a different root, library, or budget set
  // must not silently reuse the previous child. Default (no roots, no
  // libraries, no budget) keeps the historical bare `--serve-proto` launch
  // line.
  const key = JSON.stringify({ roots, libraries, residentBudgetMiB });
  if (defaultClient && defaultClientKey === key)
    return defaultClient;
  if (!existsSync(defaultBinary))
    return null;
  defaultClient?.stop();
  defaultClient = null;
  const warmup = create(EnvelopeSchema, { protocolVersion: 3 });
  warmup.payload = {
    case: 'getCapabilitiesRequest',
    value: create(GetCapabilitiesRequestSchema),
  };
  warmup.requestId = 'warmup-capabilities';
  defaultClient = new ProtoEngineProcessClient({
    command: defaultBinary,
    args: protoEngineLaunchArgs(roots, libraries, residentBudgetMiB),
    warmupEnvelope: warmup,
    warmupTimeoutMs: warmupTimeoutMsFor(libraries, roots),
  });
  defaultClientKey = key;
  return defaultClient;
}

export { safeFallback };

// The adapter also accepts structural clients. Bound each asynchronous phase
// itself rather than assuming every injected request enforces its timeout.
async function withinDecisionDeadline<T>(
  deadline: number,
  operation: (remainingMs: number) => Promise<T>,
): Promise<T> {
  const remainingMs = deadline - performance.now();
  if (remainingMs <= 0) throw new Error('Engine decision timed out');
  let timer: NodeJS.Timeout | undefined;
  try {
    const expiry = new Promise<never>((_resolve, reject) => {
      timer = setTimeout(() => reject(new Error('Engine decision timed out')), remainingMs);
    });
    const result = await Promise.race([operation(remainingMs), expiry]);
    if (performance.now() >= deadline) throw new Error('Engine decision timed out');
    return result;
  } finally {
    clearTimeout(timer);
  }
}

export async function decide(
  room: RiverRoom,
  config: EngineConfig = {},
): Promise<ExecutableDecision> {
  if (!room?.legal)
    return { action: 'fold', reason: 'no-legal', guaranteeLevel: 'operational_fallback' };

  const timeoutMs = config.timeoutMs ?? 2000;
  const deadline = performance.now() + timeoutMs;
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
  let libraries: string[];
  let residentBudgetMiB: number | undefined;
  try {
    const resolved = resolveFlopLibraries(config);
    libraries = resolved.libraries;
    residentBudgetMiB = resolved.residentBudgetMiB;
  } catch (error) {
    throw new Error(
      `invalid flop library configuration: ${error instanceof Error ? error.message : String(error)}`,
    );
  }
  if (!Number.isFinite(timeoutMs) || timeoutMs <= 0)
    return safeFallback(room);
  const client = config.engineClient
    ?? engineClient(roots, libraries, residentBudgetMiB);
  if (!client)
    return safeFallback(room);
  try {
    // The concrete process client completes its capability handshake at the
    // end of start(); awaiting it makes capabilities available on the very
    // first decision. Structural fakes omit start().
    if (client.start) await withinDecisionDeadline(deadline, () => client.start!());
    // The engine's internal solve deadline must not outlive the adapter's
    // transport deadline. Cap the caller's budget by the time left after
    // startup so native work stops before the request window expires.
    const remainingAfterStart = Math.max(1, Math.floor(deadline - performance.now()));
    const solveTimeBudgetMs = config.solveTimeBudgetMs !== undefined
      ? Math.min(resolveSolveTimeBudgetMs(config.solveTimeBudgetMs), remainingAfterStart)
      : remainingAfterStart;
    const request = toDecisionRequest(room, {
      ...(config.style !== undefined ? { style: config.style } : {}),
      ...(config.heroName !== undefined ? { heroName: config.heroName } : {}),
      solveTimeBudgetMs,
      ...(config.minimumGuaranteeLevel !== undefined
        ? { minimumGuaranteeLevel: config.minimumGuaranteeLevel }
        : {}),
    });
    const requestEnvelope = decisionEnvelope(request);
    const envelope = await withinDecisionDeadline(deadline, remainingMs => client.request(
      requestEnvelope,
      remainingMs,
    )) as Envelope;
    const decision = fromDecisionResponse(envelope, room);
    if (performance.now() >= deadline) throw new Error('Engine decision timed out');
    return decision;
  } catch (error) {
    // A GUARANTEE_BELOW_REQUEST refusal is a contract response to a
    // caller-declared floor, not an operational outage. It must propagate so
    // the explicit caller can react; it never becomes a strategic fallback.
    if (error instanceof EngineError
      && error.code === ErrorCode.GUARANTEE_BELOW_REQUEST)
      throw error;
    // A malformed resident-root or flop-library configuration is a caller
    // error, not an outage; it must be visible rather than becoming a
    // per-turn silent fallback.
    if (error instanceof Error
      && (error.message.startsWith('invalid resident root configuration')
        || error.message.startsWith('invalid flop library configuration')))
      throw error;
    // Transport, validation, or capability failures are operational errors;
    // the mapper never converts them into a strategic fold.
    return safeFallback(room);
  }
}

export function closeEngine(): void {
  // Resets the implicit-client key too: a later decide() re-evaluates the
  // launch line instead of reusing the stopped client's key.
  defaultClient?.stop();
  defaultClient = null;
  defaultClientKey = '';
}

/**
 * Warm the engine client before the first decision. When the
 * config carries resident roots or flop libraries, the engine child
 * loads every resident root before answering its first frame (~2 min
 * for the full 1,755-class library). Calling this during startup
 * preflight moves that cost off the first hand's decision path.
 *
 * The client is a keyed singleton; decide() reuses the warmed child.
 * The caller logs preflight failure; decide() uses the legal operational
 * fallback on runtime failure. Long preflight waits belong off-table, while
 * an already-seated decision's readiness wait consumes its own timeout.
 */
export async function warmEngine(config: EngineConfig): Promise<void> {
  let roots: ResidentRootSpec[];
  let libraries: string[];
  let budget: number | undefined;
  try {
    roots = parseResidentRoots(config);
    const resolved = resolveFlopLibraries(config);
    libraries = resolved.libraries;
    budget = resolved.residentBudgetMiB;
  } catch {
    return; // Malformed config surfaces on the first decide() call
  }
  if (roots.length === 0 && libraries.length === 0) return;
  const client = engineClient(roots, libraries, budget);
  if (client) await client.start?.();
}
