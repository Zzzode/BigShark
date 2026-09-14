import assert from 'node:assert/strict';
import { spawnSync } from 'node:child_process';
import { test } from 'node:test';
import { fileURLToPath } from 'node:url';
import { validateDecisionRecord } from './decision-record.js';

const approved = [
  'Author agent: author-session',
  'Approved by: independent-review-session',
  'Decision date: 2026-09-14',
  'Review outcome: Approved',
  'Reviewed scope: RFC 0004 current complete proposal',
  'Review summary: No blocking findings; runtime evidence remains required.',
].join('\n');

test('draft and proposed RFCs wait for independent agent review', () => {
  for (const status of ['Draft', 'Proposed']) {
    assert.deepEqual(validateDecisionRecord('0004', status,
      'Pending independent approval-agent review.'), []);
    assert.notEqual(validateDecisionRecord('0004', status,
      'Pending explicit maintainer approval.').length, 0);
    assert.notEqual(validateDecisionRecord('0004', status, '').length, 0);
  }
});

test('accepted and implemented RFCs require a complete independent decision', () => {
  for (const status of ['Accepted', 'Implementing', 'Implemented']) {
    assert.deepEqual(validateDecisionRecord('0004', status, approved), []);
    for (const field of approved.split('\n')) {
      assert.notEqual(validateDecisionRecord('0004', status,
        approved.replace(field, '')).length, 0, field);
    }
  }
});

test('self approval, empty records, conflicts and conditional decisions fail', () => {
  const invalid = [
    approved.replace('independent-review-session', 'author-session'),
    approved.replace('independent-review-session', '   '),
    approved.replace('2026-09-14', 'today'),
    approved.replace('Review outcome: Approved', 'Review outcome: Changes Requested'),
    approved.replace('Review outcome: Approved', 'Review outcome: Approved with blockers'),
    `${approved}\nApproved by: another-agent`,
    `${approved}\nRejected by: another-agent`,
  ];
  for (const decision of invalid) {
    assert.notEqual(validateDecisionRecord('0004', 'Accepted', decision).length, 0);
  }
});

test('rejection uses a separate agent and a complete rejection record', () => {
  const rejected = approved.replace('Approved by:', 'Rejected by:')
    .replace('Review outcome: Approved', 'Review outcome: Rejected');
  assert.deepEqual(validateDecisionRecord('0004', 'Rejected', rejected), []);
  assert.notEqual(validateDecisionRecord('0004', 'Rejected', approved).length, 0);
  assert.notEqual(validateDecisionRecord('0004', 'Rejected',
    rejected.replace('independent-review-session', 'author-session')).length, 0);
});

test('only the three existing maintainer approvals are grandfathered', () => {
  const legacy = 'Approved by: BigShark project maintainer\nDecision date: 2026-09-13';
  for (const id of ['0001', '0002', '0003']) {
    assert.deepEqual(validateDecisionRecord(id, 'Implemented', legacy), []);
    for (const status of ['Accepted', 'Implementing', 'Implemented']) {
      assert.ok(validateDecisionRecord(id, status,
        `${legacy}\nRejected by: independent-review-session`)
        .includes('decision must not contain conflicting approval and rejection fields'));
    }
    assert.notEqual(validateDecisionRecord(id, 'Accepted',
      legacy.replace('2026-09-13', '2026-09-14')).length, 0);
  }
  assert.notEqual(validateDecisionRecord('0004', 'Accepted', legacy).length, 0);
  assert.deepEqual(validateDecisionRecord('0001', 'Accepted', approved), []);
});

test('superseded records retain their historical decision text', () => {
  assert.deepEqual(validateDecisionRecord('0004', 'Superseded', 'Historical decision.'), []);
});

test('public RFC checker accepts the repository records', () => {
  const root = fileURLToPath(new URL('../../../', import.meta.url));
  const result = spawnSync(process.execPath, ['bin/check-rfcs.mjs'], {
    cwd: root,
    encoding: 'utf8',
  });
  assert.equal(result.status, 0, result.stderr);
  assert.match(result.stdout, /RFC checks passed/);
});
