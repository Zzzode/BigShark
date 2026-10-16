import assert from 'node:assert/strict';
import test from 'node:test';

import {
  ObservationCoverage,
  ObservationSource,
} from '../../../build/generated/ts/bigshark/engine/v2/engine_pb.js';
import { ObservationLedger } from '../src/observation.js';

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

interface EventInit {
  kind: string;
  text: string;
}

interface RoomOverrides {
  handId?: string;
  revision?: number;
  events?: EventInit[];
  board?: Array<{ rank: string; suit: string }>;
}

/**
 * A parsed official CLI state response with a full room that passes the
 * adapter's room validation. The ledger ingests the parsed response, not the
 * room directly, so the envelope shape matters.
 */
function stateResponse(overrides: RoomOverrides = {}): unknown {
  const { handId = 'hand-1', revision = 0, events = [], board = [] } = overrides;
  return {
    ok: true,
    room: {
      id: 'room-1',
      handId,
      revision,
      street: 'preflop',
      blinds: [10, 20],
      hero: {
        seat: 0,
        name: 'hero',
        stack: 1990,
        cards: [
          { rank: 'A', suit: 'spades' },
          { rank: 'K', suit: 'spades' },
        ],
        status: 'active',
      },
      dealer: 1,
      board,
      pot: 30,
      seats: [
        { seat: 0, name: 'hero', stack: 1990, bet: 10, status: 'active', blind: 'SB' },
        { seat: 1, name: 'villain', stack: 1980, bet: 20, status: 'active', blind: 'BB' },
      ],
      events,
    },
  };
}

const ev = (kind: string, text: string): EventInit => ({ kind, text });

// ---------------------------------------------------------------------------
// Coverage
// ---------------------------------------------------------------------------

test('ledger: full observation from hand start yields FULL coverage', () => {
  const ledger = new ObservationLedger('epoch-test');
  ledger.ingest(stateResponse({ handId: 'h-full', revision: 0 }));
  ledger.ingest(stateResponse({
    handId: 'h-full',
    revision: 1,
    events: [ev('call', 'hero call 10')],
  }));

  const provenance = ledger.provenance();
  assert.equal(provenance.source, ObservationSource.OFFICIAL_RESPONSE);
  assert.equal(provenance.coverage, ObservationCoverage.FULL);
  assert.equal(provenance.observedEventCount, 1);
  assert.equal(provenance.controllerEpoch, 'epoch-test:0:h-full');
});

test('ledger: first observation with existing events is MID_HAND_ATTACH', () => {
  const ledger = new ObservationLedger('epoch-test');
  ledger.ingest(stateResponse({
    handId: 'h-mid',
    revision: 3,
    events: [ev('raise', 'villain raise to 60')],
  }));

  const provenance = ledger.provenance();
  assert.equal(provenance.coverage, ObservationCoverage.MID_HAND_ATTACH);
  assert.equal(provenance.observedEventCount, 1);
});

test('ledger: first observation at a non-zero revision is MID_HAND_ATTACH', () => {
  const ledger = new ObservationLedger('epoch-test');
  ledger.ingest(stateResponse({ handId: 'h-mid-rev', revision: 5 }));

  const provenance = ledger.provenance();
  assert.equal(provenance.coverage, ObservationCoverage.MID_HAND_ATTACH);
  assert.equal(provenance.observedEventCount, 0);
});

test('ledger: warmup gap before the first actionable state is MID_HAND_ATTACH', () => {
  const ledger = new ObservationLedger('epoch-test');
  // The runner's first observation is already an actionable mid-hand state;
  // the warmup interval is unobserved.
  ledger.ingest(stateResponse({
    handId: 'h-warmup',
    revision: 6,
    events: [
      ev('raise', 'villain raise to 60'),
      ev('call', 'hero call 40'),
    ],
  }));

  const provenance = ledger.provenance();
  assert.equal(provenance.coverage, ObservationCoverage.MID_HAND_ATTACH);
  assert.equal(provenance.observedEventCount, 2);
});

// ---------------------------------------------------------------------------
// Restart and epoch
// ---------------------------------------------------------------------------

