# Benchmark Scenarios Reference

Source of truth for what each scenario and config does.

## Directory Hierarchy

```
benchmark/
├── benchmark.sh                         # Top-level driver
├── scenarios/
│   ├── lib.sh                           # Shared helpers (cli, run_trace_replay, ...)
│   ├── mixed-rw/
│   │   ├── run.sh                       # Populate + parallel GET/SET workload
│   │   └── configs/*.env
│   ├── mixed-size/
│   │   ├── run.sh                       # Multi-class value-size workload
│   │   └── configs/*.env
│   └── tiering-latency/
│       ├── run.sh                       # Populate + DEBUG SPILL + paced reads
│       └── configs/*.env
└── SCENARIOS.md                         # This file
```

## How Config Selection Works

1. `benchmark.sh` accepts `--config <name>[,<name2>,...]` and a scenario folder name:
   ```bash
   ./benchmark.sh --config zipfian,zipfian-flashcache mixed-rw
   ```
2. It sources `scenarios/<scenario>/configs/<name>.env` for each named config.
3. If `--config` is omitted, the scenario's **first alphabetical** `.env` is used as default.
4. If any `SWEEP_*` variables are defined in the `.env`, a **cartesian product** of all sweep values is generated. The scenario is re-run once per combination, with results nested in subdirectories (`<var>-<value>/`).
5. `benchmark.sh` starts valkey-server with `MAXMEMORY`/`MAXMEMORY_POLICY` (and `SERVER_EXTRA_ARGS`), then invokes `scenarios/<folder>/run.sh <config-path>`.

---

## Scenario: `mixed-rw`

### What it does

Synthetic mixed GET/SET workload over a fixed keyspace using `valkey-benchmark`.

**Phases:**
1. **Populate** — sequential full-keyspace write (every key `0..KEYSPACE-1` written exactly once). OOM-resilient: retries up to 100 times until `DBSIZE >= KEYSPACE`.
2. **Lower maxmemory** to config value (forces spilling for tiering configs).
3. **Workload** — parallel GET + SET split by `READ_PCT`, using configured access pattern. Optional SCAN disruptor (`SCAN_PCT`).

**Branches:**
- `DATATYPE=string` — uses `valkey-benchmark -t get`/`-t set` with `--zipfian` or uniform.
- `DATATYPE=hash|list|set|zset|stream` — compound types via `--pipe` populate + arbitrary-command workload.

**Sizing models:**
- `MAXMEMORY_MB` (constant-maxmemory): derives `KEYSPACE` from maxmemory, overhead, per-key cost, and `HOT_PCT`. Used by size-sweep configs.
- `DATASET_BYTES`: derives `KEYSPACE` from target total value bytes.
- `HOT_PCT` alone: derives `MAXMEMORY` to fit only the hot fraction.
- `MAXMEMORY_OVERRIDE=0`: use `MAXMEMORY_MB` for keyspace derivation but run with no memory cap (baseline).

**Metrics:** `--csv` output per command, `metrics-collector.sh` captures INFO ALL per tick.

### Config Table

| Config | Purpose | ITEM_SIZE | KEYSPACE | MAXMEMORY | CLIENTS | Duration/OPS | Access | Tiering |
|--------|---------|-----------|----------|-----------|---------|--------------|--------|---------|
| `uniform` | Baseline, uniform random, no cap | 400 | 500K | 0 | 200 | 10M ops | uniform | No |
| `zipfian` | Baseline, Zipfian skew, no cap | 400 | 500K | 0 | 200 | 10M ops | zipfian α=1.0 | No |
| `uniform-flashcache` | Uniform + FC tiering, worst-case (all keys equally likely to hit disk) | 512 | 4M | 1GB | 200 | 50M ops | uniform | Yes (8GB FC) |
| `zipfian-flashcache` | Zipfian + FC tiering, small memory cap | 400 | 500K | 100MB | 200 | 10M ops | zipfian α=1.0 | Yes (2GB FC) |
| `zipfian-1gb` | Primary tiering benchmark: 1GB cap, 4M keys, hot set in RAM, cold on disk | 512 | 4M | 1GB | 200 | 50M ops | zipfian α=1.0 | Yes (8GB FC) |
| `zipfian-1gb-baseline` | Apple-to-apple baseline for `zipfian-1gb` (same workload, no tiering, unlimited RAM) | 512 | 4M | 0 | 200 | 50M ops | zipfian α=1.0 | No |
| `zipfian-1gb-module` | Same workload as `zipfian-1gb` but uses flash-tiering module instead of native backend | 512 | 4M | 1GB | 200 | 50M ops | zipfian α=1.0 | Yes (module) |
| `zipfian-1gb-ttl` | `zipfian-1gb` + TTL=120s on all keys — tests expiry + tiering interaction | 512 | 4M | 1GB | 200 | 50M ops | zipfian α=1.0 | Yes (8GB FC) |
| `balanced-flashcache` | 50/50 read/write (vs 80/20) — tests simultaneous spill+fetch pressure | 512 | 4M | 1GB | 200 | 50M ops | zipfian α=1.0 | Yes (8GB FC) |
| `flashcache-local` | Local dev variant (FC path=/tmp, smaller keyspace) | 400 | 500K | 32MB | 200 | 1M ops | — | Yes (2GB FC) |
| `compound` | Sweep all compound types (hash/list/set/zset/stream), no tiering | 100 | 1K | 0 | 200 | 1M ops | uniform | No |
| `compound-flashcache` | Sweep all compound types + FC tiering | 100 | 100K | 50MB | 200 | 1M ops | uniform | Yes (2GB FC) |
| `size-sweep` | Value-size sensitivity baseline (no tiering). Same keyspace derivation as `size-sweep-fc` for comparability. | SWEEP: 500/5K/500K/5M | derived | 0 (override) | 200 | 60s | zipfian | No |
| `size-sweep-fc` | Value-size sensitivity with FC tiering. Constant-maxmemory model (512MB). | SWEEP: 500/5K/500K/5M | derived | 512MB | 200 | 60s | zipfian | Yes (10GB FC) |
| `size-sweep-fc-large` | Large-value subset (500K+5M only), lower clients, higher HOT_PCT for faster populate | SWEEP: 500K/5M | derived | 512MB | 50 | 60s | zipfian | Yes (10GB FC) |
| `size-sweep-fc-500k` | Single 500KB leg for throttle-band iteration (no sweep). `rdbcompression=no`. | 500K | derived | 512MB | 200 | 2M ops | zipfian | Yes (10GB FC) |
| `size-sweep-fc-100b` | Diagnostic: single 100B leg to capture fetch-driven memory explosion | 100 | derived | derived | 200 | 2M ops | zipfian | Yes (2GB FC) |

