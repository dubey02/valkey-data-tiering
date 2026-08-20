#!/usr/bin/env bash
# mixed-rw: Mixed read/write over a fixed keyspace.
# Unified driver: DATATYPE=string uses valkey-benchmark; compound types use --pipe populate + per-type commands.
#
# Access pattern is config-driven via ACCESS_PATTERN:
#   uniform  -> keys chosen uniformly at random
#   zipfian  -> keys chosen by Zipfian rank (--zipfian ZIPFIAN_ALPHA)
#
# Always-populate model: full populate phase, then measured read/write.
set -euo pipefail
SCEN_DIR="$(cd "$(dirname "$0")" && pwd)"
source "$SCEN_DIR/../lib.sh"

CONFIG="${1:-$SCEN_DIR/configs/uniform.env}"
source "$CONFIG"

: "${DATATYPE:=string}"
: "${ACCESS_PATTERN:=uniform}"
: "${READ_PCT:=80}"
: "${KEYSPACE:=500000}"
# Value-size: ITEM_SIZE is canonical; fall back to DATASIZE for back-compat with old string configs
: "${ITEM_SIZE:=${DATASIZE:-400}}"
: "${ITEMS_PER_KEY:=10}"
: "${OPS:=1000000}"
: "${DURATION:=}"  # If set, use --duration instead of -n (time-based workload)
: "${CLIENTS:=200}"
: "${MAXMEMORY:=0}"
: "${MAXMEMORY_POLICY:=noeviction}"
: "${ZIPFIAN_ALPHA:=1.0}"
: "${SCAN_PCT:=0}"
: "${POPULATE_BATCH:=100}"
: "${POPULATE_CLIENTS:=50}"
: "${PIPELINE:=1}"
: "${TTL:=0}"  # Key TTL in seconds (0 = no expiry)

# DATASET_BYTES: if set, derive KEYSPACE so total value bytes ≈ DATASET_BYTES
if [ -n "${DATASET_BYTES:-}" ] && [ "$DATASET_BYTES" -gt 0 ] 2>/dev/null; then
    if [ "$DATATYPE" = "string" ]; then
        _value_total=$ITEM_SIZE
    else
        _value_total=$(( ITEMS_PER_KEY * ITEM_SIZE ))
    fi
    KEYSPACE=$(( DATASET_BYTES / _value_total ))
    (( KEYSPACE < 1 )) && KEYSPACE=1
    echo "[mixed-rw] DATASET_BYTES=$DATASET_BYTES value_total=$_value_total → derived KEYSPACE=$KEYSPACE"
fi

: "${KEYSIZE:=64}"
if [ -n "${MAXMEMORY_MB:-}" ] && [ "${MAXMEMORY_MB:-0}" -gt 0 ] 2>/dev/null; then
    : "${PER_KEY_BYTES:=$((KEYSIZE + 64))}"
    : "${VALKEY_OVERHEAD_MB:=20}"
    MAXMEMORY=$(( MAXMEMORY_MB * 1048576 ))
    _overhead=$(( VALKEY_OVERHEAD_MB * 1048576 ))
    if [ "$DATATYPE" = "string" ]; then _value_total=$ITEM_SIZE
    else _value_total=$(( ITEMS_PER_KEY * ITEM_SIZE )); fi
    _denom100=$(( PER_KEY_BYTES * 100 + HOT_PCT * _value_total ))
    KEYSPACE=$(( (MAXMEMORY - _overhead) * 100 / _denom100 ))
    (( KEYSPACE < 1 )) && KEYSPACE=1
    echo "[mixed-rw] MAXMEMORY_MB=$MAXMEMORY_MB (const) -> KEYSPACE=$KEYSPACE maxmemory=$MAXMEMORY"
elif [ -n "${HOT_PCT:-}" ] && [ "${HOT_PCT:-0}" -gt 0 ] 2>/dev/null; then
    : "${PER_KEY_BYTES:=$((KEYSIZE + 64))}"
    : "${VALKEY_OVERHEAD_MB:=20}"
    _key_floor=$(( KEYSPACE * PER_KEY_BYTES ))
    _overhead=$(( VALKEY_OVERHEAD_MB * 1048576 ))
    if [ -n "${DATASET_BYTES:-}" ] && [ "$DATASET_BYTES" -gt 0 ] 2>/dev/null; then
        _hot=$(( DATASET_BYTES * HOT_PCT / 100 ))
    else
        if [ "$DATATYPE" = "string" ]; then
            _hot=$(( KEYSPACE * ITEM_SIZE * HOT_PCT / 100 ))
        else
            _hot=$(( KEYSPACE * ITEMS_PER_KEY * ITEM_SIZE * HOT_PCT / 100 ))
        fi
    fi
    MAXMEMORY=$(( _key_floor + _overhead + _hot ))
    echo "[mixed-rw] HOT_PCT=$HOT_PCT key_floor=$_key_floor overhead=$_overhead hot=$_hot -> derived MAXMEMORY=$MAXMEMORY bytes"
