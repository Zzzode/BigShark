#!/usr/bin/env node
// play.mjs — adaptive hybrid runner.
//
//   wait (next long-poll) → turn due → engine decides instantly (fallback)
//   → open a model-override window sized from the room clock → validate & act.
//
// The model never burns the clock: if its .runtime/action.json override arrives
// inside the window it is used; otherwise the engine action fires well before
// the deadline. Window = timeLeftMs - safetyMs (adaptive), so the same loop
// works on 10s tables (engine-led) and 30s+ tables (model-led).
//
// Runtime files under .runtime/ (gitignored):
//   config.json     {mode:'adaptive'|'auto', style:'tag', safetyMs, stopChips, maxBb}
//   pending.log     one JSON line per actionable snapshot (Monitor this → decide → write action.json)
//   action.json     model override: {handId, street, action, amount?}
//   results.log     one JSON line per executed action (engine|model) and hand result
//   stop            touch to stop after the current hand, then leave cleanly
//
// Usage: play.mjs [--hands N] [--max-ms M] [--stop-chips 4000] [--max-bb 50]
//                 [--style tag|station-hunter|lag] [--mode adaptive|auto]

import { spawnSync } from 'node:child_process';
import { appendFileSync, existsSync, mkdirSync, readFileSync, rmSync, writeFileSync } from 'node:fs';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import {
  decide,
  safeFallback,
} from '../../platforms/river-club/src/engine.js';
import {
  isRecord,
  parseJson,
  parseRiverError,
  parseRiverState,
  parseRoomsResponse,
} from '../../platforms/river-club/src/json.js';
import {
  isAction,
  isRiverRoom,
  isStreet,
  type ExecutableDecision,
  type RiverApiError,
  type RiverRoom,
  type RiverRoomSummary,
  type RiverState,
  type Street,
} from '../../platforms/river-club/src/types.js';
import {
  engineBudgetMs,
  overrideWindowMs,
  resultsLogEntry,
  selectCliFailureStep,
  selectPausedStep,
  selectRunnerStep,
  shouldRetryAction,
  type RunnerMode,
} from './runner-state.js';

const here = dirname(fileURLToPath(import.meta.url));
const root = resolve(here, '../../..');
const riverCli = resolve(here, '../../platforms/river-club/src/journal-cli.js');
const rt = join(root, '.runtime');
mkdirSync(rt, { recursive: true });
const PEND = join(rt, 'pending.log');
const PEND_LAST = join(rt, 'pending.json');
const ACTION = join(rt, 'action.json');
const RESULTS = join(rt, 'results.log');
const STOP = join(rt, 'stop');
const RESUME = join(rt, 'resume');
const AWAITING = join(rt, 'awaiting-table.json');
const BASELINE_F = join(rt, 'baseline.json');

type DecisionSource = 'engine' | 'model';

interface RunnerConfig {
  mode?: RunnerMode;
  style?: string;
  safetyMs?: number;
  minWindowMs?: number;
  autoWindowMs?: number;
}

interface ModelOverride {
  handId: string;
  street: Street;
  action: ExecutableDecision['action'];
  amount?: number;
  reason?: string;
}

const argv = process.argv.slice(2);
const flag = (name: string): string | undefined => {
  const index = argv.indexOf(`--${name}`);
  return index >= 0 ? argv[index + 1] : undefined;
};
const maxHandsFlag = flag('hands');
const maxMsFlag = flag('max-ms');
const stopChipsFlag = flag('stop-chips');
const maxBbFlag = flag('max-bb');
const maxHands = maxHandsFlag ? Number(maxHandsFlag) : Infinity;
const maxMs = maxMsFlag ? Number(maxMsFlag) : Infinity;
const stopChips = stopChipsFlag ? Number(stopChipsFlag) : 4000;
const maxBb = maxBbFlag ? Number(maxBbFlag) : 50;
const argStyle = flag('style');
const argMode = flag('mode');

const cfg = readRunnerConfig(join(rt, 'config.json'));
const style0 = argStyle || cfg.style || 'tag';
const mode0: RunnerMode = argMode === 'auto' || argMode === 'adaptive'
  ? argMode
  : cfg.mode || 'adaptive';
