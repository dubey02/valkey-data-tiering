#!/usr/bin/env python3
"""Turn a config-audit run into a self-contained dashboard view.

Reads an audit results directory (audit.csv + per-config .log files, as produced by
benchmark/audit-configs.sh) plus the scenario configs themselves, and emits a single
HTML file with one tab per scenario and one expandable card per config.

The output inlines all of its data. benchmark_dashboard/index.html renders views via
iframe srcdoc, which breaks relative fetches, so a self-contained file is the only
shape that works there without hardcoding raw.githubusercontent URLs.

Usage:
  generate-audit-view.py <results/TAG> [-o benchmark_dashboard/config-audit.html]
"""
import argparse
import csv
import datetime
import html
import json
import pathlib
import re
import subprocess
import sys

SCENARIOS = ["mixed-rw", "mixed-size", "tiering-latency"]
VERDICT_ORDER = {"FAIL": 0, "GRIND": 1, "TIMEOUT": 2, "PASS": 3}


def git(repo, *args):
    try:
        return subprocess.run(["git", "-C", str(repo), *args],
                              capture_output=True, text=True, check=True).stdout.strip()
    except Exception:
        return ""


def throughput(log_text):
    """Pull throughput figures out of a run log.

    Three output shapes exist: mixed-rw string configs use valkey-benchmark --csv
    ("GET","134228.19"), mixed-rw compound configs print "N requests per second",
    mixed-size prints "N rps", and tiering-latency prints "Throughput: N ops/s".
    """
    out = []
    for m in re.finditer(r'^"([A-Z][^"]*)","([0-9.]+)"', log_text, re.M):
        out.append((m.group(1).split()[0], float(m.group(2))))
    for m in re.finditer(r'^(\S+).*?: ([0-9.]+) requests per second', log_text, re.M):
        out.append((m.group(1).split()[0], float(m.group(2))))
    for m in re.finditer(r'^(\S+) \([^)]*\): ([0-9.]+) rps', log_text, re.M):
        out.append((m.group(1), float(m.group(2))))
    for m in re.finditer(r'Throughput: +([0-9]+) ops/s', log_text):
        out.append(("ops/s", float(m.group(1))))
    # De-duplicate while preserving order; sweeps repeat the same command name per leg.
    seen, uniq = set(), []
    for name, val in out:
        if (name, val) in seen:
            continue
        seen.add((name, val))
        uniq.append({"name": name, "rps": val})
    return uniq


def failure_detail(log_text, verdict, csv_detail):
    if verdict == "PASS":
        return ""
    if csv_detail:
        return csv_detail
    m = re.search(r'.*command not found.*', log_text)
    if m:
        return m.group(0).strip()
    return ""


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("results_dir")
    ap.add_argument("-o", "--out", default=None)
    args = ap.parse_args()

    rdir = pathlib.Path(args.results_dir).resolve()
    if not rdir.is_dir():
        sys.exit(f"not a directory: {rdir}")

    bench = pathlib.Path(__file__).resolve().parents[2]     # benchmark/
    repo = bench.parent                                      # repo root
    out = pathlib.Path(args.out) if args.out else repo / "benchmark_dashboard" / "config-audit.html"

    # Prefer the reclassified CSV when present; it is derived from the same logs but
    # with the corrected verdict rules.
    csv_path = rdir / "audit-reclassified.csv"
    if not csv_path.exists():
        csv_path = rdir / "audit.csv"
    if not csv_path.exists():
        sys.exit(f"no audit.csv or audit-reclassified.csv in {rdir}")

    rows = []
    with csv_path.open() as fh:
        for row in csv.DictReader(fh):
            rows.append(row)

    configs = []
    for row in rows:
        scen, cfg, verdict = row["scenario"], row["config"], row["verdict"]
        log_file = rdir / f"{scen}--{cfg}.log"
        log_text = log_file.read_text(errors="replace") if log_file.exists() else "(log missing)"
        env_file = bench / "scenarios" / scen / "configs" / f"{cfg}.env"
        env_text = env_file.read_text() if env_file.exists() else "(config file not found)"
        configs.append({
            "scenario": scen,
            "config": cfg,
            "verdict": verdict,
            "seconds": row.get("seconds", ""),
            "detail": row.get("detail", ""),
            "failure": failure_detail(log_text, verdict, row.get("detail", "")),
            "throughput": throughput(log_text),
            "env": env_text,
            "log": log_text,
            "tiered": "ext-storage-enabled" in env_text,
        })

    configs.sort(key=lambda c: (VERDICT_ORDER.get(c["verdict"], 9), c["config"]))

    meta = {
        "tag": rdir.name,
        "commit": git(repo, "rev-parse", "--short", "HEAD"),
        "branch": git(repo, "rev-parse", "--abbrev-ref", "HEAD"),
        "subject": git(repo, "log", "-1", "--format=%s"),
        "generated": datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%d %H:%M UTC"),
        "counts": {},
    }
    for c in configs:
        meta["counts"][c["verdict"]] = meta["counts"].get(c["verdict"], 0) + 1

    payload = json.dumps({"meta": meta, "configs": configs})
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(TEMPLATE.replace("__PAYLOAD__", payload))
    print(f"wrote {out}  ({len(configs)} configs, {out.stat().st_size // 1024} KB)")
    for v, n in sorted(meta["counts"].items()):
        print(f"  {v:<8} {n}")


