// RFC 0009 W1 (accepted 2026-09-29) wiring tests against the scripted minor-2
// fake engine: the resident-root launch line, the minor-2 guarantee+digest
// decode onto ExecutableDecision, the solve-budget sanitizer, and the
// no-roots default staying byte-identical to the previous behavior.
import assert from 'node:assert/strict';
import { spawnSync } from 'node:child_process';
import { dirname, join, resolve } from 'node:path';
import test from 'node:test';
import { fileURLToPath } from 'node:url';
import { existsSync, mkdirSync, mkdtempSync, readFileSync, rmSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';

import { create } from '@bufbuild/protobuf';
import { ProtoEngineProcessClient } from '../../../clients/node/proto-engine-process-client.js';
import {
  ActionType,
  DecisionResponseSchema,
  EnvelopeSchema,
  GetCapabilitiesRequestSchema,
  SolverSource,
  StrategySchema,
} from '../../../build/generated/ts/bigshark/engine/v1/engine_pb.js';
import { closeEngine, decide, parseFlopLibraries, parseResidentRoots, protoEngineLaunchArgs, resolveFlopLibraries, autoDetectedFlopLibrary, warmupTimeoutMsFor, residentBudgetMiB } from '../src/engine.js';
import {
  decisionEnvelope,
  fromV1DecisionResponse,
  resolveSolveTimeBudgetMs,
  toV1DecisionRequest,
} from '../src/v1-mapper.js';
import type { GoldenFixture, RiverRoom } from '../src/types.js';

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

const fixtures = JSON.parse(readFileSync(
  join(root, 'platforms/river-club/tests/fixtures/v0-golden.json'),
  'utf8',
)) as GoldenFixture[];

const ROOT_A = 'a'.repeat(64);
const ROOT_B = 'b'.repeat(64);

function warmup() {
  const envelope = create(EnvelopeSchema, { protocolMinor: 0 });
  envelope.payload = {
    case: 'getCapabilitiesRequest',
    value: create(GetCapabilitiesRequestSchema),
  };
  envelope.requestId = 'warmup-w1';
  return envelope;
}

function minor2Client() {
  return new ProtoEngineProcessClient({
    command: process.execPath,
    args: [fakeEngine],
    warmupEnvelope: warmup(),
    warmupTimeoutMs: 5_000,
    negotiateMinor2: true,
  });
}

// ---- launch-line construction -------------------------------------------

test('the framed launch line is unchanged without roots and appends each root in order', () => {
  // No roots: byte-identical to the historical hardcoded launch line.
  assert.deepEqual(protoEngineLaunchArgs([]), ['--serve-proto']);
  // Roots append the host's documented repeatable argument, in order.
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
  // Explicit config wins.
  assert.deepEqual(
    parseResidentRoots({ residentRoots: [{ path: '/x/policy.db', sha256: ROOT_A }] }),
    [{ path: '/x/policy.db', sha256: ROOT_A }],
  );
  // Absent everywhere yields none.
  const prior = process.env.BIGSHARK_ENGINE_RESIDENT_ROOTS;
  delete process.env.BIGSHARK_ENGINE_RESIDENT_ROOTS;
  assert.deepEqual(parseResidentRoots({}), []);
  // Environment JSON array is accepted.
  process.env.BIGSHARK_ENGINE_RESIDENT_ROOTS =
    JSON.stringify([{ path: '/e/p.db', sha256: ROOT_B }]);
  assert.deepEqual(parseResidentRoots({}), [{ path: '/e/p.db', sha256: ROOT_B }]);
  // Unparseable environment yields none (declared fallback, never a crash).
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
  // No libraries: unchanged.
  assert.deepEqual(protoEngineLaunchArgs([]), ['--serve-proto']);
  // One library appends the flag and directory.
  assert.deepEqual(
    protoEngineLaunchArgs([], ['/path/to/library']),
    ['--serve-proto', '--flop-library', '/path/to/library'],
  );
  // Multiple libraries append in order.
  assert.deepEqual(
    protoEngineLaunchArgs([], ['/lib/a', '/lib/b']),
    ['--serve-proto', '--flop-library', '/lib/a', '--flop-library', '/lib/b'],
  );
  // Roots and libraries coexist on the same launch line.
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
  // Explicit config wins.
  assert.deepEqual(
    parseFlopLibraries({ flopLibraries: ['/path/to/library'] }),
    ['/path/to/library'],
  );
  // Absent everywhere yields none (auto-detect suppressed via null detector).
  const prior = process.env.BIGSHARK_ENGINE_FLOP_LIBRARIES;
  delete process.env.BIGSHARK_ENGINE_FLOP_LIBRARIES;
  assert.deepEqual(resolveFlopLibraries({}, () => null).libraries, []);
  // Environment JSON array is accepted.
  process.env.BIGSHARK_ENGINE_FLOP_LIBRARIES = JSON.stringify(['/env/lib']);
  assert.deepEqual(resolveFlopLibraries({}, () => null).libraries, ['/env/lib']);
  // Unparseable environment yields none (declared fallback, never a crash).
  process.env.BIGSHARK_ENGINE_FLOP_LIBRARIES = '{not json';
  assert.deepEqual(resolveFlopLibraries({}, () => null).libraries, []);
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
  assert.throws(() => resolveFlopLibraries({}, () => null), /non-empty strings/);
  if (prior === undefined) delete process.env.BIGSHARK_ENGINE_FLOP_LIBRARIES;
  else process.env.BIGSHARK_ENGINE_FLOP_LIBRARIES = prior;
});

// ---- auto-detection, budget, and warmup -----------------------------------

test('resolveFlopLibraries auto-detects the bundled library and attaches the 6 GiB budget', () => {
  const prior = process.env.BIGSHARK_ENGINE_FLOP_LIBRARIES;
  delete process.env.BIGSHARK_ENGINE_FLOP_LIBRARIES;

  // Build a temp repo root with artifacts/flop-library/manifest.json.
  const tempRoot = mkdtempSync(join(tmpdir(), 'bs-autodetect-'));
  const libDir = join(tempRoot, 'artifacts', 'flop-library');
  mkdirSync(libDir, { recursive: true });
  writeFileSync(join(libDir, 'manifest.json'), '{}');

  // Detector fires: library path + auto-detected budget.
  const detected = resolveFlopLibraries({}, () => autoDetectedFlopLibrary(tempRoot));
  assert.deepEqual(detected.libraries, [libDir]);
  assert.equal(detected.residentBudgetMiB, residentBudgetMiB());
  // The auto-detected budget is 1/8 of total RAM (≥ 1 GiB on any
  // machine that can hold the library).
  assert.ok(detected.residentBudgetMiB! >= 1024,
    `expected budget >= 1024 MiB, got ${detected.residentBudgetMiB}`);

  // No manifest: no libraries, no budget.
  const emptyRoot = mkdtempSync(join(tmpdir(), 'bs-autodetect-empty-'));
  const none = resolveFlopLibraries({}, () => autoDetectedFlopLibrary(emptyRoot));
  assert.deepEqual(none.libraries, []);
  assert.equal(none.residentBudgetMiB, undefined);

  // Explicit config wins over a firing detector (no budget attached).
  const explicit = resolveFlopLibraries(
    { flopLibraries: ['/explicit'] },
    () => autoDetectedFlopLibrary(tempRoot),
  );
  assert.deepEqual(explicit.libraries, ['/explicit']);
  assert.equal(explicit.residentBudgetMiB, undefined);

  // Explicit empty array is the config opt-out.
  const optedOut = resolveFlopLibraries(
    { flopLibraries: [] },
    () => autoDetectedFlopLibrary(tempRoot),
  );
  assert.deepEqual(optedOut.libraries, []);
  assert.equal(optedOut.residentBudgetMiB, undefined);

  // Env var set (even garbage) suppresses auto-detection.
  process.env.BIGSHARK_ENGINE_FLOP_LIBRARIES = '{not json';
  const envSuppressed = resolveFlopLibraries({}, () => autoDetectedFlopLibrary(tempRoot));
  assert.deepEqual(envSuppressed.libraries, []);
  assert.equal(envSuppressed.residentBudgetMiB, undefined);

  delete process.env.BIGSHARK_ENGINE_FLOP_LIBRARIES;
  if (prior !== undefined) process.env.BIGSHARK_ENGINE_FLOP_LIBRARIES = prior;
  rmSync(tempRoot, { recursive: true, force: true });
  rmSync(emptyRoot, { recursive: true, force: true });
});

test('residentBudgetMiB auto-detects RAM and honors the env override', () => {
  const prior = process.env.BIGSHARK_ENGINE_RESIDENT_BUDGET_MIB;
  delete process.env.BIGSHARK_ENGINE_RESIDENT_BUDGET_MIB;

  // Auto-detect: 1/8 of total RAM, at least 1 GiB.
  const auto = residentBudgetMiB();
  assert.ok(auto >= 1024, `expected auto budget >= 1024 MiB, got ${auto}`);

  // Env override: explicit positive integer.
  process.env.BIGSHARK_ENGINE_RESIDENT_BUDGET_MIB = '2048';
  assert.equal(residentBudgetMiB(), 2048);

  // Env override: invalid values fall back to auto-detect.
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
  // With budget: flag and value appended after libraries.
  assert.deepEqual(
    protoEngineLaunchArgs([], ['/lib'], 6144),
    ['--serve-proto', '--flop-library', '/lib', '--resident-budget', '6144'],
  );
  // Without budget: no flag (backward compatible).
  assert.deepEqual(
    protoEngineLaunchArgs([], ['/lib']),
    ['--serve-proto', '--flop-library', '/lib'],
  );
  // Roots + libraries + budget coexist.
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

test('warmupTimeoutMsFor scales with library presence', () => {
  assert.equal(warmupTimeoutMsFor([]), 10_000);
  assert.equal(warmupTimeoutMsFor(['/lib']), 300_000);
  assert.equal(warmupTimeoutMsFor(['/lib/a', '/lib/b']), 300_000);
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

test('toV1DecisionRequest carries the supplied budget and pins 2000 when absent', () => {
  const fixture = fixtures.find(f => f.name === 'multiway-flop-check-option')!;
  const withBudget = toV1DecisionRequest(fixture.state.room, { solveTimeBudgetMs: 1400 });
  assert.equal(withBudget.options?.solveTimeBudgetMs, 1400);
  const without = toV1DecisionRequest(fixture.state.room, {});
  assert.equal(without.options?.solveTimeBudgetMs, 2000);
});

// ---- provenance decode ---------------------------------------------------

test('minor-2 served decision attaches guaranteeLevel and artifactSha256', async t => {
  const client = minor2Client();
  t.after(() => client.stop());
  // The scripted 'certified' scenario answers CALL with field 11
  // certified_bound and a field-9 digest; the facing-raise fixture supplies a
  // room where call is legal.
  const fixture = fixtures.find(f => f.name === 'multiway-turn-facing-raise')!;
  const room = { ...fixture.state.room, handId: 'certified' };
  const decision = await decide(room, {
    style: 'tag',
    heroName: fixture.state.me.name,
    timeoutMs: 5_000,
    proto: true,
    protoEngineClient: client,
  });
  assert.equal(decision.action, 'call');
  assert.equal(decision.guaranteeLevel, 'certified_bound');
  assert.equal(decision.artifactSha256, 'c'.repeat(64));
});

test('a minor-2 echo without a digest attaches the level and no digest', async t => {
  const client = minor2Client();
  t.after(() => client.stop());
  const fixture = fixtures.find(f => f.name === 'multiway-flop-check-option')!;
  const decision = await decide(fixture.state.room, {
    style: 'tag',
    heroName: fixture.state.me.name,
    timeoutMs: 5_000,
    proto: true,
    protoEngineClient: client,
  });
  assert.equal(decision.action, 'check');
  // The scripted default minor-2 echo carries field 11 and no digest.
  assert.equal(decision.guaranteeLevel, 'approximate');
  assert.equal(decision.artifactSha256, undefined);
});

// The published fixture root as a River room: flop 2c 3d 7h, hero AsKs, check
// or bet, heads up, 5 BB, pot 20 (the same root the minor-2 walk test drives).
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
      position: 'SB',
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
        blind: 'SB',
        position: 'SB',
        cards: [{ rank: 'A', suit: 'spades' }, { rank: 'K', suit: 'spades' }],
      },
      {
        seat: 1,
        name: 'Villain',
        stack: 40,
        bet: 0,
        status: 'active',
        button: true,
        blind: 'BB',
        position: 'BB',
      },
    ],
    legal: {
      actions: ['check', 'bet'],
      call: 0,
      potOdds: 0,
      raiseTo: { min: 5, max: 40 },
      min: 5,
      max: 40,
      currentBet: 0,
      amountMeaning: 'target-total',
    },
  };
}

test('real binary: a configured resident root serves the fixture flop end-to-end with its digest', async t => {
  if (!fixture) {
    t.skip('engine or fixture binary unavailable');
    return;
  }
  const client = new ProtoEngineProcessClient({
    command: engineBinary,
    args: protoEngineLaunchArgs([{ path: fixture.path, sha256: fixture.sha256 }]),
    warmupEnvelope: warmup(),
    warmupTimeoutMs: 10_000,
    negotiateMinor1: true,
    negotiateMinor2: true,
  });
  t.after(() => client.stop());
  const room = residentRoom();
  const decision = await decide(room, {
    style: 'tag',
    heroName: 'Hero',
    timeoutMs: 10_000,
    proto: true,
    protoEngineClient: client,
    residentRoots: [{ path: fixture.path, sha256: fixture.sha256 }],
  });
  // The served action is one of the fixture root's legal choices, served FROM
  // the published blueprint (the reason names it), with the artifact's digest
  // and a field-11 guarantee level riding on the decision. The heuristic
  // mappers never set field 9, so a digest match cannot pass spuriously on an
  // uncovered fallback.
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
  // No protoEngineClient override: decide() must construct the implicit
  // process client from the configured roots (the path the runner ships).
  // flopLibraries: [] suppresses auto-detection so the implicit client
  // launches with only the fixture root, not the 1,755-class library.
  const room = residentRoom();
  const decision = await decide(room, {
    style: 'tag',
    heroName: 'Hero',
    timeoutMs: 10_000,
    proto: true,
    residentRoots: [{ path: fixture.path, sha256: fixture.sha256 }],
    flopLibraries: [],
  });
  assert.match(decision.reason, /blueprint/,
    `expected the implicit client to serve the blueprint, got ${decision.reason}`);
  assert.equal(decision.artifactSha256, fixture.sha256);
  // A second decision reuses the keyed singleton (no relaunch, same answer
  // class); this is the regression the explicit-client tests cannot see.
  const again = await decide(room, {
    style: 'tag',
    heroName: 'Hero',
    timeoutMs: 10_000,
    proto: true,
    residentRoots: [{ path: fixture.path, sha256: fixture.sha256 }],
    flopLibraries: [],
  });
  assert.match(again.reason, /blueprint/);
  assert.equal(again.artifactSha256, fixture.sha256);
});

test('the level and digest gates are minor-2-only (mapper discipline)', () => {
  const fixture = fixtures.find(f => f.name === 'multiway-flop-check-option')!;
  const request = toV1DecisionRequest(fixture.state.room, {});
  const envelope = decisionEnvelope(request, 2);
  assert.equal(envelope.payload.case, 'decisionRequest');
  // Replace the request payload with a hand-built strategy response: check,
  // field 11 present, field 9 present.
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
  const built = create(EnvelopeSchema, { protocolMinor: 2 });
  built.payload = { case: 'decisionResponse', value: response };

  const atMinor2 = fromV1DecisionResponse(built, fixture.state.room, 2);
  assert.equal(atMinor2.action, 'check');
  assert.equal(atMinor2.guaranteeLevel, 'approximate');
  assert.equal(atMinor2.artifactSha256, 'd'.repeat(64));

  const atMinor0 = fromV1DecisionResponse(built, fixture.state.room, 0);
  assert.equal(atMinor0.action, 'check');
  assert.equal(atMinor0.guaranteeLevel, undefined);
  assert.equal(atMinor0.artifactSha256, undefined);
});
