#!/usr/bin/env bash
# mixed-size: Heterogeneous value-size classes on one server.
# Tests head-of-line blocking and mixed spill economics.
#
# MIX_CLASSES = space-separated "type:value_size:keysize:weight" specs
# Each class gets prefix c<i>: for key isolation.
# Reports per-class latency in client-latency.csv.
set -euo pipefail
SCEN_DIR="$(cd "$(dirname "$0")" && pwd)"
source "$SCEN_DIR/../lib.sh"

CONFIG="${1:-$SCEN_DIR/configs/default.env}"
source "$CONFIG"

: "${MIX_CLASSES:=string:512:100:70 set:4096:100:30}"
: "${OPS:=100000}"
: "${CLIENTS:=50}"
: "${READ_PCT:=80}"
: "${ACCESS_PATTERN:=zipfian}"
: "${MAXMEMORY:=0}"
: "${MAXMEMORY_POLICY:=noeviction}"
: "${KEYSPACE:=50000}"
: "${ZIPFIAN_ALPHA:=1.0}"
: "${ITEMS_PER_KEY:=10}"
: "${POPULATE_CLIENTS:=20}"

BENCH="${BENCHMARK:-$(command -v valkey-benchmark || echo ./valkey-benchmark)}"
RESULTS="${RESULTS_DIR:-.}"
mkdir -p "$RESULTS"
TMPDIR=$(mktemp -d)
trap "rm -rf $TMPDIR" EXIT

ZIPF_FLAG=""
[ "$ACCESS_PATTERN" = "zipfian" ] && ZIPF_FLAG="--zipfian $ZIPFIAN_ALPHA"

echo "[mixed-size] classes='$MIX_CLASSES' ops=$OPS clients=$CLIENTS read=${READ_PCT}% access=$ACCESS_PATTERN maxmemory=$MAXMEMORY"

cli CONFIG SET maxmemory "$MAXMEMORY" >/dev/null
cli CONFIG SET maxmemory-policy "$MAXMEMORY_POLICY" >/dev/null

# Parse size spec: supports plain integers or suffixes like 16mb, 4kb
parse_size() {
    local s="${1,,}"
    if [[ "$s" =~ ^([0-9]+)mb$ ]]; then echo $(( ${BASH_REMATCH[1]} * 1048576 ))
    elif [[ "$s" =~ ^([0-9]+)kb$ ]]; then echo $(( ${BASH_REMATCH[1]} * 1024 ))
    else echo "$s"
    fi
}

# ═══════════════════════════════════════════════════════════════════════════════
# PHASE 1: POPULATE
# ═══════════════════════════════════════════════════════════════════════════════
CLASS_IDX=0
declare -a CLASS_TYPES CLASS_SIZES CLASS_KEYSIZES CLASS_WEIGHTS CLASS_PREFIXES CLASS_LABELS
for spec in $MIX_CLASSES; do
    IFS=: read -r ctype csize ckeysize cweight <<< "$spec"
    CLASS_TYPES+=("$ctype")
    CLASS_SIZES+=("$(parse_size "$csize")")
    CLASS_KEYSIZES+=("$ckeysize")
    CLASS_WEIGHTS+=("$cweight")
    CLASS_PREFIXES+=("c${CLASS_IDX}")
    CLASS_LABELS+=("${ctype}-${csize}")
    CLASS_IDX=$((CLASS_IDX + 1))