test('ledger: controller restart changes the epoch and drops the old hand', () => {
  const ledger = new ObservationLedger('epoch-test');
  ledger.ingest(stateResponse({ handId: 'h-restart', revision: 0 }));
  const before = ledger.provenance();
  assert.equal(before.coverage, ObservationCoverage.FULL);

  ledger.markRestart();
  // Re-attach mid-hand after the restart.
  ledger.ingest(stateResponse({
    handId: 'h-restart',
    revision: 4,
    events: [ev('call', 'hero call 10')],
  }));

  const after = ledger.provenance();
  assert.notEqual(after.controllerEpoch, before.controllerEpoch);
  assert.equal(after.coverage, ObservationCoverage.MID_HAND_ATTACH);
  assert.equal(after.observedEventCount, 1);
  assert.equal(after.controllerEpoch, 'epoch-test:1:h-restart');
});

test('ledger: each ledger instance has a distinct restart-sensitive epoch base', () => {
  const first = new ObservationLedger();
  const second = new ObservationLedger();
  first.ingest(stateResponse({ handId: 'h-epoch', revision: 0 }));
  second.ingest(stateResponse({ handId: 'h-epoch', revision: 0 }));

  assert.notEqual(
    first.provenance().controllerEpoch,
    second.provenance().controllerEpoch,
  );
});

// ---------------------------------------------------------------------------
// Hand change
// ---------------------------------------------------------------------------

test('ledger: hand change resets the ledger', () => {
  const ledger = new ObservationLedger('epoch-test');
  ledger.ingest(stateResponse({
    handId: 'h-old',
    revision: 0,
    events: [ev('call', 'hero call 10')],
  }));
  assert.equal(ledger.provenance().observedEventCount, 1);

  ledger.ingest(stateResponse({ handId: 'h-new', revision: 0 }));

  const provenance = ledger.provenance();
  assert.equal(provenance.observedEventCount, 0);
  assert.equal(provenance.coverage, ObservationCoverage.FULL);
  assert.equal(provenance.controllerEpoch, 'epoch-test:0:h-new');
});

// ---------------------------------------------------------------------------
// Event counting
// ---------------------------------------------------------------------------

test('ledger: duplicate ingestion keeps the count honest', () => {
  const ledger = new ObservationLedger('epoch-test');
  const response = stateResponse({
    handId: 'h-dup',
    revision: 0,
    events: [ev('call', 'hero call 10')],
  });
  ledger.ingest(response);
  ledger.ingest(response);
  ledger.ingest(response);

  assert.equal(ledger.provenance().observedEventCount, 1);
});

test('ledger: overlapping event windows merge into one ordered stream', () => {
  const ledger = new ObservationLedger('epoch-test');
  ledger.ingest(stateResponse({ handId: 'h-merge', revision: 0 }));
  ledger.ingest(stateResponse({
    handId: 'h-merge',
    revision: 1,
    events: [ev('raise', 'villain raise to 60')],
  }));
  ledger.ingest(stateResponse({
    handId: 'h-merge',
    revision: 2,
    events: [
      ev('raise', 'villain raise to 60'),
      ev('call', 'hero call 40'),
      ev('check', 'villain check'),
    ],
  }));

  const provenance = ledger.provenance();
  assert.equal(provenance.coverage, ObservationCoverage.FULL);
  assert.equal(provenance.observedEventCount, 3);
});

test('ledger: board growth counts observed deal transitions', () => {
  const ledger = new ObservationLedger('epoch-test');
  ledger.ingest(stateResponse({ handId: 'h-deal', revision: 0 }));
  ledger.ingest(stateResponse({
    handId: 'h-deal',
    revision: 2,
    events: [
      ev('raise', 'villain raise to 60'),
      ev('call', 'hero call 40'),
    ],
    board: [
      { rank: '2', suit: 'clubs' },
      { rank: '3', suit: 'diamonds' },
      { rank: '7', suit: 'hearts' },
    ],
  }));

  // Two player actions plus the observed flop deal: the same transition
  // count the mapper emits (two ActionEvents + one BoardDeal).
  const provenance = ledger.provenance();
  assert.equal(provenance.coverage, ObservationCoverage.FULL);
  assert.equal(provenance.observedEventCount, 3);
});

