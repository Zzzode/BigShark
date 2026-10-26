// coverage.ts — flop-library hit and miss-reason distribution over results.log.
//
// T-08 instrumentation read side. The runner already appends one JSON line per
// executed action to .runtime/results.log with the source, street, and the
// decision's guarantee level, reason, and resident artifact digest. This
// module tallies that seam into a coverage report; it never joins a room and
// reads only local files. The write side is apps/river-club-agent/main.ts.
import { readFileSync } from 'node:fs';
import { join } from 'node:path';
import type { ExecutableDecision } from './types.js';

// One results.log line. The schema is built by resultsLogEntry in
// apps/river-club-agent/runner-state.ts; extra fields are tolerated.
export interface CoverageEntry {
  ts?: number;
  kind?: string;
  hand?: string;
  street?: string;
  source?: string;
  ok?: boolean;
  decision?: ExecutableDecision;
}

export type Bucket = 'resident' | 'resolved' | 'fallback';

export interface CoverageReport {
  actionable: number;
  failed: number;
  byLevel: Record<string, number>;
  byBucket: Record<Bucket, number>;
  /** Fallback reason-prefix distribution (the operational miss taxonomy). */
  byReason: Record<string, number>;
  /** Actionable decisions per street (preflop/flop/turn/river/unknown). */
  byStreet: Record<string, number>;
  /** Distinct resident artifacts that served at least one decision. */
  artifacts: string[];
}

// The guarantee-level token emitted on a resident blueprint hit.
const RESIDENT_LEVELS = new Set(['certified_bound']);
// On-demand resolver receipts are approximate but carry no resident digest.
const RESOLVED_LEVELS = new Set(['approximate', 'abstract_solved']);

function bucketFor(decision: ExecutableDecision): Bucket {
  if (decision.artifactSha256)
    return 'resident';
  const level = decision.guaranteeLevel;
  if (level !== undefined && (RESIDENT_LEVELS.has(level) || RESOLVED_LEVELS.has(level)))
    return 'resolved';
  return 'fallback';
}

// The reason prefix is the stable taxonomy token before any ':'. For example
// "turn_root_unserved" from "turn_root_unserved from the resolver path" and
// "operational-fallback" from "operational-fallback: check".
export function reasonPrefix(reason: string): string {
  const head = reason.trim().split(/[\s:]/)[0] ?? '';
  return head.length > 0 ? head : 'unknown';
}

function emptyReport(): CoverageReport {
  return {
    actionable: 0,
    failed: 0,
    byLevel: {},
    byBucket: { resident: 0, resolved: 0, fallback: 0 },
    byReason: {},
    byStreet: {},
    artifacts: [],
  };
}

// Tally parsed results.log entries. `since` is an epoch-millis floor.
export function summarizeCoverage(entries: CoverageEntry[], since = 0): CoverageReport {
  const report = emptyReport();
  const artifacts = new Set<string>();
  for (const entry of entries) {
    if ((entry.ts ?? 0) < since)
      continue;
    if (entry.ok === false) {
      report.failed += 1;
      continue;
    }
    const decision = entry.decision;
    if (!decision?.action)
      continue;
    report.actionable += 1;
    const level = decision.guaranteeLevel ?? 'none';
    report.byLevel[level] = (report.byLevel[level] ?? 0) + 1;
    const bucket = bucketFor(decision);
    report.byBucket[bucket] += 1;
    const street = entry.street ?? 'unknown';
    report.byStreet[street] = (report.byStreet[street] ?? 0) + 1;
    if (bucket === 'fallback') {
      const prefix = reasonPrefix(decision.reason);
      report.byReason[prefix] = (report.byReason[prefix] ?? 0) + 1;
    }
    if (decision.artifactSha256)
      artifacts.add(decision.artifactSha256);
  }
  report.artifacts = [...artifacts].sort();
  return report;
}

// Parse a results.log file's text into entries; malformed lines are skipped.
export function parseResultsLog(text: string): CoverageEntry[] {
  const entries: CoverageEntry[] = [];
  for (const line of text.split('\n')) {
    if (!line.trim())
      continue;
    try {
      entries.push(JSON.parse(line) as CoverageEntry);
    } catch {
      // A corrupt journal line must never break the coverage report.
    }
  }
  return entries;
}

function pct(part: number, total: number): string {
  return total === 0 ? '0.0%' : `${((100 * part) / total).toFixed(1)}%`;
}

// One human-readable line block for the periodic monitor.
export function formatCoverage(report: CoverageReport): string {
  const a = report.actionable;
  const lines = [
    `actionable=${a} failed=${report.failed}`,
    `resident=${report.byBucket.resident} (${pct(report.byBucket.resident, a)}) ` +
      `resolved=${report.byBucket.resolved} (${pct(report.byBucket.resolved, a)}) ` +
      `fallback=${report.byBucket.fallback} (${pct(report.byBucket.fallback, a)})`,
    `levels=${JSON.stringify(report.byLevel)}`,
    `streets=${JSON.stringify(report.byStreet)}`,
    `artifacts=${report.artifacts.length}`,
  ];
  const reasons = Object.entries(report.byReason).sort((x, y) => y[1] - x[1]);
  if (reasons.length > 0)
    lines.push(`fallback-reasons=${JSON.stringify(Object.fromEntries(reasons))}`);
  return lines.join('\n');
}

// CLI entry: read .runtime/results.log (or a given path) and print the report.
export function runCoverageCli(argv: string[], repoRoot: string): number {
  const path = argv[0] ?? join(repoRoot, '.runtime', 'results.log');
  let text: string;
  try {
    text = readFileSync(path, 'utf8');
  } catch {
    console.log(`coverage: no results log at ${path}`);
    return 0;
  }
  const report = summarizeCoverage(parseResultsLog(text));
  console.log(formatCoverage(report));
  return 0;
}
