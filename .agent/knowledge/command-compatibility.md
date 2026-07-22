# Command Compatibility with Data Tiering (Non-Key-Spilling POC)

Status as of 2026-07-22, branch `json-module-test` (commit 5d1e68ffc). Derived from the command
table (`src/commands/*.json`, 425 commands) plus live testing against a
tiered keyspace (flashcache-mock, `DEBUG SPILL`).

## Why most commands just work

Every command that **declares key specs** (208 of 425) passes through the
KBC filter (`preCommandExec` → `keyBlocksClient`) before execution. If a key
is flash-resident, the client blocks, the value is fetched asynchronously,
and the command re-executes against the in-memory value. Compatibility is
therefore generic — per-command work is only needed where a command touches
keys it does not declare, or iterates the keyspace outside command context.

## Compatibility matrix

Legend: ✅ works · ⚠️ works with caveats · ❌ broken (bug, fixable) ·
🚫 unsupported (known feature gap) · ❓ untested

| Group | Commands | Status | Notes |
|---|---|---|---|
| string, hash, list, set, sorted_set, bitmap, geo, hyperloglog | all keyed (147) | ✅ | KBC fetch; round-trip integrity covered by ext-storage-data-types.tcl |
| stream (keyed) | XADD..XAUTOCLAIM (21) | ✅ | KBC; XGROUP/XINFO route via container key arg | 
| generic keyed | DEL, UNLINK, EXISTS, EXPIRE*, TTL/PTTL, PERSIST, RENAME, COPY, MOVE, DUMP, RESTORE, GETDEL, TOUCH, OBJECT * | ✅ | DEL/UNLINK: correct replies + side effects via PENDING_DELETION. DUMP/COPY/RENAME live-tested |
| metadata-reading keyed | TTL, OBJECT ENCODING/FREQ, MEMORY USAGE, TYPE | ⚠️ | Work, but **fetch the whole value back to DRAM** (promotion side effect). IO-amplifying for pure-metadata reads; candidate optimization: answer from entry metadata without fetch |
| keyspace iteration | KEYS, SCAN (incl. TYPE filter), RANDOMKEY, DBSIZE | ✅ | Key names + type live in the entry; values not dereferenced. Live-tested |
| flush | FLUSHDB, FLUSHALL | ✅ | Flash side flushed via `extStorageBridge_flushDB/All`. Live-tested |
| SWAPDB | | 🚫 gated | Flash values are addressed by db id — swap strands them (fetch miss). Live-proven: pre-gate, post-swap fetch missed and wedged KBC in an infinite resubmit loop. Now errors cleanly |
| transactions | MULTI/EXEC/DISCARD/WATCH | ✅ | EXEC path KBC-checks all queued commands' keys; WATCH invalidation on flash DEL live-tested |
| scripting, declared keys | EVAL/EVALSHA/FCALL with KEYS[] | ✅ | Live-tested (strlen on flash key = correct) |
| scripting, undeclared keys | EVAL touching keys not in KEYS[] | ✅ FIXED | Synchronous fetch primitive (`extStorageSyncFetch`, auto-triggered at execution_nesting > 1). Root cause of old wrong data: `stringObjectLen` counted digits of the placeholder POINTER; GET returned addReply's defensive error bytes. Tests: ext-storage-sync-fetch.tcl |
| SORT BY/GET patterns | SORT ... BY w_* GET p_* | ✅ FIXED | `lookupKeyByPattern` passes LOOKUP_SYNCFETCH → sync fetch. Covers BY, GET, and hash-field (`w_*->f`) patterns. Tests: ext-storage-sync-fetch.tcl |
| DEBUG OBJECT | | ✅ FIXED | Replies tiering-aware info (encoding:tiered, tiering_state, value_on_external_storage:1) instead of asserting. Tests: ext-storage-blocking.tcl |
| RDB persistence | SAVE, BGSAVE, DEBUG RELOAD | 🚫 gated (fail-loudly) | Unsupported; previously SILENT data loss (SAVE OK, reload yields empty values). Now SAVE/BGSAVE return a descriptive error and rdbSave refuses (covers auto-save, SHUTDOWN save, DEBUG RELOAD) while num_items_on_flash > 0. Tests: ext-storage-blocking.tcl |
| AOF | BGREWRITEAOF | 🚫 lossy (unsupported) | Confirmed: rewrite writes an RDB-preamble base that skips tiered entries and truncates the command tail — flash keys lost on reload. Documented via KNOWN LIMITATION test in ext-storage-persistence.tcl. Persistence design will replace |
| replication | REPLICAOF, PSYNC, SYNC, WAIT, FAILOVER | 🚫 | Known gap: full sync does not fetch tiered values. Design-doc scope |
| cluster | CLUSTER * (38) | 🚫 | Known gap: cluster mode unsupported with tiering |
| MIGRATE | | ❓ | DUMP half works (KBC); full MIGRATE untested |
| keyspace notifications | NOTIFY events | ✅ | "del" for flash keys live-tested (ext-storage-del-semantics.tcl) |
| modules | JSON, bloom commands | ✅ | Full matrix in ext-storage-json-module.tcl / ext-storage-bloom-module.tcl |
| module OpenKey on tiered key | e.g. valkey-search reindex | 🚫 | Known P0 gap: ValkeyModule_OpenKey on flash key |
| blocking commands | BLPOP, BLMOVE, BLMPOP, XREAD BLOCK | ✅ | Two blocking layers compose: KBC fetch first, then command blocking. Live-tested incl. stream spilled WHILE a reader was blocked (XADD fetches + wakes it). Tests: ext-storage-blocking.tcl |
| server/connection/ACL/CONFIG/INFO/pubsub/sentinel | 190 keyless admin commands | ✅ | No keyspace value access; unaffected by design |

