import assert from 'node:assert/strict';
import { create } from '@bufbuild/protobuf';
import test from 'node:test';

import {
  ActionType,
  DecisionResponseSchema,
  EngineErrorSchema,
  EnvelopeSchema,
  ErrorCode,
  ExpandedStrategySchema,
  GetCapabilitiesRequestSchema,
  LayerStatus,
  ObservationCoverage,
  ObservationSource,
  PlayerStatus,
  PotPhase,
  SolverSource,
  Street,
  StrategySchema,
  UtilityProfile,
} from '../../../build/generated/ts/bigshark/engine/engine_pb.js';
import {
  decisionEnvelope,
  fromDecisionResponse,
  toDecisionRequest,
  EngineError,
  MappingError,
} from '../src/mapper.js';
import type { RiverRoom, RiverSeat } from '../src/types.js';

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

const rc = (rank: string, suit: string) => ({ rank, suit });

function seat(
  seat: number,
  name: string,
  overrides: Partial<RiverSeat> = {},
): RiverSeat {
  return {
    seat,
    name,
    stack: 2000,
    bet: 0,
    status: 'active',
    ...overrides,
  };
}

function makeRoom(overrides: Partial<RiverRoom> = {}): RiverRoom {
  return {
    id: 'test-room',
    handId: 'test-hand',
    revision: 1,
    street: 'preflop',
    blinds: [10, 20],
    hero: { seat: 0, cards: [rc('A', 'spades'), rc('K', 'spades')], stack: 1980, status: 'active', effectiveStackBb: 100 },
    dealer: 1,
    board: [],
    pot: 30,
    seats: [
      seat(0, 'hero', { blind: 'SB', bet: 10, stack: 1990 }),
      seat(1, 'villain', { blind: 'BB', bet: 20, stack: 1980 }),
    ],
    legal: {
      actions: ['fold', 'call', 'raise'],
      call: 10,
      raiseTo: { min: 40, max: 2000 },
      potOdds: 0.5,
    },
    events: [],
    ...overrides,
  };
}

// ---------------------------------------------------------------------------
// G/F Ledger Tests
// ---------------------------------------------------------------------------

test('mapper: preflop G/F ledger with blinds only', () => {
  const room = makeRoom();
  const request = toDecisionRequest(room);
  const players = request.state!.players;

  assert.equal(players.length, 2);

  // Hero (SB): G=10, F=0, starting=2000
  const hero = players.find(p => p.seat === 0)!;
  assert.equal(hero.grossContributed, 10n);
  assert.equal(hero.refunded, 0n);
  assert.equal(hero.startingStack, 2000n);
  assert.equal(hero.stack, 1990n);

  // Villain (BB): G=20, F=0, starting=2000
  const villain = players.find(p => p.seat === 1)!;
  assert.equal(villain.grossContributed, 20n);
  assert.equal(villain.refunded, 0n);
  assert.equal(villain.startingStack, 2000n);
  assert.equal(villain.stack, 1980n);
});

test('mapper: preflop G/F ledger with call', () => {
  const room = makeRoom({
    events: [
      { kind: 'call', text: 'hero call 10' },
    ],
    hero: { seat: 0, cards: [rc('A', 'spades'), rc('K', 'spades')], stack: 1980, status: 'active', effectiveStackBb: 100 },
    seats: [
      seat(0, 'hero', { blind: 'SB', bet: 20, stack: 1980 }),
      seat(1, 'villain', { blind: 'BB', bet: 20, stack: 1980 }),
    ],
    pot: 40,
  });

  const request = toDecisionRequest(room);
  const hero = request.state!.players.find(p => p.seat === 0)!;

  // Hero: G = 10 (SB) + 10 (call) = 20, F = 0, starting = 2000
  assert.equal(hero.grossContributed, 20n);
  assert.equal(hero.refunded, 0n);
  assert.equal(hero.startingStack, 2000n);
  assert.equal(hero.stack, 1980n);
});

