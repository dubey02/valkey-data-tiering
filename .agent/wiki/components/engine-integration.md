---
title: Engine Integration
status: active
sources:
  - src/ext_storage.c:31-1620
  - src/ext_storage.h:39-143
  - src/server.c:1912
  - src/server.c:2037
  - src/server.c:4019
  - src/server.c:4747-4776
  - src/server.h:3762
  - src/evict.c:528-540
  - src/networking.c:4336-4344
  - src/db.c:83-98
  - src/db.c:527-536
  - src/db.c:864
  - src/db.c:1895-1937
  - src/expire.c:80
  - src/config.c:2620-2635
  - src/blocked.c:945-992
  - src/sort.c:116-121
  - tests/unit/data-tiering/ext-storage-swapdb.tcl:1-203
  - tests/unit/data-tiering/ext-storage-sync-fetch.tcl:1-141
updated: 2026-07-30
type: component
tier: working
claim_count: 13
edges:
  - to: components/state-machine.md
    kind: depends_on
    source: human
    created: 2026-06-03
  - to: components/bridge-layer.md
    kind: calls
    source: human
    created: 2026-06-03
    note: spill/fetch/del submitted via the bridge
  - to: components/throttle-equilibrium.md
    kind: calls
    source: human
    created: 2026-06-03
    note: records latency, reads throttle rate
  - to: components/serialization.md
    kind: depends_on
    source: human
    created: 2026-06-03
    note: borrowed robj serialized on the IO thread
  - to: components/eviction-integration.md
    kind: depends_on
    source: llm_relation
    created: 2026-06-04
    note: spill candidate selection + eviction override
  - to: flows/completion-drain.md
    kind: refers_to
    source: human
    created: 2026-06-03
  - to: flows/spill.md
    kind: refers_to
    source: llm_relation
    created: 2026-06-04
  - to: flows/fetch.md
    kind: refers_to
    source: llm_relation
    created: 2026-06-04
    note: owns the sync-fetch flow narrative; this page owns the hook point
  - to: flows/delete.md
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
  - to: interfaces/config-and-module-args.md
    kind: refers_to
    source: llm_relation
    created: 2026-07-30
    note: maxmemory-policy guard at init and at runtime CONFIG SET
  - to: components/memory-accounting.md
    kind: refers_to
    source: llm_relation
    created: 2026-06-08
    note: cap-less spill loop gates on projected memory
  - to: decisions/known-limitations.md
    kind: refers_to
    source: llm_relation
    created: 2026-06-08
    note: items_spillover_batch_size now dead config
---

# Engine Integration

> All tiering logic lives on the **main thread** in `ext_storage.c`. Four hooks are in the
> event loop — a throttle gate on intake, the `preCommandExec` block gate before `call()`, a
> completion drain + weak spill pump in `beforeSleep`, and an aggressive pump + eviction
> override inside `performEvictions` — plus a fifth hook **inside** command execution:
> `extStorageSyncFetch` from `lookupKey`, for contexts that cannot block-and-re-execute. The
> backend IO thread does the disk work and serialization; the main thread only submits,
> transitions state, and drains.

![Event loop](../diagrams/event-loop.png)

> ⚠️ The diagram predates the mid-execution sync-fetch hook and shows only the four
> event-loop hooks. `diagrams/event-loop.dot` needs a `lookupKey → extStorageSyncFetch` node.

## Event-loop and command-path hook points

| Hook | Where | What it does |
|------|-------|--------------|
| Throttle gate | `networking.c:4342` | `extStorageThrottle_shouldThrottle(c)` queues the client under memory pressure before the read is parsed ([throttle](throttle-equilibrium.md)) |
| Block gate | `server.c:4747` → `preCommandExec` (`ext_storage.c:573-740`) | blocks the client if a key is tiered/in-flight; issues fetch/delete |
| Post-gate re-check | `server.c:4750-4776` | if a key became `TIERED` between the gate and `call()`, re-runs `preCommandExec`; `PENDING_DELETION` is exempt (`server.c:4759-4766`) |
| **Mid-execution sync fetch** | `lookupKey` (`db.c:92-98`) → `extStorageSyncFetch` (`ext_storage.c:1078-1158`) | resolves a non-resident value **without** blocking, for callers that cannot re-execute |
| Drain + weak pump | `beforeSleep` (`server.c:1912`, again `server.c:2037`) + 1 ms timer (`ext_storage.c:312`, registered `:412`) | `processCompletedStorageRequestsAndSpillOldItems()` |
| Aggressive pump + eviction override | `performEvictions` (`evict.c:531-539`) | `…Aggressive()` then `extStoragePerformEvictions()` |
| Latency feedback | `server.c:4019` | `extStorageThrottle_recordCommandLatency(duration)` after each command |
| Init | `extStorage_init` (`ext_storage.c:366-416`) | policy guard, db-id identity map, backend + throttle init, timer registration |

