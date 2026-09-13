import assert from 'node:assert/strict';
import {
  copyFileSync,
  mkdirSync,
  mkdtempSync,
  readFileSync,
  rmSync,
} from 'node:fs';
import { tmpdir } from 'node:os';
import { dirname, join, resolve } from 'node:path';
import { spawnSync } from 'node:child_process';
import test from 'node:test';
import { fileURLToPath } from 'node:url';

const root = resolve(dirname(fileURLToPath(import.meta.url)), '../../../..');
const bin = join(root, 'bin');
const canonical = join(root, 'dist', 'platforms', 'river-club', 'src');

const launchers = new Map([
  ['bigshark.mjs', '../dist/platforms/river-club/src/journal-cli.js'],
  ['check-docs.mjs', '../dist/tools/docs/check-docs.js'],
  ['check-rfcs.mjs', '../dist/tools/rfc/check-rfcs.js'],
  ['cpp-engine.mjs', '../dist/platforms/river-club/src/engine.js'],
  ['play.mjs', '../dist/apps/river-club-agent/main.js'],
  ['replay.mjs', '../dist/tools/replay/main.js'],
  ['report.mjs', '../dist/platforms/river-club/src/report.js'],
  ['river-club', '../dist/platforms/river-club/src/river-cli.js'],
  ['shoot.mjs', '../dist/platforms/river-club/src/atomic-action.js'],
  ['wait-seat.mjs', '../dist/platforms/river-club/src/wait-seat.js'],
  ['wait-turn.mjs', '../dist/platforms/river-club/src/wait-turn.js'],
]);

test('bin contains only thin JavaScript compatibility launchers', () => {
  for (const [name, target] of launchers) {
    const source = readFileSync(join(bin, name), 'utf8');
    assert.ok(source.split('\n').length <= 8, `${name} is not thin`);
    assert.match(source, new RegExp(target.replaceAll('.', '\\.')));
  }
});

test('River CLI launcher preserves help output and exit status', () => {
  const launcher = spawnSync(process.execPath, [join(bin, 'river-club'), 'help'], {
    encoding: 'utf8',
  });
  const implementation = spawnSync(
    process.execPath,
    [join(canonical, 'river-cli.js'), 'help'],
    { encoding: 'utf8' },
  );

  assert.equal(launcher.status, implementation.status);
  assert.equal(launcher.stdout, implementation.stdout);
  assert.equal(launcher.stderr, implementation.stderr);
});

test('compatibility launchers report a missing TypeScript build', t => {
  const directory = mkdtempSync(join(tmpdir(), 'bigshark-launcher-'));
  t.after(() => rmSync(directory, { force: true, recursive: true }));
  const isolatedBin = join(directory, 'bin');
  mkdirSync(isolatedBin);
  const launcherPath = join(isolatedBin, 'bigshark.mjs');
  copyFileSync(join(bin, 'bigshark.mjs'), launcherPath);

  const result = spawnSync(process.execPath, [launcherPath, 'help'], {
    encoding: 'utf8',
  });

  assert.equal(result.status, 1);
  assert.match(result.stderr, /TypeScript output is missing/);
});