test('ledger: a board present at attach time is not an observed deal', () => {
  const ledger = new ObservationLedger('epoch-test');
  ledger.ingest(stateResponse({
    handId: 'h-attach-board',
    revision: 4,
    events: [ev('check', 'hero check')],
    board: [
      { rank: '2', suit: 'clubs' },
      { rank: '3', suit: 'diamonds' },
      { rank: '7', suit: 'hearts' },
    ],
  }));

  const provenance = ledger.provenance();
  assert.equal(provenance.coverage, ObservationCoverage.MID_HAND_ATTACH);
  assert.equal(provenance.observedEventCount, 1);
});

test('ledger: unalignable event windows downgrade coverage and keep both counts', () => {
  const ledger = new ObservationLedger('epoch-test');
  ledger.ingest(stateResponse({ handId: 'h-gap', revision: 0 }));
  ledger.ingest(stateResponse({
    handId: 'h-gap',
    revision: 1,
    events: [
      ev('raise', 'villain raise to 60'),
      ev('call', 'hero call 40'),
    ],
  }));
  // The window turned over without any overlapping event: continuity is
  // unprovable, so the hand is MID_HAND_ATTACH from here on.
  ledger.ingest(stateResponse({
    handId: 'h-gap',
    revision: 9,
    events: [
      ev('bet', 'hero bet to 100'),
      ev('call', 'villain call 100'),
    ],
  }));

  const provenance = ledger.provenance();
  assert.equal(provenance.coverage, ObservationCoverage.MID_HAND_ATTACH);
  assert.equal(provenance.observedEventCount, 4);
});

// ---------------------------------------------------------------------------
// Heartbeats and non-room responses
// ---------------------------------------------------------------------------

test('ledger: heartbeat responses are ingested without creating a gap', () => {
  const ledger = new ObservationLedger('epoch-test');
  ledger.ingest(stateResponse({ handId: 'h-beat', revision: 0 }));
  // A timed wait with no update returns only changed/serverTime and a room
  // reference (id/revision), never a full room.
  ledger.ingest({
    changed: false,
    serverTime: 1_700_000_000_000,
    room: { id: 'room-1', revision: 1 },
  });
  ledger.ingest(stateResponse({
    handId: 'h-beat',
    revision: 2,
    events: [ev('check', 'hero check')],
  }));

  const provenance = ledger.provenance();
  assert.equal(provenance.coverage, ObservationCoverage.FULL);
  assert.equal(provenance.observedEventCount, 1);
});

test('ledger: responses without a room are ignored', () => {
  const ledger = new ObservationLedger('epoch-test');
  ledger.ingest({ ok: false, error: 'RATE_LIMITED', code: 'RATE_LIMITED' });
  ledger.ingest({ rooms: [] });
  ledger.ingest(null);
  ledger.ingest('not json');

  const provenance = ledger.provenance('h-never-observed');
  assert.equal(provenance.coverage, ObservationCoverage.MID_HAND_ATTACH);
  assert.equal(provenance.observedEventCount, 0);
});

// ---------------------------------------------------------------------------
// Fail-closed provenance
// ---------------------------------------------------------------------------

test('ledger: provenance for an unobserved hand fails closed', () => {
  const ledger = new ObservationLedger('epoch-test');
  ledger.ingest(stateResponse({ handId: 'h-observed', revision: 0 }));

  const provenance = ledger.provenance('h-some-other-hand');
  assert.equal(provenance.source, ObservationSource.OFFICIAL_RESPONSE);
  assert.equal(provenance.coverage, ObservationCoverage.MID_HAND_ATTACH);
  assert.equal(provenance.observedEventCount, 0);
  assert.ok(provenance.controllerEpoch.includes('h-some-other-hand'));
});

test('ledger: provenance with no observations at all fails closed', () => {
  const ledger = new ObservationLedger('epoch-test');
  const provenance = ledger.provenance();
  assert.equal(provenance.coverage, ObservationCoverage.MID_HAND_ATTACH);
  assert.equal(provenance.observedEventCount, 0);
  assert.ok(provenance.controllerEpoch.length > 0);
});
