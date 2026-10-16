import { create, type MessageInitShape } from '@bufbuild/protobuf';

import {
  ActionEventSchema,
  ActionType,
  BoardDealSchema,
  CardSchema,
  DecisionRequestSchema,
  EnvelopeSchema,
  ErrorCode,
  ForcedContributionSchema,
  ForcedContributionType,
  GameType,
  GameVariant,
  BettingStructure,
  GuaranteeLevel,
  LayerStatus,
  LegalActionSchema,
  PlayerStatus,
  PlayerStateSchema,
  PotLayerSchema,
  PotPhase,
  PotStateSchema,
  Rank,
  ReopeningRule,
  RunoutRule,
  SettlementRule,
  SolverMode,
  SolverSource,
  Street,
  Suit,
  TransitionEventSchema,
  UtilityProfile,
  type DecisionRequest,
  type Envelope,
  type ExpandedStrategy,
  type Strategy,
} from '../../../build/generated/ts/bigshark/engine/v2/engine_pb.js';
import type {
  Action,
  ExecutableDecision,
  GuaranteeLevel as GuaranteeLevelToken,
  RawEngineDecision,
  RiverEvent,
  RiverRoom,
  RiverSeat,
} from './types.js';
import { observationLedger, type ObservationProvenance } from './observation.js';

// ---------------------------------------------------------------------------
// Constants and helpers
// ---------------------------------------------------------------------------

const PLAYER_ACTION_KINDS = new Set<Action>(['fold', 'check', 'call', 'bet', 'raise']);

const MAX_SAFE_CHIP = BigInt(Number.MAX_SAFE_INTEGER);

const RANK_BY_CHAR: Record<string, Rank> = {
  '2': Rank.TWO,
  '3': Rank.THREE,
  '4': Rank.FOUR,
  '5': Rank.FIVE,
  '6': Rank.SIX,
  '7': Rank.SEVEN,
  '8': Rank.EIGHT,
  '9': Rank.NINE,
  T: Rank.TEN,
  J: Rank.JACK,
  Q: Rank.QUEEN,
  K: Rank.KING,
  A: Rank.ACE,
};

const SUIT_BY_WORD: Record<string, Suit> = {
  spades: Suit.SPADES,
  hearts: Suit.HEARTS,
  diamonds: Suit.DIAMONDS,
  clubs: Suit.CLUBS,
};

const STREET_BY_NAME: Record<RiverRoom['street'], Street | null> = {
  preflop: Street.PREFLOP,
  flop: Street.FLOP,
  turn: Street.TURN,
  river: Street.RIVER,
  showdown: null,
};

const ACTION_BY_NAME: Record<Action, ActionType> = {
  fold: ActionType.FOLD,
  check: ActionType.CHECK,
  call: ActionType.CALL,
  bet: ActionType.BET,
  raise: ActionType.RAISE,
};

const ACTION_NAME_BY_KIND: Partial<Record<ActionType, Action>> = {
  [ActionType.FOLD]: 'fold',
  [ActionType.CHECK]: 'check',
  [ActionType.CALL]: 'call',
  [ActionType.BET]: 'bet',
  [ActionType.RAISE]: 'raise',
};

const GUARANTEE_LEVEL_BY_TOKEN: Record<GuaranteeLevelToken, GuaranteeLevel> = {
  operational_fallback: GuaranteeLevel.OPERATIONAL_FALLBACK,
  approximate: GuaranteeLevel.APPROXIMATE,
  abstract_solved: GuaranteeLevel.ABSTRACT_SOLVED,
  exact_solved: GuaranteeLevel.EXACT_SOLVED,
  certified_bound: GuaranteeLevel.CERTIFIED_BOUND,
};

function toChip(value: number | undefined, field: string): bigint {
  if (typeof value !== 'number' || !Number.isInteger(value) || value < 0) {
    throw new Error(`${field} must be a non-negative integer chip amount`);
  }
  const amount = BigInt(value);
  if (amount > MAX_SAFE_CHIP) {
    throw new Error(`${field} exceeds Number.MAX_SAFE_INTEGER; refusing to truncate`);
  }
  return amount;
}

function chipToNumber(value: bigint | undefined, field: string): number | undefined {
  if (value === undefined) return undefined;
  if (value < 0n || value > MAX_SAFE_CHIP) {
    throw new Error(`${field} is outside the safe integer chip range`);
  }
  return Number(value);
}

function mapCard(rank: string, suit: string): MessageInitShape<typeof CardSchema> {
  const rankEnum = RANK_BY_CHAR[rank];
  const suitEnum = SUIT_BY_WORD[suit];
  if (rankEnum === undefined || suitEnum === undefined) {
    throw new Error(`unmappable card ${rank}${suit}`);
  }
  return { rank: rankEnum, suit: suitEnum };
}

