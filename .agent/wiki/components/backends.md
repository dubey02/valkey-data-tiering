---
title: Backends
status: active
sources:
  - src/storage/storage.h:75-130
  - src/storage/storage_dispatch.c:19-82
  - src/storage/storage_middleware.c:70-118
  - src/storage/storage_mock.c
  - src/storage/storage_flashcache_real.c:1-379
  - src/ext_storage_bridge.c:61-110
  - src/module.c:812-840
  - modules/storage_example/storage_example.c:143-163
  - modules/storage_flashcache_module/storage_flashcache_module.c:95-211
  - modules/non-key-spilling/src/lib.rs:116-564
  - modules/non-key-spilling/src/storage_type_bridge.rs:77-266
  - modules/non-key-spilling/src/dispatcher.rs:1-85
  - modules/non-key-spilling/src/backends/rocksdb/backend.rs:1-220
  - modules/non-key-spilling/src/backends/flashcache/backend.rs:60-111
  - modules/rocksdb-tiering/src/lib.rs:1-194
  - modules/rocksdb-tiering/src/bridge.rs:70-218
  - modules/rocksdb-tiering/src/asio.rs:1-120
  - modules/non-key-spilling/README.md
updated: 2026-06-05
type: component
tier: working
claim_count: 16
edges:
  - to: interfaces/storagetype-vtable.md
    kind: implements
    source: llm_relation
    needs_review: false
    created: 2026-06-05
    note: every backend realises the storageType vtable
  - to: components/serialization.md
    kind: depends_on
    source: llm_relation
    needs_review: false
    created: 2026-06-05
    note: real backends serialise robj* to bytes on their IO thread
  - to: components/pluggable-storage-api.md
    kind: refers_to
    source: llm_relation
    created: 2026-06-05
  - to: components/bridge-layer.md
    kind: refers_to
    source: llm_relation
    created: 2026-06-05
  - to: interfaces/config-and-module-args.md
    kind: refers_to
    source: llm_relation
    created: 2026-06-05
  - to: decisions/known-limitations.md
    kind: refers_to
    source: llm_relation
    created: 2026-06-05
    note: two backend contradictions (unwired sync rocksdb; unregistered key_may_exist) fold here
---

# Backends

> The concrete `storageType` implementations behind the vtable: in-tree C backends
> (compiled into the engine) and module-registered backends (C and Rust), plus how each
> one is registered and selected.

A backend is a `storageType` struct (`storage.h:75-109`) whose function pointers the
dispatch layer calls. The vtable schema itself is documented in
[storagetype-vtable](../interfaces/storagetype-vtable.md); this page catalogs the actual
implementations and the registration/selection machinery.

## Two registration paths

A backend reaches the engine one of two ways:

1. **Native (compiled-in).** `storage.h:127-130` declares four getters returning static
   `storageType` structs. The bridge picks one by name (`ext_storage_bridge.c:68-74`).
2. **Module (`ValkeyModule_RegisterStorageBackend`).** A loaded module hands the engine a
   `storageType*`; `VM_RegisterStorageBackend` (`module.c:820-832`) stashes it in
   `module_registered_storage_type`. **A module backend takes precedence over the native
   selection** — the bridge checks `moduleHasRegisteredStorageBackend()` *first*
   (`ext_storage_bridge.c:65-66`). Only one module backend may register; a second attempt is
   rejected (`module.c:823-824`).

## Catalog

| `name` (vtable) | Source | IO model | Real/mock | Reg. path | Selected by |
|---|---|---|---|---|---|
| `flashcache-mock` | `storage_mock.c` | async (own worker) | **mock** (in-mem hash) | native getter | `ext-storage-backend=flashcache-mock` (default for TCL tests) |
| `flashcache-real` | `storage_flashcache_real.c:366` | async (own IO thread) | **real** (links `libflashcache.a`) | native getter | `backend=flashcache` |
| `rocksdb` | `storage_mock.c` (alias) | async (own worker) | **mock** (same impl as flashcache-mock) | native getter | `ext-storage-backend=rocksdb` (dispatch testing) |
| `module-example` | `storage_example.c:143` | async (own worker) | mock (in-mem hash) | module | `--loadmodule storage_example` |
| `module-flashcache-real` | `storage_flashcache_module.c:151` | async (FlashCache ASIO) | **real** | module | `--loadmodule storage_flashcache_module` |
| `non-key-spilling-rust` | `storage_type_bridge.rs:77` | async | **real** (RocksDB or FlashCache) | module | `--loadmodule … backend=rocksdb|flashcache` |
| `rocksdb-tiering` | `bridge.rs:70` | async | **real** (RocksDB) | module | `--loadmodule … db_path=…` |

