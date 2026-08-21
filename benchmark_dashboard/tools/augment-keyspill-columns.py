#!/usr/bin/env python3
"""augment-keyspill-columns.py — append key-spill INFO counters as columns to
every metrics.csv under a results run (from the sibling info-full.log).
Idempotent: skips files that already carry the columns."""
import csv, pathlib, re, sys

FIELDS = ["keys_key_spilled", "total_keys_dropped_from_dict",
          "total_keys_dropped_at_completion", "total_keys_rematerialized",
          "kbc_keyspill_miss_fetch", "kbc_keyspill_absent_consumed",
          "kbc_progressive_drain_runs", "kbc_progressive_drain_resolved"]

def snapshots(info_log):
    out, cur = [], None
    for line in open(info_log, errors="ignore"):
        if line.startswith("===== INFO ALL @"):
            if cur is not None: out.append(cur)
            cur = {}
        elif cur is not None:
            m = re.match(r"([a-z_0-9]+):(\d+)\r?$", line)
            if m and m.group(1) in FIELDS: cur[m.group(1)] = m.group(2)
    if cur is not None: out.append(cur)
    return out

run = pathlib.Path(sys.argv[1])
for mcsv in sorted(run.rglob("metrics.csv")):
    info = mcsv.parent / "info-full.log"
    rows = list(csv.reader(open(mcsv)))
    if not rows or FIELDS[0] in rows[0]:
        print(f"skip {mcsv}"); continue
    snaps = snapshots(info) if info.exists() else []
    with open(mcsv, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(rows[0] + FIELDS)
        for i, row in enumerate(rows[1:]):
            snap = snaps[i] if i < len(snaps) else {}
            w.writerow(row + [snap.get(k, "0") for k in FIELDS])
    print(f"ok   {mcsv} ({len(rows)-1} rows, {len(snaps)} snapshots)")
