---
title: Integration Tests (Tiering)
status: active
sources:
  - tests/unit/data-tiering/ext-storage-data-types.tcl:1-881
  - tests/unit/data-tiering/ext-storage-persistence.tcl:1-112
  - tests/unit/data-tiering/ext-storage.tcl:1-153
  - tests/unit/data-tiering/ext-storage-snapshot.tcl:1-227
  - tests/unit/data-tiering/ext-storage-swapdb.tcl:1-203
  - tests/unit/data-tiering/ext-storage-sync-fetch.tcl:1-141
  - tests/unit/data-tiering/ext-storage-del-semantics.tcl:1-150
  - tests/unit/data-tiering/ext-storage-blocking.tcl:1-150
  - tests/unit/data-tiering/ext-storage-expiry.tcl:1-153
  - tests/unit/data-tiering/ext-storage-eviction.tcl:1-180
  - tests/unit/data-tiering/ext-storage-defrag.tcl:1-109
  - tests/unit/data-tiering/ext-storage-embstr-spill.tcl:1-62
  - tests/unit/data-tiering/ext-storage-fc-configs.tcl:1-123
  - tests/unit/data-tiering/ext-storage-max-spill-size.tcl:1-121
  - tests/unit/data-tiering/ext-storage-module-configs.tcl:1-104
  - tests/unit/data-tiering/ext-storage-module-expiry-eviction.tcl:1-284
  - tests/unit/data-tiering/ext-storage-module-spill-fetch.tcl:1-82
  - tests/unit/data-tiering/ext-storage-json-module.tcl:1-134
  - tests/unit/data-tiering/ext-storage-bloom-module.tcl:1-127
  - tests/unit/introspection.tcl:1277-1280
  - src/rdb.c:1191-1212
  - src/rdb.c:1487
  - src/aof.c:2446
  - src/aof.c:2504
  - src/lazyfree.c:141
  - src/module.c:13292
  - src/storage/storage_mock.c:82-83
  - src/config.c
  - src/ext_storage.c
  - src/ext_storage_bridge.c
updated: 2026-07-30
type: component
tier: working
claim_count: 128
edges:
  - to: flows/spill.md
    kind: refers_to
    source: human
    created: 2026-06-17
    note: data-type tests exercise the spill round-trip
  - to: flows/fetch.md
    kind: refers_to
    source: human
    created: 2026-06-17
    note: read-after-spill tests exercise the fetch path
  - to: components/serialization.md
    kind: refers_to
    source: human
    created: 2026-06-17
    note: DUMP/RESTORE + encoding-preservation tests
  - to: components/eviction-integration.md
    kind: refers_to
    source: human
    created: 2026-06-17
    note: pressure-driven (natural) spill test
  - to: components/persistence-replication.md
    kind: refers_to
    source: human
    created: 2026-06-17
    note: AOF/RDB crash-safety + snapshot persistence tests
  - to: components/backends.md
    kind: refers_to
    source: human
    created: 2026-06-17
    note: flashcache-mock, real flashcache, and module-backed suites
  - to: interfaces/config-and-module-args.md
    kind: refers_to
    source: human
    created: 2026-06-17
    note: ext-storage-* config overrides; introspection skip_configs; FC tuning-config suites
  - to: interfaces/info-metrics.md
    kind: refers_to
    source: human
    created: 2026-06-17
    note: helpers poll the spill counter; fc_* metric presence tests
  - to: decisions/known-limitations.md
    kind: refers_to
    source: human
    created: 2026-06-17
    note: remaining gaps (plain-AOF rewrite, slot migration, replication)
---

# Integration Tests (Tiering)

> Tcl integration tests under `tests/unit/data-tiering/` that validate data tiering:
> spill/fetch round-trip integrity for every data type, key lifecycle on tiered keys,
> RDB snapshotting, SWAPDB, sync fetch, DEL semantics, expiry/eviction, and the module
> backend path. This page maps each behavior to the test that covers it.

