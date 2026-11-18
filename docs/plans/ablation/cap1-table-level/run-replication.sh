#!/bin/zsh
# D-346 independent replication campaign: same five scripted styles and
# budget as run 1 (seeds 401-405) but fresh deck seeds 501-505, so the two
# campaigns are independent samples. The wall-clock solve budget makes
# individual transcripts non-bytewise-reproducible even at fixed seeds;
# replication at the frequency level is therefore the correct reproducibility
# test.
set -u
cd /home/zhangdi.zode/Develop/BigShark
D=docs/plans/ablation/cap1-table-level
export ENGINE_PATH=$D/engine-hu-root.sh
: > $D/run2.timing
run() {
  local policy=$1 seed=$2
  local log=$D/r2-${policy}-400-seed${seed}.log
  local t0=$(date +%s.%N)
  node $D/drive-practice.mjs "$policy" 400 "$seed" 300 "$log" \
    --engine-timeout-ms 120000 2> "$log.driver-err"
  local t1=$(date +%s.%N)
  echo "$policy seed$seed wall=$((t1-t0))" | tee -a "$D/run2.timing"
}
run station 501
run folder 502
run passive-random 503
run uniform 504
run srp-passive 505
echo RUN2_DONE >> $D/run2.timing
