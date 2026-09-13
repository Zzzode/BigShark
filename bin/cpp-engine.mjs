import { existsSync } from 'node:fs';
const target = new URL('../dist/platforms/river-club/src/engine.js', import.meta.url);
if (!existsSync(target)) throw new Error('BigShark TypeScript output is missing. Run npm run build:ts.');
const { buildContext, closeEngine, decide, safeFallback } = await import(target.href);
export { buildContext, closeEngine, decide, safeFallback };