The whole suite moved from `tests/unit/` into `tests/unit/data-tiering/` and grew from
3 files to 19. Counts on this page were re-derived on 2026-07-30 by counting `test {…}` /
`test "…"` block openers per file
(`grep -cE '^[[:space:]]*test[[:space:]]*[{"]' tests/unit/data-tiering/*.tcl`) and all cited
line numbers were re-read from the current files with `grep -n`.

## Test files

19 files, **225 test blocks** total. "Runs?" is the file's own skip guard.

| File | Tests | Backend | Runs? | Scope |
|------|------:|---------|-------|-------|
| ext-storage-data-types.tcl | 60 | flashcache-mock (env-overridable) | always | Spill/fetch data validity across all 6 structure types + key lifecycle + Lua/MULTI-EXEC/COPY/APPEND/PERSIST surface + bash-migration backfill |
| ext-storage-persistence.tcl | 2 | flashcache-mock (env-overridable) | always | AOF rewrite crash-safety; AOF-reload persistence via the RDB preamble |
| ext-storage-snapshot.tcl | 8 | flashcache-mock (env-overridable) | always | Fork-based RDB snapshot: SAVE, BGSAVE, DEBUG RELOAD, prepare/hold/GC-pause lifecycle |
| ext-storage-swapdb.tcl | 11 | flashcache-mock | always | SWAPDB physical-db-id indirection + runtime maxmemory-policy guard + pending-DEL guard |
| ext-storage-sync-fetch.tcl | 9 | flashcache-mock | always | Mid-execution synchronous fetch: SORT BY/GET patterns, Lua undeclared keys |
| ext-storage-del-semantics.tcl | 9 | flashcache-mock | always | DEL/UNLINK reply, notification, WATCH invalidation and dirty++ on flash-resident keys |
| ext-storage-blocking.tcl | 9 | flashcache-mock | always | BLPOP/BLMOVE/BLMPOP/XREAD BLOCK over flash keys; SAVE / DEBUG RELOAD / SWAPDB / DEBUG OBJECT smoke |
| ext-storage.tcl | 12 | module (`backend=rocksdb`) | **never** (module path absent) | dict-invariant suite (see contradiction below) |
| ext-storage-expiry.tcl | 12 | flashcache (real, `fallocate`) | always | TTL/PTTL/PERSIST/EXPIRE, passive + active expiry, FLUSHDB/FLUSHALL, expiry during fetch |
| ext-storage-eviction.tcl | 6 | flashcache (real) | always | Keys never evicted; volatile-* disables tiering; noeviction still spills; flash-full OOM |
| ext-storage-defrag.tcl | 7 | flashcache (real) | always | Active defrag concurrent with spilling; data integrity; no crash |
| ext-storage-max-spill-size.tcl | 4 | flashcache (real) | always | `ext-storage-max-spill-size` threshold, boundary case, runtime CONFIG SET, `0` = disabled |
| ext-storage-fc-configs.tcl | 21 | flashcache (real) | always | FC tuning-config defaults + IMMUTABLE/MODIFIABLE behaviour + `fc_*` INFO metric presence |
| ext-storage-embstr-spill.tcl | 1 | flashcache-mock (env-overridable) | always | ASan regression: embstr restore use-after-free on first GET |
| ext-storage-module-configs.tcl | 16 | module (flash-tiering, `backend=flashcache`) | if module built | Same config matrix as fc-configs, through the module backend |
| ext-storage-module-expiry-eviction.tcl | 18 | module (flash-tiering) | if module built | Expiry + eviction matrix through the module backend (5 server blocks) |
| ext-storage-module-spill-fetch.tcl | 3 | module (flash-tiering) | if module built | Sustained spilling beyond the first batch; fetch correctness; serialization counter |
| ext-storage-json-module.tcl | 9 | flashcache-mock + JSON module | if JSON_MODULE_PATH set | OBJ_MODULE (JSON) spill/fetch, pressure spilling, DEL/expire crash regressions |
| ext-storage-bloom-module.tcl | 8 | flashcache-mock + bloom module | if BLOOM_MODULE_PATH set | OBJ_MODULE (Rust bloom) spill/fetch, pressure spilling, DEL/expire crash regressions |
| tests/unit/introspection.tcl | — | — | always | Adds the 4 `ext-storage-*` configs to the immutable skip_configs list |

