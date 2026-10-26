#!/usr/bin/env node
// RFC 0035 stage 4: exact-ledger demand histogram over flop street-start
// decisions in the local sessions/ journals. Thin launch wrapper; the
// implementation is compiled TypeScript in dist/platforms/river-club/src.
//
// Usage: node bin/root-histogram.mjs [sessions-file-or-dir ...] [--json]
import { existsSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const here = dirname(fileURLToPath(import.meta.url));
const root = resolve(here, '..');
const target = new URL('../dist/platforms/river-club/src/root-histogram-cli.js',
  import.meta.url);
if (!existsSync(target)) {
  console.error('Compiled TypeScript output is missing. Run npm run build:ts.');
  process.exit(1);
}
const mod = await import(target);
const code = mod.runRootHistogramCli(process.argv.slice(2), root);
process.exit(code);

function resolve(from, to) {
  // Avoid importing path just for one join while keeping CWD-independent.
  return join(from, to);
}
