#!/usr/bin/env bash
# benchmark.sh - Valkey benchmark orchestrator
# Usage: ./benchmark.sh [--no-metrics] [--remote] [--tag NAME] SCENARIO [SCENARIO...]
#
# Scenarios are dispatched by folder name: mixed-rw, tiering-latency, mixed-size
#
# --remote: Deploy binaries to EC2 and run there (reads benchmark.env)
#
# Examples:
#   ./benchmark.sh mixed-rw
#   ./benchmark.sh --no-metrics --tag test mixed-rw tiering-latency
#   ./benchmark.sh --remote --config zipfian mixed-rw

DIR="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$DIR/.." && pwd)"
PORT=6399

# Resolve binaries: prefer ../src/ (repo layout), fall back to $DIR (deployed layout)
find_bin() {
    local name="$1"
    for p in "$REPO/src/$name" "$DIR/$name"; do
        [ -x "$p" ] && echo "$p" && return
    done
    echo "$DIR/$name"  # let validation catch it
}
SERVER="$(find_bin valkey-server)"
CLI="$(find_bin valkey-cli)"
TRACE_REPLAY="$(find_bin trace-replay)"
[ -x "$TRACE_REPLAY" ] || TRACE_REPLAY="$DIR/tools/trace-replay/trace-replay"
[ -x "$TRACE_REPLAY" ] || TRACE_REPLAY="$DIR/tools/trace-replay"
METRICS=true
TAG="$(date +%Y%m%d-%H%M%S)"
REMOTE=false
EZBENCH=false
CONFIG_NAME=default

# Parse flags
while [[ "${1:-}" == --* ]]; do
    case "$1" in
        --no-metrics) METRICS=false; shift ;;
        --tag) TAG="$2"; shift 2 ;;
        --remote) REMOTE=true; shift ;;
        --ezbench) EZBENCH=true; shift ;;
        --config) CONFIG_NAME="$2"; shift 2 ;;
        *) echo "Unknown flag: $1"; exit 1 ;;
    esac
done

# Split CONFIG_NAME on comma for multi-config runs
IFS=',' read -ra CONFIG_LIST <<< "$CONFIG_NAME"

