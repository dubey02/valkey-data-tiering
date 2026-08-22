#!/bin/bash
# 100-conn cold-touch arms, reusing existing datasets. Appends to touch_results.csv.
set -u
V=/mnt/nvme/valkey-fastboot/src
PORT=7788
CLI="$V/valkey-cli -p $PORT"
RESULTS=/mnt/nvme/bench/touch_results.csv
CONNS=${CONNS:-100}

log() { echo "[$(date +%H:%M:%S)] $*"; }
now() { date +%s.%N; }
elapsed() { awk "BEGIN{printf \"%.3f\", $2 - $1}"; }

wait_pong() {
  local t0=$(now)
  while true; do
    [ "$($CLI ping 2>/dev/null)" = "PONG" ] && break
    sleep 0.02
  done
  elapsed $t0 $(now)
}

kill_server() { $CLI shutdown nosave >/dev/null 2>&1; while pgrep -f "valkey-server \*:$PORT" >/dev/null; do sleep 0.2; done; }

run_one() { # label N tdir vdir
  local LABEL=$1 N=$2 TDIR=$3 VDIR=$4

  log "=== [$LABEL] TIERED ${CONNS}conn ==="
  kill_server
  [ -f $TDIR/flash.db.index ] || { log "FATAL: no index file in $TDIR"; return 1; }
  $V/valkey-server --port $PORT --daemonize yes --save '' --appendonly no \
    --dir $TDIR --logfile $TDIR/touch_boot100.log --pidfile $TDIR/valkey.pid \
    --ext-storage-enabled yes --ext-storage-path $TDIR/flash.db \
    --ext-storage-capacity-mb 32768 --ext-storage-fast-boot yes \
    --ext-storage-admission-policy flash --ext-storage-promotion-policy never \
    --ext-key-spill-enabled yes --maxmemory 32gb --maxmemory-policy noeviction
  local TB=$(wait_pong)
  $CLI config resetstat >/dev/null
  read TT GOT NILS <<< $(python3 /mnt/nvme/bench/touch.py $PORT $N $CONNS)
  local MISS=$($CLI info stats | grep "^keyspace_misses" | tr -d $'\r' | cut -d: -f2)
  log "[$LABEL] tiered: boot=${TB}s touch_${CONNS}conn=${TT}s got=$GOT nils=$NILS misses=$MISS tps=$(awk "BEGIN{printf \"%.0f\", $N/$TT}")"
  echo "$LABEL,tiered,${CONNS}conn,$N,$TB,$TT,$MISS" >> $RESULTS
  kill_server

  log "=== [$LABEL] VANILLA ${CONNS}conn ==="
  python3 -c "
import os
fd = os.open('$VDIR/dump.rdb', os.O_RDONLY)
os.posix_fadvise(fd, 0, 0, os.POSIX_FADV_DONTNEED)
os.close(fd)"
  $V/valkey-server --port $PORT --daemonize yes --save '' --appendonly no \
    --dir $VDIR --logfile $VDIR/touch_boot100.log --pidfile $VDIR/valkey.pid --maxmemory 0
  local VB=$(wait_pong)
  $CLI config resetstat >/dev/null
  read VT VGOT VNILS <<< $(python3 /mnt/nvme/bench/touch.py $PORT $N $CONNS)
  local VMISS=$($CLI info stats | grep "^keyspace_misses" | tr -d $'\r' | cut -d: -f2)
  log "[$LABEL] vanilla: boot=${VB}s touch_${CONNS}conn=${VT}s got=$VGOT nils=$VNILS misses=$VMISS tps=$(awk "BEGIN{printf \"%.0f\", $N/$VT}")"
  echo "$LABEL,vanilla,${CONNS}conn,$N,$VB,$VT,$VMISS" >> $RESULTS
  kill_server
  log "[$LABEL] DONE"
}

run_one v512 2000000 /mnt/nvme/bench/v512ix-tiered /mnt/nvme/bench/v512-vanilla-cold
run_one v4k 1000000 /mnt/nvme/bench/v4kix-tiered /mnt/nvme/bench/v4k-vanilla-cold
run_one v16k 250000 /mnt/nvme/bench/v16kix-tiered /mnt/nvme/bench/v16k-vanilla-cold
run_one v64k 320000 /mnt/nvme/bench/v64kix-tiered /mnt/nvme/bench/v64k-vanilla-cold
echo TOUCH_100_DONE
