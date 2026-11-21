import assert from 'node:assert/strict';
import {mkdtempSync, writeFileSync} from 'node:fs';
import {tmpdir} from 'node:os';
import {join} from 'node:path';
import {test} from 'node:test';

import {
  B0,
  CANDIDATES,
  bbToPotFraction,
  DIAG_MAX_BYTES,
  DIAG_MAX_INFOSETS,
  DIAG_MAX_NODES,
  DIRECTIONAL_TOL,
  GAP_ANOMALY,
  GATE_COLUMNS,
  GateInputError,
  IDENTITY_TOL,
  MAX_BYTES,
  MAX_INFOSETS,
  MAX_NODES,
  MAX_RANK,
  MAX_WALL_S,
  M_GAP,
  MONOTONE_STRATUM,
  P50_RANK,
  P95_RANK,
  R0,
  STRATA,
  STRUCTURAL_ZERO_TOL,
  T_CRIT_FULL,
  T_CRIT_W1,
  TRUNK_TERM,
  chooseBrAction,
  evaluateGates,
  hfQuantile,
  labelOf,
  loadGateCsv,
  loadHandPairManifest,
  nearestRank,
  partitionStream,
  sampleSdN1,
  selectGate1,
  ucbStats,
  type CandidateGate1,
  type GateRow,
  type HandPairManifest,
  type SchemeName,
  type StratumSpec,
} from './rfc0046-gates.js';

// --- Fixture builder ---------------------------------------------------------

type Writable = {
  -readonly [K in keyof GateRow]: GateRow[K];
};

interface BuildCtx {
  scheme: SchemeName;
  spec: StratumSpec;
  cap: number;
  seed: number;
  resolveId: string;
  policy: string;
}

interface BuildOpts {
  schemes?: SchemeName[];
  mutate?: (r: Writable, ctx: BuildCtx) => void;
  // Adds the cap-2 escalation block for (scheme, stratum) pairs.
  cap2?: Array<{scheme: SchemeName; stratum: string;
                    mutate?: (r: Writable, ctx: BuildCtx) => void}>;
}

const TRUNK_CONST: Record<SchemeName, {label: string; rows: number; bytes: number}> = {
  T0: {label: 'tiers-turn', rows: 100, bytes: 1000},
  T1: {label: 'ehs-32-64-64-turn', rows: 200, bytes: 2000},
  T2t: {label: 'ehs-turn-riverhint-train', rows: 300, bytes: 3000},
  T2h: {label: 'ehs-turn-riverhint-both', rows: 400, bytes: 4000},
};

function handPair(fallback = false): HandPairManifest {
  return {
    stratum: fallback ? 'unpaired-twotone' : 'monotone',
    board: fallback ? '7sKhAsJs' : 'QsKsAsJs',
    river: '9c',
    seed: 4320,
    iterations: 2_000_000,
    referenceDigest: 'sha256:deadbeef',
    combos: ['Ac Kd', 'Qc Jd'],
    categoryId: 'tier-3',
    actionIndices: [0, 2],
    valueGap: 0.0042,
    fallbackUsed: fallback,
  };
}

function baseRow(scheme: SchemeName, spec: StratumSpec, cap: number,
                 seed: number, resolveId: string, policy: string): Writable {
  const tc = TRUNK_CONST[scheme];
  return {
    stratum: spec.name,
    root: spec.root,
    turnBoard: spec.turnBoard,
    riverCard: spec.riverCard,
    range: spec.range,
    scheme,
    trunk: tc.label,
    cap,
    seed,
    resolveId,
    policy: policy as GateRow['policy'],
    trunkRows: tc.rows,
    trunkBytes: tc.bytes,
    infosets: 800_000,
    nodes: 4_000_000,
    peakBytes: 900_000_000,
    trainS: 6.2,
    certifyS: 1.1,
    resolveWallS: 8.0,
    walkS: 200.0,
    termination: 'Certified',
    coldcoldBitwise: true,
    warmhintBitwise: true,
    maxMarginDiff: 1e-7,
    maxCertSlack: 1e-7,
    totalNc: null,
    tTrunk: null,
    tBoundary: null,
    tRiver: null,
    tAction: null,
    extensionMass: null,
    telescopeResid: null,
    ncEndgameCap1: null,
    ncEndgameCap0: null,
    riverArgmaxDiff: null,
    note: 'final',
  };
}

function reconcileIdentity(r: Writable): void {
  // The CSV contract pins total_nc = sum of the four label terms; fixtures
  // mutate individual terms and derive the total here exactly.
  if (r.policy === 'cap0ref')
    return;
  if (r.tTrunk !== null && r.tBoundary !== null && r.tRiver !== null
      && r.tAction !== null)
    r.totalNc = r.tTrunk + r.tBoundary + r.tRiver + r.tAction;
}

function emitKey(
  out: Writable[], scheme: SchemeName, spec: StratumSpec, cap: number,
  seed: number, resolveId: string, mutate?: BuildOpts['mutate'],
): void {
  const ctxBase = {scheme, spec, cap, seed, resolveId};
  // Hybrid.
  const h = baseRow(scheme, spec, cap, seed, resolveId, 'hybrid');
  h.tTrunk = 0;
  h.tBoundary = 0.0003;
  h.tRiver = 0.0005;
  h.tAction = 0.0001;
  h.extensionMass = 0;
  h.telescopeResid = 0;
  h.ncEndgameCap1 = 0.0010;
  h.ncEndgameCap0 = 0.0005;
  if (spec.name === MONOTONE_STRATUM)
    h.riverArgmaxDiff = true;
  mutate?.(h, {...ctxBase, policy: 'hybrid'});
  reconcileIdentity(h);
  out.push(h);

  // Variants.
  const river = baseRow(scheme, spec, cap, seed, resolveId, 'rivermerge');
  river.tTrunk = 0;
  river.tBoundary = h.tBoundary;
  river.tAction = h.tAction;
  river.tRiver = spec.name === MONOTONE_STRATUM
    ? (h.tRiver ?? 0) + 0.0001
    : h.tRiver;
  river.extensionMass = 0;
  river.telescopeResid = 0;
  mutate?.(river, {...ctxBase, policy: 'rivermerge'});
  reconcileIdentity(river);
  out.push(river);

  for (const policy of ['bucketmargin', 'statictrunk'] as const) {
    const v = baseRow(scheme, spec, cap, seed, resolveId, policy);
    v.tTrunk = 0;
    v.tBoundary = h.tBoundary;
    v.tRiver = h.tRiver;
    v.tAction = h.tAction;
    v.extensionMass = 0;
    v.telescopeResid = 0;
    mutate?.(v, {...ctxBase, policy});
    reconcileIdentity(v);
    out.push(v);
  }

  // cap0ref reference (cap 0).
  const ref = baseRow(scheme, spec, 0, seed, resolveId, 'cap0ref');
  ref.nodes = 12_000_000;
  ref.peakBytes = 6_000_000_000;
  ref.resolveWallS = 420;
  ref.ncEndgameCap0 = 0.0005;
  mutate?.(ref, {...ctxBase, cap: 0, policy: 'cap0ref'});
  out.push(ref);
}