function dealtIn(seat: RiverSeat): boolean {
  if (seat.status === 'active' || seat.status === 'all-in' || seat.status === 'folded')
    return true;
  return Boolean(seat.blind) || (seat.bet ?? 0) > 0 || Boolean(seat.position);
}

function effectiveStackHint(room: RiverRoom): number {
  const raw = room.solver?.effectiveStackBb ?? room.hero?.effectiveStackBb;
  return typeof raw === 'number' && Number.isFinite(raw) && raw > 0 ? raw : 100;
}

// ---------------------------------------------------------------------------
// Shared operational helpers
// ---------------------------------------------------------------------------

/** Legal operational fallback when the engine is unavailable. */
export function safeFallback(room?: RiverRoom | null): ExecutableDecision {
  const level = { guaranteeLevel: 'operational_fallback' } as const;
  const legal = room?.legal;
  if (!legal) return { action: 'fold', amount: 0, reason: 'safe-fallback:no-legal', ...level };
  const actions = legal.actions || [];
  if (actions.includes('check')) {
    return { action: 'check', amount: 0, reason: 'safe-fallback:check', ...level };
  }
  if (actions.includes('call')) {
    return { action: 'call', amount: 0, reason: 'safe-fallback:call', ...level };
  }
  return { action: 'fold', amount: 0, reason: 'safe-fallback:fold', ...level };
}

/** Validate an engine-produced decision against the room's legal actions. */
export function validateEngineDecision(
  room: RiverRoom,
  decision: RawEngineDecision | null,
): ExecutableDecision | null {
  if (!decision || !room?.legal?.actions?.includes(decision.action)) return null;
  if (decision.action === 'bet' || decision.action === 'raise') {
    const range = room.legal.raiseTo;
    const amount = decision.amount;
    if (!range
      || !Number.isInteger(amount)
      || amount === undefined
      || amount < range.min
      || amount > range.max) {
      return null;
    }
  }
  return {
    action: decision.action,
    ...(decision.amount ? { amount: decision.amount } : {}),
    reason: `cpp:${decision.reason}`,
  };
}

/** Extract the trailing integer token from event text, or null if not exact. */
export function parseExactChipAmount(text: string): bigint | null {
  const token = (text || '').trim().split(/\s+/).at(-1) ?? '';
  if (!/^[0-9]+$/.test(token))
    return null;
  const value = Number(token);
  if (!Number.isSafeInteger(value) || value <= 0)
    return null;
  return BigInt(value);
}

/** Clamp a caller-supplied solve budget into the wire's 1..120000 ms range. */
export function resolveSolveTimeBudgetMs(value: number | undefined): number {
  if (value === undefined || !Number.isFinite(value) || !Number.isInteger(value))
    return 2000;
  return Math.min(120_000, Math.max(1, value));
}

// ---------------------------------------------------------------------------
// Action replay (returns seat numbers instead of
// string player IDs)
// ---------------------------------------------------------------------------

type RoundCode = 'x' | 'c' | 'b' | 'r' | 'f';

interface ReplayedEvent {
  event: RiverEvent;
  code: RoundCode;
  actorName: string;
  group: number;
}

function replayActionRounds(events: RiverEvent[]): ReplayedEvent[] {
  const replayed: ReplayedEvent[] = [];
  let group = 0;
  let raised = false;
  let passive = 0;
  let handOver = false;
  for (const event of events) {
    if (!PLAYER_ACTION_KINDS.has(event.kind as Action)) continue;
    const code: RoundCode = event.kind === 'check'
      ? 'x'
      : event.kind[0] as RoundCode;
    replayed.push({ event, code, actorName: event.text || '', group });
    if (handOver) continue;
    if (code === 'f') { handOver = true; continue; }
    if (code === 'b' || code === 'r') { raised = true; passive = 0; continue; }
    if (code === 'c' && (raised || group > 0)) {
      group += 1; raised = false; passive = 0; continue;
    }
    passive += 1;
    if (passive >= 2) { group += 1; raised = false; passive = 0; }
  }
  return replayed;
}

function streetForGroup(roundGroup: number, current: Street): Street {
  const derived = roundGroup <= 0
    ? Street.PREFLOP
    : roundGroup === 1
      ? Street.FLOP
      : roundGroup === 2
        ? Street.TURN
        : Street.RIVER;
  return derived > current ? current : derived;
}

// ---------------------------------------------------------------------------
// Structured action with recovered amounts
// ---------------------------------------------------------------------------

interface StructuredAction {
  kind: Action;
  seat: number;
  street: Street;
  group: number;
  /** Exact target total for bet/raise, or incremental for call. */
  amount: bigint | null;
  /** Whether this is the actor's last chip action of the current street. */
  isLastCurrentStreetChip: boolean;
  /** The raw event text for amount parsing. */
  eventText: string;
}

/**
 * Resolves an event text to a seat number by display-name prefix matching.
 * Hero is checked first, then the longest opponent name. Unmatched actors
 * fall back to a deterministic opponent seat.
 */
