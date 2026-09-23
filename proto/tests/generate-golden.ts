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
  DecisionRequestSchema,
  DecisionOptionsSchema,
  EnvelopeSchema,
  GetCapabilitiesResponseSchema,
  GuaranteeLevel,
  HandStateSchema,
  Rank,
  SolverMode,
  Suit,
} from '../../build/generated/ts/bigshark/engine/v1/engine_pb.js';

const fixtureDirectory = join(process.cwd(), 'proto', 'tests', 'fixtures');
const envelopeFixtures = [
  'capabilities-request',
  'capabilities-response',
  'capabilities-minor1',
  'capabilities-minor2',
  'decision-request',
  'decision-request-floor',
  'envelope',
  'error-response',
  'expanded-strategy',
  'expanded-strategy-bound',
  'expanded-strategy-baseline',
  'guaranteed-strategy',
  'guarantee-below-request',
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

// RFC 0002 Stage 8 enum-presence wire vector: a minor-1 capabilities response
// advertises BLUEPRINT (6) and also carries the not-yet-selectable RESOLVING
// ordinal (7) so both new enum numbers are pinned byte-for-byte on the wire.
// Protobuf-ES preserves unknown numeric enum ordinals through encode/decode.
const enumPresence = create(GetCapabilitiesResponseSchema, {
  supportedProtocolMinors: [0, 1],
  engineBuildVersion: 'enum-presence-vector',
  solverModes: [
    SolverMode.AUTOMATIC,
    6 as SolverMode,
    7 as SolverMode,
  ],
});
const enumPresenceEnvelope = create(EnvelopeSchema, {
  protocolMinor: 1,
  requestId: 'solver-enum-presence',
});
enumPresenceEnvelope.payload = {
  case: 'getCapabilitiesResponse',
  value: enumPresence,
};
writeFileSync(
  join(fixtureDirectory, 'solver-enum-presence.binpb'),
  toBinary(EnvelopeSchema, enumPresenceEnvelope),
);

// RFC 0008 stage 5 enum-presence wire vectors: one minor-2 decision request
// per GuaranteeLevel ordinal 0..5 (field 8 is a scalar optional, so one
// envelope can carry only one value). The numbers are pinned byte-for-byte;
// these are encoding vectors, never host-validated requests (ordinal 0 as a
// present floor is INVALID_REQUEST at runtime).
for (const ordinal of [0, 1, 2, 3, 4, 5]) {
  const levelRequest = create(DecisionRequestSchema, {
    state: create(HandStateSchema, {
      handId: `guarantee-enum-presence-${ordinal}`,
      street: 0 as 0,  // wire value never validated here, this is an encoding pin
    }),
    options: create(DecisionOptionsSchema, {
      strategyProfile: 'tag',
      solverMode: SolverMode.AUTOMATIC,
      minimumGuarantee: ordinal as GuaranteeLevel,
    }),
  });
  const levelEnvelope = create(EnvelopeSchema, {
    protocolMinor: 2,
    requestId: `guarantee-enum-presence-${ordinal}`,
  });
  levelEnvelope.payload = { case: 'decisionRequest', value: levelRequest };
  writeFileSync(
    join(fixtureDirectory, `guarantee-enum-presence-${ordinal}.binpb`),
    toBinary(EnvelopeSchema, levelEnvelope),
  );
}