test('mapper: preflop G/F ledger with raise and call', () => {
  const room = makeRoom({
    events: [
      { kind: 'raise', text: 'hero raise to 60' },
      { kind: 'call', text: 'villain call 40' },
    ],
    street: 'flop',
    board: [rc('2', 'clubs'), rc('3', 'diamonds'), rc('7', 'hearts')],
    hero: { seat: 0, cards: [rc('A', 'spades'), rc('K', 'spades')], stack: 1940, status: 'active', effectiveStackBb: 100 },
    seats: [
      seat(0, 'hero', { blind: 'SB', bet: 0, stack: 1940 }),
      seat(1, 'villain', { blind: 'BB', bet: 0, stack: 1940 }),
    ],
    pot: 120,
    legal: { actions: ['check', 'bet'], call: 0, raiseTo: { min: 20, max: 1940 }, potOdds: 0 },
  });

  const request = toDecisionRequest(room);
  const hero = request.state!.players.find(p => p.seat === 0)!;
  const villain = request.state!.players.find(p => p.seat === 1)!;

  // Hero: G = 10 (SB) + 50 (raise to 60, incremental = 60-10=50) = 60
  assert.equal(hero.grossContributed, 60n);
  assert.equal(hero.refunded, 0n);
  assert.equal(hero.startingStack, 2000n);
  assert.equal(hero.stack, 1940n);

  // Villain: G = 20 (BB) + 40 (call) = 60
  assert.equal(villain.grossContributed, 60n);
  assert.equal(villain.refunded, 0n);
  assert.equal(villain.startingStack, 2000n);
  assert.equal(villain.stack, 1940n);
});

test('mapper: G/F ledger with all-in for less on closed street', () => {
  // All-in for less on the preflop (a closed street). The flop is current.
  // Hero (SB) raises to 60 total. Villain (BB) calls 20 more (40 total, all-in
  // for less). Hero gets 20 refund on preflop.
  const room = makeRoom({
    street: 'flop',
    board: [rc('2', 'clubs'), rc('3', 'diamonds'), rc('7', 'hearts')],
    events: [
      { kind: 'raise', text: 'hero raise to 60' },
      { kind: 'call', text: 'villain call 20' },
    ],
    hero: { seat: 0, cards: [rc('A', 'spades'), rc('K', 'spades')], stack: 1960, status: 'active', effectiveStackBb: 100 },
    seats: [
      seat(0, 'hero', { blind: 'SB', bet: 0, stack: 1960 }),
      seat(1, 'villain', { blind: 'BB', bet: 0, stack: 1960, status: 'all-in' }),
    ],
    pot: 80,
    legal: { actions: ['check', 'bet'], call: 0, raiseTo: { min: 20, max: 1960 }, potOdds: 0 },
  });

  const request = toDecisionRequest(room);
  const hero = request.state!.players.find(p => p.seat === 0)!;
  const villain = request.state!.players.find(p => p.seat === 1)!;

  // Hero: G = 10 (SB) + 50 (raise to 60, incremental = 60-10=50) = 60
  // Preflop refund: 60 - 40 = 20 (villain all-in for less)
  assert.equal(hero.grossContributed, 60n);
  assert.equal(hero.refunded, 20n);
  assert.equal(hero.startingStack, 2000n);
  assert.equal(hero.stack, 1960n);

  // Villain: G = 20 (BB) + 20 (call) = 40. No refund for villain.
  assert.equal(villain.grossContributed, 40n);
  assert.equal(villain.refunded, 0n);
  assert.equal(villain.startingStack, 2000n);
  assert.equal(villain.stack, 1960n);
});