[ $# -eq 0 ] && { echo "Usage: benchmark.sh [--no-metrics] [--remote] [--ezbench] [--config NAME[,NAME2]] [--tag NAME] SCENARIO [SCENARIO...]"; exit 1; }

# Validate tools
[ -x "$SERVER" ] || { echo "Build valkey-server first: make -C src"; exit 1; }
[ -x "$CLI" ] || { echo "Build valkey-cli first: make -C src"; exit 1; }

# ─── ezBench mode ───
run_ezbench() {
    local SCENARIO="$1"
    local CFG_NAME="${2:-default}"
    local FOLDER="$SCENARIO"

    local CONFIG="$DIR/scenarios/$FOLDER/configs/$CFG_NAME.env"
    [ -f "$CONFIG" ] || { echo "Config not found: $CONFIG"; return 1; }
    source "$CONFIG"

    local TEST_NAME="nks-${SCENARIO}-${CFG_NAME}-${TAG}"
    local PKG="/tmp/ezbench-${TAG}"
    rm -rf "$PKG"
    mkdir -p "$PKG"/{files,client,target,private,logs,reports,controller}

    echo "╔══════════════════════════════════════════════════╗"
    echo "║  EZBENCH MODE → $TEST_NAME"
    echo "║  Scenario: $SCENARIO config=$CFG_NAME"
    echo "║  Package: $PKG"
    echo "╚══════════════════════════════════════════════════╝"

    # --- Generate test.ini ---
    local REDIS_CONF_SECTION="port=6379
maxmemory=${MAXMEMORY}
maxmemory-policy=${MAXMEMORY_POLICY}
databases=16
protected-mode=no
logfile=/opt/ezbench/logs/valkey.log
dir=/opt/ezbench
dbfilename=dump.rdb
appendonly=no
save="

    # Append ext-storage lines from SERVER_EXTRA_ARGS
    if [ -n "${SERVER_EXTRA_ARGS:-}" ]; then
        local args=($SERVER_EXTRA_ARGS)
        local i=0
        while [ $i -lt ${#args[@]} ]; do
            local key="${args[$i]}"
            if [[ "$key" == --* ]]; then
                key="${key#--}"
                local val="${args[$((i+1))]}"
                REDIS_CONF_SECTION="${REDIS_CONF_SECTION}
${key}=${val}"
                i=$((i+2))
            else
                i=$((i+1))
            fi
        done
        # Override path for ezbench (raw NVMe block device, not file)
        REDIS_CONF_SECTION=$(echo "$REDIS_CONF_SECTION" | sed 's|ext-storage-path=.*|ext-storage-path=/dev/nvme1n1|')
        REDIS_CONF_SECTION=$(echo "$REDIS_CONF_SECTION" | sed 's|ext-storage-capacity-mb=.*|ext-storage-capacity-mb=406937|')
    fi

    # Determine client counts based on read/write split
    local SET_CLIENTS=2
    local GET_CLIENTS=8
    if [ "${READ_PCT:-80}" -ge 90 ]; then
        GET_CLIENTS=9; SET_CLIENTS=1
    elif [ "${READ_PCT:-80}" -le 50 ]; then
        GET_CLIENTS=5; SET_CLIENTS=5
    fi

    cat > "$PKG/test.ini" <<EOF
[general]
test_name=${TEST_NAME}
engine=redis
availability_zone=eu-west-1a
keyname=ezbench-ssh-key
amazon_linux_version=3
enable_ssl=no
repeat=1

[runtime]
keep_stack=no
manual_start=no

[email]
send_to=abhikkum@amazon.com
send_from=abhikkum@amazon.com

[target]
count=1
instance_type=r6gd.2xlarge
volume_size=100
volume_type=gp2

[client/set]
instance_type=c6g.2xlarge
count=${SET_CLIENTS}

[client/get]
instance_type=c6g.2xlarge
count=${GET_CLIENTS}

[controller]
instance_type=m6g.2xlarge

[redis.conf]
${REDIS_CONF_SECTION}
EOF

    # --- Generate client/pre-test.sh (warmup) ---
    cat > "$PKG/client/pre-test.sh" <<'PREEOF'
#!/bin/bash
. ${EZ_HOME}/private/common.rc
REDIS_BENCHMARK=${EZ_HOME}/files/redis-benchmark
PREEOF
    cat >> "$PKG/client/pre-test.sh" <<EOF
KEYSPACE=${KEYSPACE:-500000}
VALUE_SIZE=${ITEM_SIZE:-400}
\${REDIS_BENCHMARK} -h \${EZ_TARGET} -c 10 -r \${KEYSPACE} -n \${KEYSPACE} -t set -d \${VALUE_SIZE}
sleep 5
EOF
    chmod +x "$PKG/client/pre-test.sh"

    # --- Generate client/run-test.sh ---
    local REQUESTS=${OPS:-5000000}
    local CLIENTS_PER_PROCESS=10
    cat > "$PKG/client/run-test.sh" <<'RUNEOF'
#!/bin/bash
. ${EZ_HOME}/private/common.rc
REDIS_BENCHMARK=${EZ_HOME}/files/redis-benchmark
RUNEOF
    cat >> "$PKG/client/run-test.sh" <<EOF
KEYSPACE=${KEYSPACE:-500000}
REQUESTS=${REQUESTS}
VALUE_SIZE=${ITEM_SIZE:-400}
CLIENTS_PER_PROCESS=${CLIENTS_PER_PROCESS}

if [ "\${EZ_GROUP_NAME}" == "set" ]; then
    \${REDIS_BENCHMARK} -h \${EZ_TARGET} -c \${CLIENTS_PER_PROCESS} -r \${KEYSPACE} -n \${REQUESTS} -t set -d \${VALUE_SIZE} &
    \${REDIS_BENCHMARK} -h \${EZ_TARGET} -c \${CLIENTS_PER_PROCESS} -r \${KEYSPACE} -n \${REQUESTS} -t set -d \${VALUE_SIZE} &
elif [ "\${EZ_GROUP_NAME}" == "get" ]; then
    \${REDIS_BENCHMARK} -h \${EZ_TARGET} -c \${CLIENTS_PER_PROCESS} -r \${KEYSPACE} -n \${REQUESTS} -t get -d \${VALUE_SIZE} &
    \${REDIS_BENCHMARK} -h \${EZ_TARGET} -c \${CLIENTS_PER_PROCESS} -r \${KEYSPACE} -n \${REQUESTS} -t get -d \${VALUE_SIZE} &
fi
wait
EOF
    chmod +x "$PKG/client/run-test.sh"

    # --- Copy target/setup.sh ---
    local PERF_DIR="$REPO/perf/tiering-flashcache"
    cp "$PERF_DIR/target/setup.sh" "$PKG/target/setup.sh"
    sed -i "s|\"eu-west-1\" \"non-key-spilling-native-flashcache\"|\"eu-west-1\" \"valkey-bench-${TAG}\"|" "$PKG/target/setup.sh"

    mkdir -p "$PKG/files/tools"
    cp "$DIR/tools/metrics-collector/metrics-collector.sh" "$PKG/files/tools/" 2>/dev/null || true
    cp "$DIR/tools/generate-report/generate-report.py" "$PKG/files/tools/" 2>/dev/null || true

    # --- Copy files/ ---
    cp "$REPO/src/valkey-server" "$PKG/files/redis-server"
    cp "$REPO/src/valkey-cli" "$PKG/files/redis-cli"
    [ -x "$REPO/src/valkey-benchmark" ] && cp "$REPO/src/valkey-benchmark" "$PKG/files/redis-benchmark"
    cp "$PERF_DIR/files/target-metrics-collector.sh" "$PKG/files/" 2>/dev/null || true
    cp "$PERF_DIR/files/metric_collector.py" "$PKG/files/" 2>/dev/null || true
    cp "$PERF_DIR/files/metric_collector_v2.py" "$PKG/files/" 2>/dev/null || true
    cp "$PERF_DIR/files/irq_affinity.py" "$PKG/files/" 2>/dev/null || true
    cp "$PERF_DIR/files/libvalkeylua.so" "$PKG/files/" 2>/dev/null || true
    cp "$PERF_DIR/files/install_boto.sh" "$PKG/files/" 2>/dev/null || true

    echo "$REDIS_CONF_SECTION" | tr '=' ' ' | sed 's/^//' > "$PKG/files/redis.conf"

    cat >> "$PKG/target/setup.sh" << 'REPORT_EOF'

S3_BUCKET="s3://ezbench-833348497722"
REPORT_PREFIX="${S3_BUCKET}/reports/$(hostname)-$(date +%Y%m%d-%H%M)"
METRICS_CSV="${EZ_HOME}/logs/metrics.csv"
REPORT_DIR="${EZ_HOME}/logs/report"
mkdir -p "$REPORT_DIR"

if [ -f "${EZ_HOME}/files/tools/metrics-collector.sh" ]; then
    export VALKEY_CLI="${REDIS_CLI}"
    nohup bash "${EZ_HOME}/files/tools/metrics-collector.sh" 6379 "$METRICS_CSV" 5 > ${EZ_HOME}/logs/metrics-collector.log 2>&1 &
fi

nohup bash -c '
while true; do
    sleep 30
    if [ -f "'$METRICS_CSV'" ] && [ -f "'${EZ_HOME}'/files/tools/generate-report.py" ]; then
        python3 "'${EZ_HOME}'/files/tools/generate-report.py" "'$REPORT_DIR'" 2>/dev/null
        aws s3 cp "'$REPORT_DIR'/report.html" "'$REPORT_PREFIX'/report.html" --region eu-west-1 2>/dev/null
        aws s3 cp "'$METRICS_CSV'" "'$REPORT_PREFIX'/metrics.csv" --region eu-west-1 2>/dev/null
    fi
done
' > ${EZ_HOME}/logs/report-uploader.log 2>&1 &
REPORT_EOF

    cp "$PERF_DIR/private/common.rc" "$PKG/private/common.rc" 2>/dev/null || true

    # --- Launch ---
    echo ""
    echo "→ Launching ezbench..."
    local EZBENCH_BIN="/apollo/env/ezBench/bin/ezbench"
    [ -x "$EZBENCH_BIN" ] || EZBENCH_BIN="$PERF_DIR/ezbench"
    [ -x "$EZBENCH_BIN" ] || { echo "ERROR: ezbench binary not found"; return 1; }

    cd "$PKG"
    "$EZBENCH_BIN" --run --aws-account 833348497722 --region eu-west-1 --no-auth 2>&1 | tee /tmp/ezbench-launch-${TAG}.log &
    local LAUNCH_PID=$!

    sleep 5
    local STACK_NAME=$(grep -oP 'ezBench[A-Za-z0-9]+' /tmp/ezbench-launch-${TAG}.log 2>/dev/null | head -1)

    echo ""
    echo "══════════════════════════════════════════════════"
    echo "  ezBench launched (PID: $LAUNCH_PID)"
    [ -n "$STACK_NAME" ] && echo "  Stack: $STACK_NAME"
    [ -n "$STACK_NAME" ] && echo "  URL: https://eu-west-1.console.aws.amazon.com/cloudformation/home?region=eu-west-1#/stacks?filteringText=$STACK_NAME"
    echo "  Log: /tmp/ezbench-launch-${TAG}.log"
    echo "  Package: $PKG"
    echo "══════════════════════════════════════════════════"
}

if [ "$EZBENCH" = true ]; then
    for S in "$@"; do
        run_ezbench "$S" "${CONFIG_LIST[0]}"
    done
    exit 0
fi
if [ "$REMOTE" = true ]; then
    ENV_FILE="$DIR/benchmark.env"
    [ -f "$ENV_FILE" ] || { echo "Remote mode requires benchmark.env (see benchmark.env.example)"; exit 1; }
    source "$ENV_FILE"
    : "${EC2_HOST:?EC2_HOST not set in benchmark.env}"
    : "${EC2_USER:?EC2_USER not set in benchmark.env}"
    : "${EC2_KEYPATH:?EC2_KEYPATH not set in benchmark.env}"
    EC2_KEYPATH="${EC2_KEYPATH/#\~/$HOME}"
    [ -f "$EC2_KEYPATH" ] || { echo "SSH key not found: $EC2_KEYPATH"; exit 1; }
    : "${EC2_REMOTE_DIR:=/tmp/valkey-bench}"

    SSH_OPTS="-o StrictHostKeyChecking=no -o ConnectTimeout=10 -o ServerAliveInterval=30 -i $EC2_KEYPATH"
    SSH="ssh $SSH_OPTS $EC2_USER@$EC2_HOST"
    SCP="scp $SSH_OPTS"

    echo "╔══════════════════════════════════════════════════╗"
    echo "║  REMOTE MODE → $EC2_USER@$EC2_HOST"
    echo "║  Scenarios: $*"
    echo "║  Remote dir: $EC2_REMOTE_DIR"
    echo "╚══════════════════════════════════════════════════╝"
    echo ""

    # 1. Create remote directory structure
    echo "→ Preparing remote host..."
    $SSH "pkill -9 valkey-server 2>/dev/null; pkill -9 valkey-benchmark 2>/dev/null; sleep 0.5; mkdir -p $EC2_REMOTE_DIR/{tools/metrics-collector,tools/generate-report,scenarios,results}"

    # 2. Deploy binaries
    echo "→ Deploying binaries..."
    $SCP "$SERVER" "$EC2_USER@$EC2_HOST:$EC2_REMOTE_DIR/valkey-server"
    $SCP "$CLI" "$EC2_USER@$EC2_HOST:$EC2_REMOTE_DIR/valkey-cli"
    $SCP "$TRACE_REPLAY" "$EC2_USER@$EC2_HOST:$EC2_REMOTE_DIR/tools/trace-replay"
    [ -x "$(find_bin valkey-benchmark)" ] && \
        $SCP "$(find_bin valkey-benchmark)" "$EC2_USER@$EC2_HOST:$EC2_REMOTE_DIR/valkey-benchmark"

    # 3. Deploy tools
    echo "→ Deploying tools..."
    $SCP "$DIR/tools/metrics-collector/metrics-collector.sh" \
        "$EC2_USER@$EC2_HOST:$EC2_REMOTE_DIR/tools/metrics-collector/metrics-collector.sh"

    # 4. Deploy scenario configs + lib.sh + this script
    echo "→ Deploying scenario configs..."
    $SSH "mkdir -p $EC2_REMOTE_DIR/scenarios"
    $SCP "$DIR/scenarios/lib.sh" "$EC2_USER@$EC2_HOST:$EC2_REMOTE_DIR/scenarios/lib.sh"
    for S in "$@"; do
        FOLDER="$S"
        [ -d "$DIR/scenarios/$FOLDER" ] || { echo "Unknown scenario: $S"; continue; }
        $SSH "mkdir -p $EC2_REMOTE_DIR/scenarios/$FOLDER/configs"
        for CFG in "${CONFIG_LIST[@]}"; do
            if [ -f "$DIR/scenarios/$FOLDER/configs/$CFG.env" ]; then
                $SCP "$DIR/scenarios/$FOLDER/configs/$CFG.env" \
                    "$EC2_USER@$EC2_HOST:$EC2_REMOTE_DIR/scenarios/$FOLDER/configs/$CFG.env"
            fi
        done
        [ -f "$DIR/scenarios/$FOLDER/run.sh" ] && \
            $SCP "$DIR/scenarios/$FOLDER/run.sh" \
                "$EC2_USER@$EC2_HOST:$EC2_REMOTE_DIR/scenarios/$FOLDER/run.sh"
    done
    $SCP "$DIR/benchmark.sh" "$EC2_USER@$EC2_HOST:$EC2_REMOTE_DIR/benchmark.sh"

    # 5. Deploy generate-report
    $SSH "mkdir -p $EC2_REMOTE_DIR/tools/generate-report"
    $SCP "$DIR/tools/generate-report/generate-report.py" \
        "$EC2_USER@$EC2_HOST:$EC2_REMOTE_DIR/tools/generate-report/generate-report.py"

    # 6. Run benchmark remotely
    echo ""
    echo "→ Running benchmark on remote host..."
    REMOTE_FLAGS=""
    [ "$METRICS" = false ] && REMOTE_FLAGS="--no-metrics"
    REMOTE_FLAGS="$REMOTE_FLAGS --tag $TAG --config $CONFIG_NAME"
    $SSH "cd $EC2_REMOTE_DIR && chmod +x benchmark.sh valkey-server valkey-cli valkey-benchmark tools/trace-replay tools/metrics-collector/metrics-collector.sh 2>/dev/null; ./benchmark.sh $REMOTE_FLAGS $*"

    # 7. Fetch results
    echo ""
    echo "→ Fetching results..."
    RUN_DIR="$DIR/results/$TAG"
    mkdir -p "$RUN_DIR"
    $SCP -r "$EC2_USER@$EC2_HOST:$EC2_REMOTE_DIR/results/$TAG/*" "$RUN_DIR/"

    echo "══════════════════════════════════════════════════"
    echo "  Remote run complete. Results: $RUN_DIR"
    echo "══════════════════════════════════════════════════"
    exit 0
fi

RUN_DIR="$DIR/results/$TAG"
mkdir -p "$RUN_DIR"

echo "╔══════════════════════════════════════════════════╗"
echo "║  Valkey Benchmark Run: $TAG"
echo "║  Scenarios: $*"
echo "║  Results: $RUN_DIR"
echo "╚══════════════════════════════════════════════════╝"
echo ""

# ─── Run one scenario ───
run_scenario() {
    local SCENARIO="$1"
    local CFG_NAME="${2:-default}"
    local FOLDER="$SCENARIO"

    [ -d "$DIR/scenarios/$FOLDER" ] || { echo "Unknown scenario: $SCENARIO (no folder scenarios/$FOLDER)"; return 1; }

    local CONFIG="$DIR/scenarios/$FOLDER/configs/$CFG_NAME.env"
    [ -f "$CONFIG" ] || { echo "Config not found: $CONFIG"; return 1; }
    source "$CONFIG"

    # ─── Generic sweep support ───
    if [[ -z "${_SWEEP_ACTIVE:-}" ]]; then
        local -a _sweep_names=() _sweep_vals=()
        local _v
        for _v in ${!SWEEP_*}; do
            [[ "$_v" == SWEEP_* ]] || continue
            local _name="${_v#SWEEP_}"
            local _values="${!_v}"
            [[ -n "$_values" ]] || continue
            _sweep_names+=("$_name")
            _sweep_vals+=("$_values")
        done
        if [[ ${#_sweep_names[@]} -gt 0 ]]; then
            export _SWEEP_ACTIVE=1
            _sweep_cartesian() {
                local depth=$1; shift
                if [[ $depth -ge ${#_sweep_names[@]} ]]; then
                    local _suffix=""
                    for (( i=0; i<${#_sweep_names[@]}; i++ )); do
                        local _n="${_sweep_names[$i]}"
                        _suffix+="${_n,,}-${!_n}/"
                    done
                    export _SWEEP_SUFFIX="${_suffix%/}"
                    run_scenario "$SCENARIO" "$CFG_NAME"
                    return
                fi
                local _name="${_sweep_names[$depth]}"
                local _vals_str="${_sweep_vals[$depth]}"
                for _val in $_vals_str; do
                    export "$_name=$_val"
                    _sweep_cartesian $((depth+1))
                done
            }
            _sweep_cartesian 0
            unset _SWEEP_ACTIVE _SWEEP_SUFFIX
            return 0
        fi
    fi

    # Results dir
    local RESULTS="$RUN_DIR/$SCENARIO/$CFG_NAME"
    if [[ -n "${_SWEEP_ACTIVE:-}" && -n "${_SWEEP_SUFFIX:-}" ]]; then
        RESULTS="$RUN_DIR/$SCENARIO/$CFG_NAME/$_SWEEP_SUFFIX"
    fi
    mkdir -p "$RESULTS"
    cp "$CONFIG" "$RESULTS/config.env"

    echo "═══════════════════════════════════════════════════"
    echo "  $SCENARIO | config=$CFG_NAME maxmemory=$MAXMEMORY policy=$MAXMEMORY_POLICY"
    echo "  → $RESULTS"
    echo "═══════════════════════════════════════════════════"

    # ─── Provision ext-storage backing file ───
    # FlashCache does NOT create its backing file: flashcacheInit() asserts
    # getFileSize(path) >= capacity_mb*1024*1024 and segfaults otherwise. Pre-allocate
    # the file to >= the configured capacity. Idempotent: only (re)allocates when the
    # existing file is too small (a same-size file is reused; the log is re-created on open).
    if [[ "${SERVER_EXTRA_ARGS:-}" == *--ext-storage-path* ]]; then
        _eargs=($SERVER_EXTRA_ARGS)
        _ext_path=""; _ext_cap_mb=""
        for ((i=0; i<${#_eargs[@]}; i++)); do
            case "${_eargs[$i]}" in
                --ext-storage-path) _ext_path="${_eargs[$((i+1))]}" ;;
                --ext-storage-capacity-mb) _ext_cap_mb="${_eargs[$((i+1))]}" ;;
            esac
        done
        if [ -n "$_ext_path" ] && [ -n "${_ext_cap_mb:-}" ]; then
            _need_bytes=$(( _ext_cap_mb * 1024 * 1024 ))
            _have_bytes=$(stat -c %s "$_ext_path" 2>/dev/null || echo 0)
            if [ "$_have_bytes" -lt "$_need_bytes" ]; then
                echo "  Provisioning ext-storage file $_ext_path → ${_ext_cap_mb}MB ($_need_bytes bytes)"
                rm -f "$_ext_path"
                fallocate -l "$_need_bytes" "$_ext_path" 2>/dev/null \
                    || truncate -s "$_need_bytes" "$_ext_path" \
                    || echo "  WARNING: failed to provision $_ext_path"
            fi
        fi
    fi

    # Start server
    "$CLI" -p $PORT SHUTDOWN NOSAVE 2>/dev/null || true
    sleep 0.5
    "$SERVER" \
        --port $PORT \
        --maxmemory "$MAXMEMORY" \
        --maxmemory-policy "$MAXMEMORY_POLICY" \
        --daemonize yes \
        --logfile "$RESULTS/valkey.log" \
        --dir "$RESULTS" \
        --save "" \
        --dbfilename "temp.rdb" \
        --protected-mode no \
        --latency-tracking yes \
        --latency-tracking-info-percentiles "50 90 99 99.9 100" \
        ${SERVER_EXTRA_ARGS:-}

    for i in $(seq 1 20); do
        if "$CLI" -p $PORT PING >/dev/null 2>&1; then break; fi
        sleep 0.2
    done
    "$CLI" -p $PORT PING >/dev/null 2>&1 || { echo "Server failed to start"; return 1; }

    # Metrics
    local METRICS_PID=""
    if [ "$METRICS" = true ]; then
        export VALKEY_CLI="$CLI"
        bash "$DIR/tools/metrics-collector/metrics-collector.sh" $PORT "$RESULTS/metrics.csv" 1 &
        METRICS_PID=$!
    fi

    # Execute scenario
    RESULTS_DIR="$RESULTS" VALKEY_CLI="$CLI" VALKEY_HOST=127.0.0.1 VALKEY_PORT=$PORT \
        BENCHMARK="$(find_bin valkey-benchmark)" TRACE_REPLAY="$TRACE_REPLAY" \
        bash "$DIR/scenarios/$FOLDER/run.sh" "$CONFIG"

    # Collect final state + cleanup
    "$CLI" -p $PORT INFO ALL > "$RESULTS/final-info.txt" 2>/dev/null
    [ -n "$METRICS_PID" ] && kill $METRICS_PID 2>/dev/null && wait $METRICS_PID 2>/dev/null
    "$CLI" -p $PORT SHUTDOWN NOSAVE 2>/dev/null
    echo ""

    # Per-scenario report
    python3 "$DIR/tools/generate-report/generate-report.py" "$RESULTS" 2>/dev/null
}

# ─── Main loop ───
for S in "$@"; do
    for CFG in "${CONFIG_LIST[@]}"; do
        run_scenario "$S" "$CFG"
    done
done

# Generate report
python3 "$DIR/tools/generate-report/generate-report.py" "$RUN_DIR" 2>/dev/null

echo "══════════════════════════════════════════════════"
echo "  All done. Results: $RUN_DIR"
echo "══════════════════════════════════════════════════"