fi

# Post-derivation override: allows configs to use MAXMEMORY_MB for keyspace derivation
# but run with maxmemory=0 (baseline, no eviction).  Set MAXMEMORY_OVERRIDE=0 in the env.
if [ -n "${MAXMEMORY_OVERRIDE+set}" ]; then
    MAXMEMORY=$MAXMEMORY_OVERRIDE
    echo "[mixed-rw] MAXMEMORY_OVERRIDE -> maxmemory=$MAXMEMORY (keyspace unchanged)"
fi

WRITE_PCT=$((100 - READ_PCT))
READ_CLIENTS=$((CLIENTS * READ_PCT / 100))
WRITE_CLIENTS=$((CLIENTS - READ_CLIENTS))

# Run-length flag: --duration N or -n OPS
if [ -n "${DURATION:-}" ] && [ "$DURATION" -gt 0 ] 2>/dev/null; then
    RUN_FLAG="--duration $DURATION"
    READ_RUN_FLAG="$RUN_FLAG"
    WRITE_RUN_FLAG="$RUN_FLAG"
    echo "[mixed-rw] Duration mode: ${DURATION}s per workload phase"
else
    READ_OPS=$((OPS * READ_PCT / 100))
    WRITE_OPS=$((OPS - READ_OPS))
    READ_RUN_FLAG="-n $READ_OPS"
    WRITE_RUN_FLAG="-n $WRITE_OPS"
fi

BENCH="${BENCHMARK:-$(command -v valkey-benchmark || echo ./valkey-benchmark)}"
RESULTS="${RESULTS_DIR:-.}"
mkdir -p "$RESULTS"
TMPDIR=$(mktemp -d)
trap "rm -rf $TMPDIR" EXIT

KS_FLAG=""
[ -n "${KEYSIZE:-}" ] && [ "${KEYSIZE:-0}" -gt 0 ] 2>/dev/null && KS_FLAG="--keysize $KEYSIZE"

ZIPF_FLAG=""
if [ "$ACCESS_PATTERN" = "zipfian" ]; then
    ZIPF_FLAG="--zipfian $ZIPFIAN_ALPHA"
elif [ "$ACCESS_PATTERN" != "uniform" ]; then
    echo "[mixed-rw] ERROR: ACCESS_PATTERN must be 'uniform' or 'zipfian' (got '$ACCESS_PATTERN')" >&2
    exit 1
fi

echo "[mixed-rw] type=$DATATYPE access=$ACCESS_PATTERN read=${READ_PCT}% keyspace=$KEYSPACE clients=$CLIENTS maxmemory=$MAXMEMORY $([ -n "${DURATION:-}" ] && echo "duration=${DURATION}s" || echo "ops=$OPS")"

cli CONFIG SET maxmemory "$MAXMEMORY" >/dev/null
cli CONFIG SET maxmemory-policy "$MAXMEMORY_POLICY" >/dev/null

# TTL: when > 0, use arbitrary command form "SET key value EX ttl" instead of -t set
SET_CMD="-t set"
SET_CMD_ARGS="-d $ITEM_SIZE"
SET_CMD_TAIL=""
if [ "${TTL:-0}" -gt 0 ] 2>/dev/null; then
    SET_CMD="-d $ITEM_SIZE"
    SET_CMD_ARGS=""
    SET_CMD_TAIL="-- SET __rand_int__ __data__ EX $TTL"
    echo "[mixed-rw] TTL=$TTL seconds on all SET commands"
fi