test('mapper: G/F ledger starting stack invariant', () => {
  // Verify starting_stack = stack + G - F for a multi-street scenario.
  const room = makeRoom({
    street: 'turn',
    board: [
      rc('2', 'clubs'), rc('3', 'diamonds'), rc('7', 'hearts'),
      rc('9', 'spades'),
    ],
    events: [
      { kind: 'raise', text: 'hero raise to 60' },
      { kind: 'call', text: 'villain call 40' },
      { kind: 'bet', text: 'hero bet to 100' },
      { kind: 'call', text: 'villain call 100' },
      { kind: 'check', text: 'hero check' },
      { kind: 'check', text: 'villain check' },
    ],
    hero: { seat: 0, cards: [rc('A', 'spades'), rc('K', 'spades')], stack: 1840, status: 'active', effectiveStackBb: 100 },
    seats: [
      seat(0, 'hero', { blind: 'SB', bet: 0, stack: 1840 }),
      seat(1, 'villain', { blind: 'BB', bet: 0, stack: 1840 }),
    ],
    pot: 320,
    legal: { actions: ['check', 'bet'], call: 0, raiseTo: { min: 20, max: 1840 }, potOdds: 0 },
  });

  const request = toDecisionRequest(room);
  for (const p of request.state!.players) {
    // starting = stack + G - F
    assert.equal(
      p.startingStack,
      p.stack + p.grossContributed - p.refunded,
      `seat ${p.seat}: startingStack invariant violated`,
    );
    assert.equal(p.startingStack, 2000n);
  }
});

// ---------------------------------------------------------------------------
// Pot Layer Tests
// ---------------------------------------------------------------------------

test('mapper: pot layers from room.pots', () => {
  const room = makeRoom({
    pot: 100,
    pots: [
      { size: 60, eligible: [0, 1] },
      { size: 40, eligible: [0] },
    ],
  });

  const request = toDecisionRequest(room);
  const pot = request.state!.pot!;

  assert.equal(pot!.phase, PotPhase.ACTION);
  assert.equal(pot!.potTotal, 100n);
  assert.equal(pot!.layers.length, 2);

  // Main pot: contribution level = min(10, 20) = 10
  assert.equal(pot.layers[0]!.amount, 60n);
  assert.equal(pot.layers[0]!.contributionLevel, 10n);
  assert.deepEqual(pot.layers[0]!.eligibleSeats, [0, 1]);
  assert.equal(pot.layers[0]!.status, LayerStatus.PENDING);

  // Side pot: contribution level = min(10) = 10 (only hero eligible)
  assert.equal(pot.layers[1]!.amount, 40n);
  assert.equal(pot.layers[1]!.contributionLevel, 10n);
  assert.deepEqual(pot.layers[1]!.eligibleSeats, [0]);
});

test('mapper: pot layers from net contributions when room.pots absent', () => {
  // Hero net = 60, Villain net = 40 (current street, no refund detection).
  // Layer 1: level 40, amount = 40*2 = 80, eligible [0,1]
  // Layer 2: level 60, amount = 20*1 = 20, eligible [0]
  const room = makeRoom({
    street: 'preflop',
    board: [],
    events: [
      { kind: 'raise', text: 'hero raise to 60' },
      { kind: 'call', text: 'villain call 40' },
    ],
    hero: { seat: 0, cards: [rc('A', 'spades'), rc('K', 'spades')], stack: 1940, status: 'active', effectiveStackBb: 100 },
    seats: [
      seat(0, 'hero', { bet: 0, stack: 1940 }),
      seat(1, 'villain', { bet: 0, stack: 1960 }),
    ],
    pot: 100,
    legal: { actions: ['fold', 'call', 'raise'], call: 40, raiseTo: { min: 60, max: 1940 }, potOdds: 0.4 },
  });

  const request = toDecisionRequest(room);
  const pot = request.state!.pot!;

  assert.equal(pot!.layers.length, 2);

  // Layer 1: level 40 (villain's net), amount = 40*2 = 80
  assert.equal(pot.layers[0]!.contributionLevel, 40n);
  assert.equal(pot.layers[0]!.amount, 80n);
  assert.deepEqual(pot.layers[0]!.eligibleSeats, [0, 1]);

  // Layer 2: level 60 (hero's net), amount = 20*1 = 20
  assert.equal(pot.layers[1]!.contributionLevel, 60n);
  assert.equal(pot.layers[1]!.amount, 20n);
  assert.deepEqual(pot.layers[1]!.eligibleSeats, [0]);
});

