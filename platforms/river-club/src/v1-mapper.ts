import { create, type MessageInitShape } from '@bufbuild/protobuf';

import {
  ActionType,
  ActionEventSchema,
  BettingStructure,
  CardSchema,
  DecisionRequestSchema,
  EnvelopeSchema,
  ErrorCode,
  ForcedContributionType,
  GameType,
  GameVariant,
  LegalActionSchema,
  PlayerStatus,
  PlayerStateSchema,
  Rank,
  SolverMode,
  SolverSource,
  Street,
  Suit,
  type DecisionRequest,
  type Envelope,
  type ExpandedStrategy,
  type Strategy,
} from '../../../build/generated/ts/bigshark/engine/v1/engine_pb.js';
import { validateEngineDecision } from './v0-normalizer.js';
import type {
  Action,
  ExecutableDecision,
  RiverEvent,
  RiverRoom,
  RiverSeat,
} from './types.js';

const PLAYER_ACTION_KINDS = new Set<Action>(['fold', 'check', 'call', 'bet', 'raise']);

type RoundCode = 'x' | 'c' | 'b' | 'r' | 'f';

interface ReplayedEvent {
  event: RiverEvent;
  code: RoundCode;
  actorName: string;
  group: number;
}

// Replays the v0 token-round grouping (identical to splitRounds in
// v0-normalizer) but assigns every voluntary event a round index even after
// a fold, when round transitions freeze. River snapshots carry no per-event
// street tag, so the grouped round is the authoritative street source. The
// C++ mapper replays the same token sequence to rebuild the river compact
// line and counts preflop knobs from the resulting street tags. Actor
// resolution by full display name happens later (resolveActor), so the
// replay keeps the complete event text rather than a first-token truncation.
function replayActionRounds(events: RiverEvent[]): ReplayedEvent[] {
  const replayed: ReplayedEvent[] = [];
  let group = 0;
  let raised = false;
  let passive = 0;
  let handOver = false;
  for (const event of events) {
    if (!PLAYER_ACTION_KINDS.has(event.kind as Action))
      continue;
    const code: RoundCode = event.kind === 'check'
      ? 'x'
      : event.kind[0] as RoundCode;
    replayed.push({ event, code, actorName: event.text || '', group });
    if (handOver)
      continue;
    if (code === 'f') {
      handOver = true;
      continue;
    }
    if (code === 'b' || code === 'r') {
      raised = true;
      passive = 0;
      continue;
    }
    if (code === 'c' && (raised || group > 0)) {
      group += 1;
      raised = false;
      passive = 0;
      continue;
    }
    passive += 1;
    if (passive >= 2) {
      group += 1;
      raised = false;
      passive = 0;
    }
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
  // Multi-round preflop sequences (4-bet ladders) can push the token replay
  // past the real street; the structured tag cannot exceed the current street.
  return derived > current ? current : derived;
}

const MAX_SAFE_CHIP = BigInt(Number.MAX_SAFE_INTEGER);

// Mirrors v0_json.cpp's `effectiveStackBb <= 0 ? 100 : value`: pick the
// platform value (solver first, then hero snapshot), but any non-finite or
// non-positive value becomes the 100 bb default so it is always a valid hint.
function effectiveStackHint(room: RiverRoom): number {
  const raw = room.solver?.effectiveStackBb ?? room.hero?.effectiveStackBb;
  return typeof raw === 'number' && Number.isFinite(raw) && raw > 0 ? raw : 100;
}

/** Raised when the engine returns EngineError or an unusable strategy. The
 * runner treats this as an operational failure and applies safeFallback; it
 * is never converted into a strategic fold. */
export class V1EngineError extends Error {
  constructor(
    readonly codeName: string,
    readonly code: ErrorCode,
    readonly retryable: boolean,
  ) {
    super(`engine error ${codeName}`);
    this.name = 'V1EngineError';
  }
}

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

function playerId(seat: number): string {
  return `seat-${seat}`;
}

function dealtIn(seat: RiverSeat): boolean {
  if (seat.status === 'active' || seat.status === 'all-in' || seat.status === 'folded')
    return true;
  // Seats marked waiting/leaving by River still participate in position
  // labeling when they posted money or carry a table position.
  return Boolean(seat.blind) || (seat.bet ?? 0) > 0 || Boolean(seat.position);
}

function mapCard(rank: string, suit: string): MessageInitShape<typeof CardSchema> {
  const rankEnum = RANK_BY_CHAR[rank];
  const suitEnum = SUIT_BY_WORD[suit];
  if (rankEnum === undefined || suitEnum === undefined) {
    throw new Error(`unmappable card ${rank}${suit}`);
  }
  return { rank: rankEnum, suit: suitEnum };
}

/** Builds a v1 DecisionRequest from the same frozen River state the v0 path
 * consumes. Throws on any amount outside the safe integer chip profile. */
export function toV1DecisionRequest(
  room: RiverRoom,
  config: { style?: string; heroName?: string; solverMode?: SolverMode } = {},
): DecisionRequest {
  if (!room.legal)
    throw new Error('legal actions are required');
  const street = STREET_BY_NAME[room.street];
  if (street === null)
    throw new Error(`cannot decide on street ${room.street}`);

  const blinds = room.blinds ?? [10, 20];
  const seats = room.seats.filter((seat): seat is RiverSeat => seat !== null);
  // River seats is a fixed table-slot array that can arrive sparse; the game
  // capacity is one past the highest occupied slot, not the JSON array length.
  const tableCapacity = seats.reduce(
    (capacity, seat) => Math.max(capacity, seat.seat + 1),
    2,
  );

  const players: MessageInitShape<typeof PlayerStateSchema>[] = [];
  for (const seat of seats) {
    if (!dealtIn(seat))
      continue;
    // v1 stack is the actual chip stack behind. It must not come from
    // hero.effectiveStackBb (a solver-derived estimate: frozen hands show
    // 1920 vs 1940 behind and 38 bb vs a 10,125 stack); target-total legality
    // is checked against stack + street_committed.
    players.push({
      playerId: playerId(seat.seat),
      seat: seat.seat,
      stack: toChip(seat.stack, `seat ${seat.seat} stack`),
      streetCommitted: toChip(seat.bet ?? 0, `seat ${seat.seat} bet`),
      status:
        seat.status === 'folded' || seat.status === 'leaving' || seat.status === 'waiting'
          ? PlayerStatus.FOLDED
          : seat.status === 'all-in'
            ? PlayerStatus.ALL_IN
            : PlayerStatus.ACTIVE,
    });
  }

  const forced = seats
    .filter(seat => seat.blind !== undefined)
    .flatMap(seat => {
      const id = playerId(seat.seat);
      if (seat.blind === 'SB') {
        return [{
          playerId: id,
          type: ForcedContributionType.SMALL_BLIND,
          amount: toChip(blinds[0], 'small blind'),
        }];
      }
      if (seat.blind === 'BB') {
        return [{
          playerId: id,
          type: ForcedContributionType.BIG_BLIND,
          amount: toChip(blinds[1], 'big blind'),
        }];
      }
      return [{
        playerId: id,
        type: ForcedContributionType.STRADDLE,
        amount: toChip(seat.bet ?? 0, 'straddle'),
      }];
    });

  const dealtSeats = seats.filter(dealtIn);
  const dealtPlayerIds = new Set(dealtSeats.map(seat => playerId(seat.seat)));

  // Resolves an event text (e.g. "Hero raise" or "Sir Lancelot call") to a
  // structured player id. Matching follows the v0 normalizer exactly: the
  // event text starts with the seat's full display name. The longest matching
  // name wins so a multi-word hero ("John Doe") is never shadowed by a
  // shorter player ("John"). The hero is checked first (v0's
  // text.startsWith(heroName)); otherwise the longest opponent prefix wins.
  // An actor matching no seat is attributed to a deterministic opponent
  // rather than failing a normal room, matching v0's lenient H/O tagging.
  const resolveActor = (eventText: string): string => {
    const text = eventText || '';
    const heroSeat = seats.find(candidate => candidate.seat === room.hero.seat);
    if (heroSeat?.name && text.startsWith(heroSeat.name))
      return playerId(room.hero.seat);
    let best: RiverSeat | undefined;
    for (const candidate of seats) {
      if (candidate.seat === room.hero.seat)
        continue;
      if (candidate.name && text.startsWith(candidate.name)
          && (!best?.name || candidate.name.length > best.name.length))
        best = candidate;
    }
    if (best)
      return playerId(best.seat);
    const opponent = dealtSeats.find(
      candidate => candidate.seat !== room.hero.seat && dealtIn(candidate),
    );
    return playerId(opponent ? opponent.seat : room.hero.seat);
  };

  // Minor-0 parity shim for the preflop opener position. The v0 normalizer
  // derives the opener from preflopRaises[0] (the first preflop raise in
  // event order) with first-whitespace-token EXACT equality against seat.name
  // (v0-normalizer.ts). This is independent of the betting-round replay: any
  // number of limps before it (which advance the replay group) does not
  // change which event is the opener. A multi-word opener like "Sir Lancelot"
  // yields token "Sir", matches no seat, and v0 treats it as no opener (the
  // C++ policy defaults to HJ). We identify the opener before the replay map
  // from the raw River events so the grouping cannot mask it.
  const v0StyleOpenerId = (eventText: string): string | null => {
    const firstToken = (eventText || '').split(' ')[0] || '';
    const match = seats.find(seat => seat.name === firstToken);
    return match ? playerId(match.seat) : null;
  };
  // The opener is preflopRaises[0] in raw River event order (v0 normalizer),
  // independent of replay grouping: any number of limps before it still makes
  // it the opener. On a preflop decision street only. replayActionRounds
  // preserves player-action order, so the raw first raise is the first
  // replayed token with code 'r'.
  const hasPreflopOpener = street === Street.PREFLOP
    && (room.events ?? []).some(event => event.kind === 'raise');
  const openerEventText = street === Street.PREFLOP
    ? (room.events ?? []).find(event => event.kind === 'raise')?.text ?? ''
    : null;
  const openerShimId = openerEventText === null
    ? null
    : v0StyleOpenerId(openerEventText) ?? '';

  const replayedActions = replayActionRounds(room.events ?? []);
  let openerApplied = false;

  // Current-street exact commitments drive structured amount recovery. The
  // snapshot seat.bet is a live exact integer (never a rounded display).
  const streetCommitment = currentStreetCommitments(room);

  // Pre-compute, for each replayed chip event, whether it is the actor's LAST
  // chip-moving action of the current decision street. Only such an event can
  // be recovered exactly from the structured seat commitment: its resulting
  // cumulative total equals the snapshot seat.bet. An earlier (non-final)
  // action by a player who acts again this street is not recoverable from the
  // end-of-street snapshot and must rely on exact-integer text.
  const isLastCurrentStreetChip = replayedActions.map((replayed, index) => {
    const wireStreet = streetForGroup(replayed.group, street);
    if (wireStreet !== street || !['b', 'r', 'c'].includes(replayed.code))
      return false;
    const actorId = resolveActor(replayed.actorName);
    for (let later = index + 1; later < replayedActions.length; later += 1) {
      const other = replayedActions[later]!;
      if (streetForGroup(other.group, street) !== wireStreet)
        continue;
      if (!['b', 'r', 'c'].includes(other.code))
        continue;
      if (resolveActor(other.actorName) === actorId)
        return false;  // this actor moves chips again later this street
    }
    return true;
  });
  // Per-(street, actor) street-paid from amounts actually emitted, so a
  // structured CALL increment can subtract that actor's EARLIER current-street
  // commitment. Street-scoped: seat.bet and the C++ ledger reset at each
  // dealing boundary, so preflop/postflop amounts never mix.
  //
  // The preflop street ledger is seeded with each actor's posted blind,
  // matching the C++ reconstructor (v1_resident_mapper.cpp seeds
  // preflopStreetPaid from forced contributions before replaying voluntary
  // events). Without this, the structured recovery of a blind-posting
  // actor's final preflop CALL would compute finalCommit - 0 and include the
  // blind (SB limp: would emit 100 instead of the 90 new chips); postflop
  // streets have no blind seed and are unaffected.
  // (street, actor) pairs whose ledger already has an amount we could not
  // fill; later structured recovery there would mis-subtract, so it is
  // disabled (the request will deterministically miss).
  const actorLedgerIncomplete = new Set<string>();
  const paidKey = (wireStreetValue: number | string, actor: string) =>
    `${wireStreetValue}:${actor}`;

  const structuredStreetPaid = new Map<string, bigint>();
  for (const seat of seats) {
    if (seat.blind !== 'SB' && seat.blind !== 'BB')
      continue;
    const blindAmount = seat.blind === 'BB' ? blinds[1] : blinds[0];
    structuredStreetPaid.set(
      paidKey(Street.PREFLOP, playerId(seat.seat)),
      toChip(blindAmount, 'blind'),
    );
  }

  // Assign streets through the v0 betting-round replay so grouped rounds
  // match the C++ line reconstruction exactly; tags are capped at the current
  // street for multi-round preflop ladders.
  const actionHistory: MessageInitShape<typeof ActionEventSchema>[] =
    replayedActions.map((replayed, sequence) => {
      let actor = resolveActor(replayed.actorName);
      // Apply the opener shim to the FIRST raise token in replay order,
      // regardless of the replay group limps advanced it to. The actor
      // becomes the v0 first-token seat, or an empty id (unresolved -> C++
      // HJ default).
      if (hasPreflopOpener && !openerApplied && replayed.code === 'r') {
        openerApplied = true;
        actor = openerShimId ?? actor;
      }
      const kind = replayed.code === 'x'
        ? 'check'
        : replayed.code === 'c'
          ? 'call'
          : replayed.code === 'b'
            ? 'bet'
            : replayed.code === 'r'
              ? 'raise'
              : 'fold';
      const wireEvent: MessageInitShape<typeof ActionEventSchema> = {
        sequence: BigInt(sequence),
        street: streetForGroup(replayed.group, street),
        actorPlayerId: actor,
        action: ACTION_BY_NAME[kind],
      };
      // Chip amount rules (minor 1 resident replay; minor 0 ignores these):
      //  1. past/closed streets: exact-integer text ONLY; rounded K/M/B text
      //     is left unset (deterministic OffTree miss);
      //  2. current street: exact-integer text when present, otherwise a
      //     structured recovery from the live seat commitment is allowed only
      //     for the actor's final chip action this street;
      //  3. folded/check actions carry no amount.
      // Rounded display text is never emitted as an exact chip value.
      const wireStreet = streetForGroup(replayed.group, street);
      if (kind === 'bet' || kind === 'raise' || kind === 'call') {
        const exact = parseExactChipAmount(replayed.event.text);
        const ledgerKey = paidKey(wireStreet, actor);
        const prior = structuredStreetPaid.get(ledgerKey) ?? 0n;
        let targetTotal: bigint | null = null;
        let incrementalAmount: bigint | null = null;
        if (exact !== null) {
          if (kind === 'call')
            incrementalAmount = exact;
          else
            targetTotal = exact;
        } else if (wireStreet === street && isLastCurrentStreetChip[sequence]
                   && !actorLedgerIncomplete.has(ledgerKey)) {
          const finalCommit = streetCommitment.get(actor);
          if (finalCommit !== undefined && finalCommit > 0n && finalCommit >= prior) {
            if (kind === 'call')
              incrementalAmount = finalCommit - prior;
            else
              targetTotal = finalCommit;
          }
        }
        if (targetTotal !== null && targetTotal > 0n) {
          wireEvent.targetTotal = targetTotal;
          structuredStreetPaid.set(ledgerKey, targetTotal);
        } else if (incrementalAmount !== null && incrementalAmount > 0n) {
          wireEvent.incrementalAmount = incrementalAmount;
          structuredStreetPaid.set(ledgerKey, prior + incrementalAmount);
        } else {
          actorLedgerIncomplete.add(ledgerKey);
        }
      }
      return wireEvent;
    });

  const legalActions: MessageInitShape<typeof LegalActionSchema>[] =
    room.legal.actions.map(action => {
      const legal: MessageInitShape<typeof LegalActionSchema> = {
        type: ACTION_BY_NAME[action],
      };
      if ((action === 'bet' || action === 'raise') && room.legal?.raiseTo) {
        legal.minTargetTotal = toChip(room.legal.raiseTo.min, 'legal minimum');
        legal.maxTargetTotal = toChip(room.legal.raiseTo.max, 'legal maximum');
      }
      return legal;
    });

  // River partitions the total into main pot plus side pots. Dropping the
  // side pots would make the structured pot internally inconsistent; with any
  // side pot present the engine cleanly answers UNSUPPORTED_FEATURE (minor 0
  // has no side-pot utility) which routes to the operational safe fallback.
  const riverPots = room.pots ?? [];
  const mainPot = riverPots[0]?.size ?? room.pot;
  const sidePots = riverPots.slice(1).map(potEntry => ({
    amount: toChip(potEntry.size, 'side pot'),
    eligiblePlayerIds: potEntry.eligible
      .filter(seat => dealtPlayerIds.has(playerId(seat)))
      .map(seat => playerId(seat)),
  }));

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
      },
      handId: room.handId,
      // River room revisions are monotonic counters; minor-0 carries them as
      // the decision index so engine-side fallback seeds match v0 exactly.
      decisionIndex: toChip(room.revision, 'revision'),
      street,
      buttonSeat: room.dealer ?? room.hero.seat,
      heroPlayerId: playerId(room.hero.seat),
      players,
      heroHoleCards: (room.hero.cards ?? []).map(card => mapCard(card.rank, card.suit)),
      board: (room.board ?? []).map(card => mapCard(card.rank, card.suit)),
      pot: {
        potTotal: toChip(room.pot, 'pot'),
        mainPot: toChip(mainPot, 'main pot'),
        sidePots,
      },
      forcedContributions: forced,
      actionHistory,
      legalActions,
      toCall: toChip(room.legal.call ?? 0, 'to call'),
    },
    options: {
      strategyProfile: config.style ?? 'tag',
      solveTimeBudgetMs: 2000,
      seed: 0n,
      includeSampledAction: true,
      includeFullStrategy: false,
      solverMode: config.solverMode ?? SolverMode.AUTOMATIC,
      // Exact parity with v0-normalizer.ts: pass the platform-computed
      // preflop effective stack (solver preferred, then hero snapshot).
      // v0_json sanitizes a missing/non-positive value to the 100 bb default,
      // so a 0 or negative server value must never reach the engine as an
      // (rejected) hint; mirror that sanitization with Number.isFinite.
      preflopEffectiveStackBb: effectiveStackHint(room),
    },
  });
}

