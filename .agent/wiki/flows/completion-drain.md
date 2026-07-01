---
title: Completion Drain
status: active
sources:
  - src/ext_storage.c:602-794
  - src/ext_storage.c:1055-1071
updated: 2026-06-10
type: flow
tier: working
claim_count: 6
edges:
  - to: components/engine-integration.md
    kind: refers_to
    source: human
    created: 2026-06-03
  - to: components/bridge-layer.md
    kind: refers_to
    source: llm_relation
    created: 2026-06-04
  - to: components/state-machine.md
    kind: refers_to
    source: llm_relation
    created: 2026-06-04
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
---

# Completion Drain

> Each event-loop iteration drains **all** backend completions (never capped), applies their
> state transitions, and unblocks waiting clients — then spills if still over `maxmemory`.

![Completion-drain sequence diagram](../diagrams/completion-drain-sequence.png)

<details>
<summary>Diagram source — <code>diagrams/completion-drain-sequence.mmd</code> (regenerate with <code>make -C ../diagrams seq</code>)</summary>

```mermaid
sequenceDiagram
    participant EL as event loop
    participant BS as beforeSleep / 1ms timer
    participant E as processCompletedStorageRequests
    EL->>BS: each iteration
    BS->>E: drain ALL completions (never capped)
    loop every completion
      Note over E: WRITE ok -> ONLY_FLASH (debit inflight RAM)<br/>READ ok -> ONLY_MEMORY + unblock<br/>DEL -> dbDelete
    end
    BS->>BS: spillFillToProjected(): submit while projected > maxmemory (cap-less)
    BS->>BS: blockedBeforeSleep() re-runs unblocked commands
```

</details>

## Where it runs

`processCompletedStorageRequestsAndSpillOldItems` (`:1055-1061`) is invoked from `beforeSleep`
(`server.c:1911`, again `server.c:2036`) and a 1 ms timer (`:311`); it calls
`processCompletedStorageRequests` first (`:1059`), then the cap-less spill controller
`spillFillToProjected`. See [engine-integration](../components/engine-integration.md).

## The drain loop (`processCompletedStorageRequests`, `:602-794`)

`while ((next_batch_size = extStorageBridge_pollCompletions(...)) > 0)` (`:606`) — **uncapped**,
loops until the [bridge](../components/bridge-layer.md) ring is empty. For each completion it
switches on `msg_type`:

- **WRITE** (`:622-686`): first debits the in-flight RAM credit
  (`inflight_spill_ram_bytes -= msg->ram_bytes`, `:625`), then spill result → `ONLY_FLASH` (free
  RAM, tombstone) / `ONLY_MEMORY` (fail, keep value) / delete (`PENDING_EVICT`). See [spill](spill.md).
- **READ** (`:689-751`): restore the fetched robj → `ONLY_MEMORY`, or miss, or discard
  (`PENDING_EVICT`). See [fetch](fetch.md).
- **DELETE** (`:754-785`): `dbDelete` + remove state (or defer/ignore per state). See
  [delete](delete.md).

After the switch, **every** completion ends with `unblockClientsInUseOnKey(key)` (`:792`),
`decrRefCount(key)`, and `zfree(msg)` (`:793-794`).

> **Never cap the drain.** If completions back up, values never get nullified, memory is never
> freed, and the eviction loop blocks every command — so the loop processes the entire ring each
> pass ([state-machine](../components/state-machine.md) transitions depend on this).

Unblocked clients re-execute on the next iteration (`blockedBeforeSleep`), now as RAM hits.

See also: [engine-integration](../components/engine-integration.md), [spill](spill.md), [fetch](fetch.md), [delete](delete.md).
