---
title: Integration Tests (Tiering)
status: active
sources:
  - tests/unit/ext-storage-data-types.tcl:1-880
  - tests/unit/ext-storage-persistence.tcl:1-107
  - tests/unit/ext-storage.tcl:1-153
  - tests/unit/introspection.tcl:1277-1280
  - src/rdb.c:1195
  - src/rdb.c:1454
  - src/aof.c:2441
  - src/aof.c:2492
  - src/lazyfree.c:141
  - src/storage/storage_mock.c
  - src/config.c
  - src/ext_storage.c
  - src/ext_storage_bridge.c
updated: 2026-06-25
type: component
tier: working
claim_count: 74
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
    note: AOF/RDB crash-safety + persistence tests
  - to: components/backends.md
    kind: refers_to
    source: human
    created: 2026-06-17
    note: flashcache-mock default backend; ext-storage.tcl module load
  - to: interfaces/config-and-module-args.md
    kind: refers_to
    source: human
    created: 2026-06-17
    note: ext-storage-* config overrides; introspection skip_configs
  - to: interfaces/info-metrics.md
    kind: refers_to
    source: human
    created: 2026-06-17
    note: helpers poll the spill counter
  - to: decisions/known-limitations.md
    kind: refers_to
    source: human
    created: 2026-06-17
    note: AOF-reload test is the L1 regression marker
---

# Integration Tests (Tiering)

> Tcl integration tests under `tests/unit/` that validate data tiering: spill/fetch
> round-trip integrity for every data type, key lifecycle on tiered keys, and
> AOF/RDB crash-safety. This page maps each behavior to the test that covers it.

## Test files

| File | Tag(s) | Tests | Scope |
|------|--------|------:|-------|
| ext-storage-data-types.tcl | ext-storage, ext-storage-data-types | 60 | Spill/fetch data validity across all 6 structure types + key lifecycle + Lua/MULTI/EXEC/COPY/APPEND/PERSIST command surface + bash-migration backfill (MSET, RANDOMKEY, DBSIZE, Lua edge cases) |
| ext-storage-persistence.tcl | ext-storage, ext-storage-persistence | 2 | AOF rewrite crash-safety; AOF-reload persistence (regression marker) |
| ext-storage.tcl | ext-storage | 12 | dict-invariant suite (loads an external module backend) |
| introspection.tcl | introspection | — | Adds the 4 `ext-storage-*` configs to the immutable skip_configs list |

Line numbers below are positions of the `test {…}` blocks within the named file
(authored from `grep -n`).

## How the tiering tests run

The ext-storage-data-types.tcl and ext-storage-persistence.tcl files start a server with
`ext-storage-enabled yes`, maxmemory 10mb, and maxmemory-policy allkeys-lru. The
backend defaults to `flashcache-mock` (in-memory, no disk needed) but is overridable to the
real FlashCache backend via the EXT_STORAGE_BACKEND / EXT_STORAGE_PATH / EXT_STORAGE_CAPACITY_MB
environment variables, which feed `ext-storage-backend`, `ext-storage-path`, and
`ext-storage-capacity-mb` respectively.

Both files share two Tcl helpers:

- a get_spill_count helper that scrapes the INFO field
  `total_num_items_spilled_to_ext_storage` (see [info-metrics](../interfaces/info-metrics.md)); and
- a debug_spill helper that issues DEBUG SPILL on a key and polls that counter until it
  increments, giving a deterministic "value is now on flash" barrier before the test reads it
  back. Reading the key afterwards drives the [fetch](../flows/fetch.md) path.

`flashcache-mock` is the in-tree mock backend; its IO worker serializes on PUT and
deserializes on GET via `extStorageSerializeValue` / `extStorageDeserializeValue`
(`src/storage/storage_mock.c`), so the round-trip exercises the real
serialization contract, not a raw memcpy. See [backends](backends.md),
[serialization](serialization.md).

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
| Stream | small + large (500 entries) | XLEN, XRANGE, XINFO | 254, 268, 538 |

Encoding-specific correctness asserted: ZSet float-score precision and duplicate-score lex
tie-breaking (lines 198, 241); intset membership after round-trip (line 176); and an explicit
OBJECT ENCODING-preservation test confirming listpack/hashtable/quicklist encodings survive
spill+fetch unchanged (line 630).

### Stream consumer-group state

Stream tests go beyond entries: consumer-group read/ack state must survive spill+fetch.

- single group, one read + one ack, verifies pending=0 and the remaining message is still
  unread after fetch (line 280);
- two groups with divergent read/ack progress, verifies each group's independent pending and
  unread counts after fetch (line 557).

## Coverage matrix — key lifecycle on tiered keys