function resolveActorSeat(
  eventText: string,
  room: RiverRoom,
  seats: RiverSeat[],
  dealtSeats: RiverSeat[],
): number {
  const text = eventText || '';
  const heroSeat = seats.find(s => s.seat === room.hero.seat);
  if (heroSeat?.name && text.startsWith(heroSeat.name))
    return room.hero.seat;
  let best: RiverSeat | undefined;
  for (const candidate of seats) {
    if (candidate.seat === room.hero.seat) continue;
    if (candidate.name && text.startsWith(candidate.name)
        && (!best?.name || candidate.name.length > best.name.length))
      best = candidate;
  }
  if (best) return best.seat;
  const opponent = dealtSeats.find(
    s => s.seat !== room.hero.seat && dealtIn(s),
  );
  return opponent ? opponent.seat : room.hero.seat;
}

/**
 * Builds structured actions from the River event history, recovering exact
 * chip amounts from event text and current-street seat commitments.
 */
function buildStructuredActions(
  room: RiverRoom,
  street: Street,
  seats: RiverSeat[],
  dealtSeats: RiverSeat[],
  blinds: [number, number],
): StructuredAction[] {
  const replayed = replayActionRounds(room.events ?? []);

  // Current-street exact commitments from live seat.bet.
  const streetCommitment = new Map<number, bigint>();
  for (const seat of seats) {
    streetCommitment.set(seat.seat, toChip(seat.bet ?? 0, `seat ${seat.seat} bet`));
  }

  // Pre-compute whether each replayed chip event is the actor's LAST chip
  // action of the current decision street.
  const isLastCurrentStreetChip = replayed.map((entry, index) => {
    const wireStreet = streetForGroup(entry.group, street);
    if (wireStreet !== street || !['b', 'r', 'c'].includes(entry.code))
      return false;
    const actorSeat = resolveActorSeat(entry.actorName, room, seats, dealtSeats);
    for (let later = index + 1; later < replayed.length; later += 1) {
      const other = replayed[later]!;
      if (streetForGroup(other.group, street) !== wireStreet) continue;
      if (!['b', 'r', 'c'].includes(other.code)) continue;
      if (resolveActorSeat(other.actorName, room, seats, dealtSeats) === actorSeat)
        return false;
    }
    return true;
  });

  // Per-(street, seat) street-paid ledger, seeded with blinds for preflop.
  const streetPaid = new Map<string, bigint>();
  const paidKey = (wireStreet: Street, seat: number) => `${wireStreet}:${seat}`;
  for (const seat of seats) {
    if (seat.blind !== 'SB' && seat.blind !== 'BB') continue;
    const blindAmount = seat.blind === 'BB' ? blinds[1] : blinds[0];
    streetPaid.set(
      paidKey(Street.PREFLOP, seat.seat),
      toChip(blindAmount, 'blind'),
    );
  }

  const ledgerIncomplete = new Set<string>();

  return replayed.map((entry, index) => {
    const kind: Action = entry.code === 'x'
      ? 'check'
      : entry.code === 'c'
        ? 'call'
        : entry.code === 'b'
          ? 'bet'
          : entry.code === 'r'
            ? 'raise'
            : 'fold';
    const seat = resolveActorSeat(entry.actorName, room, seats, dealtSeats);
    const wireStreet = streetForGroup(entry.group, street);

    let amount: bigint | null = null;
    if (kind === 'bet' || kind === 'raise' || kind === 'call') {
      const exact = parseExactChipAmount(entry.event.text);
      const ledgerKey = paidKey(wireStreet, seat);
      const prior = streetPaid.get(ledgerKey) ?? 0n;
      if (exact !== null) {
        if (kind === 'call') {
          amount = exact;
        } else {
          amount = exact;
        }
      } else if (wireStreet === street && isLastCurrentStreetChip[index]
                 && !ledgerIncomplete.has(ledgerKey)) {
        const finalCommit = streetCommitment.get(seat);
        if (finalCommit !== undefined && finalCommit > 0n && finalCommit >= prior) {
          if (kind === 'call') {
            amount = finalCommit - prior;
          } else {
            amount = finalCommit;
          }
        }
      }
      if (amount !== null && amount > 0n) {
        if (kind === 'call') {
          streetPaid.set(ledgerKey, prior + amount);
        } else {
          streetPaid.set(ledgerKey, amount);
        }
      } else {
        ledgerIncomplete.add(ledgerKey);
      }
    }

    return {
      kind,
      seat,
      street: wireStreet,
      group: entry.group,
      amount,
      isLastCurrentStreetChip: isLastCurrentStreetChip[index] ?? false,
      eventText: entry.event.text,
    };
  });
}

// ---------------------------------------------------------------------------
// G/F ledger reconstruction
// ---------------------------------------------------------------------------

interface GfEntry {
  gross: bigint;
  refunded: bigint;
  starting: bigint;
}

