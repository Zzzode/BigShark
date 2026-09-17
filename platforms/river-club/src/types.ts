export type JsonPrimitive = boolean | number | string | null;
export type JsonValue = JsonPrimitive | JsonValue[] | { [key: string]: JsonValue };
export type JsonObject = { [key: string]: JsonValue };

export type Action = 'fold' | 'check' | 'call' | 'bet' | 'raise';
export type Street = 'preflop' | 'flop' | 'turn' | 'river' | 'showdown';
export type PlayerStatus = 'active' | 'all-in' | 'folded' | 'leaving' | 'waiting';

export interface Card {
  rank: string;
  suit: string;
}

export interface RaiseRange {
  min: number;
  max: number;
}

export interface LegalActions {
  actions: Action[];
  call: number;
  potOdds: number;
  raiseTo?: RaiseRange;
  min?: number;
  max?: number;
  currentBet?: number;
  amountMeaning?: string;
}

export interface RiverEvent {
  kind: string;
  text: string;
}

export interface RiverSeat {
  seat: number;
  name?: string;
  stack: number;
  bet?: number;
  status: PlayerStatus;
  cards?: Card[];
  action?: string;
  button?: boolean;
  agent?: boolean;
  blind?: string;
  position?: string;
}

export interface RiverHero {
  seat: number;
  name?: string;
  stack: number;
  bet?: number;
  cards: Card[];
  status: PlayerStatus;
  effectiveStack?: number;
  effectiveStackBb?: number;
  position?: string;
}

export interface RiverSolver {
  format?: string;
  game?: string;
  street?: Street;
  heroPosition?: string;
  playersInHand?: number;
  potBb?: number;
  effectiveStackBb?: number;
  spr?: number;
  toCallBb?: number;
  potOdds?: number;
  raiseToBb?: number[];
}

export interface RiverWinner {
  seat?: number;
  amount?: number;
  description?: string;
}

export interface RiverPot {
  size: number;
  eligible: number[];
}

export interface RiverRoom {
  id: string;
  name?: string;
  mode?: string;
  next?: string;
  revision: number;
  handId: string;
  street: Street;
  blinds?: [number, number];
  timeLeftMs?: number;
  hero: RiverHero;
  dealer?: number;
  actor?: number | null;
  board: Card[];
  pot: number;
  pots?: RiverPot[];
  seats: Array<RiverSeat | null>;
  legal?: LegalActions | null;
  solver?: RiverSolver;
  events?: RiverEvent[];
  winners?: RiverWinner[];
}

export interface RiverRoomReference {
  id: string;
  revision: number;
  mode?: string;
  next?: string;
}

export type RiverRoomState = RiverRoom | RiverRoomReference;

export interface RiverMe {
  id?: string;
  name: string;
  wallet?: number;
}

export interface RiverState {
  protocol?: number;
  changed?: boolean;
  serverTime?: number;
  me?: RiverMe;
  room?: RiverRoomState | null;
  control?: JsonValue;
  ok?: boolean;
  alreadySeated?: boolean;
}

export interface RiverApiError {
  ok: false;
  error: string;
  status?: number;
  code?: string;
  retryable?: boolean;
  retryAfterMs?: number;
  next?: string;
  details?: {
    code?: string;
    retryable?: boolean;
    retryAfterMs?: number;
    next?: string;
  };
}

export interface RiverRoomSummary {
  id: string;
  name: string;
  allowAgents: boolean;
  status: string;
  seats: [number, number];
  blinds: [number, number];
}

export interface RiverRoomsResponse {
  rooms: RiverRoomSummary[];
}

export interface RiverConfig {
  url?: string;
  token?: string;
}

export interface CachedDecisionState {
  savedAt: number;
  room: {
    id: string;
    mode?: string;
    revision: number;
    handId: string;
    timeLeftMs?: number;
    legal?: LegalActions | null;
  };
}

export interface V0LegalContext {
  actions: Action[];
  call: number;
  potOdds: number;
  raiseTo?: RaiseRange;
}