// ---------------------------------------------------------------------------
// TransitionEvent Path Tests
// ---------------------------------------------------------------------------

test('mapper: transition path with BoardDeal at street boundary', () => {
  const room = makeRoom({
    street: 'flop',
    board: [rc('2', 'clubs'), rc('3', 'diamonds'), rc('7', 'hearts')],
    events: [
      { kind: 'raise', text: 'hero raise to 60' },
      { kind: 'call', text: 'villain call 40' },
      { kind: 'check', text: 'hero check' },
      { kind: 'check', text: 'villain check' },
    ],
    hero: { seat: 0, cards: [rc('A', 'spades'), rc('K', 'spades')], stack: 1940, status: 'active', effectiveStackBb: 100 },
    seats: [
      seat(0, 'hero', { bet: 0, stack: 1940 }),
      seat(1, 'villain', { bet: 0, stack: 1940 }),
    ],
    pot: 120,
    legal: { actions: ['check', 'bet'], call: 0, raiseTo: { min: 20, max: 1940 }, potOdds: 0 },
  });

  const request = toDecisionRequest(room);
  const path = request.state!.transitionPath;

  // Should have: raise, call, BoardDeal(flop), check, check = 5 events
  assert.equal(path.length, 5);

  // First two are preflop actions
  assert.equal(path[0]!.transition.case, 'action');
  assert.equal(path[0]!.transition!.value!.street, Street.PREFLOP);
  assert.equal(path[1]!.transition.case, 'action');
  assert.equal(path[1]!.transition!.value!.street, Street.PREFLOP);

  // Third is BoardDeal for flop
  assert.equal(path[2]!.transition.case, 'boardDeal');
  if (path[2]!.transition!.case === 'boardDeal') {
    assert.equal(path[2]!.transition!.value!.street, Street.FLOP);
    assert.equal(path[2]!.transition!.value!.cards.length, 3);
  }

  // Last two are flop checks
  assert.equal(path[3]!.transition.case, 'action');
  assert.equal(path[3]!.transition!.value!.street, Street.FLOP);
  assert.equal(path[4]!.transition.case, 'action');
  assert.equal(path[4]!.transition!.value!.street, Street.FLOP);
});

test('mapper: transition path sequence is monotonic', () => {
  const room = makeRoom({
    street: 'flop',
    board: [rc('2', 'clubs'), rc('3', 'diamonds'), rc('7', 'hearts')],
    events: [
      { kind: 'raise', text: 'hero raise to 60' },
      { kind: 'call', text: 'villain call 40' },
      { kind: 'bet', text: 'hero bet to 100' },
      { kind: 'fold', text: 'villain fold' },
    ],
    hero: { seat: 0, cards: [rc('A', 'spades'), rc('K', 'spades')], stack: 1840, status: 'active', effectiveStackBb: 100 },
    seats: [
      seat(0, 'hero', { bet: 0, stack: 1840 }),
      seat(1, 'villain', { bet: 0, stack: 1940, status: 'folded' }),
    ],
    pot: 220,
    legal: { actions: ['check', 'bet'], call: 0, raiseTo: { min: 20, max: 1840 }, potOdds: 0 },
  });

  const request = toDecisionRequest(room);
  const path = request.state!.transitionPath;

  for (let i = 1; i < path.length; i++) {
    const prev = Number(path[i - 1]!.sequence);
    const curr = Number(path[i]!.sequence);
    assert.ok(curr > prev, `sequence ${curr} should be > ${prev} at index ${i}`);
  }
});