function buildDataset(opts: BuildOpts = {}): GateRow[] {
  const schemes = opts.schemes ?? [...CANDIDATES];
  const out: Writable[] = [];
  for (const scheme of schemes) {
    for (const spec of STRATA) {
      for (const seed of spec.timingSeeds)
        for (let k = 0; k < 10; ++k)
          emitKey(out, scheme, spec, 1, seed, `r${k}`, opts.mutate);
      for (const seed of spec.seeds)
        if (!spec.timingSeeds.includes(seed))
          emitKey(out, scheme, spec, 1, seed, `walk-s${seed}`, opts.mutate);
    }
  }
  for (const esc of opts.cap2 ?? []) {
    const spec = STRATA.find((s) => s.name === esc.stratum)!;
    for (const seed of spec.timingSeeds)
      for (let k = 0; k < 10; ++k)
        emitKey(out, esc.scheme, spec, 2, seed, `c2r${k}`, esc.mutate);
    for (const seed of spec.seeds)
      if (!spec.timingSeeds.includes(seed))
        emitKey(out, esc.scheme, spec, 2, seed, `c2walk-s${seed}`, esc.mutate);
  }
  return out as GateRow[];
}

function evaluate(rows: GateRow[], hp = handPair()) {
  return evaluateGates(rows, hp);
}

function throwsCode(fn: () => unknown, code: string): void {
  assert.throws(fn, (e: unknown) => {
    assert.ok(e instanceof GateInputError, `expected GateInputError, got ${String(e)}`);
    assert.equal((e as GateInputError).code, code);
    return true;
  });
}

// --- Frozen constants --------------------------------------------------------

test('frozen statistics constants match the Stage-0 freeze', () => {
  assert.equal(T_CRIT_FULL, 4.303);
  assert.equal(T_CRIT_W1, 2.7764);
  assert.deepEqual([...STRATA.map((s) => s.name)], [
    'w1', 'unpaired-rainbow-ahigh', 'paired', 'monotone',
    'unpaired-twotone', 'raised-srp',
  ]);
  assert.equal(MAX_NODES, 5_000_000);
  assert.equal(MAX_INFOSETS, 1_000_000);
  assert.equal(MAX_BYTES, 1_073_741_824);
  assert.equal(DIAG_MAX_NODES, 10_000_000);
  assert.equal(DIAG_MAX_INFOSETS, 2_000_000);
  assert.equal(DIAG_MAX_BYTES, 2_147_483_648);
  assert.equal(B0, 0.010);
  assert.equal(R0, 0.002);
  assert.equal(M_GAP, 0.002);
  assert.equal(P95_RANK, 29);
  assert.equal(P50_RANK, 15);
  assert.equal(MAX_RANK, 30);
  assert.equal(MAX_WALL_S, 12.0);
  assert.deepEqual(CANDIDATES, ['T0', 'T1', 'T2t', 'T2h']);
  // Frozen boards/roots/ranges.
  const byName = new Map(STRATA.map((s) => [s.name, s]));
  assert.equal(byName.get('w1')!.range, 'w1');
  assert.equal(byName.get('w1')!.turnBoard, '2c3d7hJs');
  assert.equal(byName.get('raised-srp')!.root, 'single-raised');
  assert.equal(byName.get('raised-srp')!.riverCard, '2s');
  assert.equal(byName.get('unpaired-rainbow-ahigh')!.turnBoard, '7sKhAdJs');
  assert.equal(byName.get('monotone')!.turnBoard, 'QsKsAsJs');
  assert.equal(byName.get('unpaired-twotone')!.turnBoard, '7sKhAsJs');
});

test('CSV header is frozen exactly with 36 columns', () => {
  assert.equal(GATE_COLUMNS.length, 36);
  assert.equal(GATE_COLUMNS[0], 'stratum');
  assert.equal(GATE_COLUMNS[11], 'trunk_rows');
  assert.equal(GATE_COLUMNS[35], 'note');
});

// --- HF type-1 quantiles -----------------------------------------------------

test('bb<->root-pot unit conversion per frozen stratum (finding M2)', () => {
  const limped = STRATA.find((s) => s.name === 'paired')!;
  const raised = STRATA.find((s) => s.name === 'raised-srp')!;
  // Limped: pot 4, bb 2 -> 1 BB = 0.5 pot. Raised: pot 8, bb 2 -> 0.25 pot.
  assert.equal(limped.bigBlind, 2);
  assert.equal(limped.pot, 4);
  assert.equal(raised.pot, 8);
  assert.ok(Math.abs(bbToPotFraction(limped, 1) - 0.5) < 1e-15);
  assert.ok(Math.abs(bbToPotFraction(raised, 1) - 0.25) < 1e-15);
  // A -1e-6 root-pot floor is -2e-6 BB limped, -4e-6 BB raised.
  assert.ok(Math.abs(GAP_ANOMALY / (limped.bigBlind / limped.pot) - -2e-6) < 1e-18);
  assert.ok(Math.abs(GAP_ANOMALY / (raised.bigBlind / raised.pot) - -4e-6) < 1e-18);
});

test('nearest-rank HF type-1 indices at n=30', () => {
  const samples = Array.from({length: 30}, (_, i) => (i + 1) / 10); // 0.1..3.0 s
  const sorted = [...samples].sort((a, b) => a - b);
  assert.equal(nearestRank(sorted, P50_RANK), 1.5); // 15th ordered
  assert.equal(nearestRank(sorted, P95_RANK), 2.9); // 29th ordered
  assert.equal(nearestRank(sorted, MAX_RANK), 3.0); // 30th = max
  assert.equal(hfQuantile(samples, 0.5), 1.5);
  assert.equal(hfQuantile(samples, 0.95), 2.9);
  // Order independence: shuffled input yields the same ranks.
  const shuffled = [...samples].reverse();
  assert.equal(hfQuantile(shuffled, 0.95), 2.9);
  throwsCode(() => nearestRank(sorted, 31), 'bad-rank');
});