export interface V0DecisionContext {
  handId: string;
  revision: number;
  style: string;
  street: Street;
  blinds: [number, number];
  position: string;
  playersInHand: number;
  effectiveStackBb: number;
  hole: string[];
  board: string[];
  pot: number;
  riverGtoOn: boolean;
  riverLine?: string;
  flopLine?: string;
  turnLine?: string;
  riverBetFrac?: number;
  raises: number;
  openerPosition: string;
  heroWasRaiser: boolean;
  heroPreflopAggressor: boolean;
  limpers: number;
  legal?: V0LegalContext;
}

export interface RawEngineDecision {
  action: Action;
  amount?: number;
  reason: string;
  equity?: number;
  mdf?: number;
}

export interface ExecutableDecision {
  action: Action;
  amount?: number;
  reason: string;
}

/** Structural framed v1 client type; the concrete Envelope types live in the
 * generated Protobuf bindings imported by v1-mapper. */
export interface V1EnvelopeClient {
  request(envelope: unknown, timeoutMs?: number): Promise<unknown>;
  /** Negotiated protocol minor (0 unless the client opted into minor 1 and the
   * host capability handshake succeeded). */
  readonly negotiatedProtocolMinor?: 0 | 1;
  /** True when the current process negotiated minor 1. */
  readonly minor1Capable?: boolean;
}

export interface EngineConfig {
  style?: string;
  heroName?: string;
  timeoutMs?: number;
  /** Opt in to the RFC 0002 framed Protobuf engine path. */
  proto?: boolean;
  protoEngineClient?: V1EnvelopeClient;
  engineClient?: {
    request(message: V0DecisionContext, timeoutMs?: number): Promise<RawEngineDecision>;
  };
  /** Negotiated minor 1: force a resident blueprint lookup for covered
   * postflop heads-up decisions (a coverage miss routes to the operational
   * safe fallback) instead of AUTOMATIC heuristic-first mode. Requires a
   * client that negotiated minor 1; otherwise requests stay on minor 0. */
  protoBlueprint?: boolean;
}

export interface GoldenFixture {
  name: string;
  state: RiverState & { me: RiverMe; room: RiverRoom };
  context: V0DecisionContext;
  decision: ExecutableDecision;
}

export function isAction(value: unknown): value is Action {
  return value === 'fold'
    || value === 'check'
    || value === 'call'
    || value === 'bet'
    || value === 'raise';
}

export function isStreet(value: unknown): value is Street {
  return value === 'preflop'
    || value === 'flop'
    || value === 'turn'
    || value === 'river'
    || value === 'showdown';
}

export function isRiverRoom(
  room: unknown,
): room is RiverRoom {
  if (!isObject(room)) return false;
  return typeof room.id === 'string'
    && typeof room.revision === 'number'
    && typeof room.handId === 'string'
    && isStreet(room.street)
    && optionalString(room.name)
    && optionalString(room.mode)
    && optionalString(room.next)
    && isHero(room.hero)
    && Array.isArray(room.board)
    && room.board.every(isCard)
    && typeof room.pot === 'number'
    && Array.isArray(room.seats)
    && room.seats.every(seat => seat === null || isSeat(seat))
    && (room.blinds === undefined || isNumberPair(room.blinds))
    && optionalNumber(room.timeLeftMs)
    && optionalNumber(room.dealer)
    && (room.actor === undefined || room.actor === null || typeof room.actor === 'number')
    && (room.legal === undefined || room.legal === null || isLegalActions(room.legal))
    && (room.events === undefined
      || (Array.isArray(room.events) && room.events.every(isEvent)))
    && (room.pots === undefined
      || (Array.isArray(room.pots) && room.pots.every(isPot)))
    && (room.solver === undefined || isSolver(room.solver))
    && (room.winners === undefined
      || (Array.isArray(room.winners) && room.winners.every(isWinner)));
}

export function isRiverRoomReference(value: unknown): value is RiverRoomReference {
  return isObject(value)
    && typeof value.id === 'string'
    && typeof value.revision === 'number'
    && !('hero' in value)
    && !('handId' in value)
    && !('street' in value)
    && !('board' in value)
    && !('pot' in value)
    && !('seats' in value);
}

