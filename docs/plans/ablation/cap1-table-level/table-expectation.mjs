#!/usr/bin node
// D-346 table-level expectation calculator.
//
// Combines measured per-occurrence cap-1 structural residuals (sealed RFC
// 0047/0049 receipts, per engine seat) with measured reach frequencies
// (parse-hands.mjs summary CSV) into table-level BB/100 expectations.
//
// Usage:
//   node table-expectation.mjs <summary.csv> <out.csv>
//
// Severity constants are transcribed from sealed receipts cited in
// docs/decisions/cap1-table-level-expectation.md; the script prints the
// source of every constant in its stderr banner.
import { readFileSync, writeFileSync } from 'node:fs';

// Two-seat summed exploitability from the T6 six-stratum anchor sweep
// (docs/plans/0047-stage1-tracker.md, "T6 SIX-STRATUM ANCHOR SCOPE RESULT"),
// divided by two for the per-engine-seat loss. The five limped strata share
// the check-to-river pot-4 root shape; raised-srp is the pot-8 SRP root.
const t6 = {
  limp: {
    strata: [
      ['w1', 19.4822, 152.9223],
      ['unpaired-rainbow-ahigh', 19.4486, 154.0793],
      ['paired', 19.5817, 154.2606],
      ['monotone', 20.8220, 169.7682],
      ['unpaired-twotone', 19.5265, 153.7816],
    ],
    srp: ['raised-srp', 29.4414, 188.7606],
  },
};
const half = (x) => x / 2;
const limpU = t6.limp.strata.map(([, u]) => half(u));
const limpB = t6.limp.strata.map(([, , b]) => half(b));
const mean = (xs) => xs.reduce((a, b) => a + b, 0) / xs.length;
const L_U_MEAN = mean(limpU);
const L_U_MIN = Math.min(...limpU);
const L_U_MAX = Math.max(...limpU);
const L_B_MEAN = mean(limpB);
const L_B_MIN = Math.min(...limpB);
const L_B_MAX = Math.max(...limpB);
const S_U = half(t6.limp.srp[1]);
const S_B = half(t6.limp.srp[2]);

// Union reference (w1 coarse, 300 iters), stage2b-union/w1-coarse-300.csv:
// exact_exploitability_bb 31.24489667 (uniform) and 215.34663379 (br_fav),
// per engine seat = half. Measured on w1 only; extrapolation to every limped
// spot is explicitly labeled in the decision doc.
const U_UNION_U = half(31.24489667);
const U_UNION_B = half(215.34663379);

process.stderr.write([
  'severity constants (BB per engine seat, per occurrence):',
  `  limp T6 Uniform  mean=${L_U_MEAN.toFixed(4)} range=[${L_U_MIN.toFixed(4)}, ${L_U_MAX.toFixed(4)}]`,
  `  limp T6 BrFav    mean=${L_B_MEAN.toFixed(4)} range=[${L_B_MIN.toFixed(4)}, ${L_B_MAX.toFixed(4)}]`,
  `  srp  T6 Uniform  ${S_U.toFixed(4)}   BrFav ${S_B.toFixed(4)}`,
  `  limp union(w1)   Uniform ${U_UNION_U.toFixed(4)}  BrFav ${U_UNION_B.toFixed(4)}`,
].join('\n') + '\n');

const summary = readFileSync(process.argv[2], 'utf8').trim().split('\n').slice(1)
  .map((line) => {
    const [run, hands, event, count, rate, se] = line.split(',');
    return { run, hands: Number(hands), event, count: Number(count),
             rate: Number(rate), se: Number(se) };
  });
const byRun = new Map();
for (const row of summary) {
  if (!byRun.has(row.run)) byRun.set(row.run, { hands: row.hands });
  byRun.get(row.run)[row.event] = row;
}