// --- UCB math ----------------------------------------------------------------

test('n=3 UCB uses mean + 4.303*sd(n-1)/sqrt(3)', () => {
  const s = ucbStats([1, 2, 3], T_CRIT_FULL);
  assert.equal(s.n, 3);
  assert.ok(Math.abs(s.mean - 2) < 1e-15);
  assert.ok(Math.abs(s.sd - 1) < 1e-15);
  assert.ok(Math.abs(s.halfWidth - T_CRIT_FULL / Math.sqrt(3)) < 1e-12);
  assert.ok(Math.abs(s.ucb - (2 + T_CRIT_FULL / Math.sqrt(3))) < 1e-12);
  assert.ok(Math.abs(s.lcb - (2 - T_CRIT_FULL / Math.sqrt(3))) < 1e-12);
  assert.equal(sampleSdN1([7]), 0);
});

test('n=5 W1 UCB uses t=2.7764', () => {
  const s = ucbStats([1, 2, 3, 4, 5], T_CRIT_W1);
  const sd = Math.sqrt(2.5); // n-1
  assert.equal(s.sd, sd);
  assert.ok(Math.abs(s.ucb - (3 + (T_CRIT_W1 * sd) / Math.sqrt(5))) < 1e-12);
});

test('six-stratum-max UCB includes W1 under its own n=5 stats (worked example)',
  () => {
    // Pin hybrid total_nc per seed: full strata at 0.0009; w1 timing seeds
    // at 0.0010, extra seeds 4323/4324 at 0.020 / 0.030.
    const rows = buildDataset({
      mutate: (r, ctx) => {
        if (ctx.policy !== 'hybrid' || ctx.spec.name !== 'w1')
          return;
        const perSeed = ctx.seed >= 4323
          ? 0.01 * (ctx.seed - 4321)
          : 0.001;
        r.tTrunk = 0;
        r.tBoundary = 0.0003;
        r.tRiver = 0.0005;
        r.tAction = perSeed - 0.0003 - 0.0005;
      },
    });
    const v = evaluate(rows);
    const t0 = v.gate1.candidates.find((c) => c.scheme === 'T0')!;
    // w1 seed means: [0.001, 0.001, 0.001, 0.020, 0.030].
    const w1vals = [0.001, 0.001, 0.001, 0.020, 0.030];
    const m = w1vals.reduce((a, b) => a + b, 0) / 5;
    const sd = Math.sqrt(w1vals.reduce((a, x) => a + (x - m) ** 2, 0) / 4);
    const w1ucb = m + (T_CRIT_W1 * sd) / Math.sqrt(5);
    const w1stat = t0.perStratum.find((s) => s.stratum === 'w1')!;
    assert.ok(Math.abs(w1stat.total.ucb - w1ucb) < 1e-12);
    assert.equal(w1stat.total.n, 5);
    // Full strata sit at 0.0009 total; w1 UCB dominates the six-max.
    assert.ok(Math.abs(t0.sixMaxUcbTotal - w1ucb) < 1e-12);
    assert.ok(w1ucb > B0); // worked example deliberately over threshold
    assert.equal(t0.eligible, false);
  });

// --- Partition label rule and identity --------------------------------------

test('BR argmax ties prefer the cap-1 menu action then lowest schedule index', () => {
  // Outside action ties the inside action on value: inside wins.
  const picked = chooseBrAction([
    {value: 1.0, insideCap1: false, scheduleIndex: 0},
    {value: 1.0, insideCap1: true, scheduleIndex: 3},
  ]);
  assert.equal(picked.insideCap1, true);
  assert.equal(picked.scheduleIndex, 3);
  // Two tied inside actions: lowest frozen schedule index wins.
  const tie = chooseBrAction([
    {value: 2.0, insideCap1: true, scheduleIndex: 5},
    {value: 2.0, insideCap1: true, scheduleIndex: 2},
  ]);
  assert.equal(tie.scheduleIndex, 2);
  // Strictly better outside action is selected (then rule 2 labels T_action).
  const out = chooseBrAction([
    {value: 0.9, insideCap1: true, scheduleIndex: 0},
    {value: 1.1, insideCap1: false, scheduleIndex: 1},
  ]);
  assert.equal(out.insideCap1, false);
});

test('label precedence: extension (rule 1) beats menu (rule 2) and street (rule 3)',
  () => {
    // Extension-only node: T_action even though BR is inside and on river.
    assert.equal(labelOf({
      street: 'river', extensionOnly: true, brInsideCap1: true,
    }), 'T_action');
    // Missing-menu regret.
    assert.equal(labelOf({
      street: 'river', extensionOnly: false, brInsideCap1: false,
    }), 'T_action');
    assert.equal(labelOf({
      street: 'turn', extensionOnly: false, brInsideCap1: false,
    }), 'T_action');
    // Street labels.
    assert.equal(labelOf({
      street: 'turn', extensionOnly: false, brInsideCap1: true,
    }), 'T_boundary');
    assert.equal(labelOf({
      street: 'river', extensionOnly: false, brInsideCap1: true,
    }), 'T_river');
  });

test('cap-0-only argmax TIE prefers cap-1: river/turn nodes avoid T_action', () => {
  const tied = chooseBrAction([
    {value: 1, insideCap1: false, scheduleIndex: 4},
    {value: 1, insideCap1: true, scheduleIndex: 1},
  ]);
  assert.equal(tied.insideCap1, true);
  const riverNode = {
    street: 'river' as const, extensionOnly: false, brInsideCap1: tied.insideCap1,
  };
  assert.equal(labelOf(riverNode), 'T_river');
  const turnNode = {
    street: 'turn' as const, extensionOnly: false, brInsideCap1: tied.insideCap1,
  };
  assert.equal(labelOf(turnNode), 'T_boundary');
});

test('partition identity telescopes and T_trunk is the structural zero constant',
  () => {
    assert.equal(TRUNK_TERM, 0);
    const p = partitionStream([
      {street: 'turn', reachH: 1, g: 0.3, extensionOnly: false, brInsideCap1: true},
      {street: 'river', reachH: 1, g: 0.5, extensionOnly: false, brInsideCap1: true},
      {street: 'river', reachH: 1, g: 0.1, extensionOnly: false, brInsideCap1: false},
      // Extension-region node: g must be identically zero.
      {street: 'river', reachH: 0, g: 0, extensionOnly: true, brInsideCap1: false},
    ]);
    assert.equal(p.terms.T_trunk, 0);
    assert.equal(p.terms.T_boundary, 0.3);
    assert.equal(p.terms.T_river, 0.5);
    assert.equal(p.terms.T_action, 0.1);
    assert.equal(p.total, 0.9);
    assert.equal(p.extensionMass, 0);
    assert.equal(p.residual, 0);
  });

