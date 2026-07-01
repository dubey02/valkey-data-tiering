# Data Tiering Modes — Final Design

## Single Config: `ext-storage-mode`

Immutable at startup. Determines promotion, admission, and FC read behavior.

```
ext-storage-mode cache|hot-cold|cold-storage|large-values
ext-storage-lfu-threshold 3              # hot-cold only
ext-storage-admission-size 65536         # large-values only
```

## Modes

| Mode | Promotion (read) | Admission (write) | FC Read | Use Case |
|------|-----------------|-------------------|---------|----------|
| `cache` | Always | Memory first | Destructive | General (default) |
| `hot-cold` | LFU ≥ N | Memory first | **Non-destructive** | Scan-resistant |
| `cold-storage` | Never | Direct to flash | **Non-destructive** | Archive |
| `large-values` | Always | Big→flash, small→memory | Destructive | Mixed sizes |

**Rule**: Destructive FC for always-promote modes. Non-destructive FC for may-skip-promote modes.

## FlashCache API

```c
// Existing (destructive):
int fc_get(fc_handle *h, const void *key, size_t klen, void *buf, size_t *vlen);
// Removes key from index after read.

// New (non-destructive, for hot-cold + cold-storage):
int fc_get_keep(fc_handle *h, const void *key, size_t klen, void *buf, size_t *vlen);
// Returns value but KEEPS key in index. Flash still has the data.

// Selected at init based on ext-storage-mode:
//   cache / large-values → bridge calls fc_get (destructive)
//   hot-cold / cold-storage → bridge calls fc_get_keep (non-destructive)
```

## State Machine (5 states, unchanged for all modes)

```
ONLY_MEMORY ──spill──► COPYING_TO_FLASH ──complete──► ONLY_FLASH
     ▲                                                     │
     │                                                     │ fetch
     │                                                     ▼
     └──────promote────── COPYING_TO_MEMORY ◄──────────────┘
                               │
                               │ (if evict requested during fetch)
                               ▼
                          PENDING_EVICT ──complete──► key deleted
```

## State Transitions Per Mode

### `cache` (destructive FC, always promote)

| Event | From | To | Action |
|-------|------|----|--------|
| Memory pressure | ONLY_MEMORY | COPYING_TO_FLASH | spillItemAsync() |
| Spill completes | COPYING_TO_FLASH | ONLY_FLASH | placeholder in entry |
| Read (GET) | ONLY_FLASH | COPYING_TO_MEMORY | submitGet() |
| Fetch completes | COPYING_TO_MEMORY | ONLY_MEMORY | **Always promote** (store value in entry). FC already deleted from index. |
| Write (SET, no fetch) | ONLY_FLASH | ONLY_MEMORY | Overwrite entry. Stale flash copy already gone (destructive). |
| Write (APPEND, needs old) | ONLY_FLASH | COPYING_TO_MEMORY→ONLY_MEMORY | Fetch old, modify, stays in DRAM. |

### `hot-cold` (non-destructive FC, conditional promote)

| Event | From | To | Action |
|-------|------|----|--------|
| Memory pressure | ONLY_MEMORY | COPYING_TO_FLASH | spillItemAsync() |
| Spill completes | COPYING_TO_FLASH | ONLY_FLASH | placeholder in entry |
| Read (GET) | ONLY_FLASH | COPYING_TO_MEMORY | submitGet() |
| Fetch completes, **LFU ≥ N** | COPYING_TO_MEMORY | ONLY_MEMORY | **Promote**: store value, DELETE from flash (async). |
| Fetch completes, **LFU < N** | COPYING_TO_MEMORY | ONLY_FLASH | **No promote**: serve value to clients (temporary in entry), then restore placeholder. Flash still has copy. |
| Write (SET, no fetch) | ONLY_FLASH | ONLY_MEMORY | Overwrite entry. DELETE stale from flash (async). |
| Write (APPEND, needs old) | ONLY_FLASH | COPYING_TO_MEMORY→ONLY_MEMORY | Fetch (non-destructive), modify, keep in DRAM. DELETE stale from flash. |

