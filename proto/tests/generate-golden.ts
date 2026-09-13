import {
  create,
  fromJson,
  toBinary,
  type JsonValue,
} from '@bufbuild/protobuf';
import { readFileSync, writeFileSync } from 'node:fs';
import { join } from 'node:path';

import {
  CardSchema,
  EnvelopeSchema,
  Rank,
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
  const json = JSON.parse(
    readFileSync(join(fixtureDirectory, `${name}.json`), 'utf8'),
  ) as JsonValue;
  const envelope = fromJson(EnvelopeSchema, json);
  writeFileSync(
    join(fixtureDirectory, `${name}.binpb`),
    toBinary(EnvelopeSchema, envelope),
  );
}

const unknownEnum = create(CardSchema, {
  rank: 99 as Rank,
  suit: Suit.SPADES,
});
writeFileSync(
  join(fixtureDirectory, 'card-unknown-enum.binpb'),
  toBinary(CardSchema, unknownEnum),
);
