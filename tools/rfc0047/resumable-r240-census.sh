#!/usr/bin/env bash
# RFC 0047 S3 resumable R240 river-subgame census orchestrator.
#
# The box this runs on is shared and rarely offers one long idle window: the
# earlier whole-census probe was killed by co-tenant load >100 times and never
# completed. This orchestrator makes progress in short, checkpointed,
# monotonically increasing batches instead.
#
#   * Durable per-group store docs/plans/ablation/rfc0047/s3/r240-done.csv;
#     a completed group is never solved again, so an interrupted or re-run
#     campaign resumes at the smallest missing group instead of restarting.
#   * Fixed-size consecutive group batches sized to fit an observed short idle
#     window. Each batch invokes the online-river probe with GROUP_START/
#     GROUP_END window bounds and a single ONLY_ITER checkpoint.
#   * A 5-minute-load-average watchdog (>8) kills the current batch, salvages
#     every row already flushed to its temp CSV, and waits for load to drop
#     before resuming. No completed group is lost.
#   * NashConv is deterministic and seedless, so salvaged partial rows are
#     bit-identical to a fresh run; the merged 240-group store equals one
#     stride-1 full census. This script never touches solver or R0 semantics.
#
# Usage: tools/rfc0047/resumable-r240-census.sh
# Tunables (BATCH/ITERS/THREADS/load threshold) are constants below.
set -u
cd "$(git rev-parse --show-toplevel)"
BIN=build/release/engine/bigshark-rfc0047-online-river
OUT=docs/plans/ablation/rfc0047/s3
REC="$OUT/r240-census"            # certified evidence of record (committed)
DONE="$REC/r240-done.csv"
LOG="$OUT/orchestrator-resume.log"
TOTAL=240
BATCH=8            # ~8 groups x ~7s median wall ~= ~60s, inside short windows
ITERS=4000
THREADS=48
LOAD_MAX=8         # 5-minute load-average gate; pause above this
mkdir -p "$OUT" "$REC"
log(){ echo "$(date '+%F %T') $*" | tee -a "$LOG"; }
load5(){ uptime | awk -F'load average:' '{print $2}' | awk -F',' '{gsub(/ /,"",$2); print $2}'; }
gt_max(){ awk -v l="$(load5)" -v m="$LOAD_MAX" 'BEGIN{exit !(l+0 > m)}'; }
lt_max(){ awk -v l="$(load5)" -v m="$LOAD_MAX" 'BEGIN{exit !(l+0 < m)}'; }

done_count(){ awk 'NR>1' "$DONE" 2>/dev/null | wc -l | tr -d ' '; }
# Smallest group index not yet present (groups fill in ascending order).
next_group(){ awk -F, 'NR>1{d[$2]=1} END{g=0; while(g in d)g++; print g}' "$DONE"; }

# Append temp rows whose group is not already in the durable store, then drop
# the temp file. Header rows are skipped; dedup is by group column.
salvage(){
  local tmp="$1"
  [ -f "$tmp" ] || return 0
  local before after
  before=$(done_count)
  awk -F, 'NR==FNR{ if(FNR>1) done[$2]=1; next }
           FNR==1{ next }
           !($2 in done)' "$DONE" "$tmp" >> "$DONE"
  after=$(done_count)
  rm -f "$tmp"
  log "salvage +$((after-before)) groups (store now $after/$TOTAL)"
}

wait_idle(){ until lt_max; do log "wait load5=$(load5) >=$LOAD_MAX"; sleep 30; done; }

log "ORCHESTRATOR RESUME START; store has $(done_count)/$TOTAL groups"
while :; do
  have=$(done_count)
  [ "$have" -ge "$TOTAL" ] && break
  wait_idle
  start=$(next_group)
  end=$(( start + BATCH )); [ "$end" -gt "$TOTAL" ] && end=$TOTAL
  tmp="$OUT/batch-${start}-${end}.csv"
  rm -f "$tmp"
  log "BATCH [$start,$end) START load5=$(load5)"
  "$BIN" w1 "$ITERS" 1 "$tmp" "$THREADS" "$ITERS" "$start" "$end" >>"$LOG" 2>&1 &
  pid=$!
  paused=0
  while kill -0 "$pid" 2>/dev/null; do
    sleep 15
    if gt_max; then
      log "WATCHDOG load5=$(load5)>$LOAD_MAX -> pause batch [$start,$end), kill $pid"
      kill -TERM "$pid" 2>/dev/null; sleep 2; kill -KILL "$pid" 2>/dev/null
      wait "$pid" 2>/dev/null
      paused=1
      break
    fi
  done
  wait "$pid" 2>/dev/null
  salvage "$tmp"
  [ "$paused" -eq 0 ] && log "BATCH [$start,$end) finished cleanly"
done

# Final integrity + R0 certification over the full store.
awk -F, 'NR>1{d[$2]=$0; n++}
END{
  miss=0; for(g=0;g<240;g++) if(!(g in d)) miss++;
  worst=0; for(g in d){ split(d[g],f,","); if(f[7]+0>worst) worst=f[7]+0 }
  printf "groups=%d missing=%d worst_R0_bb=%.8f gate_0.002=%s\n", \
         n, miss, worst, (worst<0.002?"PASS":"FAIL");
}' "$DONE" | tee "$REC/r240-summary.txt"
log "ORCHESTRATOR RESUME COMPLETE"