# ═══════════════════════════════════════════════════════════════════════════════
# STRING BRANCH
# ═══════════════════════════════════════════════════════════════════════════════
if [ "$DATATYPE" = "string" ]; then
    echo "[mixed-rw] Populating $KEYSPACE keys (sequential, OOM-resilient)..."
    # Keys accounted in the keyspace: dict-resident plus key-spilled. With
    # key spilling enabled, demoted keys leave the dict but remain on flash,
    # so DBSIZE alone undercounts a fully populated keyspace.
    effective_keys() {
        local db ks
        db=$(cli DBSIZE | grep -oP '[0-9]+')
        ks=$(cli INFO everything 2>/dev/null | grep -oP '^keys_key_spilled:\K[0-9]+' | head -1)
        echo $(( ${db:-0} + ${ks:-0} ))
    }
    for attempt in $(seq 1 100); do
        "$BENCH" -h "$VALKEY_HOST" -p "$VALKEY_PORT" \
            $SET_CMD -n "$KEYSPACE" -r "$KEYSPACE" $SET_CMD_ARGS $KS_FLAG \
            --sequential -c "$POPULATE_CLIENTS" -q $SET_CMD_TAIL >> "$RESULTS/populate.txt" 2>&1 || true
        sleep 2
        DBSIZE=$(effective_keys)
        if [ "$DBSIZE" -ge "$KEYSPACE" ]; then
            echo "[mixed-rw] Populate complete: keys=$DBSIZE (attempt $attempt)"
            break
        fi
        echo "[mixed-rw] Populate attempt $attempt: keys=$DBSIZE/$KEYSPACE — retrying..."
    done

    # ── Workload: parallel GET + SET ──
    echo "[mixed-rw] Running workload: GET + SET ($ACCESS_PATTERN, $CLIENTS clients)..."
    "$BENCH" -h "$VALKEY_HOST" -p "$VALKEY_PORT" \
        -t get $READ_RUN_FLAG -r "$KEYSPACE" $KS_FLAG \
        -c "$READ_CLIENTS" -P "$PIPELINE" $ZIPF_FLAG --csv > "$RESULTS/get_output.txt" 2>&1 &
    PID_GET=$!
    "$BENCH" -h "$VALKEY_HOST" -p "$VALKEY_PORT" \
        $SET_CMD $WRITE_RUN_FLAG -r "$KEYSPACE" $SET_CMD_ARGS $KS_FLAG \
        -c "$WRITE_CLIENTS" -P "$PIPELINE" $ZIPF_FLAG --csv $SET_CMD_TAIL > "$RESULTS/set_output.txt" 2>&1 &
    PID_SET=$!

    # ── SCAN disruption (§5) ──
    PID_SCAN=""
    if [ "$SCAN_PCT" -gt 0 ] 2>/dev/null; then
        SCAN_CLIENTS=$((CLIENTS * SCAN_PCT / 100))
        [ "$SCAN_CLIENTS" -lt 1 ] && SCAN_CLIENTS=1
        if [ -n "${DURATION:-}" ] && [ "$DURATION" -gt 0 ] 2>/dev/null; then
            SCAN_RUN_FLAG="--duration $DURATION"
        else
            SCAN_OPS=$((OPS * SCAN_PCT / 100))
            SCAN_RUN_FLAG="-n $SCAN_OPS"
        fi
        echo "[mixed-rw] SCAN disruptor: $SCAN_CLIENTS clients"
        "$BENCH" -h "$VALKEY_HOST" -p "$VALKEY_PORT" \
            -t get $SCAN_RUN_FLAG -r "$KEYSPACE" $KS_FLAG \
            --sequential -c "$SCAN_CLIENTS" --csv > "$RESULTS/scan_output.txt" 2>&1 &
        PID_SCAN=$!
    fi

    wait $PID_GET $PID_SET
    [ -n "$PID_SCAN" ] && wait $PID_SCAN

    cat "$RESULTS/get_output.txt" "$RESULTS/set_output.txt" > "$RESULTS/output.txt"
    cat "$RESULTS/output.txt"

