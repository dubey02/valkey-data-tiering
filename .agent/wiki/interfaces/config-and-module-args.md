---
title: Config & Module Args
status: active
sources:
  - src/config.c:3369-3373
  - src/config.c:3482-3495
  - src/config.c:2611-2635
  - src/config.c:3415
  - src/config.c:467
  - src/config.c:776-800
  - src/config.c:920-935
  - src/ext_storage.h:42-44
  - src/ext_storage.h:71-84
  - src/ext_storage.h:101-114
  - src/ext_storage.c:93-114
  - src/ext_storage.c:366-381
  - src/ext_storage_bridge.c:88-108
  - src/storage/storage.h:59-75
  - src/debug.c:1088-1096
  - src/debug.c:1097-1143
  - .agent/knowledge/config-compatibility.md
updated: 2026-07-30
type: interface
tier: working
claim_count: 10
edges:
  - to: components/backends.md
    kind: refers_to
    source: human
    created: 2026-06-03
  - to: components/backends.md
    kind: configures
    source: human
    created: 2026-06-03
    note: backend, db_path, capacity, FlashCache tuning
  - to: components/throttle-equilibrium.md
    kind: configures
    source: human
    created: 2026-06-03
    note: throttle thresholds
  - to: components/engine-integration.md
    kind: configures
    source: human
    created: 2026-06-03
    note: ext-storage-enabled, maxmemory
  - to: components/eviction-integration.md
    kind: configures
    source: human
    created: 2026-07-30
    note: maxmemory-policy guard, spilling strategy, spillover batch size
  - to: interfaces/storagetype-vtable.md
    kind: refers_to
    source: llm_relation
    created: 2026-06-04
  - to: interfaces/bridge-api.md
    kind: refers_to
    source: llm_relation
    created: 2026-06-04
  - to: interfaces/ext-storage-api.md
    kind: refers_to
    source: llm_relation
    created: 2026-06-04
  - to: interfaces/info-metrics.md
    kind: refers_to
    source: llm_relation
    created: 2026-06-04
  - to: flows/spill.md
    kind: refers_to
    source: llm_relation
    created: 2026-06-04
  - to: components/testing.md
    kind: refers_to
    source: human
    created: 2026-07-30
    note: ext-storage-* config coverage suites
---

# Config & Module Args

> The `ext-storage-*` server config directives, registered in `config.c`, plus the two
> non-`ext-storage-*` knobs tiering constrains (`maxmemory-policy`, `maxmemory`) and the
> tiering `DEBUG` subcommands. Each directive binds to a global declared in
> [ext_storage API](ext-storage-api.md) (`src/ext_storage.h:42-44`, `src/ext_storage.h:71-84`, `src/ext_storage.h:101-114`) and
> defined in `src/ext_storage.c:93-114`.

## Directives

Registration is split across `config.c` by value type, not grouped by prefix — bool at
`src/config.c:3369`, strings at `src/config.c:3372-3373`, the rest in one block at
`src/config.c:3482-3495`.

