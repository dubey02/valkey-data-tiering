# trace-replay

Replays cache access traces against a Valkey server. Supports two trace formats, configurable client concurrency, speed control, and set-on-miss for cold-start simulation.

## Build

```bash
cd benchmark/tools/trace-replay
GOARCH=arm64 go build -o trace-replay .   # for EC2 Graviton
# or
go build -o trace-replay .                # native
```

## Usage

```bash
./trace-replay [flags]

Flags:
  -host STRING        Server host (default "localhost")
  -port INT           Server port (default 6399)
  -trace PATH         Path to trace CSV file (required unless -synth-keys)
  -twitter            Use Twitter/memcached trace format (see below)
  -ops INT            Number of operations to replay (default 100000)
  -clients INT        Concurrent client count (default 50)
  -speed FLOAT        Replay speed multiplier (0 = unlimited, 1 = realtime)
  -set-on-miss        SET key on first GET miss (cold-start simulation)
  -populate           Populate all keys before replay
  -pop-only           Only populate, don't replay
  -pop-keys INT       Max keys to populate (0 = all unique in trace)
  -stream             Stream trace from disk (low memory, for large traces)
  -checkpoint INT     Print stats every N ops (default 500000)
  -scenario STRING    Scenario name for output labeling
  -json PATH          Append JSON result to file
  -pipeline INT       Pipeline batch size (default 512)
  -synth-keys INT     Populate N synthetic keys (no trace needed)
  -synth-size INT     Value size for synthetic keys (default 1024)
  -synth-prefix STR   Key prefix for synthetic populate (default "key:")
  -synth-keysize INT  Pad synthetic keys to N bytes (with '_'; 0 = no padding)
  -spill              After synth populate, DEBUG SPILL all keys (unbounded) and
                      poll until in-flight spills drain. Reports accepted/rejected.
  -synth-read         GET each synthetic key (paced by -rps), record latency.
                      Skips populate — reads the existing deterministic keyspace.
  -rps INT            Target reads/sec for -synth-read (0 = unbounded)
```

## Trace Formats

### Default format

CSV with header: `timestamp,op,key,size`

```csv
timestamp,op,key,size
0,GET,16764961422366426745,249
0,GET,5889473265574126472,1
0,SET,16764961422366426745,249
```

- `timestamp`: relative time in seconds
- `op`: GET or SET
- `key`: opaque key string
- `size`: value size in bytes

### Twitter/memcached format (`-twitter`)

CSV (no header): `timestamp,key,key_size,value_size,seq_num,op,ttl`

```csv
0,q:q:1:8WTwl7huJeQ,17,249,1,get,0
0,yDqF:vS:GKA9AK1xJxJ9nKA991,26,27,18,add,86400
0,yDqF:vq:1AJrrr1xnK1J19G199J,27,27,21,cas,86400
0,q:q:1:FyQjIMHMee,16,0,177,delete,0
```

Operations are mapped from Memcached to Valkey:

| Trace Op | Valkey Command | Semantics |
|----------|---------------|-----------|
| `get`    | `GET key` | Read |
| `gets`   | `GET key` | Read (CAS token ignored) |
| `add`    | `SET key value NX EX ttl` | Write-if-absent |
| `cas`    | `SET key value XX EX ttl` | Write-if-exists |
| `set`    | `SET key value EX ttl` | Unconditional write |
| `delete` | `DEL key` | Remove |

Typical distribution (Twitter cluster52): ~89% get, 4.5% add, 2.9% gets, 2.5% cas, 0.8% delete, <0.1% set.

## Examples

```bash
# Default format, cold-start with set-on-miss
./trace-replay -trace cluster52_1m.csv -ops 1000000 -clients 200 -speed 0 -set-on-miss

# Twitter format, full operation replay
./trace-replay -trace cluster52_1m_full.csv -twitter -ops 1000000 -clients 200 -speed 0

# Stream large trace (low memory)
./trace-replay -trace cluster52_100m.csv -twitter -stream -ops 10000000 -clients 200 -speed 0

# Tiering storage-latency (tiering-latency): populate 1M keys (100B key / 400B val), spill all, then
# read each at unbounded TPS measuring client-side latency
./trace-replay -synth-keys 1000000 -synth-size 400 -synth-keysize 100 -spill
./trace-replay -synth-keys 1000000 -synth-keysize 100 -synth-read -rps 0 -clients 200
```

## Traces

- `cluster52_1m.csv` — 1M ops, GET-only, default format
- `cluster52_1m_full.csv` — 1M ops, all operations + TTLs, twitter format
- `cluster52_10m.csv` — 10M ops, GET-only, default format
- `cluster52.sort.zst` — Full trace, 87GB compressed, twitter format

All derived from Twitter production cache cluster #52.
