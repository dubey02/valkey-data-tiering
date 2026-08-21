#!/usr/bin/env python3
"""Publish a benchmark run as this branch's dashboard view.

benchmark_dashboard/index.html on the GitHub Pages branch is a shell: given ?branch=NAME it
fetches benchmark_dashboard/view.html from that branch and renders it. A branch publishes its
results purely by carrying its own view.html plus data/ -- the shell is never modified.

This follows the same shape as the view on unstable/policies:
  scenario tabs -> per-config checkboxes (colour-coded) -> metric sidebar -> KPI cards + charts
Time series come from each run's metrics.csv (written by tools/metrics-collector), client
latency from valkey-benchmark --csv output, server latency from INFO latencystats.

Sweep legs (e.g. size-sweep-fc/item_size-500000) become separate selectable series.

Usage:
  generate-audit-view.py <results/TAG> [--audit-csv results/TAG/audit-reclassified.csv]
Writes benchmark_dashboard/view.html and benchmark_dashboard/data/<scenario>/*.
"""
import argparse
import csv
import datetime
import json
import pathlib
import re
import shutil
import subprocess
import sys

SCENARIO_LABELS = [
    ("mixed-rw", "Mixed R/W"),
    ("mixed-size", "Mixed Size"),
    ("tiering-latency", "Tiering Latency"),
]

# Distinct hues; cycled per scenario.
PALETTE = ["#0ea5e9", "#f97316", "#8b5cf6", "#10b981", "#ef4444", "#eab308",
           "#ec4899", "#14b8a6", "#a3e635", "#f472b6", "#60a5fa", "#fb923c",
           "#c084fc", "#34d399", "#fca5a5", "#fcd34d", "#22d3ee"]

METRICS = [
    'db0_keys',
    'used_memory', 'used_memory_rss', 'maxmemory', 'keyspace_hits', 'keyspace_misses',
    'ops_per_sec', 'total_commands_delta',
    'total_num_items_spilled_to_ext_storage', 'total_num_items_fetched_from_ext_storage',
    'completion_read_ok', 'dram_value_hits', 'kbc_fetching_block',
    'num_items_spilling_to_ext_storage', 'blocked_clients',
    'cpu_user', 'cpu_sys', 'valkey_cpu_user', 'valkey_cpu_sys', 'valkey_cpu_total', 'asio_cpu_pct',
    'disk_hit_pct', 'mem_hit_pct', 'mem_frag_ratio',
    'disk_read_iops', 'disk_write_iops', 'disk_read_mb', 'disk_write_mb',
    'disk_read_merges_ps', 'disk_write_merges_ps', 'disk_r_await_ms', 'disk_w_await_ms',
    'disk_aqu_sz', 'disk_util_pct', 'disk_in_flight', 'disk_req_sz_kb',
    'throttle_total_throttled', 'throttle_queued_clients', 'throttle_current_rate',
    'throttle_allowed_tps',
    'spill_submitted_count', 'spill_serialized_count', 'mean_spill_ram',
    'inflight_spill_ram_bytes',
    'keys_key_spilled', 'total_keys_dropped_from_dict', 'total_keys_dropped_at_completion',
    'total_keys_rematerialized', 'kbc_keyspill_miss_fetch', 'kbc_keyspill_absent_consumed',
    'kbc_progressive_drain_runs', 'kbc_progressive_drain_resolved',
]

GROUPS = {
    'Dataset': ['db0_keys'],
    'Throughput': ['ops_per_sec', 'total_commands_delta', 'keyspace_hits', 'keyspace_misses'],
    'Memory': ['used_memory', 'used_memory_rss', 'maxmemory', 'mem_frag_ratio'],
    'Tiering': ['total_num_items_spilled_to_ext_storage', 'total_num_items_fetched_from_ext_storage',
                'num_items_spilling_to_ext_storage', 'completion_read_ok', 'dram_value_hits',
                'kbc_fetching_block', 'disk_hit_pct', 'mem_hit_pct'],
    'CPU': ['cpu_user', 'cpu_sys', 'valkey_cpu_user', 'valkey_cpu_sys', 'valkey_cpu_total',
            'asio_cpu_pct'],
    'Disk': ['disk_read_iops', 'disk_write_iops', 'disk_read_mb', 'disk_write_mb',
             'disk_read_merges_ps', 'disk_write_merges_ps', 'disk_r_await_ms', 'disk_w_await_ms',
             'disk_aqu_sz', 'disk_util_pct', 'disk_in_flight', 'disk_req_sz_kb'],
    'Throttle': ['throttle_total_throttled', 'throttle_queued_clients', 'throttle_current_rate',
                 'throttle_allowed_tps', 'blocked_clients'],
    'Spill Pipeline': ['spill_submitted_count', 'spill_serialized_count', 'mean_spill_ram',
                       'inflight_spill_ram_bytes'],
    'Key Spilling': ['keys_key_spilled', 'total_keys_dropped_from_dict',
                     'total_keys_dropped_at_completion', 'total_keys_rematerialized',
                     'kbc_keyspill_miss_fetch', 'kbc_keyspill_absent_consumed',
                     'kbc_progressive_drain_runs', 'kbc_progressive_drain_resolved'],
    'Latency (client)': ['_trace_latency', '_client_get_latency', '_client_set_latency'],
    'Latency (server)': ['_server_latency'],
}

