#!/bin/bash
# Steps 1+2 verification: delete tombstones + head journal (crash-safe fast boot)
# Phase A: populate -> DEL subset -> clean shutdown -> fast boot
#          expect: no resurrection (tombstones applied), survivors intact
# Phase B: add more keys -> kill -9 -> fast boot via HEAD JOURNAL
#          expect: crash recovery, deletes still honored, flushed keys present
# Phase C: kill -9 again immediately -> boot -> idempotent replay
set -u
D=/local/home/xdk/fastboot_bench/durability_test
V=/home/xdk/.meshclaw/workspaces/oss-data-tiering/valkey-data-tiering-policies/src
PORT=7899
CLI="$V/valkey-cli -p $PORT"
LOG=$D/test.log
log(){ echo "[$(date +%H:%M:%S)] $*" | tee -a $LOG; }

rm -rf $D; mkdir -p $D; : > $LOG
truncate -s 2G $D/flash.db

start(){ # $1 = logfile suffix
  $V/valkey-server --port $PORT --daemonize yes --save '' --appendonly no \
    --dir $D --logfile $D/server_$1.log --pidfile $D/valkey.pid \
    --ext-storage-enabled yes --ext-storage-path $D/flash.db \
    --ext-storage-capacity-mb 2048 --ext-storage-fast-boot yes \
    --ext-storage-admission-policy flash --ext-storage-promotion-policy never \
    --maxmemory 4gb --maxmemory-policy noeviction
  for i in $(seq 1 100); do $CLI ping 2>/dev/null | grep -q PONG && return 0; sleep 0.1; done
  log "FATAL: server did not start"; exit 1
}

N=20000
log "=== PHASE A: tombstones across clean shutdown ==="
start a1
# populate: deterministic 512B values
python3 - <<EOF
import socket
s=socket.create_connection(("127.0.0.1",$PORT))
buf=b""
def cmd(*a):
    global buf
    m=("*%d\r\n"%len(a)).encode()
    for x in a: m+=("\$%d\r\n"%len(x)).encode()+x+b"\r\n"
    s.sendall(m)
    r=b""
    while not r.endswith(b"\r\n"): r+=s.recv(65536)
for i in range($N):
    v=(("v%08d"%i)*64)[:512].encode()
    cmd(b"SET",("k:%d"%i).encode(),v)
print("populated")
EOF
sleep 3  # let spill completions drain
DBSZ=$($CLI dbsize); log "after populate: dbsize=$DBSZ (expect $N)"
# delete every 5th key (4000)
python3 - <<EOF
import socket
s=socket.create_connection(("127.0.0.1",$PORT))
def cmd(*a):
    m=("*%d\r\n"%len(a)).encode()
    for x in a: m+=("\$%d\r\n"%len(x)).encode()+x+b"\r\n"
    s.sendall(m); r=b""
    while not r.endswith(b"\r\n"): r+=s.recv(65536)
for i in range(0,$N,5): cmd(b"DEL",("k:%d"%i).encode())
print("deleted")
EOF
sleep 2
DBSZ=$($CLI dbsize); log "after deletes: dbsize=$DBSZ (expect 16000)"
$CLI shutdown nosave 2>/dev/null; sleep 1
while pgrep -f "valkey-server \*:$PORT" >/dev/null; do sleep 0.2; done
log "clean shutdown done; sidecars: $(ls $D | grep -E 'superblock|headj|index' | tr '\n' ' ')"

start a2
DBSZ=$($CLI dbsize)
NIL=0; BAD=0
for i in 0 5 1000 19995; do [ "$($CLI get k:$i)" = "" ] && NIL=$((NIL+1)); done
for i in 1 7 999 19999; do
  EXP=$(python3 -c "print((('v%08d'%$i)*64)[:512])")
  [ "$($CLI get k:$i)" = "$EXP" ] || BAD=$((BAD+1))
done
TOMB=$(grep -o "tombstones_applied=\[[0-9]*\]" $D/server_a2.log | head -1)
MODE=$(grep -o "Recovery complete ([a-z ]*)" $D/server_a2.log | head -1)
log "PHASE A RESULT: dbsize=$DBSZ (expect 16000) deleted_nil=$NIL/4 survivors_ok=$((4-BAD))/4 $MODE $TOMB"

log "=== PHASE B: crash recovery via head journal ==="
# add 20000 more keys (10MB > several 4MB staging flushes)
python3 - <<EOF
import socket
s=socket.create_connection(("127.0.0.1",$PORT))
def cmd(*a):
    m=("*%d\r\n"%len(a)).encode()
    for x in a: m+=("\$%d\r\n"%len(x)).encode()+x+b"\r\n"
    s.sendall(m); r=b""
    while not r.endswith(b"\r\n"): r+=s.recv(65536)
for i in range(20000):
    v=(("w%08d"%i)*64)[:512].encode()
    cmd(b"SET",("k2:%d"%i).encode(),v)
print("populated k2")
EOF
sleep 3
DBSZ=$($CLI dbsize); log "before crash: dbsize=$DBSZ (expect 36000)"
PID=$(cat $D/valkey.pid)
kill -9 $PID; sleep 1
log "killed -9 pid=$PID; superblock present? $(ls $D | grep -c superblock)"

start b1
DBSZ=$($CLI dbsize)
NIL=0; BAD=0
for i in 0 5 1000 19995; do [ "$($CLI get k:$i)" = "" ] && NIL=$((NIL+1)); done
for i in 1 7 999 19999; do
  EXP=$(python3 -c "print((('v%08d'%$i)*64)[:512])")
  [ "$($CLI get k:$i)" = "$EXP" ] || BAD=$((BAD+1))
done
K2OK=0
for i in 0 100 5000 15000; do
  EXP=$(python3 -c "print((('w%08d'%$i)*64)[:512])")
  [ "$($CLI get k2:$i)" = "$EXP" ] && K2OK=$((K2OK+1))
done
MODE=$(grep -o "crash recovery via head journal[^\"]*" $D/server_b1.log | head -1)
TOMB=$(grep -o "tombstones_applied=\[[0-9]*\]" $D/server_b1.log | head -1)
log "PHASE B RESULT: dbsize=$DBSZ (expect ~36000 minus staging loss) deleted_nil=$NIL/4 old_ok=$((4-BAD))/4 k2_sample_ok=$K2OK/4 mode='$MODE' $TOMB"

log "=== PHASE C: double-crash idempotency ==="
PID=$(cat $D/valkey.pid); kill -9 $PID; sleep 1
start c1
DBSZ2=$($CLI dbsize)
MODE=$(grep -o "crash recovery via head journal[^\"]*" $D/server_c1.log | head -1)
log "PHASE C RESULT: dbsize=$DBSZ2 (expect == phase B $DBSZ) mode='$MODE'"
$CLI shutdown nosave 2>/dev/null
log "=== DONE ==="
