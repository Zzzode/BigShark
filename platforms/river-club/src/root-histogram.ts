// RFC 0035 stage 4: exact-ledger demand histogram over flop street-start
// decisions observed in the local sessions/ journals.
//
// This is the "measure before training" instrument. It reads the same JSONL
// journals `bin/bigshark.mjs` writes ({ts, argv, resp}), keeps ONE snapshot
// per hand at the flop street-start boundary, and builds the exact root
// ledger tuple the engine matches through the production mapper
// (`toDecisionRequest`), so a histogram key is the same identity the
// resident's `same_root` compares — never a hand-derived guess.
//
// A snapshot whose chip history cannot be fully reconstructed (the mapper
// throws HISTORY_INCOMPLETE on text-only/amount-missing events) is counted in
// `ledgerIncomplete`, NOT silently bucketed: an inflated or fabricated key
// would poison the pre-declared coverage threshold that decides whether the
// BSFLIB03 second root is trained.
//
// The tool is honest with an empty corpus (the current state): it reports
// zero observations rather than synthesizing a histogram. It establishes the
// instrumentation gate from RFC 0035; it neither trains nor recommends.
import { toDecisionRequest } from './mapper.js';
import { PlayerStatus, type PlayerState } from '../../../build/generated/ts/bigshark/engine/engine_pb.js';
import type { RiverRoom, RiverSeat } from './types.js';

// One journal line as written by journal-cli: a timestamped argv plus the
// command response. Accepted structurally; non-object/non-room entries are
// skipped.
export interface JournalEntry {
  ts?: unknown;
  argv?: unknown;
  resp?: unknown;
  [k: string]: unknown;
}

export type PreflopFamily = 'limped' | 'srp' | '3bet' | '4bet+' | 'unknown';

export interface PositionLedger {
  pos: string;
  seat: number;
  blind: string | null;
  net: number;
  stackBehind: number;
  folded: boolean;
}

// The exact, position-canonical root identity. Position order is SB first,
// then BB, through the button, so the same preflop line reached at rotated
// seats collapses onto one key (per RFC 0035 RootSpec coordinates).
export interface RootLedgerKey {
  playerCount: number;
  bigBlind: number;
  pot: number;
  liveCount: number;
  positions: PositionLedger[];
}

export interface HistogramCell {
  key: RootLedgerKey;
  family: PreflopFamily;
  sbLive: boolean;
  bbLive: boolean;
  count: number;
}

export interface DispersionPoint {
  playerCount: number;
  bigBlind: number;
  startingStackBb: number;
  stackBehindBb: number;
}

export interface RootHistogram {
  journalEntries: number;
  roomSnapshots: number;
  distinctHands: number;
  flopStreetStart: number;
  ledgerMappable: number;
  ledgerIncomplete: number;
  ledgerIncompleteReasons: Record<string, number>;
  exactTuples: HistogramCell[];
  familyCounts: Record<PreflopFamily, number>;
  groupCounts: Record<string, number>;
  stackDispersionBb: {
    starting: number[];
    behind: number[];
  };
  openSizesBb: number[];
}

function isRecord(v: unknown): v is Record<string, unknown> {
  return typeof v === 'object' && v !== null;
}

function asActionableFlopRoom(resp: unknown): RiverRoom | null {
  if (!isRecord(resp)) return null;
  const room = (resp as { room?: unknown }).room ?? resp;
  if (!isRecord(room)) return null;
  if (room.street !== 'flop') return null;
  if (room.next !== undefined && room.next !== 'act') return null;
  if (!isRecord(room.legal)) return null;
  if (typeof room.pot !== 'number' || !Array.isArray(room.seats)) return null;
  return room as unknown as RiverRoom;
}

// A flop street-start snapshot has no flop action after the most recent deal
// event. At a fresh flop the trailing event is the flop deal; any bet/check
// after it means the street is already underway. Conservative by design: a
// truncated window whose boundary cannot be trusted is reported as not
// street-start (it would double-count hands otherwise).
export function isFlopStreetStart(room: RiverRoom): boolean {
  const events = Array.isArray(room.events) ? room.events : [];
  let lastDeal = -1;
  events.forEach((e, i) => {
    if (e?.kind === 'deal') lastDeal = i;
  });
  if (lastDeal < 0) return false;
  for (let i = lastDeal + 1; i < events.length; ++i) {
    const kind = events[i]?.kind;
    if (kind === 'fold' || kind === 'call' || kind === 'check' || kind === 'bet'
        || kind === 'raise' || kind === 'all-in')
      return false;
  }
  return true;
}