All eight set `version = VALKEY_STORAGE_VERSION` (`= 1`, `storage.h:24`).

## Native backend selection & fallback

`extStorageBridge_init` (`ext_storage_bridge.c:61`) resolves the `storageType`, then calls
`storageInit` (`ext_storage_bridge.c:96`) with `io_threads = 1` (`ext_storage_bridge.c:89`).
The name→type map (`ext_storage_bridge.c:68-74`):

- `"flashcache"` → `storageGetFlashCacheRealType()` (real)
- `"flashcache-mock"` → `storageGetFlashCacheType()` (mock)
- `"rocksdb"` → `storageGetRocksDBAsyncType()` (async mock)

If a **native** backend's `storageInit` fails (e.g. real FlashCache can't open its device),
the bridge falls back to the mock `flashcache` type and re-inits
(`ext_storage_bridge.c:98-104`). Module backends get no fallback — a failed init is fatal.

> ✅ RESOLVED (Jul 2026): The sync `storage_rocksdb.c` was deleted. Both `flashcache-mock`
> and `rocksdb` config values now route to the same `storage_mock.c` implementation.
> The shared-middleware path (`storage_middleware.c`) remains available for future real
> sync backends but is currently unwired.

## Sync vs async dispatch fork

The dispatch layer branches on whether the backend supplies `put_async`
(`storage_dispatch.c:26`):

- **Async backend** (`put_async != NULL`): owns its own IO thread; the engine calls its
  `*_async` ops directly and drains via its `poll_completions`
  (`storage_dispatch.c:47-48,74-75`).
- **Sync backend** (`put_async == NULL`): `storageInit` spins up the **shared middleware**
  thread pool (`storage_dispatch.c:26-29` → `storageMiddlewareInit`,
  `storage_middleware.c:103`), whose workers call the backend's blocking `put`/`get`/`del`
  (`storage_middleware.c:75,84,92`) and enqueue completions. Thread count defaults to 2
  (`storage_dispatch.c:27`).

No sync backends are currently wired (the sync `storage_rocksdb.c` was deleted). In
practice **only the async path runs** (both `storage_mock.c` and `storage_flashcache_real.c` use async).

## Real backends — robj serialization on the IO thread

Mock backends store opaque byte blobs; **real** backends receive engine `robj*` pointers and
must serialize them. This happens **on the backend's IO thread, not the main thread**
(matches [serialization](serialization.md)):

- **`flashcache-real`** (`storage_flashcache_real.c:160-208`): its IO worker calls
  `extStorageSerializeKey`/`extStorageSerializeValue` (`storage_flashcache_real.c:167-176`),
  then `flashcachePutItem` (`storage_flashcache_real.c:172`). GET/DEL go through
  `flashcacheGetItem` with `FC_READ` (`storage_flashcache_real.c:190-192`) / `FC_DELETE`
  (`storage_flashcache_real.c:206-208`); GET results arrive asynchronously via
  `fc_get_callback`, which `extStorageDeserializeValue`s the bytes. `flashcacheInit` runs in
  `open()` (`storage_flashcache_real.c:288`); if it fails the bridge falls back to mock.
- **`rocksdb-tiering`** (Rust) and **`non-key-spilling-rust`** (Rust) import the same engine
  symbols over FFI (`modules/rocksdb-tiering/src/lib.rs:32-36`); the `non-key-spilling` module
  additionally fetches the canonical callbacks at load via
  `GetExternalStorageSerializationCallbacks` (`modules/non-key-spilling/src/lib.rs:197,500`).

## Module backends (detail)