test('a nonzero extension-region mass breaks identity (fail-closed signal)', () => {
  const p = partitionStream([
    {street: 'river', reachH: 1, g: 0.5, extensionOnly: false, brInsideCap1: true},
    {street: 'turn', reachH: 0, g: 0.007, extensionOnly: true, brInsideCap1: true},
  ]);
  assert.ok(p.extensionMass > STRUCTURAL_ZERO_TOL);
  assert.ok(p.residual > IDENTITY_TOL);
});

// --- Gate-1 total order ------------------------------------------------------

function makeCandidate(partial: Partial<import('./rfc0046-gates.js').Gate1OrderEntry> &
    Pick<import('./rfc0046-gates.js').Gate1OrderEntry, 'scheme'>):
    import('./rfc0046-gates.js').Gate1OrderEntry {
  return {
    trunkRows: 0, trunkBytes: 0, worstP95S: 0, eligible: true,
    ...partial,
  };
}

test('Gate-1 selection total order: rows beats bytes beats p95 beats lex name',
  () => {
    const rows = [
      makeCandidate({scheme: 'B', trunkRows: 10}),
      makeCandidate({scheme: 'A', trunkRows: 20}),
    ];
    assert.equal(selectGate1(rows), 'B');
    const bytes = [
      makeCandidate({scheme: 'B', trunkRows: 10, trunkBytes: 200}),
      makeCandidate({scheme: 'A', trunkRows: 10, trunkBytes: 100}),
    ];
    assert.equal(selectGate1(bytes), 'A');
    const p95 = [
      makeCandidate({scheme: 'B', trunkRows: 10, trunkBytes: 100, worstP95S: 9}),
      makeCandidate({scheme: 'A', trunkRows: 10, trunkBytes: 100, worstP95S: 8}),
    ];
    assert.equal(selectGate1(p95), 'A');
    const lex = [
      makeCandidate({scheme: 'T2t', trunkRows: 10, trunkBytes: 100, worstP95S: 8}),
      makeCandidate({scheme: 'T2h', trunkRows: 10, trunkBytes: 100, worstP95S: 8}),
    ];
    assert.equal(selectGate1(lex), 'T2h'); // 'h' < 't'
  });

test('Gate-1 only selects eligible candidates and null when none eligible', () => {
  assert.equal(selectGate1([
    makeCandidate({scheme: 'T0', eligible: false}),
    makeCandidate({scheme: 'T1', eligible: true, trunkRows: 99}),
  ]), 'T1');
  assert.equal(selectGate1([
    makeCandidate({scheme: 'T0', eligible: false}),
  ]), null);
});

// --- End-to-end frozen dataset ----------------------------------------------

test('complete frozen dataset yields PASS with T0 selected and cap-1 Gate 3', () => {
  const v = evaluate(buildDataset());
  assert.equal(v.verdict, 'PASS');
  assert.equal(v.gate2.status, 'PASS');
  assert.deepEqual(v.gate1.eligible, ['T0', 'T1', 'T2t', 'T2h']);
  assert.equal(v.gate1.selected, 'T0');
  assert.ok(v.gate3);
  assert.equal(v.gate3!.passes, true);
  assert.equal(v.gate3!.cap, 1);
  assert.equal(v.gate3!.perStratum.length, 6);
  for (const s of v.gate3!.perStratum) {
    assert.ok(s.gap.ucb <= M_GAP);
    assert.equal(s.n, s.stratum === 'w1' ? 5 : 3);
  }
  // Gate-2 timing view: 29th <= 10 s and 30th <= 12 s.
  for (const s of v.gate2.strata) {
    assert.equal(s.classification, 'pass');
    assert.ok(s.p95S! <= 10.0);
    assert.ok(s.maxS! <= 12.0);
  }
});

test('all candidates ineligible on B0 closes via Gate 1', () => {
  const rows = buildDataset({
    mutate: (r) => {
      if (r.policy === 'hybrid' && r.tBoundary !== null) {
        r.tBoundary = 0.02;
        r.tRiver = 0.0005;
        r.tAction = 0.0295; // total = 0.05 > B0
      }
    },
  });
  const v = evaluate(rows);
  assert.equal(v.gate2.status, 'PASS');
  assert.equal(v.gate1.selected, null);
  assert.equal(v.verdict, 'CLOSE');
  assert.equal(v.gate3, null);
});

test('R0 failure on T_river makes a candidate ineligible', () => {
  const rows = buildDataset({
    mutate: (r, ctx) => {
      if (ctx.policy === 'hybrid' && r.tRiver !== null) {
        r.tRiver = 0.005;
        r.totalNc = (r.tBoundary ?? 0) + r.tRiver + (r.tAction ?? 0);
      }
    },
  });
  const v = evaluate(rows);
  assert.ok(v.gate1.candidates.every((c) => !c.eligible));
  assert.equal(v.verdict, 'CLOSE');
});

// --- Gate 3 ------------------------------------------------------------------

test('Gate-3 cap-gap sign anomaly (LCB < -1e-6 root-pot) aborts', () => {
  const rows = buildDataset({
    mutate: (r) => {
      if (r.policy === 'hybrid') {
        r.ncEndgameCap1 = 0.0004;
        r.ncEndgameCap0 = 0.0005; // gap = -0.0001, zero variance
      }
    },
  });
  assert.throws(() => evaluate(rows), (e: Error) => {
    assert.ok(e instanceof GateInputError);
    assert.equal((e as GateInputError).code, 'gap-sign-anomaly');
    return true;
  });
});

test('Gate-3 gap UCB above M with no cap-2 remeasure closes', () => {
  const rows = buildDataset({
    mutate: (r) => {
      if (r.policy === 'hybrid')
        r.ncEndgameCap1 = 0.005; // gap 0.0045 > 0.002 everywhere
    },
  });
  const v = evaluate(rows);
  assert.equal(v.verdict, 'CLOSE');
  assert.equal(v.gate3!.passes, false);
  assert.equal(v.gate3!.closedReason, 'gap-exceeds-m-no-escalation');
  assert.ok(v.gate3!.failingAtCap1.length === 6);
});

