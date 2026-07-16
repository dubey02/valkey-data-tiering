# Benchmark Tooling

Automated benchmark framework for Valkey data-tiering. This document covers the **tooling**: how to invoke benchmarks, what each tool does, and how results are structured.

For what each scenario measures and every config's knobs, see **[SCENARIOS.md](SCENARIOS.md)**.

## Quick Start (Local)

```bash
# 1. Build valkey-server + valkey-cli
make -j$(nproc) -C ../src

# 2. Build trace-replay
cd tools/trace-replay && go build && cd ../..

# 3. Run benchmarks
./benchmark.sh mixed-rw                 # single scenario, default config
./benchmark.sh mixed-rw tiering-latency # multiple scenarios, one results folder
./benchmark.sh --no-metrics mixed-rw    # skip metrics collection
```

## benchmark.sh — Main Orchestrator

`benchmark.sh` owns the server lifecycle (start/stop valkey-server, provision ext-storage backing file when tiering is enabled), starts the metrics collector, dispatches the scenario workload, and generates reports.

### Flags

| Flag | Description |
|------|-------------|
| `--remote` | Deploy to EC2 and run there (reads `benchmark.env`) |
| `--config NAME[,NAME2]` | Select scenario config(s) (default: `default`). Comma-separated configs run in sequence |
| `--tag NAME` | Results directory name (default: `YYYYMMDD-HHMMSS` timestamp) |
| `--no-metrics` | Skip metrics collection during the run |

```bash
./benchmark.sh --config uniform-flashcache mixed-rw   # single config
./benchmark.sh --config uniform,zipfian mixed-rw      # both configs in sequence
```

### Remote Mode (`--remote`)

```bash
# 1. Copy benchmark.env.example → benchmark.env and fill in SSH details
cp benchmark.env.example benchmark.env
vim benchmark.env

# 2. Run with --remote flag
./benchmark.sh --remote mixed-rw tiering-latency
```

Remote mode will:
1. Read `EC2_HOST`, `EC2_USER`, `EC2_KEYPATH` from `benchmark.env`
2. Deploy valkey-server, valkey-cli, valkey-benchmark, trace-replay, metrics-collector, generate-report, and scenario configs to the remote host
3. Execute the benchmark remotely via SSH
4. Fetch results back to the local `results/` directory

`benchmark.env` variables:
```bash
EC2_HOST=10.0.0.1
EC2_USER=ec2-user
EC2_KEYPATH=~/.ssh/my-key.pem
EC2_REMOTE_DIR=/tmp/valkey-bench   # optional, defaults to /tmp/valkey-bench
```

### Sweeps

Any scenario config can sweep one or more variables by adding `SWEEP_<NAME>` entries to the config `.env`:

```bash
# Single variable sweep
SWEEP_CLIENTS="1 50 100 200 500 1000"

# Multi-variable sweep (cartesian product: 3×2 = 6 runs)
SWEEP_CLIENTS="1 100 1000"
SWEEP_KEYSPACE="100000 500000"
```

Each sweep point runs the full scenario and stores results in nested subdirectories:

```
results/<tag>/mixed-rw/<config>/
├── clients-1/keyspace-100000/
├── clients-1/keyspace-500000/
├── clients-100/keyspace-100000/
...
```

Sweeps work in both local and `--remote` mode.

## tools/

| Tool | Language | Purpose |
|------|----------|---------|
| `tools/trace-replay/` | Go | Synthetic workload generation (paced reads, custom traces). Build with `go build` |
| `tools/metrics-collector/metrics-collector.sh` | Bash | Polls `INFO ALL` + system stats (CPU, disk) once per second → `metrics.csv`; also dumps raw INFO per tick to `info-full.log` |
| `tools/generate-report/generate-report.py` | Python | Renders `metrics.csv` + workload output into `report.md` / `report.html` |

`scenarios/lib.sh` provides shared helpers (`cli`, workload runners) sourced by each scenario's `run.sh`.

## Results Layout

Results land in `results/<tag>/<scenario>/<config>/`:

- `output.txt` — trace-replay or valkey-benchmark output
- `metrics.csv` — per-second server + system metrics
- `final-info.txt` — full `INFO ALL` at end of run
- `report.md` / `report.html` — auto-generated report with server-side latency

Reports include server-side latency (µs, from `latency_percentiles_usec_*`), throughput, and a per-second metrics table. To regenerate manually:

```bash
python3 tools/generate-report/generate-report.py results/<tag>/
```

## Directory Structure

```
benchmark/
├── benchmark.sh              # Main orchestrator
├── benchmark.env.example     # Remote mode SSH config template
├── SCENARIOS.md              # Scenario + config reference (source of truth)
├── scenarios/
│   ├── lib.sh                # Shared helpers
│   ├── mixed-rw/             # run.sh + configs/
│   ├── mixed-size/           # run.sh + configs/
│   └── tiering-latency/      # run.sh + configs/
├── tools/
│   ├── trace-replay/         # Go binary: synthetic workload generation
│   ├── metrics-collector/    # Bash: polls INFO + system stats → CSV
│   └── generate-report/      # Python: results → HTML + Markdown
└── results/                  # Output (gitignored)
```

## TODOs

- [ ] **Scenario logic extraction**: move inline case logic from benchmark.sh into scenario run.sh scripts
- [ ] **lib.sh integration**: benchmark.sh should source scenarios/lib.sh for shared helpers
