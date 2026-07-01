# Scenarios

Scenarios are run via `benchmark.sh`, not directly. Each scenario lives in a named subdirectory with a `configs/` folder containing parameterization files.

## Architecture

```
scenarios/
├── lib.sh                          # Shared helpers (cli, run_trace_replay, ...)
├── mixed-rw/
│   ├── run.sh                      # populate (sequential, full) + parallel GET/SET
│   └── configs/
│       ├── uniform.env             # ACCESS_PATTERN=uniform, baseline
│       ├── zipfian.env             # ACCESS_PATTERN=zipfian, baseline
│       ├── uniform-flashcache.env  # uniform + tiering
│       ├── zipfian-flashcache.env  # zipfian + tiering
│       ├── compound.env            # SWEEP_DATATYPE (all compound types, baseline)
│       └── compound-flashcache.env # SWEEP_DATATYPE (compound + tiering)
└── tiering-latency/
    ├── run.sh
    └── configs/ (idle, slam, idle-hash, idle-notier, idle-hash-notier, slam-notier)
```

## How It Works

1. `benchmark.sh` dispatches by folder name (e.g. `mixed-rw`, `tiering-latency`)
2. Sources `scenarios/<folder>/configs/<config>.env`
3. If any `SWEEP_*` variables are defined, generates a cartesian product and runs once per combination
4. Starts valkey-server with the config's MAXMEMORY and MAXMEMORY_POLICY
5. Invokes `scenarios/<folder>/run.sh` with the env contract (every scenario is self-contained in its `run.sh`; `benchmark.sh` holds no per-scenario logic)
6. Collects metrics, generates report

## Config Format

Each `.env` file defines the scenario parameters as shell variables:

```bash
# scenarios/mixed-rw/configs/uniform.env
MAXMEMORY=0
MAXMEMORY_POLICY=noeviction
CLIENTS=200
OPS=10000000
KEYSPACE=500000
DATASIZE=400
READ_PCT=80
ACCESS_PATTERN=uniform
```

To create a variant, add another file in `configs/`:
```bash
# scenarios/mixed-rw/configs/zipfian-flashcache.env
MAXMEMORY=32mb
MAXMEMORY_POLICY=allkeys-lru
CLIENTS=200
OPS=10000000
KEYSPACE=500000
DATASIZE=400
READ_PCT=80
ACCESS_PATTERN=zipfian
ZIPFIAN_ALPHA=1.0
```

Select configs via CLI:
```bash
./benchmark.sh --config uniform,zipfian mixed-rw   # run both access patterns in sequence
```

## Sweeps

Any config can sweep one or more variables by adding `SWEEP_<NAME>` entries. The scenario is re-run once per value (or once per combination for multi-variable sweeps).

### Single variable sweep
```bash
# Run mixed-rw at 6 different client counts
SWEEP_CLIENTS="1 50 100 200 500 1000"
```
Results: `results/<tag>/mixed-rw/<config>/clients-1/`, `clients-50/`, etc.

### Multi-variable sweep (cartesian product)
```bash
# 3 client counts × 2 keyspaces = 6 runs
SWEEP_CLIENTS="1 100 1000"
SWEEP_KEYSPACE="100000 500000"
```
Results: `results/<tag>/mixed-rw/<config>/clients-1/keyspace-100000/`, etc.

### Rules
- `SWEEP_<NAME>` sets the variable `<NAME>` for each iteration
- The base value of `<NAME>` in the config is ignored during sweeps
- Server is restarted between each sweep iteration
- Each iteration gets its own metrics.csv, output.txt, and final-info.txt
- Any variable can be swept (CLIENTS, KEYSPACE, DATASIZE, OPS, MAXMEMORY, etc.)

## Scenarios

### mixed-rw

**What:** Synthetic mixed GET/SET workload via `valkey-benchmark` over a fixed keyspace. Supports both string and compound data types via the `DATATYPE` config variable (default: string). Access pattern is config-driven (`ACCESS_PATTERN=uniform|zipfian`).

**Phases:**
1. **Sequential full populate** — every key `0..KEYSPACE-1` is written exactly once (`valkey-benchmark --sequential`) with no memory cap, so all keys exist before the measured phase.
2. Lower `maxmemory` to the config value, then run parallel GET + SET at `READ_PCT` using the configured access pattern.

**Configs:** `uniform`, `zipfian`, `uniform-flashcache`, `zipfian-flashcache`, `compound`, `compound-flashcache`.

**Config parameters (strings):**
| Parameter | Default | Description |
|-----------|---------|-------------|
| ACCESS_PATTERN | uniform | `uniform` (random) or `zipfian` (hot-set) |
| ZIPFIAN_ALPHA | 1.0 | Zipfian skewness (higher = more concentrated) |
| MAXMEMORY | 0 / cap | Memory limit (0 = baseline; flashcache configs cap to force spilling) |
| MAXMEMORY_POLICY | noeviction / allkeys-lru | Eviction policy |
| CLIENTS | 200 | Total clients (split by READ_PCT) |
| OPS | 10000000 | Total operations |
| KEYSPACE | 500000 | Number of unique keys |
| DATASIZE | 400 | Value size in bytes (alias: `ITEM_SIZE`) |
| KEYSIZE | 100 | Key size in bytes |
| READ_PCT | 80 | Percentage of ops that are GETs |
| SCAN_PCT | 0 | Percentage of clients running concurrent `--sequential` GET (scan disruption) |

**Config parameters (compound types):**
| Parameter | Default | Description |
|-----------|---------|-------------|
| DATATYPE | hash | Data type: hash/list/set/zset/stream |
| KEYSPACE | 1000 | Number of unique keys |
| ITEMS_PER_KEY | 10 | Fields/elements per key |
| ITEM_SIZE | 100 | Size per item in bytes |
| READ_PCT | 80 | Read percentage |
| OPS | 1000000 | Total ops |
| CLIENTS | 200 | Concurrent clients |
| ACCESS_PATTERN | uniform | uniform or zipfian |

**What it reveals:** Throughput under mixed read/write pressure for a given access skew and data type. For tiering, set `MAXMEMORY` below the working set so cold keys spill while the hot set stays resident.

---

### tiering-latency

**What:** Pure storage-layer read-latency probe. Populates synthetic keys, spills them all to flash with `DEBUG SPILL`, then GETs every spilled key at a target TPS. Records `p50/p99/p99.9/p100` from both **client** and **server** perspectives.

**Config parameters:**
| Parameter | Default | Description |
|-----------|---------|-------------|
| KEY_COUNT | — | Number of synthetic keys to populate + spill |
| KEY_SIZE | — | Key length in bytes (padded with `_`) |
| VALUE_SIZE | — | Value size in bytes |
| RPS | — | Read pace; `0` = unbounded |
| CLIENTS | — | Concurrent read clients |
| SPILL | yes | `yes` = tiering; `no` = RAM control |
| MAXMEMORY | — | **Must be > 0** |
| SERVER_EXTRA_ARGS | — | Must include `--enable-debug-command yes` |

**Configs:** `idle`, `slam`, `idle-hash`, `idle-notier`, `idle-hash-notier`, `slam-notier`

**What it reveals:** Cold flash-fetch latency (idle) vs saturated storage-layer latency (slam). `SPILL=no` controls isolate preemption tail from tiering fetch-path cost.

---

## Adding a New Scenario

1. Create `scenarios/<name>/run.sh` (self-contained; source `../lib.sh`, read the config, run the workload, write `output.txt`). Use an existing one as a template (`mixed-rw/run.sh`).
2. Create `scenarios/<name>/configs/default.env` (and any variants).
3. Document here.