DEFAULT_VISIBLE = ['db0_keys', 'ops_per_sec', 'used_memory', 'disk_util_pct',
                   'total_num_items_spilled_to_ext_storage',
                   'total_num_items_fetched_from_ext_storage',
                   'keys_key_spilled', 'total_keys_dropped_at_completion',
                   'blocked_clients', 'valkey_cpu_total',
                   '_trace_latency', '_client_get_latency', '_client_set_latency',
                   '_server_latency']


def git(repo, *args):
    try:
        return subprocess.run(["git", "-C", str(repo), *args],
                              capture_output=True, text=True, check=True).stdout.strip()
    except Exception:
        return ""


def pretty_leg(leg):
    """Render a sweep leg directory name as a self-describing label.

    benchmark.sh names sweep subdirectories '<var>-<value>' (lowercased var). A bare value is
    ambiguous -- 'size-sweep · 500' reads as though 500 were a rate -- so ITEM_SIZE legs get a
    byte suffix and anything unrecognised keeps 'var=value'.
    """
    var, _, value = leg.rpartition("-")
    if var == "item_size" and value.isdigit():
        n = int(value)
        for div, unit in ((1 << 30, "GB"), (1 << 20, "MB"), (1 << 10, "KB")):
            if n >= div and n % div == 0:
                return f"{n // div}{unit}"
        # Values like 500000 are decimal-round, not binary-round; show them as-is in KB/MB
        # only when exact, otherwise fall back to a plain byte count.
        for div, unit in ((1_000_000, "MB"), (1_000, "KB")):
            if n >= div and n % div == 0:
                return f"{n // div}{unit}"
        return f"{n}B"
    if var == "datatype":
        return value
    return f"{var}={value}" if var else value


def parse_bench_csv(path):
    """valkey-benchmark --csv writes a header row plus one row per command."""
    if not path.exists():
        return None
    rows = list(csv.DictReader(path.open()))
    return rows[0] if rows else None


def workload_rows(mcsv, run):
    """Return (header, workload_rows, dropped) with the populate phase removed.

    Charts that include populate are misleading: memory ramps from zero, ops/s reflects a
    SET-only sequential load, and for some configs populate is most of the wall time
    (fixed-fc-100b spends ~84% of its run populating 10.7M keys).

    Authoritative source is POPULATE_END in phase-markers.env, written by run.sh. Older runs
    have no marker, so fall back to the first sample where keyspace_hits increases -- populate
    issues only SETs, so hits stay flat until the measured workload starts.
    """
    with mcsv.open() as fh:
        lines = fh.read().splitlines()
    if len(lines) < 2:
        return (lines[0] if lines else ""), [], 0
    header, rows = lines[0], lines[1:]
    cols = header.split(",")

    populate_end = None
    marker = run / "phase-markers.env"
    if marker.exists():
        m = re.search(r'POPULATE_END=(\d+)', marker.read_text(errors="replace"))
        if m:
            populate_end = int(m.group(1))

    start = 0
    if populate_end is not None:
        for i, r in enumerate(rows):
            try:
                if int(r.split(",")[0]) >= populate_end:
                    start = i
                    break
            except (ValueError, IndexError):
                continue
    elif "keyspace_hits" in cols:
        k = cols.index("keyspace_hits")
        prev = None
        for i, r in enumerate(rows):
            f = r.split(",")
            if len(f) <= k:
                continue
            try:
                v = float(f[k] or 0)
            except ValueError:
                continue
            if prev is not None and v > prev:
                start = i
                break
            prev = v

    # Never trim everything away: a run with no detectable workload keeps its rows so the
    # series is still visible rather than silently empty.
    if start >= len(rows):
        start = 0
    return header, rows[start:], start


def parse_trace_latency(output_txt):
    """Pull client-side latency out of trace-replay's summary block.

    tiering-latency drives load with trace-replay, not valkey-benchmark, so there is no
    --csv file to read. Client latency -- the whole point of that scenario -- only appears
    in output.txt as:

        Latency:    p50=1.21ms  p99=1.25ms  p99.9=1.25ms  p100=1.25ms
    """
    if not output_txt.exists():
        return None
    txt = output_txt.read_text(errors="replace")
    m = re.search(r'Latency:\s+(p50=.*)', txt)
    if not m:
        return None
    row = {}
    for k, v in re.findall(r'(p[\d.]+)=([\d.]+)ms', m.group(1)):
        row[f"{k}_ms"] = float(v)
    if not row:
        return None
    for field, pat in (("throughput_ops", r'Throughput:\s+([\d.]+) ops/s'),
                       ("hit_ratio_pct", r'Hit ratio:\s+([\d.]+)%'),
                       ("ops", r'Ops:\s+(\d+)')):
        mm = re.search(pat, txt)
        if mm:
            row[field] = float(mm.group(1))
    return row


