---
title: Backends
status: active
sources:
  - src/storage/storage.h:24
  - src/storage/storage.h:86-150
  - src/storage/storage.h:177-179
  - src/storage/storage_dispatch.c:25-38
  - src/storage/storage_dispatch.c:50-84
  - src/storage/storage_dispatch.c:95-102
  - src/storage/storage_middleware.c:75-123
  - src/storage/storage_mock.c:396-418
  - src/storage/storage_flashcache_real.c:34-40
  - src/storage/storage_flashcache_real.c:138-250
  - src/storage/storage_flashcache_real.c:316-349
  - src/storage/storage_flashcache_real.c:502-544
  - src/ext_storage_bridge.c:66-116
  - src/ext_storage.c:393-402
  - src/config.c:3372
  - src/module.c:809-841
  - modules/flash-tiering/src/lib.rs:39
  - modules/flash-tiering/src/lib.rs:135-167
  - modules/flash-tiering/src/lib.rs:216-247
  - modules/flash-tiering/src/lib.rs:352-392
  - modules/flash-tiering/src/lib.rs:398-402
  - modules/flash-tiering/src/lib.rs:505-569
  - modules/flash-tiering/src/lib.rs:585-593
  - modules/flash-tiering/src/storage_type_bridge.rs:79
  - modules/flash-tiering/src/storage_type_bridge.rs:106-179
  - modules/flash-tiering/src/storage_type_bridge.rs:236-253
  - modules/flash-tiering/src/dispatcher.rs:1-85
  - modules/flash-tiering/src/backends/rocksdb/backend.rs:36-64
  - modules/flash-tiering/src/backends/rocksdb/backend.rs:101-102
  - modules/flash-tiering/src/backends/rocksdb/backend.rs:158-218
  - modules/flash-tiering/src/backends/flashcache/backend.rs:62-111
  - modules/flash-tiering/Cargo.toml
  - modules/flash-tiering/README.md
  - modules/rocksdb-tiering/src/lib.rs:39-42
  - modules/rocksdb-tiering/src/lib.rs:93-94
  - modules/rocksdb-tiering/src/lib.rs:186-194
  - modules/rocksdb-tiering/src/bridge.rs:70
  - modules/rocksdb-tiering/src/bridge.rs:217-231
  - modules/rocksdb-tiering/src/asio.rs:94-95
  - modules/rocksdb-tiering/src/asio.rs:161-191
updated: 2026-07-30
type: component
tier: working
claim_count: 18
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
    note: backend contradictions (unwired middleware; unregistered key_may_exist; truncated Rust vtable mirror) fold here
---

# Backends

> The concrete `storageType` implementations behind the vtable: in-tree C backends
> (compiled into the engine) and module-registered Rust backends, plus how each one is
> registered and selected.

A backend is a `storageType` struct (`src/storage/storage.h:86-150`) whose function pointers
the dispatch layer calls. The vtable schema itself is documented in
[storagetype-vtable](../interfaces/storagetype-vtable.md); this page catalogs the actual
implementations and the registration/selection machinery.

## Two registration paths

A backend reaches the engine one of two ways:

1. **Native (compiled-in).** `src/storage/storage.h:177-179` declares **three** getters
   returning static `storageType` structs. The bridge picks one by name
   (`src/ext_storage_bridge.c:73-79`).
2. **Module (`ValkeyModule_RegisterStorageBackend`).** A loaded module hands the engine a
   `storageType*`; `VM_RegisterStorageBackend` (`src/module.c:820-831`) stashes it in
   `module_registered_storage_type` (`src/module.c:818`). **A module backend takes precedence
   over the native selection** — the bridge checks `moduleHasRegisteredStorageBackend()`
   *first* (`src/ext_storage_bridge.c:70-72`). Only one module backend may register; a second
   attempt is rejected (`src/module.c:823-826`), as is a NULL struct
   (`src/module.c:822`).

## Catalog