TEMPLATE = r"""<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>Valkey Data Tiering — Benchmark Config Audit</title>
<style>
  :root { --bg:#1a1a2e;--surface:#16213e;--border:#0f3460;--text:#e4e4e4;--muted:#a0a0b0;--accent:#0ea5e9;
          --pass:#10b981;--fail:#ef4444;--warn:#f97316; }
  *{box-sizing:border-box;margin:0;padding:0}
  body{font-family:system-ui,sans-serif;background:var(--bg);color:var(--text);display:flex;flex-direction:column;min-height:100vh}
  header{padding:1rem 1.5rem .75rem;background:var(--surface);border-bottom:1px solid var(--border)}
  header h1{font-size:1rem;font-weight:600}
  header h1 span{color:var(--accent)}
  .meta{font-size:.72rem;color:var(--muted);margin-top:.35rem;display:flex;gap:1.2rem;flex-wrap:wrap}
  .meta code{color:var(--text);background:#0d1117;padding:.05rem .3rem;border-radius:3px}
  .pills{display:flex;gap:.5rem;margin-top:.6rem;flex-wrap:wrap}
  .pill{font-size:.7rem;padding:.2rem .6rem;border-radius:999px;border:1px solid var(--border);background:#0d1117}
  .pill b{font-weight:700}
  .pill.PASS b{color:var(--pass)} .pill.FAIL b{color:var(--fail)}
  .pill.GRIND b,.pill.TIMEOUT b{color:var(--warn)}
  .banner{margin:.75rem 1.5rem 0;padding:.6rem .8rem;border-left:3px solid var(--warn);background:#0d1117;font-size:.75rem;color:var(--muted);line-height:1.5}
  .banner b{color:var(--text)}
  .tab-bar{display:flex;gap:0;border-bottom:2px solid var(--border);background:var(--surface);margin-top:.75rem;overflow-x:auto}
  .tab-bar button{padding:.7rem 1.5rem;background:none;border:none;color:var(--muted);font-size:.85rem;cursor:pointer;
                  border-bottom:2px solid transparent;margin-bottom:-2px;white-space:nowrap}
  .tab-bar button:hover{color:var(--text)}
  .tab-bar button.active{color:var(--accent);border-bottom-color:var(--accent);font-weight:600}
  .tab-bar button .n{font-size:.7rem;color:var(--muted);margin-left:.4rem}
  main{padding:1rem 1.5rem 3rem;flex:1}
  .card{border:1px solid var(--border);border-radius:6px;margin-bottom:.6rem;background:var(--surface);overflow:hidden}
  .card>summary{padding:.6rem .9rem;cursor:pointer;display:flex;align-items:center;gap:.75rem;font-size:.82rem;list-style:none}
  .card>summary::-webkit-details-marker{display:none}
  .card>summary:hover{background:#1b2a4e}
  .card>summary .chev{color:var(--muted);font-size:.7rem;width:.7rem;flex-shrink:0}
  .card[open]>summary .chev{transform:rotate(90deg)}
  .name{font-family:ui-monospace,monospace;font-weight:600}
  .badge{font-size:.65rem;font-weight:700;padding:.15rem .5rem;border-radius:3px;letter-spacing:.03em;flex-shrink:0}
  .badge.PASS{background:rgba(16,185,129,.15);color:var(--pass)}
  .badge.FAIL{background:rgba(239,68,68,.15);color:var(--fail)}
  .badge.GRIND,.badge.TIMEOUT{background:rgba(249,115,22,.15);color:var(--warn)}
  .tierpill{font-size:.62rem;color:var(--muted);border:1px solid var(--border);padding:.1rem .35rem;border-radius:3px;flex-shrink:0}
  .thru{margin-left:auto;font-family:ui-monospace,monospace;font-size:.72rem;color:var(--muted);text-align:right}
  .thru b{color:var(--text);font-weight:600}
  .why{padding:.55rem .9rem;background:rgba(239,68,68,.07);border-top:1px solid var(--border);
       font-family:ui-monospace,monospace;font-size:.72rem;color:#fca5a5;white-space:pre-wrap;word-break:break-word}
  .why.warn{background:rgba(249,115,22,.07);color:#fdba74}
  .panes{display:grid;grid-template-columns:minmax(0,1fr) minmax(0,1.4fr);gap:1px;background:var(--border);border-top:1px solid var(--border)}
  @media (max-width:900px){.panes{grid-template-columns:minmax(0,1fr)}}
  .pane{background:#0d1117;display:flex;flex-direction:column;min-width:0}
  .pane h4{font-size:.68rem;text-transform:uppercase;letter-spacing:.05em;color:var(--muted);
           padding:.45rem .7rem;border-bottom:1px solid var(--border);background:var(--surface)}
  .pane pre{padding:.7rem;overflow:auto;max-height:26rem;font-family:ui-monospace,monospace;font-size:.7rem;line-height:1.5;white-space:pre}
  .empty{color:var(--muted);font-size:.8rem;padding:2rem;text-align:center}
</style>
</head>
<body>

<header>
  <h1><span>Valkey</span> Data Tiering — Benchmark Config Audit</h1>
  <div class="meta" id="meta"></div>
  <div class="pills" id="pills"></div>
</header>

<div class="banner">
  Every config in <code>benchmark/scenarios/*/configs/</code> was run end-to-end against this commit on a
  single aarch64 host with NVMe at <code>/mnt/nvme</code>. <b>Throughput here is not performance data</b> —
  the audit caps <code>OPS</code> at 200k, <code>DURATION</code> at 10s and <code>KEY_COUNT</code> at 100k so
  each config is exercised (parse &rarr; server start &rarr; populate &rarr; workload &rarr; report) in bounded
  time. A verdict of PASS means the config runs and reports throughput, not that it is fast.
</div>

<div class="tab-bar" id="tabs"></div>
<main id="body"></main>

<script>
const DATA = __PAYLOAD__;
const SCENARIOS = ['mixed-rw','mixed-size','tiering-latency'];

const esc = s => String(s).replace(/[&<>]/g, c => ({'&':'&amp;','<':'&lt;','>':'&gt;'}[c]));

const m = DATA.meta;
document.getElementById('meta').innerHTML = [
  `branch <code>${esc(m.branch)}</code>`,
  `commit <code>${esc(m.commit)}</code>`,
  `run tag <code>${esc(m.tag)}</code>`,
  `generated ${esc(m.generated)}`
].join('');

document.getElementById('pills').innerHTML =
  Object.entries(m.counts).sort().map(([v,n]) =>
    `<span class="pill ${v}"><b>${n}</b> ${v}</span>`).join('') +
  `<span class="pill"><b>${DATA.configs.length}</b> total</span>`;

function card(c) {
  const thru = c.throughput.length
    ? c.throughput.slice(0,4).map(t => `${t.name} <b>${Math.round(t.rps).toLocaleString()}</b>`).join(' · ')
    : '<i>no throughput reported</i>';
  const why = c.failure
    ? `<div class="why ${c.verdict === 'PASS' ? '' : (c.verdict === 'FAIL' ? '' : 'warn')}">${esc(c.failure)}</div>`
    : '';
  return `
  <details class="card">
    <summary>
      <span class="chev">&#9654;</span>
      <span class="badge ${c.verdict}">${c.verdict}</span>
      <span class="name">${esc(c.config)}</span>
      <span class="tierpill">${c.tiered ? 'tiering' : 'no tiering'}</span>
      ${c.seconds ? `<span class="tierpill">${esc(c.seconds)}s</span>` : ''}
      <span class="thru">${thru}</span>
    </summary>
    ${why}
    <div class="panes">
      <div class="pane"><h4>${esc(c.config)}.env</h4><pre>${esc(c.env)}</pre></div>
      <div class="pane"><h4>run output</h4><pre>${esc(c.log)}</pre></div>
    </div>
  </details>`;
}

const tabs = document.getElementById('tabs');
const body = document.getElementById('body');
let current = null;

function render(scen) {
  current = scen;
  [...tabs.children].forEach(b => b.classList.toggle('active', b.dataset.s === scen));
  const list = DATA.configs.filter(c => c.scenario === scen);
  body.innerHTML = list.length
    ? list.map(card).join('')
    : '<div class="empty">No configs recorded for this scenario.</div>';
}

SCENARIOS.forEach(s => {
  const list = DATA.configs.filter(c => c.scenario === s);
  if (!list.length) return;
  const bad = list.filter(c => c.verdict !== 'PASS').length;
  const b = document.createElement('button');
  b.dataset.s = s;
  b.innerHTML = `${s}<span class="n">${list.length}${bad ? ` · ${bad} failing` : ''}</span>`;
  b.onclick = () => render(s);
  tabs.appendChild(b);
});

render(SCENARIOS.find(s => DATA.configs.some(c => c.scenario === s)));
</script>
</body>
</html>
"""

if __name__ == "__main__":
    main()