// Preflop family from the actions before the last deal. Limped = no raise,
// SRP = exactly one raise, 3bet = two, 4bet+ beyond phase 1. Unknown when the
// preflop segment is absent (sliding window).
export function classifyPreflopFamily(room: RiverRoom): PreflopFamily {
  const events = Array.isArray(room.events) ? room.events : [];
  let lastDeal = -1;
  events.forEach((e, i) => {
    if (e?.kind === 'deal') lastDeal = i;
  });
  if (lastDeal <= 0) return 'unknown';
  const pre = events.slice(0, lastDeal);
  if (!pre.some(e => e?.kind === 'call' || e?.kind === 'raise'
      || e?.kind === 'check' || e?.kind === 'fold'))
    return 'unknown';
  const raises = pre.filter(e => e?.kind === 'raise' || e?.kind === 'bet').length;
  if (raises === 0) return 'limped';
  if (raises === 1) return 'srp';
  if (raises === 2) return '3bet';
  return '4bet+';
}

// Preflop open sizes (chips target) recovered from event text, expressed in
// big blinds. Best-effort clustering input; unparseable lines are dropped.
function openSizesBb(room: RiverRoom, bigBlind: number): number[] {
  const events = Array.isArray(room.events) ? room.events : [];
  let lastDeal = -1;
  events.forEach((e, i) => {
    if (e?.kind === 'deal') lastDeal = i;
  });
  if (lastDeal <= 0 || bigBlind <= 0) return [];
  const out: number[] = [];
  for (const e of events.slice(0, lastDeal)) {
    if (e?.kind !== 'raise' && e?.kind !== 'bet') continue;
    const m = /to\s+(\d+(?:\.\d+)?)/.exec(e.text ?? '');
    if (m) out.push(Number(m[1]) / bigBlind);
  }
  return out;
}

function blindOf(seat: RiverSeat): string | null {
  return typeof seat.blind === 'string' && seat.blind.length > 0 ? seat.blind : null;
}

// SB-first order over the global seat ring (0..capacity-1). Walking the ring
// from the SB seat and including only dealt seats yields SB, BB, …, BTN.
// Prefer the explicit blind labels; otherwise SB is the first dealt seat at
// or after (button + 1) around the ring.
function sbFirstOrder(room: RiverRoom, dealt: RiverSeat[]): RiverSeat[] {
  if (dealt.length === 0) return [];
  const bySeat = new Map<number, RiverSeat>();
  dealt.forEach(s => bySeat.set(s.seat, s));
  const dealtSet = new Set(dealt.map(s => s.seat));
  const capacity = Math.max(...dealt.map(s => s.seat)) + 1;
  const labeledSb = dealt.find(s => blindOf(s) === 'SB')?.seat;
  const button = typeof room.dealer === 'number' ? room.dealer
    : (room.hero?.seat ?? dealt.map(s => s.seat).sort((a, b) => a - b).at(-1)!);
  let sbSeat: number;
  if (labeledSb !== undefined) {
    sbSeat = labeledSb;
  } else {
    sbSeat = -1;
    for (let step = 1; step <= capacity; ++step) {
      const cand = (button + step) % capacity;
      if (dealtSet.has(cand)) { sbSeat = cand; break; }
    }
    if (sbSeat < 0) return dealt;
  }
  const out: RiverSeat[] = [];
  for (let step = 0; step < capacity && out.length < dealt.length; ++step) {
    const cand = (sbSeat + step) % capacity;
    const seat = bySeat.get(cand);
    if (seat) out.push(seat);
  }
  return out;
}

function canonicalKey(key: RootLedgerKey): string {
  return JSON.stringify(key);
}

function median(values: number[]): number | null {
  if (values.length === 0) return null;
  const s = [...values].sort((a, b) => a - b);
  const mid = Math.floor(s.length / 2);
  const center = s[mid]!;
  return s.length % 2 ? center : (center + (s[mid - 1] ?? center)) / 2;
}

// Build one position-canonical ledger key through the production mapper.
// Returns null when the snapshot cannot be mapped (reason recorded by the
// caller).
function ledgerFor(room: RiverRoom): RootLedgerKey | null {
  const req = toDecisionRequest(room);
  const state = req.state;
  if (!state) return null;
  const dealt = (room.seats ?? []).filter((s): s is RiverSeat =>
    typeof s?.seat === 'number');
  const order = sbFirstOrder(room, dealt);
  const bySeat = new Map<number, PlayerState>();
  state.players.forEach(p => bySeat.set(Number(p.seat), p));
  const positions: PositionLedger[] = [];
  for (let i = 0; i < order.length; ++i) {
    const seat = order[i]!;
    const p = bySeat.get(seat.seat);
    if (!p) return null;
    const gross = Number(p.grossContributed ?? 0n);
    const refunded = Number(p.refunded ?? 0n);
    positions.push({
      pos: i === 0 ? 'SB' : i === 1 ? 'BB' : i === order.length - 1 ? 'BTN' : `UTG+${i - 2}`,
      seat: seat.seat,
      blind: blindOf(seat),
      net: gross - refunded,
      stackBehind: Number(p.stack ?? seat.stack ?? 0),
      folded: Number(p.status) === PlayerStatus.FOLDED,
    });
  }
  const liveCount = positions.filter(p => !p.folded).length;
  return {
    playerCount: positions.length,
    bigBlind: Array.isArray(room.blinds) ? Number(room.blinds[1] ?? 0) : 0,
    pot: Number(state.pot?.potTotal ?? room.pot ?? 0),
    liveCount,
    positions,
  };
}