/**
 * Reconstructs the G/F (gross contributed / refunded) ledger for each dealt
 * player from the structured action history.
 *
 * G = blinds + sum of all chip-moving actions across all streets.
 * F = cumulative uncalled refunds, detected at street boundaries when the
 *     highest street commitment exceeds the second-highest.
 *
 * Throws MappingError with HISTORY_INCOMPLETE if any chip action amount
 * cannot be recovered, since the engine replays from hand start.
 */
function reconstructGfLedger(
  room: RiverRoom,
  actions: StructuredAction[],
  dealtSeats: RiverSeat[],
  blinds: [number, number],
): Map<number, GfEntry> {
  const gross = new Map<number, bigint>();
  const refunded = new Map<number, bigint>();
  const streetCommitted = new Map<string, bigint>();

  for (const seat of dealtSeats) {
    gross.set(seat.seat, 0n);
    refunded.set(seat.seat, 0n);
  }

  // Seed blinds.
  for (const seat of dealtSeats) {
    if (seat.blind === 'SB') {
      gross.set(seat.seat, (gross.get(seat.seat) ?? 0n) + toChip(blinds[0], 'SB'));
      streetCommitted.set(`${Street.PREFLOP}:${seat.seat}`, toChip(blinds[0], 'SB'));
    } else if (seat.blind === 'BB') {
      gross.set(seat.seat, (gross.get(seat.seat) ?? 0n) + toChip(blinds[1], 'BB'));
      streetCommitted.set(`${Street.PREFLOP}:${seat.seat}`, toChip(blinds[1], 'BB'));
    }
  }

  // Track per-street cumulative commitments for refund detection.
  // We process actions in order, tracking the current street.
  let currentStreet = Street.PREFLOP;

  for (const action of actions) {
    if (action.street !== currentStreet) {
      // Street boundary: detect refunds for the closing street.
      detectRefunds(currentStreet, streetCommitted, refunded);
      currentStreet = action.street;
    }

    if (action.kind === 'fold' || action.kind === 'check') continue;
    if (action.amount === null) {
      throw new MappingError(
        `HISTORY_INCOMPLETE: missing chip amount for ${action.kind} by seat ${action.seat} on ${Street[action.street]}`,
        ErrorCode.HISTORY_INCOMPLETE,
      );
    }

    const key = `${action.street}:${action.seat}`;
    const prior = streetCommitted.get(key) ?? 0n;

    if (action.kind === 'call') {
      gross.set(action.seat, (gross.get(action.seat) ?? 0n) + action.amount);
      streetCommitted.set(key, prior + action.amount);
    } else {
      // bet or raise: amount is the target total.
      const incremental = action.amount - prior;
      if (incremental < 0n) {
        throw new MappingError(
          `REPLAY_MISMATCH: ${action.kind} target ${action.amount} < prior commitment ${prior} for seat ${action.seat}`,
          ErrorCode.REPLAY_MISMATCH,
        );
      }
      gross.set(action.seat, (gross.get(action.seat) ?? 0n) + incremental);
      streetCommitted.set(key, action.amount);
    }
  }

  // Detect refunds for the last closed street. When the room's current street
  // is later than the last action's street (or the hand reached showdown),
  // the last action's street is closed and its refunds must be recorded.
  // The current (open) street's refund is applied by the engine at close.
  const roomStreet = STREET_BY_NAME[room.street];
  if (roomStreet === null || currentStreet !== roomStreet) {
    detectRefunds(currentStreet, streetCommitted, refunded);
  }

  // Validate the invariant: starting_stack = stack + G - F.
  const result = new Map<number, GfEntry>();
  for (const seat of dealtSeats) {
    const g = gross.get(seat.seat) ?? 0n;
    const f = refunded.get(seat.seat) ?? 0n;
    const stack = toChip(seat.stack, `seat ${seat.seat} stack`);
    const starting = stack + g - f;
    if (starting < 0n) {
      throw new MappingError(
        `REPLAY_MISMATCH: negative starting stack for seat ${seat.seat}: stack=${stack} G=${g} F=${f}`,
        ErrorCode.REPLAY_MISMATCH,
      );
    }
    result.set(seat.seat, { gross: g, refunded: f, starting });
  }
  return result;
}

/**
 * Detects refunds at a street boundary. When the highest street commitment
 * exceeds the second-highest, the excess is refunded to the highest player.
 * Uses streetCommitted directly so blind posters are included even if they
 * have not acted yet.
 */
