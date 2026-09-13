import type {
  Action,
  ExecutableDecision,
  RawEngineDecision,
  RiverEvent,
  RiverRoom,
  V0DecisionContext,
} from './types.js';

const RIVER_CHILDREN: Record<string, Partial<Record<Action, string>>> = {
  '': { check: 'c', bet: 'b' },
  c: { check: 'cc', bet: 'cb' },
  b: { fold: 'bf', call: 'bc', raise: 'br' },
  cb: { fold: 'cbf', call: 'cbc', raise: 'cbr' },
  br: { fold: 'brf', call: 'brc' },
  cbr: { fold: 'cbrf', call: 'cbrc' },
};
const RIVER_TERMINALS = new Set([
  'cc',
  'bf',
  'bc',
  'cbf',
  'cbc',
  'brf',
  'brc',
  'cbrf',
  'cbrc',
]);
const PLAYER_ACTIONS = new Set<Action>(['fold', 'check', 'call', 'bet', 'raise']);

export function safeFallback(room?: RiverRoom | null): ExecutableDecision {
  const legal = room?.legal;
  if (!legal) return { action: 'fold', amount: 0, reason: 'safe-fallback:no-legal' };
  const actions = legal.actions || [];
  if (actions.includes('check')) {
    return { action: 'check', amount: 0, reason: 'safe-fallback:check' };
  }
  if (actions.includes('call')) {
    return { action: 'call', amount: 0, reason: 'safe-fallback:call' };
  }
  return { action: 'fold', amount: 0, reason: 'safe-fallback:fold' };
}

export function normalizePosition(position?: string): string {
  if (!position) return 'BTN';
  return position.includes('/') ? position.split('/')[0] || 'BTN' : position;
}

interface RoundToken {
  actor: 'H' | 'O';
  action: 'x' | 'c' | 'b' | 'r' | 'f';
}

function splitRounds(
  events: RiverEvent[],
  heroName: string,
): { ok: boolean; rounds: RoundToken[][] } {
  const tokens: RoundToken[] = [];
  for (const event of events) {
    if (!PLAYER_ACTIONS.has(event.kind as Action)) continue;
    const name = (event.text || '').split(' ')[0];
    tokens.push({
      actor: name && name === heroName ? 'H' : 'O',
      action: event.kind === 'check'
        ? 'x'
        : event.kind[0] as RoundToken['action'],
    });
  }

  const rounds: RoundToken[][] = [[]];
  let raised = false;
  let passiveActions = 0;
  let handOver = false;

  for (const token of tokens) {
    if (handOver) return { ok: false, rounds };
    rounds.at(-1)?.push(token);
    if (token.action === 'f') {
      handOver = true;
      continue;
    }
    if (token.action === 'b' || token.action === 'r') {
      raised = true;
      passiveActions = 0;
      continue;
    }
    if (token.action === 'c' && (raised || rounds.length > 1)) {
      rounds.push([]);
      raised = false;
      passiveActions = 0;
      continue;
    }
    passiveActions++;
    if (passiveActions >= 2) {
      rounds.push([]);
      raised = false;
      passiveActions = 0;
    }
  }
  return { ok: !handOver, rounds };
}

const roundLine = (round: RoundToken[]): string => round
  .map(token => token.actor + token.action)
  .join(',');

interface RiverContext {
  riverGtoOn: boolean;
  riverLine?: string;
  flopLine?: string;
  turnLine?: string;
  riverBetFrac?: number;
}

function riverContext(room: RiverRoom, heroName: string): RiverContext {
  if (room.street !== 'river' || room.board?.length !== 5) {
    return { riverGtoOn: false };
  }
  if ((room.solver?.playersInHand ?? 2) !== 2) {
    return { riverGtoOn: false };
  }

  const { ok, rounds } = splitRounds(room.events || [], heroName);
  if (!ok || rounds.length < 4) return { riverGtoOn: false };
  const flop = rounds[1] || [];
  const turn = rounds[2] || [];
  const river = rounds[3] || [];
  let line = '';

  for (const token of river) {
    const action: Action = token.action === 'x'
      ? 'check'
      : token.action === 'c'
        ? 'call'
        : token.action === 'b'
          ? 'bet'
          : token.action === 'r'
            ? 'raise'
            : 'fold';
    const next = RIVER_CHILDREN[line]?.[action];
    if (!next) return { riverGtoOn: false };
    line = next;
    if (RIVER_TERMINALS.has(line)) return { riverGtoOn: false };
  }

  const call = room.legal?.call ?? 0;
  const pot = room.pot ?? 0;
  return {
    riverGtoOn: true,
    riverLine: line,
    flopLine: roundLine(flop),
    turnLine: roundLine(turn),
    riverBetFrac: call > 0 && pot > 0 ? +(call / pot).toFixed(3) : 0.75,
  };
}

export function buildContext(
  room: RiverRoom,
  config: { style?: string; heroName?: string } = {},
): V0DecisionContext {
  const hero = room.hero;
  const hole = (hero.cards || []).map(card => card.rank + card.suit[0]);
  const board = (room.board || []).map(card => card.rank + card.suit[0]);
  const position = normalizePosition(room.solver?.heroPosition || hero.position);
  const events = room.events || [];
  const heroName = config.heroName || hero.name || '';
  const raises = events.filter(event => event.kind === 'raise');
  const preflopRaises = room.street === 'preflop' ? raises : [];
  const limpers = room.street === 'preflop'
    ? events.filter(event => event.kind === 'call').length
    : 0;

  let openerPosition = '';
  if (preflopRaises[0]?.text) {
    const openerName = preflopRaises[0].text.split(' ')[0];
    const opener = (room.seats || []).find(seat => seat?.name === openerName);
    if (opener?.position) openerPosition = normalizePosition(opener.position);
  }

  const lastRaiserIsHero = raises.length
    ? Boolean(raises.at(-1)?.text.startsWith(heroName))
    : false;

  const context: V0DecisionContext = {
    handId: room.handId,
    revision: room.revision ?? 0,
    style: config.style || 'tag',
    street: room.street,
    blinds: room.blinds || [10, 20],
    position,
    playersInHand: room.solver?.playersInHand ?? 2,
    effectiveStackBb: room.solver?.effectiveStackBb ?? hero.effectiveStackBb ?? 100,
    hole,
    board,
    pot: room.pot ?? 0,
    ...riverContext(room, heroName),
    raises: preflopRaises.length,
    openerPosition,
    heroWasRaiser: lastRaiserIsHero,
    heroPreflopAggressor: lastRaiserIsHero,
    limpers,
  };
  if (room.legal) {
    context.legal = {
      actions: room.legal.actions,
      call: room.legal.call ?? 0,
      potOdds: room.legal.potOdds ?? 0,
      ...(room.legal.raiseTo ? { raiseTo: room.legal.raiseTo } : {}),
    };
  }
  return context;
}

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
