import { describe, it } from 'node:test';
import assert from 'node:assert/strict';
import type { RiverRoom, RiverSeat, Action } from '../src/types.js';
import {
  summarizeRootDemand,
  isFlopStreetStart,
  classifyPreflopFamily,
  type JournalEntry,
} from '../src/root-histogram.js';

const rc = (rank: string, suit: string) => ({ rank, suit });

function seat(n: number, overrides: Partial<RiverSeat> = {}): RiverSeat {
  return { seat: n, name: `Player${n}`, stack: 1980, bet: 0, status: 'active', ...overrides };
}

const flopLegal = {
  actions: ['check', 'bet'] as Action[],
  call: 0,
  raiseTo: { min: 20, max: 1980 },
  potOdds: 0,
};

// Six-handed limped flop street-start: 10/20 blinds, every seat posts 20,
// pot 120, then the flop deal. Mirrors the production six-way limped root.
function limpedSixHand(handId: string, opts: { withFold?: boolean } = {}): RiverRoom {
  const folded = opts.withFold;
  return {
    id: 'room',
    handId,
    revision: 1,
    street: 'flop',
    next: 'act',
    blinds: [10, 20],
    dealer: 2,
    board: [rc('2', 'clubs'), rc('3', 'diamonds'), rc('7', 'hearts')],
    pot: folded ? 100 : 120,
    hero: { seat: 4, cards: [rc('A', 'spades'), rc('K', 'spades')], stack: 1980, status: 'active', effectiveStackBb: 100 },
    seats: [
      seat(0, { bet: 0, stack: 1980 }),
      seat(1, { bet: 0, stack: 1980 }),
      seat(2, { bet: 0, stack: 1980 }), // button
      seat(3, { blind: 'SB', bet: 0, stack: 1980 }),
      seat(4, { blind: 'BB', bet: 0, stack: 1980 }),
      folded
        ? seat(5, { bet: 0, stack: 2000, status: 'folded' })
        : seat(5, { bet: 0, stack: 1980 }),
    ],
    legal: flopLegal,
    events: [
      { kind: 'call', text: 'Player5 call 20' },
      { kind: 'call', text: 'Player0 call 20' },
      { kind: 'call', text: 'Player1 call 20' },
      { kind: 'call', text: 'Player2 call 20' },
      { kind: 'call', text: 'Player3 call 10' },
      { kind: 'check', text: 'Player4 check' },
      ...(folded ? [{ kind: 'fold', text: 'Player5 fold' }] : []),
      { kind: 'deal', text: 'Flop deal' },
    ],
  };
}

function entry(room: RiverRoom | null): JournalEntry {
  return { ts: '2026-10-25T10:00:00Z', argv: ['state'], resp: room ? { room } : null };
}