const safetyMs = cfg.safetyMs ?? 6000;
const minWindowMs = cfg.minWindowMs ?? 1200;
const autoWindowMs = cfg.autoWindowMs ?? 1500;

const style = style0;
const cli = (args: string[]) => spawnSync(process.execPath, [riverCli, ...args], {
  encoding: 'utf8', maxBuffer: 8 * 1024 * 1024,
});
const sleep = (ms: number): Promise<void> => new Promise(resolve => setTimeout(resolve, ms));
const log = (file: string, value: Record<string, unknown>): void => {
  appendFileSync(file, `${JSON.stringify({ ts: Date.now(), ...value })}\n`);
};

function note(value: Record<string, unknown>): void {
  process.stderr.write(`${JSON.stringify(value)}\n`);
}

function digest(room: RiverRoom): Record<string, unknown> {
  const h = room.hero;
  return {
    hand: room.handId, st: room.street, t_ms: room.timeLeftMs,
    pos: h?.position, cards: h?.cards?.map(c => c.rank + c.suit[0]).join('') || '',
    stackBb: h?.effectiveStackBb, potBb: room.solver?.potBb, spr: room.solver?.spr,
    toCallBb: room.solver?.toCallBb, n: room.solver?.playersInHand,
    legal: room.legal ? { acts: room.legal.actions, call: room.legal.call, odds: room.legal.potOdds, to: room.legal.raiseTo } : null,
    board: (room.board || []).map(c => c.rank + c.suit[0]).join(''),
    ev: (room.events || []).slice(-8).map(e => e.text),
  };
}

function readCliError(
  stdout: string,
  stderr: string,
  status: number | null,
): RiverApiError {
  try {
    const parsed = parseRiverError(stdout);
    if (parsed) return parsed;
  } catch { /* use the process output below */ }
  const error: RiverApiError = {
    ok: false,
    error: (stdout || stderr || 'River CLI request failed').trim().slice(0, 200),
  };
  if (status !== null) error.status = status;
  return error;
}

const denied = new Set<string>();
async function findAndJoin(): Promise<boolean> {
  while (Date.now() < startedAt + maxMs && !existsSync(STOP)) {
    const roomsResult = cli(['rooms']);
    if (roomsResult.status !== 0) {
      const error = readCliError(
        roomsResult.stdout,
        roomsResult.stderr,
        roomsResult.status,
      );
      const failure = selectCliFailureStep(error);
      note({ event: 'rooms-failed', code: error.code, error: error.error });
      if (failure.kind === 'retry') {
        await sleep(failure.delayMs);
        continue;
      }
      return false;
    }

    let availableRooms: RiverRoomSummary[];
    try {
      availableRooms = parseRoomsResponse(roomsResult.stdout).rooms;
    } catch {
      await sleep(2_500);
      continue;
    }
    const open = availableRooms
      .filter(r => r.allowAgents && r.status === 'playing' && r.seats[0] > 0
        && r.blinds[1] <= maxBb && r.seats[0] < r.seats[1] && !denied.has(r.id))
      .sort((a, b) => (b.seats[0] - a.seats[0]) || (a.blinds[1] - b.blinds[1]));
    for (const r of open) {
      const buyIn = r.blinds[1] * 100;
      const res = cli(['join', r.id, '--buy-in', String(buyIn)]);
      let p: RiverState | null = null;
      if (res.status === 0) {
        try { p = parseRiverState(res.stdout); } catch { /* invalid success response */ }
      }
      const error = res.status === 0
        ? null
        : readCliError(res.stdout, res.stderr, res.status);
      const code = error?.code;
      if (res.status === 0 && p?.room) { note({ event: 'joined', id: r.id, name: r.name, bb: r.blinds[1], buyIn }); return true; }
      if (p?.alreadySeated || code === 'ALREADY_SEATED') return true;
      if (code && ['ROOM_PASSWORD_INVALID', 'AGENTS_DISABLED', 'ROOM_FULL'].includes(code)) {
        denied.add(r.id);
        continue;
      }
      if (error) {
        const failure = selectCliFailureStep(error);
        if (failure.kind === 'retry') {
          await sleep(failure.delayMs);
          continue;
        }
        if (failure.kind === 'fatal') return false;
      }
      note({ event: 'join-failed', id: r.id, raw: (res.stdout || res.stderr || '').slice(0, 200) });
    }
    await sleep(5000);
  }
  return false;
}

