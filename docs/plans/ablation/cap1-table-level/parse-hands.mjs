#!/usr/bin node
// D-346 transcript parser: turns bigshark-practice raw transcripts plus their
// .events.log sidecars into per-hand CSV rows and reach-frequency summaries.
//
// Usage:
//   node parse-hands.mjs <label>=<rawLogPath> [<label>=<rawLogPath> ...] \
//        --rows out-rows.csv --summary out-summary.csv
//
// Each rawLogPath must have a sibling "<rawLogPath>.events.log" produced by
// drive-practice.mjs. Root classes:
//   limp_ctr  river reached, no preflop raise, no flop/turn aggression
//             (pot-4 check-to-river; the five limped T6 anchor strata)
//   srp_ctr   river reached, exactly one preflop raise, no flop/turn
//             aggression (pot-8 single-raised check-to-river; raised-srp)
//   other     river reached after flop/turn aggression, no all-in
//             (unmeasured severity; carried as an explicit unknown)
//   allin_runout  river reached only as an all-in runout (no remaining
//             decision; structural zero menu residual)
//   none      hand ended before the river
import { readFileSync, writeFileSync } from 'node:fs';

const args = process.argv.slice(2);
const inputs = [];
let rowsPath = '';
let summaryPath = '';
for (let i = 0; i < args.length; ++i) {
  if (args[i] === '--rows') rowsPath = args[++i];
  else if (args[i] === '--summary') summaryPath = args[++i];
  else inputs.push(args[i]);
}
if (!inputs.length || !rowsPath || !summaryPath) {
  process.stderr.write('bad arguments\n');
  process.exit(2);
}

const readEvents = (logPath) =>
  readFileSync(`${logPath}.events.log`, 'utf8').trim().split('\n')
    .filter(Boolean)
    .map((line) => {
      const arrow = line.indexOf('=> ');
      const payload = line.slice(arrow + 3).trim();
      const sp = payload.indexOf(' ');
      return sp < 0 ? payload : payload.slice(sp + 1);
    });

const classifyPick = (pick) => {
  if (pick.startsWith('folds')) return 'fold';
  if (pick.startsWith('checks')) return 'check';
  if (pick.startsWith('calls')) return 'call';
  const bet = pick.match(/^bets (\d+)/);
  if (bet) return { kind: 'bet', target: Number(bet[1]) };
  const raise = pick.match(/^raises to (\d+)/);
  if (raise) return { kind: 'raise', target: Number(raise[1]) };
  throw new Error(`unparsed event pick: ${pick}`);
};

const botAction = (line) => {
  const m = line.match(/seat 1 \([^)]*\) (folds|checks|calls|bets (\d+)|raises to (\d+))/);
  if (!m) return null;
  if (m[1] === 'folds') return { kind: 'fold' };
  if (m[1] === 'checks') return { kind: 'check' };
  if (m[1] === 'calls') return { kind: 'call' };
  if (m[2] !== undefined) return { kind: 'bet', target: Number(m[2]) };
  return { kind: 'raise', target: Number(m[3]) };
};

const rows = [];
const summaries = [];

