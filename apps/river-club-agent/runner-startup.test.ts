import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import test from 'node:test';

// The runner entry point performs real CLI operations on import. Keep this
// structural ordering guard separate from runtime adapter preflight tests;
// it is not live provider or complete-hand acceptance evidence.
const source = readFileSync(new URL('./main.js', import.meta.url), 'utf8');
const offTablePreflight = /if \(!s0\.room\) \{\s*await engineWarmup;\s*await findAndJoin\(\);\s*\}/u;

test('startup source guard places off-table engine preflight before join', () => {
  assert.ok(offTablePreflight.test(source), 'off-table preflight must finish before join');
});

test('startup source guard has no unconditional seated preflight wait', () => {
  const loop = source.lastIndexOf('while (Date.now() < startedAt');
  assert.ok(loop >= 0, 'the guarded entry point retains its operating loop');
  const startup = source.slice(0, loop);
  assert.ok(offTablePreflight.test(startup), 'preflight belongs in the off-table branch');
  assert.ok(!/await engineWarmup/u.test(startup.replace(offTablePreflight, '')),
    'an already-seated runner must not unconditionally wait for preflight');
});