function detectRefunds(
  street: Street,
  streetCommitted: Map<string, bigint>,
  refunded: Map<number, bigint>,
): void {
  const prefix = `${street}:`;
  const commitments: Array<{ seat: number; amount: bigint }> = [];
  for (const [key, amount] of streetCommitted) {
    if (!key.startsWith(prefix)) continue;
    if (amount <= 0n) continue;
    const seat = Number(key.slice(prefix.length));
    commitments.push({ seat, amount });
  }
  if (commitments.length < 2) return;

  // Sort descending by amount.
  commitments.sort((a, b) => {
    if (a.amount > b.amount) return -1;
    if (a.amount < b.amount) return 1;
    return a.seat - b.seat;
  });

  const highest = commitments[0]!;
  const second = commitments[1]!;
  if (highest.amount > second.amount) {
    const refund = highest.amount - second.amount;
    refunded.set(highest.seat, (refunded.get(highest.seat) ?? 0n) + refund);
  }
}

// ---------------------------------------------------------------------------
// Pot layer construction
// ---------------------------------------------------------------------------

/**
 * Builds phase-tagged pot layers from the River room's pot partition or from
 * per-player net contributions.
 */
function buildPotLayers(
  room: RiverRoom,
  gfLedger: Map<number, GfEntry>,
  dealtSeats: RiverSeat[],
): MessageInitShape<typeof PotLayerSchema>[] {
  const net = new Map<number, bigint>();
  for (const seat of dealtSeats) {
    const entry = gfLedger.get(seat.seat);
    if (entry) {
      net.set(seat.seat, entry.gross - entry.refunded);
    }
  }

  const riverPots = room.pots ?? [];
  if (riverPots.length > 0) {
    // Use the platform's pot partition directly.
    return riverPots.map(pot => {
      const eligibleDealt = pot.eligible.filter(s => net.has(s));
      // Contribution level = min net contribution among eligible players.
      let level = 0n;
      for (const seat of eligibleDealt) {
        const n = net.get(seat) ?? 0n;
        if (level === 0n || n < level) level = n;
      }
      return {
        contributionLevel: level,
        amount: toChip(pot.size, 'pot layer'),
        eligibleSeats: eligibleDealt,
        status: LayerStatus.PENDING,
      };
    });
  }

  // Build layers from per-player net contributions.
  const levels = new Set<bigint>();
  for (const n of net.values()) {
    if (n > 0n) levels.add(n);
  }
  const sortedLevels = [...levels].sort((a, b) => a < b ? -1 : a > b ? 1 : 0);

  const layers: MessageInitShape<typeof PotLayerSchema>[] = [];
  let prevLevel = 0n;
  for (const level of sortedLevels) {
    const contributors = [...net.entries()]
      .filter(([, n]) => n >= level)
      .map(([seat]) => seat);
    const amount = (level - prevLevel) * BigInt(contributors.length);
    layers.push({
      contributionLevel: level,
      amount,
      eligibleSeats: contributors,
      status: LayerStatus.PENDING,
    });
    prevLevel = level;
  }
  return layers;
}

// ---------------------------------------------------------------------------
// TransitionEvent path building
// ---------------------------------------------------------------------------

/**
 * Builds the ordered transition path with explicit BoardDeal events at street
 * boundaries. Also computes potBefore and stackAfter for each ActionEvent by
 * replaying chip movement from the starting stacks.
 */
