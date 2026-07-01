---
title: Config & Module Args
status: active
sources:
  - src/config.c:3312-3427
  - src/ext_storage.h:38-44
updated: 2026-06-04
type: interface
tier: working
claim_count: 6
edges:
  - to: components/backends.md
    kind: refers_to
    source: human
    created: 2026-06-03
  - to: components/backends.md
    kind: configures
    source: human
    created: 2026-06-03
    note: backend, db_path, capacity
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
---

# Config & Module Args

> The `ext-storage-*` server config directives, registered in `config.c`. Each binds to a
> global in [ext_storage API](ext-storage-api.md) (`ext_storage.h:38-44`). Most are
> `IMMUTABLE_CONFIG` — set at startup, not runtime-tunable.

## Directives (`config.c:3312-3427`)

| Directive | Line | Type / mutability | Bound var | Default | Range |
|-----------|------|-------------------|-----------|---------|-------|
| `ext-storage-enabled` | 3312 | bool, IMMUTABLE | `ext_data_enabled` | `0` (off) | — |
| `ext-storage-backend` | 3315 | string, IMMUTABLE, allow-empty | `ext_storage_backend` | `""` | backend name |
| `ext-storage-path` | 3316 | string, IMMUTABLE, allow-empty | `ext_storage_path` | `""` | device/dir/URI |
| `ext-storage-items-spillover-batch-size` | 3426 | int, **MODIFIABLE** | `items_spillover_batch_size` | `10` | 1–100 |
| `ext-storage-capacity-mb` | 3427 | longlong, IMMUTABLE, MEMORY | `ext_storage_capacity_mb` | `1024` | 64–4194304 |

Notes:
- `ext-storage-enabled` is the master switch; when `0`, `INFO` emits no external_storage
  block and the [preCommandExec gate](../components/engine-integration.md) is inert ([info-metrics](info-metrics.md)).
- `ext-storage-items-spillover-batch-size` is the only runtime-tunable directive — it caps
  how many items one spill pass submits ([spill flow](../flows/spill.md)).
- `ext-storage-capacity-mb` feeds `storageConfig.capacity_bytes` via the
  [bridge](bridge-api.md) → [vtable open](storagetype-vtable.md).

## Backend module args

The live `storageConfig` ([storageType vtable](storagetype-vtable.md), `src/storage/storage.h:57-64`)
carries no opaque per-backend field — backend choice is `ext-storage-backend`, and any
backend-specific options are supplied as module load args. Per-backend details:
[backends](../components/backends.md).

See also: [backends](../components/backends.md), [engine-integration](../components/engine-integration.md), [throttle-equilibrium](../components/throttle-equilibrium.md).
