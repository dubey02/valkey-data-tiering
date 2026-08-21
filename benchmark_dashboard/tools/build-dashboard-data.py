#!/usr/bin/env python3
"""build-dashboard-data.py — convert a benchmark results run into
benchmark_dashboard/data/ files for the branch view.

Per dataset produces:
  <name>.csv                 metrics.csv + key-spill columns from info-full.log
  <name>-get-latency.csv     get_output.txt as-is
  <name>-set-latency.csv     set_output.txt as-is
  <name>-server-latency.txt  latency_percentiles_usec_* lines from final-info.txt
"""
import csv, os, re, shutil, sys

KEYSPILL_FIELDS = [
    "keys_key_spilled", "total_keys_dropped_from_dict",
    "total_keys_dropped_at_completion", "total_keys_rematerialized",
    "kbc_keyspill_miss_fetch", "kbc_keyspill_absent_consumed",
    "kbc_progressive_drain_runs", "kbc_progressive_drain_resolved",
]

# results config dir -> dashboard dataset stem.
# Names shared with unstable's view enable cross-branch comparison.
DATASETS = {
    "mixed-rw/zipfian-flashcache":   "zipfian-80-20",
    "mixed-rw/uniform-flashcache":   "uniform-80-20",
    "mixed-rw/balanced-flashcache":  "balanced-50-50",
    "mixed-rw/zipfian-1gb-ttl":      "zipfian-ttl-120",
    "mixed-rw/zipfian-1gb":          "zipfian-1gb",
    "mixed-rw/size-sweep-fc-100b":   "size-sweep-100b",
    # Value Sizes tab (valid legs only: 500KB/5MB blocked by FC 1MB staging ceiling)
    "mixed-rw/size-sweep-fc/datatype-string/item_size-5000": "size-sweep-5000",
}

# Tiering-latency configs -> data/tiering-latency/<stem>.*
TL_DATASETS = {
    "tiering-latency/idle":      "idle",
    "tiering-latency/idle-hash": "idle-hash",
    "tiering-latency/slam":      "slam",
}

def snapshots_keyspill(info_log):
    """Yield one dict of keyspill fields per INFO snapshot, in order."""
    out, cur = [], None
    with open(info_log, errors="ignore") as f:
        for line in f:
            if line.startswith("===== INFO ALL @"):
                if cur is not None: out.append(cur)
                cur = {}
                continue
            if cur is None: continue
            m = re.match(r"([a-z_0-9]+):(\d+)\r?$", line)
            if m and m.group(1) in KEYSPILL_FIELDS:
                cur[m.group(1)] = m.group(2)
    if cur is not None: out.append(cur)
    return out

def build(run_dir, out_dir):
    os.makedirs(out_dir, exist_ok=True)
    for cfg, stem in DATASETS.items():
        src = os.path.join(run_dir, cfg)
        if not os.path.isdir(src):
            print(f"SKIP {cfg} (missing)"); continue
        snaps = snapshots_keyspill(os.path.join(src, "info-full.log"))
        with open(os.path.join(src, "metrics.csv")) as f:
            rows = list(csv.reader(f))
        header, data = rows[0], rows[1:]
        if len(snaps) != len(data):
            print(f"WARN {cfg}: {len(data)} metric rows vs {len(snaps)} snapshots; padding")
        with open(os.path.join(out_dir, stem + ".csv"), "w", newline="") as f:
            w = csv.writer(f)
            w.writerow(header + KEYSPILL_FIELDS)
            for i, row in enumerate(data):
                snap = snaps[i] if i < len(snaps) else {}
                w.writerow(row + [snap.get(k, "0") for k in KEYSPILL_FIELDS])
        for src_name, dst_suffix in (("get_output.txt", "-get-latency.csv"),
                                     ("set_output.txt", "-set-latency.csv")):
            p = os.path.join(src, src_name)
            if os.path.exists(p):
                shutil.copy(p, os.path.join(out_dir, stem + dst_suffix))
        fi = os.path.join(src, "final-info.txt")
        if os.path.exists(fi):
            lines = [l for l in open(fi, errors="ignore")
                     if l.startswith("latency_percentiles_usec_")]
            with open(os.path.join(out_dir, stem + "-server-latency.txt"), "w") as f:
                f.writelines(lines)
        print(f"OK   {cfg} -> {stem}.* ({len(data)} samples)")

def build_tl(run_dir, out_dir):
    """Tiering-latency tab: data/tiering-latency/<stem>{.csv,-latency.csv,-server-latency.txt}"""
    tl_dir = os.path.join(out_dir, "tiering-latency")
    os.makedirs(tl_dir, exist_ok=True)
    for cfg, stem in TL_DATASETS.items():
        src = os.path.join(run_dir, cfg)
        if not os.path.isdir(src):
            print(f"SKIP {cfg} (missing)"); continue
        shutil.copy(os.path.join(src, "metrics.csv"), os.path.join(tl_dir, stem + ".csv"))
        out = open(os.path.join(src, "output.txt"), errors="ignore").read()
        tp = re.search(r"Throughput:\s+(\d+)", out)
        lat = re.search(r"Latency:\s+p50=([\d.]+)ms\s+p99=([\d.]+)ms\s+p99\.9=([\d.]+)ms\s+p100=([\d.]+)ms", out)
        if tp and lat:
            with open(os.path.join(tl_dir, stem + "-latency.csv"), "w") as f:
                f.write("throughput_ops,p50_latency_ms,p99_latency_ms,p999_latency_ms,p100_latency_ms\n")
                f.write(f"{tp.group(1)},{lat.group(1)},{lat.group(2)},{lat.group(3)},{lat.group(4)}\n")
        fi = os.path.join(src, "final-info.txt")
        if os.path.exists(fi):
            lines = [l for l in open(fi, errors="ignore")
                     if l.startswith("latency_percentiles_usec_")]
            with open(os.path.join(tl_dir, stem + "-server-latency.txt"), "w") as f:
                f.writelines(lines)
        print(f"OK   {cfg} -> tiering-latency/{stem}.*")

if __name__ == "__main__":
    build(sys.argv[1], sys.argv[2])
    build_tl(sys.argv[1], sys.argv[2])