describe('RFC 0035 root demand histogram', () => {
  it('counts a limped six-handed flop street start with the exact ledger', () => {
    const h = summarizeRootDemand([entry(limpedSixHand('h1'))]);
    assert.equal(h.journalEntries, 1);
    assert.equal(h.flopStreetStart, 1);
    assert.equal(h.ledgerMappable, 1);
    assert.equal(h.ledgerIncomplete, 0);
    assert.equal(h.distinctHands, 1);
    assert.equal(h.exactTuples.length, 1);
    const cell = h.exactTuples[0]!;
    assert.equal(cell.family, 'limped');
    assert.equal(cell.count, 1);
    assert.equal(cell.key.playerCount, 6);
    assert.equal(cell.key.pot, 120);
    assert.equal(cell.key.bigBlind, 20);
    assert.equal(cell.key.liveCount, 6);
    // Position order starts at SB (seat 3), then BB (seat 4), around the ring.
    assert.equal(cell.key.positions[0]!.pos, 'SB');
    assert.equal(cell.key.positions[0]!.seat, 3);
    assert.equal(cell.key.positions[1]!.pos, 'BB');
    assert.equal(cell.key.positions[1]!.seat, 4);
    assert.equal(cell.key.positions.at(-1)!.pos, 'BTN');
    assert.equal(cell.key.positions.at(-1)!.seat, 2);
    assert.ok(cell.key.positions.every(p => p.net === 20));
    assert.ok(cell.key.positions.every(p => p.stackBehind === 1980));
  });

  it('dedupes repeated state polls of the same hand to one demand row', () => {
    const room = limpedSixHand('h1');
    const h = summarizeRootDemand([entry(room), entry(room), entry(room)]);
    assert.equal(h.roomSnapshots, 3);
    assert.equal(h.flopStreetStart, 3);
    assert.equal(h.distinctHands, 1);
    assert.equal(h.ledgerMappable, 1);
    assert.equal(h.exactTuples.length, 1);
    assert.equal(h.exactTuples[0]!.count, 1);
  });

  it('records a folded seat in the mask and live count', () => {
    const h = summarizeRootDemand([entry(limpedSixHand('hf', { withFold: true }))]);
    const cell = h.exactTuples[0]!;
    assert.equal(cell.key.liveCount, 5);
    const folded = cell.key.positions.find(p => p.seat === 5)!;
    assert.equal(folded.folded, true);
  });

  it('classifies an SRP (one preflop raise) and a 3-bet (two raises)', () => {
    const srp = limpedSixHand('srp');
    srp.events = [
      { kind: 'fold', text: 'Player3 fold' },
      { kind: 'raise', text: 'Player0 raise to 60' },
      { kind: 'call', text: 'Player4 call 40' },
      { kind: 'fold', text: 'Player5 fold' },
      { kind: 'fold', text: 'Player1 fold' },
      { kind: 'fold', text: 'Player2 fold' },
      { kind: 'deal', text: 'Flop deal' },
    ];
    srp.pot = 120;
    assert.equal(classifyPreflopFamily(srp), 'srp');
    srp.events.splice(1, 0, { kind: 'raise', text: 'Player4 raise to 180' });
    assert.equal(classifyPreflopFamily(srp), '3bet');
  });

  it('is not street start once any flop action follows the deal', () => {
    const room = limpedSixHand('h2');
    room.events!.push({ kind: 'check', text: 'Player3 check' });
    assert.equal(isFlopStreetStart(room), false);
    assert.equal(summarizeRootDemand([entry(room)]).flopStreetStart, 0);
  });

  it('counts amount-incomplete snapshots as ledgerIncomplete, never a fake key', () => {
    // The synthetic-rooms fixture uses text-only events with no recoverable
    // amounts; the production mapper throws HISTORY_INCOMPLETE.
    const incomplete = limpedSixHand('h3');
    incomplete.events = [
      { kind: 'call', text: 'Player5 call' },
      { kind: 'deal', text: 'Flop deal' },
    ];
    const h = summarizeRootDemand([entry(incomplete)]);
    assert.equal(h.ledgerMappable, 0);
    assert.equal(h.ledgerIncomplete, 1);
    assert.ok(Object.keys(h.ledgerIncompleteReasons).length > 0);
    assert.equal(h.exactTuples.length, 0);
  });

  it('reports an empty corpus honestly (the current production state)', () => {
    const h = summarizeRootDemand([]);
    assert.equal(h.journalEntries, 0);
    assert.equal(h.ledgerMappable, 0);
    assert.equal(h.exactTuples.length, 0);
  });

  it('ignores non-room and non-flop journal entries', () => {
    const preflop = limpedSixHand('hp');
    preflop.street = 'preflop';
    const h = summarizeRootDemand([
      entry(preflop),
      entry(null),
      { ts: 'x', argv: ['rooms'], resp: { rooms: [] } },
    ]);
    assert.equal(h.roomSnapshots, 0);
    assert.equal(h.ledgerMappable, 0);
  });
});
