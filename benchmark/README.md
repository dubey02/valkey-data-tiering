# Benchmark Suite

Automated benchmark framework for Valkey. Measures throughput, latency, and scaling characteristics.

## Quick Start (Local)

```bash
# 1. Build valkey-server + valkey-cli
make -j$(nproc) -C ../src

# 2. Build trace-replay
cd tools/trace-replay && go build && cd ../..

# 3. Run benchmarks
./benchmark.sh mixed-rw                # single scenario
./benchmark.sh mixed-rw tiering-latency # multiple scenarios, one results folder
./benchmark.sh --no-metrics mixed-rw    # skip metrics collection
```

Results land in `results/<timestamp>/`. Each scenario gets a subdirectory with:
- `output.txt` — trace-replay or valkey-benchmark output
- `metrics.csv` — per-second server + system metrics
- `final-info.txt` — full INFO ALL at end of run
- `report.md` / `report.html` — auto-generated report with server-side latency

## Remote (EC2)

Run benchmarks on a remote EC2 instance:

```bash
# 1. Copy benchmark.env.example → benchmark.env and fill in SSH details
cp benchmark.env.example benchmark.env
vim benchmark.env

# 2. Run with --remote flag
./benchmark.sh --remote mixed-rw tiering-latency
```

The remote mode will:
1. Read `EC2_HOST`, `EC2_USER`, `EC2_KEYPATH` from `benchmark.env`
2. Deploy valkey-server, valkey-cli, trace-replay, and scenario configs to the remote host
3. Execute the benchmark remotely via SSH
4. Fetch results back to the local `results/` directory

Required `benchmark.env` variables:
```bash
EC2_HOST=10.0.0.1
EC2_USER=ec2-user
EC2_KEYPATH=~/.ssh/my-key.pem
EC2_REMOTE_DIR=/tmp/valkey-bench   # optional, defaults to /tmp/valkey-bench
```

## ezbench (Real Hardware)

Run scenarios on real r6gd.2xlarge instances with NVMe via ezbench infrastructure:

```bash
# Run a single scenario
./benchmark.sh --ezbench --config uniform-flashcache --tag my-test mixed-rw

# Run with custom tag
./benchmark.sh --ezbench --config zipfian-flashcache --tag perf-v2 mixed-rw

# Multiple scenarios (launches separate stacks in parallel)
./benchmark.sh --ezbench --config uniform-flashcache --tag uni-run mixed-rw
./benchmark.sh --ezbench --config idle --tag lat-run tiering-latency
```

**Prerequisites:**
- `mwinit -o` (Amazon auth — run before first use)
- ezbench binary at `/apollo/env/ezBench/bin/ezbench`
- AWS account 833348497722 with ezbench-turtle-role

**What `--ezbench` does:**
1. Reads scenario config (e.g., `scenarios/mixed-rw/configs/uniform-flashcache.env`)
2. Generates ezbench package at `/tmp/ezbench-${TAG}/` (test.ini, client scripts, target setup)
3. Provisions r6gd.2xlarge target + c6g.2xlarge clients via CloudFormation
4. Deploys binaries, starts server with FlashCache on raw NVMe (`/dev/nvme1n1`)
5. Runs warmup (populate keyspace) then benchmark workload
6. Pushes metrics to CloudWatch namespace `valkey-bench-${TAG}`
7. Uploads live HTML reports to `s3://ezbench-833348497722/reports/`
8. Tears down infrastructure on completion

**Monitoring during test:**
```bash
# Progress URL (printed on launch)
http://ec2-<ip>.eu-west-1.compute.amazonaws.com/ezbench/report.html

# CloudWatch metrics (per-test namespace)
aws cloudwatch get-metric-statistics \
  --namespace "valkey-bench-my-test" \
  --metric-name ValkeyCpuPct ...

# SSH to target for live INFO
ssh ec2-user@<target-ip> "/opt/ezbench/files/redis-cli INFO all"

# Live report from S3
aws s3 cp s3://ezbench-833348497722/reports/<hostname>/report.html /tmp/
```

