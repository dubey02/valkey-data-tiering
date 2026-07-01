---
title: Tiering State Machine
status: active
sources:
  - src/ext_storage.h
  - context/state-machine-design.md
  - src/ext_storage.c
updated: 2026-06-03
type: component
tier: working
claim_count: 9
edges:
  - to: flows/evict-during-fetch.md
    kind: refers_to
    source: human
    created: 2026-06-03
  - to: flows/spill.md
    kind: refers_to
    source: human
    created: 2026-06-03
  - to: flows/fetch.md
    kind: refers_to
    source: human
    created: 2026-06-03
  - to: components/engine-integration.md
    kind: refers_to
    source: human
    created: 2026-06-03
  - to: components/eviction-integration.md
    kind: refers_to
    source: human
    created: 2026-06-03
---

# Tiering State Machine

> Each key is in one of 5 states, stored in `robj->tiering_state` (3 bits). `ONLY_MEMORY`
> is the implicit default and is **not** tracked in any side table.

![State machine](../diagrams/state-machine.png)

## States

Source: `TieringState` enum in `src/ext_storage.h`.

| State | Value | Meaning |
|-------|-------|---------|
| `ONLY_MEMORY` | 0 | Value in RAM, normal operation (default, untracked) |
| `COPYING_TO_FLASH` | 1 | Spill in-flight; value still in RAM (still serveable for reads) |
| `ONLY_FLASH` | 2 | Value on disk; `encoding == OBJ_ENCODING_TIERED` |
| `COPYING_TO_MEMORY` | 3 | Fetch or async-delete in-flight |
| `PENDING_EVICT` | 4 | Eviction requested while an op was in-flight |

## Transitions

| From | Event | To |
|------|-------|-----|
| ONLY_MEMORY | `spillItemAsync()` | COPYING_TO_FLASH |
| COPYING_TO_FLASH | spill OK (free RAM, mark tiered) | ONLY_FLASH |
| COPYING_TO_FLASH | spill FAIL (keep RAM value) | ONLY_MEMORY |
| COPYING_TO_FLASH | eviction arrives | PENDING_EVICT |
| ONLY_FLASH | GET/SET fetch · DEL/evict async delete | COPYING_TO_MEMORY |
| COPYING_TO_MEMORY | fetch OK (restore, unblock) | ONLY_MEMORY |
| COPYING_TO_MEMORY | delete OK | *key deleted* |
| COPYING_TO_MEMORY | eviction arrives | PENDING_EVICT |
| PENDING_EVICT | op completes (discard value) | *key deleted* |

## Blocking matrix

| Command | ONLY_MEMORY | COPYING_TO_FLASH | ONLY_FLASH | COPYING_TO_MEMORY | PENDING_EVICT |
|---------|-------------|------------------|------------|-------------------|---------------|
| GET | serve RAM | serve RAM (still there) | block + fetch | block | block |
| SET | normal | block (wait for spill) | block + fetch | block | block |
| DEL | normal | block (wait for spill) | block + async delete | block | block |
| Eviction | free | skip (in-flight) | async delete | → PENDING_EVICT | already pending |

## Design notes

- **GET during COPYING_TO_FLASH serves from RAM** — the value is still present until the
  spill completion frees it.
- **SET/DEL during COPYING_TO_FLASH block** — must not mutate an in-flight value.
- **`PENDING_EVICT`** avoids the fetch-just-to-evict thrash: when an in-flight fetch
  finishes for a key already chosen for eviction, the value is discarded and the key
  deleted rather than promoted. See [evict-during-fetch](../flows/evict-during-fetch.md).
- **Fetch failures are fatal** — if a value was written it must be readable.

Related: [spill](../flows/spill.md), [fetch](../flows/fetch.md), [engine-integration](engine-integration.md),
[eviction-integration](eviction-integration.md).
