# Benchmark Suite — Agent Reference (Updated Jul 2026)

## Quick Start

```bash
cd /home/abhikkum/workspace/private-valkey-non-key-spilling/private-valkey

# Build
make -j$(nproc) -C src

# Run locally (scenario = folder name under scenarios/)
./benchmark/benchmark.sh --config zipfian-1gb mixed-rw
./benchmark/benchmark.sh --config uniform-flashcache --tag t19 mixed-rw

# Run on EC2 (reads benchmark/benchmark.env)
./benchmark/benchmark.sh --remote --config zipfian-1gb --tag t32 mixed-rw

# Multiple scenarios
./benchmark/benchmark.sh --config idle tiering-latency
```

## Command Line

```
Usage: benchmark.sh [--no-metrics] [--remote] [--ezbench] [--config NAME[,NAME2]] [--tag NAME] SCENARIO [SCENARIO...]
```

| Flag | Description |
|------|-------------|
| `--remote` | Deploy binaries to EC2, run there, fetch results (reads `benchmark.env`) |
| `--config NAME` | Config file name (without .env) from `scenarios/<SCENARIO>/configs/` |
| `--tag NAME` | Results subdirectory name |
| `--no-metrics` | Skip metrics-collector |
| `--ezbench` | Use ezbench infrastructure |

## Scenarios

| Folder | Tool | Description |
|--------|------|-------------|
| `mixed-rw` | valkey-benchmark | **Primary scenario.** Populate N keys → run concurrent GET+SET. Access pattern (uniform/zipfian), data types, TTL, pipeline depth all configurable. |
| `tiering-latency` | trace-replay | Flash read latency measurement. Populate → DEBUG SPILL → wait for drain → GET at target RPS. Reports p50/p99/p99.9/p100. |
| `mixed-size` | valkey-benchmark | Mixed value sizes in single workload (smoke test). |

## Config Files (mixed-rw)

Located at `benchmark/scenarios/mixed-rw/configs/`:

| Config | What it tests |
|--------|---------------|
| `zipfian-1gb.env` | **Standard benchmark.** Zipfian access, 1GB maxmem, 4M keys, 512B values, FlashCache |
| `uniform-flashcache.env` | Uniform access (worst-case, no hot set), same params as zipfian-1gb |
| `zipfian-1gb-ttl.env` | Zipfian + TTL=60s (tests expiry+tiering interaction) |
| `zipfian-1gb-module.env` | Module backend (currently broken) |
| `compound-flashcache.env` | All data types (hash, list, set, zset, stream) with FC |
| `size-sweep-fc.env` | Standard size sweep (512B values) |
| `fixed-fc-100b.env` | Tiny values (100B, metadata overhead dominates) |
| `fixed-fc-500k.env` | Large values (500KB, tests throttle behavior; does not converge) |
| `size-sweep-fc-large.env` | Extra large values (500KB + 5MB legs) |
| `balanced-flashcache.env` | 50/50 read/write ratio |
| `flashcache-local.env` | Local FlashCache (no EC2 needed, uses /tmp) |
| `uniform.env` / `zipfian.env` | Without FlashCache (baseline, no tiering) |
| `compound.env` | Complex types without tiering (baseline) |

## Config Files (tiering-latency)

| Config | What it tests |
|--------|---------------|
| `idle.env` | 1 RPS fetch latency (raw disk read time) |
| `idle-hash.env` | Large hash (~1MB) fetch latency |
| `idle-notier.env` | Baseline without tiering |
| `idle-hash-notier.env` | Baseline hash without tiering |
| `slam.env` | Max RPS fetch latency (p99 under pressure) |
| `slam-notier.env` | Baseline slam without tiering |

## Config Variables