# ═══════════════════════════════════════════════════════════════════════════════
# COMPOUND BRANCH (hash, list, set, zset, stream)
# ═══════════════════════════════════════════════════════════════════════════════
else
    echo "[mixed-rw] Compound: items_per_key=$ITEMS_PER_KEY item_size=$ITEM_SIZE"

    # Generate a fixed payload of ITEM_SIZE bytes (no newlines)
    PAYLOAD=$(head -c "$ITEM_SIZE" /dev/urandom | base64 -w0 | tr -d '\n' | head -c "$ITEM_SIZE")

    # ── Populate via --pipe ──
    echo "[mixed-rw] Populating $KEYSPACE keys x $ITEMS_PER_KEY items ($DATATYPE)..."

    generate_populate_commands() {
        local start=$1 end=$2
        for ((k=start; k<end; k++)); do
            local padkey=$(printf '%012d' "$k")
            case "$DATATYPE" in
                hash)
                    for ((i=0; i<ITEMS_PER_KEY; i++)); do
                        printf 'HSET key:%s f:%d %s\r\n' "$padkey" "$i" "$PAYLOAD"
                    done
                    ;;
                list)
                    for ((i=0; i<ITEMS_PER_KEY; i++)); do
                        printf 'RPUSH key:%s %s\r\n' "$padkey" "$PAYLOAD"
                    done
                    ;;
                set)
                    for ((i=0; i<ITEMS_PER_KEY; i++)); do
                        printf 'SADD key:%s elem:%d:%s\r\n' "$padkey" "$i" "$PAYLOAD"
                    done
                    ;;
                zset)
                    for ((i=0; i<ITEMS_PER_KEY; i++)); do
                        printf 'ZADD key:%s %d elem:%d:%s\r\n' "$padkey" "$i" "$i" "$PAYLOAD"
                    done
                    ;;
                stream)
                    for ((i=0; i<ITEMS_PER_KEY; i++)); do
                        printf 'XADD key:%s * f %s\r\n' "$padkey" "$PAYLOAD"
                    done
                    ;;
                *) echo "[mixed-rw] ERROR: unknown DATATYPE '$DATATYPE'" >&2; exit 1 ;;
            esac
        done
    }

    KEYS_PER_WORKER=$(( (KEYSPACE + POPULATE_CLIENTS - 1) / POPULATE_CLIENTS ))
    PIDS=()
    for ((w=0; w<POPULATE_CLIENTS; w++)); do
        START=$((w * KEYS_PER_WORKER))
        END=$(( START + KEYS_PER_WORKER ))
        (( END > KEYSPACE )) && END=$KEYSPACE
        (( START >= KEYSPACE )) && break
        generate_populate_commands "$START" "$END" | \
            cli --pipe > "$TMPDIR/pop_${w}.txt" 2>&1 &
        PIDS+=($!)
    done
    for pid in "${PIDS[@]}"; do wait "$pid" || true; done

    POP_ERRORS=$(grep -hoEi 'errors: [0-9]+' "$TMPDIR"/pop_*.txt 2>/dev/null | grep -oE '[0-9]+' | awk '{s+=$1} END{print s+0}' || true)
    POP_ERRORS=${POP_ERRORS:-0}
    # Count dict-resident plus key-spilled keys: with key spilling enabled,
    # demoted keys leave the dict but remain on flash.
    DBSIZE=$(cli DBSIZE | grep -oP '[0-9]+')
    KEYSPILLED=$(cli INFO everything 2>/dev/null | grep -oP '^keys_key_spilled:\K[0-9]+' | head -1)
    DBSIZE=$(( ${DBSIZE:-0} + ${KEYSPILLED:-0} ))
    OOM_REJECTS=$(cli INFO everything 2>/dev/null | grep -oP 'oom_reject_write_count:\K[0-9]+' || echo 0)
    OOM_REJECTS=${OOM_REJECTS:-0}
    echo "[mixed-rw] Population: keys=$DBSIZE/$KEYSPACE pipe_errors=$POP_ERRORS oom_rejects=$OOM_REJECTS"

    if [ "$POP_ERRORS" -gt 0 ] || [ "$OOM_REJECTS" -gt 0 ] || [ "$DBSIZE" -lt "$KEYSPACE" ]; then
        echo "[mixed-rw] POPULATE ABORTED (OOM hard cap) — skipping workload."
        echo "POPULATE_ABORTED: datatype=$DATATYPE keys=$DBSIZE/$KEYSPACE oom_rejects=$OOM_REJECTS pipe_errors=$POP_ERRORS" \
            > "$RESULTS/output.txt"
        cat "$RESULTS/output.txt"
        exit 0
    fi
    echo "[mixed-rw] Population complete. DBSIZE=$DBSIZE"

    # ── Workload ──
    echo "[mixed-rw] Running workload..."
    ACCESS_FLAG=""
    [ "$ACCESS_PATTERN" = "zipfian" ] && ACCESS_FLAG="--zipfian $ZIPFIAN_ALPHA"

    case "$DATATYPE" in
        hash)
            READ_CMD="HGETALL key:__rand_int__"
            WRITE_CMD="HSET key:__rand_int__ f:0 $PAYLOAD"
            ;;
        list)
            READ_CMD="LRANGE key:__rand_int__ 0 -1"
            WRITE_CMD="LSET key:__rand_int__ 0 $PAYLOAD"
            ;;
        set)
            READ_CMD="SMEMBERS key:__rand_int__"
            WRITE_CMD="SADD key:__rand_int__ elem:0:$PAYLOAD"
            ;;
        zset)
            READ_CMD="ZRANGE key:__rand_int__ 0 -1 WITHSCORES"
            WRITE_CMD="ZADD key:__rand_int__ 999 elem:0:$PAYLOAD"
            ;;
        stream)
            READ_CMD="XRANGE key:__rand_int__ - + COUNT $ITEMS_PER_KEY"
            WRITE_CMD="XADD key:__rand_int__ * f $PAYLOAD"
            ;;
    esac

    set -f
    "$BENCH" -h "$VALKEY_HOST" -p "$VALKEY_PORT" \
        $READ_RUN_FLAG -r "$KEYSPACE" -c "$READ_CLIENTS" -P "$PIPELINE" \
        $ACCESS_FLAG --csv \
        $READ_CMD > "$TMPDIR/read.csv" 2>&1 &
    PID_READ=$!

    "$BENCH" -h "$VALKEY_HOST" -p "$VALKEY_PORT" \
        $WRITE_RUN_FLAG -r "$KEYSPACE" -c "$WRITE_CLIENTS" -P "$PIPELINE" \
        $ACCESS_FLAG --csv \
        $WRITE_CMD > "$TMPDIR/write.csv" 2>&1 &
    PID_WRITE=$!

    # ── SCAN disruption (§5) ──
    PID_SCAN=""
    if [ "$SCAN_PCT" -gt 0 ] 2>/dev/null; then
        SCAN_CLIENTS=$((CLIENTS * SCAN_PCT / 100))
        [ "$SCAN_CLIENTS" -lt 1 ] && SCAN_CLIENTS=1
        if [ -n "${DURATION:-}" ] && [ "$DURATION" -gt 0 ] 2>/dev/null; then
            SCAN_RUN_FLAG="--duration $DURATION"
        else
            SCAN_OPS=$((OPS * SCAN_PCT / 100))
            SCAN_RUN_FLAG="-n $SCAN_OPS"
        fi
        echo "[mixed-rw] SCAN disruptor: $SCAN_CLIENTS clients"
        "$BENCH" -h "$VALKEY_HOST" -p "$VALKEY_PORT" \
            -t get $SCAN_RUN_FLAG -r "$KEYSPACE" \
            --sequential -c "$SCAN_CLIENTS" --csv > "$TMPDIR/scan.csv" 2>&1 &
        PID_SCAN=$!
    fi
    set +f

    wait $PID_READ $PID_WRITE
    [ -n "$PID_SCAN" ] && wait $PID_SCAN

    # ── Results ──
    {
    for f in "$TMPDIR/read.csv" "$TMPDIR/write.csv"; do
        [ -f "$f" ] || continue
        LINE=$(tail -1 "$f")
        CMD=$(echo "$LINE" | cut -d',' -f1 | tr -d '"')
        RPS=$(echo "$LINE" | cut -d',' -f2 | tr -d '"')
        P50=$(echo "$LINE" | cut -d',' -f5 | tr -d '"')
        echo "$CMD: $RPS requests per second, p50=$P50 msec"
    done
    } > "$RESULTS/output.txt"

    cat "$RESULTS/output.txt"

    # Write client latency CSV
    {
    echo "command,rps,p50_ms,p95_ms,p99_ms,p100_ms"
    for f in "$TMPDIR/read.csv" "$TMPDIR/write.csv"; do
        [ -f "$f" ] || continue
        LINE=$(tail -1 "$f")
        CMD=$(echo "$LINE" | cut -d',' -f1 | tr -d '"' | awk '{print $1}')
        RPS=$(echo "$LINE" | cut -d',' -f2 | tr -d '"')
        P50=$(echo "$LINE" | cut -d',' -f5 | tr -d '"')
        P95=$(echo "$LINE" | cut -d',' -f6 | tr -d '"')
        P99=$(echo "$LINE" | cut -d',' -f7 | tr -d '"')
        P100=$(echo "$LINE" | cut -d',' -f8 | tr -d '"')
        echo "$CMD,$RPS,$P50,$P95,$P99,$P100"
    done
    } > "$RESULTS/client-latency.csv"
fi