test('mapper: ActionEvent has potBefore and stackAfter', () => {
  const room = makeRoom({
    events: [
      { kind: 'call', text: 'hero call 10' },
    ],
    hero: { seat: 0, cards: [rc('A', 'spades'), rc('K', 'spades')], stack: 1980, status: 'active', effectiveStackBb: 100 },
    seats: [
      seat(0, 'hero', { blind: 'SB', bet: 20, stack: 1980 }),
      seat(1, 'villain', { blind: 'BB', bet: 20, stack: 1980 }),
    ],
    pot: 40,
  });

  const request = toDecisionRequest(room);
  const path = request.state!.transitionPath;

  assert.equal(path.length, 1);
  assert.equal(path[0]!.transition.case, 'action');
  if (path[0]!.transition!.case === 'action') {
    const action = path[0]!.transition!.value!;
    // potBefore should be 30 (blinds only, before hero's call)
    assert.equal(action.potBefore, 30n);
    // stackAfter should be 1980 (2000 - 20 SB - 10 call = 1970... wait)
    // Actually: starting=2000, SB=10, call=10, so stackAfter = 2000-10-10 = 1980
    assert.equal(action.stackAfter, 1980n);
    assert.equal(action.action, ActionType.CALL);
    assert.equal(action.incrementalAmount, 10n);
  }
});

// ---------------------------------------------------------------------------
// ObservationProvenance Tests
// ---------------------------------------------------------------------------

test('mapper: observation provenance fields', () => {
  const room = makeRoom({
    handId: 'mapper-provenance-hand',
    revision: 42,
    events: [
      { kind: 'call', text: 'hero call 10' },
    ],
    hero: { seat: 0, cards: [rc('A', 'spades'), rc('K', 'spades')], stack: 1980, status: 'active', effectiveStackBb: 100 },
    seats: [
      seat(0, 'hero', { blind: 'SB', bet: 20, stack: 1980 }),
      seat(1, 'villain', { blind: 'BB', bet: 20, stack: 1980 }),
    ],
    pot: 40,
  });

  // Without supplied provenance the mapper fails closed: the process
  // observation ledger has no observation for this hand, so coverage is
  // MID_HAND_ATTACH and the count is zero. FULL is never fabricated.
  const request = toDecisionRequest(room);
  const provenance = request.state!.provenance!;
  assert.equal(provenance.source, ObservationSource.OFFICIAL_RESPONSE);
  assert.equal(provenance.coverage, ObservationCoverage.MID_HAND_ATTACH);
  assert.equal(provenance.observedEventCount, 0);

  // Supplied provenance passes through verbatim.
  const supplied = {
    source: ObservationSource.OFFICIAL_RESPONSE,
    coverage: ObservationCoverage.FULL,
    controllerEpoch: 'runner-epoch:0:mapper-provenance-hand',
    observedEventCount: 1,
  };
  const suppliedRequest = toDecisionRequest(room, { provenance: supplied });
  const suppliedProvenance = suppliedRequest.state!.provenance!;
  assert.equal(suppliedProvenance.source, supplied.source);
  assert.equal(suppliedProvenance.coverage, supplied.coverage);
  assert.equal(suppliedProvenance.controllerEpoch, supplied.controllerEpoch);
  assert.equal(suppliedProvenance.observedEventCount, supplied.observedEventCount);
});

// ---------------------------------------------------------------------------
// GameDefinition and RuleProfile Tests
// ---------------------------------------------------------------------------

test('mapper: game definition with rule profile', () => {
  const room = makeRoom();
  const request = toDecisionRequest(room);
  const game = request.state!.game;

  assert.equal(game!.tableCapacity, 2);
  assert.equal(game!.smallBlind, 10n);
  assert.equal(game!.bigBlind, 20n);
  assert.equal(game!.ruleProfile!.settlement, 1); // CLOCKWISE_LEFT_OF_BUTTON
  assert.equal(game!.ruleProfile!.reopening, 1); // NEVER_CUMULATIVE
  assert.equal(game!.ruleProfile!.runout, 1); // SINGLE
  assert.equal(game!.ruleProfile!.dealtSeatCount, 2);
});