test('cap0ref build failure closes Gate 3', () => {
  const rows = buildDataset({
    mutate: (r, ctx) => {
      if (r.policy === 'cap0ref' && ctx.spec.name === 'paired') {
        r.termination = 'ResourceExhausted';
        r.maxCertSlack = null;
      }
    },
  });
  const v = evaluate(rows);
  assert.equal(v.verdict, 'CLOSE');
  assert.equal(v.gate3!.closedReason, 'cap0ref-build-failure');
  assert.deepEqual(v.gate3!.failingAtCap1, ['paired']);
});

test('cap-2 escalation rescues only the failing strata and yields PASS', () => {
  // T0 monotone fails at cap 1; cap-2 remeasure on monotone fits the
  // unchanged envelope and clears M.
  const rows = buildDataset({
    mutate: (r, ctx) => {
      if (r.policy === 'hybrid' && ctx.spec.name === MONOTONE_STRATUM)
        r.ncEndgameCap1 = 0.005;
    },
    cap2: [{
      scheme: 'T0',
      stratum: MONOTONE_STRATUM,
      mutate: (r) => {
        if (r.policy === 'hybrid') {
          r.ncEndgameCap1 = 0.0010;
          r.ncEndgameCap0 = 0.0005;
        }
      },
    }],
  });
  const v = evaluate(rows);
  assert.equal(v.verdict, 'PASS');
  assert.equal(v.gate3!.cap, 2);
  assert.equal(v.gate3!.escalation!.attempted, true);
  assert.equal(v.gate3!.escalation!.passed, true);
  assert.deepEqual(v.gate3!.failingAtCap1, [MONOTONE_STRATUM]);
});

test('failed cap-2 escalation closes', () => {
  const rows = buildDataset({
    mutate: (r, ctx) => {
      if (r.policy === 'hybrid' && ctx.spec.name === MONOTONE_STRATUM)
        r.ncEndgameCap1 = 0.005;
    },
    cap2: [{
      scheme: 'T0',
      stratum: MONOTONE_STRATUM,
      mutate: (r) => {
        if (r.policy === 'hybrid')
          r.ncEndgameCap1 = 0.006; // still above M
      },
    }],
  });
  const v = evaluate(rows);
  assert.equal(v.verdict, 'CLOSE');
  assert.equal(v.gate3!.cap, 1);
  assert.equal(v.gate3!.closedReason, 'cap2-escalation-failed');
});

// --- Variant directional checks ---------------------------------------------

test('rivermerge strict check fails on monotone when river mass is equal', () => {
  const rows = buildDataset({
    mutate: (r, ctx) => {
      if (ctx.policy === 'rivermerge' && ctx.spec.name === MONOTONE_STRATUM)
        r.tRiver = 0.0005; // equal to hybrid: strict gap 0
    },
  });
  throwsCode(() => evaluate(rows), 'directional-rivermerge-strict');
});

test('rivermerge non-strict bound is satisfied with equality off monotone', () => {
  const rows = buildDataset();
  const v = evaluate(rows);
  assert.equal(v.verdict, 'PASS');
});

test('rivermerge below T_river minus tol fails on a non-monotone stratum', () => {
  // paired stratum: pot=4, bb=2; directional tol 1e-9 root-pot = 2e-9 BB.
  const rows = buildDataset({
    mutate: (r, ctx) => {
      if (ctx.policy === 'rivermerge' && ctx.spec.name === 'paired')
        r.tRiver = 0.0005 - 1e-7;
    },
  });
  throwsCode(() => evaluate(rows), 'directional-rivermerge');
});

test('bucketmargin and statictrunk turn-mass directional checks', () => {
  for (const policy of ['bucketmargin', 'statictrunk'] as const) {
    const rows = buildDataset({
      mutate: (r, ctx) => {
        if (ctx.policy === policy && ctx.spec.name === 'paired')
          r.tBoundary = 0.0003 - 1e-7;
      },
    });
    throwsCode(() => evaluate(rows), 'directional-turn');
  }
});

// --- Structural partition predicates ----------------------------------------

test('identity residual beyond 1e-9 is rejected', () => {
  const rows = buildDataset({
    mutate: (r, ctx) => {
      if (r.policy === 'hybrid' && ctx.spec.name === 'paired' && ctx.seed === 4320)
        r.telescopeResid = 2e-9;
    },
  });
  throwsCode(() => evaluate(rows), 'identity-residual');
});

test('t_trunk must be the pinned zero', () => {
  const rows = buildDataset({
    mutate: (r, ctx) => {
      if (r.policy === 'hybrid' && ctx.spec.name === 'paired' && ctx.seed === 4320)
        r.tTrunk = 1e-10;
    },
  });
  throwsCode(() => evaluate(rows), 'trunk-not-zero');
});

test('extension_mass must be identically zero', () => {
  const rows = buildDataset({
    mutate: (r, ctx) => {
      if (r.policy === 'hybrid' && ctx.spec.name === 'paired' && ctx.seed === 4320)
        r.extensionMass = 1e-10;
    },
  });
  throwsCode(() => evaluate(rows), 'extension-mass');
});

test('a term below -1e-6 root-pot is rejected', () => {
  // paired stratum: the -1e-6 root-pot floor is -2e-6 BB; -1e-5 BB is below.
  const rows = buildDataset({
    mutate: (r, ctx) => {
      if (r.policy === 'hybrid' && ctx.spec.name === 'paired' && ctx.seed === 4320)
        r.tAction = -1e-5;
    },
  });
  throwsCode(() => evaluate(rows), 'negative-term');
});

// --- W1/full substitution, seed accounting ----------------------------------

test('a full stratum populated with range=w1 rows is rejected', () => {
  const rows = buildDataset({
    mutate: (r, ctx) => {
      if (ctx.spec.name === 'paired')
        r.range = 'w1';
    },
  });
  throwsCode(() => evaluate(rows), 'range-substitution');
});

test('w1 populated with range=full rows is rejected', () => {
  const rows = buildDataset({
    mutate: (r, ctx) => {
      if (ctx.spec.name === 'w1')
        r.range = 'full';
    },
  });
  throwsCode(() => evaluate(rows), 'range-substitution');
});

test('wrong cold-resolve count for a seed aborts', () => {
  const rows = buildDataset().filter((r) =>
    !(r.scheme === 'T0' && r.stratum === 'paired' && r.seed === 4322
      && r.resolveId === 'r9'));
  throwsCode(() => evaluate(rows), 'timing-count');
});