function isObject(value: unknown): value is Record<string, unknown> {
  return typeof value === 'object' && value !== null && !Array.isArray(value);
}

function isCard(value: unknown): value is Card {
  return isObject(value)
    && typeof value.rank === 'string'
    && typeof value.suit === 'string';
}

function isPlayerStatus(value: unknown): value is PlayerStatus {
  return value === 'active'
    || value === 'all-in'
    || value === 'folded'
    || value === 'leaving'
    || value === 'waiting';
}

function isHero(value: unknown): value is RiverHero {
  return isObject(value)
    && typeof value.seat === 'number'
    && typeof value.stack === 'number'
    && Array.isArray(value.cards)
    && value.cards.every(isCard)
    && isPlayerStatus(value.status)
    && optionalString(value.name)
    && optionalNumber(value.bet)
    && optionalNumber(value.effectiveStack)
    && optionalNumber(value.effectiveStackBb)
    && optionalString(value.position);
}

function isSeat(value: unknown): value is RiverSeat {
  return isObject(value)
    && typeof value.seat === 'number'
    && typeof value.stack === 'number'
    && isPlayerStatus(value.status)
    && optionalString(value.name)
    && optionalNumber(value.bet)
    && optionalString(value.action)
    && optionalBoolean(value.button)
    && optionalBoolean(value.agent)
    && optionalString(value.blind)
    && optionalString(value.position)
    && (value.cards === undefined
      || (Array.isArray(value.cards) && value.cards.every(isCard)));
}

function isLegalActions(value: unknown): value is LegalActions {
  if (!isObject(value)
    || !Array.isArray(value.actions)
    || !value.actions.every(isAction)
    || typeof value.call !== 'number'
    || typeof value.potOdds !== 'number') {
    return false;
  }
  return optionalNumber(value.min)
    && optionalNumber(value.max)
    && optionalNumber(value.currentBet)
    && optionalString(value.amountMeaning)
    && (value.raiseTo === undefined || (
      isObject(value.raiseTo)
      && typeof value.raiseTo.min === 'number'
      && typeof value.raiseTo.max === 'number'
    ));
}

function isEvent(value: unknown): value is RiverEvent {
  return isObject(value)
    && typeof value.kind === 'string'
    && typeof value.text === 'string';
}

function isPot(value: unknown): value is RiverPot {
  return isObject(value)
    && typeof value.size === 'number'
    && Array.isArray(value.eligible)
    && value.eligible.every(seat => typeof seat === 'number');
}

function isSolver(value: unknown): value is RiverSolver {
  if (!isObject(value)) return false;
  const numericFields = [
    'playersInHand',
    'potBb',
    'effectiveStackBb',
    'spr',
    'toCallBb',
    'potOdds',
  ];
  return numericFields.every(field =>
    value[field] === undefined || typeof value[field] === 'number')
    && (value.street === undefined || isStreet(value.street))
    && optionalString(value.format)
    && optionalString(value.game)
    && (value.heroPosition === undefined || typeof value.heroPosition === 'string')
    && (value.raiseToBb === undefined
      || (Array.isArray(value.raiseToBb)
        && value.raiseToBb.every(amount => typeof amount === 'number')));
}

function isWinner(value: unknown): value is RiverWinner {
  return isObject(value)
    && optionalNumber(value.seat)
    && optionalNumber(value.amount)
    && optionalString(value.description);
}

function isNumberPair(value: unknown): value is [number, number] {
  return Array.isArray(value)
    && value.length === 2
    && typeof value[0] === 'number'
    && typeof value[1] === 'number';
}

function optionalString(value: unknown): boolean {
  return value === undefined || typeof value === 'string';
}

function optionalNumber(value: unknown): boolean {
  return value === undefined || typeof value === 'number';
}

function optionalBoolean(value: unknown): boolean {
  return value === undefined || typeof value === 'boolean';
}
