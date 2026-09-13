#!/usr/bin/env node
// bigshark — thin journaling wrapper around the official river-club CLI.
// Every command and its JSON response are teed to sessions/<date>.jsonl
// as a decision/review journal. Pass-through semantics are otherwise identical:
// same argv, same env (RIVER_CLUB_TOKEN etc.), same exit code, same stdout.

import { spawn } from 'node:child_process';
import { appendFileSync, mkdirSync } from 'node:fs';
import { dirname, join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const here = dirname(fileURLToPath(import.meta.url));
const root = resolve(here, '../../../..');
const logDir = join(root, 'sessions');

const args = process.argv.slice(2);
const stamp = () => new Date().toISOString();
const today = () => stamp().slice(0, 10);

function writeJournal(resp: unknown): void {
  // Don't persist credential configuration output (it echoes server/player, never the token).
  const entry = { ts: stamp(), argv: args, resp };
  try {
    mkdirSync(logDir, { recursive: true });
    appendFileSync(join(logDir, `${today()}.jsonl`), `${JSON.stringify(entry)}\n`, { mode: 0o600 });
  } catch { /* journaling must never break play */ }
}

function journal(line: string): void {
  const trimmed = line.trim();
  if (!trimmed) return;
  let response: unknown;
  try { response = JSON.parse(trimmed) as unknown; } catch { response = { raw: trimmed }; }
  writeJournal(response);
}

const child = spawn(process.execPath, [join(here, 'river-cli.js'), ...args], {
  env: process.env,
  stdio: ['inherit', 'pipe', 'inherit'],
});

let buf = '';
let frag = '';   // accumulates pretty-printed multi-line JSON fragments
function feed(line: string): void {
  const trimmed = line.trim();
  if (!trimmed) { if (frag) frag += '\n'; return; }
  // Compact JSON (e.g. `watch` mode): journal directly.
  if (trimmed.startsWith('{') && trimmed.endsWith('}')) {
    try {
      writeJournal(JSON.parse(trimmed) as unknown);
      frag = '';
      return;
    } catch { /* fall through */ }
  }
  // Pretty JSON: accumulate until the buffer parses as a complete value.
  frag = frag ? `${frag}\n${trimmed}` : trimmed;
  try {
    writeJournal(JSON.parse(frag) as unknown);
    frag = '';
  } catch { /* wait for more lines */ }
}
child.stdout.on('data', (chunk: Buffer) => {
  process.stdout.write(chunk);
  buf += chunk.toString('utf8');
  let nl;
  while ((nl = buf.indexOf('\n')) !== -1) {
    feed(buf.slice(0, nl));
    buf = buf.slice(nl + 1);
  }
});
child.on('close', code => {
  if (buf.trim()) feed(buf);
  if (frag.trim()) journal(frag);
  process.exit(code ?? 1);
});