---

## Scenario: `mixed-size`

### What it does

Heterogeneous value-size classes running concurrently on one server. Tests head-of-line blocking and mixed spill economics.

**Phases:**
1. **Populate** — each class populates its own key prefix (`c0:`, `c1:`, ...) using `--pipe` for compound types or `SET` for strings.
2. **Workload** — concurrent per-class GET+WRITE at weight-proportional client counts. Each class gets `OPS * weight / 100` operations.

**Config format:**
```
MIX_CLASSES="type:value_size:keysize:weight type2:size2:ks2:weight2 ..."
```
Weight is percentage of total ops/clients.

### Config Table

| Config | Purpose | MIX_CLASSES | KEYSPACE | MAXMEMORY | CLIENTS | OPS | Access | Tiering |
|--------|---------|-------------|----------|-----------|---------|-----|--------|---------|
| `default` | Smoke baseline (no tiering): 70% string-512B + 30% set-4KB | `string:512:100:70 set:4096:100:30` | 10K | 0 | 50 | 50K | uniform | No |

---

## Scenario: `tiering-latency`

### What it does

Pure storage-layer read latency probe. Isolates flash fetch cost from throughput noise.

**Phases:**
1. **Populate** synthetic keys via `trace-replay` tool.
2. **Spill** all keys to flash via `DEBUG SPILL` (or skip if `SPILL=no`).
3. **Wait** for drain to complete.
4. **Paced reads** at target `RPS` using `trace-replay -synth-read`.
5. **Capture** server-side latency percentiles from `INFO latencystats`.

**Key knobs:** `RPS=0` means unbounded (saturate the storage layer). `SPILL=no` configs are RAM controls to isolate whether tail latency is load-induced preemption vs flash fetch path.

### Config Table

| Config | Purpose | KEY_COUNT | VALUE_SIZE | RPS | CLIENTS | Tiering | Notes |
|--------|---------|-----------|------------|-----|---------|---------|-------|
| `idle` | Pure idle flash-fetch latency (1 req at a time, no contention) | 10 | 400 | 1 | 1 | Yes (2GB FC) | String keys |
| `idle-hash` | Idle flash-fetch for compound type (~1MB hash per fetch) | 10 | 1024 | 1 | 1 | Yes (2GB FC) | hash, 1000 items/key |
| `idle-notier` | RAM control for `idle` (same traffic, keys stay in memory) | 10 | 400 | 1 | 1 | No | SPILL=no |
| `idle-hash-notier` | RAM control for `idle-hash` | 10 | 1024 | 1 | 1 | No | SPILL=no, hash 1000 items |
| `slam` | Saturated: 1M keys, 200 clients, unbounded TPS — all reads hit disk | 1M | 400 | 0 | 200 | Yes (2GB FC) | maxmemory=8GB for populate |
| `slam-notier` | RAM control for `slam` (same load, keys in memory) | 1M | 400 | 0 | 200 | No | SPILL=no |

---

## Sweep Mechanics

Any config can sweep variables by defining `SWEEP_<NAME>`:

```bash
SWEEP_ITEM_SIZE="500 5000 500000 5000000"   # 4 legs
SWEEP_CLIENTS="50 200"                       # 2 legs → 8 total (cartesian)
```

- Server restarts between each combination.
- Results nest: `results/<tag>/<scenario>/<config>/<var>-<value>/[<var2>-<value2>/]`
- The base value of `<NAME>` is ignored when `SWEEP_<NAME>` is set.
