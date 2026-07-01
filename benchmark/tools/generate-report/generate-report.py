#!/usr/bin/env python3
"""generate-report.py - Generate HTML + Markdown reports from benchmark results.

Usage:
  python3 generate-report.py RESULTS_DIR
    - If RESULTS_DIR contains subdirectories (scenarios), generates per-scenario
      reports AND a top-level aggregate report.
    - If RESULTS_DIR is a single scenario, generates just that scenario's report.

Output:
  RESULTS_DIR/report.html    (aggregate or single)
  RESULTS_DIR/report.md
  RESULTS_DIR/<scenario>/report.html  (per-scenario)
  RESULTS_DIR/<scenario>/report.md
"""
import csv, json, glob, os, re, sys
from pathlib import Path

SCENARIO_NAMES = {
    "mixed-rw": "Mixed R/W",
    "tiering-latency": "Tiering Storage Latency",
    "mixed-size": "Mixed Value Sizes",
}

def read_scenario_config(scenario_dir):
    """Read the config .env that was used (look for it alongside results)."""
    # Check if config was copied into results, or find it relative to benchmark dir
    for candidate in [
        os.path.join(scenario_dir, "config.env"),
        # Walk up to find scenarios dir
    ]:
        if os.path.exists(candidate):
            return Path(candidate).read_text()
    # Try to find from the benchmark tree
    bench_dir = scenario_dir
    for _ in range(3):
        parent = os.path.dirname(bench_dir)
        if parent == bench_dir:
            break
        bench_dir = parent
    name = os.path.basename(scenario_dir)
    # Map scenario ID to folder — IDs are folder names now
    folder = name
    cfg_path = os.path.join(bench_dir, "scenarios", folder, "configs", "default.env")
    if os.path.exists(cfg_path):
        return Path(cfg_path).read_text()
    return None

def parse_trace_replay(path):
    text = Path(path).read_text()
    result = {"file": os.path.basename(path)}
    # Final summary TPS
    m = re.search(r'Throughput:\s*([\d.]+)\s*ops/s', text)
    if m: result["tps"] = float(m.group(1))
    elif (m := re.search(r'([\d.]+)\s*ops/s', text)):
        result["tps"] = float(m.group(1))
    # Latencies
    for pct in ["p50", "p99", "p99.9", "p100"]:
        m = re.search(rf'{re.escape(pct)}[=:\s]+([\d.]+)', text)
        if m: result[pct] = float(m.group(1))
    # Hit ratio
    m = re.search(r'Hit ratio:\s*([\d.]+)%', text)
    if m: result["hit_ratio"] = float(m.group(1))
    # Ops
    m = re.search(r'Ops:\s*([\d,]+)', text)
    if m: result["ops"] = int(m.group(1).replace(",", ""))
    # Duration
    m = re.search(r'Duration:\s*([\d.]+)s', text)
    if m: result["duration_s"] = float(m.group(1))
    return result


def parse_valkey_benchmark(path):
    """Parse valkey-benchmark output in -q or --csv format, with optional --latency-tracking lines."""
    text = Path(path).read_text()
    result = {"file": os.path.basename(path), "commands": {}}
    total_ops = 0

    # Try --csv format: "test","rps","avg","min","p50","p95","p99","max"
    csv_lines = [l.strip() for l in text.replace('\r', '\n').split('\n')
                 if l.strip() and l.strip().startswith('"') and ',' in l and 'requests per second' not in l
                 and not l.strip().startswith('"test"')]
    for line in csv_lines:
        parts = [p.strip('"') for p in line.split(',')]
        if len(parts) >= 7:
            try:
                cmd = parts[0].split()[0]
                rps = float(parts[1])
                entry = {"rps": rps}
                entry["avg_ms"] = float(parts[2])
                entry["min_ms"] = float(parts[3])
                entry["p50_ms"] = float(parts[4])
                entry["p95_ms"] = float(parts[5])
                entry["p99_ms"] = float(parts[6])
                if len(parts) >= 9:  # has p99.9 and max
                    entry["p999_ms"] = float(parts[7])
                    entry["max_ms"] = float(parts[8])
                elif len(parts) >= 8:  # max only (no p99.9)
                    entry["max_ms"] = float(parts[7])
                result["commands"][cmd] = entry
                total_ops += rps
            except (ValueError, IndexError):
                continue

    # Fallback: -q format 'CMD: NNN.NN requests per second, p50=X.XXX msec'
    if not result["commands"]:
        lines = [l.strip() for l in text.replace('\r', '\n').split('\n') if 'requests per second' in l]
        for line in lines:
            m = re.match(r'(.+?):\s*([\d.]+)\s*requests per second,\s*p50=([\d.]+)\s*msec', line)
            if m:
                cmd, rps, p50 = m.group(1).strip(), float(m.group(2)), float(m.group(3))
                cmd_key = cmd.split()[0] if ' ' in cmd else cmd
                result["commands"][cmd_key] = {"rps": rps, "p50_ms": p50}
                total_ops += rps

    if result["commands"]:
        result["tps"] = total_ops
    return result