function stopRequested(): boolean { return existsSync(STOP); }

// Table disbanded / emptied → PAUSE and wait for explicit user approval
// (`touch .runtime/resume`) instead of silently buying into another table.
async function pauseForApproval(reason: string): Promise<boolean> {
  note({ event: 'awaiting-approval', reason });
  writeFileSync(AWAITING, JSON.stringify({ since: Date.now(), reason }));
  while (true) {
    const step = selectPausedStep(stopRequested(), existsSync(RESUME));
    if (step === 'stop') break;
    if (step === 'resume') {
      rmSync(RESUME, { force: true });
      rmSync(AWAITING, { force: true });
      note({ event: 'approval-resumed' });
      return true;
    }
    await sleep(2000);
  }
  rmSync(AWAITING, { force: true });
  return false;
}

function readRunnerConfig(path: string): RunnerConfig {
  if (!existsSync(path)) return {};
  try {
    const value = parseJson(readFileSync(path, 'utf8'));
    if (!isRecord(value)) return {};
    const config: RunnerConfig = {};
    if (value.mode === 'adaptive' || value.mode === 'auto') config.mode = value.mode;
    if (typeof value.style === 'string') config.style = value.style;
    if (typeof value.safetyMs === 'number') config.safetyMs = value.safetyMs;
    if (typeof value.minWindowMs === 'number') config.minWindowMs = value.minWindowMs;
    if (typeof value.autoWindowMs === 'number') config.autoWindowMs = value.autoWindowMs;
    return config;
  } catch {
    return {};
  }
}

function readModelOverride(path: string): ModelOverride | null {
  try {
    const value = parseJson(readFileSync(path, 'utf8'));
    if (!isRecord(value)
      || typeof value.handId !== 'string'
      || !isStreet(value.street)
      || !isAction(value.action)) {
      return null;
    }
    const override: ModelOverride = {
      handId: value.handId,
      street: value.street,
      action: value.action,
    };
    if (typeof value.amount === 'number') override.amount = value.amount;
    if (typeof value.reason === 'string') override.reason = value.reason;
    return override;
  } catch {
    return null;
  }
}

function validOverride(
  room: RiverRoom,
  override: ModelOverride | null,
): ExecutableDecision | null {
  if (!override || !room.legal?.actions.includes(override.action)) return null;
  if (override.action === 'bet' || override.action === 'raise') {
    const range = room.legal.raiseTo;
    if (!range
      || override.amount === undefined
      || !Number.isInteger(override.amount)
      || override.amount < range.min
      || override.amount > range.max) {
      return null;
    }
  }
  return {
    action: override.action,
    ...(override.amount === undefined ? {} : { amount: override.amount }),
    reason: override.reason || 'model',
  };
}

function responseSucceeded(status: number | null, value: unknown): boolean {
  return status === 0 && isRecord(value) && value.ok !== false;
}

function responseCode(value: unknown): string | undefined {
  if (!isRecord(value)) return undefined;
  if (typeof value.code === 'string') return value.code;
  return isRecord(value.details) && typeof value.details.code === 'string'
    ? value.details.code
    : undefined;
}

async function decideWithinBudget(room: RiverRoom): Promise<ExecutableDecision> {
  const timeoutMs = engineBudgetMs(room.timeLeftMs ?? 0, safetyMs);
  if (timeoutMs === 0) return safeFallback(room);
  return decide(room, { style, heroName, timeoutMs });
}

async function loadInitialState(): Promise<RiverState> {
  while (!stopRequested()) {
    const result = cli(['state']);
    if (result.status === 0) return parseRiverState(result.stdout);
    const error = readCliError(result.stdout, result.stderr, result.status);
    const failure = selectCliFailureStep(error);
    note({ event: 'initial-state-failed', code: error.code, error: error.error });
    if (failure.kind === 'retry') {
      await sleep(failure.delayMs);
      continue;
    }
    if (failure.kind === 'leaving') {
      await sleep(1_000);
      continue;
    }
    throw new Error(error.error);
  }
  throw new Error('Stop requested before initial state was available');
}

const startedAt = Date.now();
let acted = 0;
let baseline: number | null = null;
let timerMs: number | null = null;
let heroName = '';

rmSync(ACTION, { force: true });