function buildTransitionPath(
  room: RiverRoom,
  actions: StructuredAction[],
  gfLedger: Map<number, GfEntry>,
  dealtSeats: RiverSeat[],
  blinds: [number, number],
  street: Street,
): MessageInitShape<typeof TransitionEventSchema>[] {
  // Initialize stacks from starting stacks.
  const stacks = new Map<number, bigint>();
  for (const seat of dealtSeats) {
    const entry = gfLedger.get(seat.seat);
    if (entry) stacks.set(seat.seat, entry.starting);
  }

  // Apply blinds.
  let pot = 0n;
  for (const seat of dealtSeats) {
    if (seat.blind === 'SB') {
      const amount = toChip(blinds[0], 'SB');
      pot += amount;
      stacks.set(seat.seat, (stacks.get(seat.seat) ?? 0n) - amount);
    } else if (seat.blind === 'BB') {
      const amount = toChip(blinds[1], 'BB');
      pot += amount;
      stacks.set(seat.seat, (stacks.get(seat.seat) ?? 0n) - amount);
    }
  }

  // Track per-street cumulative commitments for refund detection.
  const streetCommitted = new Map<string, bigint>();
  for (const seat of dealtSeats) {
    if (seat.blind === 'SB') {
      streetCommitted.set(`${Street.PREFLOP}:${seat.seat}`, toChip(blinds[0], 'SB'));
    } else if (seat.blind === 'BB') {
      streetCommitted.set(`${Street.PREFLOP}:${seat.seat}`, toChip(blinds[1], 'BB'));
    }
  }

  const path: MessageInitShape<typeof TransitionEventSchema>[] = [];
  let sequence = 0;
  let currentStreet = Street.PREFLOP;

  const boardCardsForStreet = (s: Street): MessageInitShape<typeof CardSchema>[] => {
    const board = room.board ?? [];
    if (s === Street.FLOP) {
      return board.slice(0, 3).map(c => mapCard(c.rank, c.suit));
    }
    if (s === Street.TURN) {
      return board.slice(3, 4).map(c => mapCard(c.rank, c.suit));
    }
    if (s === Street.RIVER) {
      return board.slice(4, 5).map(c => mapCard(c.rank, c.suit));
    }
    return [];
  };

  for (const action of actions) {
    // Insert BoardDeal at street boundary.
    if (action.street !== currentStreet) {
      // Note: we don't adjust pot/stacks for refunds here because the
      // platform already reflects post-refund state. The potBefore/stackAfter
      // values are informational; the engine validates G/F, not these fields.

      currentStreet = action.street;

      const dealCards = boardCardsForStreet(currentStreet);
      if (dealCards.length > 0) {
        path.push({
          sequence: BigInt(sequence++),
          transition: {
            case: 'boardDeal',
            value: {
              street: currentStreet,
              cards: dealCards,
            },
          },
        });
      }
    }

    const potBefore = pot;
    const key = `${action.street}:${action.seat}`;
    const prior = streetCommitted.get(key) ?? 0n;

    let chipDelta = 0n;
    if (action.kind === 'call' && action.amount !== null) {
      chipDelta = action.amount;
      streetCommitted.set(key, prior + chipDelta);
    } else if ((action.kind === 'bet' || action.kind === 'raise') && action.amount !== null) {
      chipDelta = action.amount - prior;
      streetCommitted.set(key, action.amount);
    }

    // Cap at player's remaining stack (all-in protection).
    const playerStack = stacks.get(action.seat) ?? 0n;
    if (chipDelta > playerStack) {
      chipDelta = playerStack;
    }

    pot += chipDelta;
    const newStack = playerStack - chipDelta;
    stacks.set(action.seat, newStack);

    const allIn = newStack === 0n && chipDelta > 0n;

    const actionEvent: MessageInitShape<typeof ActionEventSchema> = {
      sequence: BigInt(sequence),
      street: action.street,
      actorSeat: action.seat,
      action: ACTION_BY_NAME[action.kind],
      potBefore,
      stackAfter: newStack,
      allIn,
    };

    if (action.kind === 'call' && action.amount !== null) {
      actionEvent.incrementalAmount = action.amount;
    } else if ((action.kind === 'bet' || action.kind === 'raise') && action.amount !== null) {
      actionEvent.targetTotal = action.amount;
    }

    path.push({
      sequence: BigInt(sequence++),
      transition: {
        case: 'action',
        value: actionEvent,
      },
    });
  }

  // Ensure BoardDeal events exist for all dealt board cards, even when
  // there are no actions on a street. The action loop inserts them at
  // street boundaries; this fills gaps for streets with no actions.
  if (street !== Street.PREFLOP) {
    const dealtStreets = new Set<Street>();
    for (const event of path) {
      if (event.transition?.case === 'boardDeal'
        && event.transition.value?.street !== undefined) {
        dealtStreets.add(event.transition.value.street);
      }
    }
    for (const s of [Street.FLOP, Street.TURN, Street.RIVER]) {
      if (s > street) break;
      if (!dealtStreets.has(s)) {
        const cards = boardCardsForStreet(s);
        if (cards.length > 0) {
          path.push({
            sequence: BigInt(sequence++),
            transition: {
              case: 'boardDeal',
              value: { street: s, cards },
            },
          });
        }
      }
    }
  }

  return path;
}

// ---------------------------------------------------------------------------
// Error class
// ---------------------------------------------------------------------------

export class EngineError extends Error {
  constructor(
    readonly codeName: string,
    readonly code: ErrorCode,
    readonly retryable: boolean,
  ) {
    super(`engine error ${codeName}`);
    this.name = 'EngineError';
  }
}

export class MappingError extends Error {
  constructor(
    message: string,
    readonly code: ErrorCode,
  ) {
    super(message);
    this.name = 'MappingError';
  }
}

// ---------------------------------------------------------------------------
// toDecisionRequest
// ---------------------------------------------------------------------------

export interface MapperConfig {
  style?: string;
  heroName?: string;
  solverMode?: SolverMode;
  minimumGuaranteeLevel?: GuaranteeLevelToken;
  solveTimeBudgetMs?: number;
  /**
   * Honest observation provenance for the hand. When omitted, the mapper
   * reads the process observation ledger for this hand; with no ledger
   * observation it fails closed to MID_HAND_ATTACH and never fabricates
   * FULL coverage.
   */
  provenance?: ObservationProvenance;
}

/**
 * Builds a DecisionRequest from a RiverRoom snapshot. Throws on any
 * unmappable data, missing chip amounts, or G/F invariant violation.
 */
