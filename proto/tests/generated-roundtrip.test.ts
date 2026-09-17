import {
  fromBinary,
  fromJson,
  toBinary,
  toJson,
  type JsonValue,
} from '@bufbuild/protobuf';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { join } from 'node:path';
import test from 'node:test';

import {
  ActionType,
  CardSchema,
  EnvelopeSchema,
  SolverMode,
  SolverSource,
  Suit,
} from '../../build/generated/ts/bigshark/engine/v1/engine_pb.js';

const fixtureDirectory = join(process.cwd(), 'proto', 'tests', 'fixtures');
const envelopeFixtures = [
  'capabilities-request',
  'capabilities-response',
  'capabilities-minor1',
  'decision-request',
  'envelope',
  'error-response',
  'expanded-strategy',
  'expanded-strategy-bound',
  'expanded-strategy-baseline',
];

for (const name of envelopeFixtures) {
  test(`TypeScript preserves the ${name} binary and ProtoJSON vector`, () => {
    const json = JSON.parse(
      readFileSync(join(fixtureDirectory, `${name}.json`), 'utf8'),
    ) as JsonValue;
    const binary = readFileSync(join(fixtureDirectory, `${name}.binpb`));
    const fromJsonMessage = fromJson(EnvelopeSchema, json);
    const fromBinaryMessage = fromBinary(EnvelopeSchema, binary);

    assert.deepEqual(
      Buffer.from(toBinary(EnvelopeSchema, fromJsonMessage)),
      binary,
    );
    assert.deepEqual(toJson(EnvelopeSchema, fromBinaryMessage), json);
  });
}

test('TypeScript preserves exact strategy integers and oneofs', () => {
  const binary = readFileSync(join(fixtureDirectory, 'envelope.binpb'));
  const fromBinaryMessage = fromBinary(EnvelopeSchema, binary);
  assert.equal(fromBinaryMessage.payload.case, 'decisionResponse');

  const result = fromBinaryMessage.payload.value.result;
  assert.equal(result.case, 'strategy');
  assert.equal(result.value.actions[0]?.type, ActionType.RAISE);
  assert.equal(result.value.actions[0]?.targetTotal, 9_007_199_254_740_993n);
  assert.equal(result.value.actions[0]?.expectedValue, -42n);
});

const cppVector = process.env.BIGSHARK_CPP_PROTO_VECTOR;
if (cppVector) {
  test('TypeScript consumes the C++-generated capability vector', () => {
    const binary = readFileSync(cppVector);
    const envelope = fromBinary(EnvelopeSchema, binary);
    assert.equal(envelope.requestId, 'cpp-vector');
    assert.equal(envelope.payload.case, 'getCapabilitiesResponse');
    assert.equal(envelope.payload.value.maximumRequestBytes, 1_048_576n);
    assert.deepEqual(
      Buffer.from(toBinary(EnvelopeSchema, envelope)),
      binary,
    );
  });
}

test('TypeScript preserves the full expanded_strategy distribution vector', () => {
  const binary = readFileSync(
    join(fixtureDirectory, 'expanded-strategy.binpb'),
  );
  const envelope = fromBinary(EnvelopeSchema, binary);
  assert.equal(envelope.protocolMinor, 1);
  assert.equal(envelope.payload.case, 'decisionResponse');
  const result = envelope.payload.value.result;
  assert.equal(result.case, 'expandedStrategy');
  if (result.case !== 'expandedStrategy') return;
  const expanded = result.value;
  assert.equal(expanded.actions.length, 7, 'seven-action distribution survives');
  let sum = 0;
  for (const policy of expanded.actions) sum += policy.probability;
  assert.ok(Math.abs(sum - 1) < 1e-12, 'probabilities sum to one');
  assert.equal(expanded.actions[0]?.type, ActionType.FOLD);
  assert.equal(expanded.actions[6]?.type, ActionType.RAISE);
  assert.equal(expanded.actions[6]?.targetTotal, 120n);
  assert.equal(expanded.selectedAction?.type, ActionType.RAISE);
  assert.equal(expanded.selectedAction?.targetTotal, 80n);
  assert.equal(expanded.solver?.source, SolverSource.BLUEPRINT);
  assert.equal(
    expanded.solver?.artifactSha256,
    '0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef',
  );
  assert.equal(expanded.solver?.guarantee, 'uncertified');
});

for (const [name, guarantee] of [
  ['expanded-strategy-bound.binpb', 'modeled_exact_bound'],
  ['expanded-strategy-baseline.binpb', 'baseline'],
] as const) {
  test(`TypeScript preserves the ${guarantee} guarantee vector`, () => {
    const binary = readFileSync(join(fixtureDirectory, name));
    const envelope = fromBinary(EnvelopeSchema, binary);
    const result = envelope.payload.case === 'decisionResponse'
      ? envelope.payload.value.result
      : undefined;
    assert.equal(result?.case, 'expandedStrategy');
    if (result?.case !== 'expandedStrategy') return;
    assert.equal(result.value.solver?.guarantee, guarantee);
    assert.equal(result.value.solver?.source, SolverSource.BLUEPRINT);
  });
}

test('TypeScript preserves the minor-1 capabilities vector with BLUEPRINT', () => {
  const binary = readFileSync(join(fixtureDirectory, 'capabilities-minor1.binpb'));
  const envelope = fromBinary(EnvelopeSchema, binary);
  assert.equal(envelope.protocolMinor, 1);
  assert.equal(envelope.payload.case, 'getCapabilitiesResponse');
  if (envelope.payload.case !== 'getCapabilitiesResponse') return;
  assert.deepEqual(
    [...envelope.payload.value.supportedProtocolMinors],
    [0, 1],
  );
  assert.ok(
    envelope.payload.value.solverModes.includes(SolverMode.BLUEPRINT),
    'BLUEPRINT (6) advertised on minor 1',
  );
});

test('TypeScript pins enum ordinals 6 and 7 on the wire', () => {
  const binary = readFileSync(join(fixtureDirectory, 'solver-enum-presence.binpb'));
  const envelope = fromBinary(EnvelopeSchema, binary);
  assert.equal(envelope.payload.case, 'getCapabilitiesResponse');
  if (envelope.payload.case !== 'getCapabilitiesResponse') return;
  assert.deepEqual(
    [...envelope.payload.value.solverModes],
    [SolverMode.AUTOMATIC, SolverMode.BLUEPRINT, 7],
  );
  assert.deepEqual(
    Buffer.from(toBinary(EnvelopeSchema, envelope)),
    binary,
  );
});

test('TypeScript preserves unknown fields and additive enum values', () => {
  const binary = readFileSync(join(fixtureDirectory, 'envelope.binpb'));
  const withUnknownField = Uint8Array.from([...binary, 0xa0, 0x06, 0x01]);
  assert.deepEqual(
    Buffer.from(toBinary(
      EnvelopeSchema,
      fromBinary(EnvelopeSchema, withUnknownField),
    )),
    Buffer.from(withUnknownField),
  );

  const cardBinary = readFileSync(
    join(fixtureDirectory, 'card-unknown-enum.binpb'),
  );
  const card = fromBinary(CardSchema, cardBinary);
  assert.equal(card.rank, 99);
  assert.equal(card.suit, Suit.SPADES);
  assert.deepEqual(Buffer.from(toBinary(CardSchema, card)), cardBinary);
});
