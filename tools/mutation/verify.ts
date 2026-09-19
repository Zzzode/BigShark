#!/usr/bin/env node
// Mutation verification for a C++ test target (RFC 0008 stage gate evidence).
//
// WHY THIS EXISTS. This repository has twice shipped a test that could not fail:
// a multiway walk that never dealt a public card, and a sampler suite with no
// zero-weight coverage. Both were found by breaking the implementation and
// noticing the test stayed green, never by reading the test. Reading a test tells
// you what it asserts; only a mutation tells you whether a real defect can
// violate that assertion.
//
// For each mutation: apply the edit, rebuild ONLY the affected test target,
// classify the result, then restore the file from a pristine copy.
//
// THREE OUTCOMES, and the third is the one that has to be MEASURED rather than
// declared:
//   RED   -- the suite failed. The mutation is caught.
//   EQUIV -- the suite passed and the mutation is listed under
//            `expected.equivalent`. It changes no reachable behavior; the suite
//            should pin that with counters so the claim expires loudly.
//   GAP   -- the suite passed and the mutation is not recorded as equivalent.
//            A coverage gap. Reporting an equivalent mutant as a gap is a false
//            alarm, and the reverse error is worse: a genuine gap waved through
//            as "equivalent" leaves the defect class untested forever. Resolve
//            every GAP by closing it or by proving equivalence through
//            reachability, never by argument alone.
//   BUILD -- the mutation did not compile. That is not a test result.
//
// Usage:
//   node dist/tools/mutation/verify.js --config tools/mutation/game-definition.json

import { copyFileSync, mkdtempSync, readFileSync, rmSync, writeFileSync } from 'node:fs';
import { spawnSync } from 'node:child_process';
import { tmpdir } from 'node:os';
import { dirname, join, resolve } from 'node:path';
import process from 'node:process';
import { fileURLToPath } from 'node:url';

interface Mutation {
  name: string;
  file: string;
  find: string;
  replace: string;
}

interface Config {
  description: string;
  expected?: { equivalent?: string[]; rationale?: string };
  build: { command: string; args: string[] };
  test: { command: string; args: string[] };
  mutations: Mutation[];
}

type Verdict = 'RED' | 'EQUIV' | 'GAP' | 'BUILD';

const argv = process.argv.slice(2);
const configIndex = argv.indexOf('--config');
const configArg = configIndex === -1 ? undefined : argv[configIndex + 1];
if (configArg === undefined) {
  process.stderr.write('usage: node dist/tools/mutation/verify.js --config <file.json>\n');
  process.exit(2);
}

const root = resolve(dirname(fileURLToPath(import.meta.url)), '..', '..', '..');
const configPath = resolve(root, configArg);
const config = JSON.parse(readFileSync(configPath, 'utf8')) as Config;

const run = (command: string, args: string[]) =>
  spawnSync(command, args, { cwd: root, encoding: 'utf8', stdio: 'pipe' });

// Pristine copies, taken before anything is touched, so a failure partway
// through cannot leave the tree mutated.
const files = [...new Set(config.mutations.map((m) => m.file))];
const backupDir = mkdtempSync(join(tmpdir(), 'bs-mutate-'));
const backupPath = (file: string) => join(backupDir, file.replaceAll('/', '_'));
const restore = (): void => {
  for (const file of files) copyFileSync(backupPath(file), join(root, file));
  rmSync(backupDir, { recursive: true, force: true });
};
for (const file of files) copyFileSync(join(root, file), backupPath(file));
process.on('SIGINT', () => {
  restore();
  process.exit(130);
});

interface Result {
  name: string;
  verdict: Verdict;
  detail: string;
}

const results: Result[] = [];
try {
  for (const mutation of config.mutations) {
    const path = join(root, mutation.file);
    const original = readFileSync(path, 'utf8');
    // An anchor that is missing or ambiguous would silently mutate the wrong
    // thing, which is worse than not running: it would report a mutation as
    // caught when the edit never happened.
    const occurrences = original.split(mutation.find).length - 1;
    if (occurrences !== 1) {
      results.push({
        name: mutation.name,
        verdict: 'BUILD',
        detail: occurrences === 0 ? 'anchor text not found' : 'anchor text is ambiguous',
      });
      continue;
    }
    writeFileSync(path, original.replace(mutation.find, mutation.replace));

    const build = run(config.build.command, config.build.args);
    if (build.status !== 0) {
      writeFileSync(path, original);
      results.push({ name: mutation.name, verdict: 'BUILD', detail: 'mutation did not compile' });
      continue;
    }

    const test = run(config.test.command, config.test.args);
    // The first line naming the failure is the most informative thing to keep:
    // it says WHICH invariant noticed, which is what a reviewer needs in order
    // to judge whether the suite caught the right thing rather than merely
    // failing somewhere.
    const notice = `${test.stdout}${test.stderr}`
      .split('\n')
      .find((line) => /MISMATCH|CHECK failed|error/i.test(line))
      ?.trim()
      .slice(0, 110);
    const passed = test.status === 0;
    const equivalent = (config.expected?.equivalent ?? []).includes(mutation.name);
    results.push({
      name: mutation.name,
      verdict: passed ? (equivalent ? 'EQUIV' : 'GAP') : 'RED',
      detail: notice ?? '',
    });
    writeFileSync(path, original);
  }
} finally {
  restore();
}

const width = Math.max(...results.map((r) => r.name.length));
for (const r of results) process.stdout.write(`${r.verdict.padEnd(5)} ${r.name.padEnd(width)}  ${r.detail}\n`);

const count = (verdict: Verdict) => results.filter((r) => r.verdict === verdict).length;
const gaps = count('GAP');
const unbuilt = count('BUILD');
process.stdout.write(`\n${count('RED')}/${results.length} caught`);
if (count('EQUIV')) process.stdout.write(`, ${count('EQUIV')} equivalent (recorded, pinned by the suite)`);
if (gaps) process.stdout.write(`, ${gaps} UNCAUGHT GAP${gaps > 1 ? 'S' : ''}`);
if (unbuilt) process.stdout.write(`, ${unbuilt} not compiled`);
process.stdout.write('\n');
if (gaps)
  process.stdout.write(
    '\nEach gap above is a missing fixture or a mutant that needs proving equivalent.\n' +
      'Prove it by measuring reachability over the tree, then either close the gap or\n' +
      'record the mutant under expected.equivalent together with the pins that hold\n' +
      'the proof — a claim with no pin expires silently when a fixture changes.\n',
  );
process.exitCode = gaps || unbuilt ? 1 : 0;