export function toDecisionRequest(
  room: RiverRoom,
  config: MapperConfig = {},
): DecisionRequest {
  if (!room.legal) throw new Error('legal actions are required');
  const street = STREET_BY_NAME[room.street];
  if (street === null) throw new Error(`cannot decide on street ${room.street}`);

  const blinds = room.blinds ?? [10, 20];
  const seats = room.seats.filter((s): s is RiverSeat => s !== null);
  const tableCapacity = seats.reduce(
    (cap, seat) => Math.max(cap, seat.seat + 1),
    2,
  );
  const dealtSeats = seats.filter(dealtIn);

  // Build structured actions with recovered amounts.
  const actions = buildStructuredActions(room, street, seats, dealtSeats, blinds);

  // Reconstruct G/F ledger.
  const gfLedger = reconstructGfLedger(room, actions, dealtSeats, blinds);

  // Build player states.
  const players: MessageInitShape<typeof PlayerStateSchema>[] = dealtSeats.map(seat => {
    const gf = gfLedger.get(seat.seat);
    if (!gf) {
      throw new MappingError(
        `REPLAY_MISMATCH: no G/F entry for seat ${seat.seat}`,
        ErrorCode.REPLAY_MISMATCH,
      );
    }
    return {
      seat: seat.seat,
      ...(seat.name ? { displayName: seat.name } : {}),
      startingStack: gf.starting,
      stack: toChip(seat.stack, `seat ${seat.seat} stack`),
      streetCommitted: toChip(seat.bet ?? 0, `seat ${seat.seat} bet`),
      grossContributed: gf.gross,
      refunded: gf.refunded,
      status:
        seat.status === 'folded' || seat.status === 'leaving' || seat.status === 'waiting'
          ? PlayerStatus.FOLDED
          : seat.status === 'all-in'
            ? PlayerStatus.ALL_IN
            : PlayerStatus.ACTIVE,
    };
  });

  // Build forced contributions.
  const forced: MessageInitShape<typeof ForcedContributionSchema>[] = dealtSeats
    .filter(seat => seat.blind !== undefined)
    .flatMap((seat): MessageInitShape<typeof ForcedContributionSchema>[] => {
      if (seat.blind === 'SB') {
        return [{
          seat: seat.seat,
          type: ForcedContributionType.SMALL_BLIND,
          amount: toChip(blinds[0], 'small blind'),
        }];
      }
      if (seat.blind === 'BB') {
        return [{
          seat: seat.seat,
          type: ForcedContributionType.BIG_BLIND,
          amount: toChip(blinds[1], 'big blind'),
        }];
      }
      // Straddle and other forced contributions map to ANTE.
      return [{
        seat: seat.seat,
        type: ForcedContributionType.ANTE,
        amount: toChip(seat.bet ?? 0, 'straddle'),
      }];
    });

  // Build pot layers.
  const potLayers = buildPotLayers(room, gfLedger, dealtSeats);

  // Build transition path.
  const transitionPath = buildTransitionPath(
    room, actions, gfLedger, dealtSeats, blinds, street,
  );

  // Build legal actions. Cap maxTargetTotal at the hero's stack +
  // street_committed: the engine validates this bound and rejects a
  // platform-reported maximum that exceeds what the hero can wager.
  const heroPlayer = players.find(p => p.seat === room.hero.seat);
  const heroMaxTotal = heroPlayer
    ? (heroPlayer.stack as bigint) + (heroPlayer.streetCommitted as bigint)
    : null;
  const legalActions: MessageInitShape<typeof LegalActionSchema>[] =
    room.legal.actions.map(action => {
      const legal: MessageInitShape<typeof LegalActionSchema> = {
        type: ACTION_BY_NAME[action],
      };
      if ((action === 'bet' || action === 'raise') && room.legal?.raiseTo) {
        legal.minTargetTotal = toChip(room.legal.raiseTo.min, 'legal minimum');
        const rawMax = toChip(room.legal.raiseTo.max, 'legal maximum');
        legal.maxTargetTotal = heroMaxTotal !== null && rawMax > heroMaxTotal
          ? heroMaxTotal
          : rawMax;
      }
      return legal;
    });

  return create(DecisionRequestSchema, {
    state: {
      game: {
        variant: GameVariant.NLHE,
        bettingStructure: BettingStructure.NO_LIMIT,
        gameType: GameType.CASH,
        tableCapacity,
        amountUnit: { name: 'chip', decimalPlaces: 0 },
        smallBlind: toChip(blinds[0], 'small blind'),
        bigBlind: toChip(blinds[1], 'big blind'),
        ruleProfile: {
          settlement: SettlementRule.CLOCKWISE_LEFT_OF_BUTTON,
          reopening: ReopeningRule.NEVER_CUMULATIVE,
          runout: RunoutRule.SINGLE,
          dealtSeatCount: players.length,
        },
      },
      handId: room.handId,
      decisionIndex: toChip(room.revision, 'revision'),
      street,
      buttonSeat: room.dealer ?? room.hero.seat,
      heroSeat: room.hero.seat,
      players,
      heroHoleCards: (room.hero.cards ?? []).map(c => mapCard(c.rank, c.suit)),
      board: (room.board ?? []).map(c => mapCard(c.rank, c.suit)),
      pot: {
        phase: PotPhase.ACTION,
        layers: potLayers,
        potTotal: toChip(room.pot, 'pot'),
      },
      forcedContributions: forced,
      transitionPath,
      legalActions,
      toCall: toChip(room.legal.call ?? 0, 'to call'),
      provenance: config.provenance ?? observationLedger.provenance(room.handId),
    },
    options: {
      strategyProfile: config.style ?? 'tag',
      solveTimeBudgetMs: resolveSolveTimeBudgetMs(config.solveTimeBudgetMs),
      seed: 0n,
      includeSampledAction: true,
      includeFullStrategy: false,
      solverMode: config.solverMode ?? SolverMode.AUTOMATIC,
      preflopEffectiveStackBb: effectiveStackHint(room),
      ...(config.minimumGuaranteeLevel !== undefined
        ? { minimumGuarantee: GUARANTEE_LEVEL_BY_TOKEN[config.minimumGuaranteeLevel] }
        : {}),
      utilityProfile: UtilityProfile.CASH_CHIP,
    },
  });
}

