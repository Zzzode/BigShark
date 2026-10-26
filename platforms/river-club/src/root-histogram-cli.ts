// CLI for the RFC 0035 stage-4 exact-ledger demand histogram. Reads
// sessions/*.jsonl (or explicit paths), aggregates flop street-start root
// ledgers, and prints the demand report. With --json it emits the full
// histogram for downstream threshold tooling.
import { readFileSync, readdirSync, statSync } from 'node:fs';
import { join } from 'node:path';
import {
  summarizeRootDemand,
  histogramReport,
  emptyHistogram,
  type JournalEntry,
} from './root-histogram.js';

function readJsonl(path: string): JournalEntry[] {
  const text = readFileSync(path, 'utf8');
  const entries: JournalEntry[] = [];
  for (const line of text.split('\n')) {
    const trimmed = line.trim();
    if (!trimmed) continue;
    try {
      const value = JSON.parse(trimmed) as unknown;
      if (typeof value === 'object' && value !== null)
        entries.push(value as JournalEntry);
    } catch {
      // A corrupt journal line is skipped, not fatal: journals are append-only
      // operational evidence and a torn tail must never block the histogram.
    }
  }
  return entries;
}

function collectPaths(args: string[]): string[] {
  const explicit = args.filter(a => !a.startsWith('--'));
  if (explicit.length > 0) return explicit;
  return [];
}

export function runRootHistogramCli(argv: string[], repoRoot: string): number {
  const asJson = argv.includes('--json');
  const paths: string[] = [];
  const explicit = collectPaths(argv);
  if (explicit.length > 0) {
    for (const p of explicit) {
      const st = statSync(p, { throwIfNoEntry: false });
      if (st?.isFile()) paths.push(p);
      else if (st?.isDirectory())
        for (const name of readdirSync(p).sort())
          if (name.endsWith('.jsonl')) paths.push(join(p, name));
    }
  } else {
    const dir = join(repoRoot, 'sessions');
    const st = statSync(dir, { throwIfNoEntry: false });
    if (st?.isDirectory())
      for (const name of readdirSync(dir).sort())
        if (name.endsWith('.jsonl')) paths.push(join(dir, name));
  }

  const entries = paths.flatMap(p => {
    try {
      return readJsonl(p);
    } catch {
      return [];
    }
  });

  const histogram = entries.length === 0 ? emptyHistogram()
    : summarizeRootDemand(entries);

  if (asJson) {
    console.log(JSON.stringify({ report: histogramReport(histogram), histogram }, null, 2));
    return 0;
  }

  const report = histogramReport(histogram);
  console.log('RFC 0035 stage 4: flop street-start exact-ledger demand');
  console.log(`sessions paths:        ${paths.length}`);
  console.log(`journal entries:       ${histogram.journalEntries}`);
  console.log(`actionable room shots: ${histogram.roomSnapshots}`);
  console.log(`distinct hands:        ${histogram.distinctHands}`);
  console.log(`flop street-start:     ${histogram.flopStreetStart}`);
  console.log(`ledger mappable:       ${histogram.ledgerMappable}`);
  console.log(`ledger incomplete:     ${histogram.ledgerIncomplete}`);
  if (histogram.ledgerIncomplete > 0)
    console.log(`  reasons:             ${JSON.stringify(histogram.ledgerIncompleteReasons)}`);
  console.log(`preflop families:      ${JSON.stringify(histogram.familyCounts)}`);
  console.log(`(live,blind) groups:   ${Object.keys(histogram.groupCounts).length} ${JSON.stringify(histogram.groupCounts)}`);
  console.log(`distinct exact tuples: ${report.distinctExactTuples}`);
  console.log(`median start stack BB: ${report.medianStartingStackBb ?? 'n/a'}`);
  console.log(`median behind BB:      ${report.medianStackBehindBb ?? 'n/a'}`);
  console.log(`median open size BB:   ${report.medianOpenSizeBb ?? 'n/a'}`);
  if (histogram.ledgerMappable === 0) {
    console.log('\nNO real flop street-start observations: the stage-4/5 gate is unmet; '
      + 'no second BSFLIB03 root may be trained from this corpus.');
  } else {
    console.log('\ntop exact ledgers:');
    for (const t of report.top)
      console.log(`  ${(t.share * 100).toFixed(1).padStart(5)}%  n=${String(t.count).padStart(3)}  `
        + `${t.family.padEnd(6)} live=${t.liveCount} pot=${t.pot} (BB ${t.bigBlind})`);
  }
  return 0;
}