Line numbers below are positions of the `test {…}` blocks within the named file
(authored from `grep -n`).

## How the tiering tests run

ext-storage-data-types.tcl and ext-storage-persistence.tcl start a server with
`ext-storage-enabled yes`, maxmemory 10mb, and `maxmemory-policy allkeys-lru`
(data-types.tcl lines 46-53; persistence.tcl lines 49-56). The backend defaults to
`flashcache-mock` (in-memory, no disk needed) but is overridable to the real FlashCache
backend via the EXT_STORAGE_BACKEND / EXT_STORAGE_PATH / EXT_STORAGE_CAPACITY_MB
environment variables, which feed `ext-storage-backend`, `ext-storage-path`, and
`ext-storage-capacity-mb` respectively. The disk-backed suites (expiry, eviction, defrag,
fc-configs, max-spill-size) instead hard-code `ext-storage-backend flashcache` and
`fallocate` their own db file, so they exercise the real FlashCache backend unconditionally.

Both files share two Tcl helpers:

- a get_spill_count helper that scrapes the INFO field
  `total_num_items_spilled_to_ext_storage` (see [info-metrics](../interfaces/info-metrics.md)); and
- a debug_spill helper that issues DEBUG SPILL on a key and polls that counter until it
  increments, giving a deterministic "value is now on flash" barrier before the test reads it
  back. Reading the key afterwards drives the [fetch](../flows/fetch.md) path.

data-types.tcl adds a third helper, get_info_field (line 38), for reading arbitrary INFO fields.

`flashcache-mock` is the in-tree mock backend; its IO worker serializes on PUT and
deserializes on GET via `extStorageSerializeValue` / `extStorageDeserializeValue`
(declared `src/storage/storage_mock.c:82-83`, called at `src/storage/storage_mock.c:129`
and `src/storage/storage_mock.c:189`), so the round-trip exercises the real serialization
contract, not a raw memcpy. See [backends](backends.md), [serialization](serialization.md).

## Coverage matrix — data validity by type

Each cell is a spill (via DEBUG SPILL) followed by a read-back assertion (the
[spill](../flows/spill.md) → [fetch](../flows/fetch.md) round-trip).
All in ext-storage-data-types.tcl.

| Type | Encodings covered | Round-trip / read commands | Test (line) |
|------|-------------------|----------------------------|-------------|
| String | raw, binary-safe, large (64 KiB) | GET, MGET | binary 307, large 317 |
| Hash | listpack + hashtable (200 fields) | HGETALL, HGET, HLEN, HEXISTS, HMGET | 59, 71, 80, 89, 100 |
| List | quicklist (200 items) | LRANGE, LLEN, LINDEX, LPOP, RPOP | 114, 124, 134, 145 |
| Set | intset + hashtable (200) | SMEMBERS, SCARD, SISMEMBER | 158, 167, 176, 184 |
| ZSet | skiplist (200); float scores; ties | ZRANGE WITHSCORES, ZSCORE, ZCARD, ZRANK, ZRANGEBYSCORE | 198, 213, 222, 231, 241 |
| Stream | small + large (500 entries) | XLEN, XRANGE, XINFO | 254, 268, 539 |

Encoding-specific correctness asserted: ZSet float-score precision and duplicate-score lex
tie-breaking (lines 198, 241); intset membership after round-trip (line 176); and an explicit
OBJECT ENCODING-preservation test confirming listpack/hashtable/quicklist encodings survive
spill+fetch unchanged (line 631). The "large string" test (line 317) is named `>1MB` but
actually writes 65536 bytes.

### Stream consumer-group state

Stream tests go beyond entries: consumer-group read/ack state must survive spill+fetch.

- single group, one read + one ack, verifies pending=0 and the remaining message is still
  unread after fetch (line 280);