export function emptyHistogram(): RootHistogram {
  return {
    journalEntries: 0,
    roomSnapshots: 0,
    distinctHands: 0,
    flopStreetStart: 0,
    ledgerMappable: 0,
    ledgerIncomplete: 0,
    ledgerIncompleteReasons: {},
    exactTuples: [],
    familyCounts: { limped: 0, srp: 0, '3bet': 0, '4bet+': 0, unknown: 0 },
    groupCounts: {},
    stackDispersionBb: { starting: [], behind: [] },
    openSizesBb: [],
  };
}

// Aggregate parsed journal entries into the RFC 0035 demand histogram.
export function summarizeRootDemand(entries: JournalEntry[]): RootHistogram {
  const out = emptyHistogram();
  out.journalEntries = entries.length;

  // Keep one flop street-start snapshot per hand (first observed), so repeated
  // state polls before the first flop action do not inflate the demand.
  const firstByHand = new Map<string, RiverRoom>();
  for (const entry of entries) {
    const room = asActionableFlopRoom(entry.resp);
    if (!room) continue;
    out.roomSnapshots += 1;
    if (typeof room.handId !== 'string' || room.handId.length === 0) continue;
    if (!isFlopStreetStart(room)) continue;
    out.flopStreetStart += 1;
    if (!firstByHand.has(room.handId)) firstByHand.set(room.handId, room);
  }
  out.distinctHands = firstByHand.size;

  const cells = new Map<string, HistogramCell>();
  for (const [handId, room] of firstByHand) {
    let key: RootLedgerKey | null = null;
    try {
      key = ledgerFor(room);
    } catch (err) {
      const reason = err instanceof Error
        ? (err.message.split(':')[0] ?? 'unknown')
        : 'unknown';
      out.ledgerIncompleteReasons[reason] =
        (out.ledgerIncompleteReasons[reason] ?? 0) + 1;
      out.ledgerIncomplete += 1;
      continue;
    }
    if (!key) {
      out.ledgerIncompleteReasons.noLedger =
        (out.ledgerIncompleteReasons.noLedger ?? 0) + 1;
      out.ledgerIncomplete += 1;
      continue;
    }
    out.ledgerMappable += 1;
    const family = classifyPreflopFamily(room);
    out.familyCounts[family] += 1;
    const sbLive = key.positions.find(p => p.pos === 'SB')?.folded === false;
    const bbLive = key.positions.find(p => p.pos === 'BB')?.folded === false;
    const group = `live=${key.liveCount} sbLive=${sbLive} bbLive=${bbLive}`;
    out.groupCounts[group] = (out.groupCounts[group] ?? 0) + 1;
    const ck = canonicalKey(key);
    const existing = cells.get(ck);
    if (existing) {
      existing.count += 1;
    } else {
      cells.set(ck, { key, family, sbLive, bbLive, count: 1 });
    }
    const bb = key.bigBlind;
    if (bb > 0) {
      for (const p of key.positions) {
        out.stackDispersionBb.behind.push(p.stackBehind / bb);
        const start = p.stackBehind + p.net;
        out.stackDispersionBb.starting.push(start / bb);
      }
      out.openSizesBb.push(...openSizesBb(room, bb));
    }
  }

  out.exactTuples = [...cells.values()].sort((a, b) => b.count - a.count);
  return out;
}

export interface HistogramReport {
  mappable: number;
  distinctExactTuples: number;
  top: Array<{
    share: number;
    count: number;
    family: PreflopFamily;
    liveCount: number;
    pot: number;
    bigBlind: number;
  }>;
  medianStartingStackBb: number | null;
  medianStackBehindBb: number | null;
  medianOpenSizeBb: number | null;
}

// The decision-facing summary the RFC 0035 threshold is read against.
export function histogramReport(h: RootHistogram): HistogramReport {
  return {
    mappable: h.ledgerMappable,
    distinctExactTuples: h.exactTuples.length,
    top: h.exactTuples.slice(0, 20).map(c => ({
      share: h.ledgerMappable > 0 ? c.count / h.ledgerMappable : 0,
      count: c.count,
      family: c.family,
      liveCount: c.key.liveCount,
      pot: c.key.pot,
      bigBlind: c.key.bigBlind,
    })),
    medianStartingStackBb: median(h.stackDispersionBb.starting),
    medianStackBehindBb: median(h.stackDispersionBb.behind),
    medianOpenSizeBb: median(h.openSizesBb),
  };
}
