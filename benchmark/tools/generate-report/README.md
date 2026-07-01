# generate-report.py

Generates an HTML report with Chart.js time-series and summary tables from benchmark results.

## Usage

```bash
python3 generate-report.py RESULTS_DIR [OUTPUT.html]
```

If `OUTPUT.html` is omitted, writes `report.html` inside `RESULTS_DIR`.

## Input Directory Structure

| Pattern | Purpose |
|---------|---------|
| `*.csv` | metrics-collector.sh output (time-series charts) |
| `*.json` | Micro-bench results (rendered as summary tables) |
| `*.txt` | trace-replay output (parsed for TPS + latency percentiles) |

## Output

Single self-contained HTML file with:

- **Time-series section** — Chart.js line charts per CSV file:
  - Memory (used_memory, RSS, maxmemory)
  - Throughput (hits/misses)
  - Tiering operations (spills, fetches, respills)
  - Blocked clients
  - CPU (user%, sys%)
  - Disk IOPS (read/write)
  - Disk bandwidth (read/write MB/s)

## Dependencies

- Python 3.6+ (stdlib only, no pip packages)
- Chart.js loaded from CDN in the output HTML

## JSON Format

Accepts either:
- Array of objects: `[{"metric": "tps", "value": 120000}, ...]`
- Single object: `{"tps": 120000, "p50_ms": 0.24}`

## trace-replay TXT Format

Parses lines matching:
- `NNN ops/s` → TPS
- `p50=N.N`, `p99=N.N`, `p99.9=N.N`, `p100=N.N` → latency percentiles