// ---------------------------------------------------------------------------
// DecisionOptions Tests
// ---------------------------------------------------------------------------

test('mapper: decision options with utility profile', () => {
  const room = makeRoom();
  const request = toDecisionRequest(room);
  const options = request.options;

  assert.equal(options!.strategyProfile, 'tag');
  assert.equal(options!.utilityProfile, UtilityProfile.CASH_CHIP);
  assert.equal(options!.solverMode, 1); // AUTOMATIC
  assert.equal(options!.includeSampledAction, true);
  assert.equal(options!.includeFullStrategy, false);
});

// ---------------------------------------------------------------------------
// Legal Actions Tests
// ---------------------------------------------------------------------------

test('mapper: legal actions with raise bounds', () => {
  const room = makeRoom({
    legal: {
      actions: ['fold', 'call', 'raise'],
      call: 10,
      raiseTo: { min: 40, max: 2000 },
      potOdds: 0.5,
    },
  });

  const request = toDecisionRequest(room);
  const legal = request.state!.legalActions!;

  assert.equal(legal.length, 3);
  assert.equal(legal[0]!.type, ActionType.FOLD);
  assert.equal(legal[1]!.type, ActionType.CALL);
  assert.equal(legal[2]!.type, ActionType.RAISE);
  assert.equal(legal[2]!.minTargetTotal, 40n);
  assert.equal(legal[2]!.maxTargetTotal, 2000n);
  assert.equal(request.state!.toCall, 10n);
});

// ---------------------------------------------------------------------------
// Response Mapping Tests
// ---------------------------------------------------------------------------

test('mapper: fromDecisionResponse with strategy', () => {
  const room = makeRoom();
  const request = toDecisionRequest(room);
  const envelope = decisionEnvelope(request);

  const strategy = create(StrategySchema, {
    actions: [{ type: ActionType.CALL, probability: 1 }],
    selectedAction: { type: ActionType.CALL },
    solver: {
      source: SolverSource.POSTFLOP_HEURISTIC,
      reasonCode: 'postflop-heuristic',
      guaranteeLevel: 'approximate',
      utilityProfile: UtilityProfile.CASH_CHIP,
    },
  });

  const response = create(DecisionResponseSchema);
  response.result = { case: 'strategy', value: strategy };
  envelope.payload = { case: 'decisionResponse', value: response };

  const decision = fromDecisionResponse(envelope, room);
  assert.equal(decision.action, 'call');
  assert.equal(decision.guaranteeLevel, 'approximate');
});

test('mapper: fromDecisionResponse with expanded strategy', () => {
  const room = makeRoom({
    street: 'flop',
    board: [rc('2', 'clubs'), rc('3', 'diamonds'), rc('7', 'hearts')],
    legal: { actions: ['check', 'bet'], call: 0, raiseTo: { min: 20, max: 1980 }, potOdds: 0 },
  });
  const request = toDecisionRequest(room);
  const envelope = decisionEnvelope(request);

  const expanded = create(ExpandedStrategySchema, {
    actions: [
      { type: ActionType.CHECK, probability: 0.6 },
      { type: ActionType.BET, targetTotal: 100n, probability: 0.4 },
    ],
    selectedAction: { type: ActionType.BET, targetTotal: 100n },
    solver: {
      source: SolverSource.BLUEPRINT,
      reasonCode: 'blueprint',
      guaranteeLevel: 'exact_solved',
      utilityProfile: UtilityProfile.CASH_CHIP,
    },
  });

  const response = create(DecisionResponseSchema);
  response.result = { case: 'expandedStrategy', value: expanded };
  envelope.payload = { case: 'decisionResponse', value: response };

  const decision = fromDecisionResponse(envelope, room);
  assert.equal(decision.action, 'bet');
  assert.equal(decision.amount, 100);
  assert.equal(decision.guaranteeLevel, 'exact_solved');
});

