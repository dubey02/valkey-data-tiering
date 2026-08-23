#!/bin/bash
# Fast-boot durability step 9: kill -9 crash matrix.
# Contract under WAL always-mode:
#   (1) every ACKED write is present after crash boot;
#   (2) transactions (execution-unit groups) are all-or-nothing -- a partial
#       group must never be visible, acked or not;
#   (3) acked deletes stay deleted.
# Scenarios: A = steady singleton SETs (3 trials), B = atomic 3-key groups
# via EVAL (3 trials), C = mixed writes+deletes (1 trial). Kill lands at a
# random point under load, so across trials it samples mid-staging,
# mid-WAL-fsync, and mid-checkpoint states. Recovery path exercised:
# checkpoint (64MB cadence) + delta + WAL replay.
set -u
SRC=$(cd "$(dirname "$0")/../src" && pwd)
D=/local/home/xdk/fastboot_bench/crashmatrix
PORT=7875
CLI="$SRC/valkey-cli -p $PORT"
PASS=0; FAIL=0
ok()  { echo "  PASS: $1"; PASS=$((PASS+1)); }
bad() { echo "  FAIL: $1"; FAIL=$((FAIL+1)); }

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

V=$(head -c 2048 /dev/zero | tr '\0' 'z')

# --- writers: record an id to the acked file ONLY after the reply arrives ---
writer_singleton() { # $1=tag  writes s:<tag>:<i>, appends i to acked file
  local i=0
  while :; do
    i=$((i+1))
    [ "$($CLI set "s:$1:$i" "$V-$i" 2>/dev/null)" = "OK" ] || break
    echo $i >> $D/acked_$1.txt
  done
}
writer_group() { # $1=tag  atomic 3-key unit via EVAL
  local i=0
  local SCRIPT='for j=1,3 do redis.call("set", KEYS[j], ARGV[1]) end return 1'
  while :; do
    i=$((i+1))
    r=$($CLI eval "$SCRIPT" 3 "g:$1:$i:1" "g:$1:$i:2" "g:$1:$i:3" "$V-$i" 2>/dev/null)
    [ "$r" = "1" ] || break
    echo $i >> $D/acked_$1.txt
  done
}
writer_mixed() { # $1=tag  set k, set k2, del k -- acked delete must persist
  local i=0
  while :; do
    i=$((i+1))
    [ "$($CLI set "m:$1:$i" "$V-$i" 2>/dev/null)" = "OK" ] || break
    echo "set $i" >> $D/acked_$1.txt
    if [ $((i % 3)) -eq 0 ]; then
      [ "$($CLI del "m:$1:$((i-1))" 2>/dev/null)" = "1" ] || break
      echo "del $((i-1))" >> $D/acked_$1.txt
    fi
  done
}

check_singleton() { # every acked id present with the right value
  local tag=$1 miss=0 n=0
  while read -r i; do
    n=$((n+1))
    local sfx=$($CLI getrange "s:$tag:$i" $((-${#i}-1)) -1)
    [ "$sfx" = "-$i" ] || miss=$((miss+1))
  done < $D/acked_$tag.txt
  [ $miss -eq 0 ] && ok "[$tag] $n/$n acked singleton writes present" \
                  || bad "[$tag] $miss/$n acked writes MISSING"
}
check_group() { # acked groups complete; ANY visible group must be complete
  local tag=$1 miss=0 n=0 partial=0
  while read -r i; do
    n=$((n+1))
    local c=0
    for j in 1 2 3; do [ "$($CLI exists "g:$tag:$i:$j")" = "1" ] && c=$((c+1)); done
    [ $c -eq 3 ] || miss=$((miss+1))
  done < $D/acked_$tag.txt
  # all-or-nothing for every visible id (incl. durable-but-unacked tail)
  local maxi=$(tail -1 $D/acked_$tag.txt)
  for i in $(seq 1 $((maxi + 5))); do
    local c=0
    for j in 1 2 3; do [ "$($CLI exists "g:$tag:$i:$j")" = "1" ] && c=$((c+1)); done
    [ $c -ne 0 ] && [ $c -ne 3 ] && partial=$((partial+1))
  done
  [ $miss -eq 0 ] && ok "[$tag] $n/$n acked groups fully present" \
                  || bad "[$tag] $miss/$n acked groups incomplete"
  [ $partial -eq 0 ] && ok "[$tag] no partial group visible (all-or-nothing)" \
                     || bad "[$tag] $partial PARTIAL groups visible"
}
check_mixed() { # last acked op per key wins
  local tag=$1 wrong=0 n=0
  declare -A want
  while read -r op i; do
    n=$((n+1))
    [ "$op" = set ] && want[$i]=1 || want[$i]=0
  done < $D/acked_$tag.txt
  for i in "${!want[@]}"; do
    local e=$($CLI exists "m:$tag:$i")
    [ "$e" = "${want[$i]}" ] || wrong=$((wrong+1))
  done
  [ $wrong -eq 0 ] && ok "[$tag] $n acked ops, final state exact (deletes honored)" \
                   || bad "[$tag] $wrong keys with wrong final state"
}

run_trial() { # $1=scenario $2=trial-num $3=kill-delay-seconds
  local tag="$1$2"
  rm -f $D/acked_$tag.txt; touch $D/acked_$tag.txt
  writer_$1 $tag &
  local W=$!
  sleep $3
  kill9                      # server dies mid-load
  kill $W 2>/dev/null; wait $W 2>/dev/null
  start_srv
  case $1 in
    singleton) check_singleton $tag ;;
    group)     check_group $tag ;;
    mixed)     check_mixed $tag ;;
  esac
}

rm -rf $D; mkdir -p $D; truncate -s 10G $D/flash.db
start_srv
# base data so recovery has a real checkpoint + delta to work through
$SRC/valkey-benchmark -p $PORT -t set --sequential -r 30000 -n 30000 -c 20 -d 4096 -q >/dev/null 2>&1
sleep 2

echo "=== Scenario A: kill -9 under steady acked singleton writes (3 trials) ==="
run_trial singleton 1 2.0
run_trial singleton 2 3.5
run_trial singleton 3 1.2
echo "=== Scenario B: kill -9 under atomic 3-key groups (3 trials) ==="
run_trial group 1 2.0
run_trial group 2 3.1
run_trial group 3 1.4
echo "=== Scenario C: kill -9 under mixed writes+deletes (1 trial) ==="
run_trial mixed 1 3.0
grep -c 'FC: Delta recovery' $D/server.log > /dev/null && \
  ok "recovery path exercised checkpoint+delta ($(grep -c 'FC: Delta recovery' $D/server.log) crash boots)"
$CLI shutdown nosave 2>/dev/null

echo
echo "RESULT: $PASS passed, $FAIL failed"
[ $FAIL -eq 0 ] && echo "CRASH MATRIX PASSED"