| `name` (vtable) | Source | IO model | Real/mock | Reg. path | Selected by |
|---|---|---|---|---|---|
| `flashcache` | `src/storage/storage_mock.c:397` | async (own worker) | **mock** (in-mem hash) | native getter (`storageGetFlashCacheType`, `src/storage/storage_mock.c:414`) | `ext-storage-backend=flashcache-mock` |
| `flashcache` (same struct, aliased) | `src/storage/storage_mock.c:418` | async (own worker) | **mock** (identical impl) | native getter (`storageGetRocksDBAsyncType`) | `ext-storage-backend=rocksdb` (dispatch testing) |
| `flashcache-real` | `src/storage/storage_flashcache_real.c:503` | async (own IO thread) | **real** (links `libflashcache.a`) | native getter (`storageGetFlashCacheRealType`, `src/storage/storage_flashcache_real.c:544`) | `ext-storage-backend=flashcache` (also the default, `src/ext_storage.c:394`) |
| `flash-tiering-rust` | `modules/flash-tiering/src/storage_type_bridge.rs:79` | async | **real** (RocksDB or FlashCache) | module | `--loadmodule … backend=rocksdb\|flashcache db_path=…` |
| `rocksdb-tiering` | `modules/rocksdb-tiering/src/bridge.rs:70` | async | **real** (RocksDB) | module | `--loadmodule … db_path=…` |

There are **four distinct vtable structs** (the mock is exposed under two getters). All four
set `version = VALKEY_STORAGE_VERSION` (`= 1`, `src/storage/storage.h:24`):
`src/storage/storage_mock.c:398`, `src/storage/storage_flashcache_real.c:502-543`,
`modules/flash-tiering/src/storage_type_bridge.rs:238`,
`modules/rocksdb-tiering/src/bridge.rs:219`.