- two groups with divergent read/ack progress, verifies each group's independent pending and
  unread counts after fetch (line 558).

## Coverage matrix — key lifecycle on tiered keys

| Behavior | What it asserts | Test (line) |
|----------|-----------------|-------------|
| TTL survives spill/fetch | TTL ≈ unchanged after spill then access, all 6 types | 331 |
| DEL on a tiered key | key removed; one test per type (string/hash/list/set/zset/stream) | 365, 372, 379, 386, 393, 400 |
| UNLINK on a tiered key | async-free path removes all 6 types | 590 |
| Overwrite while tiered | writing new fields/value reflects immediately | 407 |
| TYPE on tiered keys | correct type for all 6 types without explicit fetch | 416 |
| SCAN + TYPE on tiered keys | SCAN enumerates tiered keys with correct types | 440 |
| RENAME a tiered key | data preserved under the new name; old name gone | 462 |
| DUMP / RESTORE | DUMP yields a valid RDB payload that RESTOREs intact | 472 |
| Multi-key read (MGET) | crosses tiered + in-memory keys in one command | 608 |
| SUNIONSTORE | combines a tiered set with an in-memory set | 621 |
| Pressure-driven spill | natural (non-DEBUG) spill under a 2 MB cap preserves mixed-type data | 484 |

The pressure-driven test (line 484) triggers spilling through the
[eviction-integration](eviction-integration.md) path rather than DEBUG SPILL; it lowers
`maxmemory` to 2 MB, populates 50 keys each of string/hash/list, waits for the spill counter to
advance, then verifies data integrity for keys that were admitted. The Lua INCRBY test
(line 711) is the only other test in this file that spills via natural LRU pressure rather than
DEBUG SPILL — it must, because INT/EMBSTR-encoded numeric values are rejected by DEBUG SPILL
(see [known-limitations](../decisions/known-limitations.md)).

## Coverage matrix — scripting, transactions & extra commands

These 11 tests (all in ext-storage-data-types.tcl, appended after the lifecycle block) assert
that the transparent-fetch path works when tiered keys are touched indirectly — inside Lua
scripts, MULTI/EXEC transactions, and by COPY/APPEND/PERSIST. All spill via DEBUG SPILL except
the Lua INCRBY test (line 711), which uses natural LRU pressure.

### Lua scripting on tiered keys

| Test (line) | What it asserts |
|-------------|-----------------|
| EVAL reads tiered string (line 666) | `redis.call('GET', KEYS[1])` fetches a single tiered string verbatim |
| EVAL reads multiple tiered keys (line 673) | two tiered keys fetched and concatenated in one script |
| EVAL writes then reads different tiered key (line 687) | `SET KEYS[1]` + `GET KEYS[2]`; the read of the tiered key returns its value while another key is written |
| EVAL hash operations on tiered key (line 698) | `HGET` of an existing field + `HSET`/`HGET` of a new field within one script |
| EVAL INCRBY on tiered integer string (line 711) | pressure-spilled INT value is fetched, INCRBY 50 applied, `GET` returns the updated number |

### MULTI/EXEC transactions on tiered keys

| Test (line) | What it asserts |
|-------------|-----------------|
| MULTI/EXEC reads tiered key (line 732) | queued `GET` inside a transaction returns the tiered value on EXEC |
| MULTI/EXEC write + read tiered key (line 742) | queued `APPEND` then `GET`; EXEC returns the appended result |
| WATCH on tiered key aborts on modification (line 753) | a WATCHed tiered key modified before EXEC causes the transaction to abort (EXEC returns empty) |

### COPY / APPEND / PERSIST on tiered keys

| Test (line) | What it asserts |
|-------------|-----------------|
| COPY on tiered key (line 772) | `COPY src dst` duplicates a tiered value; source still exists |
| APPEND on tiered string (line 781) | `APPEND` fetches, appends, and the combined value reads back |
| PERSIST removes TTL on tiered key (line 789) | `PERSIST` clears the TTL (`TTL` = -1) while the value survives |

