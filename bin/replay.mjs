#!/usr/bin/env node

import { existsSync } from 'node:fs';
const target = new URL('../dist/tools/replay/main.js', import.meta.url);
if (!existsSync(target)) { console.error('BigShark TypeScript output is missing. Run npm run build:ts.'); process.exit(1); }
await import(target.href);
