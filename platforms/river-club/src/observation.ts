import { randomUUID } from 'node:crypto';

import {
  ObservationCoverage,
  ObservationSource,
} from '../../../build/generated/ts/bigshark/engine/engine_pb.js';
import { isRecord } from './json.js';
import { isRiverRoom, type Card, type RiverEvent, type RiverRoom } from './types.js';

/**
 * Honest observation provenance for one hand decision. A plain, fully-
 * populated shape of the wire ObservationProvenance message; the ledger
 * always sets every field.
 */
export interface ObservationProvenance {
  source: ObservationSource;
  coverage: ObservationCoverage;
  controllerEpoch: string;
  observedEventCount: number;
}

// Player-action event kinds that become ActionEvents on the transition path.
// Must stay in sync with the mapper's replay filter: the host validates
// observed_event_count against the replayed transition path length, so the
// ledger counts exactly the transitions the mapper emits.
const ACTION_KINDS = new Set(['fold', 'check', 'call', 'bet', 'raise']);

/**
 * One observed River action event. Event texts are not globally unique
 * actions, so the ledger aligns consecutive event windows by ordered overlap
 * instead of treating a repeated text as either a distinct or a duplicate
 * action.
 */
interface ObservedEvent {
  kind: string;
  text: string;
}

interface HandObservation {
  handId: string;
  /** Revision of the first observation ingested for this hand. */
  firstRevision: number;
  /** Ordered action stream reconstructed from overlapping event windows. */
  events: ObservedEvent[];
  /** Board deals observed as transitions (board growth between snapshots). */
  dealsObserved: number;
  /** Number of board deals visible in the latest snapshot. */
  lastBoardDeals: number;
  /** The first observation saw revision 0 with no prior action events. */
  fromStart: boolean;
  /**
   * Observation continuity is broken (controller restart, or event windows
   * that could not be aligned); coverage can never be FULL for this hand.
   */
  gap: boolean;
}

function toObservedEvent(event: RiverEvent): ObservedEvent {
  return { kind: event.kind, text: event.text };
}

function sameEvent(a: ObservedEvent, b: ObservedEvent): boolean {
  return a.kind === b.kind && a.text === b.text;
}

/**
 * Number of board deals a board of this size represents: one for the flop
 * (3 cards), plus one each for the turn (4) and river (5).
 */
function dealsForBoard(board: readonly Card[] | undefined): number {
  const size = board?.length ?? 0;
  if (size >= 5) return 3;
  if (size >= 4) return 2;
  if (size >= 3) return 1;
  return 0;
}

/**
 * Length of the longest suffix of `prior` that equals a prefix of
 * `incoming`: the overlap between two consecutive sliding event windows.
 */
function longestSuffixOverlap(
  prior: ObservedEvent[],
  incoming: ObservedEvent[],
): number {
  const max = Math.min(prior.length, incoming.length);
  for (let len = max; len > 0; len -= 1) {
    let matches = true;
    for (let i = 0; i < len; i += 1) {
      if (!sameEvent(prior[prior.length - len + i]!, incoming[i]!)) {
        matches = false;
        break;
      }
    }
    if (matches) return len;
  }
  return 0;
}

/**
 * Per-hand observation ledger fed by every official CLI response.
 *
 * The ledger never asserts completeness it cannot prove: FULL coverage
 * requires an observation at the hand's first revision with no prior action
 * events, followed by continuous, alignable event windows. A mid-hand
 * attach, a controller restart, or unalignable windows all downgrade the
 * hand to MID_HAND_ATTACH.
 *
 * observedEventCount counts transition events observed: player actions
 * (aligned from the sliding event windows) plus board deals seen as board
 * growth between snapshots. This matches the transition path the mapper
 * builds from the same observations, which the host validates by replay.
 */
export class ObservationLedger {
  private hand: HandObservation | null = null;
  private restartCount = 0;
  private readonly epochBase: string;

  constructor(epochBase?: string) {
    this.epochBase = epochBase ?? randomUUID();
  }

