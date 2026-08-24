#!/bin/bash
# Slim repro loop for the wedged-fetch P1. Cycles: EVAL-group load -> kill -9
# -> crash boot -> probe every group key with a timeout. On a wedge, STOPS
# with the server alive for gdb autopsy and prints the wedged key.
set -u
SRC=$(cd "$(dirname "$0")/../src" && pwd)
D=/local/home/xdk/fastboot_bench/wedge
PORT=7876
CLI="$SRC/valkey-cli -p $PORT"
V=$(head -c 2048 /dev/zero | tr '\0' 'z')
SCRIPT='for j=1,3 do redis.call("set", KEYS[j], ARGV[1]) end return 1'

start_srv() {
  $SRC/valkey-server --port $PORT --daemonize yes --pidfile $D/pid \
    --logfile $D/server.log --dir $D --save '' --appendonly no \
    --maxmemory 8gb --rdbcompression no \
    --ext-storage-enabled yes --ext-storage-path $D/flash.db \
    --ext-storage-capacity-mb 8192 --ext-storage-checkpoint-mb 64 \
    --ext-storage-admission-policy flash --ext-storage-promotion-policy never \
    --ext-key-spill-enabled yes --ext-storage-fast-boot yes \
    --ext-storage-wal-enabled yes --ext-storage-wal-fsync always
  for i in $(seq 1 600); do [ "$($CLI ping 2>/dev/null)" = PONG ] && return; sleep 0.1; done
  echo "BOOT TIMEOUT"; exit 1
}
kill9() { local P=$(cat $D/pid); kill -9 $P; while kill -0 $P 2>/dev/null; do sleep 0.05; done; }

rm -rf $D; mkdir -p $D; truncate -s 10G $D/flash.db
start_srv
$SRC/valkey-benchmark -p $PORT -t set --sequential -r 30000 -n 30000 -c 20 -d 4096 -q >/dev/null 2>&1
sleep 2

for CYCLE in $(seq 1 20); do
  echo "=== cycle $CYCLE $(date -u +%T) ==="
  ( i=0
    while :; do
      i=$((i+1))
      r=$($CLI eval "$SCRIPT" 3 "g:c$CYCLE:$i:1" "g:c$CYCLE:$i:2" "g:c$CYCLE:$i:3" "$V-$i" 2>/dev/null)
      [ "$r" = "1" ] || break
      echo $i > $D/last_acked
    done ) &
  W=$!
  sleep 2.5
  kill9
  kill $W 2>/dev/null; wait $W 2>/dev/null
  start_srv
  LAST=$(cat $D/last_acked)
  echo "  cycle $CYCLE: last acked id=$LAST; probing..."
  for i in $(seq 1 $((LAST + 3))); do
    for j in 1 2 3; do
      K="g:c$CYCLE:$i:$j"
      timeout 3 $CLI exists "$K" > /dev/null 2>&1
      if [ $? -eq 124 ]; then
        echo "WEDGED: $K (cycle $CYCLE, last_acked=$LAST)"
        echo "$K" > $D/wedged_key
        echo "SERVER LEFT ALIVE (pid $(cat $D/pid)) FOR AUTOPSY"
        exit 42
      fi
    done
  done
  echo "  cycle $CYCLE clean"
done
echo "NO WEDGE IN 20 CYCLES"
$CLI shutdown nosave 2>/dev/null
