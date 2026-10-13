import { isRiverRoom, type Action, type RiverRoom } from '../../platforms/river-club/src/types.js';

export type ReplaySource = 'historical' | 'synthetic';
export interface ReplaySnapshot {
  room: RiverRoom;
  heroName: string;
  source: ReplaySource;
  file: string;
  line: number;
}
export interface ReplayStats {
  lines: number;
  malformed: number;
  skipped: number;
  duplicates: number;
}
function object(value: unknown): value is Record<string, unknown> {
  return typeof value === 'object' && value !== null && !Array.isArray(value);
}

// Scope deduplication to a room: hand ids and revisions may recur at other tables.
export function collectReplaySnapshots(
  journals: readonly { name: string; text: string }[], source: ReplaySource,
): { snapshots: ReplaySnapshot[]; stats: ReplayStats } {
  const snapshots: ReplaySnapshot[] = [];
  const seen = new Set<string>();
  const stats: ReplayStats = { lines: 0, malformed: 0, skipped: 0, duplicates: 0 };
  for (const journal of journals) {
    const lines = journal.text.split('\n');
    for (const [index, line] of lines.entries()) {
      if (!line.trim()) continue;
      stats.lines++;
      let record: unknown;
      try { record = JSON.parse(line) as unknown; }
      catch { stats.malformed++; continue; }
      const response = object(record) && object(record.resp) ? record.resp : undefined;
      const room = response?.room;
      if (!isRiverRoom(room) || room.next !== 'act' || !room.legal
        || !Array.isArray(room.legal.actions) || room.legal.actions.length === 0
        || room.hero.cards.length !== 2 || !Number.isSafeInteger(room.revision)
        || room.revision < 0 || !room.handId || room.street === 'showdown') {
        stats.skipped++; continue;
      }
      const key = JSON.stringify([room.id, room.handId, room.revision, room.street]);
      if (seen.has(key)) { stats.duplicates++; continue; }
      seen.add(key);
      const me = response?.me;
      const heroName = object(me) && typeof me.name === 'string' ? me.name : room.hero.name ?? '';
      snapshots.push({ room, heroName, source, file: journal.name, line: index + 1 });
    }
  }
  return { snapshots, stats };
}

export function decisionProblem(
  room: RiverRoom, decision: { action: Action; amount?: number } | undefined,
): string | null {
  if (!decision || !room.legal?.actions.includes(decision.action)) return 'illegal action';
  if (decision.action === 'bet' || decision.action === 'raise') {
    const range = room.legal.raiseTo;
    if (!range || !Number.isFinite(range.min) || !Number.isFinite(range.max)
      || range.min > range.max || decision.amount === undefined
      || !Number.isFinite(decision.amount) || decision.amount < range.min
      || decision.amount > range.max) return 'amount outside legal target interval';
  }
  return null;
}

export function replayExitCode(illegal: number, fallbacks: number): number {
  return illegal === 0 && fallbacks === 0 ? 0 : 1;
}