`extStorageIsInitialized()` (`ext_storage.c:338-340`) is the "tiering is live" predicate — it
reports whether `extStorage_init` got as far as allocating the db-id maps, and is what other
subsystems test before applying tiering-specific rules (see [Guards](#guards)).

## SWAPDB: logical vs physical db ids

The backend is keyed by a **physical** db id, not the `server.db[]` index. `extStorage_init`
identity-maps the two (`ext_storage.c:385-391`); `SWAPDB` swaps the mapping entries
(`extStorageSwapDbIds`, `ext_storage.c:352-360`, called from `db.c:1931` after
`dbSwapDatabases`). After a swap, **logical db N is not backend namespace N**.

The rule is directional and mechanical:

| Direction | Call | Applies to |
|---|---|---|
| logical → physical | `extStoragePhysicalDbId(db->id)` | **every** call that addresses the backend: `submitGet`, `submitPut`, `submitDel`, `flushDB`, `forkRead` |
| physical → logical | `extStorageLogicalDbId(msg->db_id)` | the completion path, which is handed a physical id and must find the owning `serverDb` |

Both are identity when the maps are unallocated (`ext_storage.c:343`, `:348`), so the calls are
safe before init and on non-tiering builds. Getting the direction wrong does not fault — it
silently reads or writes **another database's** keyspace, which is why every site translates
explicitly rather than relying on a wrapper.

Complete inventory of translating sites in the tree:

- **Submits (logical → physical):** `ext_storage.c:625` (MULTI/EXEC fetch), `:710`/`:712`
  (block-gate delete/fetch), `:1099` (sync fetch), `:1205` (spill), `:1245`
  (`extStorageEvictFlashKey`), `:1797` (fork-child materialize); `expire.c:80` (active-expire
  delete); `db.c:864` (FLUSHDB); `debug.c:1133`, `debug.c:1189` (DEBUG spill helpers).
- **Completion (physical → logical):** `ext_storage.c:754-759` — `msg->db_id` is physical and
  is resolved to `server.db[extStorageLogicalDbId(db_id)]`. This is correct even for IO in
  flight across a swap, because `dbSwapDatabases` moves the entries *and* their tiering-state
  bits together with the keyspace.
- **The one deliberate non-translating call:** `ext_storage.c:934`, the
  `READ_RETRY` resubmit inside the completion handler, passes `msg->db_id` straight through —
  it is **already** physical. Translating there would be the bug.

Test coverage: tests/unit/data-tiering/ext-storage-swapdb.tcl.

## Mid-execution synchronous fetch

`extStorageSyncFetch(db, key)` (`ext_storage.c:1078-1158`) is the escape hatch for call sites
that cannot use the block-and-re-execute protocol. `lookupKey` invokes it (`db.c:95`) when the
value is non-resident **and** either `server.execution_nesting > 1` (Lua / EXEC-inner commands)
or the caller opted in with `LOOKUP_SYNCFETCH` (`server.h:3762`) — currently only SORT's
`BY`/`GET` pattern resolution (`sort.c:116-121`). It stalls the main thread, never times out
(a Lua script may already have applied writes, so there is nothing to roll back), and warns
every 5 s while stalled (`ext_storage.c:1141-1147`). On return the caller must re-find the
entry (`db.c:96-97`); a surviving placeholder is reported as a normal key miss.

**Drain-ordering consequence — the part that concerns this page:** sync fetch polls the bridge
itself and processes **only its own key's** completions, appending every other completion to a
`deferred_completions` list (`ext_storage.c:1112-1130`). Those deferred messages are then
processed **first**, ahead of anything still queued in the bridge, on the next drain
(`processCompletedStorageRequests`, `ext_storage.c:1040-1051`). Arrival order is preserved
across the two sources; per-key order is safe regardless, because a key has at most one
in-flight operation. Deferral exists for keyspace isolation: a spill completion processed
mid-command could free memory the running command still references. Counted as
`sync_fetch_deferred_completions`. Flow detail: [fetch](../flows/fetch.md). Test coverage:
tests/unit/data-tiering/ext-storage-sync-fetch.tcl.

## The block gate (`preCommandExec`, `ext_storage.c:573-740`)

For each key of the command, `keyBlocksClient` (`ext_storage.c:460-563`) decides per the
[state machine](state-machine.md): GET on `COPYING_TO_FLASH` serves from RAM (value still
present); SET/DEL on `COPYING_TO_FLASH` block; any command on `ONLY_FLASH`,
`COPYING_TO_MEMORY`, or `PENDING_EVICT` blocks. `PENDING_DELETION` (`:499-525`) is the
asymmetric case: DEL/UNLINK pass **through** so the re-executed command performs the keyspace
removal with full command-layer side effects, everything else blocks — unless the deleting
client has vanished, in which case the gate finishes the deletion inline (`:516-521`).

For an `ONLY_FLASH` key the gate issues the IO — `extStorageBridge_submitDel` for DEL/UNLINK,
else `extStorageBridge_submitGet` (`:710`/`:712`) — flips the key to `COPYING_TO_MEMORY`
(`:723`), then `blockClientInUseOnKeys` (`:736`) and returns `CMD_FILTER_REJECT` (`:739`). Two
refinements on that path: an already-expired key is converted from a READ into a DELETE so the
fetch is never issued (`:690-706`), and a backend rejection (throttled) un-blocks the key and
marks it confirmed-absent instead (`:715-720`). MULTI/EXEC checks every key of every queued
command (`:592-645`). The [fetch](../flows/fetch.md) and [delete](../flows/delete.md) flows
expand these paths.

## Completion drain (`processCompletedStorageRequests`, `ext_storage.c:1035-1061`)

Deferred sync-fetch completions first (`:1044-1051`), then loops
`extStorageBridge_pollCompletions` (`:1053`) until the queue is empty — **never capped**. Each
message goes through `processOneCompletion` (`:752-1027`), which runs the WRITE/READ/DELETE
transition, frees or restores the value, then `unblockClientsInUseOnKey(key)` (`:1023`) so the
unblocked command re-executes next iteration. A DELETE that was issued for a client DEL leaves
the entry in `PENDING_DELETION` rather than removing it (`:1003`); the eventual `dbDelete`
releases the clients queued behind it (`db.c:534`). Detail:
[completion-drain](../flows/completion-drain.md).

## The spill controller

Two strategies are selectable at runtime by `ext_storage_spilling_strategy`
(`ext_storage.h:106-112`), and both pumps dispatch on it (`ext_storage.c:1527`, `:1539`):

| Strategy | Controller | Behaviour |
|---|---|---|
| `SPILLING_STRATEGY_V2` (default) | `spillFillToProjected` (`ext_storage.c:1378-1420`) | cap-less, projected-memory gated; identical for both pumps |
| `SPILLING_STRATEGY_V1` (legacy A/B) | `spillItemCountBeforeSleep` (`:1454-1487`) / `spillItemCountAggressive` (`:1489-1512`) | item-count batches at 1.0× / 1.1× raw `used_memory`, bounded by `items_spillover_batch_size` and the dynamic `max_num_concurrent_items_spilled` cap |

V2 is the design: a loop `while (extStorageProjectedMemory() > server.maxmemory)` (`:1393`)
submitting candidates from `findBestEvictionCandidate(spillPoolLRU,…)` (`:1396`) until
projected memory is back at the setpoint. Queue depth is an **emergent output** of the
projected-memory signal, not a fixed constant. Stop conditions are setpoint reached,
no candidate (`spill_skipped_null`, `:1397`), or submit-queue backpressure
(`spillItemAsync == -1`, `:1412`) — there is **no memory-headroom clamp**, deliberately:
halting on memory pressure would starve the disk and *raise* memory, since only completions
free RAM. `items_spillover_batch_size` is read only by V1, so under the default strategy the
directive is dead config (see [known-limitations](../decisions/known-limitations.md)).

> ⚠️ CONTRADICTION (code vs code): `spillFillToProjected` also breaks on
> `if (spill_submitted >= 64)` (`ext_storage.c:1416`), commented as a "cold-start brake: yield
> to event loop". But `spill_submitted` is a **cumulative** process-lifetime counter
> (`ext_storage.c:149`, exported as the `spill_submitted` INFO field) and is never reset, so
> after the 64th spill of the process the loop breaks after **one** submit per invocation —
> permanently degrading the fill loop to one-submit-per-event-loop-iteration. Either the
> comment or the counter is wrong; a per-pass local was clearly intended. Flagged, not fixed
> (engine change, not a wiki change).

> ⚠️ CONTRADICTION (stale code comment): the comment block at `ext_storage.c:1273-1278` states
> `extStorageUpdateSpillConcurrency` "was removed" and that spill concurrency is no longer
> capped. The function is still defined at `:1430-1439`, still declared
> (`ext_storage.h:139-143`), and is still the actuator the legacy coupled throttle drives. The
> comment describes the V2-only world; the V1 A/B path outlived it.

| Pump | Fn | Behaviour |
|------|----|-----------|
| Weak (per iteration) | `processCompletedStorageRequestsAndSpillOldItems` (`:1522-1530`) | drain completions, then the selected controller |
| Aggressive (per command, under eviction) | `…Aggressive` (`:1534-1542`) | drain completions, then the selected controller |

Both early-return when `maxmemory == 0`; `spillFillToProjected` itself early-returns on
`MAXMEMORY_NO_EVICTION` and when already at setpoint (`:1379-1380`). Under V2 the two pump
bodies are identical and kept as separate symbols only for their existing call sites. See
[eviction-integration](eviction-integration.md) and [memory-accounting](memory-accounting.md)
for the projected-memory Smith predictor, and [throttle-equilibrium](throttle-equilibrium.md)
for the now-decoupled throttle.

## Eviction override (`extStoragePerformEvictions`, `ext_storage.c:1297-1352`)

Called from `performEvictions` (`evict.c:534`); always returns 1 (handled) once tiering is
enabled, and sets `*result`. Order matters and is deliberate:

1. `C_ERR` at the hard cap `used_memory > maxmemory + maxmemory/5` (1.2×, `:1310-1315`, raw
   `used_memory`). Checked **first**, so an inflated in-flight credit cannot pin projected
   memory below the setpoint while raw memory runs away.
2. `C_OK` if `extStorageProjectedMemory() <= maxmemory` (`:1320-1324`).
3. Otherwise (over setpoint, under the hard cap) **always** `C_OK` (`:1349-1351`) — the
   throttle owns back-pressure in this band. Exhaustion of spillable items is recorded as the
   `no_spillable_items_count` metric only (`:1336-1347`); it no longer produces `C_ERR`.

Detail: [eviction-integration](eviction-integration.md).

## Guards

| Guard | Where | Refuses |
|---|---|---|
| Startup `maxmemory-policy` | `extStorage_init` (`ext_storage.c:369-381`) | anything but `allkeys-lru`, `allkeys-lfu`, `noeviction` — logs and **disables tiering** rather than failing to start |
| Runtime `maxmemory-policy` | `updateMaxmemoryPolicy` (`config.c:2620-2635`) | the same set, but rejects the `CONFIG SET` with an error while tiering is live (gated on `extStorageIsInitialized()`); previously this bypassed the check silently |
| SWAPDB pending-DEL | `swapdbCommand` (`db.c:1918-1921`) via `blockedInUseDelClientExistsForDbs` (`blocked.c:971-992`) | the swap while any blocked-in-use client has a pending DEL/UNLINK on either db — reject-and-retry, so a re-executed DEL cannot address the swapped-in keyspace |
| Pending-DEL orphan | `keyBlocksClient` (`ext_storage.c:510-521`) via `blockedInUseClientWithPendingDeleteExists` (`blocked.c:951-962`) | nothing — it *detects* the deleting client having vanished and finishes the deletion inline |
| `DEBUG EXT-STORAGE-PAUSE-COMPLETIONS` | `ext_storage.c:1029-1033`, honoured at `:1038` | tests only: holds all completions (and the clients blocked on them) in flight to make in-flight windows deterministic |

The policy restriction itself (why `volatile-*` needs flash-aware sampling) belongs to
[eviction-integration](eviction-integration.md); the directive surface is
[config-and-module-args](../interfaces/config-and-module-args.md).

## Spill mechanics (`spillItemAsync`, `ext_storage.c:1167-1227`)

Zero-copy: a borrowed value robj carrying the entry's real `type`/`encoding` is submitted
via the bridge (`:1205`), so the **IO thread** serializes every object type
(`createDumpPayload` in `extStorageSerializeValue`, `:267-280`) — the main thread never
serializes. Skips embedded, already-`TIERED`, `refcount != 1`, and non-`ONLY_MEMORY` objects
(`:1172-1179`). Each submit calls `extStorageOnSpillSubmit()` (`:1220`) to open window-1 of the
Smith predictor (see [memory-accounting](memory-accounting.md)) before the state flips to
`COPYING_TO_FLASH` (`:1223`). On WRITE-OK the drain releases the window-2 credit (`:767`),
frees the real RAM value by its true type, and tombstones the entry as `OBJ_ENCODING_TIERED`
with an empty sds (`:817-818`). See [serialization](serialization.md) and the
[spill flow](../flows/spill.md).

## Metrics

The gate, drain, pumps, and sync fetch export the `kbc_*` (including
`kbc_pending_deletion_block`, `ext_storage.c:1562`), `sync_fetch_*` (`:1563-1567`),
`completion_*`, and `spill_*` counters from `genExternalStorageInfoString`
(`:1550-1591`) — catalogued in [info-metrics](../interfaces/info-metrics.md). API surface:
[ext-storage-api](../interfaces/ext-storage-api.md).