test('missing extra W1 UCB seed aborts', () => {
  const rows = buildDataset().filter((r) =>
    !(r.stratum === 'w1' && r.seed === 4324));
  throwsCode(() => evaluate(rows), 'ucb-seed-count');
});

test('a final hybrid row with non-Certified termination is rejected', () => {
  const rows = buildDataset({
    mutate: (r, ctx) => {
      if (r.policy === 'hybrid' && ctx.spec.name === 'paired' && ctx.seed === 4320
          && ctx.resolveId === 'r3')
        r.termination = 'CertificationRejected';
    },
  });
  throwsCode(() => evaluate(rows), 'non-certified-final');
});

test('missing joined variant/cap0ref rows abort', () => {
  const rows = buildDataset().filter((r) =>
    !(r.policy === 'rivermerge' && r.stratum === 'paired' && r.scheme === 'T0'));
  throwsCode(() => evaluate(rows), 'missing-policy-row');
  const rows2 = buildDataset().filter((r) =>
    !(r.policy === 'cap0ref' && r.stratum === 'paired' && r.scheme === 'T0'));
  throwsCode(() => evaluate(rows2), 'missing-policy-row');
});

// --- Diagnostics and Gate-2 typed mapping -----------------------------------

// Replace one of the R=10 timing slots (all five policy rows) with a typed
// truncated hybrid failure, mirroring how a failed cold resolve is recorded
// (it occupies the slot; it is not retried into a 31st resolve).
function failTimingSlot(
  rows: GateRow[], scheme: SchemeName, stratum: string, resolveId: string,
): GateRow[] {
  return rows.filter((r) =>
    !(r.scheme === scheme && r.stratum === stratum && r.seed === 4320
      && r.resolveId === resolveId));
}

function addDiagnosticPair(
  rows: GateRow[], scheme: SchemeName, stratum: string,
  diagTermination: GateRow['termination'],
  suffix = '1',
): GateRow[] {
  const out = [...failTimingSlot(rows, scheme, stratum, 'r9')] as Writable[];
  const spec = STRATA.find((s) => s.name === stratum)!;
  const truncated = baseRow(scheme, spec, 1, 4320, `fail${suffix}`, 'hybrid');
  truncated.termination = 'SolveDeadline';
  truncated.note = 'truncated';
  truncated.nodes = 5_000_000;
  truncated.maxCertSlack = null;
  truncated.totalNc = null;
  out.push(truncated);
  const diag = baseRow(scheme, spec, 1, 4320, `diag${suffix}`, 'hybrid');
  diag.termination = diagTermination;
  diag.note = 'diagnostic';
  diag.nodes = 9_000_000;
  diag.infosets = 1_500_000;
  diag.peakBytes = 2_000_000_000;
  diag.resolveWallS = 25;
  if (diagTermination !== 'Certified')
    diag.maxCertSlack = null;
  out.push(diag);
  return out as GateRow[];
}

test('deadline failure plus certified diagnostic profile -> REDIRECT', () => {
  const rows = addDiagnosticPair(buildDataset(), 'T0', 'paired', 'Certified');
  const v = evaluate(rows);
  assert.equal(v.gate2.status, 'REDIRECT');
  assert.equal(v.verdict, 'REDIRECT');
  const s = v.gate2.strata.find((x) => x.scheme === 'T0' && x.stratum === 'paired')!;
  assert.equal(s.classification, 'budget-tight');
});

test('diagnostic profile that cannot certify -> CLOSE (bound unreachable)', () => {
  const rows = addDiagnosticPair(buildDataset(), 'T0', 'paired',
    'CertificationRejected');
  const v = evaluate(rows);
  assert.equal(v.gate2.status, 'CLOSE');
  assert.equal(v.verdict, 'CLOSE');
});

test('a failed seed without the diagnostic profile aborts (unclassifiable)', () => {
  const base = failTimingSlot(buildDataset(), 'T0', 'paired', 'r9');
  const rows = [...base] as Writable[];
  const spec = STRATA.find((s) => s.name === 'paired')!;
  const truncated = baseRow('T0', spec, 1, 4320, 'fail1', 'hybrid');
  truncated.termination = 'SolveDeadline';
  truncated.note = 'truncated';
  truncated.maxCertSlack = null;
  truncated.totalNc = null;
  rows.push(truncated);
  throwsCode(() => evaluate(rows as GateRow[]), 'diagnostic-required');
});

test('CertificationRejected corroborated by a rejected diagnostic -> CLOSE', () => {
  const base = failTimingSlot(buildDataset(), 'T0', 'paired', 'r9');
  const rows = [...base] as Writable[];
  const spec = STRATA.find((s) => s.name === 'paired')!;
  const truncated = baseRow('T0', spec, 1, 4320, 'fail1', 'hybrid');
  truncated.termination = 'CertificationRejected';
  truncated.note = 'truncated';
  truncated.maxCertSlack = null;
  truncated.totalNc = null;
  rows.push(truncated);
  const diag = baseRow('T0', spec, 1, 4320, 'diag1', 'hybrid');
  diag.termination = 'CertificationRejected';
  diag.note = 'diagnostic';
  diag.nodes = 9_000_000;
  diag.maxCertSlack = null;
  rows.push(diag);
  const v = evaluate(rows as GateRow[]);
  assert.equal(v.verdict, 'CLOSE');
  assert.equal(v.gate1.selected, null);
  assert.deepEqual(v.gate1.candidates, []); // Gate 2 stops Gate 1
});

test('CertificationRejected contradicted by a certifying diagnostic aborts', () => {
  const rows: Writable[] = [...addDiagnosticPair(
    buildDataset(), 'T0', 'paired', 'Certified')];
  for (const r of rows) {
    if (r.stratum === 'paired' && r.resolveId === 'fail1')
      r.termination = 'CertificationRejected';
  }
  throwsCode(() => evaluate(rows as GateRow[]), 'inconsistent-classification');
});

test('p95 and max are independent at n=30: one 11s tail resolve still passes', () => {
  // Exactly one of the 30 resolves (seed 4320, slot r9) lands at 11 s: the
  // 29th ordered value stays 8 s while the 30th is 11 s <= 12 s.
  const rows = buildDataset({
    mutate: (r, ctx) => {
      if (r.policy === 'hybrid' && ctx.spec.name === 'paired'
          && ctx.seed === 4320 && ctx.resolveId === 'r9')
        r.resolveWallS = 11;
    },
  });
  const v = evaluate(rows);
  assert.equal(v.verdict, 'PASS');
  const s = v.gate2.strata.find((x) => x.scheme === 'T0' && x.stratum === 'paired')!;
  assert.ok(s.p95S! <= 10);
  assert.ok(s.maxS! > 10 && s.maxS! <= 12);
});