## Bug ledger (audit 2026-07-20..22, branch json-module-test)

### Fixed on this branch (commit 5d1e68ffc)

1. **moduleNotifyKeyUnlink crash** — DEL/expiry/eviction/overwrite of a
   flash-resident MODULE key SIGSEGV'd (placeholder sds cast to moduleValue*).
   Fix: `!objectIsTiered()` guard; unlink/unlink2 deliberately not fired for
   tiered keys (module got its `free` at spill; generic KEY event + keyspace
   notifications still fire). Suites: json-module, bloom-module.
2. **DEL of flash key replied 0 + lost all side effects** (WATCH, "del"
   notification, dirty++). Fix: internal-TS-style TIERING_STATE_PENDING_DELETION
   — completion keeps the entry, re-executed DEL performs the real removal.
   Orphan guard + TIERED_SAFETY exemption. Suite: del-semantics.
3. **SORT BY/GET pattern crash** — pattern keys bypass the filter; SIGSEGV.
   Fix: sync-fetch primitive + LOOKUP_SYNCFETCH in lookupKeyByPattern
   (BY, GET, hash-field patterns). Suite: sync-fetch.
4. **Lua undeclared-key wrong data** — strlen returned digit-count of the
   placeholder heap POINTER; GET returned addReply's defensive error bytes.
   Fix: sync-fetch auto-trigger at execution_nesting > 1. Suite: sync-fetch.
5. **objectGetExpire type-confusion** (the data-types flake) — three sites
   passed the placeholder sds where an robj is expected; garbage
   hasexpire/expiry silently converted READs into DELETEs (random key loss).
   All sites now pass the entry. Validated: 3x clean suite runs.
6. **Read-miss wedge** — miss completion left a TIERED placeholder with
   cleared state => infinite KBC resubmit loop (live-reproduced via SWAPDB;
   also reachable via FlashCache GC). Miss now deletes the entry.
7. **RDB silent data loss => fail-loudly gates** — SAVE/BGSAVE/rdbSave refuse
   while values are on flash (descriptive client errors; rate-limited log for
   auto-save). Suite: blocking.
8. **DEBUG OBJECT assert crash** — now replies tiering info. Suite: blocking.
9. **SWAPDB stranded flash data** — flash values are addressed by db id;
   gated with a clear error. Suite: blocking.

### Still open

10. **AOF rewrite loses tiered keys** (unsupported feature, documented):
    RDB-preamble base skips tiered entries + rewrite truncates the command
    tail. Covered by the KNOWN LIMITATION test in ext-storage-persistence.tcl.
    Real fix = persistence design (see replication-design.md for the
    RDB_OPCODE_TIERED direction).
11. **Metadata reads over-fetch** (perf): TTL/OBJECT/MEMORY USAGE promote the
    full value to answer from metadata. Optimization candidate.
12. **keys_confirmed_absent dead code** — write-only hashtable, consumed
    nowhere. Cleanup candidate.
13. **Module OpenKey on tiered key** (P0 gap): sync-fetch Phase 2 wires
    extStorageSyncFetch into ValkeyModule_OpenKey.

## Testing status

- Full data-tiering regression 2026-07-22: **17 suites, 195/195 green**
  (mock backend). New suites this branch: json-module (9), bloom-module (8),
  del-semantics (9), sync-fetch (9), blocking (10); persistence converted to
  KNOWN-LIMITATION assertions (2).
- Flake note: one suite timeout observed under a concurrent engine build on
  the dev box (CPU starvation, client ~700 cmds/s) — check system load
  before declaring a regression.
- Remaining automation candidates: KEYS/SCAN/RANDOMKEY/FLUSHDB sweep as a
  dedicated suite; real-FlashCache (non-mock) run of the full directory.
