#!/bin/bash
# driver_rocks_c40g.sh — 1KB ratio matrix with 40GiB block cache + 2GiB memtable, 64T.
# Deletes any existing DB and populates fresh on the first run,
# then reuses it (skip_populate=1) for the remaining ratios.
set -u
cd /mnt/nvme
echo "deleting old rocks.db..."
rm -rf /mnt/nvme/rocks.db
first=1
for pct in 100 95 80 50 0; do
  rid="rocks_1kb_r${pct}_t64_c40g"
  skip=$([ "$first" = "1" ] && echo 0 || echo 1)
  first=0
  echo "=== starting $rid (skip_populate=$skip) at $(date -u) ==="
  ./run_bench.sh "$rid" nvme0n1 /mnt/nvme/rocksdb_bench \
      /mnt/nvme/rocks.db 200000000 1024 "$pct" 64 300 900 42 \
      "/mnt/nvme/results/$rid" "$skip"
  echo "=== finished $rid rc=$? at $(date -u) ==="
done
{ echo "ALL DONE"; date -u; } > /mnt/nvme/driver_done_c40g.txt
