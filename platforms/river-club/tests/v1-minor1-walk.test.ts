// Core regression for adversarial review #1/#2: production toV1DecisionRequest
// must populate postflop chip amounts (BET/RAISE target_total, CALL
// incremental_amount) from the localized River event text, and the resulting
// requests must hit real resident blueprint rows AFTER postflop aggression —
// not only at the flop-open node. Walks the published artifact's tree using
// only the distributions the real engine returns, then asserts:
//   1) flop check -> opponent bet -> hero FACING the bet hits BLUEPRINT;
//   2) a flop check/check -> turn bet/call walk hits at the river root;
//   3) a river facing-raise node hits BLUEPRINT.
// Mutation control: with amount filling stripped the same walks become
// UNSUPPORTED_FEATURE.
import assert from 'node:assert/strict';
import { create } from '@bufbuild/protobuf';
import { spawnSync } from 'node:child_process';
import { existsSync, mkdtempSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { dirname, join, resolve } from 'node:path';
import test from 'node:test';
import { fileURLToPath } from 'node:url';

import { ProtoEngineProcessClient } from '../../../clients/node/proto-engine-process-client.js';
import {
  ActionType,
  EnvelopeSchema,
  GetCapabilitiesRequestSchema,
  SolverMode,
  SolverSource,
  type DecisionRequest,
  type Envelope,
} from '../../../build/generated/ts/bigshark/engine/v1/engine_pb.js';
import { toV1DecisionRequest } from '../src/v1-mapper.js';
import type { RiverEvent, RiverRoom } from '../src/types.js';

const here = dirname(fileURLToPath(import.meta.url));
const root = resolve(here, '../../../../');
const engineBinary = process.env.BIGSHARK_ENGINE_BINARY
  || join(root, 'bin', 'bigshark-engine');
const fixturePublisher = process.env.BS_RESIDENT_FIXTURE_BIN
  || join(root, 'bin', 'bs-resident-fixture');

interface PublishedFixture { path: string; sha256: string }

function publishFixture(): PublishedFixture | null {
  if (!existsSync(engineBinary) || !existsSync(fixturePublisher))
    return null;
  const dir = mkdtempSync(join(tmpdir(), 'bs-stage8-walk-'));
  const out = spawnSync(fixturePublisher, [dir], { encoding: 'utf8' });
  if (out.status !== 0)
    throw new Error(`fixture publisher failed: ${out.stderr}`);
  return JSON.parse(out.stdout.trim()) as PublishedFixture;
}

const fixture = publishFixture();

// Fixture root: flop 2c 3d 7h, fixed turn 9h river 8s, BB 5, pot 20 (10/10
// matched preflop), button on seat 1, stacks 40/40. Seat 0 (non-button)
// acts first postflop and holds AsKs/QhJh; seat 1 holds AcKc/QdJd.
const rc = (rank: string, suit: string) => ({ rank, suit });
const HERO_CARDS: Record<0 | 1, ReturnType<typeof rc>[]> = {
  0: [rc('A', 'spades'), rc('K', 'spades')],
  1: [rc('A', 'clubs'), rc('K', 'clubs')],
};
const BOARD: Record<'flop' | 'turn' | 'river', ReturnType<typeof rc>[]> = {
  flop: [rc('2', 'clubs'), rc('3', 'diamonds'), rc('7', 'hearts')],
  turn: [rc('2', 'clubs'), rc('3', 'diamonds'), rc('7', 'hearts'), rc('9', 'hearts')],
  river: [rc('2', 'clubs'), rc('3', 'diamonds'), rc('7', 'hearts'), rc('9', 'hearts'),
          rc('8', 'spades')],
};

interface Walk {
  street: 'flop' | 'turn' | 'river';
  events: RiverEvent[];
  stacks: [number, number];
  streetCom: [number, number];
  pot: number;
}

function startWalk(): Walk {
  // Production snapshots always carry the preflop close-out. The fixture root
  // is a 10/10 matched pot with a 5 big blind (a BB straddle): the BB (seat
  // 1) makes it 10 and the SB (seat 0) calls 8 more, closing preflop at 20.
  // The token-round grouper needs these events for correct street tags.
  return {
    street: 'flop',
    events: [
      { kind: 'raise', text: `${seatName(1)} raise to 10` },
      { kind: 'call', text: `${seatName(0)} call 8` },
    ],
    stacks: [40, 40],
    streetCom: [0, 0],
    pot: 20,
  };
}

const seatName = (seat: 0 | 1): string => (seat === 0 ? 'alpha' : 'bravo');

function addEvent(walk: Walk, seat: 0 | 1,
                  kind: 'bet' | 'raise' | 'call' | 'check', target?: number): void {
  const name = seatName(seat);
  const text = kind === 'check'
    ? `${name} check`
    : kind === 'bet'
      ? `${name} bet to ${target}`
      : kind === 'raise'
        ? `${name} raise to ${target}`
        : `${name} call ${target}`;
  walk.events.push({ kind, text });
  if (kind === 'check') {
    // x/x closes the street when commitments are matched.
    if (walk.streetCom[0] === walk.streetCom[1]
        && walk.events.slice(-2).every(e => e.kind === 'check')) {
      walk.streetCom = [0, 0];
      walk.street = walk.street === 'flop' ? 'turn' : 'river';
    }
    return;
  }
  const due = Math.max(walk.streetCom[0], walk.streetCom[1]) - walk.streetCom[seat];
  const payment = kind === 'call'
    ? Math.min(due, walk.stacks[seat])
    : target! - walk.streetCom[seat];
  walk.stacks[seat] -= payment;
  walk.streetCom[seat] += payment;
  walk.pot += payment;
  if (kind === 'call') {
    // A call matches commitments and closes the street.
    walk.streetCom = [0, 0];
    walk.street = walk.street === 'flop' ? 'turn' : 'river';
  }
}

function buildRoom(walk: Walk, heroSeat: 0 | 1): RiverRoom {
  const toCall = Math.max(walk.streetCom[0], walk.streetCom[1])
    - walk.streetCom[heroSeat];
  const aggressiveMax = walk.stacks[heroSeat] + walk.streetCom[heroSeat];
  const legal = toCall === 0
    ? {
        actions: ['check', 'bet'] as const,
        call: 0,
        potOdds: 0,
        currentBet: 0,
        amountMeaning: 'target-total' as const,
        min: 1,
        max: walk.stacks[heroSeat],
        raiseTo: { min: 1, max: walk.stacks[heroSeat] },
      }
    : {
        actions: ['fold', 'call', 'raise'] as const,
        call: toCall,
        potOdds: toCall / (walk.pot + toCall),
        currentBet: toCall,
        amountMeaning: 'target-total' as const,
        min: 1,
        max: aggressiveMax,
        raiseTo: { min: 1, max: aggressiveMax },
      };
  return {
    id: 'walk-room',
    revision: 1,
    handId: 'walk-hand',
    street: walk.street,
    blinds: [2, 5],
    hero: {
      seat: heroSeat,
      name: seatName(heroSeat),
      stack: walk.stacks[heroSeat],
      bet: walk.streetCom[heroSeat],
      status: 'active',
      cards: HERO_CARDS[heroSeat] as never,
    },
    dealer: 1,
    board: BOARD[walk.street] as never,
    pot: walk.pot,
    pots: [{ size: walk.pot, eligible: [0, 1] }],
    seats: ([0, 1] as const).map(seat => ({
      seat,
      name: seatName(seat),
      stack: walk.stacks[seat],
      bet: walk.streetCom[seat],
      status: 'active' as const,
      blind: seat === 1 ? 'BB' : 'SB',
      position: seat === 1 ? 'BTN' : 'BB',
    })),
    legal: legal as never,
    events: walk.events,
  } as unknown as RiverRoom;
}

function decisionFrame(request: DecisionRequest): Envelope {
  const envelope = create(EnvelopeSchema);
  envelope.protocolMinor = 1;
  envelope.requestId = `walk-${Math.random()}`;
  envelope.payload = { case: 'decisionRequest', value: request };
  return envelope;
}

function capabilitiesEnvelope(): Envelope {
  const envelope = create(EnvelopeSchema);
  envelope.protocolMinor = 0;
  envelope.requestId = `warm-${Math.random()}`;
  envelope.payload = {
    case: 'getCapabilitiesRequest',
    value: create(GetCapabilitiesRequestSchema),
  };
  return envelope;
}

function residentClient(published: PublishedFixture): ProtoEngineProcessClient {
  return new ProtoEngineProcessClient({
    command: engineBinary,
    args: ['--serve-proto', '--resident-root', `${published.path}=${published.sha256}`],
    warmupEnvelope: capabilitiesEnvelope(),
    warmupTimeoutMs: 10_000,
    negotiateMinor1: true,
  });
}

function blueprintRequest(walk: Walk, heroSeat: 0 | 1, stripAmounts = false):
    DecisionRequest {
  const room = buildRoom(walk, heroSeat);
  const request = toV1DecisionRequest(room, { solverMode: SolverMode.BLUEPRINT });
  request.options!.solverMode = SolverMode.BLUEPRINT;
  request.options!.seed = 42n;
  if (stripAmounts) {
    // Postflop ActionEvents are FLOP(2)..RIVER(4).
    for (const event of request.state!.actionHistory) {
      if (event.street >= 2) {
        event.targetTotal = undefined;
        event.incrementalAmount = undefined;
      }
    }
  }
  return request;
}

async function ask(client: ProtoEngineProcessClient, walk: Walk,
                   heroSeat: 0 | 1, stripAmounts = false): Promise<Envelope> {
  return client.request(
    decisionFrame(blueprintRequest(walk, heroSeat, stripAmounts)), 10_000);
}

function assertBlueprintHit(response: Envelope, label: string): void {
  assert.equal(response.protocolMinor, 1, `${label}: minor 1`);
  assert.equal(response.payload.case, 'decisionResponse', `${label}: decision frame`);
  if (response.payload.case !== 'decisionResponse') throw new Error('unreachable');
  const result = response.payload.value.result;
  if (result.case === 'error')
    process.stderr.write(`[${label}] engine error: ${JSON.stringify(result.value)}\n`);
  assert.equal(result.case, 'expandedStrategy',
    `${label}: expected expanded blueprint, got ${result.case}`);
  if (result.case !== 'expandedStrategy') throw new Error('unreachable');
  const expanded = result.value;
  assert.ok(expanded.actions.length > 0, `${label}: nonempty distribution`);
  assert.equal(expanded.solver?.source, SolverSource.BLUEPRINT,
    `${label}: source BLUEPRINT`);
  assert.equal(expanded.solver?.artifactSha256, fixture?.sha256,
    `${label}: artifact digest`);
  assert.equal(expanded.solver?.guarantee, 'uncertified', `${label}: guarantee`);
}

// Smallest target of the given kind; falls back to the all-in target if that is
// the only abstract aggression at the node.
function pickAction(response: Envelope, kind: ActionType): number | undefined {
  if (response.payload.case !== 'decisionResponse'
    || response.payload.value.result.case !== 'expandedStrategy')
    throw new Error('expected expanded strategy');
  const candidates = response.payload.value.result.value.actions
    .filter(a => a.type === kind && (a.targetTotal ?? 0n) > 0n)
    .sort((a, b) => Number((a.targetTotal ?? 0n) - (b.targetTotal ?? 0n)));
  const first = candidates[0]?.targetTotal;
  return first === undefined ? undefined : Number(first);
}

test('production postflop amounts: flop check -> opponent bet -> hero faces bet hits BLUEPRINT',
  { skip: fixture === null ? 'engine/fixture unavailable' : false },
  async t => {
    if (!fixture) return;
    const client = residentClient(fixture);
    t.after(() => client.stop());

    const walk = startWalk();
    addEvent(walk, 0, 'check');
    const seat1Node = await ask(client, walk, 1);
    assertBlueprintHit(seat1Node, 'seat1 after seat0 check');
    const betTarget = pickAction(seat1Node, ActionType.BET);
    assert.ok(betTarget, 'seat1 has an abstract bet');

    addEvent(walk, 1, 'bet', betTarget);
    const facing = await ask(client, walk, 0);
    assertBlueprintHit(facing, 'hero facing flop bet after check->bet');
  });

test('production postflop amounts: flop check/check then turn bet/call reaches a river hit',
  { skip: fixture === null ? 'engine/fixture unavailable' : false },
  async t => {
    if (!fixture) return;
    const client = residentClient(fixture);
    t.after(() => client.stop());

    const walk = startWalk();
    addEvent(walk, 0, 'check');
    addEvent(walk, 1, 'check');
    assert.equal(walk.street, 'turn', 'check/check deals the turn');

    const turnSeat0 = await ask(client, walk, 0);
    assertBlueprintHit(turnSeat0, 'turn root seat0');
    const turnBet = pickAction(turnSeat0, ActionType.BET);
    assert.ok(turnBet, 'turn abstract bet');
    addEvent(walk, 0, 'bet', turnBet);

    const due = Math.max(...walk.streetCom);
    addEvent(walk, 1, 'call', due);
    assert.equal(walk.street, 'river', 'turn call deals the river');

    const river = await ask(client, walk, 0);
    assertBlueprintHit(river, 'river root after flop x/x and turn bet/call');
  });

test('production postflop amounts: river facing-raise hits BLUEPRINT',
  { skip: fixture === null ? 'engine/fixture unavailable' : false },
  async t => {
    if (!fixture) return;
    const client = residentClient(fixture);
    t.after(() => client.stop());

    const walk = startWalk();
    addEvent(walk, 0, 'check'); addEvent(walk, 1, 'check');
    addEvent(walk, 0, 'check'); addEvent(walk, 1, 'check');
    assert.equal(walk.street, 'river');

    const riverSeat0 = await ask(client, walk, 0);
    const riverBet = pickAction(riverSeat0, ActionType.BET);
    assert.ok(riverBet, 'river abstract bet');
    addEvent(walk, 0, 'bet', riverBet);

    const seat1Facing = await ask(client, walk, 1);
    assertBlueprintHit(seat1Facing, 'seat1 facing river bet');
    const raiseTarget = pickAction(seat1Facing, ActionType.RAISE);
    assert.ok(raiseTarget, 'seat1 has an abstract raise');
    addEvent(walk, 1, 'raise', raiseTarget);

    const heroFacingRaise = await ask(client, walk, 0);
    assertBlueprintHit(heroFacingRaise, 'hero facing river raise');
  });

test('mutation: stripping postflop amounts turns a forced BLUEPRINT walk into a coverage miss',
  { skip: fixture === null ? 'engine/fixture unavailable' : false },
  async t => {
    if (!fixture) return;
    const client = residentClient(fixture);
    t.after(() => client.stop());

    const walk = startWalk();
    addEvent(walk, 0, 'check');
    const seat1Node = await ask(client, walk, 1);
    const betTarget = pickAction(seat1Node, ActionType.BET)!;
    addEvent(walk, 1, 'bet', betTarget);

    const stripped = await ask(client, walk, 0, true);
    if (stripped.payload.case !== 'decisionResponse') throw new Error('bad payload');
    assert.equal(stripped.payload.value.result.case, 'error',
      'without postflop amounts forced BLUEPRINT must miss');
    if (stripped.payload.value.result.case !== 'error') throw new Error('unreachable');
    assert.equal(stripped.payload.value.result.value.retryable, false);
  });

test('rounded current-street display recovers the EXACT structured amount and still hits',
  { skip: fixture === null ? 'engine/fixture unavailable' : false },
  async t => {
    if (!fixture) return;
    const client = residentClient(fixture);
    t.after(() => client.stop());

    const walk = startWalk();
    addEvent(walk, 0, 'check');
    const seat1Node = await ask(client, walk, 1);
    // Pick a non-jam bet so the bettor stays active (the jam would trip the
    // all-in reject at the facing node).
    const betTarget = pickAction(seat1Node, ActionType.BET)!;
    assert.ok(betTarget < 40, 'choose a non-jam abstract target');

    // Apply the bet to the walk ledger, then overwrite its text with a
    // K-rounded display. The exact integer parser must reject the text
    // (decimal + K), and current-street structured recovery must supply
    // seat.bet = betTarget instead.
    addEvent(walk, 1, 'bet', betTarget);
    walk.events[walk.events.length - 1] = {
      kind: 'bet',
      text: `${seatName(1)} bet to ${betTarget / 1000}K`,
    };

    const request = blueprintRequest(walk, 0);
    const currentStreetEvents = request.state!.actionHistory.filter(
      e => e.street === 2 /* FLOP */ && e.actorPlayerId === 'seat-1'
        && e.action === ActionType.BET);
    assert.equal(currentStreetEvents.length, 1);
    assert.equal(currentStreetEvents[0]!.targetTotal, BigInt(betTarget),
      'wire target is the exact structured commitment, never the rounded text');

    const response = await client.request(decisionFrame(request), 10_000);
    assertBlueprintHit(response, 'facing bet recovered from structured seat data');
  });

test('adapter never sends a rounded K value as exact: 1.2K display with 1225 structured', () => {
  // Production hand class: opponent bets a half pot 1225, displayed "1.2K".
  // The wire must carry 1225 (structured), never the rounded 1200. This test
  // fails under the old suffix-scaling parser (which returned 1200).
  const room = {
    id: 'rounded-room',
    revision: 1,
    handId: 'rounded-hand',
    street: 'turn',
    blinds: [10, 20],
    dealer: 1,
    pot: 3675,
    pots: [{ size: 3675, eligible: [0, 1] }],
    hero: {
      seat: 0,
      name: 'hero',
      stack: 8000,
      bet: 0,
      status: 'active',
      cards: [{ rank: 'A', suit: 'spades' }, { rank: 'K', suit: 'spades' }],
    },
    board: [
      { rank: '2', suit: 'clubs' }, { rank: '3', suit: 'diamonds' },
      { rank: '7', suit: 'hearts' }, { rank: '9', suit: 'hearts' },
    ],
    seats: [
      { seat: 0, name: 'hero', stack: 8000, bet: 0, status: 'active' as const },
      { seat: 1, name: 'yao', stack: 8000, bet: 1225, status: 'active' as const },
    ],
    legal: {
      actions: ['fold', 'call', 'raise'] as const,
      call: 1225,
      potOdds: 1225 / (3675 + 1225),
      currentBet: 1225,
      amountMeaning: 'target-total' as const,
      min: 1,
      max: 9225,
      raiseTo: { min: 1, max: 9225 },
    },
    events: [
      // Preflop (group 0): BB makes it 40, SB calls 20.
      { kind: 'raise', text: 'yao raise to 40' },
      { kind: 'call', text: 'hero call 20' },
      // Flop (group 1): check/check.
      { kind: 'check', text: 'hero check' },
      { kind: 'check', text: 'yao check' },
      // Turn (group 2): hero checks, yao bets the rounded-display 1225.
      { kind: 'check', text: 'hero check' },
      { kind: 'bet', text: 'yao bet to 1.2K' },
    ],
  } as unknown as RiverRoom;

  const request = toV1DecisionRequest(room, { solverMode: SolverMode.BLUEPRINT });
  const betEvents = request.state!.actionHistory.filter(
    e => e.action === ActionType.BET && e.actorPlayerId === 'seat-1');
  assert.equal(betEvents.length, 1);
  assert.notEqual(betEvents[0]!.targetTotal, 1200n, 'rounded 1200 must never be emitted');
  assert.equal(betEvents[0]!.targetTotal, 1225n, 'exact structured 1225 is recovered');
});