def parse_server_latency(path):
    """Parse latency_percentiles_usec_* from final-info.txt"""
    if not os.path.exists(path):
        return {}
    text = Path(path).read_text()
    latencies = {}
    for m in re.finditer(r'latency_percentiles_usec_(\S+):(.+)', text):
        cmd = m.group(1).lower()
        if cmd in ("info", "config|get", "config|set", "dbsize", "shutdown"):
            continue
        vals = {}
        for pm in re.finditer(r'(p[\d.]+)=([\d.]+)', m.group(2)):
            vals[pm.group(1)] = float(pm.group(2))
        if vals:
            latencies[cmd] = vals
    return latencies

def parse_outcome(final_info, config_text):
    """Detect a populate OOM (writes rejected at the memory hard cap) from
    captured final-info.txt. Fires ONLY on a real OOM signal — incomplete
    population alone (db keys < KEYSPACE) is NOT an OOM: random-key workloads
    (valkey-benchmark -r N) never touch every key, so db0:keys < KEYSPACE is
    normal."""
    if not final_info or not os.path.exists(final_info):
        return None
    text = Path(final_info).read_text()
    def g(pat):
        m = re.search(pat, text)
        return int(m.group(1)) if m else 0
    oom = g(r'oom_reject_write_count:(\d+)')
    oom_err = g(r'errorstat_OOM:count=(\d+)')
    if oom == 0 and oom_err == 0:
        return None
    expected = 0
    if config_text:
        m = re.search(r'^\s*KEYSPACE=(\d+)', config_text, re.M)
        if m: expected = int(m.group(1))
    return {"oom_reject_write_count": oom,
            "errorstat_OOM": oom_err,
            "spilled": g(r'total_num_items_spilled_to_ext_storage:(\d+)'),
            "db_keys": g(r'db0:keys=(\d+)'), "expected_keys": expected}

def parse_metrics_csv(path):
    rows = []
    with open(path) as f:
        for row in csv.DictReader(f):
            rows.append(row)
    return rows

