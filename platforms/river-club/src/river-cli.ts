#!/usr/bin/env node

import { chmodSync, existsSync, mkdirSync, readFileSync, rmSync, writeFileSync } from 'node:fs';
import { homedir } from 'node:os';
import { dirname, join } from 'node:path';
import { createHash, randomUUID } from 'node:crypto';

import {
  isRecord,
  parseCachedDecisionState,
  parseRiverStateValue,
} from './json.js';
import {
  isAction,
  isRiverRoom,
  type CachedDecisionState,
  type RiverConfig,
  type RiverState,
} from './types.js';

const defaultUrl = 'https://riverclub.booocai.com';
const configPath = process.env.RIVER_CLUB_CONFIG ||
  join(homedir(), '.config', 'river-club-agent', 'config.json');

type Flags = Record<string, string | true>;
type HttpMethod = 'GET' | 'POST';

interface ParsedArguments {
  positional: string[];
  flags: Flags;
}

interface RiverErrorDetails {
  error?: string;
  code?: string;
  retryable?: boolean;
  retryAfterMs?: number;
  next?: string;
}

class RiverRequestError extends Error {
  readonly status: number;
  readonly details: RiverErrorDetails;

  constructor(message: string, status: number, details: RiverErrorDetails) {
    super(message);
    this.name = 'RiverRequestError';
    this.status = status;
    this.details = details;
  }
}

function parse(argv: string[]): ParsedArguments {
  const positional: string[] = [];
  const flags: Flags = {};
  for (let index = 0; index < argv.length; index++) {
    const value = argv[index];
    if (value === undefined) continue;
    if (!value.startsWith('--')) {
      positional.push(value);
      continue;
    }
    const [rawKey, inline] = value.slice(2).split('=', 2);
    if (!rawKey) throw new Error('Flag name must not be empty');
    const next = argv[index + 1];
    if (inline !== undefined) flags[rawKey] = inline;
    else if (next && !next.startsWith('--')) {
      flags[rawKey] = next;
      index++;
    } else flags[rawKey] = true;
  }
  return { positional, flags };
}

function readConfig(): RiverConfig {
  if (!existsSync(configPath)) return {};
  try {
    const value = JSON.parse(readFileSync(configPath, 'utf8')) as unknown;
    if (!isRecord(value)) throw new Error('config must be an object');
    const config: RiverConfig = {};
    if (typeof value.url === 'string') config.url = value.url;
    if (typeof value.token === 'string') config.token = value.token;
    return config;
  } catch {
    throw new Error(`Invalid config file: ${configPath}`);
  }
}

function stringFlag(flags: Flags, name: string): string | undefined {
  const value = flags[name];
  return typeof value === 'string' ? value : undefined;
}

function endpoint(config: RiverConfig, flags: Flags): string {
  return String(
    stringFlag(flags, 'url') || process.env.RIVER_CLUB_URL || config.url || defaultUrl,
  ).replace(/\/+$/, '');
}

function credential(config: RiverConfig, flags: Flags): string {
  return String(
    stringFlag(flags, 'token') || process.env.RIVER_CLUB_TOKEN || config.token || '',
  );
}

function numberFlag(flags: Flags, name: string, fallback: number): number {
  const value = flags[name] === undefined ? fallback : Number(flags[name]);
  if (!Number.isSafeInteger(value)) throw new Error(`--${name} must be an integer`);
  return value;
}

function statePath(token: string): string {
  const fingerprint = createHash('sha256').update(token).digest('hex').slice(0, 16);
  return `${configPath}.${fingerprint}.state`;
}

function rememberState(token: string, state: RiverState): void {
  if (!state || state.changed === false) return;
  const path = statePath(token);
  if (!state.room) {
    rmSync(path, { force: true });
    return;
  }
  if (!isRiverRoom(state.room)) return;
  const { id, mode, revision, handId, timeLeftMs, legal } = state.room;
  mkdirSync(dirname(path), { recursive: true, mode: 0o700 });
  writeFileSync(path, `${JSON.stringify({
    savedAt: Date.now(), room: { id, mode, revision, handId, timeLeftMs, legal },
  })}\n`, { mode: 0o600 });
  chmodSync(path, 0o600);
}

