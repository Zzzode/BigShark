// Wiring tests against the scripted fake engine: the resident-root launch
// line, the guarantee+digest decode onto ExecutableDecision, the solve-budget
// sanitizer, and the no-roots default launch line.
import assert from 'node:assert/strict';
import { spawnSync } from 'node:child_process';
import { dirname, join, resolve } from 'node:path';
import test from 'node:test';
import { fileURLToPath } from 'node:url';
import { existsSync, mkdirSync, mkdtempSync, readFileSync, rmSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';

import { create } from '@bufbuild/protobuf';
import { ProtoEngineProcessClient, warmupEnvelope } from '../../../clients/node/proto-engine-process-client.js';
import {
  ActionType,
  DecisionResponseSchema,
  EnvelopeSchema,
  GetCapabilitiesRequestSchema,
  SolverSource,
  StrategySchema,
} from '../../../build/generated/ts/bigshark/engine/v2/engine_pb.js';
import { closeEngine, decide, parseFlopLibraries, parseResidentRoots, protoEngineLaunchArgs, resolveFlopLibraries, autoDetectedFlopLibrary, autoDetectedFlopLibraries, warmupTimeoutMsFor, residentBudgetMiB, warmEngine } from '../src/engine.js';
import {
  decisionEnvelope,
  fromDecisionResponse,
  resolveSolveTimeBudgetMs,
  toDecisionRequest,
} from '../src/mapper.js';
import { observationLedger } from '../src/observation.js';
import type { RiverRoom } from '../src/types.js';

const here = dirname(fileURLToPath(import.meta.url));
const root = join(here, '../../../../');
const fakeEngine = join(root, 'dist/clients/node/tests/fixtures/fake-proto-engine.js');
const engineBinary = process.env.BIGSHARK_ENGINE_BINARY
  || join(root, 'bin', 'bigshark-engine');
const fixturePublisher = process.env.BS_RESIDENT_FIXTURE_BIN
  || join(root, 'bin', 'bs-resident-fixture');

interface PublishedFixture { path: string; sha256: string }

function publishFixture(): PublishedFixture | null {
  if (!existsSync(engineBinary) || !existsSync(fixturePublisher)) return null;
  const dir = mkdtempSync(join(tmpdir(), 'bs-w1-resident-'));
  const published = spawnSync(fixturePublisher, [dir], { encoding: 'utf8' });
  if (published.status !== 0)
    throw new Error(`fixture publisher failed: ${published.stderr}`);
  return JSON.parse(published.stdout.trim()) as PublishedFixture;
}

const fixture = publishFixture();

const ROOT_A = 'a'.repeat(64);
const ROOT_B = 'b'.repeat(64);

function scriptedClient() {
  return new ProtoEngineProcessClient({
    command: process.execPath,
    args: [fakeEngine],
    warmupEnvelope: warmupEnvelope(),
    warmupTimeoutMs: 5_000,
  });
}

// ---- launch-line construction -------------------------------------------

test('the launch line is unchanged without roots and appends each root in order', () => {
  assert.deepEqual(protoEngineLaunchArgs([]), ['--serve-proto']);
  assert.deepEqual(
    protoEngineLaunchArgs([
      { path: '/x/policy.db', sha256: ROOT_A },
      { path: '/y/other.db', sha256: ROOT_B },
    ]),
    [
      '--serve-proto',
      '--resident-root', `/x/policy.db=${ROOT_A}`,
      '--resident-root', `/y/other.db=${ROOT_B}`,
    ],
  );
});

test('parseResidentRoots validates the config surface (explicit over env)', () => {
  assert.deepEqual(
    parseResidentRoots({ residentRoots: [{ path: '/x/policy.db', sha256: ROOT_A }] }),
    [{ path: '/x/policy.db', sha256: ROOT_A }],
  );
  const prior = process.env.BIGSHARK_ENGINE_RESIDENT_ROOTS;
  delete process.env.BIGSHARK_ENGINE_RESIDENT_ROOTS;
  assert.deepEqual(parseResidentRoots({}), []);
  process.env.BIGSHARK_ENGINE_RESIDENT_ROOTS =
    JSON.stringify([{ path: '/e/p.db', sha256: ROOT_B }]);
  assert.deepEqual(parseResidentRoots({}), [{ path: '/e/p.db', sha256: ROOT_B }]);
  process.env.BIGSHARK_ENGINE_RESIDENT_ROOTS = '{not json';
  assert.deepEqual(parseResidentRoots({}), []);
  delete process.env.BIGSHARK_ENGINE_RESIDENT_ROOTS;
  if (prior !== undefined) process.env.BIGSHARK_ENGINE_RESIDENT_ROOTS = prior;
});

test('parseResidentRoots rejects a malformed pin instead of silently dropping it', () => {
  assert.throws(
    () => parseResidentRoots({ residentRoots: [{ path: '/x/p.db', sha256: 'AB'.repeat(32) }] }),
    /64-lowercase-hex/,
  );
  assert.throws(
    () => parseResidentRoots({ residentRoots: [{ path: '', sha256: ROOT_A }] }),
    /nonempty path/,
  );
  const prior = process.env.BIGSHARK_ENGINE_RESIDENT_ROOTS;
  process.env.BIGSHARK_ENGINE_RESIDENT_ROOTS = JSON.stringify([{ path: '/x', sha256: 'zz' }]);
  assert.throws(() => parseResidentRoots({}), /64-lowercase-hex/);
  if (prior === undefined) delete process.env.BIGSHARK_ENGINE_RESIDENT_ROOTS;
  else process.env.BIGSHARK_ENGINE_RESIDENT_ROOTS = prior;
});

// ---- flop library launch-line and parsing --------------------------------

test('the launch line appends --flop-library for each library directory', () => {
  assert.deepEqual(protoEngineLaunchArgs([]), ['--serve-proto']);
  assert.deepEqual(
    protoEngineLaunchArgs([], ['/path/to/library']),
    ['--serve-proto', '--flop-library', '/path/to/library'],
  );
  assert.deepEqual(
    protoEngineLaunchArgs([], ['/lib/a', '/lib/b']),
    ['--serve-proto', '--flop-library', '/lib/a', '--flop-library', '/lib/b'],
  );
  assert.deepEqual(
    protoEngineLaunchArgs(
      [{ path: '/x/policy.db', sha256: ROOT_A }],
      ['/lib/a'],
    ),
    [
      '--serve-proto',
      '--resident-root', `/x/policy.db=${ROOT_A}`,
      '--flop-library', '/lib/a',
    ],
  );
});

test('parseFlopLibraries validates the config surface (explicit over env)', () => {
  assert.deepEqual(
    parseFlopLibraries({ flopLibraries: ['/path/to/library'] }),
    ['/path/to/library'],
  );
  const prior = process.env.BIGSHARK_ENGINE_FLOP_LIBRARIES;
  delete process.env.BIGSHARK_ENGINE_FLOP_LIBRARIES;
  assert.deepEqual(resolveFlopLibraries({}, () => []).libraries, []);
  process.env.BIGSHARK_ENGINE_FLOP_LIBRARIES = JSON.stringify(['/env/lib']);
  assert.deepEqual(resolveFlopLibraries({}, () => []).libraries, ['/env/lib']);
  process.env.BIGSHARK_ENGINE_FLOP_LIBRARIES = '{not json';
  assert.deepEqual(resolveFlopLibraries({}, () => []).libraries, []);
  delete process.env.BIGSHARK_ENGINE_FLOP_LIBRARIES;
  if (prior !== undefined) process.env.BIGSHARK_ENGINE_FLOP_LIBRARIES = prior;
});

test('parseFlopLibraries rejects malformed entries instead of silently dropping them', () => {
  assert.throws(
    () => parseFlopLibraries({ flopLibraries: [''] }),
    /non-empty strings/,
  );
  assert.throws(
    () => parseFlopLibraries({ flopLibraries: [42] as unknown as string[] }),
    /non-empty strings/,
  );
  const prior = process.env.BIGSHARK_ENGINE_FLOP_LIBRARIES;
  process.env.BIGSHARK_ENGINE_FLOP_LIBRARIES = JSON.stringify(['ok', '']);
  assert.throws(() => resolveFlopLibraries({}, () => []), /non-empty strings/);
  if (prior === undefined) delete process.env.BIGSHARK_ENGINE_FLOP_LIBRARIES;
  else process.env.BIGSHARK_ENGINE_FLOP_LIBRARIES = prior;
});

// ---- auto-detection, budget, and warmup -----------------------------------

test('resolveFlopLibraries auto-detects bundled libraries and attaches the budget', () => {
  const prior = process.env.BIGSHARK_ENGINE_FLOP_LIBRARIES;
  delete process.env.BIGSHARK_ENGINE_FLOP_LIBRARIES;

  const tempRoot = mkdtempSync(join(tmpdir(), 'bs-autodetect-'));
  const libDir = join(tempRoot, 'artifacts', 'flop-library');
  mkdirSync(libDir, { recursive: true });
  writeFileSync(join(libDir, 'manifest.json'), '{}');

  const detected = resolveFlopLibraries({}, () => autoDetectedFlopLibraries(tempRoot));
  assert.deepEqual(detected.libraries, [libDir]);
  assert.equal(detected.residentBudgetMiB, residentBudgetMiB());
  assert.ok(detected.residentBudgetMiB! >= 2048,
    `expected budget >= 2048 MiB, got ${detected.residentBudgetMiB}`);

  const emptyRoot = mkdtempSync(join(tmpdir(), 'bs-autodetect-empty-'));
  const none = resolveFlopLibraries({}, () => autoDetectedFlopLibraries(emptyRoot));
  assert.deepEqual(none.libraries, []);
  assert.equal(none.residentBudgetMiB, undefined);

  const explicit = resolveFlopLibraries(
    { flopLibraries: ['/explicit'] },
    () => autoDetectedFlopLibraries(tempRoot),
  );
  assert.deepEqual(explicit.libraries, ['/explicit']);
  assert.equal(explicit.residentBudgetMiB, undefined);

  const optedOut = resolveFlopLibraries(
    { flopLibraries: [] },
    () => autoDetectedFlopLibraries(tempRoot),
  );
  assert.deepEqual(optedOut.libraries, []);
  assert.equal(optedOut.residentBudgetMiB, undefined);

  process.env.BIGSHARK_ENGINE_FLOP_LIBRARIES = '{not json';
  const envSuppressed = resolveFlopLibraries({}, () => autoDetectedFlopLibraries(tempRoot));
  assert.deepEqual(envSuppressed.libraries, []);
  assert.equal(envSuppressed.residentBudgetMiB, undefined);

  delete process.env.BIGSHARK_ENGINE_FLOP_LIBRARIES;
  if (prior !== undefined) process.env.BIGSHARK_ENGINE_FLOP_LIBRARIES = prior;
  rmSync(tempRoot, { recursive: true, force: true });
  rmSync(emptyRoot, { recursive: true, force: true });
});

test('autoDetectedFlopLibraries finds multiple libraries sorted by name', () => {
  const tempRoot = mkdtempSync(join(tmpdir(), 'bs-multilib-'));
  const lib2p = join(tempRoot, 'artifacts', 'flop-library');
  const lib3p = join(tempRoot, 'artifacts', 'flop-library-3p');
  mkdirSync(lib2p, { recursive: true });
  mkdirSync(lib3p, { recursive: true });
  writeFileSync(join(lib2p, 'manifest.json'), '{}');
  writeFileSync(join(lib3p, 'manifest.json'), '{}');

  const found = autoDetectedFlopLibraries(tempRoot);
  assert.deepEqual(found, [lib2p, lib3p]);

  const noManifest = join(tempRoot, 'artifacts', 'flop-library-empty');
  mkdirSync(noManifest, { recursive: true });
  assert.deepEqual(autoDetectedFlopLibraries(tempRoot), [lib2p, lib3p]);

  rmSync(tempRoot, { recursive: true, force: true });
});

test('residentBudgetMiB auto-detects RAM and honors the env override', () => {
  const prior = process.env.BIGSHARK_ENGINE_RESIDENT_BUDGET_MIB;
  delete process.env.BIGSHARK_ENGINE_RESIDENT_BUDGET_MIB;

  const auto = residentBudgetMiB();
  assert.ok(auto >= 2048, `expected auto budget >= 2048 MiB, got ${auto}`);

  process.env.BIGSHARK_ENGINE_RESIDENT_BUDGET_MIB = '2048';
  assert.equal(residentBudgetMiB(), 2048);

  process.env.BIGSHARK_ENGINE_RESIDENT_BUDGET_MIB = 'abc';
  assert.equal(residentBudgetMiB(), auto);
  process.env.BIGSHARK_ENGINE_RESIDENT_BUDGET_MIB = '0';
  assert.equal(residentBudgetMiB(), auto);
  process.env.BIGSHARK_ENGINE_RESIDENT_BUDGET_MIB = '-5';
  assert.equal(residentBudgetMiB(), auto);

  delete process.env.BIGSHARK_ENGINE_RESIDENT_BUDGET_MIB;
  if (prior !== undefined) process.env.BIGSHARK_ENGINE_RESIDENT_BUDGET_MIB = prior;
});

test('the launch line appends --resident-budget only when a budget is given', () => {
  assert.deepEqual(
    protoEngineLaunchArgs([], ['/lib'], 6144),
    ['--serve-proto', '--flop-library', '/lib', '--resident-budget', '6144'],
  );
  assert.deepEqual(
    protoEngineLaunchArgs([], ['/lib']),
    ['--serve-proto', '--flop-library', '/lib'],
  );
  assert.deepEqual(
    protoEngineLaunchArgs(
      [{ path: '/x/policy.db', sha256: ROOT_A }],
      ['/lib/a'],
      2048,
    ),
    [
      '--serve-proto',
      '--resident-root', `/x/policy.db=${ROOT_A}`,
      '--flop-library', '/lib/a',
      '--resident-budget', '2048',
    ],
  );
});

test('warmupTimeoutMsFor scales with library count', () => {
  assert.equal(warmupTimeoutMsFor([]), 10_000);
  assert.equal(warmupTimeoutMsFor(['/lib']), 300_000);
  assert.equal(warmupTimeoutMsFor(['/lib/a', '/lib/b']), 600_000);
  assert.equal(warmupTimeoutMsFor(['/a', '/b', '/c']), 600_000);
});

test('warmupTimeoutMsFor includes pinned roots without changing library-only allowances', () => {
  const roots = [
    { path: '/x/policy.db', sha256: ROOT_A },
    { path: '/y/policy.db', sha256: ROOT_B },
  ];
  assert.equal(warmupTimeoutMsFor([], []), 10_000);
  assert.equal(warmupTimeoutMsFor([], roots.slice(0, 1)), 300_000);
  assert.equal(warmupTimeoutMsFor([], roots), 600_000);
  assert.equal(warmupTimeoutMsFor(['/lib'], roots.slice(0, 1)), 600_000);
});

// ---- solve budget sanitizer ---------------------------------------------

test('resolveSolveTimeBudgetMs clamps into the declared 1..120000 window', () => {
  assert.equal(resolveSolveTimeBudgetMs(undefined), 2000);
  assert.equal(resolveSolveTimeBudgetMs(Number.NaN), 2000);
  assert.equal(resolveSolveTimeBudgetMs(12.5), 2000);
  assert.equal(resolveSolveTimeBudgetMs(0), 1);
  assert.equal(resolveSolveTimeBudgetMs(-500), 1);
  assert.equal(resolveSolveTimeBudgetMs(120_001), 120_000);
  assert.equal(resolveSolveTimeBudgetMs(1500), 1500);
});

test('toDecisionRequest carries the supplied budget and pins 2000 when absent', () => {
  const room = residentRoom();
  const withBudget = toDecisionRequest(room, { solveTimeBudgetMs: 1400 });
  assert.equal(withBudget.options?.solveTimeBudgetMs, 1400);
  const without = toDecisionRequest(room, {});
  assert.equal(without.options?.solveTimeBudgetMs, 2000);
});

// ---- provenance decode ---------------------------------------------------

test('served decision attaches guaranteeLevel and artifactSha256', async t => {
  const client = scriptedClient();
  t.after(() => client.stop());
  // The scripted 'expanded' scenario answers BET with blueprint source,
  // artifactSha256, and guaranteeLevel 'approximate'.
  const room = { ...residentRoom(), handId: 'expanded' };
  const decision = await decide(room, {
    style: 'tag',
    heroName: 'Hero',
    timeoutMs: 5_000,
    engineClient: client,
  });
  assert.equal(decision.action, 'bet');
  assert.equal(decision.guaranteeLevel, 'approximate');
  assert.equal(decision.artifactSha256, '0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef');
});

test('an echo without a digest attaches the level and no digest', async t => {
  const client = scriptedClient();
  t.after(() => client.stop());
  const room = residentRoom();
  const decision = await decide(room, {
    style: 'tag',
    heroName: 'Hero',
    timeoutMs: 5_000,
    engineClient: client,
  });
  assert.equal(decision.action, 'check');
  assert.equal(decision.guaranteeLevel, 'approximate');
  assert.equal(decision.artifactSha256, undefined);
});

// The published fixture root as a River room: flop 2c 3d 7h, hero AsKs, check
// or bet, heads up, 5 BB, pot 20. Heads-up convention: the button posts the
// small blind. Preflop: SB (Villain) raises to 10, BB (Hero) calls; pot 20.
function residentRoom(): RiverRoom {
  return {
    id: 'w1-resident-room',
    name: 'W1 resident wiring',
    mode: 'playing',
    next: 'act',
    revision: 1,
    handId: 'w1-resident',
    street: 'flop',
    blinds: [2, 5],
    timeLeftMs: 20_000,
    hero: {
      seat: 0,
      name: 'Hero',
      stack: 40,
      bet: 0,
      cards: [{ rank: 'A', suit: 'spades' }, { rank: 'K', suit: 'spades' }],
      status: 'active',
      position: 'BB',
    },
    dealer: 1,
    actor: 0,
    board: [
      { rank: '2', suit: 'clubs' },
      { rank: '3', suit: 'diamonds' },
      { rank: '7', suit: 'hearts' },
    ],
    pot: 20,
    pots: [{ size: 20, eligible: [0, 1] }],
    seats: [
      {
        seat: 0,
        name: 'Hero',
        stack: 40,
        bet: 0,
        status: 'active',
        agent: true,
        blind: 'BB',
        position: 'BB',
        cards: [{ rank: 'A', suit: 'spades' }, { rank: 'K', suit: 'spades' }],
      },
      {
        seat: 1,
        name: 'Villain',
        stack: 40,
        bet: 0,
        status: 'active',
        button: true,
        blind: 'SB',
        position: 'SB',
      },
    ],
    events: [
      { kind: 'raise', text: 'Villain raises to 10' },
      { kind: 'call', text: 'Hero calls 5' },
    ],
    legal: {
      actions: ['check', 'bet'],
      call: 0,
      potOdds: 0,
      raiseTo: { min: 5, max: 200 },
      min: 5,
      max: 200,
      currentBet: 0,
      amountMeaning: 'target-total',
    },
  };
}

// RFC 0022: the resident blueprint path replays from hand start and requires
// FULL observation coverage. Simulate the runner's intake: a fresh controller
// observes the hand from its first revision before the actionable snapshot,
// so the ledger's provenance is honestly FULL with an accurate event count.
function observeFixtureHand(room: RiverRoom): void {
  observationLedger.markRestart();
  // Hand start: revision 0, no actions, no board.
  observationLedger.ingest({ room: { ...room, revision: 0, events: [], board: [] } });
  // Actionable snapshot: the actions and the flop deal were observed.
  observationLedger.ingest({ room });
}

test('real binary: a configured resident root serves the fixture flop end-to-end with its digest', async t => {
  if (!fixture) {
    t.skip('engine or fixture binary unavailable');
    return;
  }
  const client = new ProtoEngineProcessClient({
    command: engineBinary,
    args: protoEngineLaunchArgs([{ path: fixture.path, sha256: fixture.sha256 }]),
    warmupEnvelope: warmupEnvelope(),
    warmupTimeoutMs: warmupTimeoutMsFor([], [{ path: fixture.path, sha256: fixture.sha256 }]),
  });
  t.after(() => client.stop());
  await client.start();
  const room = residentRoom();
  observeFixtureHand(room);
  const decision = await decide(room, {
    style: 'tag',
    heroName: 'Hero',
    timeoutMs: 10_000,
    engineClient: client,
    residentRoots: [{ path: fixture.path, sha256: fixture.sha256 }],
  });
  assert.ok(decision.action === 'check' || decision.action === 'bet',
    `unexpected action ${decision.action}`);
  assert.match(decision.reason, /blueprint/,
    `expected a blueprint-served decision, got ${decision.reason}`);
  assert.equal(decision.artifactSha256, fixture.sha256);
  assert.ok(decision.guaranteeLevel !== undefined
    && decision.guaranteeLevel !== 'operational_fallback',
  `expected a served level, got ${String(decision.guaranteeLevel)} (${decision.reason})`);
});

test('real binary: the IMPLICIT client path serves the fixture root (singleton launch line)', async t => {
  if (!fixture) {
    t.skip('engine or fixture binary unavailable');
    return;
  }
  t.after(() => closeEngine());
  // No engineClient override: decide() must construct the implicit
  // process client from the configured roots (the path the runner ships).
  // flopLibraries: [] suppresses auto-detection so the implicit client
  // launches with only the fixture root, not the 1,755-class library.
  const room = residentRoom();
  observeFixtureHand(room);
  const config = {
    style: 'tag',
    heroName: 'Hero',
    timeoutMs: 10_000,
    residentRoots: [{ path: fixture.path, sha256: fixture.sha256 }],
    flopLibraries: [],
  };
  await warmEngine(config);
  const decision = await decide(room, config);
  assert.match(decision.reason, /blueprint/,
    `expected the implicit client to serve the blueprint, got ${decision.reason}`);
  assert.equal(decision.artifactSha256, fixture.sha256);
  const again = await decide(room, config);
  assert.match(again.reason, /blueprint/);
  assert.equal(again.artifactSha256, fixture.sha256);
});

test('guarantee level and digest are decoded from the engine response', () => {
  const room = residentRoom();
  const request = toDecisionRequest(room, {});
  const envelope = decisionEnvelope(request);
  assert.equal(envelope.payload.case, 'decisionRequest');
  const response = create(DecisionResponseSchema);
  response.result = {
    case: 'strategy',
    value: create(StrategySchema, {
      actions: [{ type: ActionType.CHECK, probability: 1 }],
      selectedAction: { type: ActionType.CHECK },
      solver: {
        source: SolverSource.POSTFLOP_HEURISTIC,
        reasonCode: 'postflop-heuristic',
        guaranteeLevel: 'approximate',
        artifactSha256: 'd'.repeat(64),
      },
    }),
  };
  const built = create(EnvelopeSchema, { protocolVersion: 3 });
  built.payload = { case: 'decisionResponse', value: response };

  const decoded = fromDecisionResponse(built, room);
  assert.equal(decoded.action, 'check');
  assert.equal(decoded.guaranteeLevel, 'approximate');
  assert.equal(decoded.artifactSha256, 'd'.repeat(64));
});
