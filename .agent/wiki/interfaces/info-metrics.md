---
title: INFO Metrics
status: active
sources:
  - src/ext_storage.c:207-208
  - src/ext_storage.c:385
  - src/ext_storage.c:1078-1168
updated: 2026-06-10
type: interface
tier: working
claim_count: 6
edges:
  - to: components/throttle-equilibrium.md
    kind: refers_to
    source: human
    created: 2026-06-03
  - to: interfaces/throttle-api.md
    kind: refers_to
    source: human
    created: 2026-06-04
  - to: components/engine-integration.md
    kind: refers_to
    source: llm_relation
    created: 2026-06-04
  - to: flows/completion-drain.md
    kind: refers_to
    source: llm_relation
    created: 2026-06-04
  - to: components/memory-accounting.md
    kind: refers_to
    source: llm_relation
    created: 2026-06-08
    note: inflight_spill_ram_bytes / projected_memory INFO fields
---

# INFO Metrics

> Exact field names emitted by `genExternalStorageInfoString()` (`ext_storage.c:1078-1168`).
> The whole block is suppressed when
> `ext_data_enabled == 0` (early return, `:1079`). Backend module metrics are appended after
> (`moduleGetExternalStorageMetrics()`, `:1161-1165`).

## Item counters & gauges (`:1081-1086`)

The three `total_num_items_*` are monotonic lifetime counters; the three `num_items_*`
are current gauges (instantaneous, not cumulative).

| Field | Backing var | Kind |
|-------|-------------|------|
| `total_num_items_spilled_to_ext_storage` | `total_items_spilled_to_ext_storage` | lifetime total |
| `total_num_items_fetched_from_ext_storage` | `total_items_fetched_from_ext_storage` | lifetime total |
| `total_num_items_deleted_from_ext_storage` | `total_items_deleted_from_ext_storage` | lifetime total |
| `num_items_spilling_to_ext_storage` | `total_items_spilling_to_ext_storage` | gauge (in-flight) |
| `num_items_fetching_from_ext_storage` | `total_items_fetching_from_ext_storage` | gauge (in-flight) |
| `num_items_on_flash` | `num_items_on_flash` | gauge (current) |

## `preCommandExec` (KBC) gate stats (`:1087-1094`)

`kbc_total_calls`, `kbc_in_memory`, `kbc_spilling_block`, `kbc_fetching_block`,
`kbc_pending_evict_block`, `kbc_confirmed_absent`, `kbc_key_may_exist_false`,
`kbc_key_may_exist_true`. These count the disposition of each command at the
[preCommandExec gate](../components/engine-integration.md) (block reason or fast-path).

## Completion-drain stats (`:1095-1101`)

`completion_batches`, `completion_read_ok`, `completion_read_miss`, `completion_write_ok`,
`completion_write_fail`, `completion_delete_ok`, `completion_pending_evict`. See
[completion-drain](../flows/completion-drain.md).

## Memory-pressure / OOM (`:1102-1104`)

`oom_reject_write_count`, `oom_reject_read_count`, `memory_hard_cap_exceeded_count`.

## Spill-loop stats (`:1105-1108`)

`spill_attempts`, `spill_submitted`, `spill_skipped_non_spillable`, `spill_skipped_null`.

## DRAM hit counter (`:1109`)

`dram_value_hits` (backing var `ext_storage.c:208`) counts reads served while the value is still
resident in RAM — incremented on a resident hit in `keyBlocksClient`/`preCommandExec`
(`ext_storage.c:385`, `:415`). The header comment gives the intended ratio
`DRAM_hit% = (Δdram_value_hits − Δcompletion_read_ok) / Δdram_value_hits` (`ext_storage.c:207`).

## Throttle (`:1110-1113`, from [throttle-api](throttle-api.md))

| Field | Source getter |
|-------|---------------|
| `throttle_total_throttled` | `extStorageThrottle_getThrottledCount()` |
| `throttle_queued_clients` | `extStorageThrottle_getQueuedClients()` |
| `throttle_current_rate` (`%.4f`) | `extStorageThrottle_getCurrentRate()` |
| `throttle_allowed_tps` (`%.1f`) | `extStorageThrottle_getAllowedTps()` |

## Smith-predictor accounting (`:1149-1153`)

Emitted in a second `sdscatprintf` block (`:1148-1158`) — the spill controller's two-stage
Smith predictor internals:

| Field | Backing | Kind |
|-------|---------|------|
| `spill_submitted_count` | `spill_submitted_count` atomic | counter — submits counted on the main thread (`:1149`) |
| `spill_serialized_count` | `spill_serialized_count` atomic | counter — serializes counted on the IO thread (`:1150`) |
| `mean_spill_ram` | `mean_spill_ram` atomic | gauge — EMA (α=1/16) of measured footprints (`:1151`) |
| `inflight_spill_ram_bytes` | `inflight_spill_ram_bytes` atomic | gauge — in-flight spill RAM (serialize→completion window) (`:1152`) |
| `projected_memory` | `extStorageProjectedMemory()` | gauge — `used_memory` minus committed-but-not-freed spill RAM (`:1153`) |

`submit_depth = spill_submitted_count − spill_serialized_count` is window-1 depth;
`used_memory − projected_memory` is the in-flight backlog / disk-saturation proxy. See
[memory-accounting](../components/memory-accounting.md).

See also: [throttle-equilibrium](../components/throttle-equilibrium.md), [engine-integration](../components/engine-integration.md).