**Instance types:**
- Target: r6gd.2xlarge (8 vCPU, 64GB RAM, 475GB NVMe)
- Clients: 8× c6g.2xlarge (GET) + 2× c6g.2xlarge (SET)
- Controller: m6g.2xlarge

**Available flashcache configs:**
| Scenario | Config | Keyspace | Value Size | maxmemory |
|----------|--------|----------|-----------|-----------|
| mixed-rw | `uniform-flashcache` | 500K | 400B | 100MB |
| mixed-rw | `zipfian-flashcache` | 500K | 400B | 32MB |

## Available Scenarios

| ID | Name | Tools | What it measures | Requires Tiering |
|----|------|-------|-----------------|:---:|
| mixed-rw | Mixed R/W | valkey-benchmark | Throughput under configurable GET/SET ratio, access pattern, and data type. Full sequential populate so GETs always hit. | No |
| mixed-size | Mixed value sizes | valkey-benchmark | Throughput and latency when several value-size classes share one keyspace (`MIX_CLASSES="type:value_size:keysize:weight ..."`). Each class is populated under its own key prefix and driven by a parallel read/write pair. | No |
| tiering-latency | Tiering storage latency | trace-replay | Storage-layer read latency (client + server p50/p99/p99.9/p100) | Yes* |

\* `tiering-latency` has both tiering and no-tiering control configs (`SPILL=no`) for isolating the fetch-path cost from load-induced effects.

## Sweeps

Any scenario config can sweep one or more variables by adding `SWEEP_<NAME>` entries:

```bash
# Single variable sweep (e.g. in any config .env file)
MAXMEMORY=0
MAXMEMORY_POLICY=noeviction
OPS=1000000
KEYSPACE=500000
DATASIZE=400
READ_PCT=80
SWEEP_CLIENTS="1 50 100 200 500 1000"
```

```bash
# Multi-variable sweep (cartesian product: 3×2 = 6 runs)
SWEEP_CLIENTS="1 100 1000"
SWEEP_KEYSPACE="100000 500000"
```

Results are stored in nested subdirectories:
```
results/<tag>/mixed-rw/<config>/
├── clients-1/
├── clients-50/
├── clients-100/
├── clients-200/
├── clients-500/
└── clients-1000/
```

For multi-variable sweeps:
```
results/<tag>/mixed-rw/<config>/
├── clients-1/keyspace-100000/
├── clients-1/keyspace-500000/
├── clients-100/keyspace-100000/
...
```

Usage:
```bash
./benchmark.sh --config <config-with-sweep> mixed-rw
./benchmark.sh --remote --config <config-with-sweep> mixed-rw
```

## Directory Structure

```
benchmark/
├── benchmark.sh              # Main orchestrator
├── scenarios/                # Scenario configs + docs
│   ├── README.md             # Scenario architecture details
│   ├── lib.sh               # Shared helpers
│   ├── mixed-rw/             # run.sh + configs (see below)
│   ├── mixed-size/           # run.sh + configs: default
│   └── tiering-latency/      # run.sh + configs: idle, idle-hash, slam, *-notier
├── tools/
│   ├── trace-replay/         # Go binary: synthetic workload generation
│   ├── metrics-collector/    # Bash: polls INFO + system stats → CSV
│   └── generate-report/      # Python: results → HTML + Markdown
└── results/                  # Output (gitignored)
```

`mixed-rw` configs, grouped by what they are for:

| Group | Configs | Notes |
|-------|---------|-------|
| No-tiering baselines | `uniform`, `zipfian`, `zipfian-1gb-baseline`, `compound` | `MAXMEMORY=0`, `noeviction`. `compound` sweeps `DATATYPE` over hash/list/set/zset/stream. |
| FlashCache, 400–512B values | `uniform-flashcache`, `zipfian-flashcache`, `balanced-flashcache`, `zipfian-1gb`, `zipfian-1gb-ttl`, `compound-flashcache` | The main tiering set. `balanced-flashcache` is 50/50 read/write; `zipfian-1gb-ttl` adds `TTL=120` to every SET. |
| FlashCache, value-size sweeps | `size-sweep-fc`, `size-sweep-fc-large` | Sweep `ITEM_SIZE` (500B–5MB, and 500KB–5MB respectively) via `SWEEP_ITEM_SIZE`. `KEYSPACE` is re-derived per sweep point from `MAXMEMORY_MB` + `HOT_PCT`, so the hot set stays a fixed fraction of DRAM as the value size changes. |
| FlashCache, fixed value size | `fixed-fc-100b` (100 B), `fixed-fc-500k` (500 KB) | Single value size, no sweep — the two ends of the range isolated for detailed study. `-100b` derives `KEYSPACE` from `DATASET_BYTES` (1 GiB of values); `-500k` derives it from `MAXMEMORY_MB`. `-500k` does not currently converge; see the note in the file. |
| No-tiering size sweep | `size-sweep` | Same `SWEEP_ITEM_SIZE` and derivation as `size-sweep-fc`, but `MAXMEMORY_OVERRIDE=0` runs it uncapped as a baseline. |
| Local dev | `flashcache-local` | Zipfian twin of `zipfian-flashcache` backed by `/tmp/flashcache.db` instead of `/mnt/nvme`, with 10x fewer ops. Costs 2 GB of `/tmp` (the backing file is pre-allocated). |
| Module backend | `zipfian-1gb-module` | Loads `libflash_tiering_module.so` from `modules/flash-tiering` instead of the built-in backend. Neither `benchmark.sh` nor `--remote` builds or ships that .so, so the server aborts on a missing module unless you build it (`cargo build --release` in `modules/flash-tiering`) and place it at `$EC2_REMOTE_DIR` yourself. |

### Sizing constraint: maxmemory must cover the DRAM key floor

Keys stay in DRAM, so `maxmemory` has to hold the whole key set before any value can be
cached. `mixed-rw/run.sh` budgets `KEYSIZE + 64` bytes per key, so:

```
KEYSPACE * (KEYSIZE + 64)  <  MAXMEMORY
```

If that does not hold, populate can never reach `KEYSPACE`: DBSIZE climbs to whatever the
cap allows and then stops, and the OOM-resilient populate loop retries up to 100 times
without progressing. Symptom in the log is a run of identical lines:

```
[mixed-rw] Populate attempt 12: DBSIZE=226083/500000 — retrying...
[mixed-rw] Populate attempt 13: DBSIZE=226083/500000 — retrying...
```

`flashcache-local` used to violate this — `KEYSPACE=500000`, `KEYSIZE=100`, `MAXMEMORY=32mb`
needs a 78 MB key floor — and stalled at ~226K keys. Either raise `MAXMEMORY` above the
floor or lower `KEYSPACE`.

The `size-sweep*` configs avoid the problem by deriving `KEYSPACE` from `MAXMEMORY_MB` /
`DATASET_BYTES` plus `HOT_PCT` rather than hardcoding both sides.

## Config Audit

`audit-configs.sh` runs **every** config in `scenarios/*/configs/` against a remote host and
classifies each one, so a config that has silently rotted gets caught:

```bash
./audit-configs.sh                          # all configs
./audit-configs.sh --timeout 600 --tag mytag
./audit-configs.sh scenarios/mixed-rw/configs/zipfian.env   # just these
```

It is not a performance measurement. `OPS` is capped at 200k, `DURATION` at 10s and `KEY_COUNT`
at 100k on a deployed copy of each config, so each one is exercised end-to-end (parse -> server
start -> populate -> workload -> report) in bounded time. Verdicts:

| Verdict | Meaning |
|---------|---------|
| `PASS` | Ran and reported throughput. |
| `FAIL` | Config parse error, server failed to start, or no throughput produced. |
| `GRIND` | Populate never converged — ≥5 retries without reaching `KEYSPACE`. |
| `TIMEOUT` | Exceeded `--timeout`. |