// confirm seat; baseline = total bankroll (off-table wallet + on-table stack),
// persisted across restarts so stop-loss survives process bounces.
let s0: RiverState;
try {
  s0 = await loadInitialState();
} catch (error) {
  note({
    event: 'initial-state-unavailable',
    error: error instanceof Error ? error.message : String(error),
  });
  process.exit(1);
}
if (s0.me) heroName = s0.me.name;
if (existsSync(BASELINE_F)) {
  try {
    const saved = parseJson(readFileSync(BASELINE_F, 'utf8'));
    if (isRecord(saved) && typeof saved.baseline === 'number') baseline = saved.baseline;
  } catch { /* invalid baseline is replaced below */ }
} else if (s0.me) {
  const wallet = s0.me.wallet ?? 0;
  baseline = wallet + (isRiverRoom(s0.room) ? s0.room.hero.stack : 0);
}
if (baseline === null && s0.me) {
  baseline = (s0.me.wallet ?? 0) + (isRiverRoom(s0.room) ? s0.room.hero.stack : 0);
}
if (baseline !== null && !existsSync(BASELINE_F)) {
  writeFileSync(BASELINE_F, JSON.stringify({ baseline, at: Date.now() }));
}
if (!s0.room) await findAndJoin();

while (Date.now() < startedAt + maxMs && acted < maxHands && !existsSync(STOP)) {
  const nextResult = cli(['next', '--timeout', '6000']);
  if (nextResult.status !== 0) {
    const error = readCliError(nextResult.stdout, nextResult.stderr, nextResult.status);
    const failure = selectCliFailureStep(error);
    note({ event: 'next-failed', code: error.code, error: error.error });
    if (failure.kind === 'retry') {
      await sleep(failure.delayMs);
      continue;
    }
    if (failure.kind === 'leaving') {
      await sleep(1_000);
      continue;
    }
    if (failure.kind === 'off-table') {
      if (!(await pauseForApproval('table-ended'))) break;
      if (!(await findAndJoin())) break;
      continue;
    }
    break;
  }

  let s: RiverState;
  try { s = parseRiverState(nextResult.stdout); }
  catch { await sleep(2500); continue; }

  if (s?.me) heroName = s.me.name || heroName;

  const step = selectRunnerStep(s, heroName, baseline, stopChips);
  if (step.kind === 'off-table') {
    note({ event: 'off-table' });
    if (!(await pauseForApproval('table-ended'))) break;
    if (!(await findAndJoin())) break;
    continue;
  }
  if (step.kind === 'leaving') { await sleep(1000); continue; }
  if (step.kind === 'heartbeat') { await sleep(200); continue; }
  if (step.kind === 'empty-table') {
    note({ event: 'empty-table-leave' });
    cli(['leave']);
    await sleep(2000);
    if (!(await pauseForApproval('empty-table'))) break;
    if (!(await findAndJoin())) break;
    continue;
  }
  if (step.kind === 'stop-loss') {
    note({ event: 'stop-loss', baseline, total: step.total });
    break;
  }
  const room = step.room;

  if (room.winners?.length) log(RESULTS, { kind: 'hand-end', hand: room.handId, winners: room.winners, pot: room.pot });
  if (step.kind === 'wait') continue;

  // ---- our decision point ----
  const timeLeftMs = room.timeLeftMs ?? 0;
  if (timerMs === null) {
    timerMs = timeLeftMs;
    note({ event: 'timer-detected', ms: timerMs });
  }
  timerMs = Math.max(timerMs, timeLeftMs);

  const decisionStartedAt = Date.now();
  const fb = await decideWithinBudget(room);
  const windowMs = overrideWindowMs(
    mode0,
    timeLeftMs,
    Date.now() - decisionStartedAt,
    safetyMs,
    minWindowMs,
    autoWindowMs,
  );

  const pending = { ...digest(room), style, engine: fb, windowMs, deadline: Date.now() + windowMs };
  writeFileSync(PEND_LAST, JSON.stringify(pending));
  log(PEND, pending);

  // ---- model override window ----
  let override: ModelOverride | null = null;
  let source: DecisionSource = 'engine';
  const end = Date.now() + windowMs;
  while (Date.now() < end) {
    if (existsSync(ACTION)) {
      const candidate = readModelOverride(ACTION);
      if (candidate?.handId === room.handId && candidate.street === room.street) {
        override = candidate;
        break;
      }
    }
    await sleep(120);
  }
  rmSync(ACTION, { force: true });

  // Refresh snapshot right before firing: joins/leaves/chat during our window
  // bump the server revision and would otherwise make the bound action STALE.
  const refreshResult = cli(['state']);
  if (refreshResult.status !== 0) {
    const error = readCliError(
      refreshResult.stdout,
      refreshResult.stderr,
      refreshResult.status,
    );
    note({ event: 'refresh-failed', code: error.code, error: error.error });
    continue;
  }
  let fresh: RiverState;
  try {
    fresh = parseRiverState(refreshResult.stdout || '{}');
  } catch {
    note({ event: 'refresh-invalid' });
    continue;
  }
  const fr = fresh.room;
  if (!isRiverRoom(fr)
    || !fr.legal
    || fr.handId !== room.handId
    || fr.street !== room.street) {
    note({
      event: 'refresh-skip',
      hand: isRiverRoom(fr) ? fr.handId : undefined,
      street: isRiverRoom(fr) ? fr.street : undefined,
      next: fr?.next,
    });
    continue; // turn moved on; loop will see the new state
  }

  let decision: ExecutableDecision;
  const modelDecision = validOverride(fr, override);
  if (modelDecision) {
    decision = modelDecision;
    source = 'model';
  } else {
    if (override) note({ event: 'model-decision-invalid', raw: override });
    decision = await decideWithinBudget(fr);
    source = 'engine';
  }

  const actArgs = ['act', decision.action];
  if (decision.action === 'bet' || decision.action === 'raise') actArgs.push('--amount', String(decision.amount));
  const res = cli(actArgs);
  let raw: unknown = null;
  try { raw = parseJson(res.stdout); } catch {
    raw = (res.stdout || res.stderr || '').slice(0, 200);
  }
  const ok = responseSucceeded(res.status, raw);
  log(RESULTS, resultsLogEntry(
    { kind: 'action', hand: room.handId, street: room.street, source, decision, ok,
      raw: ok ? undefined : raw }));
  note({ event: 'acted', source, action: decision.action, amount: decision.amount ?? null, reason: decision.reason, ok });

  // Stale snapshot (network stall + server deadline): ONE immediate engine
  // re-decision on fresh state, no override window — recover the fold/check
  // before the auto-fold timer kills the hand.
  if (!ok && shouldRetryAction(responseCode(raw))) {
    const retryResult = cli(['state']);
    let retryState: RiverState | null = null;
    if (retryResult.status === 0) {
      try { retryState = parseRiverState(retryResult.stdout || '{}'); } catch { /* */ }
    }
    const retryRoom = retryState?.room;
    if (isRiverRoom(retryRoom)
      && retryRoom.legal
      && retryRoom.handId === room.handId
      && retryRoom.street === room.street) {
      const d2 = await decideWithinBudget(retryRoom);
      const args2 = ['act', d2.action];
      if (d2.action === 'bet' || d2.action === 'raise') args2.push('--amount', String(d2.amount));
      const r2 = cli(args2);
      let raw2: unknown = null;
      try { raw2 = parseJson(r2.stdout); } catch { raw2 = r2.stdout?.slice(0, 160); }
      const ok2 = responseSucceeded(r2.status, raw2);
      log(RESULTS, resultsLogEntry(
        { kind: 'action-retry', hand: retryRoom.handId, street: retryRoom.street,
          source: 'engine-recovery', decision: d2, ok: ok2, raw: ok2 ? undefined : raw2 }));
      note({ event: 'acted', source: 'engine-recovery', action: d2.action, amount: d2.amount ?? null, reason: d2.reason, ok: ok2 });
      if (ok2) { acted++; continue; }
    }
  }
  if (ok) acted++;
  else await sleep(1500);
}

// graceful exit
note({ event: 'stopping', acted });
cli(['leave']);
for (let i = 0; i < 30; i++) {
  let state: RiverState;
  try { state = parseRiverState(cli(['next', '--timeout', '1000']).stdout || '{}'); }
  catch { await sleep(1000); continue; }
  if (state.room === null || state.room === undefined) break;
  await sleep(1000);
}
note({ event: 'stopped' });
