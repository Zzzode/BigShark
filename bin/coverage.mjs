#!/usr/bin/env node
// coverage.mjs — flop-library hit/miss distribution from .runtime/results.log.
//
// Thin launch wrapper; the implementation is compiled TypeScript in
// platforms/river-club/src/coverage.ts. Usage:
//   node bin/coverage.mjs [path-to-results.log]
import { existsSync } from 'node:fs';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const here = dirname(fileURLToPath(import.meta.url));
const root = resolve(here, '..');
const target = new URL('../dist/platforms/river-club/src/coverage.js', import.meta.url);
if (!existsSync(target)) {
  console.error('BigShark TypeScript output is missing. Run npm run build:ts.');
  process.exit(1);
}
const mod = await import(target.pathname);
const repoRoot = join(root);
process.exit(mod.runCoverageCli(process.argv.slice(2), repoRoot));