test('mapper: fromDecisionResponse with engine error throws', () => {
  const room = makeRoom();
  const request = toDecisionRequest(room);
  const envelope = decisionEnvelope(request);

  const error = create(EngineErrorSchema, {
    code: ErrorCode.OFF_TREE_AMOUNT,
    message: 'off-tree amount',
    retryable: false,
  });

  const response = create(DecisionResponseSchema);
  response.result = { case: 'error', value: error };
  envelope.payload = { case: 'decisionResponse', value: response };

  assert.throws(
    () => fromDecisionResponse(envelope, room),
    (err: unknown) => {
      assert.ok(err instanceof EngineError);
      assert.equal(err.code, ErrorCode.OFF_TREE_AMOUNT);
      assert.equal(err.retryable, false);
      return true;
    },
  );
});

test('mapper: fromDecisionResponse with non-decision payload throws', () => {
  const room = makeRoom();
  const envelope = create(EnvelopeSchema, { protocolVersion: 3 });
  envelope.payload = { case: 'getCapabilitiesRequest', value: create(GetCapabilitiesRequestSchema) };

  assert.throws(
    () => fromDecisionResponse(envelope, room),
    /did not contain a decision/,
  );
});

// ---------------------------------------------------------------------------
// Player Status Tests
// ---------------------------------------------------------------------------

test('mapper: player status mapping', () => {
  const room = makeRoom({
    seats: [
      seat(0, 'hero', { blind: 'SB', bet: 10, stack: 1990 }),
      seat(1, 'villain', { blind: 'BB', bet: 20, stack: 1980, status: 'all-in' }),
      seat(2, 'folder', { status: 'folded', stack: 2000 }),
    ],
  });

  const request = toDecisionRequest(room);
  const players = request.state!.players;

  assert.equal(players.length, 3);
  assert.equal(players.find(p => p.seat === 0)!.status, PlayerStatus.ACTIVE);
  assert.equal(players.find(p => p.seat === 1)!.status, PlayerStatus.ALL_IN);
  assert.equal(players.find(p => p.seat === 2)!.status, PlayerStatus.FOLDED);
});

// ---------------------------------------------------------------------------
// Missing Amount Tests
// ---------------------------------------------------------------------------

test('mapper: missing chip amount throws HISTORY_INCOMPLETE', () => {
  // A bet event without a chip amount in the text, and no structured fallback
  // (not the last action of the current street).
  const room = makeRoom({
    street: 'flop',
    board: [rc('2', 'clubs'), rc('3', 'diamonds'), rc('7', 'hearts')],
    events: [
      { kind: 'raise', text: 'hero raise to 60' },
      { kind: 'call', text: 'villain call 40' },
      { kind: 'bet', text: 'hero bet' },  // No amount!
      { kind: 'fold', text: 'villain fold' },
    ],
    hero: { seat: 0, cards: [rc('A', 'spades'), rc('K', 'spades')], stack: 1840, status: 'active', effectiveStackBb: 100 },
    seats: [
      seat(0, 'hero', { bet: 0, stack: 1840 }),
      seat(1, 'villain', { bet: 0, stack: 1940, status: 'folded' }),
    ],
    pot: 220,
    legal: { actions: ['check', 'bet'], call: 0, raiseTo: { min: 20, max: 1840 }, potOdds: 0 },
  });

  assert.throws(
    () => toDecisionRequest(room),
    /HISTORY_INCOMPLETE/,
  );
});

// ---------------------------------------------------------------------------
// Envelope Tests
// ---------------------------------------------------------------------------

test('mapper: decisionEnvelope wraps with protocol version 3', () => {
  const room = makeRoom();
  const request = toDecisionRequest(room);
  const envelope = decisionEnvelope(request);

  assert.equal(envelope.protocolVersion, 3);
  assert.equal(envelope.payload.case, 'decisionRequest');
});