- **`storage_example`** (C) — reference implementation; an in-memory async backend that
  registers via `ValkeyModule_RegisterStorageBackend` in `OnLoad` (`storage_example.c:161`).
  Takes no args.
- **`storage_flashcache_module`** (C) — real FlashCache with its own ASIO thread; parses
  `<flash_file_path> <size_bytes>` args (`storage_flashcache_module.c:172-181`), runs
  `flashcacheInit` (`storage_flashcache_module.c:197`), registers the backend
  (`storage_flashcache_module.c:210`).
- **`non-key-spilling`** (Rust) — the primary value-spill module. Compile-time backend selection via
  Cargo features `backend-rocksdb` (default) / `backend-flashcache` (`dispatcher.rs:11-19`),
  runtime selection via the `backend=…` load arg (`create_dispatcher`,
  `modules/non-key-spilling/src/lib.rs:378`). It exposes a single C `storageType` named
  `non-key-spilling-rust` whose `*_async` fns forward to the Rust `BackendDispatcher`
  (`storage_type_bridge.rs:108-266`); registration uses the `GetApi` →
  `RegisterStorageBackend` dance (`modules/non-key-spilling/src/lib.rs:116,492`).
- **`rocksdb-tiering`** (Rust) — a standalone RocksDB-only module
  (`modules/rocksdb-tiering/src/lib.rs:1-194`); same registration pattern, vtable name
  `rocksdb-tiering` (`bridge.rs:70`), multi-threaded IO worker pool
  (`modules/rocksdb-tiering/src/asio.rs:94-95`).

### Rust backend semantics

- **RocksDB backend** maps each logical DB to a **column family** named by `cf_name`
  (`"db_0"`, `"db_1"`, …) (`modules/non-key-spilling/src/backends/rocksdb/backend.rs:167-169`);
  **reads are destructive** — `get_item` does an atomic get-then-`WriteBatch`-`delete_cf`
  (`modules/non-key-spilling/src/backends/rocksdb/backend.rs:177-178`) to keep each value in
  exactly one location. Configurable via `compression_type`, `bloom_filter_bits_per_key`,
  `disable_wal`, `direct_io`, etc. (see
  [config-and-module-args](../interfaces/config-and-module-args.md)).
- **FlashCache backend** is **single-IO-thread, async-read** (`is_async_read()` returns `true`,
  `modules/non-key-spilling/src/backends/flashcache/backend.rs:108-110`); its synchronous
  `get_item` deliberately errors
  (`modules/non-key-spilling/src/backends/flashcache/backend.rs:71-74`) because reads must go
  through the async callback path.

> ⚠️ CONTRADICTION: both Rust backends implement `key_may_exist`
> (`modules/non-key-spilling/src/backends/rocksdb/backend.rs:218`,
> `modules/non-key-spilling/src/backends/flashcache/backend.rs:100`) — a sync bloom/occupancy
> probe — but the `non-key-spilling` module **deliberately does not register it**
> (`modules/non-key-spilling/src/lib.rs:533-536`, commented out): *"Registering it causes false
> positives that block clients forever."* This is the v1 invariant in action — the key is
> always in the dict, so the engine's state machine, not a bloom filter, tracks existence. The
> README's *"No bloom filter is needed"* (`README.md`) matches the code; the RocksDB backend
> still builds an SST bloom filter internally, but it is not consulted on the existence-check path.

> ⚠️ CONTRADICTION (doc vs code): the README Quick-Start/ezBench sections still use the
> predecessor "key-spilling" module name, modules/key-spilling/ paths, and a
> lib​key_spilling_module.so artifact. The live crate is **`non-key-spilling`** (`MODULE_NAME`,
> `modules/non-key-spilling/src/lib.rs:39`, and `valkey_module! { name: "non-key-spilling" }` at
> `modules/non-key-spilling/src/lib.rs:564`). Treat the README's in-scope *behaviour* as
> source; ignore the stale key-spilling paths/artifact names.

## See also

[storageType vtable](../interfaces/storagetype-vtable.md) ·
[pluggable-storage-api](pluggable-storage-api.md) ·
[bridge-layer](bridge-layer.md) ·
[serialization](serialization.md) ·
[config-and-module-args](../interfaces/config-and-module-args.md)