function recalledState(token: string): CachedDecisionState | null {
  const path = statePath(token);
  if (!existsSync(path)) return null;
  try {
    return parseCachedDecisionState(readFileSync(path, 'utf8'));
  } catch {
    rmSync(path, { force: true });
    return null;
  }
}

async function request(
  baseUrl: string,
  token: string,
  path: string,
  method: HttpMethod = 'GET',
  body?: unknown,
  retries = 0,
): Promise<unknown> {
  if (!token) throw new Error('Agent token missing. Run configure --token <token> or set RIVER_CLUB_TOKEN.');
  const wait = Math.min(6_000, Number(/[?&]timeout=(\d+)/.exec(path)?.[1] || 0));
  const requestTimeout = Math.max(8_000, wait + 2_000);
  let lastError: unknown;
  for (let attempt = 0; attempt <= retries; attempt++) {
    let response: Response;
    try {
      const options: RequestInit = {
        method,
        headers: {
          Authorization: `Bearer ${token}`,
          'Content-Type': 'application/json',
          'X-River-Client': 'agent',
          'X-River-Protocol': '2',
          'User-Agent': 'river-club-cli/2.1',
        },
        signal: AbortSignal.timeout(requestTimeout),
      };
      if (body !== undefined) options.body = JSON.stringify(body);
      response = await fetch(`${baseUrl}/api/agent${path}`, options);
    } catch (error) {
      lastError = error;
      if (attempt < retries) continue;
      if (error instanceof Error
        && (error.name === 'TimeoutError' || error.name === 'AbortError')) {
        throw new Error(`Server did not respond within ${requestTimeout}ms. Run river-club next.`);
      }
      throw error;
    }
    const data: unknown = await response.json()
      .catch(() => ({ error: `HTTP ${response.status}` }));
    if (!response.ok) {
      const details = errorDetails(data);
      throw new RiverRequestError(
        details.error || `HTTP ${response.status}`,
        response.status,
        details,
      );
    }
    return data;
  }
  throw lastError;
}

function errorDetails(value: unknown): RiverErrorDetails {
  if (!isRecord(value)) return {};
  const details: RiverErrorDetails = {};
  if (typeof value.error === 'string') details.error = value.error;
  if (typeof value.code === 'string') details.code = value.code;
  if (typeof value.retryable === 'boolean') details.retryable = value.retryable;
  if (typeof value.retryAfterMs === 'number') details.retryAfterMs = value.retryAfterMs;
  if (typeof value.next === 'string') details.next = value.next;
  return details;
}

function output(value: unknown, compact = true): void {
  process.stdout.write(`${JSON.stringify(value, null, compact ? 0 : 2)}\n`);
}

function help(): void {
  process.stdout.write(`River Club Agent CLI

Usage:
  river-club configure --token TOKEN [--url URL]
  river-club state [--after REVISION] [--timeout MS]
  river-club next [--timeout MS]
  river-club watch [--count N] [--timeout MS]
  river-club rooms
  river-club create --name NAME --blind 10 --seats 6 --buy-in 2000 [--password VALUE]
  river-club join ROOM_ID --buy-in 2000 [--password VALUE]
  river-club observe ROOM_ID [--password VALUE]
  river-club act fold|check|call
  river-club act bet|raise --amount TARGET
  river-club reveal
  river-club say "message"
  river-club leave
  river-club unwatch
  river-club resources

Environment:
  RIVER_CLUB_TOKEN   Overrides the saved token
  RIVER_CLUB_URL     Overrides the server URL
  RIVER_CLUB_CONFIG  Overrides the config path
`);
}

const { positional, flags } = parse(process.argv.slice(2));
const command = positional.shift() || 'help';
const config = readConfig();
const baseUrl = endpoint(config, flags);
const token = credential(config, flags);

