#!/bin/bash
# run_bench.sh — host-side wrapper. Starts system collectors, runs the benchmark
# binary, stores everything under /mnt/nvme/results/<run_id>/.
# Usage: ./run_bench.sh <run_id> <nvme_dev> <binary> [args...]
set -u
RUN_ID="$1"; NVME_DEV="$2"; shift 2
RDIR="/mnt/nvme/results/${RUN_ID}"
mkdir -p "$RDIR"

echo "=== run: $RUN_ID ===" | tee "$RDIR/run_info.txt"
date -u +"start: %Y-%m-%dT%H:%M:%SZ" | tee -a "$RDIR/run_info.txt"
echo "cmd: $*" | tee -a "$RDIR/run_info.txt"
uname -a >> "$RDIR/run_info.txt"

# collectors
iostat -x 1 -d "$NVME_DEV" > "$RDIR/iostat.log" 2>&1 &
IOSTAT_PID=$!
( while true; do date -u +"%s $(free -m | awk 'NR==2{print $3" "$4" "$6}')"; sleep 10; done ) > "$RDIR/mem.log" 2>&1 &
MEM_PID=$!

# run benchmark (results_dir arg is expected to be $RDIR already)
"$@" > "$RDIR/bench_stdout.log" 2> "$RDIR/bench_stderr.log" &
BENCH_PID=$!
pidstat -t -p $BENCH_PID 1 > "$RDIR/pidstat.log" 2>&1 &
PIDSTAT_PID=$!

wait $BENCH_PID
RC=$?
kill $IOSTAT_PID $MEM_PID $PIDSTAT_PID 2>/dev/null
date -u +"end: %Y-%m-%dT%H:%M:%SZ" >> "$RDIR/run_info.txt"
echo "exit_code: $RC" >> "$RDIR/run_info.txt"
gzip -f "$RDIR/iostat.log" "$RDIR/pidstat.log" 2>/dev/null
echo "run $RUN_ID finished rc=$RC, results in $RDIR"
exit $RC