**Key detail**: When LFU < N and we don't promote, the value is temporarily in the entry just long enough for blocked clients to re-execute their commands. Then we restore the TIERED placeholder. Flash still has the original copy (non-destructive read). No re-spill IO needed.

**On promote (LFU ≥ N)**: We must DELETE from flash explicitly because non-destructive read left it there. This avoids stale data accumulating on flash.

### `cold-storage` (non-destructive FC, never promote, direct-to-flash writes)

| Event | From | To | Action |
|-------|------|----|--------|
| Write (SET) | — | COPYING_TO_FLASH→ONLY_FLASH | PUT value directly to flash. Entry starts as TIERED. |
| Write (SET) on existing ONLY_FLASH | ONLY_FLASH | ONLY_FLASH | PUT new value to flash (FC index updated atomically, old data = garbage for GC). |
| Read (GET) | ONLY_FLASH | COPYING_TO_MEMORY | submitGet() |
| Fetch completes | COPYING_TO_MEMORY | ONLY_FLASH | **Never promote**: serve value (temporary in entry), restore placeholder. Flash keeps copy. |
| Write (APPEND, needs old) | ONLY_FLASH | ONLY_FLASH | Fetch old (non-destructive), compute new, PUT new to flash, entry stays TIERED. |
| Memory pressure | N/A | N/A | No spill needed — nothing in memory except metadata. |

**Key detail**: In cold-storage, values NEVER persist in DRAM. Entry is always OBJ_ENCODING_TIERED after the command completes. Even modify-writes result in the new value going to flash.

### `large-values` (destructive FC, always promote, size-gated admission)

| Event | From | To | Action |
|-------|------|----|--------|
| Write (small value) | — | ONLY_MEMORY | Normal: value in DRAM. |
| Write (large value > threshold) | — | COPYING_TO_FLASH→ONLY_FLASH | Direct-to-flash: PUT value, entry starts TIERED. |
| Memory pressure (small values) | ONLY_MEMORY | COPYING_TO_FLASH | spillItemAsync() |
| Spill completes | COPYING_TO_FLASH | ONLY_FLASH | placeholder in entry |
| Read (GET) any ONLY_FLASH key | ONLY_FLASH | COPYING_TO_MEMORY | submitGet() |
| Fetch completes | COPYING_TO_MEMORY | ONLY_MEMORY | **Always promote** (store value). FC already deleted (destructive). |
| Write (SET, no fetch) on TIERED | ONLY_FLASH | depends on size | If new val small→ONLY_MEMORY. If large→re-PUT to flash. |

## Write Path: How We Know If Fetch Is Needed

Commands that **never need old value** (skip fetch, safe for all modes):
- `SET`, `MSET`, `SETNX`, `SETEX`, `PSETEX`
- `DEL`, `UNLINK`
- `EXPIRE`, `PEXPIRE`, `EXPIREAT`, `PEXPIREAT`, `PERSIST`
- `RENAME` (destination key)
- `COPY` (destination key)

Everything else → fetch first. No command-by-command whitelist maintenance needed:
the above is a fixed set. Any new write command not in this list = needs fetch.

## GC Eviction Behavior

FlashCache GC may autonomously evict keys from disk when flash is full.

- Key in ONLY_FLASH: GC evicts → next GET returns NOT_FOUND → treat as key miss
- Key in COPYING_TO_MEMORY: GC won't evict (active IO reference)
- Key in COPYING_TO_FLASH: GC won't evict (being written)

**Data loss on GC eviction of ONLY_FLASH keys is acceptable and documented.**
This is analogous to maxmemory eviction — when resources are full, cold data is lost.

## Implementation Priority

1. `cache` mode — current behavior, zero changes needed
2. `large-values` mode — add size gate in write path (admission)
3. `hot-cold` mode — add LFU check + non-destructive FC GET + temporary-serve-then-demote
4. `cold-storage` mode — add direct-to-flash writes + never-promote