Results land in `results/<tag>/`: an `audit.csv` plus one `<scenario>--<config>.log` per config.
`reclassify.sh results/<tag>` re-derives verdicts from those saved logs without re-running
anything, which is how you fix a classifier bug after a long run.

### Dashboard view

`tools/generate-audit-view/generate-audit-view.py` publishes a run as this branch's dashboard
view, in the same shape as the view on `unstable` / `policies`:

```
scenario tabs -> colour-coded per-config checkboxes -> metric sidebar -> KPI cards + charts
```

```bash
python3 tools/generate-audit-view/generate-audit-view.py results/<tag>
# -> benchmark_dashboard/view.html  +  benchmark_dashboard/data/<scenario>/
```

Tabs are the scenarios (`mixed-rw`, `mixed-size`, `tiering-latency`). Each config is a checkbox
that graphs as a line series; sweep legs are separate series. The sidebar toggles which metric
charts show, grouped Throughput / Memory / Tiering / CPU / Disk / Throttle / Spill Pipeline /
Latency. Time series come from each run's `metrics.csv`, client latency from the
`valkey-benchmark --csv` output, server latency from `INFO latencystats`.

`benchmark_dashboard/index.html` is a shell: given `?branch=NAME` it fetches
`benchmark_dashboard/view.html` from that branch and renders it. A branch publishes its results
just by carrying its own `view.html` plus `data/`, so this works against the deployed dashboard
immediately — no merge required, and the Pages branch needs no knowledge of the branch:

```
https://dubey02.github.io/valkey-data-tiering/benchmark_dashboard/index.html?branch=<branch>
```

Two constraints worth knowing before writing a view:

- The shell renders views via iframe `srcdoc`, so **relative fetches do not work**. Data must be
  fetched over absolute `raw.githubusercontent.com` URLs; the generator stamps `currentBranch`
  so a view reads its own branch's data. This also means the view only works once pushed.
- Views report load status to the shell by posting `{type:'view-status', text, cls}` to `parent`.

Generating a view **overwrites** `view.html`, which on `unstable` is the performance dashboard.
That is the intended pattern: each branch's `view.html` shows whatever that branch is about.

Configs that produced no `metrics.csv` still appear in the picker, disabled and badged with
their verdict, so a failing config stays visible instead of silently vanishing. An audit run
caps `OPS`/`DURATION`, so its series are short — the view marks any series with under 10 samples
and says so. For graphs worth reading, generate from a full-length run instead.

## Report Generation

Reports are auto-generated at the end of each run. They include:
- **Server-side latency** (µs) from INFO `latency_percentiles_usec_*`
- **Throughput** from trace-replay output
- **Per-second metrics table** (memory, CPU, disk, hit/miss counts)

To regenerate manually:
```bash
python3 tools/generate-report/generate-report.py results/<tag>/
```

## Configuration

Each scenario has a `configs/default.env` (or named variants) with parameters:
```bash
MAXMEMORY=0
MAXMEMORY_POLICY=noeviction
CLIENTS=200
OPS=1000000
```

Select a config with `--config`:
```bash
./benchmark.sh --config uniform-flashcache mixed-rw   # single config
./benchmark.sh --config uniform,zipfian mixed-rw      # run both configs in sequence
```

To sweep a variable, add `SWEEP_<NAME>` to any config file:
```bash
SWEEP_CLIENTS="1 50 100 200 500 1000"
```

See `scenarios/README.md` for per-scenario parameter documentation.

## TODOs

- [x] **Remote mode** (`--remote` flag): deploy + run on EC2 via SSH
- [x] **ezbench integration** (`--ezbench` flag): provision real hardware via CloudFormation
- [x] **Custom config CLI**: `./benchmark.sh --config uniform-flashcache mixed-rw`
- [x] **Sweep support**: `SWEEP_CLIENTS="1 50 100"` in any config for parameter sweeps
- [ ] **Scenario logic extraction**: move inline case logic from benchmark.sh into scenario run.sh scripts
- [ ] **lib.sh integration**: benchmark.sh should source scenarios/lib.sh for shared helpers