def parse_server_latency(final_info):
    """Pull latency_percentiles_usec_* lines out of INFO ALL."""
    if not final_info.exists():
        return ""
    out = []
    for line in final_info.read_text(errors="replace").splitlines():
        if line.startswith("latency_percentiles_usec_"):
            out.append(line.strip())
    return "\n".join(out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("results_dir")
    ap.add_argument("--audit-csv", default=None)
    ap.add_argument("--capped", action="store_true",
                    help="Run came from audit-configs.sh without --full, i.e. OPS/DURATION were "
                         "capped. Changes how the view explains short series.")
    args = ap.parse_args()

    rdir = pathlib.Path(args.results_dir).resolve()
    if not rdir.is_dir():
        sys.exit(f"not a directory: {rdir}")

    bench = pathlib.Path(__file__).resolve().parents[2]
    repo = bench.parent
    dash = repo / "benchmark_dashboard"
    data_root = dash / "data"

    # Verdicts, if an audit CSV is available: shown on each series so a config that failed
    # is still visible in the picker rather than silently absent.
    verdicts, details = {}, {}
    acsv = pathlib.Path(args.audit_csv) if args.audit_csv else None
    for cand in [acsv, rdir / "audit-reclassified.csv", rdir / "audit.csv"]:
        if cand and cand.exists():
            for row in csv.DictReader(cand.open()):
                verdicts[(row["scenario"], row["config"])] = row["verdict"]
                details[(row["scenario"], row["config"])] = row.get("detail", "")
            break

    scenarios = []
    for scen_id, scen_label in SCENARIO_LABELS:
        sdir = rdir / scen_id
        if not sdir.is_dir():
            continue
        out_dir = data_root / scen_id
        if out_dir.exists():
            shutil.rmtree(out_dir)
        out_dir.mkdir(parents=True, exist_ok=True)

        series = []
        # A run directory is any directory containing metrics.csv; sweeps nest one level.
        for mcsv in sorted(sdir.rglob("metrics.csv")):
            run = mcsv.parent
            rel = run.relative_to(sdir)
            cfg = rel.parts[0]
            leg = "/".join(rel.parts[1:])
            slug = str(rel).replace("/", "__")
            label = cfg if not leg else f"{cfg} · {pretty_leg(leg)}"

            # A sweep's verdict is per-config, but individual legs can abort while siblings
            # succeed. Read each leg's own output.txt so a failed leg is not charted and a
            # healthy sibling is not tarred with the config-level verdict.
            out_txt = run / "output.txt"
            aborted = ""
            if out_txt.exists():
                am = re.search(r'POPULATE_ABORTED:.*', out_txt.read_text(errors="replace"))
                if am:
                    aborted = am.group(0).strip()

            header, wrows, dropped = workload_rows(mcsv, run)
            (out_dir / f"{slug}.csv").write_text("\n".join([header, *wrows]) + "\n")

            for src, kind in (("get_output.txt", "get"), ("set_output.txt", "set")):
                row = parse_bench_csv(run / src)
                if row:
                    with (out_dir / f"{slug}-{kind}-latency.csv").open("w", newline="") as fh:
                        w = csv.DictWriter(fh, fieldnames=list(row.keys()))
                        w.writeheader()
                        w.writerow(row)
            # Compound configs write a combined client-latency.csv instead.
            clat = run / "client-latency.csv"
            if clat.exists():
                shutil.copyfile(clat, out_dir / f"{slug}-client-latency.csv")

            srv = parse_server_latency(run / "final-info.txt")
            if srv:
                (out_dir / f"{slug}-server-latency.txt").write_text(srv + "\n")

            trace = parse_trace_latency(run / "output.txt")
            if trace:
                with (out_dir / f"{slug}-trace-latency.csv").open("w", newline="") as fh:
                    w = csv.DictWriter(fh, fieldnames=list(trace.keys()))
                    w.writeheader()
                    w.writerow(trace)

            # Sample count is not run length: the collector's poll loop (INFO ALL + system
            # stats) takes ~1.15s per tick, so record wall seconds from the timestamps too.
            # Both describe the measured workload only -- populate rows are dropped above.
            samples = len(wrows)
            wall = 0
            if samples >= 2:
                try:
                    wall = int(wrows[-1].split(",")[0]) - int(wrows[0].split(",")[0])
                except (ValueError, IndexError):
                    wall = 0

            series.append({
                "file": None if aborted else f"{slug}.csv",
                "aborted": aborted,
                "label": label,
                "config": cfg,
                "leg": leg,
                "samples": samples,
                "wall": wall,
                "populateSamples": dropped,
                "verdict": verdicts.get((scen_id, cfg), ""),
            })

        # Configs that produced no metrics.csv at all (e.g. server never started) still need
        # to appear, otherwise the view silently hides a failure.
        seen = {s["config"] for s in series}
        for (vscen, vcfg), verdict in sorted(verdicts.items()):
            if vscen == scen_id and vcfg not in seen:
                series.append({"file": None, "label": vcfg, "config": vcfg, "leg": "",
                               "samples": 0, "verdict": verdict})

        for i, s in enumerate(series):
            s["color"] = PALETTE[i % len(PALETTE)]

        # Configs that exist on disk but were not part of this run at all (deliberately
        # excluded, e.g. known-broken legs). Recorded so the view can say so rather than
        # just appearing to have fewer configs than the suite has.
        cfg_dir = bench / "scenarios" / scen_id / "configs"
        on_disk = {p.stem for p in cfg_dir.glob("*.env")} if cfg_dir.is_dir() else set()
        in_run = {s["config"] for s in series}
        not_run = sorted(on_disk - in_run)

        if series:
            scenarios.append({"id": scen_id, "label": scen_label, "series": series,
                              "notRun": not_run})

    if not scenarios:
        sys.exit(f"no scenario data found under {rdir}")

    branch = git(repo, "rev-parse", "--abbrev-ref", "HEAD") or "unstable"
    meta = {
        "tag": rdir.name,
        "branch": branch,
        "commit": git(repo, "rev-parse", "--short", "HEAD"),
        "generated": datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%d %H:%M UTC"),
        "capped": bool(args.capped),
        "verdictCounts": {},
        "details": {f"{k[0]}/{k[1]}": v for k, v in details.items() if v},
    }
    for v in verdicts.values():
        meta["verdictCounts"][v] = meta["verdictCounts"].get(v, 0) + 1

    payload = json.dumps({
        "meta": meta, "scenarios": scenarios,
        "metrics": METRICS, "groups": GROUPS, "defaultVisible": DEFAULT_VISIBLE,
    })

    (dash / "view.html").write_text(TEMPLATE.replace("__PAYLOAD__", payload))

    n_series = sum(len(s["series"]) for s in scenarios)
    print(f"wrote {dash/'view.html'}")
    print(f"  branch={branch} tag={meta['tag']}")
    print(f"  {len(scenarios)} scenarios, {n_series} series")
    for s in scenarios:
        thin = [x['label'] for x in s['series'] if x['samples'] < 10]
        print(f"    {s['id']:<16} {len(s['series'])} series"
              + (f"  ({len(thin)} with <10 samples)" if thin else ""))
    print(f"  data -> {data_root}")


TEMPLATE = r"""<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>Valkey Data Tiering — Benchmarks</title>
  <script src="https://cdn.jsdelivr.net/npm/chart.js@4"></script>
  <script src="https://cdn.jsdelivr.net/npm/papaparse@5"></script>
  <style>
    :root { --bg:#1a1a2e;--surface:#16213e;--border:#0f3460;--text:#e4e4e4;--muted:#a0a0b0;--accent:#0ea5e9; }
    *{box-sizing:border-box;margin:0;padding:0}
    body{font-family:system-ui,sans-serif;background:var(--bg);color:var(--text);display:flex;flex-direction:column;height:100vh}
    /* Scenario tab bar */
    .tab-bar{display:flex;gap:0;border-bottom:2px solid var(--border);flex-shrink:0;background:var(--surface)}
    .tab-bar button{padding:.7rem 1.5rem;background:none;border:none;color:var(--muted);font-size:.85rem;cursor:pointer;border-bottom:2px solid transparent;margin-bottom:-2px;transition:all .15s}
    .tab-bar button:hover{color:var(--text)}
    .tab-bar button.active{color:var(--accent);border-bottom-color:var(--accent);font-weight:600}
    .tab-bar button .bad{color:#ef4444;font-size:.7rem;margin-left:.4rem}
    /* Dataset selector bar */
    .top-bar{padding:.5rem 1.5rem;border-bottom:1px solid var(--border);display:flex;flex-wrap:wrap;gap:.4rem;align-items:center;flex-shrink:0}
    .top-bar label{display:flex;align-items:center;gap:.3rem;background:var(--surface);border:1px solid var(--border);padding:.25rem .6rem;border-radius:4px;font-size:.75rem;cursor:pointer}
    .top-bar label:hover{border-color:var(--accent)}
    .top-bar label.dead{opacity:.55}
    .top-bar input[type="checkbox"]{accent-color:var(--accent)}
    .top-bar .vb{font-size:.6rem;font-weight:700;padding:.05rem .3rem;border-radius:2px}
    .top-bar .vb.FAIL{background:rgba(239,68,68,.2);color:#fca5a5}
    .top-bar .vb.GRIND,.top-bar .vb.TIMEOUT{background:rgba(249,115,22,.2);color:#fdba74}
    .top-bar .thin{font-size:.6rem;color:var(--muted)}
    .layout{display:flex;flex:1;overflow:hidden}
    /* Sidebar */
    .sidebar{width:220px;background:var(--surface);border-right:1px solid var(--border);overflow-y:auto;flex-shrink:0;padding:.5rem}
    .sidebar h3{font-size:.75rem;color:var(--muted);padding:.4rem .3rem;text-transform:uppercase;letter-spacing:.05em}
    .sidebar label{display:flex;align-items:center;gap:.3rem;padding:.2rem .3rem;font-size:.72rem;cursor:pointer;border-radius:3px}
    .sidebar label:hover{background:var(--border)}
    .sidebar input[type="checkbox"]{accent-color:var(--accent);width:13px;height:13px}
    .sidebar .group{margin-bottom:.8rem}
    .sidebar .btn-row{display:flex;gap:.3rem;padding:.3rem;margin-bottom:.5rem}
    .sidebar button{font-size:.68rem;padding:.2rem .5rem;background:var(--bg);border:1px solid var(--border);color:var(--muted);border-radius:3px;cursor:pointer}
    .sidebar button:hover{color:var(--text);border-color:var(--accent)}
    /* Main content */
    .main{flex:1;overflow-y:auto;padding:1.5rem}
    .notice{background:var(--surface);border-left:3px solid #f97316;padding:.55rem .8rem;margin-bottom:1rem;font-size:.72rem;color:var(--muted);line-height:1.5}
    .notice b{color:var(--text)}
    .kpi-row{display:grid;grid-template-columns:repeat(auto-fit,minmax(140px,1fr));gap:.8rem;margin-bottom:1.2rem}
    .kpi{background:var(--surface);border:1px solid var(--border);border-radius:6px;padding:.6rem;text-align:center}
    .kpi .value{font-size:1.2rem;font-weight:700;color:var(--accent)}
    .kpi .label{font-size:.68rem;color:var(--muted);margin-top:.15rem}
    .chart-grid{display:grid;grid-template-columns:1fr 1fr;gap:1.2rem}
    .chart-card{background:var(--surface);border:1px solid var(--border);border-radius:6px;padding:.8rem;display:none}
    .chart-card.visible{display:block}
    .chart-card.wide{grid-column:1/-1}
    .chart-card h3{font-size:.78rem;color:var(--muted);margin-bottom:.4rem}
    canvas{width:100%!important;max-height:240px}
    .scenario-view{display:none;flex-direction:column;flex:1;overflow:hidden}
    .scenario-view.active{display:flex}
    .no-data{display:flex;align-items:center;justify-content:center;flex:1;color:var(--muted);font-size:.9rem;font-style:italic}
    @media(max-width:1000px){.chart-grid{grid-template-columns:1fr}.sidebar{width:180px}}
  </style>
</head>
<body>

  <div class="tab-bar" id="tab-bar"></div>
  <div id="views"></div>

<script>
const DATA = __PAYLOAD__;

const GITHUB_OWNER = 'dubey02';
const GITHUB_REPO = 'valkey-data-tiering';
const DATA_PATH = 'benchmark_dashboard/data';
const currentBranch = DATA.meta.branch;

function rawUrl(branch, path) {
  return `https://raw.githubusercontent.com/${GITHUB_OWNER}/${GITHUB_REPO}/${branch}/${path}`;
}

function setStatus(text, cls) {
  try { parent.postMessage({ type: 'view-status', text, cls }, '*'); } catch (e) {}
}

const chartOpts = {
  responsive:true, animation:false, interaction:{intersect:false,mode:'index'},
  plugins:{legend:{labels:{color:'#a0a0b0',boxWidth:10,font:{size:10}}}},
  scales:{
    x:{ticks:{color:'#a0a0b0',maxTicksLimit:15},grid:{color:'#0f3460'}},
    y:{ticks:{color:'#a0a0b0'},grid:{color:'#0f3460'}},
  }
};

function fetchCsv(path) {
  return new Promise(resolve => {
    Papa.parse(rawUrl(currentBranch, path), {
      download:true, header:true, dynamicTyping:true,
      complete: r => resolve(r.data.filter(row => Object.values(row).some(v => v !== null))),
      error: () => resolve([])
    });
  });
}
function fetchText(path) {
  return fetch(rawUrl(currentBranch, path)).then(r => r.ok ? r.text() : '').catch(() => '');
}

const LAT_FIELDS = ['avg_latency_ms','p50_latency_ms','p95_latency_ms','p99_latency_ms','max_latency_ms'];

// Per-series status. A config-level verdict must NOT be applied to a series that ran fine:
// a sweep fails as a whole if any leg aborts, but its healthy legs are still valid data.
// So only report a failure for a series that actually has none: it aborted, or it produced
// no metrics at all.
function seriesStatus(s) {
  if (s.aborted) return 'ABORTED';
  if (s.file) return '';                                  // has workload data -> it ran
  return s.verdict && s.verdict !== 'PASS' ? s.verdict : '';
}
const viewsEl = document.getElementById('views');
const tabBar = document.getElementById('tab-bar');
const scenarioState = {};

// ─── Build one scenario view ───
DATA.scenarios.forEach(scen => {
  const wrap = document.createElement('div');
  wrap.className = 'scenario-view';
  wrap.id = 'scenario-' + scen.id;
  wrap.innerHTML = `
    <div class="top-bar"></div>
    <div class="layout">
      <aside class="sidebar"></aside>
      <main class="main">
        <div class="notice"></div>
        <div class="kpi-row"></div>
        <div class="chart-grid"></div>
      </main>
    </div>`;
  viewsEl.appendChild(wrap);

  const st = {
    scen,
    topBar: wrap.querySelector('.top-bar'),
    sidebar: wrap.querySelector('.sidebar'),
    notice: wrap.querySelector('.notice'),
    kpiRow: wrap.querySelector('.kpi-row'),
    grid: wrap.querySelector('.chart-grid'),
    loaded: {}, latency: {get:{},set:{},server:{},trace:{}}, charts: {},
    active: new Set(), visible: new Set(DATA.defaultVisible),
    sidebarCBs: {}, built: false,
  };
  scenarioState[scen.id] = st;

  // Default selection: the six longest series, so the first paint shows real curves rather
  // than whichever configs happen to sort first (often the shortest ones). Falls back to
  // whatever exists when no series is long enough (e.g. mixed-size has a single short run).
  const ranked = scen.series.filter(s => s.file).slice().sort((a, b) => b.samples - a.samples);
  (ranked.filter(s => s.samples > 1).length ? ranked.filter(s => s.samples > 1) : ranked)
    .slice(0, 6).forEach(s => st.active.add(s.file));

  // Series checkboxes
  scen.series.forEach(s => {
    const lbl = document.createElement('label');
    lbl.style.borderLeft = `3px solid ${s.color}`;
    if (!s.file) lbl.classList.add('dead');
    const cb = document.createElement('input');
    cb.type = 'checkbox';
    cb.checked = st.active.has(s.file);
    cb.disabled = !s.file;
    cb.onchange = () => {
      if (cb.checked) st.active.add(s.file); else st.active.delete(s.file);
      render(st);
    };
    lbl.appendChild(cb);
    lbl.append(s.label);
    const badge = seriesStatus(s);
    if (badge) {
      const b = document.createElement('span');
      b.className = 'vb ' + (badge === 'ABORTED' ? 'FAIL' : badge);
      b.textContent = badge;
      lbl.appendChild(b);
    }
    if (s.file && s.samples < 10) {
      const t = document.createElement('span');
      t.className = 'thin';
      t.textContent = `${s.samples}pt`;
      lbl.appendChild(t);
    }
    lbl.title = s.file
      ? `${s.config}${s.leg ? ' / ' + s.leg : ''} — ${s.samples} workload samples over ${s.wall}s wall `
        + `(${s.populateSamples} populate samples dropped) `
        + `(the collector polls INFO ALL, so a tick is ~1.15s, not 1s). Populate is not `
        + `duration-bounded, so legs with a larger derived KEYSPACE run longer.`
      : (s.aborted
          ? `${s.label} — populate aborted, not charted: ${s.aborted}`
          : `${s.config} — ${s.verdict}: ${DATA.meta.details[scen.id + '/' + s.config] || 'no data produced'}`);
    st.topBar.appendChild(lbl);
  });

  // Metric sidebar
  const btnRow = document.createElement('div');
  btnRow.className = 'btn-row';
  const mk = (text, fn) => { const b = document.createElement('button'); b.textContent = text; b.onclick = fn; return b; };
  const allMetrics = () => [...DATA.metrics, '_trace_latency', '_client_get_latency',
                            '_client_set_latency', '_server_latency'];
  btnRow.append(
    mk('All',     () => { st.visible = new Set(allMetrics()); syncSidebar(st); updateVisibility(st); }),
    mk('None',    () => { st.visible.clear(); syncSidebar(st); updateVisibility(st); }),
    mk('Default', () => { st.visible = new Set(DATA.defaultVisible); syncSidebar(st); updateVisibility(st); }),
  );
  st.sidebar.appendChild(btnRow);

  Object.entries(DATA.groups).forEach(([group, fields]) => {
    const div = document.createElement('div');
    div.className = 'group';
    const h = document.createElement('h3');
    h.textContent = group;
    div.appendChild(h);
    fields.forEach(f => {
      const lbl = document.createElement('label');
      const cb = document.createElement('input');
      cb.type = 'checkbox';
      cb.checked = st.visible.has(f);
      cb.onchange = () => { if (cb.checked) st.visible.add(f); else st.visible.delete(f); updateVisibility(st); };
      st.sidebarCBs[f] = cb;
      lbl.appendChild(cb);
      lbl.append(f.replace(/^_/, '').replace(/_/g, ' '));
      div.appendChild(lbl);
    });
    st.sidebar.appendChild(div);
  });

  // Chart cards
  DATA.metrics.forEach(m => {
    const card = document.createElement('div');
    card.className = 'chart-card' + ((m === 'ops_per_sec' || m === 'db0_keys') ? ' wide' : '') + (st.visible.has(m) ? ' visible' : '');
    card.dataset.metric = m;
    card.innerHTML = `<h3>${m.replace(/_/g, ' ')}</h3><canvas></canvas>`;
    st.grid.appendChild(card);
    st.charts[m] = new Chart(card.querySelector('canvas'),
      { type:'line', data:{labels:[],datasets:[]}, options:chartOpts });
  });
  [['_trace_latency','Client Latency (ms, trace-replay) — primary metric for tiering-latency'],
   ['_client_get_latency','Client GET Latency (ms)'],
   ['_client_set_latency','Client SET Latency (ms)'],
   ['_server_latency','Server-side Latency (µs)']].forEach(([m,title]) => {
    const card = document.createElement('div');
    card.className = 'chart-card wide' + (st.visible.has(m) ? ' visible' : '');
    card.dataset.metric = m;
    card.innerHTML = `<h3>${title}</h3><canvas></canvas>`;
    st.grid.appendChild(card);
    st.charts[m] = new Chart(card.querySelector('canvas'),
      { type:'bar', data:{labels:[],datasets:[]}, options:chartOpts });
  });

  const bad = scen.series.filter(s => seriesStatus(s) && !s.aborted);
  const thin = scen.series.filter(s => s.file && s.samples < 10);
  const notRun = scen.notRun || [];
  const aborted = scen.series.filter(s => s.aborted);
  st.notice.innerHTML =
    `Run <b>${DATA.meta.tag}</b> at commit <b>${DATA.meta.commit}</b>, generated ${DATA.meta.generated}. `
    + `Charts cover the measured workload only — populate-phase samples are dropped, so memory
       does not ramp from zero and ops/s is not diluted by the SET-only load phase. `
    + (DATA.meta.capped
        ? `Workload lengths were <b>capped</b> (config audit), so series are short by construction —
           treat these as "it ran", not as performance data. `
        : `Configs ran at their own full <b>OPS</b>/<b>DURATION</b>. `)
    + (bad.length ? `<b>${bad.length}</b> config(s) did not pass and carry no series: `
        + bad.map(s => `${s.config} (${s.verdict})`).join(', ') + '. ' : '')
    + (notRun.length ? `Not included in this run: <b>${notRun.join(', ')}</b>. ` : '')
    + (aborted.length ? `<b>${aborted.length}</b> leg(s) aborted during populate and are not
        charted: ` + aborted.map(s => s.label).join(', ') + '. ' : '')
    + (thin.length && !DATA.meta.capped
        ? `<b>${thin.length}</b> series still have under 10 points — metrics are sampled once per
           second and those configs set a small OPS/RPS of their own, so the workload simply
           finishes in a few seconds. Raising their OPS is a config change, not a harness one.`
        : '');
});

function syncSidebar(st) {
  Object.entries(st.sidebarCBs).forEach(([f, cb]) => cb.checked = st.visible.has(f));
}
function updateVisibility(st) {
  st.grid.querySelectorAll('.chart-card').forEach(el =>
    el.classList.toggle('visible', st.visible.has(el.dataset.metric)));
}

async function loadScenario(st) {
  if (st.built) return;
  st.built = true;
  const withData = st.scen.series.filter(s => s.file);
  const base = f => `${DATA_PATH}/${st.scen.id}/${f}`;

  await Promise.all(withData.map(async s => {
    st.loaded[s.file] = await fetchCsv(base(s.file));
  }));
  // trace-replay scenarios (tiering-latency) report client latency here, not via --csv files.
  await Promise.all(withData.map(async s => {
    const t = await fetchCsv(base(`${s.file.replace('.csv','')}-trace-latency.csv`));
    if (t[0]) st.latency.trace[s.file] = t[0];
  }));
  await Promise.all(withData.map(async s => {
    const stem = s.file.replace('.csv', '');
    const [g, sx] = await Promise.all([
      fetchCsv(base(`${stem}-get-latency.csv`)),
      fetchCsv(base(`${stem}-set-latency.csv`)),
    ]);
    if (g[0]) st.latency.get[s.file] = g[0];
    if (sx[0]) st.latency.set[s.file] = sx[0];
    // Compound configs emit a single combined file instead of get/set pairs.
    if (!g[0] && !sx[0]) {
      const c = await fetchCsv(base(`${stem}-client-latency.csv`));
      if (c[0]) st.latency.get[s.file] = { p50_latency_ms:c[0].p50_ms, p95_latency_ms:c[0].p95_ms,
                                           p99_latency_ms:c[0].p99_ms, max_latency_ms:c[0].p100_ms };
      if (c[1]) st.latency.set[s.file] = { p50_latency_ms:c[1].p50_ms, p95_latency_ms:c[1].p95_ms,
                                           p99_latency_ms:c[1].p99_ms, max_latency_ms:c[1].p100_ms };
    }
  }));
  await Promise.all(withData.map(async s => {
    const txt = await fetchText(base(`${s.file.replace('.csv','')}-server-latency.txt`));
    if (!txt) return;
    const parsed = {};
    txt.trim().split('\n').forEach(line => {
      const m = line.match(/^latency_percentiles_usec_(\w+):(.+)/);
      if (!m) return;
      const vals = {};
      m[2].split(',').forEach(p => { const [k,v] = p.split('='); vals[k] = parseFloat(v); });
      parsed[m[1]] = vals;
    });
    st.latency.server[s.file] = parsed;
  }));
  render(st);
}

function render(st) {
  const sel = st.scen.series.filter(s => s.file && st.active.has(s.file));

  DATA.metrics.forEach(m => {
    const chart = st.charts[m];
    const datasets = [];
    let maxLen = 0;
    sel.forEach(s => {
      const rows = st.loaded[s.file];
      if (!rows || !rows.length) return;
      maxLen = Math.max(maxLen, rows.length);
      datasets.push({ label:s.label, data:rows.map(r => r[m] ?? 0), borderColor:s.color,
                      tension:.2, pointRadius:0, borderWidth:1.5 });
    });
    chart.data.labels = Array.from({length:maxLen}, (_,i) => i+1);
    chart.data.datasets = datasets;
    chart.update();
  });

  traceBar(st, sel);
  latBar(st, '_client_get_latency', st.latency.get, sel);
  latBar(st, '_client_set_latency', st.latency.set, sel);
  srvBar(st, sel);
  kpis(st, sel);
  updateVisibility(st);
}

// trace-replay reports p50/p99/p99.9/p100 -- a different percentile set to
// valkey-benchmark's avg/p50/p95/p99/max, so it gets its own chart rather than being
// forced into the GET/SET bars.
const TRACE_FIELDS = ['p50_ms','p99_ms','p99.9_ms','p100_ms'];
function traceBar(st, sel) {
  const chart = st.charts['_trace_latency'];
  chart.data.labels = TRACE_FIELDS.map(f => f.replace('_ms',''));
  chart.data.datasets = sel.filter(s => st.latency.trace[s.file]).map(s => ({
    label:s.label, data:TRACE_FIELDS.map(f => parseFloat(st.latency.trace[s.file][f]) || 0),
    backgroundColor:s.color+'cc', borderColor:s.color, borderWidth:1 }));
  chart.update();
}

function latBar(st, key, data, sel) {
  const chart = st.charts[key];
  chart.data.labels = LAT_FIELDS.map(f => f.replace('_latency_ms','').replace('_',' '));
  chart.data.datasets = sel.filter(s => data[s.file]).map(s => ({
    label:s.label, data:LAT_FIELDS.map(f => parseFloat(data[s.file][f]) || 0),
    backgroundColor:s.color+'cc', borderColor:s.color, borderWidth:1 }));
  chart.update();
}

function srvBar(st, sel) {
  const chart = st.charts['_server_latency'];
  const pct = ['p50','p99','p99.9','p100'], cmds = ['get','set'];
  const labels = [];
  cmds.forEach(c => pct.forEach(p => labels.push(`${c} ${p}`)));
  chart.data.labels = labels;
  chart.data.datasets = sel.filter(s => st.latency.server[s.file]).map(s => {
    const srv = st.latency.server[s.file];
    const data = [];
    cmds.forEach(c => pct.forEach(p => data.push(srv[c]?.[p] ?? 0)));
    return { label:s.label, data, backgroundColor:s.color+'cc', borderColor:s.color, borderWidth:1 };
  });
  chart.update();
}

function kpis(st, sel) {
  st.kpiRow.innerHTML = '';
  sel.forEach(s => {
    const rows = st.loaded[s.file];
    if (!rows || !rows.length) return;
    // Skip the ramp when there is enough of a series to have a steady state.
    const steady = rows.length > 12 ? rows.slice(10) : rows;
    const avg = steady.reduce((a,r) => a + (r.ops_per_sec || 0), 0) / (steady.length || 1);
    const kpi = document.createElement('div');
    kpi.className = 'kpi';
    kpi.innerHTML = `<div class="value" style="color:${s.color}">${(avg/1000).toFixed(1)}K</div>`
                  + `<div class="label">ops/s &middot; ${s.label}</div>`;
    st.kpiRow.appendChild(kpi);
  });
}

// ─── Tabs ───
let currentScenario = DATA.scenarios[0].id;
DATA.scenarios.forEach(scen => {
  const bad = scen.series.filter(s => seriesStatus(s)).length;
  const btn = document.createElement('button');
  btn.dataset.scenario = scen.id;
  btn.innerHTML = scen.label + (bad ? ` <span class="bad">${bad} failing</span>` : '');
  btn.onclick = () => activate(scen.id);
  tabBar.appendChild(btn);
});

function activate(id) {
  currentScenario = id;
  document.querySelectorAll('.scenario-view').forEach(v => v.classList.remove('active'));
  document.querySelectorAll('.tab-bar button').forEach(b => b.classList.remove('active'));
  document.getElementById('scenario-' + id).classList.add('active');
  tabBar.querySelector(`[data-scenario="${id}"]`).classList.add('active');
  loadScenario(scenarioState[id]);
}

const totalBad = Object.entries(DATA.meta.verdictCounts)
  .filter(([v]) => v !== 'PASS').reduce((a,[,n]) => a+n, 0);
setStatus(`${DATA.meta.branch} · ${DATA.meta.tag}` + (totalBad ? ` · ${totalBad} failing` : ''),
          totalBad ? 'error' : 'ok');
activate(currentScenario);
</script>
</body>
</html>
"""

if __name__ == "__main__":
    main()