> ⚠️ The two C **module** backends this page previously catalogued —
> *modules/storage_example/* (vtable name *module-example*) and
> *modules/storage_flashcache_module/* (vtable name *module-flashcache-real*, which took a
> *flash_file_path* load arg) — **no longer exist anywhere in the tree**. `modules/` on
> `unstable` contains only `flash-tiering`, `rocksdb-tiering` and `valkeymodule-rs`. There is
> no C reference-module backend today; the reference module path is the Rust
> `flash-tiering` crate. The removed C sources have no successor file, so no citation is
> given for them.

## Native backend selection & fallback

`extStorageBridge_init` (`src/ext_storage_bridge.c:66`) resolves the `storageType`, then calls
`storageInit` (`src/ext_storage_bridge.c:108`) with `io_threads = 1`
(`src/ext_storage_bridge.c:93`). The name→type map (`src/ext_storage_bridge.c:73-79`):

- `"flashcache"` → `storageGetFlashCacheRealType()` (real)
- `"flashcache-mock"` → `storageGetFlashCacheType()` (mock)
- `"rocksdb"` → `storageGetRocksDBAsyncType()` (the same mock struct)

An unrecognised name resolves to no type and the bridge returns `-1`
(`src/ext_storage_bridge.c:81-84`). The config key `ext-storage-backend` is an immutable
string config defaulting to `""` (`src/config.c:3372`); when unset the caller substitutes
`"flashcache"` — the **real** FlashCache backend (`src/ext_storage.c:394-395`).

> ⚠️ CORRECTED (Jul 2026): **there is no mock fallback.** This page previously said a failed
> native `storageInit` fell back to the mock `flashcache` type and re-inited. The current code
> does not: `storageInit` failure makes `extStorageBridge_init` log and return `-1`
> (`src/ext_storage_bridge.c:108-112`), and the caller then clears `ext_data_enabled` and
> disables tiering entirely (`src/ext_storage.c:398-401`). Native and module backends are now
> treated identically here — a failed init is fatal to tiering for both.

> ✅ RESOLVED (Jul 2026): the sync RocksDB backend (storage_rocksdb.c) was deleted; only
> storage_dispatch.c, storage_middleware.c, storage_mock.c and storage_flashcache_real.c remain
> under `src/storage/`. Both `flashcache-mock` and `rocksdb` config values route to the same
> in-memory mock struct (`src/storage/storage_mock.c:414-418`). The shared-middleware path
> remains available for future real sync backends but is currently unwired.

## Sync vs async dispatch fork

The dispatch layer branches on whether the backend supplies `put_async`
(`src/storage/storage_dispatch.c:32`):

- **Async backend** (`put_async != NULL`): owns its own IO thread; the engine calls its
  `*_async` ops directly (`src/storage/storage_dispatch.c:53-55,63-64,72-73`) and drains via
  its `poll_completions` (`src/storage/storage_dispatch.c:80-82`).
- **Sync backend** (`put_async == NULL`): `storageInit` spins up the **shared middleware**
  thread pool (`src/storage/storage_dispatch.c:32-36` → `storageMiddlewareInit`,
  `src/storage/storage_middleware.c:109-123`), whose workers call the backend's blocking
  `put`/`get`/`del` (`src/storage/storage_middleware.c:81,90,98`) and enqueue completions.
  Thread count comes from `cfg->io_threads`, defaulting to 2 when unset
  (`src/storage/storage_dispatch.c:33`).

No sync backends are currently wired. In practice **only the async path runs** — all four
live vtables set `put`/`get`/`del` to NULL and supply the `*_async` trio
(`src/storage/storage_mock.c:401-405`,
`modules/flash-tiering/src/storage_type_bridge.rs:241-247`,
`modules/rocksdb-tiering/src/bridge.rs:222-228`).

## Snapshot support (native only)

`storage.h` grew four optional fork/snapshot hooks after `get_stats` — `snapshot_hold`,
`snapshot_release`, `gc_pause`, `fork_read` (`src/storage/storage.h:127-149`) — and the engine
gates on the first two via `storageSnapshotSupported`
(`src/storage/storage_dispatch.c:95-102`). The native mock populates all four
(`src/storage/storage_mock.c:408-411`) and so does the real FlashCache backend
(`src/storage/storage_flashcache_real.c:451-501`).

> ⚠️ FLAGGED, NOT FIXED: neither Rust module mirrors those four fields. The Rust `StorageType`
> mirror stops at `get_stats` (`modules/flash-tiering/src/storage_type_bridge.rs:59-74`,
> `modules/rocksdb-tiering/src/bridge.rs:51-74`), while the C struct the engine reads is four
> pointers longer (`src/storage/storage.h:86-150`). `storageSnapshotSupported` therefore reads
> past the end of a module-registered struct. Whether this is a live defect or a deliberate
> "modules don't snapshot" boundary that needs an explicit version/length guard is a code
> question, not a wiki one — see [known-limitations](../decisions/known-limitations.md).

## Real backends — robj serialization on the IO thread

Mock backends store opaque byte blobs; **real** backends receive engine `robj*` pointers and
must serialize them. This happens **on the backend's IO thread, not the main thread**
(matches [serialization](serialization.md)):

- **`flashcache-real`**: its IO worker (`src/storage/storage_flashcache_real.c:160`) calls
  `extStorageSerializeKey`/`extStorageSerializeValue`
  (`src/storage/storage_flashcache_real.c:193-195`, declared
  `src/storage/storage_flashcache_real.c:34-40`), then `flashcachePutItem`
  (`src/storage/storage_flashcache_real.c:198`). GET/DEL go through `flashcacheGetItem` with
  `FC_READ` (`src/storage/storage_flashcache_real.c:231-233`) / `FC_DELETE`
  (`src/storage/storage_flashcache_real.c:247-249`); GET results arrive asynchronously via
  `fc_get_callback`, which `extStorageDeserializeValue`s the bytes
  (`src/storage/storage_flashcache_real.c:138-146`). `flashcacheInit` runs in the vtable's
  `open()` (`src/storage/storage_flashcache_real.c:316,349`).
- **`rocksdb-tiering`** (Rust) imports the engine serialization symbols directly over FFI
  (`modules/rocksdb-tiering/src/lib.rs:39-42`). **`flash-tiering`** (Rust) instead fetches the
  canonical callbacks at load time via `GetExternalStorageSerializationCallbacks`
  (`modules/flash-tiering/src/lib.rs:216-247`, invoked from init at
  `modules/flash-tiering/src/lib.rs:525-530`), falling back to placeholder callbacks with a
  warning if the API is unavailable.

## Module backends (detail)

- **`flash-tiering`** (Rust) — the primary value-spill module; `MODULE_NAME`
  (`modules/flash-tiering/src/lib.rs:39`) and `valkey_module! { name: "flash-tiering" }`
  (`modules/flash-tiering/src/lib.rs:585-593`). Backends are compiled in behind Cargo features
  `backend-rocksdb` / `backend-flashcache` (`modules/flash-tiering/src/dispatcher.rs:8-12`);
  the Cargo **default is `backend-flashcache`** (`modules/flash-tiering/Cargo.toml`).
  `backend=` and `db_path=` are both **required** load args
  (`modules/flash-tiering/src/lib.rs:359-365`) and `backend=` picks the variant at runtime via
  `create_dispatcher` (`modules/flash-tiering/src/lib.rs:398-402`, called from init at
  `modules/flash-tiering/src/lib.rs:533`). It exposes a single C `storageType` named
  `flash-tiering-rust` (`modules/flash-tiering/src/storage_type_bridge.rs:79`) whose `*_async`
  fns forward to the Rust `BackendDispatcher`
  (`modules/flash-tiering/src/storage_type_bridge.rs:106-179`); registration uses the
  `RedisModule_GetApi` → `ValkeyModule_RegisterStorageBackend` dance
  (`modules/flash-tiering/src/lib.rs:135-167`, called at
  `modules/flash-tiering/src/lib.rs:517-522`) with the static struct pointer from
  `get_storage_type_ptr` (`modules/flash-tiering/src/storage_type_bridge.rs:253`).
- **`rocksdb-tiering`** (Rust) — a standalone RocksDB-only module; same registration pattern,
  vtable name `rocksdb-tiering` (`modules/rocksdb-tiering/src/bridge.rs:70`),
  `valkey_module!` name at `modules/rocksdb-tiering/src/lib.rs:186-188`, `db_path=` required
  (`modules/rocksdb-tiering/src/lib.rs:93-94`), and a multi-threaded IO worker pool
  (`modules/rocksdb-tiering/src/asio.rs:94-95`) split into one ordering-preserving write
  worker and N−1 read workers (`modules/rocksdb-tiering/src/asio.rs:161-191`).

### Rust backend semantics

- **RocksDB backend** maps each logical DB to a **column family** named by `cf_name`
  (`"db_0"`, `"db_1"`, …) (`modules/flash-tiering/src/backends/rocksdb/backend.rs:101-102`,
  used at `modules/flash-tiering/src/backends/rocksdb/backend.rs:159,168,188`);
  **reads are destructive** — `get_item` does an atomic get-then-`WriteBatch`-`delete_cf`
  (`modules/flash-tiering/src/backends/rocksdb/backend.rs:167-178`) to keep each value in
  exactly one location. Configurable via `compression_type`, `bloom_filter_bits_per_key`,
  `disable_wal`, `direct_io`, `num_io_threads`
  (`modules/flash-tiering/src/backends/rocksdb/backend.rs:36-64`; see
  [config-and-module-args](../interfaces/config-and-module-args.md)).
- **FlashCache backend** is **single-IO-thread, async-read** (`is_async_read()` returns `true`,
  `modules/flash-tiering/src/backends/flashcache/backend.rs:109-111`); its synchronous
  `get_item` deliberately errors
  (`modules/flash-tiering/src/backends/flashcache/backend.rs:72-76`) because reads must go
  through the async callback path. Its `delete_key` is a no-op returning `Ok`
  (`modules/flash-tiering/src/backends/flashcache/backend.rs:78-80`) — deletion is implicit in
  the destructive-read model.

> ⚠️ CONTRADICTION: both Rust backends implement `key_may_exist`
> (`modules/flash-tiering/src/backends/rocksdb/backend.rs:218`,
> `modules/flash-tiering/src/backends/flashcache/backend.rs:101-103`, dispatched via
> `modules/flash-tiering/src/dispatcher.rs:65-73`) — a sync bloom/occupancy probe — but the
> `flash-tiering` module **deliberately does not register it**
> (`modules/flash-tiering/src/lib.rs:552-559`, commented out): *"key_may_exist callback is NOT
> needed with the new RegisterStorageBackend API. The engine's state machine handles key
> existence tracking. Registering it causes false positives that block clients forever."* This
> is the v1 invariant in action — the key is always in the dict, so the engine's state machine,
> not a bloom filter, tracks existence. The README's *"No bloom filter is needed"*
> (`modules/flash-tiering/README.md`) matches the code; the RocksDB backend still builds an SST
> bloom filter internally (`modules/flash-tiering/src/backends/rocksdb/backend.rs:90`), but it
> is not consulted on the existence-check path.

> ⚠️ CONTRADICTION (doc vs code): the README title still reads "Non-Key-Spilling", and its
> Quick-Start/ezBench sections still use the predecessor module name, modules/key-spilling/
> paths and a libkey_spilling_module.so artifact. The live module is built from
> `modules/flash-tiering`
> (`modules/flash-tiering/src/lib.rs:39` and
> `modules/flash-tiering/src/lib.rs:585-593`), package name *flash-tiering-module*
> (`modules/flash-tiering/Cargo.toml`). Treat the README's in-scope *behaviour* as source;
> ignore the stale paths and artifact names.

## See also

[storageType vtable](../interfaces/storagetype-vtable.md) ·
[pluggable-storage-api](pluggable-storage-api.md) ·
[bridge-layer](bridge-layer.md) ·
[serialization](serialization.md) ·
[config-and-module-args](../interfaces/config-and-module-args.md)
