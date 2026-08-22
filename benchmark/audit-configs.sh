#!/usr/bin/env bash
# audit-configs.sh — run every benchmark config on the remote host with cost caps
# and classify PASS / FAIL / TIMEOUT.
#
# Not a perf measurement: OPS/DURATION/KEY_COUNT are capped so each config is
# exercised end-to-end (parse -> server start -> populate -> workload -> report)
# in bounded time. Perf numbers from an audit run are meaningless.
#
# Usage: ./audit-configs.sh [--timeout SECS] [--tag NAME] [CONFIG_PATH ...]
set -uo pipefail
DIR="$(cd "$(dirname "$0")" && pwd)"
REMOTE_DIR=/tmp/valkey-bench
PER_CONFIG_TIMEOUT=600
TAG="audit-$(date +%Y%m%d-%H%M%S)"
FULL=false

while [[ "${1:-}" == --* ]]; do
    case "$1" in
        --timeout) PER_CONFIG_TIMEOUT="$2"; shift 2 ;;
        --tag) TAG="$2"; shift 2 ;;
        --full) FULL=true; shift ;;
        *) echo "Unknown flag: $1"; exit 1 ;;
    esac
done

source "$DIR/benchmark.env"
EC2_KEYPATH="${EC2_KEYPATH/#\~/$HOME}"
SSH_OPTS="-o StrictHostKeyChecking=no -o ConnectTimeout=10 -o ServerAliveInterval=30 -i $EC2_KEYPATH"
SSH="ssh $SSH_OPTS $EC2_USER@$EC2_HOST"
SCP="scp -q $SSH_OPTS"

# Cost caps applied to the deployed copy of each config
CAP_OPS=200000
CAP_DURATION=10
CAP_KEY_COUNT=100000

