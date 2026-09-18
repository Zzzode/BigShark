// RFC 0005 Stage 9 adapter resolving-eligibility tests. Covers the monotone
// enrollment/become-eligible sequence, fresh-root enrollment, manual override,
// miss/off-tree, changed digest, process-restart (fresh state), new-hand reset,
// and the forced-mode-without-eligibility case.
import assert from 'node:assert/strict';
import test from 'node:test';

import {
  ResolverEligibility,
  type ArtifactRootSnapshot,
} from '../src/resolver_eligibility.js';

const PINNED = 'a'.repeat(64);
const OTHER = 'b'.repeat(64);

const root = (digest: string, rootKey: string, complete = true): ArtifactRootSnapshot =>
  ({ digest, rootKey, complete });

test('eligible only after a fresh pinned-root snapshot AND matching blueprint play', () => {
  const e = new ResolverEligibility(PINNED);
  assert.equal(e.isEligible(), false);
  assert.equal(e.canRequestResolving({ digest: PINNED, rootKey: 'flop-root' }), false);

  e.observeRootSnapshot(root(PINNED, 'flop-root'));
  assert.equal(e.isEnrolled(), true);
  // Enrolled but the matching blueprint has not been used yet.
  assert.equal(e.isEligible(), false);
  assert.equal(e.canRequestResolving({ digest: PINNED, rootKey: 'flop-root' }), false);

  e.observeBlueprintExecution(root(PINNED, 'flop-root'));
  assert.equal(e.isEligible(), true);
  assert.equal(e.canRequestResolving({ digest: PINNED, rootKey: 'flop-root' }), true);
});

test('an incomplete or non-pinned snapshot never enrolls', () => {
  const e = new ResolverEligibility(PINNED);
  e.observeRootSnapshot(root(PINNED, 'flop-root', false));
  assert.equal(e.isEnrolled(), false);
  e.observeBlueprintExecution(root(PINNED, 'flop-root'));
  assert.equal(e.isEligible(), false);

  // A different pinned digest is a hard invalidation for the hand.
  e.observeRootSnapshot(root(OTHER, 'flop-root'));
  assert.equal(e.isEligible(), false);
  assert.equal(e.invalidationReason(), 'changed-digest');
  // Reaching the same public history with the right digest cannot restore it.
  e.observeRootSnapshot(root(PINNED, 'flop-root'));
  assert.equal(e.isEnrolled(), false);
});

test('blueprint play at a different root is an off-tree invalidation', () => {
  const e = new ResolverEligibility(PINNED);
  e.observeRootSnapshot(root(PINNED, 'flop-a'));
  e.observeBlueprintExecution(root(PINNED, 'flop-b'));
  assert.equal(e.isEligible(), false);
  assert.equal(e.invalidationReason(), 'off-tree-miss');
  assert.equal(e.canRequestResolving({ digest: PINNED, rootKey: 'flop-a' }), false);
});

test('manual override and heuristic fallback permanently clear the hand', () => {
  for (const reason of ['manual-override', 'heuristic-fallback', 'malformed-response',
                        'uncertified-execution', 'off-tree-miss'] as const) {
    const e = new ResolverEligibility(PINNED);
    e.observeRootSnapshot(root(PINNED, 'flop-root'));
    e.observeBlueprintExecution(root(PINNED, 'flop-root'));
    assert.equal(e.isEligible(), true);
    e.invalidate(reason);
    assert.equal(e.isEligible(), false);
    assert.equal(e.isEnrolled(), false);
    // Identical snapshots after the invalidation must not restore it.
    e.observeRootSnapshot(root(PINNED, 'flop-root'));
    e.observeBlueprintExecution(root(PINNED, 'flop-root'));
    assert.equal(e.isEligible(), false);
    assert.equal(e.invalidationReason(), reason);
  }
});

test('forced resolving with no eligibility stays suppressed', () => {
  const e = new ResolverEligibility(PINNED);
  // Even at the right root, without enrollment/blueprint provenance the adapter
  // must not assert resolving.
  assert.equal(e.canRequestResolving({ digest: PINNED, rootKey: 'flop-root' }), false);
});

test('a new hand resets every flag (mid-hand recovery loses eligibility)', () => {
  const e = new ResolverEligibility(PINNED);
  e.observeRootSnapshot(root(PINNED, 'flop-root'));
  e.observeBlueprintExecution(root(PINNED, 'flop-root'));
  assert.equal(e.isEligible(), true);
  e.invalidate('manual-override');

  // A new hand can re-enroll from scratch.
  e.resetHand();
  assert.equal(e.isEligible(), false);
  assert.equal(e.invalidationReason(), 'manual-override', 'reason retained for the report');
  e.observeRootSnapshot(root(PINNED, 'flop-root'));
  e.observeBlueprintExecution(root(PINNED, 'flop-root'));
  assert.equal(e.isEligible(), true);
  assert.equal(e.canRequestResolving({ digest: PINNED, rootKey: 'flop-root' }), true);
});

test('process/session restart is a fresh, ineligible state', () => {
  const first = new ResolverEligibility(PINNED);
  first.observeRootSnapshot(root(PINNED, 'flop-root'));
  first.observeBlueprintExecution(root(PINNED, 'flop-root'));
  assert.equal(first.isEligible(), true);
  // A restart constructs brand-new per-session state with no provenance.
  const restarted = new ResolverEligibility(PINNED);
  assert.equal(restarted.isEligible(), false);
  restarted.invalidate('process-restart');
  assert.equal(restarted.canRequestResolving({ digest: PINNED, rootKey: 'flop-root' }), false);
});

test('a new root on the same hand does not carry eligibility across', () => {
  const e = new ResolverEligibility(PINNED);
  e.observeRootSnapshot(root(PINNED, 'flop-root'));
  e.observeBlueprintExecution(root(PINNED, 'flop-root'));
  assert.equal(e.isEligible(), true);
  // Resolving can only be requested against the enrolled artifact root.
  assert.equal(e.canRequestResolving({ digest: PINNED, rootKey: 'different-root' }), false);
});