done
NUM_CLASSES=${#CLASS_TYPES[@]}

for ((ci=0; ci<NUM_CLASSES; ci++)); do
    local_type="${CLASS_TYPES[$ci]}"
    local_size="${CLASS_SIZES[$ci]}"
    local_keysize="${CLASS_KEYSIZES[$ci]}"
    local_prefix="${CLASS_PREFIXES[$ci]}"
    local_label="${CLASS_LABELS[$ci]}"

    PAYLOAD=$(head -c "$local_size" /dev/urandom | base64 -w0 | tr -d '\n' | head -c "$local_size")

    echo "[mixed-size] Populating class $ci ($local_label): prefix=$local_prefix keyspace=$KEYSPACE"

    if [ "$local_type" = "string" ]; then
        # String: use arbitrary-command mode with namespace prefix
        generate_string_pop() {
            local start=$1 end=$2
            for ((k=start; k<end; k++)); do
                printf 'SET %s:%012d %s\r\n' "$local_prefix" "$k" "$PAYLOAD"
            done
        }
        KEYS_PER_WORKER=$(( (KEYSPACE + POPULATE_CLIENTS - 1) / POPULATE_CLIENTS ))
        PIDS=()
        for ((w=0; w<POPULATE_CLIENTS; w++)); do
            START=$((w * KEYS_PER_WORKER))
            END=$(( START + KEYS_PER_WORKER ))
            (( END > KEYSPACE )) && END=$KEYSPACE
            (( START >= KEYSPACE )) && break
            generate_string_pop "$START" "$END" | cli --pipe > "$TMPDIR/pop_${ci}_${w}.txt" 2>&1 &
            PIDS+=($!)
        done
        for pid in "${PIDS[@]}"; do wait "$pid" || true; done
    else
        # Compound types
        generate_compound_pop() {
            local start=$1 end=$2
            local item_size=$(( local_size / ITEMS_PER_KEY ))
            [ "$item_size" -lt 1 ] && item_size=1
            local item_payload=$(head -c "$item_size" /dev/urandom | base64 -w0 | tr -d '\n' | head -c "$item_size")
            for ((k=start; k<end; k++)); do
                local padkey=$(printf '%012d' "$k")
                case "$local_type" in
                    hash)
                        for ((i=0; i<ITEMS_PER_KEY; i++)); do
                            printf 'HSET %s:%s f:%d %s\r\n' "$local_prefix" "$padkey" "$i" "$item_payload"
                        done ;;
                    list)
                        for ((i=0; i<ITEMS_PER_KEY; i++)); do
                            printf 'RPUSH %s:%s %s\r\n' "$local_prefix" "$padkey" "$item_payload"
                        done ;;
                    set)
                        for ((i=0; i<ITEMS_PER_KEY; i++)); do
                            printf 'SADD %s:%s elem:%d:%s\r\n' "$local_prefix" "$padkey" "$i" "$item_payload"
                        done ;;
                    zset)
                        for ((i=0; i<ITEMS_PER_KEY; i++)); do
                            printf 'ZADD %s:%s %d elem:%d:%s\r\n' "$local_prefix" "$padkey" "$i" "$i" "$item_payload"
                        done ;;
                    stream)
                        for ((i=0; i<ITEMS_PER_KEY; i++)); do
                            printf 'XADD %s:%s * f %s\r\n' "$local_prefix" "$padkey" "$item_payload"
                        done ;;
                    *) echo "[mixed-size] ERROR: unknown type '$local_type'" >&2; exit 1 ;;
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
            generate_compound_pop "$START" "$END" | cli --pipe > "$TMPDIR/pop_${ci}_${w}.txt" 2>&1 &
            PIDS+=($!)
        done
        for pid in "${PIDS[@]}"; do wait "$pid" || true; done
    fi
done

DBSIZE=$(cli DBSIZE | grep -oP '[0-9]+')
echo "[mixed-size] Population complete. DBSIZE=$DBSIZE"

# ═══════════════════════════════════════════════════════════════════════════════
# PHASE 2: CONCURRENT WORKLOAD (per-class, weighted)
# ═══════════════════════════════════════════════════════════════════════════════
echo "[mixed-size] Running workload..."
WRITE_PCT=$((100 - READ_PCT))
WORKLOAD_PIDS=()

