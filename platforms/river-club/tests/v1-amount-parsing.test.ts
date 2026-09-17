// Unit tests for the postflop action amount boundary (adversarial review
// #1 round 2): exact integer text is trusted; K/M/B rounded display is never
// emitted as an exact chip target.
import assert from 'node:assert/strict';
import test from 'node:test';

import { parseExactChipAmount, toV1DecisionRequest } from '../src/v1-mapper.js';
import { SolverMode } from '../../../build/generated/ts/bigshark/engine/v1/engine_pb.js';
import type { RiverRoom } from '../src/types.js';

test('parseExactChipAmount trusts plain exact integer tokens', () => {
  assert.equal(parseExactChipAmount('alpha bet to 980'), 980n);
  assert.equal(parseExactChipAmount('alpha bet to 1225'), 1225n);
  assert.equal(parseExactChipAmount('bravo call 40'), 40n);
  assert.equal(parseExactChipAmount('Chen Meng  40'), 40n);
  assert.equal(parseExactChipAmount('980'), 980n);
});

test('parseExactChipAmount rejects rounded K/M/B and decimal display', () => {
  for (const text of [
    'Yao bet to 1.2K',
    'alpha bet to 1.2K',
    'bet 2.5K',
    'raise 1K',
    'ALL IN 1.0K',
    'ALL IN 2.5K',
    'jam 3.6K',
    'bet 1.5M',
    'bet 2B',
    'bet 70.5',
    'check',
    '',
    'bet abc',
  ]) {
    assert.equal(parseExactChipAmount(text), null, `must not trust "${text}"`);
  }
});

// Preflop structured CALL recovery must subtract the posted blind: the C++
// ledger seeds preflop street-paid from forced contributions, and the
// structured recovery of a blind-posting actor's rounded-display final call
// must report only the NEW chips, never the blind-inclusive cumulative total.
// Preflop BLUEPRINT is currently inert (Stage 10 activates it); these pin the
// boundary now so the double-count ships without a known bug.
function preflopRoom(extra: {
  heroSeat: number;
  heroName: string;
  heroBet: number;
  seats: Array<{seat:number;name:string;stack:number;bet:number;blind?:string}>;
  events: Array<{kind:string;text:string}>;
}): RiverRoom {
  return {
    id: 'p', revision: 1, handId: 'ph', street: 'preflop',
    blinds: [10, 20], dealer: extra.seats.length - 1, pot: 200,
    pots: [{ size: 200, eligible: extra.seats.map(s => s.seat) }],
    hero: {
      seat: extra.heroSeat, name: extra.heroName, stack: 1000,
      bet: extra.heroBet, status: 'active',
      cards: [{ rank: 'A', suit: 'spades' }, { rank: 'K', suit: 'spades' }],
    },
    board: [],
    seats: extra.seats.map(s => ({ status: 'active' as const, ...s })),
    legal: {
      actions: ['fold', 'call', 'raise'] as const, call: 80, potOdds: 0.2,
      currentBet: 100, amountMeaning: 'target-total' as const, min: 1,
      max: 1100, raiseTo: { min: 1, max: 1100 },
    },
    events: extra.events,
  } as unknown as RiverRoom;
}

test('preflop structured call recovery: SB limp increment excludes the SB blind (90 not 100)', () => {
  // Blinds 10/20; BB makes it 100 (exact text), SB flat-calls with a rounded
  // display; SB seat.bet is the cumulative 100. New money = 100 - 10 = 90.
  const room = preflopRoom({
    heroSeat: 0, heroName: 'sb', heroBet: 100,
    seats: [
      { seat: 0, name: 'sb', stack: 1000, bet: 100, blind: 'SB' },
      { seat: 1, name: 'bb', stack: 1000, bet: 100, blind: 'BB' },
    ],
    events: [
      { kind: 'raise', text: 'bb raise to 100' },
      { kind: 'call', text: 'sb call 0.1K' },  // rounded -> structured recovery
    ],
  });
  const request = toV1DecisionRequest(room, { solverMode: SolverMode.BLUEPRINT });
  const sbCall = request.state!.actionHistory.find(
    e => e.actorPlayerId === 'seat-0' && e.action === 3 /* CALL */);
  assert.ok(sbCall, 'SB call event present');
  assert.equal(sbCall!.incrementalAmount, 90n, 'SB limp is 90 new chips, not 100');
  assert.equal(sbCall!.targetTotal, undefined, 'call carries no target total');
});

test('preflop structured call recovery: BB call increment excludes the BB blind (20 not 40)', () => {
  // Blinds 10/20; an opener makes it 40 (exact text), BB flat-calls with a
  // rounded display; BB seat.bet is cumulative 40. New money = 40 - 20 = 20.
  const room = preflopRoom({
    heroSeat: 1, heroName: 'bb', heroBet: 40,
    seats: [
      { seat: 0, name: 'sb', stack: 1000, bet: 40, blind: 'SB' },
      { seat: 1, name: 'bb', stack: 1000, bet: 40, blind: 'BB' },
    ],
    events: [
      { kind: 'raise', text: 'sb raise to 40' },
      { kind: 'call', text: 'bb call 0.04K' },  // rounded -> structured recovery
    ],
  });
  const request = toV1DecisionRequest(room, { solverMode: SolverMode.BLUEPRINT });
  const bbCall = request.state!.actionHistory.find(
    e => e.actorPlayerId === 'seat-1' && e.action === 3 /* CALL */);
  assert.ok(bbCall, 'BB call event present');
  assert.equal(bbCall!.incrementalAmount, 20n, 'BB completion/call is 20 new chips, not 40');
});