# Config list: all of them, or the ones named on the command line
if [ $# -gt 0 ]; then
    CONFIGS=("$@")
else
    mapfile -t CONFIGS < <(cd "$DIR" && ls scenarios/*/configs/*.env)
fi

OUT="$DIR/results/$TAG"
mkdir -p "$OUT"
CSV="$OUT/audit.csv"
echo "scenario,config,verdict,seconds,detail" > "$CSV"

echo "→ Deploying harness + binaries to $EC2_USER@$EC2_HOST:$REMOTE_DIR"
$SSH "pkill -9 valkey-server 2>/dev/null; pkill -9 valkey-benchmark 2>/dev/null; \
      rm -rf $REMOTE_DIR/scenarios; \
      mkdir -p $REMOTE_DIR/tools/metrics-collector $REMOTE_DIR/tools/generate-report $REMOTE_DIR/results"
for b in valkey-server valkey-cli valkey-benchmark; do
    $SCP "$DIR/../src/$b" "$EC2_USER@$EC2_HOST:$REMOTE_DIR/$b"
done
$SCP "$DIR/tools/trace-replay/trace-replay"                       "$EC2_USER@$EC2_HOST:$REMOTE_DIR/tools/trace-replay"
$SCP "$DIR/tools/metrics-collector/metrics-collector.sh"          "$EC2_USER@$EC2_HOST:$REMOTE_DIR/tools/metrics-collector/"
$SCP "$DIR/tools/generate-report/generate-report.py"              "$EC2_USER@$EC2_HOST:$REMOTE_DIR/tools/generate-report/"
$SCP "$DIR/benchmark.sh"                                          "$EC2_USER@$EC2_HOST:$REMOTE_DIR/benchmark.sh"
$SSH "mkdir -p $REMOTE_DIR/scenarios"
$SCP "$DIR/scenarios/lib.sh" "$EC2_USER@$EC2_HOST:$REMOTE_DIR/scenarios/lib.sh"
for S in mixed-rw mixed-size tiering-latency; do
    $SSH "mkdir -p $REMOTE_DIR/scenarios/$S/configs"
    $SCP "$DIR/scenarios/$S/run.sh" "$EC2_USER@$EC2_HOST:$REMOTE_DIR/scenarios/$S/run.sh"
done
$SSH "chmod +x $REMOTE_DIR/benchmark.sh $REMOTE_DIR/valkey-* $REMOTE_DIR/tools/trace-replay \
      $REMOTE_DIR/tools/metrics-collector/metrics-collector.sh $REMOTE_DIR/scenarios/*/run.sh 2>/dev/null; true"
echo "→ Deployed."
echo ""

for CFG_PATH in "${CONFIGS[@]}"; do
    SCEN="$(basename "$(dirname "$(dirname "$CFG_PATH")")")"
    CFG="$(basename "$CFG_PATH" .env)"
    LOCAL_CFG="$DIR/$CFG_PATH"

    # Apply cost caps to a scratch copy, then deploy that. --full deploys the config
    # unmodified, which is what you want for graphable time series: the capped runs are
    # too short to produce enough metric ticks to read.
    TMP_CFG="$(mktemp)"
    if [ "$FULL" = true ]; then
        cp "$LOCAL_CFG" "$TMP_CFG"
    else
        sed -E -e "s/^OPS=.*/OPS=$CAP_OPS/" \
               -e "s/^DURATION=.*/DURATION=$CAP_DURATION/" \
               -e "s/^KEY_COUNT=[0-9]{5,}.*/KEY_COUNT=$CAP_KEY_COUNT/" \
               "$LOCAL_CFG" > "$TMP_CFG"
    fi
    $SCP "$TMP_CFG" "$EC2_USER@$EC2_HOST:$REMOTE_DIR/scenarios/$SCEN/configs/$CFG.env"
    rm -f "$TMP_CFG"

    printf '%-16s %-26s ' "$SCEN" "$CFG"
    LOG="$OUT/$SCEN--$CFG.log"

    # A local `timeout` only kills the ssh client, not the remote benchmark.sh. An orphaned
    # run's cleanup step issues SHUTDOWN NOSAVE on port 6399 and will kill the *next* config's
    # server a few seconds after it starts. Clear the host before every config.
    $SSH "pkill -9 -f 'benchma[r]k.sh' 2>/dev/null; pkill -9 valkey-server 2>/dev/null; \
          pkill -9 valkey-benchmark 2>/dev/null; pkill -9 -f 'trace-repla[y]' 2>/dev/null; true" >/dev/null 2>&1

    START=$(date +%s)
    timeout "$PER_CONFIG_TIMEOUT" $SSH \
        "cd $REMOTE_DIR && ./benchmark.sh --tag $TAG --config $CFG $SCEN" > "$LOG" 2>&1
    RC=$?
    ELAPSED=$(( $(date +%s) - START ))

    if [ $RC -eq 124 ]; then
        $SSH "pkill -9 -f 'benchma[r]k.sh' 2>/dev/null; pkill -9 valkey-server 2>/dev/null; \
              pkill -9 valkey-benchmark 2>/dev/null; true" >/dev/null 2>&1
    fi

    # Classify
    DETAIL=""
    if [ $RC -eq 124 ]; then
        VERDICT=TIMEOUT
        DETAIL="exceeded ${PER_CONFIG_TIMEOUT}s"
    elif grep -q 'command not found' "$LOG"; then
        VERDICT=FAIL
        DETAIL="config parse error: $(grep -m1 'command not found' "$LOG" | sed 's/.*line/line/')"
    elif grep -q 'Server failed to start' "$LOG"; then
        VERDICT=FAIL
        DETAIL="server failed to start"
    elif grep -q 'Config not found' "$LOG"; then
        VERDICT=FAIL
        DETAIL="config not found on remote"
    elif grep -q 'POPULATE_ABORTED' "$LOG"; then
        VERDICT=FAIL
        DETAIL="$(grep -m1 -o 'POPULATE_ABORTED.*' "$LOG")"
    elif grep -qE 'ERROR:|error: ' "$LOG"; then
        VERDICT=FAIL
        DETAIL="$(grep -m1 -E 'ERROR:|error: ' "$LOG" | cut -c1-120)"
    else
        # Throughput shapes: mixed-rw string uses valkey-benchmark --csv ("GET","134228.19"),
        # mixed-rw compound prints "N requests per second", mixed-size prints "N rps",
        # tiering-latency prints "Throughput: N ops/s".
        THRU="$(grep -hoE '^"(GET|SET)","[0-9.]+"|[0-9.]+ requests per second|[0-9.]+ rps|Throughput: +[0-9]+ ops/s' "$LOG" | head -3 | paste -sd' ')"
        if [ -n "$THRU" ]; then
            VERDICT=PASS
            DETAIL="$THRU"
        else
            VERDICT=FAIL
            DETAIL="no throughput line in output"
        fi
    fi

    echo "$VERDICT (${ELAPSED}s)"
    [ -n "$DETAIL" ] && echo "                                    $DETAIL"
    printf '%s,%s,%s,%s,"%s"\n' "$SCEN" "$CFG" "$VERDICT" "$ELAPSED" "${DETAIL//\"/\'}" >> "$CSV"
done

echo ""
echo "══ Audit summary ══"
awk -F, 'NR>1{c[$3]++} END{for(v in c) printf "  %-8s %d\n", v, c[v]}' "$CSV"
echo "  CSV: $CSV"
