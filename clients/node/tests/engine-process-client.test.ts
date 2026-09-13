import assert from 'node:assert/strict';
import { dirname, join } from 'node:path';
import test from 'node:test';
import { fileURLToPath } from 'node:url';

import { EngineProcessClient } from '../engine-process-client.js';

const here = dirname(fileURLToPath(import.meta.url));
const fakeEngine = join(here, 'fixtures', 'fake-engine.js');

function client() {
  return new EngineProcessClient<
    { mode?: string; value?: string },
    { requestIndex: number; value: string | null }
  >({
    command: process.execPath,
    args: [fakeEngine],
    warmupRequest: { value: 'warmup' },
    warmupTimeoutMs: 500,
  });
}

test('handles split output and consecutive requests', async t => {
  const engine = client();
  t.after(() => engine.stop());

  const split = await engine.request({ mode: 'split', value: 'first' });
  const second = await engine.request({ value: 'second' });

  assert.equal(split.value, 'first');
  assert.equal(second.value, 'second');
  assert.ok(second.requestIndex > split.requestIndex);
});

test('restarts after a request timeout', async t => {
  const engine = client();
  t.after(() => engine.stop());

  await assert.rejects(
    engine.request({ mode: 'hang', value: 'never' }, 10),
    /timed out/,
  );
  const next = engine.request({ value: 'next' }, 500);
  assert.equal((await next).value, 'next');
});

test('late responses from a timed-out process cannot shift the new queue', async t => {
  const engine = client();
  t.after(() => engine.stop());

  await assert.rejects(
    engine.request({ mode: 'late', value: 'late' }, 10),
    /timed out/,
  );
  assert.equal((await engine.request({ value: 'next' }, 500)).value, 'next');
});

test('rejects malformed JSON and continues with the next response', async t => {
  const engine = client();
  t.after(() => engine.stop());

  await assert.rejects(
    engine.request({ mode: 'malformed' }),
    /invalid JSON/,
  );
  assert.equal((await engine.request({ value: 'recovered' })).value, 'recovered');
});

test('rejects pending requests when the process exits', async t => {
  const engine = client();
  t.after(() => engine.stop());

  await assert.rejects(
    engine.request({ mode: 'exit' }),
    /exited/,
  );
});