| Behavior | What it asserts | Test (line) |
|----------|-----------------|-------------|
| TTL survives spill/fetch | TTL ≈ unchanged after spill then access, all 6 types | 331 |
| DEL on a tiered key | key removed; one test per type (string/hash/list/set/zset/stream) | 365, 372, 379, 386, 393, 400 |
| UNLINK on a tiered key | async-free path removes all 6 types | 589 |
| Overwrite while tiered | writing new fields/value reflects immediately | 407 |
| TYPE on tiered keys | correct type for all 6 types without explicit fetch | 416 |
| SCAN + TYPE on tiered keys | SCAN enumerates tiered keys with correct types | 439 |
| RENAME a tiered key | data preserved under the new name; old name gone | 461 |
| DUMP / RESTORE | DUMP yields a valid RDB payload that RESTOREs intact | 471 |
| Multi-key read (MGET) | crosses tiered + in-memory keys in one command | 607 |
| SUNIONSTORE | combines a tiered set with an in-memory set | 620 |
| Pressure-driven spill | natural (non-DEBUG) spill under a 2 MB cap preserves mixed-type data | 483 |

The pressure-driven test (line 483) triggers spilling through the
[eviction-integration](eviction-integration.md) path rather than DEBUG SPILL; it lowers
`maxmemory` to 2 MB, populates mixed types, waits for the spill counter to advance, then
verifies data integrity for keys that were admitted. The Lua INCRBY test (line 710) is the
only other test that spills via natural LRU pressure rather than DEBUG SPILL — it must,
because INT/EMBSTR-encoded numeric values are rejected by DEBUG SPILL (see
[known-limitations](../decisions/known-limitations.md)).

## Coverage matrix — scripting, transactions & extra commands

These 11 tests (all in ext-storage-data-types.tcl, appended after the lifecycle block) assert
that the transparent-fetch path works when tiered keys are touched indirectly — inside Lua
scripts, MULTI/EXEC transactions, and by COPY/APPEND/PERSIST. All spill via DEBUG SPILL except
the Lua INCRBY test (line 710), which uses natural LRU pressure.

### Lua scripting on tiered keys

| Test (line) | What it asserts |
|-------------|-----------------|
| EVAL reads tiered string (line 665) | `redis.call('GET', KEYS[1])` fetches a single tiered string verbatim |
| EVAL reads multiple tiered keys (line 672) | two tiered keys fetched and concatenated in one script |
| EVAL writes then reads different tiered key (line 686) | `SET KEYS[1]` + `GET KEYS[2]`; the read of the tiered key returns its value while another key is written |
| EVAL hash operations on tiered key (line 697) | `HGET` of an existing field + `HSET`/`HGET` of a new field within one script |
| EVAL INCRBY on tiered integer string (line 710) | pressure-spilled INT value is fetched, INCRBY 50 applied, `GET` returns the updated number |

### MULTI/EXEC transactions on tiered keys

| Test (line) | What it asserts |
|-------------|-----------------|
| MULTI/EXEC reads tiered key (line 731) | queued `GET` inside a transaction returns the tiered value on EXEC |
| MULTI/EXEC write + read tiered key (line 741) | queued `APPEND` then `GET`; EXEC returns the appended result |
| WATCH on tiered key aborts on modification (line 752) | a WATCHed tiered key modified before EXEC causes the transaction to abort (EXEC returns empty) |

### COPY / APPEND / PERSIST on tiered keys

| Test (line) | What it asserts |
|-------------|-----------------|
| COPY on tiered key (line 771) | `COPY src dst` duplicates a tiered value; source still exists |
| APPEND on tiered string (line 780) | `APPEND` fetches, appends, and the combined value reads back |
| PERSIST removes TTL on tiered key (line 788) | `PERSIST` clears the TTL (`TTL` = -1) while the value survives |

## Coverage matrix — command-surface & Lua edge cases (bash-migration backfill)