export function decisionEnvelope(request: DecisionRequest, protocolMinor: 0 | 1 = 0): Envelope {
  const envelope = create(EnvelopeSchema, {
    protocolMinor,
  });
  envelope.payload = { case: 'decisionRequest', value: request };
  return envelope;
}

type StrategyLike = Strategy | ExpandedStrategy;

// River localizes chip moves as a trailing chip token. Two display classes
// exist in production:
//
//   1) exact integers ("... 70", "<name> 980") — authoritative;
//   2) rounded display ("... 1.2K", "ALL IN 3.6K", "2.5M") — rounded to a
//      100/1000 grid and NOT the exact chip total (a real half-pot bet of 1225
//      is displayed "1.2K"; treating 1200 as the target would replay the wrong
//      node).
//
// Only class 1 is trusted: the LAST whitespace token must be pure digits with
// no decimal point and no K/M/B multiplier suffix. Any decimal or suffix, or a
// missing amount, returns null. The caller then leaves the wire amount unset
// (deterministic engine OffTree miss) unless current-street structured data
// recovers the exact value. No rounded value is ever sent as exact. Exported
// for direct unit testing.
export function parseExactChipAmount(text: string): bigint | null {
  const token = (text || '').trim().split(/\s+/).at(-1) ?? '';
  if (!/^[0-9]+$/.test(token))
    return null;
  const value = Number(token);
  if (!Number.isSafeInteger(value) || value <= 0)
    return null;
  return BigInt(value);
}

