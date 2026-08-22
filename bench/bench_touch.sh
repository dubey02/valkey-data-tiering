#!/bin/bash
# Cold start INCLUDING full keyspace touch: boot -> PONG -> GET every key.
# Usage: bench_touch.sh <label> <num_keys> <tiered_dir> <vanilla_dir>
set -u
LABEL=$1; N=$2; TDIR=$3; VDIR=$4
V=/mnt/nvme/valkey-fastboot/src
PORT=7788
CLI="$V/valkey-cli -p $PORT"
RESULTS=/mnt/nvme/bench/touch_results.csv

log() { echo "[$(date +%H:%M:%S)] $*"; }
now() { date +%s.%N; }
elapsed() { awk "BEGIN{printf \"%.3f\", $2 - $1}"; }

# Pre-generate GET command streams (inline protocol), reused across arms.
CMDS=/mnt/nvme/bench/getcmds_$N.txt
if [ ! -f $CMDS ]; then
  python3 -c "
import sys
with open('$CMDS','w') as f:
    for i in range(1, $N+1): f.write('GET key:%d\r\n' % i)"
fi
NSHARD=8
if [ ! -f $CMDS.shard0 ]; then
  python3 -c "
n=$N; s=$NSHARD
fs=[open('$CMDS.shard%d'%k,'w') for k in range(s)]
per=(n+s-1)//s
for k in range(s):
    lo, hi = k*per+1, min((k+1)*per, n)
    for i in range(lo, hi+1): fs[k].write('GET key:%d\r\n' % i)
for f in fs: f.close()"
fi

wait_pong() {
  local t0=$(now)
  while true; do
    [ "$($CLI ping 2>/dev/null)" = "PONG" ] && break
    sleep 0.02
  done
  elapsed $t0 $(now)
}

kill_server() { $CLI shutdown nosave >/dev/null 2>&1; while pgrep -f "valkey-server \*:$PORT" >/dev/null; do sleep 0.2; done; }

run_touch() { # -> "elapsed replies nils"
  python3 /mnt/nvme/bench/touch.py $PORT $N 1
}

run_touch_8c() { # -> "elapsed replies nils"
  python3 /mnt/nvme/bench/touch.py $PORT $N 8
}

SKIP_TIERED=${SKIP_TIERED:-0}
if [ "$SKIP_TIERED" != "1" ]; then
############ TIERED (index reflection) ############
log "=== [$LABEL] TIERED cold start + touch ==="
kill_server
[ -f $TDIR/flash.db.index ] || { log "FATAL: no index file in $TDIR"; exit 1; }
$V/valkey-server --port $PORT --daemonize yes --save '' --appendonly no \
  --dir $TDIR --logfile $TDIR/touch_boot.log --pidfile $TDIR/valkey.pid \
  --ext-storage-enabled yes --ext-storage-path $TDIR/flash.db \
  --ext-storage-capacity-mb 32768 --ext-storage-fast-boot yes \
  --ext-storage-admission-policy flash --ext-storage-promotion-policy never \
  --ext-key-spill-enabled yes --maxmemory 32gb --maxmemory-policy noeviction
T_BOOT=$(wait_pong)
$CLI config resetstat >/dev/null
read T1C GOT NILS <<< $(run_touch); MISS=$($CLI info stats | grep "^keyspace_misses" | tr -d $'\r' | cut -d: -f2); ERR=$NILS
log "[$LABEL] tiered: boot=${T_BOOT}s touch_1conn=${T1C}s errors=$ERR misses=$MISS tps=$(awk "BEGIN{printf \"%.0f\", $N/$T1C}")"
echo "$LABEL,tiered,1conn,$N,$T_BOOT,$T1C,$MISS" >> $RESULTS
# 8-connection variant (fresh boot so every key is cold again)
kill_server
$V/valkey-server --port $PORT --daemonize yes --save '' --appendonly no \
  --dir $TDIR --logfile $TDIR/touch_boot8.log --pidfile $TDIR/valkey.pid \
  --ext-storage-enabled yes --ext-storage-path $TDIR/flash.db \
  --ext-storage-capacity-mb 32768 --ext-storage-fast-boot yes \
  --ext-storage-admission-policy flash --ext-storage-promotion-policy never \
  --ext-key-spill-enabled yes --maxmemory 32gb --maxmemory-policy noeviction
T_BOOT8=$(wait_pong)
$CLI config resetstat >/dev/null
read T8C GOT8 NILS8 <<< $(run_touch_8c)
MISS8=$($CLI info stats | grep "^keyspace_misses" | tr -d '\r' | cut -d: -f2)
log "[$LABEL] tiered: boot=${T_BOOT8}s touch_8conn=${T8C}s misses=$MISS8 tps=$(awk "BEGIN{printf \"%.0f\", $N/$T8C}")"
echo "$LABEL,tiered,8conn,$N,$T_BOOT8,$T8C,$MISS8" >> $RESULTS
kill_server

fi
############ VANILLA ############
log "=== [$LABEL] VANILLA cold start + touch ==="
python3 -c "
import os
fd = os.open('$VDIR/dump.rdb', os.O_RDONLY)
os.posix_fadvise(fd, 0, 0, os.POSIX_FADV_DONTNEED)
os.close(fd)"
$V/valkey-server --port $PORT --daemonize yes --save '' --appendonly no \
  --dir $VDIR --logfile $VDIR/touch_boot.log --pidfile $VDIR/valkey.pid --maxmemory 0
VT_BOOT=$(wait_pong)
$CLI config resetstat >/dev/null
read VT1C VGOT VNILS <<< $(run_touch); VMISS=$($CLI info stats | grep "^keyspace_misses" | tr -d $'\r' | cut -d: -f2); VERR=$VNILS
log "[$LABEL] vanilla: boot=${VT_BOOT}s touch_1conn=${VT1C}s errors=$VERR misses=$VMISS tps=$(awk "BEGIN{printf \"%.0f\", $N/$VT1C}")"
echo "$LABEL,vanilla,1conn,$N,$VT_BOOT,$VT1C,$VMISS" >> $RESULTS
# 8-conn variant (no reboot needed for vanilla — DRAM state identical — but
# reboot anyway for symmetry of the cold-boot total)
kill_server
python3 -c "
import os
fd = os.open('$VDIR/dump.rdb', os.O_RDONLY)
os.posix_fadvise(fd, 0, 0, os.POSIX_FADV_DONTNEED)
os.close(fd)"
$V/valkey-server --port $PORT --daemonize yes --save '' --appendonly no \
  --dir $VDIR --logfile $VDIR/touch_boot8.log --pidfile $VDIR/valkey.pid --maxmemory 0
VT_BOOT8=$(wait_pong)
$CLI config resetstat >/dev/null
read VT8C VGOT8 VNILS8 <<< $(run_touch_8c)
VMISS8=$($CLI info stats | grep "^keyspace_misses" | tr -d '\r' | cut -d: -f2)
log "[$LABEL] vanilla: boot=${VT_BOOT8}s touch_8conn=${VT8C}s misses=$VMISS8 tps=$(awk "BEGIN{printf \"%.0f\", $N/$VT8C}")"
echo "$LABEL,vanilla,8conn,$N,$VT_BOOT8,$VT8C,$VMISS8" >> $RESULTS
kill_server
log "[$LABEL] TOUCH DONE"
