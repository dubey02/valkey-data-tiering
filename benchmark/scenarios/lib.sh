#!/usr/bin/env bash
# Common functions for benchmark scenarios
set -euo pipefail

: "${VALKEY_CLI:=./valkey-cli}"
: "${VALKEY_HOST:=127.0.0.1}"
: "${VALKEY_PORT:=6379}"
: "${RESULTS_DIR:=./results}"

cli() { $VALKEY_CLI -h "$VALKEY_HOST" -p "$VALKEY_PORT" "$@"; }

info_field() { cli INFO "$1" | grep -oP "^$2:\K[0-9.]+"; }

used_memory() { info_field memory used_memory; }
maxmemory() { info_field memory maxmemory; }
tiered_values() { info_field tiering tiered_values 2>/dev/null || echo 0; }
tiered_fetches() { info_field tiering stat_tiered_fetches_requested 2>/dev/null || echo 0; }
tiered_spills() { info_field tiering stat_tiered_spills_ok 2>/dev/null || echo 0; }

wait_for_stable_memory() {
    local target_pct=${1:-5} interval=${2:-1} window=${3:-10}
    local maxmem=$(maxmemory)
    local threshold=$(( maxmem * target_pct / 100 ))
    local stable_count=0

    echo "Waiting for memory to stabilize within ${target_pct}% of maxmemory ($maxmem)..."
    while (( stable_count < window )); do
        local mem=$(used_memory)
        local diff=$(( mem - maxmem ))
        (( diff < 0 )) && diff=$(( -diff ))
        if (( diff <= threshold )); then
            (( stable_count++ ))
        else
            stable_count=0
        fi
        sleep "$interval"
    done
    echo "Memory stable at $(used_memory) bytes"
}

collect_snapshot() {
    local label=$1 outfile=${2:-"$RESULTS_DIR/snapshots.csv"}
    local ts=$(date +%s)
    local mem=$(used_memory)
    local spills=$(tiered_spills)
    local fetches=$(tiered_fetches)
    local tiered=$(tiered_values)
    echo "$ts,$label,$mem,$spills,$fetches,$tiered" >> "$outfile"
}

populate_keys() {
    local count=$1 size=${2:-500}
    echo "Populating $count keys (${size}B values)..."
    for ((i=0; i<count; i+=1000)); do
        local batch=$((count - i < 1000 ? count - i : 1000))
        local pipe=""
        for ((j=i; j<i+batch; j++)); do
            pipe+="SET key:$j $(head -c "$size" /dev/urandom | base64 | head -c "$size")\r\n"
        done
        printf "$pipe" | cli --pipe >/dev/null 2>&1
    done
    echo "Populated $count keys"
}

find_trace() {
    local name="$1"
    local repo_dir="${REPO_DIR:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)}"
    for p in \
        "$repo_dir/utils/benchmark/traces/$name" \
        "$repo_dir/benchmark/traces/$name" \
        "./traces/$name" \
        "$HOME/valkey-tiering/traces/$name"; do
        [ -f "$p" ] && echo "$p" && return
    done
    echo "$name"  # fallback: let trace-replay error
}

run_trace_replay() {
    local tr="${TRACE_REPLAY:-}"
    if [ -z "$tr" ]; then
        local repo_dir="${REPO_DIR:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)}"
        for p in \
            "$repo_dir/benchmark/tools/trace-replay/trace-replay" \
            "$repo_dir/utils/benchmark/trace-replay/trace-replay" \
            "./trace-replay"; do
            [ -x "$p" ] && tr="$p" && break
        done
    fi
    [ -x "$tr" ] || { echo "[error] trace-replay not found"; return 1; }
    "$tr" -host "${VALKEY_HOST:-127.0.0.1}" -port "$VALKEY_PORT" "$@"
}

ensure_results_dir() {
    mkdir -p "$RESULTS_DIR"
}

timestamp() { date +%s; }