for (const input of inputs) {
  const eq = input.indexOf('=');
  const label = input.slice(0, eq);
  const logPath = input.slice(eq + 1);
  const lines = readFileSync(logPath, 'utf8').split('\n');
  const events = readEvents(logPath);
  let eventIdx = 0;

  let hand = null;
  const close = (endMode) => {
    if (!hand) return;
    hand.end_mode = endMode;
    hand.river_reached = hand.river_pot !== null ? 1 : 0;
    // A pot at or above one full buy-in (200 chips) can only form when a
    // player's remaining chips went in; the jam-target band above catches the
    // same event from action lines.
    if (hand.river_pot !== null && hand.river_pot >= 200) hand.all_in = 1;
    if (!hand.river_reached) hand.root_class = 'none';
    else if (hand.all_in) hand.root_class = 'allin_runout';
    else if (hand.preflop_raises === 0 && hand.flop_aggr === 0 && hand.turn_aggr === 0)
      hand.root_class = 'limp_ctr';
    else if (hand.preflop_raises === 1 && hand.flop_aggr === 0 && hand.turn_aggr === 0)
      hand.root_class = 'srp_ctr';
    else hand.root_class = 'other';
    rows.push({ run: label, ...hand });
    hand = null;
  };

  for (const raw of lines) {
    const line = raw.replace(/^>\s*/, '');
    let m;
    if ((m = line.match(/^=== Hand #(\d+)\s+\[(BTN\/SB|BB)\] ===/))) {
      close('unterminated');
      hand = {
        hand_id: Number(m[1]), seat0_pos: m[2] === 'BTN/SB' ? 'sb' : 'bb',
        preflop_raises: 0, flop_aggr: 0, turn_aggr: 0, river_aggr: 0,
        flop_reached: 0, turn_reached: 0,
        river_pot: null, all_in: 0, end_mode: '',
      };
      continue;
    }
    if (!hand) continue;
    // Street changes on BOTH transcript markers: the "--- FLOP: cards (pot)
    // ---" deal line (the bot may act before the prompt header) and the
    // "-- FLOP -- pot N --" prompt header.
    if ((m = line.match(/^-+ (PREFLOP|FLOP|TURN|RIVER)(?::| --)/))) {
      hand.street = m[1];
      if (m[1] === 'FLOP') hand.flop_reached = 1;
      if (m[1] === 'TURN') hand.turn_reached = 1;
      if (m[1] === 'RIVER') {
        const pm = line.match(/pot (\d+)/);
        if (pm) hand.river_pot = Number(pm[1]);
      }
      continue;
    }
    // Preflop aggression is printed as both "bets" (SB open / BB option under
    // the unified Bet verb) and "raises"; either makes the hand an SRP.
    const applyAction = (act) => {
      if (typeof act !== 'object') return;
      if (hand.street === 'PREFLOP' && (act.kind === 'bet' || act.kind === 'raise'))
        hand.preflop_raises += 1;
      if (act.kind === 'bet' || act.kind === 'raise') {
        if (hand.street === 'FLOP') hand.flop_aggr += 1;
        if (hand.street === 'TURN') hand.turn_aggr += 1;
        if (hand.street === 'RIVER') hand.river_aggr += 1;
      }
      // 100 BB = 200 chips; observed jam targets are 192..200 while ordinary
      // pot-fraction tops stay well below, so >= 190 is a jam-only band.
      if (act.target >= 190) hand.all_in = 1;
    };
    if (line.startsWith('Legal:')) {
      // One prompt for seat 0; consume the matching recorded reply.
      const pick = events[eventIdx++];
      if (pick === undefined) throw new Error(`missing event for hand ${hand.hand_id}`);
      applyAction(classifyPick(pick));
      continue;
    }
    const act = botAction(line);
    if (act) {
      applyAction(act);
      continue;
    }
    if (line.startsWith('Hand ends by a fold')) close('fold');
    else if (line.startsWith('Showdown')) close('showdown');
  }
  close('unterminated');
  if (eventIdx !== events.length)
    throw new Error(`${label}: consumed ${eventIdx} of ${events.length} events`);

  const n = rows.filter((r) => r.run === label).length;
  const count = (pred) => rows.filter((r) => r.run === label && pred(r)).length;
  const rate = (c) => ({ c, rate: c / n, se: Math.sqrt((c / n) * (1 - c / n) / n) });
  summaries.push({
    run: label, hands: n,
    flop: rate(count((r) => r.flop_reached)),
    turn: rate(count((r) => r.turn_reached)),
    river: rate(count((r) => r.river_reached)),
    limp_ctr: rate(count((r) => r.root_class === 'limp_ctr')),
    srp_ctr: rate(count((r) => r.root_class === 'srp_ctr')),
    check_to_river: rate(count((r) => r.root_class === 'limp_ctr' || r.root_class === 'srp_ctr')),
    other_river: rate(count((r) => r.root_class === 'other')),
    allin_runout: rate(count((r) => r.root_class === 'allin_runout')),
    all_in: rate(count((r) => r.all_in)),
    fold_ends: rate(count((r) => r.end_mode === 'fold')),
  });
}

const rowHeader = 'run,hand_id,seat0_pos,preflop_raises,flop_aggr,turn_aggr,flop_reached,turn_reached,river_reached,river_pot,river_aggr,all_in,end_mode,root_class';
writeFileSync(rowsPath, `${rowHeader}\n${rows.map((r) =>
  [r.run, r.hand_id, r.seat0_pos, r.preflop_raises, r.flop_aggr, r.turn_aggr,
   r.flop_reached, r.turn_reached, r.river_reached, r.river_pot ?? '', r.river_aggr,
   r.all_in, r.end_mode, r.root_class].join(',')).join('\n')}\n`);

const fmt = (x) => x.toFixed(5);
const summaryLines = [
  'run,hands,event,count,rate,se_rate',
  ...summaries.flatMap((s) => ['flop', 'turn', 'river', 'limp_ctr', 'srp_ctr', 'check_to_river',
    'other_river', 'allin_runout', 'all_in', 'fold_ends']
    .map((ev) => [s.run, s.hands, ev, s[ev].c, fmt(s[ev].rate), fmt(s[ev].se)].join(','))),
];
writeFileSync(summaryPath, `${summaryLines.join('\n')}\n`);
process.stdout.write(`${summaryLines.join('\n')}\n`);
