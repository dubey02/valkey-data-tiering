#!/bin/bash
# Fast-boot durability steps 3+8 verification.
# T1: acked-write loss after kill -9 (the 43/200 class) -> replay recovers all
# T2: double kill -9 -> idempotent
# T3: tombstone in WAL tail -> delete survives crash
# T4: checkpoint + delta crash boot -> no full log scan
# T5: retirement -> WAL truncated + replay evidence consumed after checkpoint
set -u
SRC=$(cd "$(dirname "$0")/../src" && pwd)
D=/local/home/xdk/fastboot_bench/step38
PORT=7873
CLI="$SRC/valkey-cli -p $PORT"
PASS=0; FAIL=0
ok()  { echo "  PASS: $1"; PASS=$((PASS+1)); }
bad() { echo "  FAIL: $1"; FAIL=$((FAIL+1)); }

start_srv() { # extra args passed through
  $SRC/valkey-server --port $PORT --daemonize yes --pidfile $D/pid \
    --logfile $D/server.log --dir $D --save '' --appendonly no \
    --maxmemory 8gb --rdbcompression no \
    --ext-storage-enabled yes --ext-storage-path $D/flash.db \
    --ext-storage-capacity-mb 8192 \
    --ext-storage-admission-policy flash --ext-storage-promotion-policy never \
    --ext-key-spill-enabled yes --ext-storage-fast-boot yes \
    --ext-storage-wal-enabled yes --ext-storage-wal-fsync always "$@"
  for i in $(seq 1 300); do [ "$($CLI ping 2>/dev/null)" = PONG ] && return; sleep 0.1; done
  echo "SERVER FAILED TO BOOT"; exit 1
}
kill9() { local P=$(cat $D/pid); kill -9 $P; while kill -0 $P 2>/dev/null; do sleep 0.05; done; }
clean_stop() { $CLI shutdown nosave 2>/dev/null; sleep 0.7; }

rm -rf $D; mkdir -p $D; truncate -s 10G $D/flash.db

echo "=== T1: acked writes survive kill -9 (WAL boot replay) ==="
start_srv
# base data that spills
$SRC/valkey-benchmark -p $PORT -t set --sequential -r 5000 -n 5000 -c 20 -d 4096 -q >/dev/null 2>&1
# 300 sequential acked writes, then immediate kill: staging tail unflushed
V=$(head -c 4096 /dev/zero | tr '\0' 'y')
for i in $(seq 1 300); do $CLI set "tail:$i" "$V" >/dev/null; done
kill9
start_srv
SURV=0; for i in $(seq 1 300); do [ "$($CLI strlen tail:$i)" = "4096" ] && SURV=$((SURV+1)); done
[ $SURV -eq 300 ] && ok "300/300 acked writes present after crash (was the 43/200 loss class)" \
                  || bad "only $SURV/300 acked writes survived"
grep -q 'boot replay applied' $D/server.log && ok "boot replay engaged" || bad "no boot replay in log"

echo "=== T2: double kill -9 idempotent ==="
kill9
start_srv
SURV=0; for i in $(seq 1 300); do [ "$($CLI strlen tail:$i)" = "4096" ] && SURV=$((SURV+1)); done
[ $SURV -eq 300 ] && ok "300/300 after second crash (replay evidence retained + re-replayed)" \
                  || bad "only $SURV/300 after second crash"

echo "=== T3: tombstone in WAL tail survives crash ==="
$CLI set delme "$V" >/dev/null
$CLI del delme >/dev/null
$CLI set keepme "$V" >/dev/null
kill9
start_srv
[ "$($CLI --no-raw get delme)" = "(nil)" ] && ok "deleted key stays deleted after crash" || bad "deleted key resurrected"
[ "$($CLI strlen keepme)" = "4096" ] && ok "post-delete write survived" || bad "keepme lost"
clean_stop

echo "=== T4: checkpoint + delta crash boot (no full scan) ==="
rm -rf $D; mkdir -p $D; truncate -s 10G $D/flash.db
start_srv --ext-storage-checkpoint-mb 64
# ~400MB of data -> several 64MB checkpoints
$SRC/valkey-benchmark -p $PORT -t set --sequential -r 100000 -n 100000 -c 50 -d 4096 -q >/dev/null 2>&1
sleep 2  # let io-thread idle path take a checkpoint
# small post-checkpoint delta
$SRC/valkey-benchmark -p $PORT -t set -r 100000 -n 3000 -c 20 -d 4096 -q >/dev/null 2>&1
for i in $(seq 1 50); do $CLI set "t4tail:$i" "$V" >/dev/null; done
grep -q 'Checkpoint: index serialized' $D/server.log && ok "runtime checkpoint taken" || bad "no runtime checkpoint"
kill9
T0=$(date +%s.%N)
start_srv --ext-storage-checkpoint-mb 64
BOOT=$(awk -v a=$T0 -v b=$(date +%s.%N) 'BEGIN{printf "%.2f", b-a}')
grep -q 'Delta recovery' $D/server.log && ok "crash boot used checkpoint+delta (boot ${BOOT}s)" || bad "no delta recovery in log"
grep -q 'crash boot from checkpoint' $D/server.log && ok "index restored without superblock" || bad "checkpoint index not used"
S=0; for i in $(seq 1 50); do [ "$($CLI strlen t4tail:$i)" = "4096" ] && S=$((S+1)); done
[ $S -eq 50 ] && ok "50/50 tail-marker writes after checkpoint+delta+replay boot" || bad "only $S/50 tail markers"
# spot-check base data via client path
M=0; for k in 5 4999 50000 99999; do L=$($CLI strlen "key:$(printf %012d $k)"); [ "$L" = "4096" ] || M=$((M+1)); done
[ $M -eq 0 ] && ok "base data spot-check clean" || bad "$M/4 base keys wrong"

echo "=== T5: retirement truncates WAL after checkpoint ==="
# force pressure so replayed keys respill, then let cron fire (wal-max tiny)
$CLI config set ext-storage-wal-max-mb 16 >/dev/null
$SRC/valkey-benchmark -p $PORT -t set -r 100000 -n 8000 -c 20 -d 4096 -q >/dev/null 2>&1
sleep 15
WB=$($CLI info external_storage | tr -d '\r' | awk -F: '/wal_active_bytes/{print $2}')
TR=$($CLI info external_storage | tr -d '\r' | awk -F: '/wal_truncations/{print $2}')
DK=$($CLI info external_storage | tr -d '\r' | awk -F: '/wal_dirty_keys/{print $2}')
if [ "${TR:-0}" -ge 1 ]; then ok "WAL truncated (truncations=$TR active_bytes=$WB dirty=$DK)"
else bad "no truncation fired (active_bytes=$WB dirty=$DK)"; fi
ls $D/flash.db.wal.replay >/dev/null 2>&1 && bad "replay evidence not consumed" || ok "replay evidence consumed"
clean_stop

echo
echo "RESULT: $PASS passed, $FAIL failed"
[ $FAIL -eq 0 ] && echo "ALL STEP 3+8 TESTS PASSED"