These 7 tests (ext-storage-data-types.tcl lines 800–870, under the "MISSING FROM UPSTREAM BASH
MIGRATION" banner) backfill cases that the deleted upstream `.sh`/`.py` suites covered but the
initial Tcl port omitted. Most spill via DEBUG SPILL; the exceptions are RANDOMKEY (line 811)
and DBSIZE (line 816), which are keyspace-introspection checks that run against the keys already
spilled by the preceding MSET test (line 800) rather than spilling their own.

| Test (line) | What it asserts |
|-------------|-----------------|
| MSET writes multiple tiered-eligible keys (line 800) | MSET of 3 keys; after 2 are spilled, all 3 read back verbatim (tiered + resident mix) |
| RANDOMKEY returns a key when tiered keys exist (line 811) | RANDOMKEY is non-empty when the keyspace contains tiered keys |
| DBSIZE includes tiered keys (line 816) | DBSIZE is non-zero — tiered keys stay counted in the dict |
| Lua conditional branch on tiered value (line 821) | EVAL fetches a tiered value and an if/else on its prefix takes the expected branch |
| Lua loop over multiple tiered keys (line 836) | EVAL loops over `#KEYS`, fetches each tiered value, and sums their numeric prefixes (= 60) |
| Lua pcall error handling on tiered key (line 855) | a Lua `pcall` of INCR on a non-numeric tiered value is caught as an error, not a crash |
| MULTI/EXEC APPEND on tiered key (line 870) | queued APPEND + GET in a transaction; EXEC returns the appended result |

## Persistence tests and crash-prevention guards

ext-storage-persistence.tcl covers the interaction of tiered objects with AOF/RDB.

| Test (line) | Asserts | Status |
|-------------|---------|--------|
| BGREWRITEAOF does not crash with tiered keys (line 58) | server survives an AOF rewrite with 6 tiered keys present; PING ok, DBSIZE intact | passes |
| Tiered keys persist across AOF reload (line 82) | tiered keys survive a DEBUG LOADAOF reload | **intentionally failing** — regression marker |

These tests exercise the staged `objectIsTiered` guards in the persistence paths:

- RDB save / AOF preamble: `rdbSaveKeyValuePair` returns `0` for a tiered value, omitting the
  **whole** key/value pair (`src/rdb.c:1195`). This is why BGREWRITEAOF (which uses the RDB
  preamble) does not crash — tiered keys are silently skipped — and equally why they do **not**
  survive reload (data loss). See [persistence-replication](persistence-replication.md),
  [known-limitations](../decisions/known-limitations.md) (L1).
- RDB fork child: `dismissObject` is called only for non-tiered objects (`src/rdb.c:1454`).
- Plain-command AOF rewrite: both the per-slot and whole-DB rewrite loops currently
  `serverAssert(false)` on a tiered object (`src/aof.c:2441` in `rewriteSlotToAppendOnlyFileRio`,
  `src/aof.c:2492` in `rewriteAppendOnlyFileRio`) — a `TODO: Handle tiered objects`. The default
  RDB-preamble rewrite path does not reach these asserts.
- Lazyfree: `lazyfreeGetFreeEffort` returns early for a tiered object (`src/lazyfree.c:141`),
  which is what makes the UNLINK-on-tiered test (line 589) safe.

> ⚠️ CONTRADICTION: the failing "Tiered keys persist across AOF reload" assertion encodes the
> *desired* behavior (tiered keys survive reload), but the code at `src/rdb.c:1195` deliberately
> drops them. The test is a forward-looking regression marker for unimplemented persistence,
> not a currently-passing guarantee. Tracked as L1 in
> [known-limitations](../decisions/known-limitations.md).

## ext-storage.tcl — dict-invariant suite

`ext-storage.tcl` (12 tests) asserts the core v1 invariant — keys stay in the dict after their
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
> (filename libkey_spilling_module.so, backend=rocksdb), but no modules/key-spilling/ directory
> exists in this repo. The only in-tree module directories are modules/rocksdb-tiering/ and
> modules/valkeymodule-rs/ (the Rust binding scaffolding); the flashcache and rocksdb backends are
> compiled into the core under src/storage/ (storage_mock.c, storage_flashcache_real.c),
> not built as loadable modules. Because of the
> file-existence guard at the top of the file, the suite currently **always SKIPs** — its 12 tests
> do not run until the module path is corrected (likely to the rocksdb-tiering module).
> See [backends](backends.md).

## introspection.tcl — config immutability

The staged change to introspection.tcl (lines 1277–1280) adds `ext-storage-enabled`,
`ext-storage-backend`, `ext-storage-path`, and `ext-storage-capacity-mb` to the test's
skip_configs list. These directives are IMMUTABLE_CONFIG and cannot be round-tripped via
CONFIG SET at runtime, so the introspection "set every config" sanity test must skip them.
See [config-and-module-args](../interfaces/config-and-module-args.md).

## What is not covered

- **Persistence is asserted, not achieved.** AOF/RDB reload of tiered keys fails by design
  (L1); there is no RDB-file (`SAVE`/`DEBUG RELOAD`) persistence test, only the AOF marker.
- **No replication test.** Tiering state is node-local and not replicated; no test exercises a
  replica full-sync with tiered keys.
- **Real backend is opt-in.** CI runs against `flashcache-mock`; the real FlashCache backend
  path is reachable only via environment variables and is not the default.
- **ext-storage.tcl is dormant** until its module path is fixed (see the contradiction above).

## Related pages

[spill](../flows/spill.md) · [fetch](../flows/fetch.md) ·
[serialization](serialization.md) · [eviction-integration](eviction-integration.md) ·
[persistence-replication](persistence-replication.md) · [backends](backends.md) ·
[config-and-module-args](../interfaces/config-and-module-args.md) ·
[info-metrics](../interfaces/info-metrics.md) ·
[known-limitations](../decisions/known-limitations.md)
