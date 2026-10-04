import {
  isRecord,
} from '../../platforms/river-club/src/json.js';
import {
  isRiverRoom,
  type ExecutableDecision,
  type ResidentRootSpec,
  type RiverApiError,
  type RiverRoom,
  type RiverState,
} from '../../platforms/river-club/src/types.js';

export type RunnerStep =
  | { kind: 'off-table' }
  | { kind: 'leaving' }
  | { kind: 'heartbeat' }
  | { kind: 'empty-table'; room: RiverRoom }
  | { kind: 'stop-loss'; room: RiverRoom; total: number }
  | { kind: 'wait'; room: RiverRoom }
  | { kind: 'act'; room: RiverRoom };

export type PausedStep = 'resume' | 'stop' | 'wait';
export type RunnerMode = 'adaptive' | 'auto';
export type CliFailureStep =
  | { kind: 'retry'; delayMs: number }
  | { kind: 'off-table' }
  | { kind: 'leaving' }
  | { kind: 'fatal' };

export function selectRunnerStep(
  state: RiverState,
  heroName: string,
  baseline: number | null,
  stopChips: number,
): RunnerStep {
  const room = state.room;
  if (!room) return { kind: 'off-table' };
  if (room.mode === 'leaving') return { kind: 'leaving' };
  if (!isRiverRoom(room)) return { kind: 'heartbeat' };

  const opponents = room.seats.filter(seat =>
    seat !== null
    && seat.status !== 'leaving'
    && seat.name !== heroName);
  if (opponents.length === 0) return { kind: 'empty-table', room };

  if (baseline !== null && state.me?.wallet !== undefined) {
    const total = state.me.wallet + room.hero.stack;
    if (baseline - total >= stopChips) {
      return { kind: 'stop-loss', room, total };
    }
  }

  return room.legal ? { kind: 'act', room } : { kind: 'wait', room };
}

export function selectPausedStep(
  stopRequested: boolean,
  resumeRequested: boolean,
): PausedStep {
  if (stopRequested) return 'stop';
  return resumeRequested ? 'resume' : 'wait';
}

export function shouldRetryAction(code: string | undefined): boolean {
  return code === 'STALE_STATE' || code === 'NOT_YOUR_TURN';
}

export function selectCliFailureStep(error: RiverApiError): CliFailureStep {
  if (error.code === 'RATE_LIMITED') {
    return { kind: 'retry', delayMs: Math.max(0, error.retryAfterMs ?? 2_500) };
  }
  if (error.code === 'NOT_YOUR_TURN') return { kind: 'retry', delayMs: 100 };
  if (error.code === 'LEAVING') return { kind: 'leaving' };
  if (error.code === 'NOT_SEATED' || error.code === 'NOT_IN_ROOM') {
    return { kind: 'off-table' };
  }
  if (error.retryable) {
    return { kind: 'retry', delayMs: Math.max(0, error.retryAfterMs ?? 2_500) };
  }
  return { kind: 'fatal' };
}

export function engineBudgetMs(
  timeLeftMs: number,
  safetyMs: number,
  maximumMs = 2_000,
): number {
  const budgetMs = Math.max(0, Math.min(maximumMs, timeLeftMs - safetyMs));
  return budgetMs < 50 ? 0 : budgetMs;
}

export function overrideWindowMs(
  mode: RunnerMode,
  timeLeftMs: number,
  elapsedMs: number,
  safetyMs: number,
  minimumWindowMs: number,
  autoWindowMs: number,
): number {
  const availableMs = Math.max(0, timeLeftMs - elapsedMs - safetyMs);
  if (mode === 'auto') return Math.min(autoWindowMs, availableMs);
  if (availableMs < minimumWindowMs) return 0;
  return Math.min(20_000, availableMs);
}

// RFC 0009 W1: validate the runtime config's `residentRoots` value. Pure and
// exported so the runner's startup preflight and the tests share one
// definition. A malformed value THROWS: the runner fails fast rather than
// silently playing without the configured roots, and the throw must never be
// caught by the config file's parse fallback (an invalid root spec must not
// discard the sibling settings). Mirrors `parseResidentRoots` in
// platforms/river-club/src/engine.ts, which the launcher applies to the same
// values; the duplication is deliberate (the runner imports only the engine
// entry points it needs) and the two validators are pinned by tests.
export function parseConfigResidentRoots(value: unknown): ResidentRootSpec[] {
  if (!Array.isArray(value)) {
    throw new Error('residentRoots must be an array of {path, sha256} objects');
  }
  return value.map(entry => {
    if (!isRecord(entry)) {
      throw new Error('residentRoots entries must be {path, sha256} objects');
    }
    const path = typeof entry.path === 'string' ? entry.path : '';
    const sha256 = typeof entry.sha256 === 'string' ? entry.sha256 : '';
    if (!path || !/^[a-f0-9]{64}$/.test(sha256)) {
      throw new Error(
        'resident root requires a path and a 64-lowercase-hex sha256 pin',
      );
    }
    return { path, sha256 };
  });
}

// Flop class library directories the runtime config's `flopLibraries` value.
// Pure and exported so the runner's startup preflight and the tests share one
// definition. A malformed value THROWS: the runner fails fast rather than
// silently playing without the configured library. Mirrors
// `parseFlopLibraries` in platforms/river-club/src/engine.ts; the duplication
// is deliberate (the runner imports only the engine entry points it needs).
export function parseConfigFlopLibraries(value: unknown): string[] {
  if (!Array.isArray(value)) {
    throw new Error('flopLibraries must be an array of directory path strings');
  }
  return value.map(entry => {
    if (typeof entry !== 'string' || entry.length === 0) {
      throw new Error('flopLibraries entries must be non-empty strings');
    }
    return entry;
  });
}

// RFC 0008 stage 5 (R10/R13): pure builder for the .runtime/results.log entry.
// Extracted verbatim from main.ts's two log sites so the journal schema is
// unit-testable without spawning the runner: the failure `raw` payload is
// attached exactly when ok is false, and the decision (including its
// guaranteeLevel, the L6 journal-visibility payoff) is embedded as-is.
export interface ResultsLogEntryInput {
  kind: 'action' | 'action-retry';
  hand: string | undefined;
  street: string | undefined;
  source: string;
  decision: ExecutableDecision;
  ok: boolean;
  raw?: unknown;
}

export function resultsLogEntry(input: ResultsLogEntryInput): {
  kind: 'action' | 'action-retry';
  hand: string | undefined;
  street: string | undefined;
  source: string;
  decision: ExecutableDecision;
  ok: boolean;
  raw?: unknown;
} {
  return {
    kind: input.kind,
    hand: input.hand,
    street: input.street,
    source: input.source,
    decision: input.decision,
    ok: input.ok,
    ...(input.ok ? {} : { raw: input.raw }),
  };
}
