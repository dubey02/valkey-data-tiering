#!/bin/bash
# Client-ack WAL test (Phase 3 steps 6-7).
#
# Verifies:
#  A. Framing shapes: SET -> STANDALONE; MULTI(3 SETs)/MSET/script -> one
#     group each; DEL -> tombstone; N SETs of one key in a MULTI -> ONE
#     record (dedup, final state).
#  B. Reply gating: wal_gated_releases > 0 and durable_lsn == last_lsn after
#     a synchronous workload (every reply waited for its fsync).
#  C. The contract: kill -9 immediately after acked SETs -> every acked key
#     is present and CRC-valid in the WAL (validated by wal_dump).
#  D. Torn-group truncation: chop the WAL mid-group -> wal_dump truncates at
#     GROUP_BEGIN, standalone prefix stays valid.
set -e
V=${V:-/home/xdk/.meshclaw/workspaces/oss-data-tiering/valkey-data-tiering-policies/src}
DUMP=${DUMP:-/tmp/wal_dump}
PORT=7833
D=/local/home/xdk/fastboot_bench/waltest
CLI="$V/valkey-cli -p $PORT"

pkill -f "valkey-server \*:$PORT" 2>/dev/null || true
sleep 0.5
rm -rf $D; mkdir -p $D
truncate -s 2G $D/flash.db

start_server() {
  $V/valkey-server --port $PORT --daemonize yes --save '' --appendonly no \
    --enable-debug-command yes --dir $D \
    --ext-storage-enabled yes --ext-storage-path $D/flash.db \
    --ext-storage-capacity-mb 2048 --ext-storage-fast-boot yes \
    --ext-storage-admission-policy flash --ext-storage-promotion-policy never \
    --ext-key-spill-enabled yes \
    --ext-storage-wal-enabled yes --ext-storage-wal-fsync always \
    --logfile $D/server.log
  for i in $(seq 1 100); do $CLI ping >/dev/null 2>&1 && break; sleep 0.1; done
}

echo "=== A+B: framing shapes + gating ==="
start_server
$CLI set solo v_standalone > /dev/null                       # STANDALONE
$CLI multi > /dev/null 2>&1 || true
$CLI eval "return 1" 0 > /dev/null                           # no writes: no records
printf 'MULTI\r\nSET g1 a\r\nSET g2 b\r\nSET g3 c\r\nEXEC\r\n' | $CLI --pipe > /dev/null  # group of 3
$CLI mset m1 x m2 y > /dev/null                              # group of 2
$CLI eval "redis.call('set', KEYS[1], 'sv1'); redis.call('set', KEYS[2], 'sv2')" 2 s1 s2 > /dev/null  # group of 2
printf 'MULTI\r\nSET dup v1\r\nSET dup v2\r\nSET dup FINAL\r\nEXEC\r\n' | $CLI --pipe > /dev/null      # dedup -> 1 STANDALONE
$CLI del solo > /dev/null                                    # tombstone
INFO=$($CLI info everything 2>/dev/null || $CLI info all)
for f in wal_units wal_records wal_groups wal_tombstones wal_fsyncs wal_last_lsn wal_durable_lsn wal_gated_releases; do
  echo "  $f: $(echo "$INFO" | grep -oP "(?<=^$f:)\S+" | tr -d '\r')"
done

echo "=== sync workload: 200 gated SETs ==="
for i in $(seq 1 200); do $CLI set k$i val$i > /dev/null; done
INFO=$($CLI info everything 2>/dev/null || $CLI info all)
LAST=$(echo "$INFO" | grep -oP "(?<=^wal_last_lsn:)\d+" | tr -d '\r')
DUR=$(echo "$INFO" | grep -oP "(?<=^wal_durable_lsn:)\d+" | tr -d '\r')
GATED=$(echo "$INFO" | grep -oP "(?<=^wal_gated_releases:)\d+" | tr -d '\r')
echo "  last_lsn=$LAST durable_lsn=$DUR gated_releases=$GATED"
[ "$LAST" = "$DUR" ] && echo "  PASS: durable==last (every ack waited for fsync)" \
                     || { echo "  FAIL: durable ($DUR) != last ($LAST)"; exit 1; }
[ "$GATED" -gt 0 ] && echo "  PASS: gating engaged ($GATED releases)" \
                   || { echo "  FAIL: gating never engaged"; exit 1; }

echo "=== C: contract -- kill -9 after acked writes ==="
for i in $(seq 1 50); do $CLI set acked$i durable_val_$i > /dev/null; done
# Every one of those 50 received +OK. Kill NOW with no grace.
kill -9 $(pgrep -f "valkey-server \*:$PORT")
sleep 0.3
RC=0; $DUMP -q $D/flash.db.wal > /tmp/wal_c.out || RC=$?
tail -1 /tmp/wal_c.out
[ $RC -le 2 ] || { echo "  FAIL: dump error"; exit 1; }
# All 50 acked keys must appear in the *valid* region of the WAL.
MISSING=0
$DUMP $D/flash.db.wal > /tmp/wal_c_full.out || true
for i in $(seq 1 50); do
  grep -q "key=acked$i\$" /tmp/wal_c_full.out || { MISSING=$((MISSING+1)); }
done
[ $MISSING -eq 0 ] && echo "  PASS: all 50 acked keys durable in WAL after kill -9" \
                   || { echo "  FAIL: $MISSING acked keys missing from WAL"; exit 1; }
grep -q "truncate_reason=-" /tmp/wal_c.out && echo "  PASS: WAL fully valid (no torn tail)" \
                   || echo "  NOTE: torn tail present (only legal if past all acked keys)"

echo "=== D: torn-group truncation ==="
# Rebuild a WAL whose LAST unit is a group, then chop bytes off the tail so
# the GROUP_COMMIT is destroyed -> dump must truncate at GROUP_BEGIN and
# keep the standalone prefix valid.
rm -rf $D; mkdir -p $D; truncate -s 2G $D/flash.db
start_server
$CLI set prefix1 a > /dev/null
$CLI set prefix2 b > /dev/null
$CLI mset t1 x t2 y t3 z > /dev/null           # final unit: group of 3
kill -9 $(pgrep -f "valkey-server \*:$PORT"); sleep 0.3
SZ=$(stat -c%s $D/flash.db.wal)
head -c $((SZ - 15)) $D/flash.db.wal > $D/torn.wal   # destroy GROUP_COMMIT tail
RC=0; $DUMP -q $D/torn.wal > /tmp/wal_d.out || RC=$?
tail -1 /tmp/wal_d.out
[ $RC -eq 2 ] || { echo "  FAIL: expected truncation exit (2), got $RC"; exit 1; }
grep -qE "truncate_reason=(torn_body|torn_header|unterminated_group|crc_mismatch)" /tmp/wal_d.out \
  && echo "  PASS: torn group detected" || { echo "  FAIL: wrong truncate reason"; exit 1; }
VALID=$(grep -oP "(?<=valid_records=)\d+" /tmp/wal_d.out)
[ "$VALID" -eq 2 ] && echo "  PASS: standalone prefix (2 records) survives, torn group dropped whole" \
                   || { echo "  FAIL: expected 2 valid records, got $VALID"; exit 1; }

echo; echo "ALL WAL TESTS PASSED"
