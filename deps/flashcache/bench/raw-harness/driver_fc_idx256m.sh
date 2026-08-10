#!/bin/bash
# driver_fc_idx256m.sh — 1KB ratio matrix with pre-sized 256M-bucket index + 2GiB staging.
# QD=128, fresh populate per run (destructive reads), seed 42, settle 300s, measure 900s.
set -u
cd /mnt/nvme
for pct in 100 95 80 50 0; do
  rid="fc_1kb_r${pct}_idx256m"
  echo "=== starting $rid at $(date -u) ==="
  ./run_bench.sh "$rid" nvme1n1 /mnt/nvme/fc_bench \
      /mnt/nvme/fc.db 600 200000000 1024 "$pct" 300 900 42 \
      "/mnt/nvme/results/$rid" 0 128
  echo "=== finished $rid rc=$? at $(date -u) ==="
done
{ echo "ALL DONE"; date -u; } > /mnt/nvme/driver_done_idx256m.txt
