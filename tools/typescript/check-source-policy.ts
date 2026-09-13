#!/usr/bin/env node

import { readdirSync } from 'node:fs';
import { dirname, extname, join, relative, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const root = resolve(dirname(fileURLToPath(import.meta.url)), '../../..');
const sourceRoots = ['apps', 'clients', 'platforms', 'tools'];
const errors: string[] = [];

function scan(path: string): void {
  for (const entry of readdirSync(path, { withFileTypes: true })) {
    const child = join(path, entry.name);
    if (entry.isSymbolicLink()) continue;
    if (entry.isDirectory()) {
      scan(child);
      continue;
    }
    if (extname(entry.name) === '.js' || extname(entry.name) === '.mjs') {
      errors.push(`${relative(root, child)}: handwritten JavaScript is forbidden`);
    }
  }
}

for (const directory of sourceRoots) scan(join(root, directory));

if (errors.length) {
  for (const error of errors) process.stderr.write(`${error}\n`);
  process.exit(1);
}

process.stdout.write('TypeScript source policy passed.\n');
