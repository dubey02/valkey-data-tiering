#!/bin/bash
# Head-to-head: vanilla valkey AOF-always vs tiered WAL-always.
# 50GB @ 4KB = 13,107,200 keys. No pipelining anywhere. 100 conns total.
# Per arm: populate -> 2M-op 50/50 uniform full-dataset -> tail-marker oracle
#          -> kill -9 -> timed crash boot -> oracle check -> 3x1M-op 50/50
#          (zipfian, gaussian, uniform).
set -u
SRC=/mnt/nvme/valkey-fastboot/src
B=/mnt/nvme/bench/aof_h2h
PORT=7855
CLI="$SRC/valkey-cli -p $PORT"
BM="$SRC/valkey-benchmark -p $PORT"
N=13107200
VS=4096
CSV=$B/results.csv
mkdir -p $B
echo "arm,phase,dist,ops,wall_s,tps,get_rps,set_rps,notes" > $CSV

now() { date +%s.%N; }
row() { echo "$1,$2,$3,$4,$5,$6,$7,$8,$9" >> $CSV; }

boot_wait() {  # $1=t0  -> prints boot seconds
  while :; do
    [ "$($CLI ping 2>/dev/null)" = "PONG" ] && break
    sleep 0.1
  done
  awk -v a="$1" -v b="$(now)" 'BEGIN{printf "%.2f", b-a}'
}

# concurrent 50/50: GET(50c) + SET(50c), each $1 ops, dist flag(s) in $2
mixed() {
  local OPS=$1 DFLAG=$2 TAG=$3
  local t0=$(now)
  $BM -t set -r $N -n $OPS -c 50 --threads 8 -d $VS $DFLAG --csv -q > $B/${TAG}_set.csv 2>/dev/null &
  local SP=$!
  $BM -t get -r $N -n $OPS -c 50 --threads 8 $DFLAG --csv -q > $B/${TAG}_get.csv 2>/dev/null
  wait $SP
  local wall=$(awk -v a="$t0" -v b="$(now)" 'BEGIN{printf "%.2f", b-a}')
  local grps=$(grep -m1 '"GET' $B/${TAG}_get.csv | cut -d, -f2 | tr -d '"'); grps=${grps:-0}
  local srps=$(grep -m1 '"SET' $B/${TAG}_set.csv | cut -d, -f2 | tr -d '"'); srps=${srps:-0}
  local tps=$(awk -v o=$((OPS*2)) -v w=$wall 'BEGIN{printf "%.0f", (w>0)?o/w:0}')
  echo "$wall $tps $grps $srps"
}

tailmark_write() {  # 200 sequential acked 4KB writes right before the kill
  local V=$(head -c $VS /dev/zero | tr '\0' 'x')
  for i in $(seq 1 200); do $CLI set "tailmark:$i" "$V" > /dev/null; done
}
tailmark_check() {  # prints number of survivors
  local ok=0
  for i in $(seq 1 200); do
    [ "$($CLI --no-raw get "tailmark:$i" 2>/dev/null)" != "(nil)" ] && ok=$((ok+1))
  done
  echo $ok
}

misses() { $CLI info stats | awk -F: '/keyspace_misses/{gsub("\r","");print $2}'; }

run_arm() {
  local ARM=$1; shift
  local DIR=$B/$ARM
  # Guard: kill any ghost server still on the port (a prior failed run).
  if [ "$($CLI ping 2>/dev/null)" = "PONG" ]; then
    $CLI shutdown nosave 2>/dev/null
    for i in $(seq 1 100); do [ "$($CLI ping 2>/dev/null)" != "PONG" ] && break; sleep 0.2; done
  fi
  rm -rf $DIR; mkdir -p $DIR
  [ "$ARM" = tiered ] && truncate -s 200G $DIR/flash.db
  local SERVER_ARGS=("$@")

  echo "=== PHASE_START $ARM boot-fresh $(date -u +%T) ==="
  local t0=$(now)
  $SRC/valkey-server --port $PORT --daemonize yes --pidfile $DIR/pid \
      --logfile $DIR/server.log --dir $DIR --save '' "${SERVER_ARGS[@]}"
  local bt=$(boot_wait $t0)
  row $ARM boot-fresh - - $bt - - - -

  echo "=== PHASE_START $ARM populate $(date -u +%T) ==="
  t0=$(now)
  $BM -t set --sequential -r $N -n $N -c 100 --threads 8 -d $VS --csv -q > $B/${ARM}_pop.csv 2>/dev/null
  local pw=$(awk -v a="$t0" -v b="$(now)" 'BEGIN{printf "%.2f", b-a}')
  local prps=$(grep -m1 '"SET' $B/${ARM}_pop.csv | cut -d, -f2 | tr -d '"')
  row $ARM populate sequential $N $pw $prps - $prps "dbsize=$($CLI dbsize)"

  echo "=== PHASE_START $ARM mixed-full $(date -u +%T) ==="
  read w tps g s <<< "$(mixed 1000000 "" ${ARM}_mixfull)"
  row $ARM mixed-full uniform 2000000 $w $tps $g $s -

  echo "=== PHASE_START $ARM tailmark+kill $(date -u +%T) ==="
  tailmark_write
  sleep 0.3
  local PID=$(cat $DIR/pid)
  kill -9 $PID
  while kill -0 $PID 2>/dev/null; do sleep 0.1; done
  local DU=$(du -sh --apparent-size $DIR 2>/dev/null | cut -f1); local DUREAL=$(du -sh $DIR 2>/dev/null | cut -f1)

  echo "=== PHASE_START $ARM crash-boot $(date -u +%T) ==="
  t0=$(now)
  $SRC/valkey-server --port $PORT --daemonize yes --pidfile $DIR/pid \
      --logfile $DIR/server.log --dir $DIR --save '' "${SERVER_ARGS[@]}"
  bt=$(boot_wait $t0)
  local surv=$(tailmark_check)
  row $ARM crash-boot - - $bt - - - "tailmark_survivors=$surv/200;du=$DUREAL;dbsize=$($CLI dbsize)"

  for D in zipfian gaussian uniform; do
    echo "=== PHASE_START $ARM postboot-$D $(date -u +%T) ==="
    local DFLAG="--$D"
    [ "$D" = uniform ] && DFLAG=""   # uniform is the benchmark default; no flag exists
    local m0=$(misses)
    read w tps g s <<< "$(mixed 500000 "$DFLAG" ${ARM}_pb_$D)"
    local m1=$(misses)
    row $ARM postboot $D 1000000 $w $tps $g $s "misses_delta=$((m1-m0))"
  done

  $CLI shutdown nosave 2>/dev/null
  sleep 1
  echo "=== PHASE_DONE $ARM $(date -u +%T) ==="
}

run_arm vanilla --appendonly yes --appendfsync always --maxmemory 0

run_arm tiered --appendonly no --maxmemory 8gb --rdbcompression no \
  --ext-storage-enabled yes --ext-storage-path $B/tiered/flash.db \
  --ext-storage-capacity-mb 81920 \
  --ext-storage-admission-policy flash --ext-storage-promotion-policy never \
  --ext-key-spill-enabled yes --ext-storage-fast-boot yes \
  --ext-storage-wal-enabled yes --ext-storage-wal-fsync always

echo "=== H2H_DONE $(date -u +%T) ==="