| Directive | Line | Type / mutability | Bound var | Default | Range |
|-----------|------|-------------------|-----------|---------|-------|
| `ext-storage-enabled` | 3369 | bool, IMMUTABLE | `ext_data_enabled` | `0` (off) | — |
| `ext-storage-backend` | 3372 | string, IMMUTABLE, allow-empty | `ext_storage_backend` | `""` | backend name |
| `ext-storage-path` | 3373 | string, IMMUTABLE, allow-empty | `ext_storage_path` | `""` | device/dir/URI |
| `ext-storage-items-spillover-batch-size` | 3482 | int, MODIFIABLE | `items_spillover_batch_size` | `10` | 1–100 |
| `ext-storage-capacity-mb` | 3483 | longlong, IMMUTABLE, MEMORY | `ext_storage_capacity_mb` | `1024` | 64–4194304 |
| `ext-storage-max-spill-size` | 3484 | longlong, MODIFIABLE, MEMORY | `ext_storage_max_spill_size` | `128MB` | 0–LLONG_MAX |
| `ext-storage-index-size` | 3485 | longlong, IMMUTABLE | `ext_storage_index_size` | `1048576` (1M entries/DB) | 1024–LLONG_MAX |
| `ext-storage-max-allocated-percent` | 3486 | int, IMMUTABLE | `ext_storage_max_allocated_percent` | `90` | 50–100 |
| `ext-storage-max-in-flight-reads` | 3487 | int, IMMUTABLE | `ext_storage_max_in_flight_reads` | `128` | 1–4096 |
| `ext-storage-min-gc-rate` | 3488 | longlong, MODIFIABLE, MEMORY, apply | `ext_storage_min_gc_rate` | `4096` (4KB/s) | 4096–1073741824 |
| `ext-storage-max-gc-rate` | 3489 | longlong, MODIFIABLE, MEMORY, apply | `ext_storage_max_gc_rate` | `30MB` (/s) | 4096–1073741824 |
| `ext-storage-max-buffered-write-size` | 3490 | longlong, MODIFIABLE, MEMORY, apply | `ext_storage_max_buffered_write_size` | `4MB` | 0–LLONG_MAX |
| `ext-storage-buffered-write-flush-threshold` | 3491 | longlong, MODIFIABLE, MEMORY, apply | `ext_storage_buffered_write_flush_threshold` | `1MB` | 0–LLONG_MAX |
| `ext-storage-throttling-strategy` | 3492 | enum, MODIFIABLE | `ext_storage_throttling_strategy` | `v2` | `v1` \| `v2` |
| `ext-storage-spilling-strategy` | 3493 | enum, MODIFIABLE | `ext_storage_spilling_strategy` | `v2` | `v1` \| `v2` |
| `ext-storage-throttle-band-start` | 3494 | int, MODIFIABLE | `ext_storage_throttle_band_start` | `100` (1.0×) | 0–200 |
| `ext-storage-throttle-band-end` | 3495 | int, MODIFIABLE | `ext_storage_throttle_band_end` | `120` (1.2×) | 0–200 |

"apply" = the four FlashCache tuning knobs run `updateExtStorageFcConfig`
(`src/config.c:2611-2618`) on `CONFIG SET`, which calls `extStorageBridge_applyFcConfigs()`
to push the new value into a live backend. The other MODIFIABLE directives are read
directly by their consumers, so they take effect on the next pass with no apply hook.

Notes:

- `ext-storage-enabled` is the master switch; when `0`, `INFO` emits no external_storage
  block and the [preCommandExec gate](../components/engine-integration.md) is inert
  ([info-metrics](info-metrics.md)).
- `ext-storage-items-spillover-batch-size` caps how many items one spill pass submits, but
  **only under `ext-storage-spilling-strategy` = `v1`** (`src/ext_storage.c:1462`); the default
  `v2` (cap-less Smith-predictor) controller ignores it entirely. See
  [eviction-integration](../components/eviction-integration.md) § Two spill controllers and the
  [spill flow](../flows/spill.md).
- `ext-storage-max-spill-size` is a per-item size filter, not a rate limit: it is applied on the
  IO thread against the **post-serialization** length in `extStorageSerializeValue`
  (`src/ext_storage.c:271-277`), which aborts the spill for oversized items. `0` disables the
  filter.
- `ext-storage-throttle-band-start` / `-end` are percent-of-`maxmemory` band edges consumed by
  the [throttle](../components/throttle-equilibrium.md); `ext-storage-throttling-strategy`
  selects `adjustRateV2` (decoupled, default) vs the legacy coupled `adjustRate`
  (`src/ext_storage.h:101-104`).
- `ext-storage-capacity-mb`, `-index-size`, `-max-allocated-percent`, `-max-in-flight-reads`
  and the four GC/buffered-write knobs are marshalled into `storageConfig` by the
  [bridge](bridge-api.md) at `src/ext_storage_bridge.c:89-104` → [vtable
  open](storagetype-vtable.md).

## Constrained non-`ext-storage-*` directives

### `maxmemory-policy` — guarded while tiering is active

`maxmemory-policy` is `MODIFIABLE_CONFIG` and, since commit b36f2d1a1, carries the apply fn
`updateMaxmemoryPolicy` (registered at `src/config.c:3415`, defined at
`src/config.c:2620-2635`). Runtime `CONFIG SET` is **rejected** for `volatile-lru`,
`volatile-lfu`, `volatile-random`, `volatile-ttl` and `allkeys-random` when
`ext_data_enabled && extStorageIsInitialized()`; the value rolls back and the client gets
`-ERR CONFIG SET failed (possibly related to argument 'maxmemory-policy') - …`. The same rule
applies at **startup** through a different mechanism (`extStorage_init`,
`src/ext_storage.c:373-381`) which disables tiering instead of failing — apply fns are not run
by `loadServerConfigFromString` (`src/config.c:467`). Full behaviour, the exact refused set and
the `noeviction` caveat: [eviction-integration](../components/eviction-integration.md)
§ Policy gate.