// Optional pooled pseudo-run: --pool outname=run1,run2,... pools binomial
// counts across the named runs (correct for different run lengths) instead
// of averaging rates.
const poolArg = process.argv.find((a) => a.startsWith('--pool='));
let poolSpec = null;
if (poolArg) {
  const [outName, list] = poolArg.slice('--pool='.length).split('=');
  poolSpec = { outName, runs: list.split(',') };
}
if (poolSpec) {
  const events = ['limp_ctr', 'srp_ctr', 'other_river', 'allin_runout'];
  const totalHands = poolSpec.runs.reduce((s, name) => s + byRun.get(name).hands, 0);
  const pooled = { hands: totalHands };
  for (const ev of events) {
    const c = poolSpec.runs.reduce((s, name) => s + byRun.get(name)[ev].count, 0);
    const rate = c / totalHands;
    pooled[ev] = { count: c, rate, se: Math.sqrt(rate * (1 - rate) / totalHands) };
  }
  byRun.set(poolSpec.outName, pooled);
}

const out = [
  'run,hands,basis,component,per_hand_bb,bb_per_100,se_bb_per_100',
];
const push = (run, hands, basis, component, perHand, sePerHand) => {
  out.push([run, hands, basis, component, perHand.toFixed(6),
            (100 * perHand).toFixed(4), (100 * sePerHand).toFixed(4)].join(','));
};

for (const [run, r] of byRun) {
  const n = r.hands;
  const fLimp = r.limp_ctr.rate, seLimp = r.limp_ctr.se;
  const fSrp = r.srp_ctr.rate, seSrp = r.srp_ctr.se;
  const fOther = r.other_river.rate, seOther = r.other_river.se;
  const fAllin = r.allin_runout ? r.allin_runout.rate : 0;

  // Measured component: only root classes with sealed per-occurrence losses.
  const calc = (lLoss, sLoss) => {
    const perHand = fLimp * lLoss + fSrp * sLoss;
    const se = Math.hypot(seLimp * lLoss, seSrp * sLoss);
    return { perHand, se };
  };
  // T6 Uniform band: limp mean with stratum extrema, srp point value.
  for (const [tag, lLoss] of [['t6u_limp_mean', L_U_MEAN],
                              ['t6u_limp_min', L_U_MIN],
                              ['t6u_limp_max', L_U_MAX]]) {
    const { perHand, se } = calc(lLoss, S_U);
    push(run, n, 't6_uniform', tag, perHand, se);
  }
  const bMean = calc(L_B_MEAN, S_B);
  push(run, n, 't6_brfav', 'brfav_limp_mean', bMean.perHand, bMean.se);
  const bMax = calc(L_B_MAX, S_B);
  push(run, n, 't6_brfav', 'brfav_limp_max', bMax.perHand, bMax.se);

  // Union reference (w1-derived) applied to limp class; srp keeps its T6 value
  // because no union SRP measurement exists.
  const unionU = calc(U_UNION_U, S_U);
  push(run, n, 'union_uniform', 'union_limp_w1', unionU.perHand, unionU.se);
  const unionB = calc(U_UNION_B, S_B);
  push(run, n, 'union_brfav', 'union_limp_w1', unionB.perHand, unionB.se);

  // Unknown other-river component. Lower branch: zero (measured component
  // alone). Upper proxy: charge every non-all-in other-river hand the srp
  // per-occurrence value, the more severe of the two measured classes under
  // T6 Uniform. All-in runouts are structurally zero in both branches: once
  // every remaining chip is committed no raise decision exists.
  const base = calc(L_U_MEAN, S_U);
  const upper = base.perHand + fOther * S_U;
  const upperSe = Math.hypot(base.se, seOther * S_U);
  push(run, n, 't6_uniform_band', 'lower_other_zero', base.perHand, base.se);
  push(run, n, 't6_uniform_band', 'upper_other_eq_srp', upper, upperSe);
  // All-in runout frequency (from the summary CSV) contributes zero BB in
  // both branches; this row pins that structural zero explicitly.
  push(run, n, 'structural_zero', `allin_runout(f=${fAllin.toFixed(4)})`, 0, 0);
}

writeFileSync(process.argv[3], out.join('\n') + '\n');
process.stdout.write(out.join('\n') + '\n');