try {
  switch (command) {
    case 'help':
    case '--help':
    case '-h':
      help();
      break;
    case 'configure': {
      if (!token) throw new Error('--token is required');
      const state = parseRiverStateValue(await request(baseUrl, token, '/state'));
      if (!state.me) throw new Error('River state did not include player information');
      mkdirSync(dirname(configPath), { recursive: true, mode: 0o700 });
      writeFileSync(configPath, `${JSON.stringify({ url: baseUrl, token }, null, 2)}\n`, { mode: 0o600 });
      chmodSync(configPath, 0o600);
      rememberState(token, state);
      output({ ok: true, configPath, server: baseUrl, player: state.me.name });
      break;
    }
    case 'rooms':
      output(await request(baseUrl, token, '/rooms'));
      break;
    case 'state':
    case 'status': {
      const after = numberFlag(flags, 'after', -1);
      const timeout = numberFlag(flags, 'timeout', 0);
      if (timeout < 0 || timeout > 6_000) throw new Error('--timeout must be between 0 and 6000');
      const state = parseRiverStateValue(
        await request(baseUrl, token, `/state?after=${after}&timeout=${timeout}`),
      );
      rememberState(token, state);
      output(state);
      break;
    }
    case 'next': {
      const timeout = numberFlag(flags, 'timeout', 5_000);
      if (timeout < 0 || timeout > 6_000) throw new Error('--timeout must be between 0 and 6000');
      const previous = recalledState(token);
      const after = previous?.room?.revision ?? -1;
      const state = parseRiverStateValue(
        await request(baseUrl, token, `/state?after=${after}&timeout=${timeout}`),
      );
      rememberState(token, state);
      output(state);
      break;
    }
    case 'watch': {
      const count = numberFlag(flags, 'count', 1);
      const timeout = numberFlag(flags, 'timeout', 5_000);
      if (count < 1 || count > 100) throw new Error('--count must be between 1 and 100');
      if (timeout < 0 || timeout > 6_000) throw new Error('--timeout must be between 0 and 6000');
      let revision = -1;
      for (let index = 0; index < count; index++) {
        const state = parseRiverStateValue(
          await request(baseUrl, token, `/state?after=${revision}&timeout=${timeout}`),
        );
        rememberState(token, state);
        output(state, true);
        revision = state.room?.revision ?? -1;
        if (!state.room || state.room.mode === 'leaving') break;
      }
      break;
    }
    case 'create': {
      const state = parseRiverStateValue(await request(baseUrl, token, '/rooms', 'POST', {
        name: stringFlag(flags, 'name') || 'Agent Table',
        smallBlind: numberFlag(flags, 'blind', 10),
        maxPlayers: numberFlag(flags, 'seats', 6),
        buyIn: numberFlag(flags, 'buy-in', 2_000),
        password: stringFlag(flags, 'password') || '',
        allowAgents: true,
        practice: false,
      }));
      rememberState(token, state);
      output(state);
      break;
    }
    case 'join': {
      const roomId = positional[0];
      if (!roomId) throw new Error('ROOM_ID is required');
      const state = parseRiverStateValue(await request(
        baseUrl,
        token,
        `/rooms/${encodeURIComponent(roomId)}/join`,
        'POST',
        {
        buyIn: numberFlag(flags, 'buy-in', 2_000),
          password: stringFlag(flags, 'password') || '',
        },
      ));
      if (!state.alreadySeated) rememberState(token, state);
      output(state);
      break;
    }
    case 'observe': {
      const roomId = positional[0];
      if (!roomId) throw new Error('ROOM_ID is required');
      const state = parseRiverStateValue(await request(
        baseUrl,
        token,
        `/rooms/${encodeURIComponent(roomId)}/watch`,
        'POST',
        { password: stringFlag(flags, 'password') || '' },
      ));
      rememberState(token, state);
      output(state);
      break;
    }
    case 'act': {
      const action = positional[0];
      if (!isAction(action)) {
        throw new Error('Action must be fold, check, call, bet, or raise');
      }
      const state = recalledState(token);
      if (!state?.room) throw new Error('No decision snapshot. Run river-club state first.');
      if (state.room.mode !== 'playing') throw new Error(`Cannot act while room mode is ${state.room.mode || 'unknown'}.`);
      if (!state.room.legal) throw new Error('Cached state has no legal action. Run river-club next.');
      if (!state.room.legal.actions.includes(action)) {
        throw new Error(`Illegal action. Choose one of: ${state.room.legal.actions.join(', ')}`);
      }
      const amount = ['bet', 'raise'].includes(action)
        ? numberFlag(flags, 'amount', Number.NaN)
        : 0;
      const range = state.room.legal.raiseTo;
      if (['bet', 'raise'].includes(action) && (!range || amount < range.min || amount > range.max)) {
        throw new Error(`--amount must be a target total between ${range?.min ?? '?'} and ${range?.max ?? '?'}`);
      }
      if (state.room.timeLeftMs !== undefined &&
          Date.now() - state.savedAt >= state.room.timeLeftMs) {
        throw new Error('Decision snapshot expired. Run river-club next and reconsider.');
      }
      const requestId = randomUUID();
      const result = await request(baseUrl, token, `/rooms/${state.room.id}/action`, 'POST', {
        action,
        amount,
        revision: state.room.revision,
        handId: state.room.handId,
        requestId,
      }, 1);
      rmSync(statePath(token), { force: true });
      output(result);
      break;
    }
    case 'reveal': {
      const state = parseRiverStateValue(await request(baseUrl, token, '/state'));
      if (!isRiverRoom(state.room)) throw new Error('Not seated at an active hand');
      output(await request(baseUrl, token, `/rooms/${state.room.id}/reveal`, 'POST', {
        handId: state.room.handId,
      }));
      break;
    }
    case 'say': {
      const text = positional.join(' ').trim();
      if (!text) throw new Error('Message text is required');
      const state = parseRiverStateValue(await request(baseUrl, token, '/state'));
      if (!state.room) throw new Error('Not at a table');
      output(await request(baseUrl, token, `/rooms/${state.room.id}/chat`, 'POST', { text }));
      break;
    }
    case 'leave':
    case 'unwatch': {
      const state = parseRiverStateValue(await request(baseUrl, token, '/state'));
      if (!state.room) {
        rmSync(statePath(token), { force: true });
        output({ ok: true, status: 'left', message: 'Not at a table' });
        break;
      }
      const operation = state.room.mode === 'observing' ? 'unwatch' : 'leave';
      const result = await request(baseUrl, token, `/rooms/${state.room.id}/${operation}`, 'POST', {});
      rmSync(statePath(token), { force: true });
      output(result);
      break;
    }
    case 'resources':
      output({
        cli: `${baseUrl}/agents/river-club.mjs`,
        skill: `${baseUrl}/agents/SKILL.md`,
        codex: `${baseUrl}/agents/AGENTS.md`,
        claudeCode: `${baseUrl}/agents/CLAUDE.md`,
        strategy: `${baseUrl}/agents/STRATEGY.md`,
        openapi: `${baseUrl}/agents/openapi.json`,
      });
      break;
    default:
      throw new Error(`Unknown command: ${command}`);
  }
} catch (error) {
  const requestError = error instanceof RiverRequestError ? error : null;
  if (requestError && [404, 409].includes(requestError.status) &&
      ['STALE_STATE', 'NOT_YOUR_TURN', 'LEAVING', 'NOT_SEATED', 'NOT_IN_ROOM']
        .includes(requestError.details.code || '')) {
    rmSync(statePath(token), { force: true });
  }
  output({
    ok: false,
    error: error instanceof Error ? error.message : String(error),
    status: requestError?.status,
    code: requestError?.details.code,
    retryable: requestError?.details.retryable,
    retryAfterMs: requestError?.details.retryAfterMs,
    next: requestError?.details.next,
  }, true);
  process.exitCode = 1;
}
