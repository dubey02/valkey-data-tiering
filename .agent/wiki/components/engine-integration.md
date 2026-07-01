---
title: Engine Integration
status: active
sources:
  - src/ext_storage.c:79-1168
  - src/server.c:1911
  - src/server.c:2036
  - src/server.c:4017
  - src/server.c:4718-4735
  - src/evict.c:540-555
  - src/networking.c:4309
updated: 2026-06-10
type: component
tier: working
claim_count: 10
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

> All tiering logic lives on the **main thread** in `ext_storage.c` and is wired into the
> event loop at four points: a throttle gate on intake, the `preCommandExec` block gate
> before `call()`, a completion-drain + weak spill pump in `beforeSleep`, and an aggressive
> pump + eviction override inside `performEvictions`. The backend IO thread does the disk
> work and serialization; the main thread only submits, transitions state, and drains.

![Event loop](../diagrams/event-loop.png)

## Event-loop hook points

| Hook | Where | What it does |
|------|-------|--------------|
| Throttle gate | `networking.c:4309` | `extStorageThrottle_shouldThrottle(c)` queues the client under memory pressure before the command runs ([throttle](throttle-equilibrium.md)) |
| Block gate | `server.c:4718` → `preCommandExec` (`ext_storage.c:444-593`) | blocks the client if a key is tiered/in-flight; issues fetch/delete |
| Post-gate re-check | `server.c:4721-4735` | if a key became `TIERED` between the gate and `call()`, re-runs `preCommandExec` (closes a spill/command race) |
| Drain + weak pump | `beforeSleep` (`server.c:1911`, again `server.c:2036`) + 1 ms timer (`ext_storage.c:311`) | `processCompletedStorageRequestsAndSpillOldItems()` |
| Aggressive pump + eviction override | `performEvictions` (`evict.c:545-551`) | `…Aggressive()` then `extStoragePerformEvictions()` |
| Latency feedback | `server.c:4017` | `extStorageThrottle_recordCommandLatency(duration)` after each command |

## The block gate (`preCommandExec`, `ext_storage.c:444-593`)

For each key of the command, `keyBlocksClient` (`ext_storage.c:359-434`) decides per the
[state machine](state-machine.md): GET on `COPYING_TO_FLASH` serves from RAM (value still
present); SET/DEL on `COPYING_TO_FLASH` block; any command on `ONLY_FLASH`,
`COPYING_TO_MEMORY`, or `PENDING_EVICT` blocks. For an `ONLY_FLASH` key the gate issues the
IO — `extStorageBridge_submitDel` for DEL/UNLINK, else `extStorageBridge_submitGet`
(`:563`/`:565`) — flips the key to `COPYING_TO_MEMORY`, then `blockClientInUseOnKeys` (`:589`)
and returns `CMD_FILTER_REJECT` (`:592`). MULTI/EXEC checks every key of every queued command
(`:458-515`). The [fetch](../flows/fetch.md) and [delete](../flows/delete.md) flows expand
these paths.

## Completion drain (`processCompletedStorageRequests`, `ext_storage.c:602-794`)

Loops `extStorageBridge_pollCompletions` (`:606`) until the queue is empty — **never capped**.
Each completion runs its WRITE/READ/DELETE transition, frees or restores the value, then
`unblockClientsInUseOnKey(key)` (`:792`) so the unblocked command re-executes next iteration.
Detail: [completion-drain](../flows/completion-drain.md).

## The spill controller (cap-less, projected-gated)

Both spill entry points now delegate to one **cap-less** controller,
`spillFillToProjected` (`ext_storage.c:1009-1046`): a loop
`while (extStorageProjectedMemory() > server.maxmemory)` (`:1024`) that submits candidates
from `findBestEvictionCandidate(spillPoolLRU,…)` (`:1027`) until projected memory is back at the
setpoint. Queue depth is an **emergent output** of the projected-memory signal, not a fixed
constant — the old `SPILL_CONCURRENT_BASE`/LIMIT cap, `max_num_concurrent_items_spilled`, and the
throttle-driven `extStorageUpdateSpillConcurrency` were **removed** (removal comment `:100-104`).
The `items_spillover_batch_size` variable and its config directive still exist but the cap-less
loop **no longer reads them** (now dead config — see [known-limitations](../decisions/known-limitations.md)).

| Pump | Fn | Behaviour |
|------|----|-----------|
| Weak (per iteration) | `processCompletedStorageRequestsAndSpillOldItems` (`:1055-1061`) | drain completions, then `spillFillToProjected()` |
| Aggressive (per command, under eviction) | `…Aggressive` (`:1066-1071`) | drain completions, then `spillFillToProjected()` — now identical |

Both early-return when `maxmemory == 0`; `spillFillToProjected` itself early-returns on
`MAXMEMORY_NO_EVICTION` and when already at setpoint (`:1010-1011`). The two former pumps are
kept as separate symbols only for their existing call sites — their bodies are now identical.
The controller stops only on setpoint, no-candidate (`spill_skipped_null`, `:1028`), or
submit-queue backpressure (`spillItemAsync == -1`, `:1039`); there is **no memory-headroom
clamp**. See [eviction-integration](eviction-integration.md) and [memory-accounting](memory-accounting.md)
for the projected-memory Smith predictor, and [throttle-equilibrium](throttle-equilibrium.md)
for the now-decoupled throttle.

## Eviction override (`extStoragePerformEvictions`, `ext_storage.c:938-983`)

Called from `performEvictions` (`evict.c:551`); returns 1 (handled) and sets `*result`:
`C_OK` if `extStorageProjectedMemory() <= maxmemory` (`:945`), if items are in-flight, or if
`total_keys > num_items_on_flash` (spillable items remain); `C_ERR` at the hard cap
`used_memory > maxmemory + maxmemory/5` (1.2×, `:952-956`, raw `used_memory`) or when nothing is
left to spill. The over-budget decision reads **projected** memory; only the hard OOM cap reads
raw `used_memory`. Detail: [eviction-integration](eviction-integration.md).

## Spill mechanics (`spillItemAsync`)

Zero-copy: a borrowed value robj carrying the entry's real `type`/`encoding` is submitted
via the bridge (`ext_storage.c:846`), so the **IO thread** serializes every object type
(`createDumpPayload` in `extStorageSerializeValue`, `:238-243`) — the main thread never
serializes. Skips embedded, already-`TIERED`, or `refcount != 1` objects (`:813-820`). Each
submit calls `extStorageOnSpillSubmit()` (`:861`) to open window-1 of the Smith predictor (see
[memory-accounting](memory-accounting.md)). On WRITE-OK the drain frees the real RAM value and
tombstones the entry as `OBJ_ENCODING_TIERED` with an empty sds (`:672-675`). See
[serialization](serialization.md) and the [spill flow](../flows/spill.md).

## Metrics

The gate, drain, and pumps export the `kbc_*`, `completion_*`, and `spill_*` counters in
[info-metrics](../interfaces/info-metrics.md). API surface: [ext-storage-api](../interfaces/ext-storage-api.md).