| Variable | Default | Description |
|----------|---------|-------------|
| `MAXMEMORY` | — | Memory limit (e.g., `1gb`) |
| `MAXMEMORY_POLICY` | `allkeys-lru` | Eviction/spill policy |
| `CLIENTS` | `200` | Concurrent benchmark clients |
| `OPS` | `50000000` | Total operations (or use `DURATION`) |
| `DURATION` | — | Duration in seconds (alternative to OPS) |
| `KEYSPACE` | `4000000` | Unique key count |
| `DATASIZE` | `512` | Value size bytes |
| `KEYSIZE` | `100` | Key size bytes |
| `READ_PCT` | `80` | GET percentage (rest is SET) |
| `ACCESS_PATTERN` | `uniform` | `uniform` or `zipfian` |
| `ZIPFIAN_ALPHA` | `1.0` | Skewness (higher = hotter hot set) |
| `TTL` | `0` | Key TTL in seconds (0 = no expiry). Uses `SET key val EX ttl` |
| `PIPELINE` | `1` | Pipeline depth for GET/SET groups |
| `POPULATE_CLIENTS` | `50` | Clients during populate phase (use 1 for large values to avoid OOM burst) |
| `SERVER_EXTRA_ARGS` | — | Extra server flags |
| `EXT_STORAGE_BACKEND` | `flashcache` | Storage backend (`flashcache`, `flashcache-mock`, `rocksdb`) |
| `EXT_STORAGE_PATH` | — | Flash file path |
| `EXT_STORAGE_CAPACITY_MB` | `8192` | Flash capacity |

## Fixed Benchmark Parameters (standard runs)

For apple-to-apple comparison, all standard benchmarks use:
- maxmemory=1GB, keysize=100B, item_size=512B, keyspace=4M
- ops=50M, allkeys-lru, FlashCache=8GB, clients=200

## Results Structure

```
benchmark/results/<tag>/
├── <scenario>/
│   └── <config>/
│       ├── populate.txt     # Populate phase output
│       ├── get_output.txt   # GET group results (CSV)
│       ├── set_output.txt   # SET group results (CSV)
│       ├── metrics.csv      # Per-second INFO + system metrics
│       ├── final-info.txt   # INFO ALL at end
│       ├── config.env       # Copy of config used
│       ├── valkey.log       # Server log
│       ├── report.html      # Auto-generated
│       └── report.md        # Auto-generated
```

## Remote Mode

Requires `benchmark/benchmark.env`:
```bash
EC2_HOST=34.251.133.59
EC2_USER=ec2-user
EC2_KEYPATH=~/.ssh/id_rsa
```

Current EC2 instance: `i-06aee9f2b1addaa87` (r7gd.4xlarge, eu-west-1c, NVMe at /mnt/nvme).

## Key Constraints

- `allkeys-lru` required for tiering (standard eviction policy)
- `maxmemory > 0` required for ext_storage completions to drain
- `--enable-debug-command yes` required for tiering-latency (DEBUG SPILL)
- FlashCache needs pre-sized backing file: `fallocate -l 8G /mnt/nvme/flashcache.db`
- Large values (500KB+): use `POPULATE_CLIENTS=1` to avoid valkey-benchmark OOM exit
- Never throttle/chunk benchmarks — unbounded traffic IS the workload

## Metrics & Interpretation

### Key INFO metrics (from metrics.csv)
| Metric | Type | Meaning |
|--------|------|---------|
| `total_num_items_spilled_to_ext_storage` | counter | Total spills |
| `total_num_items_fetched_from_ext_storage` | counter | Total fetches |
| `num_items_spilling_to_ext_storage` | gauge | Currently in-flight spills |
| `dram_value_hits` | counter | Requests served from memory |
| `completion_read_ok` | counter | Requests served from disk |
| `throttle_current_rate` | gauge | 0.0–1.0 throttle intensity |
| `throttle_allowed_tps` | gauge | Current TPS cap |
| `instantaneous_ops_per_sec` | gauge | Server-reported TPS |

### Hit rate formulas
```
Memory hit %  = dram_value_hits / (dram_value_hits + completion_read_ok) × 100
Disk hit %    = completion_read_ok / (dram_value_hits + completion_read_ok) × 100
```
These sum to 100% (excludes keyspace_misses which are keys that don't exist at all).

### Latency
- **Client-side** (valkey-benchmark CSV): includes async disk wait for tiered keys
- **Server-side** (INFO latencystats): main-thread execution only, excludes async wait

## Benchmark History (reference runs)

| Tag | Config | TPS | Memory Hit | Disk Hit | Notes |
|-----|--------|-----|-----------|----------|-------|
| T14 | zipfian-1gb | 131K | ~75% | ~25% | Native FC, V2 Smith |
| T19 | uniform-flashcache | 82K | 55% | 45% | Worst-case (no hot set) |
| T30 | zipfian-1gb | 107K | — | — | After rebase |
| T32 | zipfian-1gb | 128K | — | — | FC retry fix, latest |
