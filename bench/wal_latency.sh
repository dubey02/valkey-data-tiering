#!/bin/bash
# Quantify client-ack WAL gating cost: SET latency, no pipelining, 50 conns.
# Arms: wal-off | wal everysec | wal always
set -u
SRC=/home/xdk/.meshclaw/workspaces/oss-data-tiering/valkey-data-tiering-policies/src
D=/local/home/xdk/fastboot_bench/wallat
PORT=7841
CLI="$SRC/valkey-cli -p $PORT"

stop_srv() { $CLI shutdown nosave 2>/dev/null; sleep 0.5; }

run_arm() {
  local NAME="$1"; shift
  rm -rf $D/data; mkdir -p $D/data; truncate -s 2G $D/data/flash.db
  $SRC/valkey-server --port $PORT --daemonize no \
    --save '' --appendonly no --maxmemory 8gb \
    --ext-storage-enabled yes --ext-storage-path $D/data/flash.db \
    --ext-storage-admission-policy flash --ext-storage-promotion-policy never \
    --ext-key-spill-enabled yes --ext-storage-fast-boot yes \
    --rdbcompression no \
    "$@" > $D/server_$NAME.log 2>&1 &
  local SPID=$!
  for i in $(seq 1 50); do $CLI ping 2>/dev/null | grep -q PONG && break; sleep 0.2; done
  # 200k SETs, 512B values, 50 clients, NO pipelining
  $SRC/valkey-benchmark -p $PORT -t set -n 200000 -c 50 -d 512 -P 1 --precision 2 2>/dev/null \
    | tail -6 > $D/bench_$NAME.txt
  echo "== $NAME =="
  cat $D/bench_$NAME.txt
  $CLI info external_storage 2>/dev/null | grep -E 'wal_(fsyncs|gated|last_lsn|durable)' || true
  stop_srv
  kill $SPID 2>/dev/null; wait $SPID 2>/dev/null
}

stop_srv
run_arm off      --ext-storage-wal-enabled no
run_arm everysec --ext-storage-wal-enabled yes --ext-storage-wal-fsync everysec
run_arm always   --ext-storage-wal-enabled yes --ext-storage-wal-fsync always
echo WAL_LATENCY_DONE
