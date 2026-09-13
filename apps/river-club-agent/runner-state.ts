import {
  isRiverRoom,
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
