---
title: Pluggable Storage API
status: active
sources:
  - src/storage/storage.h:1-135
  - src/storage/storage_dispatch.c:1-84
  - src/storage/storage_middleware.c:1-176
updated: 2026-06-04
type: component
tier: working
claim_count: 8
edges:
  - to: interfaces/storagetype-vtable.md
    kind: refers_to
    source: human
    created: 2026-06-03
  - to: components/backends.md
    kind: refers_to
    source: human
    created: 2026-06-03
  - to: components/bridge-layer.md
    kind: refers_to
    source: llm_relation
    created: 2026-06-04
  - to: components/serialization.md
    kind: refers_to
    source: llm_relation
    created: 2026-06-04
---

# Pluggable Storage API

> The dispatch layer in `src/storage/storage_dispatch.c` turns the engine's
> `storageSubmit{Put,Get,Del}` calls into either a backend's **async** ops or the **shared
> middleware** that wraps a sync backend on worker threads. One decision — `put_async != NULL`
> — picks the path, made once per call so neither the engine nor the [bridge](bridge-layer.md)
> branches on backend type. Interface: [storageType vtable](../interfaces/storagetype-vtable.md).

## Global instance & init

`server_storage` (the chosen `storageType*`) and `server_storage_ctx` (its opaque handle)
are the two globals (`storage_dispatch.c:4-5`). `storageInit(type, cfg)`
(`storage_dispatch.c:19`) calls `type->open(cfg)` for the ctx and, **only if the backend has
no `put_async`**, spins up the middleware with `io_threads` (or 2) worker threads
(`storage_dispatch.c:26-30`). `storageShutdown` mirrors this (`storage_dispatch.c:34-42`).

## Dispatch (`storage_dispatch.c:44-71`)

Each submit is the same shape:

```c
storageSubmitPut(...)  -> put_async ? put_async(...) : storageMiddlewareSubmit(STORAGE_OP_PUT, ...)
storageSubmitGet(...)  -> get_async ? get_async(...) : storageMiddlewareSubmit(STORAGE_OP_GET, ...)
storageSubmitDel(...)  -> del_async ? del_async(...) : storageMiddlewareSubmit(STORAGE_OP_DEL, ...)
```

`storagePollCompletions(max)` likewise calls `poll_completions` for async backends, else
`storageMiddlewarePoll(max)` (`storage_dispatch.c:73-78`); `storageCron` calls the optional
`cron` (`storage_dispatch.c:80-83`).

## Async path

The backend owns its IO: `put_async`/`get_async`/`del_async` return `STORAGE_WOULDBLOCK`
on submission and the backend later delivers completions via its `completion_fn` (set in
`storageInit`), drained by `poll_completions`. Native async backends:
[FlashCache (real + mock) and RocksDBAsync](backends.md).

## Sync path — shared middleware (`storage_middleware.c`)

For a sync backend (no `put_async`), the middleware runs N `mw_worker` threads
(`storage_middleware.c:51`, spawned in `storageMiddlewareInit` `storage_middleware.c:103-117`):

1. **Submit** (`storageMiddlewareSubmit`, `storage_middleware.c:134`): **copies** the key and
   value into a heap request (`memcpy`, `storage_middleware.c:142` / `storage_middleware.c:146`), enqueues it on a
   mutex+cond request queue, and returns `STORAGE_WOULDBLOCK` (`storage_middleware.c:162`). The
   copy is why this path suits sync backends that can't borrow caller memory.
2. **Worker** (`mw_worker`, `storage_middleware.c:51-100`): pops a request and calls the
   backend's sync `put`/`get`/`del` (`storage_middleware.c:75` / `storage_middleware.c:84` / `storage_middleware.c:92`); PUT frees the
   copied value afterward (`storage_middleware.c:78`); GET captures `value`/`vlen`/`expire_ms`
   into the completion; then enqueues the completion on a ring.
3. **Poll** (`storageMiddlewarePoll`, `storage_middleware.c:165`): drains the completion ring on
   the main thread, invoking `completion_fn(c, privdata)` per completion (`storage_middleware.c:170`).

> Both paths deliver a `storageCompletion` to the same `completion_fn` the
> [bridge](bridge-layer.md) registered, so the engine's drain logic is identical regardless of
> path.

## Where serialization happens

The dispatch/middleware layer moves **opaque bytes** for sync backends; the engine's robj
ser/deser (RDB DUMP) is performed by the async backends' IO threads (the bridge passes robj
pointers through). See [serialization](serialization.md).

See also: [storageType vtable](../interfaces/storagetype-vtable.md), [bridge-layer](bridge-layer.md), [backends](backends.md).