for ((ci=0; ci<NUM_CLASSES; ci++)); do
    local_type="${CLASS_TYPES[$ci]}"
    local_size="${CLASS_SIZES[$ci]}"
    local_keysize="${CLASS_KEYSIZES[$ci]}"
    local_weight="${CLASS_WEIGHTS[$ci]}"
    local_prefix="${CLASS_PREFIXES[$ci]}"
    local_label="${CLASS_LABELS[$ci]}"

    CLASS_OPS=$((OPS * local_weight / 100))
    CLASS_CLIENTS=$((CLIENTS * local_weight / 100))
    [ "$CLASS_CLIENTS" -lt 1 ] && CLASS_CLIENTS=1
    READ_OPS=$((CLASS_OPS * READ_PCT / 100))
    WRITE_OPS=$((CLASS_OPS - READ_OPS))
    READ_CL=$((CLASS_CLIENTS * READ_PCT / 100))
    [ "$READ_CL" -lt 1 ] && READ_CL=1
    WRITE_CL=$((CLASS_CLIENTS - READ_CL))
    [ "$WRITE_CL" -lt 1 ] && WRITE_CL=1

    # Build per-type read/write commands using the namespaced prefix
    # NOTE: --keysize is not used here; keys are embedded in the command string via prefix.
    if [ "$local_type" = "string" ]; then
        READ_CMD="GET ${local_prefix}:__rand_int__"
        WRITE_PAYLOAD=$(head -c "$local_size" /dev/urandom | base64 -w0 | tr -d '\n' | head -c "$local_size")
        WRITE_CMD="SET ${local_prefix}:__rand_int__ $WRITE_PAYLOAD"
    else
        item_size=$(( local_size / ITEMS_PER_KEY ))
        [ "$item_size" -lt 1 ] && item_size=1
        ITEM_PAYLOAD=$(head -c "$item_size" /dev/urandom | base64 -w0 | tr -d '\n' | head -c "$item_size")
        case "$local_type" in
            hash)
                READ_CMD="HGETALL ${local_prefix}:__rand_int__"
                WRITE_CMD="HSET ${local_prefix}:__rand_int__ f:0 $ITEM_PAYLOAD" ;;
            list)
                READ_CMD="LRANGE ${local_prefix}:__rand_int__ 0 -1"
                WRITE_CMD="LSET ${local_prefix}:__rand_int__ 0 $ITEM_PAYLOAD" ;;
            set)
                READ_CMD="SMEMBERS ${local_prefix}:__rand_int__"
                WRITE_CMD="SADD ${local_prefix}:__rand_int__ elem:0:$ITEM_PAYLOAD" ;;
            zset)
                READ_CMD="ZRANGE ${local_prefix}:__rand_int__ 0 -1 WITHSCORES"
                WRITE_CMD="ZADD ${local_prefix}:__rand_int__ 999 elem:0:$ITEM_PAYLOAD" ;;
            stream)
                READ_CMD="XRANGE ${local_prefix}:__rand_int__ - + COUNT $ITEMS_PER_KEY"
                WRITE_CMD="XADD ${local_prefix}:__rand_int__ * f $ITEM_PAYLOAD" ;;
        esac
    fi

    set -f
    if [ "$READ_OPS" -gt 0 ]; then
        "$BENCH" -h "$VALKEY_HOST" -p "$VALKEY_PORT" \
            -n "$READ_OPS" -r "$KEYSPACE" -c "$READ_CL" \
            $ZIPF_FLAG --csv \
            $READ_CMD > "$TMPDIR/class_${ci}_read.csv" 2>&1 &
        WORKLOAD_PIDS+=($!)
    fi
    if [ "$WRITE_OPS" -gt 0 ]; then
        "$BENCH" -h "$VALKEY_HOST" -p "$VALKEY_PORT" \
            -n "$WRITE_OPS" -r "$KEYSPACE" -c "$WRITE_CL" \
            $ZIPF_FLAG --csv \
            $WRITE_CMD > "$TMPDIR/class_${ci}_write.csv" 2>&1 &
        WORKLOAD_PIDS+=($!)
    fi
    set +f
done

for pid in "${WORKLOAD_PIDS[@]}"; do wait "$pid" || true; done

# ═══════════════════════════════════════════════════════════════════════════════
# PHASE 3: RESULTS
# ═══════════════════════════════════════════════════════════════════════════════
{
echo "command,rps,p50_ms,p95_ms,p99_ms,p100_ms"
for ((ci=0; ci<NUM_CLASSES; ci++)); do
    local_label="${CLASS_LABELS[$ci]}"
    for rw in read write; do
        f="$TMPDIR/class_${ci}_${rw}.csv"
        [ -f "$f" ] || continue
        LINE=$(tail -1 "$f")
        [ -z "$LINE" ] && continue
        CMD=$(echo "$LINE" | cut -d',' -f1 | tr -d '"' | awk '{print $1}')
        RPS=$(echo "$LINE" | cut -d',' -f2 | tr -d '"')
        P50=$(echo "$LINE" | cut -d',' -f5 | tr -d '"')
        P95=$(echo "$LINE" | cut -d',' -f6 | tr -d '"')
        P99=$(echo "$LINE" | cut -d',' -f7 | tr -d '"')
        P100=$(echo "$LINE" | cut -d',' -f8 | tr -d '"')
        echo "${local_label}-${rw},$RPS,$P50,$P95,$P99,$P100"
    done
done
} > "$RESULTS/client-latency.csv"

# Human-readable output
{
for ((ci=0; ci<NUM_CLASSES; ci++)); do
    local_label="${CLASS_LABELS[$ci]}"
    for rw in read write; do
        f="$TMPDIR/class_${ci}_${rw}.csv"
        [ -f "$f" ] || continue
        LINE=$(tail -1 "$f")
        [ -z "$LINE" ] && continue
        CMD=$(echo "$LINE" | cut -d',' -f1 | tr -d '"')
        RPS=$(echo "$LINE" | cut -d',' -f2 | tr -d '"')
        P50=$(echo "$LINE" | cut -d',' -f5 | tr -d '"')
        echo "${local_label}-${rw} ($CMD): $RPS rps, p50=$P50 ms"
    done
done
} > "$RESULTS/output.txt"

cat "$RESULTS/output.txt"
