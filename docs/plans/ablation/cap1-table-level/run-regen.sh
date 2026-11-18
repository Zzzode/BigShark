#!/bin/zsh
# D-346 regeneration run: recreate the five mounted-engine transcripts from
# the recorded seeds after an accidental local deletion. Outputs are
# byte-deterministic for a fixed binary and seed (CRN deck + solver seeds).
set -u
cd /home/zhangdi.zode/Develop/BigShark
D=docs/plans/ablation/cap1-table-level
export ENGINE_PATH=$D/engine-hu-root.sh
: > $D/regen.timing
run() {
  local policy=$1 seed=$2
  local log=$D/hu-${policy}-400-seed${seed}.log
  local t0=$(date +%s.%N)
  node $D/drive-practice.mjs "$policy" 400 "$seed" 300 "$log" \
    --engine-timeout-ms 120000 2> "$log.driver-err"
  local t1=$(date +%s.%N)
  echo "$policy seed$seed wall=$((t1-t0))" | tee -a "$D/regen.timing"
}
run station 401
run folder 402
run passive-random 403
run uniform 404
run srp-passive 405
echo REGEN_DONE >> $D/regen.timing