## Coverage matrix — command-surface & Lua edge cases (bash-migration backfill)

These 7 tests (ext-storage-data-types.tcl lines 801–881, under the "MISSING FROM UPSTREAM BASH
MIGRATION" banner) backfill cases that the deleted upstream `.sh`/`.py` suites covered but the
initial Tcl port omitted. Most spill via DEBUG SPILL; the exceptions are RANDOMKEY (line 812)
and DBSIZE (line 817), which are keyspace-introspection checks that run against the keys already
spilled by the preceding MSET test (line 801) rather than spilling their own.

| Test (line) | What it asserts |
|-------------|-----------------|
| MSET writes multiple tiered-eligible keys (line 801) | MSET of 3 keys; after 2 are spilled, all 3 read back verbatim (tiered + resident mix) |
| RANDOMKEY returns a key when tiered keys exist (line 812) | RANDOMKEY is non-empty when the keyspace contains tiered keys |
| DBSIZE includes tiered keys (line 817) | DBSIZE is non-zero — tiered keys stay counted in the dict |
| Lua conditional branch on tiered value (line 822) | EVAL fetches a tiered value and an if/else on its prefix takes the expected branch |
| Lua loop over multiple tiered keys (line 837) | EVAL loops over `#KEYS`, fetches each tiered value, and sums their numeric prefixes (= 60) |
| Lua pcall error handling on tiered key (line 856) | a Lua `pcall` of INCR on a non-numeric tiered value is caught as an error, not a crash |
| MULTI/EXEC APPEND on tiered key (line 871) | queued APPEND + GET in a transaction; EXEC returns the appended result |

## Persistence tests and crash-prevention guards

ext-storage-persistence.tcl (2 tests) covers the interaction of tiered objects with AOF/RDB.

| Test (line) | Asserts |
|-------------|---------|
| BGREWRITEAOF does not crash with tiered keys (line 58) | server survives an AOF rewrite with 6 tiered keys present; PING ok, DBSIZE = 6 |
| AOF rewrite (RDB preamble) preserves tiered keys (line 82) | 3 tiered keys survive BGREWRITEAOF + DEBUG LOADAOF with values intact |

The second test **used to be an intentionally-failing regression marker** for unimplemented
persistence. It is no longer: `rdbSaveKeyValuePair` now materializes a tiered value's serialized
bytes instead of skipping the key, so the RDB-preamble base of an AOF rewrite carries tiered
data (`src/rdb.c:1191-1212`; the test's own header comment at lines 83-87 states the same).
Broader snapshot coverage lives in ext-storage-snapshot.tcl (see below).

These tests exercise the staged `objectIsTiered` guards in the persistence paths:

- RDB save / AOF preamble: `rdbSaveKeyValuePair` calls `extStorageMaterializeTiered` for a
  tiered value and splices the on-flash DUMP payload into a standard RDB entry
  (`src/rdb.c:1209-1212`); it returns `0` (skip) only when the value was logically deleted or
  GC-evicted. Runs in the BGSAVE fork child or on the main thread under
  `extStorageSnapshotPrepare()`. See [persistence-replication](persistence-replication.md).
- RDB fork child: `dismissObject` is called only for non-tiered objects (`src/rdb.c:1487`).
- Plain-command (non-preamble) AOF rewrite: both rewrite loops now **skip tiered objects with
  a warning** instead of asserting — `src/aof.c:2446` in `rewriteSlotToAppendOnlyFileRio` and
  `src/aof.c:2504` in `rewriteAppendOnlyFileRio`. A RESTORE-based emit is planned with slot
  migration. `aof-use-rdb-preamble` defaults to yes, so the default path is the covered one.
- Lazyfree: `lazyfreeGetFreeEffort` returns early for a tiered object (`src/lazyfree.c:141`),
  which is what makes the UNLINK-on-tiered test (line 590) safe.

> ⚠️ Note: the two `objectIsTiered` skip sites in `src/aof.c` carry each other's log wording —
> the site inside `rewriteSlotToAppendOnlyFileRio` (line 2446) logs "AOF rewrite
> (non-preamble)", while the site inside `rewriteAppendOnlyFileRio` (line 2504) logs "Slot
> snapshot". Behaviour is the same (skip + warn); only the messages look transposed.

## Snapshot / RDB tests

ext-storage-snapshot.tcl (8 tests) covers the fork-based V1 snapshot design end to end.

| Test (line) | Asserts |
|-------------|---------|
| snapshot INFO fields present and backend supports snapshotting (line 70) | snapshot capability + INFO surface |
| SAVE with all data types on flash round-trips via DEBUG RELOAD (line 76) | foreground SAVE materializes every type |
| TTL survives snapshot of a flash-resident key (line 114) | expire preserved through save/reload |
| BGSAVE with tiered values completes and RDB is loadable (line 123) | fork path + standard RDB output |
| ongoing writes during BGSAVE do not corrupt the snapshot (line 147) | writes concurrent with the fork |
| tiering remains fully functional after snapshot (line 175) | GC/hold released on completion |
| snapshot prepare refuses when completions are stalled (line 187) | prepare-time safety guard |
| re-spill after reload under real memory pressure (line 207) | loaded values re-tier |

ext-storage-blocking.tcl adds a smoke layer over the same paths: SAVE while values are on
flash (line 110), DEBUG RELOAD round-trip (line 121), and SWAPDB (line 131).

## SWAPDB, sync fetch, DEL semantics, blocking

| File | Test (line) | Asserts |
|------|-------------|---------|
| swapdb | flash keys in both dbs reachable after swap (line 54) | physical-db-id indirection |
| swapdb | swap back and re-fetch (line 71) | mapping is reversible |
| swapdb | DEL of a flash key after swap returns the right reply (line 81) | completion routes to the owning keyspace |
| swapdb | new spills after swap land in the right namespace (line 93) | post-swap writes |
| swapdb | same-index swap is a no-op that works (line 107) | degenerate case |
| swapdb | volatile-* / allkeys-random rejected at runtime with tiering active (line 118) | runtime policy guard |
| swapdb | supported policies still settable (line 126); tiering still functional after rejects (line 132) | guard is not over-broad |
| swapdb | SWAPDB rejected while a DEL of a flash key is in flight (line 138) | pending-DEL guard |
| swapdb | blocked non-DEL client does not trip the guard (line 168) | guard specificity |
| swapdb | volatile-* allowed when tiering is disabled (line 199) | guard is tiering-scoped |
| sync-fetch | SORT BY with flash-resident weight keys (line 52) | previously SIGSEGV in sort |
| sync-fetch | SORT BY + GET with flash data keys (line 63); hash-field pattern (line 75) | pattern-key fetch |
| sync-fetch | Lua undeclared key read (line 87) / write (line 102) / read-your-own-write (line 110) | keys outside `KEYS[]` |
| sync-fetch | EVAL with undeclared flash key inside MULTI/EXEC (line 120) | nesting |
| sync-fetch | sync fetch metrics recorded (line 129); top-level commands still use the async filter (line 134) | path selection |
| del-semantics | DEL (line 51) / UNLINK (line 58) of a flash key returns 1 | the "replies 0" regression |
| del-semantics | mixed flash/memory/absent DEL count (line 65); two flash keys returns 2 (line 75) | reply arithmetic |
| del-semantics | keyspace `del` notification fires (line 83); WATCH invalidated (line 99); dirty counter increments (line 143) | full command-layer side effects |
| del-semantics | GET queued behind a pending DEL sees the key deleted (line 118); SET after DEL creates a fresh key (line 135) | pending-deletion state |
| blocking | BLPOP (line 48) / BLMOVE (line 58) / BLMPOP (line 68) on flash-resident lists | fetch-then-block layering |
| blocking | BLPOP woken by RPUSH (line 77); XREAD BLOCK woken by XADD after a spill (line 87) | wakeup while tiered |
| blocking | DEBUG OBJECT on a flash-resident key reports tiering info without crashing (line 138) | introspection |

## Expiry, eviction, defrag, size limits (real FlashCache backend)

| File | Coverage | Tests |
|------|----------|------:|
| ext-storage-expiry.tcl | TTL/PTTL (lines 43, 51), PERSIST (56), EXPIRE (63), passive expiry (71, 82), active expiry cron (86), FLUSHDB/FLUSHALL (107, 118), expiry racing a fetch (130), liveness (103, 147) | 12 |
| ext-storage-eviction.tcl | keys never deleted under pressure (44), DBSIZE constant (54), liveness (65), volatile-lru disables tiering (85), noeviction still spills (114), flash-full returns OOM without data loss (149) | 6 |
| ext-storage-defrag.tcl | startup with activedefrag + tiering (46), defrag during spilling (52), data integrity (61), config/cycle-time (68), sustained writes (77), FLUSHDB + defrag (90), liveness (103) | 7 |
| ext-storage-max-spill-size.tcl | oversize items not spilled (51), runtime CONFIG SET (76), exactly-at-limit is spilled (89), `0` disables the check (105) | 4 |
| ext-storage-fc-configs.tcl | 3 IMMUTABLE configs' defaults (21, 25, 29) + CONFIG SET rejection (33, 38, 43); 5 MODIFIABLE defaults (48, 52, 56, 60, 84) + CONFIG SET (64, 69, 74, 79, 88); 5 `fc_*` INFO metrics (94, 99, 104, 109, 114) | 21 |
| ext-storage-embstr-spill.tcl | ASan regression for the embstr restore use-after-free: spill a 64 B key with a 100 B value, then GET (47) | 1 |

## Module-backend suites (flash-tiering)

Three files drive tiering through the loadable flash-tiering module rather than the built-in
backend. All three resolve the .so under modules/flash-tiering/target/release/ and `return`
early (skip) when it is not built, so they only run after `cargo build --release
--features backend-flashcache`.

| File | Tests | Coverage |
|------|------:|----------|
| ext-storage-module-configs.tcl | 16 | The fc-configs matrix repeated through the module: 3 IMMUTABLE defaults (27, 31, 35) + rejections (40, 45, 50), 5 MODIFIABLE defaults (56, 60, 64, 68, 72) + propagation to the module (77, 82, 87, 92, 97) |
| ext-storage-module-expiry-eviction.tcl | 18 | Expiry block (52–149, 12 tests) mirroring ext-storage-expiry.tcl, plus eviction (169, 176, 187), volatile-lru disables tiering (205), noeviction spills (232), flash-full OOM (260) across 5 server blocks |
| ext-storage-module-spill-fetch.tcl | 3 | Sustained spilling beyond the first batch (40) — the ram_bytes predictor regression — fetch correctness (57), `spill_serialized_count` tracking (73) |

## OBJ_MODULE data (JSON, bloom) — external modules

Two suites cover module-owned value types crossing the spill/fetch boundary. Both skip unless
an external .so path is provided by environment variable, so neither runs in a default CI pass.

| File | Skip guard | Tests | Coverage |
|------|-----------|------:|----------|
| ext-storage-json-module.tcl | JSON_MODULE_PATH unset or missing (line 13) | 9 | baseline ops (54), explicit spill (60), fetch round-trip (68), modify after round-trip (74), pressure spilling (79, 88), write to a spilled doc (94), DEL (117) and expire (125) of a flash-resident module key |
| ext-storage-bloom-module.tcl | BLOOM_MODULE_PATH unset or missing (line 21) | 8 | baseline ops (62), explicit spill (70), membership preserved across round-trip (79), add after round-trip (86), pressure spilling (91, 102), DEL (109) and expire (119) of a flash-resident filter |

Both files' headers name the same regression they guard: a `moduleNotifyKeyUnlink` crash on the
tiered placeholder when a flash-resident module key is deleted or expires, fixed by an
`objectIsTiered` guard in module.c (`src/module.c:13292`).

## ext-storage.tcl — dict-invariant suite

ext-storage.tcl (12 tests) asserts the core v1 invariant — keys stay in the dict after their
values spill — plus the read/lifecycle surface against a module-backed backend:

| Test (line) | Asserts |
|-------------|---------|
| keys remain in dict after spill (line 50) | DBSIZE = full key count after spilling |
| EXISTS returns 1 for tiered keys (line 68) | existence answered from the dict |
| GET fetches value from disk (line 79) | transparent fetch on read |
| TTL preserved across spill/fetch (line 86) | TTL ≈ unchanged |
| TYPE works on tiered keys (line 96) | type reported without losing tiering |
| SCAN includes tiered keys (line 101) | enumeration unaffected |
| DEL works on tiered keys (line 117) | removal |
| SET overwrites tiered key (line 122) | overwrite |
| OBJECT ENCODING on tiered key (line 128) | raw/embstr after fetch |
| INFO shows spill/fetch metrics (line 134) | spill and fetch counters advance |
| MGET fetches multiple tiered keys (line 141) | multi-key fetch |
| EXPIRE on tiered key works (line 148) | TTL set on a tiered key |

> ⚠️ CONTRADICTION: this file loads its backend from a module path under modules/key-spilling/
> (filename libkey_spilling_module.so, backend=rocksdb, set at line 9), but no
> modules/key-spilling/ directory exists in this repo. The in-tree module directories are
> modules/flash-tiering/ (the flash-tiering module used by the ext-storage-module-* suites),
> modules/rocksdb-tiering/, and modules/valkeymodule-rs/ (Rust binding scaffolding); the
> flashcache and mock backends are compiled into the core under src/storage/ (storage_mock.c,
> storage_flashcache_real.c, storage_dispatch.c, storage_middleware.c), not built as loadable
> modules. Because of the file-existence guard at lines 12-16, the suite currently **always
> SKIPs** — its 12 tests do not run until the module path is corrected (most likely to
> modules/flash-tiering/, which the module-* suites already use). See [backends](backends.md).

## introspection.tcl — config immutability

The staged change to introspection.tcl (lines 1277–1280) adds `ext-storage-enabled`,
`ext-storage-backend`, `ext-storage-path`, and `ext-storage-capacity-mb` to the test's
skip_configs list. These directives are IMMUTABLE_CONFIG and cannot be round-tripped via
CONFIG SET at runtime, so the introspection "set every config" sanity test must skip them.
See [config-and-module-args](../interfaces/config-and-module-args.md).

## What is not covered

- **Plain (non-preamble) AOF rewrite drops tiered keys.** Both non-preamble rewrite loops skip
  tiered objects with a warning (`src/aof.c:2446`, `src/aof.c:2504`); no test asserts that
  behaviour, and no test runs with `aof-use-rdb-preamble no`.
- **Slot migration with tiering is unsupported and untested** — same skip sites.
- **No replication test.** No file in tests/unit/data-tiering/ exercises a replica full-sync
  with tiered values.
- **ext-storage.tcl is dormant** until its module path is fixed (see the contradiction above).
- **The module-backend and OBJ_MODULE suites are conditional.** 6 of the 19 files — 66 of the
  225 test blocks (ext-storage.tcl 12, module-configs 16, module-expiry-eviction 18,
  module-spill-fetch 3, json-module 9, bloom-module 8) — sit behind a build or environment
  guard and do not run in a default pass. That leaves 159 blocks in the unconditional set.
- **Pass/fail state is not asserted here.** Test counts and line numbers on this page were
  derived statically from the .tcl sources; the suite was not executed during this pass.

## Related pages

[spill](../flows/spill.md) · [fetch](../flows/fetch.md) ·
[serialization](serialization.md) · [eviction-integration](eviction-integration.md) ·
[persistence-replication](persistence-replication.md) · [backends](backends.md) ·
[config-and-module-args](../interfaces/config-and-module-args.md) ·
[info-metrics](../interfaces/info-metrics.md) ·
[known-limitations](../decisions/known-limitations.md)