The policy value also reaches the backend: the bridge sets
`storageConfig.eviction_enabled = (server.maxmemory_policy != MAXMEMORY_NO_EVICTION)`
(`src/ext_storage_bridge.c:94`), i.e. under `noeviction` the backend is told flash must never
delete data.

### `maxmemory`

Runtime-tunable as usual (no tiering-specific guard), but `maxmemory == 0` makes both spill
pumps no-ops (`src/ext_storage.c:1524`, `src/ext_storage.c:1536`) — DEBUG SPILL still works, natural spilling
never triggers.

## DEBUG subcommands

Both require `ext-storage-enabled`, otherwise they reply
`-ERR ext-storage-enabled is not set`.

| Subcommand | Site | Purpose |
|------------|------|---------|
| `DEBUG SPILL <key>` | `src/debug.c:1097-1143` | Force one key to spill: rejects missing keys, already-tiered keys, `INT`/`EMBSTR`/embedded-value keys (not spillable), and any key not in `ONLY_MEMORY`; then submits a PUT and sets `COPYING_TO_FLASH` |
| `DEBUG EXT-STORAGE-PAUSE-COMPLETIONS <0\|1>` | `src/debug.c:1088-1096` | **Tests only** (added by b36f2d1a1). Sets `ext_storage_debug_pause_completions` (`src/ext_storage.h:43`), which makes the completion drain return immediately (`src/ext_storage.c:1038`) — holds flash completions, and the clients blocked on them, in flight so tests can deterministically occupy in-flight windows |

> ⚠️ FLAGGED: `src/debug.c` contains a **second, unreachable `DEBUG SPILL` branch** at
> `src/debug.c:1162-1199`. The `else if` chain matches the copy at
> `src/debug.c:1097` first. The dead copy is an older version — it lacks the
> `INT`/`EMBSTR`/embedded-value rejection and builds the value robj with
> `createStringObject` (string-only) rather than mirroring the real type/encoding. Flagged, not
> fixed.

## Backend module args

The live `storageConfig` ([storageType vtable](storagetype-vtable.md),
`src/storage/storage.h:59-75`) carries no *opaque* per-backend field (no `void *` options
blob), so a backend cannot receive arbitrary options through it — backend choice is
`ext-storage-backend` and anything not modelled in the struct must come from module load args.
It does, however, carry a fixed **FlashCache tuning block**
(`src/storage/storage.h:67-74`: `index_size`, `max_allocated_percent`, `max_in_flight_reads`,
`min_gc_rate`, `max_gc_rate`, `max_buffered_write_size`, `buffered_write_flush_threshold`) fed
from the directives above, plus `eviction_enabled`. Per-backend details:
[backends](../components/backends.md).

## Reconciliation with `.agent/knowledge/config-compatibility.md`

That audit note (shipped by b36f2d1a1) agrees with the code on the policy guard — init guard
plus the runtime `updateMaxmemoryPolicy` apply-guard, value rolling back on rejection — and on
SWAPDB now being supported via the logical→physical db-id indirection. Two divergences, code
winning:

1. Its line pointer `ext_storage.c:1319+` for the `noeviction` spill early-returns is stale;
   the current sites are `src/ext_storage.c:1379`, `src/ext_storage.c:1455`, `src/ext_storage.c:1490`.
2. Its claim that under `noeviction` "writes OOM exactly like vanilla" does not hold — see the
   flagged contradiction on
   [eviction-integration](../components/eviction-integration.md) § noeviction is accepted but
   makes tiering inert.

Its `ext-storage-*` coverage claim (dedicated suites ext-storage-fc-configs 21 tests,
ext-storage-module-configs 16, ext-storage-max-spill-size 4) is about tests, not behaviour —
see [testing](../components/testing.md).

See also: [backends](../components/backends.md), [engine-integration](../components/engine-integration.md), [throttle-equilibrium](../components/throttle-equilibrium.md), [eviction-integration](../components/eviction-integration.md).
