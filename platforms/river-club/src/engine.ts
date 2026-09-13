import { existsSync } from 'node:fs';
import { fileURLToPath } from 'node:url';

import { EngineProcessClient } from '../../../clients/node/engine-process-client.js';
import {
  buildContext,
  safeFallback,
  validateEngineDecision,
} from './v0-normalizer.js';
import type {
  EngineConfig,
  ExecutableDecision,
  RawEngineDecision,
  RiverRoom,
  V0DecisionContext,
} from './types.js';

const defaultBinary = process.env.BIGSHARK_ENGINE_BINARY || fileURLToPath(
  new URL('../../../../bin/bigshark-engine', import.meta.url),
);
let defaultClient: EngineProcessClient<V0DecisionContext, RawEngineDecision> | null = null;

function engineClient(): EngineProcessClient<V0DecisionContext, RawEngineDecision> | null {
  if (!defaultClient && existsSync(defaultBinary)) {
    defaultClient = new EngineProcessClient({
      command: defaultBinary,
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
    });
  }
  return defaultClient;
}

export { buildContext, safeFallback };

export async function decide(
  room: RiverRoom,
  config: EngineConfig = {},
): Promise<ExecutableDecision> {
  if (!room?.legal) return { action: 'fold', reason: 'no-legal' };
  const client = config.engineClient || engineClient();
  if (!client) return safeFallback(room);

  try {
    const context = buildContext(room, config);
    const rawDecision = await client.request(context, config.timeoutMs ?? 2000);
    return validateEngineDecision(room, rawDecision) || safeFallback(room);
  } catch {
    return safeFallback(room);
  }
}

export function closeEngine(): void {
  defaultClient?.stop();
  defaultClient = null;
}
