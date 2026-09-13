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
  Suit,
} from '../../build/generated/ts/bigshark/engine/v1/engine_pb.js';

const fixtureDirectory = join(process.cwd(), 'proto', 'tests', 'fixtures');
const envelopeFixtures = [
  'capabilities-request',
  'capabilities-response',
  'decision-request',
  'envelope',
  'error-response',
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