def scenario_markdown(name, trace_results, metrics_rows, server_latency=None, config_text=None, client_latency=None):
    full_name = f"{name} - {SCENARIO_NAMES[name]}" if name in SCENARIO_NAMES else name
    lines = [f"## {full_name}\n"]
    if config_text:
        lines.append("### Configuration\n")
        lines.append("| Parameter | Value |")
        lines.append("|-----------|-------|")
        cfg_vars = {}
        for line in config_text.strip().split('\n'):
            line = line.strip()
            if line and not line.startswith('#') and '=' in line:
                k, v = line.split('=', 1)
                cfg_vars[k] = v
                lines.append(f"| {k} | {v} |")
        # Add derived values for mixed-rw
        if "READ_PCT" in cfg_vars and "CLIENTS" in cfg_vars:
            clients = int(cfg_vars["CLIENTS"])
            read_pct = int(cfg_vars["READ_PCT"])
            lines.append(f"| GET_CLIENTS | {clients * read_pct // 100} |")
            lines.append(f"| SET_CLIENTS | {clients - clients * read_pct // 100} |")
        lines.append("")
    if trace_results and trace_results.get("outcome"):
        o = trace_results["outcome"]
        lines.append("### Outcome: ABORTED (OOM during populate)\n")
        lines.append("| Result | Value |")
        lines.append("|--------|-------|")
        lines.append("| Status | ABORTED — memory hard cap (4× maxmemory) hit during populate |")
        lines.append(f"| Keys populated | {o['db_keys']:,} / {o['expected_keys']:,} |")
        lines.append(f"| OOM-rejected writes | {o['oom_reject_write_count']:,} |")
        lines.append(f"| Items spilled to flash | {o['spilled']:,} |")
        lines.append("")
    if trace_results:
        tr = trace_results
        lines.append(f"| Metric | Value |")
        lines.append(f"|--------|-------|")
        if "tps" in tr: lines.append(f"| Throughput | {tr['tps']:.0f} ops/s |")
        if "hit_ratio" in tr: lines.append(f"| Hit ratio | {tr['hit_ratio']:.1f}% |")
        if "ops" in tr: lines.append(f"| Ops | {tr['ops']:,} |")
        if "duration_s" in tr: lines.append(f"| Duration | {tr['duration_s']:.1f}s |")
        if "commands" in tr:
            has_percentiles = any("p99_ms" in v for v in tr["commands"].values())
            if has_percentiles:
                lines.append("### Client-Side Latency (ms)\n")
                lines.append("| Command | TPS | p50 | p95 | p99 | p99.9 | max |")
                lines.append("|---------|-----|-----|-----|-----|-------|-----|")
                for cmd, vals in sorted(tr["commands"].items()):
                    def fv(k): return f"{vals[k]:.3f}" if k in vals else ""
                    lines.append(f"| {cmd} | {vals['rps']:.0f} | {fv('p50_ms')} | {fv('p95_ms')} | {fv('p99_ms')} | {fv('p999_ms')} | {fv('max_ms')} |")
            else:
                for cmd, vals in sorted(tr["commands"].items()):
                    lines.append(f"| {cmd} TPS | {vals['rps']:.0f} ops/s |")
                    lines.append(f"| {cmd} p50 | {vals['p50_ms']:.3f} ms |")
        lines.append("")
        if any(p in tr for p in ("p50", "p99", "p99.9", "p100")):
            lines.append("### Client-Side Latency (ms)\n")
            lines.append("| p50 | p99 | p99.9 | p100 |")
            lines.append("|-----|-----|-------|------|")
            lines.append(f"| {tr.get('p50','')} | {tr.get('p99','')} | {tr.get('p99.9','')} | {tr.get('p100','')} |")
            lines.append("")
    if client_latency:
        lines.append("### Client-Side Latency (ms)\n")
        lines.append("| Command | p50 | p95 | p99 | p100 |")
        lines.append("|---------|-----|-----|-----|------|")
        for cmd, vals in sorted(client_latency.items()):
            lines.append(f"| {cmd.upper()} | {vals['p50']:.3f} | {vals['p95']:.3f} | {vals['p99']:.3f} | {vals['p100']:.3f} |")
        lines.append("")
    if server_latency:
        lines.append("### Server-Side Latency (µs)\n")
        lines.append("| Command | p50 | p90 | p99 | p99.9 | p100 |")
        lines.append("|---------|-----|-----|-----|-------|------|")
        for cmd, vals in sorted(server_latency.items()):
            p50 = vals.get("p50", "")
            p90 = vals.get("p90", "")
            p99 = vals.get("p99", "")
            p999 = vals.get("p99.9", "")
            p100 = vals.get("p100", "")
            lines.append(f"| {cmd.upper()} | {p50} | {p90} | {p99} | {p999} | {p100} |")
        lines.append("")
    if metrics_rows:
        # Per-interval DRAM-hit% from the engine's command-agnostic counters:
        #   D = dram_value_hits     (every key access — read OR write — ALLOWED with the
        #                            value resident in DRAM; a fetched request lands here
        #                            once, on its post-promotion re-exec)
        #   F = completion_read_ok  (successful flash->DRAM value promotions = the request
        #                            that TRIGGERED each fetch)
        #   C = kbc_fetching_block  (requests that found a fetch already in flight for the
        #                            key and coalesced onto it — served by that one fetch,
        #                            so they land in D but NOT in F)
        #   DRAM_hit% = (ΔD - ΔF - ΔC) / ΔD * 100
        # = fraction of requests for EXISTING keys served directly from DRAM without
        # eliciting OR waiting on a fetch. Subtracting C accounts for fetches that serve
        # multiple requests (1 trigger in F + N coalesced waiters in C). Bounded [0,100];
        # true misses are excluded (engine counts them as confirmed-absent, not in D).
        # spilling_block/pending_evict are intentionally NOT subtracted: spilling-block
        # writes re-fetch (already in F); pending-evict requests end as misses (not in D).
        prev_sp = prev_ft = prev_dvh = prev_cro = prev_fbk = None
        for r in metrics_rows:
            sp = int(r.get("total_num_items_spilled_to_ext_storage", 0) or 0)
            ft = int(r.get("total_num_items_fetched_from_ext_storage", 0) or 0)
            dvh = int(r.get("dram_value_hits", 0) or 0)
            cro = int(r.get("completion_read_ok", 0) or 0)
            fbk = int(r.get("kbc_fetching_block", 0) or 0)
            r["spilled_delta"] = sp - prev_sp if prev_sp is not None else 0
            r["fetched_delta"] = ft - prev_ft if prev_ft is not None else 0   # disk fetches this window
            dvh_d = dvh - prev_dvh if prev_dvh is not None else 0
            cro_d = cro - prev_cro if prev_cro is not None else 0
            fbk_d = fbk - prev_fbk if prev_fbk is not None else 0
            r["dram_hits_delta"] = dvh_d - cro_d - fbk_d
            r["dram_hit_pct"] = f"{(dvh_d - cro_d - fbk_d) / dvh_d * 100:.1f}" if dvh_d > 0 else ""
            prev_sp, prev_ft, prev_dvh, prev_cro, prev_fbk = sp, ft, dvh, cro, fbk
        cols = ["timestamp", "used_memory", "used_memory_rss", "ops_per_sec", "total_commands_delta",
                "total_num_items_spilled_to_ext_storage", "spilled_delta",
                "total_num_items_fetched_from_ext_storage", "fetched_delta",
                "dram_value_hits", "completion_read_ok", "kbc_fetching_block",
                "dram_hits_delta", "dram_hit_pct",
                "num_items_spilling_to_ext_storage",
                "spill_submitted_count", "spill_serialized_count", "mean_spill_ram", "inflight_spill_ram_bytes",
                "keyspace_hits", "keyspace_misses",
                "throttle_total_throttled", "throttle_queued_clients", "throttle_current_rate", "throttle_allowed_tps",
                "blocked_clients", "valkey_cpu_user", "valkey_cpu_sys", "asio_cpu_pct", "cpu_user", "cpu_sys",
                "disk_hit_pct", "mem_frag_ratio",
                "disk_read_iops", "disk_write_iops", "disk_read_mb", "disk_write_mb",
                "disk_read_merges_ps", "disk_write_merges_ps", "disk_r_await_ms", "disk_w_await_ms",
                "disk_aqu_sz", "disk_util_pct", "disk_in_flight", "disk_req_sz_kb"]
        # Filter to cols that exist
        cols = [c for c in cols if c in metrics_rows[0]]
        lines.append("### Metrics\n")
        hdr = {"dram_hits_delta": "dram_hits", "dram_hit_pct": "DRAM hit %", "disk_hit_pct": "Disk hit %", "asio_cpu_pct": "ASIO CPU %", "mem_frag_ratio": "Frag ratio"}
        lines.append("| " + " | ".join(hdr.get(c, c) for c in cols) + " |")
        lines.append("| " + " | ".join(["---"] * len(cols)) + " |")
        for row in metrics_rows:
            vals = []
            for c in cols:
                v = row.get(c, "")
                if c == "used_memory" and v:
                    v = f"{int(v)//1048576}MB"
                elif c == "used_memory_rss" and v:
                    v = f"{int(v)//1048576}MB"
                vals.append(str(v))
            lines.append("| " + " | ".join(vals) + " |")
        lines.append("")
    return "\n".join(lines)

