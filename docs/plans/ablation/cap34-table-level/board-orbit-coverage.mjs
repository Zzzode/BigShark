#!/usr/bin node
// D-380 structural coverage-cost curve over the 134,459 canonical river
// board classes.
//
// Each canonical class is an orbit of concrete five-card boards under the
// 24 suit permutations; its orbit size is the exact deal-frequency mass of
// the class under a uniform random river deal (the same joint-deal
// normalization the seedless engine uses). This script re-derives every
// orbit size with the canonicalization rule pinned by the native ctest
// (board_classes.cpp: rank-major card ids, component-wise minimum over the
// 24 suit permutations after sorting) and validates three sealed invariants:
//   1. class count == 134,459
//   2. summed orbit mass == C(52,5) == 2,598,960
//   3. monochrome classes == C(13,5) == 1,287, each orbit size 4
// Offline analysis tooling only; no engine file is touched or imported.
//
// Usage: node board-orbit-coverage.mjs
import { writeFileSync } from 'node:fs';

const N = 52;
const K = 5;
const PERMS = (() => {
  const out = [];
  const gen = (cur, rest) => {
    if (rest.length === 0) { out.push(cur.slice()); return; }
    for (let i = 0; i < rest.length; i++) {
      cur.push(rest[i]);
      gen(cur, rest.filter((_, j) => j !== i));
      cur.pop();
    }
  };
  gen([], [0, 1, 2, 3]);
  return out;
})();
if (PERMS.length !== 24) throw new Error('suit permutation count');

const canonicalKey = (board) => {
  let best = Array(K).fill(N);
  for (const perm of PERMS) {
    const mapped = board.map((c) => (Math.floor(c / 4) * 4) + perm[c % 4]).sort((a, b) => a - b);
    for (let i = 0; i < K; i++) {
      if (mapped[i] < best[i]) { best = mapped; break; }
      if (mapped[i] > best[i]) break;
    }
  }
  return best.join(',');
};

// All C(52,5) concrete boards via lexicographic combinations.
const counts = new Map();
const combo = [0, 1, 2, 3, 4];
let boards = 0;
while (true) {
  const key = canonicalKey(combo);
  counts.set(key, (counts.get(key) ?? 0) + 1);
  boards++;
  let i = K - 1;
  while (i >= 0 && combo[i] === N - K + i) i--;
  if (i < 0) break;
  combo[i]++;
  for (let j = i + 1; j < K; j++) combo[j] = combo[j - 1] + 1;
}

// Invariant 1 + 2.
const totalMass = [...counts.values()].reduce((a, b) => a + b, 0);
if (counts.size !== 134459) throw new Error(`class count ${counts.size} != 134459`);
if (totalMass !== 2598960) throw new Error(`mass ${totalMass} != 2598960`);
if (boards !== 2598960) throw new Error(`boards ${boards}`);

// Invariant 3: every monochrome board (5 cards one suit) canonicalizes to a
// class whose orbit is exactly the 4 suit choices; C(13,5)=1287 classes.
const monoClasses = new Set();
for (let s = 0; s < 4; s++) {
  const ranks = [];
  for (let a = 0; a < 13; a++) for (let b = a + 1; b < 13; b++)
    for (let c = b + 1; c < 13; c++) for (let d = c + 1; d < 13; d++)
      for (let e = d + 1; e < 13; e++)
        monoClasses.add(canonicalKey([a, b, c, d, e].map((r) => r * 4 + s)));
}
if (monoClasses.size !== 1287) throw new Error(`mono classes ${monoClasses.size}`);
for (const k of monoClasses) if (counts.get(k) !== 4) throw new Error('mono orbit size');

// Orbit-size histogram.
const hist = new Map();
for (const m of counts.values()) hist.set(m, (hist.get(m) ?? 0) + 1);
const histLines = ['orbit_size,classes,cumulative_mass'];
let cum = 0;
for (const [size, n] of [...hist.entries()].sort((a, b) => a[0] - b[0])) {
  cum += size * n;
  histLines.push(`${size},${n},${cum}`);
}

// Coverage curve: classes ranked by descending orbit mass.
const ranked = [...counts.values()].sort((a, b) => b - a);
const TOTAL = 2598960;
const CLASSES = 134459;
// Measured per-anchor walls (seconds, Uniform arm, 28 threads) from the
// D-370/D-371 gate2 receipts; one solve per canonical class at the w1 ledger.
const builds = [
  ['cap3-20k', 482.319402],
  ['cap4-20k', 1482.597121],
  ['cap4-6k', 463.095160],
];
const targets = [0.5, 0.8, 0.95, 0.99];
const curve = ['coverage_target,classes_needed,share_of_classes,mass_covered,' +
  builds.map(([t]) => `${t}_machine_hours,${t}_days_100slots`).join(',')];
let idx = 0;
let mass = 0;
for (const q of targets) {
  while (mass / TOTAL < q) mass += ranked[idx++];
  const share = idx / CLASSES;
  const cols = builds.map(([, wallS]) => {
    const h = (idx * wallS) / 3600;
    return `${h.toFixed(1)},${(h / 100 / 24).toFixed(2)}`;
  });
  curve.push(`${q},${idx},${share.toFixed(4)},${(mass / TOTAL).toFixed(6)},${cols.join(',')}`);
}
// Full-universe row for reference against the 55,375 machine-hour line.
const full = builds.map(([, wallS]) => {
  const h = (CLASSES * wallS) / 3600;
  return `${h.toFixed(1)},${(h / 100 / 24).toFixed(2)}`;
});
curve.push(`1.0,${CLASSES},1.0000,1.000000,${full.join(',')}`);

const out = ['# invariant checks: classes=134459 mass=2598960 mono=1287x4 all PASS',
 ...histLines, '', ...curve].join('\n') + '\n';
process.stdout.write(out);
writeFileSync(new URL('./board-orbit-coverage.csv', import.meta.url), out);
