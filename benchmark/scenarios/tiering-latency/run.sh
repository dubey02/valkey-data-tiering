#!/usr/bin/env bash
# tiering-latency — pure storage-layer read latency.
# Populate synthetic keys, DEBUG SPILL them all, wait for drain, then GET each
# at a target TPS. Records p50/p99/p99.9/p100 from client and server (INFO latencystats).
set -euo pipefail
SCENARIO_DIR="$(cd "$(dirname "$0")" && pwd)"
source "$SCENARIO_DIR/../lib.sh"

CONFIG="${1:-$SCENARIO_DIR/configs/idle.env}"
source "$CONFIG"
: "${RESULTS_DIR:=.}"
TRACE_REPLAY="${TRACE_REPLAY:-$SCENARIO_DIR/../../tools/trace-replay/trace-replay}"
PORT="${VALKEY_PORT:-6379}"

echo "[tiering-latency] keys=$KEY_COUNT keysize=${KEY_SIZE}B valsize=${VALUE_SIZE}B rps=$RPS clients=$CLIENTS spill=${SPILL:-yes}"

# Server-side per-command latency tracking with p100 bucket
cli CONFIG SET latency-tracking yes >/dev/null
cli CONFIG SET latency-tracking-info-percentiles "50 99 99.9 100" >/dev/null
cli CONFIG RESETSTAT >/dev/null

# Phase 1(+2+3): populate via regular SET; optionally DEBUG SPILL all + wait for drain.
# SPILL=no leaves keys in RAM (no-tiering control: same traffic, reads served from memory).
if [ "${SPILL:-yes}" = "no" ]; then
    "$TRACE_REPLAY" -host "$VALKEY_HOST" -port "$PORT" \
        -synth-keys "$KEY_COUNT" -synth-size "$VALUE_SIZE" -synth-keysize "$KEY_SIZE" \
        -synth-type "${SYNTH_TYPE:-string}" -synth-items "${SYNTH_ITEMS:-1}" -pop-only
else
    "$TRACE_REPLAY" -host "$VALKEY_HOST" -port "$PORT" \
        -synth-keys "$KEY_COUNT" -synth-size "$VALUE_SIZE" -synth-keysize "$KEY_SIZE" \
        -synth-type "${SYNTH_TYPE:-string}" -synth-items "${SYNTH_ITEMS:-1}" -spill
fi

# Phase 4: paced reads (client-side latency)
"$TRACE_REPLAY" -host "$VALKEY_HOST" -port "$PORT" \
    -synth-keys "$KEY_COUNT" -synth-keysize "$KEY_SIZE" -synth-read \
    -synth-type "${SYNTH_TYPE:-string}" \
    -rps "$RPS" -clients "$CLIENTS" -scenario "tiering-latency" -checkpoint 0 \
    | tee "$RESULTS_DIR/output.txt"

# Phase 5: server-side GET percentiles
SRV=$(cli INFO latencystats | grep -oP '^latency_percentiles_usec_get:\K.*' | tr -d '\r' || true)

echo "" | tee -a "$RESULTS_DIR/output.txt"
echo "  Server-side GET (INFO latencystats, usec): ${SRV:-<none>}" | tee -a "$RESULTS_DIR/output.txt"
