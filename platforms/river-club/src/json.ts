import type {
  CachedDecisionState,
  RiverApiError,
  RiverRoomSummary,
  RiverRoomsResponse,
  RiverState,
} from './types.js';
import {
  isRiverRoom,
  isRiverRoomReference,
} from './types.js';

export function parseJson(text: string): unknown {
  return JSON.parse(text) as unknown;
}

export function parseRiverState(text: string): RiverState {
  return parseRiverStateValue(parseJson(text));
}

export function parseRiverStateValue(value: unknown): RiverState {
  if (!isRecord(value)) throw new Error('River response must be an object');
  if (value.ok === false) throw new Error('River error response is not state');
  if (value.me !== undefined && !isRiverMe(value.me)) {
    throw new Error('River player information is invalid');
  }
  if (value.room !== undefined
    && value.room !== null
    && !isRiverRoom(value.room)
    && !isRiverRoomReference(value.room)) {
    throw new Error('River room state is invalid');
  }
  return value as unknown as RiverState;
}

export function parseRiverError(text: string): RiverApiError | null {
  const value = parseJson(text);
  if (!isRecord(value) || value.ok !== false) return null;
  const details = isRecord(value.details) ? value.details : null;
  const error: RiverApiError = {
    ok: false,
    error: typeof value.error === 'string' ? value.error : 'River CLI request failed',
  };
  if (typeof value.status === 'number') error.status = value.status;
  if (typeof value.code === 'string') error.code = value.code;
  else if (typeof details?.code === 'string') error.code = details.code;
  if (typeof value.retryable === 'boolean') error.retryable = value.retryable;
  else if (typeof details?.retryable === 'boolean') error.retryable = details.retryable;
  if (typeof value.retryAfterMs === 'number') error.retryAfterMs = value.retryAfterMs;
  else if (typeof details?.retryAfterMs === 'number') {
    error.retryAfterMs = details.retryAfterMs;
  }
  if (typeof value.next === 'string') error.next = value.next;
  else if (typeof details?.next === 'string') error.next = details.next;
  return error;
}

export function parseRoomsResponse(text: string): RiverRoomsResponse {
  const value = parseJson(text);
  if (!isRecord(value)
    || !Array.isArray(value.rooms)
    || !value.rooms.every(isRoomSummary)) {
    throw new Error('River rooms response must contain a rooms array');
  }
  return { rooms: value.rooms };
}

export function parseCachedDecisionState(text: string): CachedDecisionState {
  const value = parseJson(text);
  if (!isRecord(value)
    || typeof value.savedAt !== 'number'
    || !isRecord(value.room)
    || typeof value.room.id !== 'string'
    || typeof value.room.revision !== 'number'
    || typeof value.room.handId !== 'string') {
    throw new Error('Cached River decision state is invalid');
  }
  return value as unknown as CachedDecisionState;
}

export function isRecord(value: unknown): value is Record<string, unknown> {
  return typeof value === 'object' && value !== null && !Array.isArray(value);
}

export function message(error: unknown): string {
  return error instanceof Error ? error.message : String(error);
}

function isRoomSummary(value: unknown): value is RiverRoomSummary {
  if (!isRecord(value)) return false;
  return typeof value.id === 'string'
    && typeof value.name === 'string'
    && typeof value.allowAgents === 'boolean'
    && typeof value.status === 'string'
    && isNumberPair(value.seats)
    && isNumberPair(value.blinds);
}

function isNumberPair(value: unknown): value is [number, number] {
  return Array.isArray(value)
    && value.length === 2
    && typeof value[0] === 'number'
    && typeof value[1] === 'number';
}

function isRiverMe(value: unknown): boolean {
  return isRecord(value)
    && typeof value.name === 'string'
    && (value.id === undefined || typeof value.id === 'string')
    && (value.wallet === undefined || typeof value.wallet === 'number');
}
