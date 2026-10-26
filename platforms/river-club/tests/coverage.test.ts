import assert from 'node:assert/strict';
import { describe, it } from 'node:test';

import {
  formatCoverage,
  parseResultsLog,
  reasonPrefix,
  summarizeCoverage,
  type CoverageEntry,
} from '../src/coverage.js';
import type { ExecutableDecision } from '../src/types.js';

function decision(partial: Partial<ExecutableDecision>): ExecutableDecision {
  return { action: 'check', reason: 'unknown', ...partial };
}

function entry(partial: Partial<CoverageEntry>): CoverageEntry {
  return { decision: decision({}), ...partial };
}

const SHA = 'a'.repeat(64);
const SHA_B = 'b'.repeat(64);

describe('coverage summarizer', () => {
  it('buckets resident, resolved, and fallback decisions', () => {
    const entries = [
      entry({
        street: 'flop',
        decision: decision({ action: 'bet', amount: 20, reason: 'blueprint:12',
          guaranteeLevel: 'certified_bound', artifactSha256: SHA }),
      }),
      entry({
        street: 'turn',
        decision: decision({ reason: 'subgame-resolve:turn-allin-equity',
          guaranteeLevel: 'approximate' }),
      }),
      entry({
        street: 'turn',
        decision: decision({ reason: 'turn_root_unserved from the resolver path',
          guaranteeLevel: 'operational_fallback' }),
      }),
      entry({
        street: 'flop',
        decision: decision({ action: 'fold', reason: 'operational-fallback: fold',
          guaranteeLevel: 'operational_fallback' }),
      }),
    ];
    const report = summarizeCoverage(entries);
    assert.equal(report.actionable, 4);
    assert.equal(report.failed, 0);
    assert.deepEqual(report.byBucket, { resident: 1, resolved: 1, fallback: 2 });
    assert.equal(report.byReason.turn_root_unserved, 1);
    assert.equal(report.byReason['operational-fallback'], 1);
    assert.equal(report.byStreet.flop, 2);
    assert.equal(report.byStreet.turn, 2);
    assert.deepEqual(report.artifacts, [SHA]);
  });

  it('counts failed actions separately and ignores non-decisions', () => {
    const entries: CoverageEntry[] = [
      { ...entry({}), ok: false },
      { ts: 1, kind: 'hand-result' },
      { ts: 4, kind: 'hand-result' },
      { ts: 5, decision: decision({ guaranteeLevel: 'certified_bound', artifactSha256: SHA_B }) },
    ];
    const report = summarizeCoverage(entries, 2);
    assert.equal(report.actionable, 1); // only the ts>=2 decision
    assert.equal(report.failed, 0); // the failure is before the since cutoff
    assert.deepEqual(report.artifacts, [SHA_B]);
  });

  it('deduplicates artifacts and lists distinct digests sorted', () => {
    const entries = [
      entry({ decision: decision({ guaranteeLevel: 'certified_bound', artifactSha256: SHA_B }) }),
      entry({ decision: decision({ guaranteeLevel: 'certified_bound', artifactSha256: SHA }) }),
      entry({ decision: decision({ guaranteeLevel: 'certified_bound', artifactSha256: SHA }) }),
    ];
    assert.deepEqual(summarizeCoverage(entries).artifacts, [SHA, SHA_B]);
  });

  it('reasonPrefix takes the token before colon or whitespace', () => {
    assert.equal(reasonPrefix('turn_root_unserved from x'), 'turn_root_unserved');
    assert.equal(reasonPrefix('operational-fallback: check'), 'operational-fallback');
    assert.equal(reasonPrefix('blueprint:12'), 'blueprint');
    assert.equal(reasonPrefix('   '), 'unknown');
  });

  it('parses a log with malformed lines and formats stable output', () => {
    const text = [
      JSON.stringify(entry({ street: 'river', decision: decision({ guaranteeLevel: 'approximate' }) })),
      'not json',
      '',
      JSON.stringify(entry({ street: 'river' })),
    ].join('\n');
    const report = summarizeCoverage(parseResultsLog(text));
    assert.equal(report.actionable, 2);
    assert.equal(report.byStreet.river, 2);
    const formatted = formatCoverage(report);
    assert.match(formatted, /actionable=2/);
    assert.match(formatted, /resolved=1/);
    assert.match(formatted, /fallback=1/);
    assert.match(formatted, /fallback-reasons=\{"unknown":1\}/);
  });
});
