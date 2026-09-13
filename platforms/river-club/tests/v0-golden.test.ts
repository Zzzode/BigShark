import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { dirname, join, resolve } from 'node:path';
import test from 'node:test';
import { fileURLToPath } from 'node:url';

import { EngineProcessClient } from '../../../clients/node/engine-process-client.js';
import {
  buildContext,
  decide,
} from '../src/engine.js';
import type {
  GoldenFixture,
  RawEngineDecision,
  V0DecisionContext,
} from '../src/types.js';

const here = dirname(fileURLToPath(import.meta.url));
const root = resolve(here, '../../../..');
const engineBinary = process.env.BIGSHARK_ENGINE_BINARY
  || join(root, 'bin', 'bigshark-engine');
const fixtures = JSON.parse(readFileSync(
  join(root, 'platforms', 'river-club', 'tests', 'fixtures', 'v0-golden.json'),
  'utf8',
)) as GoldenFixture[];

function createEngineClient(): EngineProcessClient<
  V0DecisionContext,
  RawEngineDecision
> {
  return new EngineProcessClient({
    command: engineBinary,
    warmupRequest: {
      handId: 'warmup',
      revision: 0,
      style: 'tag',
      street: 'preflop',
      blinds: [10, 20],
      position: 'BTN',
      playersInHand: 2,
      effectiveStackBb: 100,
      hole: [],
      board: [],
      pot: 0,
      riverGtoOn: false,
      raises: 0,
      openerPosition: '',
      heroWasRaiser: false,
      heroPreflopAggressor: false,
      limpers: 0,
    },
    warmupTimeoutMs: 10_000,
  });
}

for (const fixture of fixtures) {
  test(`preserves River v0 behavior: ${fixture.name}`, async t => {
    const engineClient = createEngineClient();
    t.after(() => engineClient.stop());
    const config = {
      style: 'tag',
      heroName: fixture.state.me.name,
      timeoutMs: 10_000,
      engineClient,
    };
    assert.deepEqual(buildContext(fixture.state.room, config), fixture.context);
    const actual = await decide(fixture.state.room, config);
    if (process.env.BIGSHARK_ALLOW_CFR_FALLBACK === '1'
      && fixture.decision.reason.startsWith('cpp:gto')
      && actual.reason.startsWith('cpp:gto-cfr')) {
      assert.equal(actual.action, fixture.decision.action);
      assert.equal(actual.amount, fixture.decision.amount);
    } else {
      assert.deepEqual(actual, fixture.decision);
    }
  });
}

test('fixtures contain no source identities or localized text', () => {
  const serialized = JSON.stringify(fixtures);
  assert.doesNotMatch(serialized, /\p{Script=Han}/u);
  assert.doesNotMatch(serialized, /feishu:|ou_[a-z0-9]+/);
});

test('unsupported river action lines disable the v0 solver', () => {
  const source = fixtures.find(
    fixture => fixture.name === 'heads-up-river-check-option',
  );
  assert.ok(source);
  const room = structuredClone(source.state.room);
  room.events = [
    { kind: 'call', text: 'Opponent call' },
    { kind: 'check', text: 'Hero check' },
    { kind: 'check', text: 'Hero check' },
    { kind: 'check', text: 'Opponent check' },
    { kind: 'check', text: 'Hero check' },
    { kind: 'check', text: 'Opponent check' },
    { kind: 'check', text: 'Hero check' },
    { kind: 'raise', text: 'Opponent raise' },
  ];

  assert.equal(buildContext(room, {
    style: 'tag',
    heroName: 'Hero',
  }).riverGtoOn, false);
});