def scenario_html(name, trace_results, metrics_rows):
    html = [f"<h2>{name}</h2>"]
    if trace_results:
        tr = trace_results
        html.append("<table><tr><th>Metric</th><th>Value</th></tr>")
        if "tps" in tr: html.append(f"<tr><td>Throughput</td><td><b>{tr['tps']:.0f} ops/s</b></td></tr>")
        if "p50" in tr: html.append(f"<tr><td>p50</td><td>{tr['p50']:.2f} ms</td></tr>")
        if "p99" in tr: html.append(f"<tr><td>p99</td><td>{tr['p99']:.2f} ms</td></tr>")
        if "p99.9" in tr: html.append(f"<tr><td>p99.9</td><td>{tr['p99.9']:.2f} ms</td></tr>")
        if "hit_ratio" in tr: html.append(f"<tr><td>Hit ratio</td><td>{tr['hit_ratio']:.1f}%</td></tr>")
        if "ops" in tr: html.append(f"<tr><td>Ops</td><td>{tr['ops']:,}</td></tr>")
        if "duration_s" in tr: html.append(f"<tr><td>Duration</td><td>{tr['duration_s']:.1f}s</td></tr>")
        html.append("</table>")
    if metrics_rows and len(metrics_rows) > 1:
        # Chart for used_memory over time
        labels = [r.get("timestamp", str(i)) for i, r in enumerate(metrics_rows)]
        mem_data = [int(r.get("used_memory", 0))//(1024*1024) for r in metrics_rows]
        html.append(f'<div class="chart-box"><canvas id="chart_{name}"></canvas></div>')
        html.append(f"""<script>
new Chart(document.getElementById('chart_{name}'), {{
  type: 'line',
  data: {{ labels: {json.dumps(labels[-60:])}, datasets: [{{
    label: 'used_memory (MB)', data: {json.dumps(mem_data[-60:])},
    borderColor: '#4a90d9', fill: false
  }}] }},
  options: {{ responsive: true, plugins: {{ title: {{ display: true, text: '{name} - Memory' }} }} }}
}});
</script>""")
        # TPS chart (ops_per_sec + total_commands_delta)
        ops_ema = [int(r.get("ops_per_sec", 0)) for r in metrics_rows]
        ops_delta = [int(r.get("total_commands_delta", 0)) for r in metrics_rows]
        if any(v > 0 for v in ops_ema + ops_delta):
            html.append(f'<div class="chart-box"><canvas id="tps_{name}"></canvas></div>')
            html.append(f"""<script>
new Chart(document.getElementById('tps_{name}'), {{
  type: 'line',
  data: {{ labels: {json.dumps(labels[-60:])}, datasets: [{{
    label: 'ops/sec (EMA)', data: {json.dumps(ops_ema[-60:])},
    borderColor: '#e67e22', fill: false
  }}, {{
    label: 'ops/sec (delta)', data: {json.dumps(ops_delta[-60:])},
    borderColor: '#27ae60', fill: false, borderDash: [5,3]
  }}] }},
  options: {{ responsive: true, plugins: {{ title: {{ display: true, text: '{name} - Throughput (ops/sec)' }} }} }}
}});
</script>""")
    return "\n".join(html)

def process_scenario(scenario_dir, results_root=None):
    # Derive name as relative path from results root
    if results_root:
        name = os.path.relpath(scenario_dir, results_root)
    else:
        parent = os.path.basename(os.path.dirname(scenario_dir))
        base = os.path.basename(scenario_dir)
        if parent and parent not in ("results",) and not parent.startswith("20"):
            name = f"{parent}/{base}"
        else:
            name = base
    trace_results = None
    metrics_rows = []
    server_latency = None

    # Check for sweep-point subdirectories (C1 client scaling)
    sweep_dirs = sorted(glob.glob(os.path.join(scenario_dir, "clients-*")),
                        key=lambda d: int(os.path.basename(d).split("-")[1]))
    if sweep_dirs:
        config_text = read_scenario_config(scenario_dir)
        md_lines = []
        full_name = f"{name} - {SCENARIO_NAMES[name]}" if name in SCENARIO_NAMES else name
        md_lines.append(f"## {full_name}\n")
        if config_text:
            md_lines.append("### Configuration\n")
            md_lines.append("| Parameter | Value |")
            md_lines.append("|-----------|-------|")
            for line in config_text.strip().split('\n'):
                line = line.strip()
                if line and not line.startswith('#') and '=' in line:
                    k, v = line.split('=', 1)
                    md_lines.append(f"| {k} | {v} |")
            md_lines.append("")

        # Summary table
        md_lines.append("### Throughput by Client Count\n")
        md_lines.append("| Clients | GET TPS | SET TPS | Total TPS | GET p50 | SET p50 |")
        md_lines.append("|---------|---------|---------|-----------|---------|---------|")

        sweep_summaries = []
        for sd in sweep_dirs:
            label = os.path.basename(sd)  # clients-50
            c_count = label.split("-")[1]
            output = os.path.join(sd, "output.txt")
            tr = None
            if os.path.exists(output):
                tr = parse_valkey_benchmark(output)
                if not tr:
                    tr = parse_trace_replay(output)
            # Enrich with hit ratio
            fi = os.path.join(sd, "final-info.txt")
            if tr and "commands" in tr and os.path.exists(fi):
                info_text = Path(fi).read_text()
                hits = re.search(r'keyspace_hits:(\d+)', info_text)
                misses = re.search(r'keyspace_misses:(\d+)', info_text)
                if hits and misses:
                    h, m = int(hits.group(1)), int(misses.group(1))
                    if h + m > 0:
                        tr["hit_ratio"] = h / (h + m) * 100
            sweep_summaries.append((c_count, sd, tr))

            if tr and "commands" in tr:
                get_rps = tr["commands"].get("GET", {}).get("rps", 0)
                set_rps = tr["commands"].get("SET", {}).get("rps", 0)
                get_p50 = tr["commands"].get("GET", {}).get("p50_ms", 0)
                set_p50 = tr["commands"].get("SET", {}).get("p50_ms", 0)
                md_lines.append(f"| {c_count} | {get_rps:,.0f} | {set_rps:,.0f} | {get_rps+set_rps:,.0f} | {get_p50:.3f}ms | {set_p50:.3f}ms |")
            elif tr and "tps" in tr:
                md_lines.append(f"| {c_count} | {tr['tps']:,.0f} | - | {tr['tps']:,.0f} | {tr.get('p50', 0):.3f}ms | - |")

        md_lines.append("")

        # Per-sweep-point details
        for c_count, sd, tr in sweep_summaries:
            md_lines.append(f"### Clients = {c_count}\n")
            fi = os.path.join(sd, "final-info.txt")
            lat = parse_server_latency(fi)
            if lat:
                md_lines.append("**Server-Side Latency (µs)**\n")
                md_lines.append("| Command | p50 | p99 | p99.9 | p100 |")
                md_lines.append("|---------|-----|-----|-------|------|")
                for cmd, vals in sorted(lat.items()):
                    md_lines.append(f"| {cmd.upper()} | {vals.get('p50', '')} | {vals.get('p99', '')} | {vals.get('p99.9', '')} | {vals.get('p100', '')} |")
                md_lines.append("")
            csvs = glob.glob(os.path.join(sd, "*.csv"))
            if csvs:
                rows = []
                for c in csvs:
                    rows.extend(parse_metrics_csv(c))
                if rows:
                    cols = ["timestamp", "used_memory", "used_memory_rss", "ops_per_sec", "total_commands_delta",
                            "total_num_items_spilled_to_ext_storage", "total_num_items_fetched_from_ext_storage",
                            "completion_read_ok", "num_items_spilling_to_ext_storage",
                            "keyspace_hits", "keyspace_misses",
                            "blocked_clients", "valkey_cpu_user", "valkey_cpu_sys", "valkey_cpu_total",
                            "cpu_user", "cpu_sys", "disk_read_iops", "disk_write_iops"]
                    cols = [c for c in cols if c in rows[0]]
                    md_lines.append("**Metrics**\n")
                    md_lines.append("| " + " | ".join(cols) + " |")
                    md_lines.append("| " + " | ".join(["---"] * len(cols)) + " |")
                    for row in rows:
                        vals = []
                        for col in cols:
                            v = row.get(col, "")
                            if col in ("used_memory", "used_memory_rss") and v:
                                v = f"{int(v)//1048576}MB"
                            vals.append(str(v))
                        md_lines.append("| " + " | ".join(vals) + " |")
                    md_lines.append("")

        md = "\n".join(md_lines)
        Path(os.path.join(scenario_dir, "report.md")).write_text(md)
        # Simple HTML
        html = HTML_TEMPLATE.replace("{{TITLE}}", f"Benchmark: {name}").replace("{{BODY}}", f"<pre>{md}</pre>")
        Path(os.path.join(scenario_dir, "report.html")).write_text(html)
        # Return first sweep's results for aggregate summary table
        first_tr = sweep_summaries[0][2] if sweep_summaries else None
        return name, first_tr, [], None, config_text, md

    output = os.path.join(scenario_dir, "output.txt")
    if os.path.exists(output):
        trace_results = parse_trace_replay(output)
        if not trace_results.get("tps"):
            bench_results = parse_valkey_benchmark(output)
            if bench_results:
                trace_results = bench_results

    final_info = os.path.join(scenario_dir, "final-info.txt")
    server_latency = parse_server_latency(final_info)

    # Parse client-side latency CSV (from compound scenarios)
    client_latency = {}
    client_lat_csv = os.path.join(scenario_dir, "client-latency.csv")
    if os.path.exists(client_lat_csv):
        with open(client_lat_csv) as f:
            reader = csv.DictReader(f)
            for row in reader:
                cmd = row.get("command", "")
                if cmd:
                    client_latency[cmd] = {
                        "rps": float(row.get("rps", 0)),
                        "p50": float(row.get("p50_ms", 0)),
                        "p95": float(row.get("p95_ms", 0)),
                        "p99": float(row.get("p99_ms", 0)),
                        "p100": float(row.get("p100_ms", 0)),
                    }

    # Enrich valkey-benchmark results with hit ratio from final-info
    if trace_results and "commands" in trace_results and os.path.exists(final_info):
        info_text = Path(final_info).read_text()
        hits = re.search(r'keyspace_hits:(\d+)', info_text)
        misses = re.search(r'keyspace_misses:(\d+)', info_text)
        if hits and misses:
            h, m = int(hits.group(1)), int(misses.group(1))
            if h + m > 0:
                trace_results["hit_ratio"] = h / (h + m) * 100
                trace_results["ops"] = h + m

    csvs = [c for c in glob.glob(os.path.join(scenario_dir, "*.csv"))
            if os.path.basename(c) != "client-latency.csv"]
    for c in csvs:
        metrics_rows.extend(parse_metrics_csv(c))

    # Write per-scenario reports
    config_text = read_scenario_config(scenario_dir)
    outcome = parse_outcome(final_info, config_text)
    if outcome:
        if trace_results is None:
            trace_results = {}
        trace_results["outcome"] = outcome
    md = scenario_markdown(name, trace_results, metrics_rows, server_latency, config_text, client_latency)
    Path(os.path.join(scenario_dir, "report.md")).write_text(md)

    html_body = scenario_html(name, trace_results, metrics_rows)
    html = HTML_TEMPLATE.replace("{{TITLE}}", f"Benchmark: {name}").replace("{{BODY}}", html_body)
    Path(os.path.join(scenario_dir, "report.html")).write_text(html)

    return name, trace_results, metrics_rows, server_latency, config_text, None

HTML_TEMPLATE = """<!DOCTYPE html>
<html><head><meta charset="utf-8"><title>{{TITLE}}</title>
<script src="https://cdn.jsdelivr.net/npm/chart.js"></script>
<style>
body{font-family:system-ui;margin:2em;background:#fafafa;color:#333}
table{border-collapse:collapse;margin:1em 0}
th,td{border:1px solid #ddd;padding:6px 12px;text-align:right}
th{background:#f0f0f0;text-align:left}
td:first-child{text-align:left}
.chart-box{width:100%;max-width:800px;margin:1em 0}
h1{color:#2c3e50} h2{color:#34495e;border-bottom:1px solid #eee;padding-bottom:4px}
b{color:#2980b9}
</style></head><body>
<h1>{{TITLE}}</h1>
{{BODY}}
</body></html>"""

def main():
    if len(sys.argv) < 2:
        print("Usage: generate-report.py RESULTS_DIR", file=sys.stderr)
        sys.exit(1)

    results_dir = sys.argv[1]

    # Find scenario subdirectories (contain output.txt or metrics.csv)
    # Supports flat, config-nested, and sweep-nested layouts via recursive glob
    scenarios = []
    for output_path in sorted(Path(results_dir).rglob("output.txt")):
        scenarios.append(str(output_path.parent))

    if not scenarios:
        # Single scenario directory
        scenarios = [results_dir]

    all_results = []
    for s in scenarios:
        name, tr, metrics, latency, cfg, prebuilt_md = process_scenario(s, results_root=results_dir)
        all_results.append((name, tr, metrics, latency, cfg, prebuilt_md))
        print(f"  {name}: report.html + report.md")

    # Aggregate report (if multiple scenarios)
    if len(all_results) > 1 or scenarios[0] != results_dir:
        # Markdown aggregate
        md_lines = ["# Benchmark Run\n"]
        md_lines.append("| Scenario | TPS | Hit Ratio | GET p50 (µs) | GET p99 (µs) | GET p99.9 (µs) |")
        md_lines.append("|----------|-----|-----------|-------------|-------------|---------------|")
        for name, tr, _, lat, _, _ in all_results:
            tps = "ABORTED (OOM)" if (tr and tr.get("outcome")) else (f"{tr.get('tps', 0):,.0f}" if tr else "")
            hr = f"{tr.get('hit_ratio', 0):.1f}%" if tr else ""
            gp50 = lat.get("get", {}).get("p50", "") if lat else ""
            gp99 = lat.get("get", {}).get("p99", "") if lat else ""
            gp999 = lat.get("get", {}).get("p99.9", "") if lat else ""
            md_lines.append(f"| {name} | {tps} | {hr} | {gp50} | {gp99} | {gp999} |")
        md_lines.append("")
        for name, tr, metrics, latency, cfg, prebuilt_md in all_results:
            if prebuilt_md:
                md_lines.append(prebuilt_md)
            else:
                md_lines.append(scenario_markdown(name, tr, metrics, latency, cfg))
        Path(os.path.join(results_dir, "report.md")).write_text("\n".join(md_lines))

        # HTML aggregate
        html_body = "<h2>Summary</h2>\n<table><tr><th>Scenario</th><th>TPS</th><th>Hit Ratio</th><th>GET p50 (µs)</th><th>GET p99 (µs)</th><th>SET p50 (µs)</th><th>SET p99 (µs)</th></tr>\n"
        for name, tr, _, lat, _, _ in all_results:
            tps = "<b>ABORTED (OOM)</b>" if (tr and tr.get("outcome")) else (f"<b>{tr.get('tps', 0):,.0f}</b>" if tr else "")
            hr = f"{tr.get('hit_ratio', 0):.1f}%" if tr else ""
            gp50 = lat.get("get", {}).get("p50", "") if lat else ""
            gp99 = lat.get("get", {}).get("p99", "") if lat else ""
            sp50 = lat.get("set", {}).get("p50", "") if lat else ""
            sp99 = lat.get("set", {}).get("p99", "") if lat else ""
            html_body += f"<tr><td>{name}</td><td>{tps}</td><td>{hr}</td><td>{gp50}</td><td>{gp99}</td><td>{sp50}</td><td>{sp99}</td></tr>\n"
        html_body += "</table>\n"
        for name, tr, metrics, _, _, _ in all_results:
            html_body += scenario_html(name, tr, metrics)
        html = HTML_TEMPLATE.replace("{{TITLE}}", "Benchmark Run").replace("{{BODY}}", html_body)
        Path(os.path.join(results_dir, "report.html")).write_text(html)
        print(f"\nAggregate: {results_dir}/report.html + report.md")

if __name__ == "__main__":
    main()