// Exact current-street commitment per wire player id, taken from the live
// structured snapshot (seat.bet). These are exact integers, never rounded.
function currentStreetCommitments(room: RiverRoom): Map<string, bigint> {
  const commitments = new Map<string, bigint>();
  for (const seat of room.seats ?? []) {
    if (!seat)
      continue;
    commitments.set(playerId(seat.seat), toChip(seat.bet ?? 0, 'seat bet'));
  }
  return commitments;
}

function chipToNumber(value: bigint | undefined, field: string): number | undefined {
  if (value === undefined)
    return undefined;
  if (value < 0n || value > MAX_SAFE_CHIP) {
    throw new Error(`${field} is outside the safe integer chip range`);
  }
  return Number(value);
}

// Validates the sampled action against the FULL reported distribution (up to
// 32 actions on minor 1) by kind and exact target total, then converts it into
// the executable decision the v0 runner validates against the platform legal
// set. Engine errors and transport anomalies throw; the caller applies the
// operational safe fallback rather than folding.
function executeFromStrategy(strategy: StrategyLike,
                             room: Parameters<typeof fromV1DecisionResponse>[1]):
    Awaited<ReturnType<typeof fromV1DecisionResponse>> {
  if (strategy.actions.length < 1)
    throw new V1EngineError('NO_DECISION', ErrorCode.NO_DECISION, true);

  // The sampled action is what executes. It must be a member of the full
  // reported distribution by kind and exact target total.
  const selected = strategy.selectedAction;
  if (!selected) {
    throw new V1EngineError(
      'NO_DECISION',
      ErrorCode.NO_DECISION,
      true,
    );
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
      || (strategy.solver?.source === SolverSource.BLUEPRINT ? 'blueprint' : 'v1'),
    ...(strategy.solver?.equity !== undefined ? { equity: strategy.solver.equity } : {}),
    ...(strategy.solver?.minimumDefenseFrequency !== undefined
      ? { mdf: strategy.solver.minimumDefenseFrequency }
      : {}),
  };
  const validated = validateEngineDecision(room, raw);
  if (!validated)
    throw new Error('engine strategy failed platform legality validation');
  return validated;
}

