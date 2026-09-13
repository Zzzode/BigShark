#!/usr/bin/env node

import { createInterface } from 'node:readline';

const input = createInterface({
  input: process.stdin,
  crlfDelay: Infinity,
});

let requestIndex = 0;
for await (const line of input) {
  if (!line) continue;
  requestIndex++;
  const request = JSON.parse(line);
  const mode = request.mode || 'echo';

  if (mode === 'exit') process.exit(7);
  if (mode === 'hang') continue;
  if (mode === 'malformed') {
    process.stdout.write('{invalid json}\n');
    continue;
  }

  const response = `${JSON.stringify({
    requestIndex,
    value: request.value ?? null,
  })}\n`;

  if (mode === 'split') {
    const middle = Math.floor(response.length / 2);
    process.stdout.write(response.slice(0, middle));
    setTimeout(() => process.stdout.write(response.slice(middle)), 10);
    continue;
  }
  if (mode === 'late') {
    await new Promise(resolve => setTimeout(resolve, 80));
    process.stdout.write(response);
    continue;
  }
  process.stdout.write(response);
}