test('p95 above 10s (all 30 Certified) needs the diagnostic and maps REDIRECT', () => {
  // Two resolves at 11 s force the 29th ordered value above 10 s while the
  // 30th stays <= 12 s.
  const slow: BuildOpts = {
    mutate: (r, ctx) => {
      if (r.policy === 'hybrid' && ctx.scheme === 'T0'
          && ctx.spec.name === 'paired'
          && ctx.seed === 4320 && (ctx.resolveId === 'r8' || ctx.resolveId === 'r9'))
        r.resolveWallS = 11;
    },
  };
  throwsCode(() => evaluate(buildDataset(slow)), 'diagnostic-required');
  const rows = [...buildDataset(slow)] as Writable[];
  const spec = STRATA.find((s) => s.name === 'paired')!;
  const diag = baseRow('T0', spec, 1, 4320, 'diag1', 'hybrid');
  diag.termination = 'Certified';
  diag.note = 'diagnostic';
  diag.nodes = 9_000_000;
  diag.infosets = 1_500_000;
  diag.peakBytes = 2_000_000_000;
  diag.resolveWallS = 25;
  rows.push(diag);
  const v = evaluate(rows as GateRow[]);
  assert.equal(v.gate2.status, 'REDIRECT');
  assert.equal(v.verdict, 'REDIRECT');
  const s = v.gate2.strata.find((x) => x.scheme === 'T0' && x.stratum === 'paired')!;
  assert.deepEqual(s.failures, ['wall-p95-or-max']);
});

test('a Certified resolve beyond the 1M-infoset envelope with diagnostic -> REDIRECT',
  () => {
    const base = failTimingSlot(buildDataset(), 'T0', 'paired', 'r9') as Writable[];
    // The failure slot is an envelope-exhausted certified... model it as a
    // truncated ResourceExhausted resolve plus a certifying diagnostic.
    const spec = STRATA.find((s) => s.name === 'paired')!;
    const truncated = baseRow('T0', spec, 1, 4320, 'fail1', 'hybrid');
    truncated.termination = 'ResourceExhausted';
    truncated.note = 'truncated';
    truncated.infosets = 1_000_001;
    truncated.maxCertSlack = null;
    truncated.totalNc = null;
    base.push(truncated);
    const diag = baseRow('T0', spec, 1, 4320, 'diag1', 'hybrid');
    diag.termination = 'Certified';
    diag.note = 'diagnostic';
    diag.nodes = 9_000_000;
    diag.infosets = 1_500_000;
    diag.peakBytes = 2_000_000_000;
    diag.resolveWallS = 25;
    base.push(diag);
    const v = evaluate(base as GateRow[]);
    assert.equal(v.verdict, 'REDIRECT');
    const s = v.gate2.strata.find((x) => x.scheme === 'T0' && x.stratum === 'paired')!;
    assert.equal(s.classification, 'budget-tight');
    assert.equal(s.envelopePass, false);
  });

test('a Certified cap0ref outside the 20M/8GiB/600s outer budget aborts', () => {
  const rows = buildDataset({
    mutate: (r, ctx) => {
      if (r.policy === 'cap0ref' && ctx.spec.name === 'paired' && ctx.seed === 4320)
        r.nodes = 20_000_001;
    },
  });
  throwsCode(() => evaluate(rows), 'cap0ref-outer-budget');
});

test('at most one diagnostic profile row per (candidate,stratum)', () => {
  const withOne = addDiagnosticPair(buildDataset(), 'T0', 'paired', 'Certified');
  const rows = addDiagnosticPair(withOne, 'T0', 'monotone', 'Certified');
  // Different strata: allowed.
  assert.equal(evaluate(rows).gate2.status, 'REDIRECT');
  // Second diagnostic on the SAME (T0, paired): rejected.
  const doubled = addDiagnosticPair(withOne, 'T0', 'paired', 'Certified', '2');
  throwsCode(() => evaluate(doubled), 'diagnostic-limit');
});

test('diagnostic quota is global: one per stratum across ALL candidates', () => {
  // Addendum section 3 freezes exactly one diagnostic per stratum IN TOTAL;
  // a second diagnostic for the same stratum on a DIFFERENT candidate is also
  // rejected (no budget hunting across T0..T2h).
  const withOne = addDiagnosticPair(buildDataset(), 'T0', 'paired', 'Certified');
  const cross = addDiagnosticPair(withOne, 'T1', 'paired', 'Certified', '2');
  throwsCode(() => evaluate(cross), 'diagnostic-limit');
});

test('diagnostic profile beyond its own bounds is bound-unreachable', () => {
  const base = failTimingSlot(buildDataset(), 'T0', 'paired', 'r9');
  const rows = [...base] as Writable[];
  const spec = STRATA.find((s) => s.name === 'paired')!;
  const truncated = baseRow('T0', spec, 1, 4320, 'fail1', 'hybrid');
  truncated.termination = 'SolveDeadline';
  truncated.note = 'truncated';
  truncated.maxCertSlack = null;
  truncated.totalNc = null;
  rows.push(truncated);
  const diag = baseRow('T0', spec, 1, 4320, 'diag1', 'hybrid');
  diag.termination = 'DiagnosticInconclusive';
  diag.note = 'diagnostic';
  diag.nodes = DIAG_MAX_NODES + 1;
  diag.maxCertSlack = null;
  rows.push(diag);
  const v = evaluate(rows as GateRow[]);
  assert.equal(v.verdict, 'CLOSE');
});

// --- Determinism, hand pair, trunk constants --------------------------------

