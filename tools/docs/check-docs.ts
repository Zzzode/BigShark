#!/usr/bin/env node

import { existsSync, readdirSync, readFileSync } from 'node:fs';
import { dirname, extname, join, relative, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const root = resolve(dirname(fileURLToPath(import.meta.url)), '../../..');
const excludedDirectories = new Set([
  '.cache',
  '.git',
  '.runtime',
  'build',
  'dist',
  'node_modules',
  'sessions',
]);
const textExtensions = new Set([
  '.c',
  '.cc',
  '.cpp',
  '.h',
  '.hpp',
  '.js',
  '.json',
  '.md',
  '.mjs',
  '.ts',
]);

function excluded(path: string): boolean {
  const rel = relative(root, path);
  if (!rel || rel.startsWith('..')) return false;
  const parts = rel.split('/');
  const top = parts[0];
  if (!top) return false;
  if (excludedDirectories.has(top)) return true;
  if (parts[0] === 'engine') {
    return parts[1] === 'third_party' || parts[1] === '.cache'
      || parts[1] === 'build' || Boolean(parts[1]?.startsWith('build-'));
  }
  return false;
}

function collect(path: string, out: string[]): void {
  for (const entry of readdirSync(path, { withFileTypes: true })) {
    const child = join(path, entry.name);
    if (entry.isSymbolicLink() || excluded(child)) continue;
    if (entry.isDirectory()) {
      collect(child, out);
    } else if (textExtensions.has(extname(entry.name))) {
      out.push(child);
    }
  }
}

function lineNumber(text: string, index: number): number {
  return text.slice(0, index).split('\n').length;
}

const files: string[] = [];
collect(root, files);
const errors: string[] = [];
const han = /\p{Script=Han}/gu;
const markdownLink = /\[[^\]]*\]\(([^)\s]+)(?:\s+"[^"]*")?\)/g;

for (const file of files) {
  const text = readFileSync(file, 'utf8');
  for (const match of text.matchAll(han)) {
    errors.push(`${relative(root, file)}:${lineNumber(text, match.index)} contains CJK text`);
  }

  if (extname(file) !== '.md') continue;
  for (const match of text.matchAll(markdownLink)) {
    const target = match[1];
    if (!target) continue;
    if (target.startsWith('#') || /^[a-z][a-z0-9+.-]*:/i.test(target)) continue;
    const path = decodeURIComponent(target.split('#', 1)[0] || '');
    if (!path) continue;
    const absolute = resolve(dirname(file), path);
    if (!existsSync(absolute)) {
      errors.push(`${relative(root, file)}:${lineNumber(text, match.index)} broken link: ${target}`);
    }
  }
}

if (errors.length) {
  for (const error of errors) process.stderr.write(`${error}\n`);
  process.exit(1);
}

process.stdout.write(`Documentation checks passed (${files.length} text files).\n`);