/** Converts a v1 DecisionResponse into the executable decision shape the v0
 * runner already validates. Handles both the minor-0 Strategy (at most five
 * actions, degenerate) and the negotiated minor-1 ExpandedStrategy (the full
 * resident distribution, up to 32 actions). Engine errors and transport
 * anomalies throw; the caller applies the operational safe fallback rather
 * than folding. */
export function fromV1DecisionResponse(
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
    throw new V1EngineError(name, error.code, error.retryable);
  }
  if (response.result.case === 'strategy')
    return executeFromStrategy(response.result.value, room);
  if (response.result.case === 'expandedStrategy')
    return executeFromStrategy(response.result.value, room);
  throw new V1EngineError('NO_DECISION', ErrorCode.NO_DECISION, true);
}

/** RFC 0005 Stage 9 certification classification of a response, decoded
 * WITHOUT executing it. The adapter uses this to decide whether a forced
 * RESOLVING request produced a certified whole-range result. Anything other
 * than source RESOLVING(7) with guarantee "modeled_exact_bound" is uncertified
 * (a deadline baseline carries source BLUEPRINT / "baseline") and must be
 * labeled as such by the caller; an EngineError is never a strategic fold. */
export type ResolveCertification =
  | { kind: 'modeled-exact-bound'; digest: string }
  | { kind: 'baseline'; digest?: string }
  | { kind: 'uncertified'; digest?: string }
  | { kind: 'error'; code: number; retryable: boolean };

export function classifyResolveResponse(envelope: Envelope): ResolveCertification {
  if (envelope.payload.case !== 'decisionResponse')
    return { kind: 'uncertified' };
  const response = envelope.payload.value;
  if (response.result.case === 'error') {
    return {
      kind: 'error',
      code: response.result.value.code,
      retryable: response.result.value.retryable,
    };
  }
  const strategy: StrategyLike | undefined =
    response.result.case === 'expandedStrategy'
      ? response.result.value
      : response.result.case === 'strategy'
        ? response.result.value
        : undefined;
  const guarantee = strategy?.solver?.guarantee;
  const digest = strategy?.solver?.artifactSha256;
  if (strategy?.solver?.source === SolverSource.RESOLVING
      && guarantee === 'modeled_exact_bound'
      && digest !== undefined)
    return { kind: 'modeled-exact-bound', digest };
  if (guarantee === 'baseline')
    return { kind: 'baseline', ...(digest !== undefined ? { digest } : {}) };
  return { kind: 'uncertified', ...(digest !== undefined ? { digest } : {}) };
}
