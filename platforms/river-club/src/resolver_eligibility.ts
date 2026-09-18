// RFC 0005 Stage 9 per-hand, monotone resolving eligibility (adapter side).
//
// The engine host is STATELESS: no eligibility bit crosses the RPC, and an
// explicit SOLVER_MODE_RESOLVING request is only the adapter's assertion that
// the eligible-prefix precondition held. This module owns that assertion's
// provenance for one hand.
//
// Eligibility begins at the exact artifact root, never at hand start:
//   1. a FRESH, COMPLETE artifact-root snapshot carrying the operator-pinned
//      digest enrolls the hand (preflop chart play does not enroll);
//   2. the flag becomes true only after the MATCHING blueprint distribution is
//      actually used at that root;
//   3. any manual override, operational/heuristic fallback, miss or off-tree
//      node, changed artifact digest, malformed/failed/uncertified execution,
//      or process/session restart permanently clears the hand until a new hand
//      starts. Reaching the same public history never restores eligibility.
//
// The class is synchronous and free of any RPC or process state so it can be
// unit tested directly and driven identically by the live frame loop.

// Stable identity of one artifact-root snapshot as the adapter observes it.
export interface ArtifactRootSnapshot {
  // 64-lowercase-hex digest reported by the host for this snapshot.
  digest: string;
  // Canonical artifact-root identity (board/history prefix, pot, stacks,
  // button). Two snapshots with different keys are different enrollments.
  rootKey: string;
  // True only for a fresh, complete artifact-root snapshot. A partial or
  // non-root snapshot never enrolls.
  complete: boolean;
}

export type ResolveInvalidationReason =
  | 'manual-override'
  | 'heuristic-fallback'
  | 'off-tree-miss'
  | 'changed-digest'
  | 'malformed-response'
  | 'uncertified-execution'
  | 'process-restart';

export class ResolverEligibility {
  private readonly pinnedDigest: string;
  private enrolled = false;
  private eligible = false;
  // Once the hand is invalidated it cannot recover before a new hand.
  private handDead = false;
  private enrolledRootKey: string | null = null;

  constructor(pinnedDigest: string) {
    this.pinnedDigest = pinnedDigest;
  }

  // Begin a new hand: every flag resets, including on mid-hand recovery.
  resetHand(): void {
    this.enrolled = false;
    this.eligible = false;
    this.handDead = false;
    this.enrolledRootKey = null;
  }

  // A fresh complete artifact-root snapshot with the pinned digest enrolls the
  // hand. A non-root, incomplete, or mismatched-digest snapshot does not enroll
  // and, when the digest differs, permanently invalidates the hand.
  observeRootSnapshot(snapshot: ArtifactRootSnapshot): void {
    if (this.handDead)
      return;
    if (snapshot.digest !== this.pinnedDigest) {
      this.invalidate('changed-digest');
      return;
    }
    if (!snapshot.complete)
      return;
    this.enrolled = true;
    this.enrolledRootKey = snapshot.rootKey;
    // Becoming eligible still requires the matching blueprint to be used.
    this.eligible = false;
  }

  // Called when the MATCHING blueprint distribution was actually executed at
  // the enrolled artifact root. Only this moves the hand from enrolled to
  // eligible. An execution at a different root is an off-tree invalidation.
  observeBlueprintExecution(snapshot: ArtifactRootSnapshot): void {
    if (this.handDead || !this.enrolled)
      return;
    if (snapshot.digest !== this.pinnedDigest ||
        snapshot.rootKey !== this.enrolledRootKey) {
      this.invalidate(snapshot.digest !== this.pinnedDigest
        ? 'changed-digest'
        : 'off-tree-miss');
      return;
    }
    this.eligible = true;
  }

  // Any non-blueprint/non-resolving execution, override, miss, or failure
  // permanently clears the hand.
  invalidate(reason: ResolveInvalidationReason): void {
    this.enrolled = false;
    this.eligible = false;
    this.handDead = true;
    this.enrolledRootKey = null;
    this.lastReason = reason;
  }

  private lastReason: ResolveInvalidationReason | null = null;
  invalidationReason(): ResolveInvalidationReason | null {
    return this.lastReason;
  }

  isEnrolled(): boolean {
    return this.enrolled && !this.handDead;
  }

  isEligible(): boolean {
    return this.enrolled && this.eligible && !this.handDead;
  }

  // The adapter may send SOLVER_MODE_RESOLVING only when enrolled, eligible,
  // still on the pinned artifact, and at the enrolled root. The actual current
  // request may be a descendant; only the artifact-root identity must match.
  canRequestResolving(request: { digest: string; rootKey: string }): boolean {
    return this.isEligible()
      && request.digest === this.pinnedDigest
      && request.rootKey === this.enrolledRootKey;
  }
}