test('coldcold/warmhint determinism booleans are enforced', () => {
  const cold = buildDataset({
    mutate: (r, ctx) => {
      if (r.policy === 'hybrid' && ctx.scheme === 'T0' && ctx.seed === 4320)
        r.coldcoldBitwise = false;
    },
  });
  throwsCode(() => evaluate(cold), 'determinism');
  const warm = buildDataset({
    mutate: (r, ctx) => {
      if (r.policy === 'hybrid' && ctx.scheme === 'T2t' && ctx.seed === 4320)
        r.warmhintBitwise = false;
    },
  });
  throwsCode(() => evaluate(warm), 'determinism');
  const margin = buildDataset({
    mutate: (r, ctx) => {
      if (r.policy === 'hybrid' && ctx.scheme === 'T0' && ctx.seed === 4320)
        r.maxMarginDiff = 2e-6;
    },
  });
  throwsCode(() => evaluate(margin), 'determinism');
  // Every Certified gate row, including variants and cap0ref, must be
  // cold-vs-cold bitwise identical.
  const variantCold = buildDataset({
    mutate: (r, ctx) => {
      if (r.policy === 'rivermerge' && ctx.scheme === 'T0' && ctx.seed === 4320)
        r.coldcoldBitwise = false;
    },
  });
  throwsCode(() => evaluate(variantCold), 'determinism');
  const refCold = buildDataset({
    mutate: (r, ctx) => {
      if (r.policy === 'cap0ref' && ctx.scheme === 'T0'
          && ctx.spec.name === 'paired' && ctx.seed === 4320)
        r.coldcoldBitwise = false;
    },
  });
  throwsCode(() => evaluate(refCold), 'determinism');
  // A Certified cap0ref row must still carry certifier evidence.
  const refSlack = buildDataset({
    mutate: (r, ctx) => {
      if (r.policy === 'cap0ref' && ctx.scheme === 'T0'
          && ctx.spec.name === 'paired' && ctx.seed === 4320)
        r.maxCertSlack = null;
    },
  });
  throwsCode(() => evaluate(refSlack), 'missing-field');
});

test('certification slack beyond 1e-6 root-pot is rejected', () => {
  const rows = buildDataset({
    mutate: (r, ctx) => {
      if (r.policy === 'hybrid' && ctx.scheme === 'T0' && ctx.seed === 4320)
        r.maxCertSlack = 2e-6;
    },
  });
  throwsCode(() => evaluate(rows), 'cert-slack');
});

test('behavioral hand pair requires argmax difference on its target stratum', () => {
  const rows = buildDataset({
    mutate: (r, ctx) => {
      if (r.policy === 'hybrid' && ctx.spec.name === MONOTONE_STRATUM)
        r.riverArgmaxDiff = false;
    },
  });
  throwsCode(() => evaluate(rows), 'handpair-argmax');
  // Fallback manifest targets unpaired-twotone instead.
  const fbRows = buildDataset({
    mutate: (r, ctx) => {
      if (r.policy === 'hybrid' && ctx.spec.name === 'unpaired-twotone')
        r.riverArgmaxDiff = true;
    },
  });
  const v = evaluate(fbRows, handPair(true));
  assert.equal(v.handpair.stratum, 'unpaired-twotone');
  assert.equal(v.verdict, 'PASS');
});

test('trunk_rows/trunk_bytes must be constant per candidate', () => {
  const rows = buildDataset({
    mutate: (r) => {
      if (r.scheme === 'T1' && r.stratum === 'paired')
        r.trunkRows = 201;
    },
  });
  throwsCode(() => evaluate(rows), 'trunk-inconsistency');
});

test('a missing candidate block aborts', () => {
  const rows = buildDataset({schemes: ['T0', 'T1', 'T2h']});
  throwsCode(() => evaluate(rows), 'missing-candidate');
});

// --- CSV loader and manifest -------------------------------------------------

function csvLine(r: GateRow): string {
  const b = (x: boolean | null): string => (x === null ? '' : x ? 'true' : 'false');
  const n = (x: number | null): string => (x === null ? '' : String(x));
  return [
    r.stratum, r.root, r.turnBoard, r.riverCard, r.range, r.scheme, r.trunk,
    r.cap, r.seed, r.resolveId, r.policy,
    r.trunkRows, r.trunkBytes,
    r.infosets, r.nodes, r.peakBytes, r.trainS, r.certifyS, r.resolveWallS,
    r.walkS, r.termination,
    b(r.coldcoldBitwise), b(r.warmhintBitwise), n(r.maxMarginDiff),
    n(r.maxCertSlack),
    n(r.totalNc), n(r.tTrunk), n(r.tBoundary), n(r.tRiver), n(r.tAction),
    n(r.extensionMass), n(r.telescopeResid),
    n(r.ncEndgameCap1), n(r.ncEndgameCap0),
    b(r.riverArgmaxDiff), r.note,
  ].join(',');
}

test('CSV loader requires the exact frozen header and parses rows', () => {
  const dir = mkdtempSync(join(tmpdir(), 'rfc0046-'));
  const path = join(dir, 'gates.csv');
  const rows = buildDataset().slice(0, 50);
  writeFileSync(path, [GATE_COLUMNS.join(','), ...rows.map(csvLine)].join('\n'));
  const loaded = loadGateCsv(path);
  assert.equal(loaded.length, 50);
  assert.equal(loaded[0]!.stratum, rows[0]!.stratum);
  assert.equal(loaded[0]!.coldcoldBitwise, true);

  const badHeader = join(dir, 'bad.csv');
  writeFileSync(badHeader,
    [GATE_COLUMNS.slice(1).join(','), csvLine(rows[0]!)].join('\n'));
  throwsCode(() => loadGateCsv(badHeader), 'header-mismatch');

  const reordered = join(dir, 'reordered.csv');
  const cols = [...GATE_COLUMNS];
  [cols[0], cols[1]] = [cols[1]!, cols[0]!];
  writeFileSync(reordered, [cols.join(','), csvLine(rows[0]!)].join('\n'));
  throwsCode(() => loadGateCsv(reordered), 'header-mismatch');
});

test('manifest loader pins the frozen constants and rejects tampering', () => {
  const dir = mkdtempSync(join(tmpdir(), 'rfc0046-'));
  const path = join(dir, 'handpair.json');
  writeFileSync(path, JSON.stringify({
    board: 'QsKsAsJs', river: '9c', seed: 4320, iterations: 2_000_000,
    reference_digest: 'sha256:x', combos: ['Ac Kd', 'Qc Jd'],
    category_id: 'tier-3', action_indices: [0, 2], value_gap: 0.01,
    fallback_used: false,
  }));
  const hp = loadHandPairManifest(path);
  assert.equal(hp.stratum, 'monotone');
  const bad = join(dir, 'bad.json');
  writeFileSync(bad, JSON.stringify({
    board: 'QsKsAsJs', river: '9c', seed: 4321, iterations: 2_000_000,
    reference_digest: 'sha256:x', combos: ['Ac Kd', 'Qc Jd'],
    category_id: 'tier-3', action_indices: [0, 2], value_gap: 0.01,
    fallback_used: false,
  }));
  throwsCode(() => loadHandPairManifest(bad), 'handpair-constants');
});
