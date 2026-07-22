# JSON Module (OBJ_MODULE) × Data Tiering Compatibility

Tested 2026-07-20 on branch `json-module-test` (from unstable, commit 05a27e486)
with valkey-io/valkey-json built against this fork's valkeymodule.h
(`cmake3 -DBUILD_RELEASE=ON -DENABLE_UNIT_TESTS=OFF -DVALKEY_MODULE_H_PATH=<fork>/src/valkeymodule.h`).

## Result matrix

| Path | Result |
|---|---|
| Module load + JSON ops, tiering on | PASS |
| Explicit spill (DEBUG SPILL) of module key | PASS |
| Fetch + round-trip (mock and real FlashCache) | PASS — document byte-identical |
| Memory-pressure spill (3246 module keys, real FC, 20MB maxmemory) | PASS |
| Reads of flash-resident docs | PASS |
| Write (JSON.SET subpath) to flash-resident doc | PASS |
| DEL of flash-resident module key | **SIGSEGV** |
| EXPIRE→lazy-expire of flash-resident module key | same crash path (unlink) |

## Why spill/fetch works for OBJ_MODULE

`extStorageSerializeValue` uses `createDumpPayload`/`rdbSaveObject`, which for
OBJ_MODULE invokes the module's `rdb_save` callback; `rdbLoadObject` invokes
`rdb_load` on fetch. valkey-json's callbacks are pure serialization (no
main-thread-only API usage), so IO-thread execution works. NOT guaranteed for
all modules — any module whose rdb callbacks touch server state is unsafe on
the IO thread.

## The DEL crash (P0) — FIXED

1. Spill completion (ext_storage.c ~728): value replaced with empty-sds
   placeholder, `encoding = OBJ_ENCODING_TIERED`, **`entry->type` remains
   OBJ_MODULE**.
2. DEL of the flash key → delete completion → `dbGenericDeleteWithDictIndex`
   → `moduleNotifyKeyUnlink` (module.c ~13272).
3. `val->type == OBJ_MODULE` still true → casts placeholder sds to
   `moduleValue*` → `mv->type` reads heap garbage → SIGSEGV.
   Observed fault address 0x4007878787880 = payload 'x' bytes as pointer.

Crash stack: `moduleNotifyKeyUnlink ← dbGenericDeleteWithDictIndex ←
processCompletedStorageRequests ← aeProcessEvents` (main thread).

FIX (applied on branch json-module-test): `!objectIsTiered(val)` guard in
`moduleNotifyKeyUnlink` before the moduleValue cast — same pattern as the
pre-existing guards in `decrRefCount` (object.c:663) and
`lazyfreeGetFreeEffort` (lazyfree.c:141; that one already covered the
free-effort dispatch, so no second fix was needed). Semantics documented in
the code comment: generic KEY event + keyspace notifications still fire for
tiered keys; the owning type's unlink/unlink2 callback is deliberately NOT
called (module's in-memory object was destroyed at spill; module got its
`free` callback then). Regression tests un-gated in
ext-storage-json-module.tcl — DEL + EXPIRE of flash-resident module key now
pass (9/9 suite green; ext-storage-data-types 60/60 unaffected).

## Module ecosystem survey: who registers unlink/unlink2?

- valkey-json: NO (only `free`, json.cc:2816)
- valkey-bloom: NO (explicitly `unlink: None, unlink2: None`, data_type.rs:42,49)
- valkey-search: NO — owns no keyspace values at all (module type is
  aux-callbacks-only, rdb_serialization.cc:288); tracks key add/delete via
  keyspace event subscriptions (KeyspaceEventManager), which work for tiered
  keys. Its reindex-on-write path calls ValkeyModule_OpenKey → lands on the
  separate "module opens tiered key" P0 gap.
- Only in-tree users of unlink: core test modules (datatype2.c, test_lazyfree.c).

Long-term (design doc): tiering-aware module API — opt-in unlink variant
accepting a non-resident value (key name only).

## Related pre-documented hazard

storage_flashcache_real.c:191 — IO-thread PUT calls
`objectComputeSize(NULL, ...)`; the OBJ_MODULE branch passes NULL key into
the module's `mem_usage2` ctx. valkey-json survives (ignores key). Modules
that read `ctx->from_key` would crash.

## Tests

`tests/unit/data-tiering/ext-storage-json-module.tcl`
- 9 tests (7 functional + 2 crash regressions), run with:
  `JSON_MODULE_PATH=/tmp/valkey-json/build/src/libjson.so ./runtest --single unit/data-tiering/ext-storage-json-module`

`tests/unit/data-tiering/ext-storage-bloom-module.tcl` (added 2026-07-21)
- 8 tests, same matrix for valkey-bloom (Rust module — different rdb
  callbacks/allocator exercised on IO threads). Build: cargo build --release
  in /tmp/valkey-bloom. Run with:
  `BLOOM_MODULE_PATH=/tmp/valkey-bloom/target/release/libvalkey_bloom.so ./runtest --single unit/data-tiering/ext-storage-bloom-module`
- All 8 pass including DEL/EXPIRE of flash-resident filter (unlink-guard
  regression, validated for a second module type).
