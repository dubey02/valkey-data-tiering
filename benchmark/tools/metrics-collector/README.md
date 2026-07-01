# metrics-collector.sh

Polls Valkey `INFO ALL` and system stats at a configurable interval, outputting CSV.

## Usage

```bash
./metrics-collector.sh PORT OUTFILE [INTERVAL]
```

- **PORT** — Valkey server port
- **OUTFILE** — Output CSV path
- **INTERVAL** — Polling interval in seconds (default: 1)

## Environment

- `VALKEY_CLI` — Path to valkey-cli binary (default: `valkey-cli` from PATH)

## Output Columns

| Column | Source |
|--------|--------|
| timestamp | epoch seconds |
| used_memory | INFO memory |
| used_memory_rss | INFO memory |
| maxmemory | INFO memory |
| keyspace_hits/misses | INFO stats |
| tiered_spills/fetches_requested/fetches_promoted/respills/write_through | INFO stats (tiering) |
| tiering_keys_on_disk | INFO keyspace |
| blocked_clients | INFO clients |
| cpu_user/cpu_sys | /proc/stat (system-wide %) |
| disk_read_iops/disk_write_iops | /sys/block/*/stat |
| disk_read_mb/disk_write_mb | /sys/block/*/stat |

## Notes

- Gracefully handles missing tiering stats (outputs 0).
- Auto-detects NVMe or SCSI block device for disk stats.
- Send SIGINT/SIGTERM to stop collection cleanly.