  /**
   * Ingest one parsed official CLI response before any caller early return,
   * heartbeat, wait, or changed-hand skip. Responses without a full room
   * (errors, heartbeats, room references, room lists) carry no per-hand
   * observation and are ignored without creating a gap.
   */
  ingest(response: unknown): void {
    if (!isRecord(response)) return;
    const room = response.room;
    if (!isRiverRoom(room)) return;
    this.observeRoom(room);
  }

  private observeRoom(room: RiverRoom): void {
    const incoming = (room.events ?? [])
      .filter(event => ACTION_KINDS.has(event.kind))
      .map(toObservedEvent);
    const boardDeals = dealsForBoard(room.board);
    if (this.hand === null || this.hand.handId !== room.handId) {
      // New hand: the first observation's revision and event set decide
      // whether observation started at the hand beginning. Board deals
      // visible at attach time were not observed as transitions.
      this.hand = {
        handId: room.handId,
        firstRevision: room.revision,
        events: incoming,
        dealsObserved: 0,
        lastBoardDeals: boardDeals,
        fromStart: room.revision === 0 && incoming.length === 0,
        gap: false,
      };
      return;
    }
    // Board growth between snapshots is an observed deal transition.
    if (boardDeals > this.hand.lastBoardDeals) {
      this.hand.dealsObserved += boardDeals - this.hand.lastBoardDeals;
      this.hand.lastBoardDeals = boardDeals;
    }
    if (incoming.length === 0) return;
    const prior = this.hand.events;
    if (prior.length === 0) {
      this.hand.events = incoming;
      return;
    }
    const overlap = longestSuffixOverlap(prior, incoming);
    if (overlap === 0) {
      // The windows share no events: the window turned over or events were
      // lost or reordered. Continuity is unprovable, so keep both windows
      // and downgrade coverage rather than silently dropping either side.
      this.hand.gap = true;
      this.hand.events = prior.concat(incoming);
      return;
    }
    // A duplicate or unchanged window (overlap === incoming.length) appends
    // nothing, so the observed count stays honest.
    const tail = incoming.slice(overlap);
    if (tail.length > 0) this.hand.events = prior.concat(tail);
  }

  /**
   * Invalidate the observation epoch on controller restart. Pre-restart
   * observations belong to the old epoch and are dropped; the next
   * observation starts a fresh, honestly-scoped ledger under a new epoch.
   */
  markRestart(): void {
    this.restartCount += 1;
    this.hand = null;
  }

  /**
   * Honest provenance for `handId` (defaults to the ledger's current hand).
   * Fails closed to MID_HAND_ATTACH with a zero count when the hand was not
   * observed from its beginning or was never observed at all; the ledger
   * never fabricates FULL coverage.
   */
  provenance(handId?: string): ObservationProvenance {
    const target = handId ?? this.hand?.handId ?? 'unobserved';
    if (this.hand === null || this.hand.handId !== target) {
      return {
        source: ObservationSource.OFFICIAL_RESPONSE,
        coverage: ObservationCoverage.MID_HAND_ATTACH,
        controllerEpoch: this.epoch(target, true),
        observedEventCount: 0,
      };
    }
    return {
      source: ObservationSource.OFFICIAL_RESPONSE,
      coverage: this.hand.fromStart && !this.hand.gap
        ? ObservationCoverage.FULL
        : ObservationCoverage.MID_HAND_ATTACH,
      controllerEpoch: this.epoch(target, false),
      observedEventCount: this.hand.events.length + this.hand.dealsObserved,
    };
  }

  // The wire field is capped at 128 characters; the epoch is opaque and only
  // compared for equality, so a stable prefix truncation preserves per-hand
  // identity without risking a protocol rejection.
  private epoch(handId: string, unobserved: boolean): string {
    const epoch = `${this.epochBase}:${this.restartCount}:${handId}`
      + (unobserved ? ':unobserved' : '');
    return epoch.length <= 128 ? epoch : epoch.slice(0, 128);
  }
}

/**
 * Process-wide observation ledger. The River runner ingests every official
 * CLI response into this singleton; the mapper reads honest provenance from
 * it for the hand it maps. Tests construct isolated ObservationLedger
 * instances and never touch this singleton.
 */
export const observationLedger = new ObservationLedger();
