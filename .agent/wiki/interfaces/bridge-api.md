---
title: Bridge API
status: active
sources:
  - src/ext_storage_bridge.h:1-29
updated: 2026-06-04
type: interface
tier: working
claim_count: 5
edges:
  - to: components/bridge-layer.md
    kind: refers_to
    source: human
    created: 2026-06-03
  - to: components/pluggable-storage-api.md
    kind: refers_to
    source: llm_relation
    created: 2026-06-04
  - to: interfaces/storagetype-vtable.md
    kind: refers_to
    source: llm_relation
    created: 2026-06-04
---

# Bridge API

> `src/ext_storage_bridge.h` — the narrow adapter `ext_storage.c` calls instead of the old
> module-event API. It owns backend init and translates engine `robj`/`sds` submit calls
> into the [storageType vtable](storagetype-vtable.md) dispatch helpers. Implementation +
> mapping detail: [bridge-layer](../components/bridge-layer.md).

## Functions

| Fn (`ext_storage_bridge.h`) | Line | Replaces (legacy) | Role |
|------|------|-------------------|------|
| `extStorageBridge_init(backend_name, path, capacity)` | 10 | — | open the named backend; called from `extStorage_init()` |
| `extStorageBridge_isReady(void)` | 13 | `moduleHasExternalStorageSubscribers` | backend ready check |
| `extStorageBridge_submitPut(db_id, robj *key, robj *value, expire_ms)` | 16 | `moduleFireExternalStorageEvent` | spill |
| `extStorageBridge_submitGet(db_id, sds key)` | 17 | ↑ | fetch |
| `extStorageBridge_submitDel(db_id, sds key)` | 18 | ↑ | async delete |
| `extStorageBridge_pollCompletions(ValkeyModuleExternalStorageMsg **out, int max)` | 23 | `moduleGetCompletedExternalStorageResponses` | drain |
| `extStorageBridge_shutdown(void)` / `storageShutdown(void)` | 26-27 | — | teardown |

`pollCompletions` returns `ValkeyModuleExternalStorageMsg**` specifically to stay
compatible with the existing `processCompletedStorageRequests()` logic (`:20-22` comment).

See also: [bridge-layer](../components/bridge-layer.md), [pluggable-storage-api](../components/pluggable-storage-api.md), [storagetype-vtable](storagetype-vtable.md).