- bloom's mem_usage is the v1 callback (value-only, bloom_callback.rs:110) —
  the objectComputeSize(NULL) key-ctx hazard does not apply to bloom (ctx is
  only built for mem_usage2).
- Pressure-test note: default bloom filters are ~300B; use
  `BF.RESERVE <key> 0.01 20000` (~25KB bitmap) to build memory pressure.

## DEL of flash key replies 0: FIXED (internal-TS style pending-deletion)

Was: optimized flash DEL removed the entry inside the delete completion, so
the re-executed DEL found nothing — reply 0, no signalModifiedKey (WATCH),
no keyspace "del" notification, no dirty++.

Fix (mirrors internal amz_tiered_storage.c MSG_TYPE_DELETE_KEY handling):
- New TIERING_STATE_PENDING_DELETION (=5, fits 3-bit field). The delete
  completion (client-DEL case = COPYING_TO_MEMORY + non-expired TTL) keeps
  the entry and marks it PENDING_DELETION instead of dbDelete.
- keyBlocksClient: PENDING_DELETION lets DEL/UNLINK through; blocks all
  other commands until the DEL drains. Orphan guard: if no blocked client
  with a pending DEL exists (deleting client disconnected), the next command
  finishes the deletion inline with full side effects.
- db.c dbGenericDeleteWithDictIndex: physical delete of a PENDING_DELETION
  entry calls unblockClientsInUseOnKey to release queued waiters.
- GOTCHA (cost a debug cycle): server.c has a TIERED_SAFETY re-check after
  preCommandExec that re-runs the filter for any key with TIERED encoding and
  returns WITHOUT executing. PENDING_DELETION keys keep the TIERED placeholder
  legitimately — without an exemption there, the re-executed DEL is silently
  dropped (filter accepts, command never called, client hangs forever). Any
  future state that leaves TIERED encoding visible in command context needs
  the same exemption.
- Expiry-triggered and GC-eviction deletes unchanged (no client waiting).
- MULTI/EXEC path passes is_delete_cmd=false (EXEC waits for the DEL to drain).
- New metric: kbc_pending_deletion_block.

Result: DEL/UNLINK of flash keys return correct counts, fire the keyspace
"del" notification (valkey-search-relevant), invalidate WATCH, bump dirty.
Tests: ext-storage-del-semantics.tcl (9 tests: reply counts, mixed keys,
notification, WATCH abort, queued-GET ordering, dirty counter). json/bloom
suites updated to assert DEL returns 1. Full pass: del-sem 9/9, json 9/9,
bloom 8/8, data-types 60/60.

## Sync-fetch primitive: IMPLEMENTED (2026-07-21, branch json-module-test)

Design: .agent/knowledge/sync-fetch-design.md. Fixes SORT BY/GET
crash + Lua undeclared-key wrong data. Key implementation facts:
- Hook lives in lookupKey (db.c) — single funnel. Trigger: LOOKUP_SYNCFETCH
  flag OR server.execution_nesting > 1. NOTE: every top-level command runs at
  nesting 1 (enterExecutionUnit in call()); script/EXEC-inner commands run at
  >= 2 — the threshold is > 1, not > 0.
- processCompletedStorageRequests refactored: per-completion body extracted
  into processOneCompletion(); deferred_completions list consumed first.
- Miss detection in the sync loop uses a completion_read_miss counter delta
  (keys_confirmed_absent has NO consumers — write-only, unreliable).
- Anomaly resolved: Lua strlen "15" was sdigits10() of the placeholder heap
  POINTER (stringObjectLen INT-encoding branch); Lua GET garbage was
  addReply's "-ERR value on flash" defensive bytes. No hidden
  serialized-bytes installer exists.
- Latent wedge noted (not fixed): async read-miss leaves a TIERED placeholder
  with state removed; KBC defensive branch would resubmit forever. Unhit
  because mock never misses; real FC GC can trigger it. Follow-up needed.
- Tests: ext-storage-sync-fetch.tcl (9). Full pass across 5 suites: 95/95.

## Session 2026-07-22 additions

- FIXED objectGetExpire type-confusion (3 sites in ext_storage.c): passing
  the placeholder sds as robj read garbage hasexpire bits — KBC could turn a
  READ into a DELETE => flaky key loss (the data-types TYPE flake). Pattern
  rule: expire lives on the ENTRY; never pass objectGetVal(entry) to
  objectGetExpire.
- FIXED read-miss wedge: miss completion deletes the tiered placeholder
  (was: infinite KBC resubmit loop, live-reproduced via SWAPDB).
- GATED SWAPDB (flash data addressed by db id; in-flight completions carry
  db ids — swap strands/misroutes them).
- SAVE/BGSAVE/rdbSave fail-loudly gates + DEBUG OBJECT tiered-info reply
  (was assert crash). AOF rewrite loss documented as KNOWN LIMITATION test
  (persistence/replication unsupported — design doc scope).
- Blocking commands validated: BLPOP/BLMOVE/BLMPOP immediate-pop on flash
  lists, BLPOP wake-by-RPUSH, XREAD BLOCK woken after stream spilled while
  reader was blocked. Suite: ext-storage-blocking.tcl.
- Test-infra lesson: framework per-test timeouts fire under concurrent
  builds on this box (CPU starvation, client crawls ~700 cmds/s) — check
  system load before declaring a suite regression.