// ---------------------------------------------------------------------------
// decisionEnvelope
// ---------------------------------------------------------------------------

export function decisionEnvelope(request: DecisionRequest): Envelope {
  const envelope = create(EnvelopeSchema, { protocolVersion: 3 });
  envelope.payload = { case: 'decisionRequest', value: request };
  return envelope;
}

// ---------------------------------------------------------------------------
// fromDecisionResponse
// ---------------------------------------------------------------------------

type StrategyLike = Strategy | ExpandedStrategy;

function executeFromStrategy(
  strategy: StrategyLike,
  room: RiverRoom,
  guaranteeLevel: string | undefined,
  artifactSha256?: string,
): ExecutableDecision {
  if (strategy.actions.length < 1)
    throw new EngineError('NO_DECISION', ErrorCode.NO_DECISION, true);

  const selected = strategy.selectedAction;
  if (!selected) {
    throw new EngineError('NO_DECISION', ErrorCode.NO_DECISION, true);
  }
  const isMember = strategy.actions.some(policy =>
    policy.type === selected.type
    && (policy.targetTotal ?? 0n) === (selected.targetTotal ?? 0n));
  if (!isMember)
    throw new Error('selected action is not a member of the strategy distribution');

  const action = ACTION_NAME_BY_KIND[selected.type];
  if (action === undefined)
    throw new Error(`unknown engine action kind ${selected.type}`);
  const target = chipToNumber(selected.targetTotal, 'selected target');

  const raw = {
    action,
    ...(target !== undefined ? { amount: target } : {}),
    reason: strategy.solver?.diagnosticReason
      || strategy.solver?.reasonCode
      || (strategy.solver?.source === SolverSource.BLUEPRINT ? 'blueprint' : 'engine'),
    ...(strategy.solver?.equity !== undefined ? { equity: strategy.solver.equity } : {}),
    ...(strategy.solver?.minimumDefenseFrequency !== undefined
      ? { mdf: strategy.solver.minimumDefenseFrequency }
      : {}),
  };
  const validated = validateEngineDecision(room, raw);
  if (!validated)
    throw new Error('engine strategy failed platform legality validation');
  return {
    ...validated,
    ...(guaranteeLevel === undefined ? {} : { guaranteeLevel }),
    ...(artifactSha256 === undefined ? {} : { artifactSha256 }),
  };
}

/**
 * Converts a DecisionResponse into the executable decision shape.
 * Handles both Strategy and ExpandedStrategy. Engine errors and transport
 * anomalies throw; the caller applies the operational safe fallback.
 */
export function fromDecisionResponse(
  envelope: Envelope,
  room: RiverRoom,
): ExecutableDecision {
  if (envelope.payload.case !== 'decisionResponse') {
    throw new Error('engine response did not contain a decision');
  }
  const response = envelope.payload.value;
  if (response.result.case === 'error') {
    const error = response.result.value;
    const name = ErrorCode[error.code] ?? 'UNSPECIFIED';
    throw new EngineError(name, error.code, error.retryable);
  }
  const level = response.result.case === 'strategy' || response.result.case === 'expandedStrategy'
    ? response.result.value.solver?.guaranteeLevel
    : undefined;
  const digest = response.result.case === 'strategy' || response.result.case === 'expandedStrategy'
    ? response.result.value.solver?.artifactSha256
    : undefined;
  if (response.result.case === 'strategy')
    return executeFromStrategy(response.result.value, room, level, digest);
  if (response.result.case === 'expandedStrategy')
    return executeFromStrategy(response.result.value, room, level, digest);
  throw new EngineError('NO_DECISION', ErrorCode.NO_DECISION, true);
}
