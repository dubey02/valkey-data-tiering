# Design: Synchronous Fetch Primitive (Mid-Execution Key Access)

Status: DESIGN — not implemented. 2026-07-21, branch `json-module-test`.

## Problem

The KBC filter (`preCommandExec`) handles every key access that is (a) knowable
before execution and (b) recoverable by block-and-re-execute. Three access
classes fail those prerequisites and today crash or return wrong data:

1. **Lua undeclared keys** — `redis.call('get', name)` where `name` is
   computed inside the script. Name unknowable pre-execution; script may have
   already performed writes, so abort-and-retry is impossible (no rollback).
2. **SORT BY/GET patterns** — `weight_*` keys derive from the sorted
   collection's elements, resolved per-element mid-sort (`lookupKeyByPattern`,
   sort.c:69). Live-reproduced SIGSEGV.
3. **Module `ValkeyModule_OpenKey` on a tiered key** — modules access keys
   from callbacks at arbitrary points (known P0, e.g. valkey-search reindex).

All three need the same thing: *the value must become resident right now,
mid-command, without unwinding the command*. This is the primitive internal
TS provides via its Lua expedite path; the POC never built it.

## API

```c
/* Ensure `key`'s value is resident in memory, stalling the main thread on
 * the IO round-trip if needed. Returns the entry (value resident) or NULL
 * (key absent / evicted by FC GC / pending deletion). Never times out —
 * callers cannot roll back partial execution, so the read must complete. */
dbEntry *extStorageSyncFetch(serverDb *db, sds key);
```

Wiring (two mechanisms, no per-command changes):

- **New lookup flag** `LOOKUP_SYNCFETCH (1<<5)`: `lookupKeyReadWithFlags` /
  `lookupKeyWriteWithFlags` call `extStorageSyncFetch` when the entry is
  tiered/in-flight and the flag is set. Explicit opt-in callers:
  `lookupKeyByPattern` (SORT), module `OpenKey` (phase 2).
- **Automatic in nested execution**: same lookup functions apply the flag
  implicitly when `server.execution_nesting > 0` (Lua, functions, module
  calls). Rationale: any tiered entry reached at nesting depth escaped the
  filter by definition and has no blocking option. Top-level (nesting == 0)
  behavior unchanged — the filter and TIERED_SAFETY remain the mechanism.

## Core algorithm

```
extStorageSyncFetch(db, key):
  loop:
    state = extStorageGetState(db, key)
    switch state:
      ONLY_MEMORY:        entry = dbFind(key); return entry (or NULL — absent)
      PENDING_DELETION:   return NULL             /* logically deleted */
      ONLY_FLASH:         submitGet(key); state->COPYING_TO_MEMORY; wait
      COPYING_TO_MEMORY:  wait                    /* op already in flight */
      COPYING_TO_FLASH:   wait                    /* spill in flight; its
                            completion moves us to ONLY_FLASH, then fetch.
                            Required even for reads when the caller may
                            write: mutating a value the IO thread is
                            serializing is a data race. */
      PENDING_EVICT:      wait                    /* eviction completes ->
                                                     absent -> NULL */
  wait := drainCompletionsSelective(key)
```

### Selective completion draining — the safety core

While stalled we must consume the completion queue to receive our key's
completion, but the queue carries other keys' completions too, and processing
those mid-command mutates the keyspace under the running command (e.g. a
spill completion frees a value the SORT vector still references — UAF).

```
drainCompletionsSelective(key):
  poll completions from bridge (short batches)
  for each c: if c.key == key -> process via the NORMAL state-machine handler
              else            -> append to db-level deferred_completions list
  brief backoff between polls (start 0, escalate to usleep(50-200us))
  log a warning if stalled > 5s (observability; never abort)
```

`processCompletedStorageRequests` is changed to consume `deferred_completions`
FIRST, then poll the bridge. Deferred items thus run later in their normal
context (beforeSleep/timer) with unchanged semantics.

**Ordering correctness**: per-key ordering is what matters, and the state
machine guarantees at most ONE in-flight operation per key (any key in a
COPYING_*/PENDING_* state blocks new submissions). Deferring other keys'
completions therefore cannot reorder any key's operations. Cross-key order
carries no semantics.

**Isolation correctness**: during the stall, the event loop does not run —
no other clients, no timers, no eviction cycle. The only keyspace mutation is
installing OUR key's value (or its miss/delete resolution). Everything else
is queued, not executed.

### Processing our own completion mid-command

Reuses the existing READ-completion handler verbatim: install deserialized
value, state -> ONLY_MEMORY, LRU/LFU registration, counters,
`unblockClientsInUseOnKey`. The unblock only QUEUES blocked clients for
`processUnblockedClients` (runs later in beforeSleep) — it does not execute
them inline, so no reentrancy. Read-miss (FC GC took it) resolves to
confirmed-absent -> return NULL -> caller sees a normal miss (correct cache
semantics).

### Writes

Uniform rule: a mid-execution WRITE to a non-resident key also sync-fetches
first, then proceeds as a normal in-memory overwrite. The v1 destructive
fetch (FC_READ consumes the flash copy) makes this orphan-free: after fetch
there is no stale flash record to shadow. (When fc_get_keep()/non-destructive
mode lands for hot-cold, overwrite must also invalidate the kept flash copy —
noted as a dependency for that project.)

## What this deliberately does not do

- **No timeout / abort**: callers cannot roll back partial script execution.
  Matches internal TS semantics ("the synchronous read must complete
  regardless of duration"). A wedged backend stalls the server — same
  failure domain as internal; mitigated by the >5s warning log + existing
  backend health metrics.
- **No top-level use**: the async filter path stays the default; sync fetch
  is strictly the escape hatch for non-blockable contexts. This bounds the
  latency impact to scripts/SORT-patterns/module-callbacks that actually
  touch cold keys.
- **No promotion-policy consultation**: the value MUST be resident for the
  command, so sync fetch always installs (PROMOTE_ALWAYS semantics);
  ext-storage-mode policies continue to govern only the async path.

## Metrics (INFO ext_storage)

- `sync_fetch_count`, `sync_fetch_miss_count`
- `sync_fetch_wait_us_max`, `sync_fetch_wait_us_total` (avg derivable)
- `sync_fetch_deferred_completions` (queue-jump pressure indicator)

## Interactions reviewed

- **Blocked clients on the same key**: they blocked on COPYING_TO_MEMORY via
  the filter; our completion-processing unblocks (queues) them; they
  re-execute after our command finishes and find the value resident. Same as
  the normal flow, just with a different agent driving the completion.
- **PENDING_DELETION** (this branch's DEL fix): sync fetch returns NULL —
  consistent with the key being logically deleted; the draining DEL's
  re-execution is unaffected (it passes through KBC, not sync fetch).
- **Rehash pause machinery**: unchanged — the completion handler we reuse
  already manages it; no scan of the affected table is in progress because
  all other completions are deferred.
- **MULTI/EXEC**: already covered pre-execution for extractable keys; SORT-BY
  or EVAL inside MULTI now covered by this primitive at nesting depth.

## Open item folded in

The unexplained Lua probe result (strlen returning 15 ≈ serialized-payload
size on an undeclared flash key) must be re-investigated during
implementation: if some existing path installs raw serialized bytes, it must
be found and removed, not just shadowed by this primitive.

## Phasing

1. Primitive + deferred-completions list + `LOOKUP_SYNCFETCH` +
   `execution_nesting` auto-trigger + SORT `lookupKeyByPattern` opt-in.
   Tests: SORT BY spilled weights (crash regression), Lua undeclared
   read/write/read-your-write, EVAL inside MULTI, miss case, metrics.
2. Module `OpenKey` opt-in (fixes the remaining module P0), valkey-search
   compatibility test.
3. Revisit SORT: optional future upgrade from sync fetch to batched pre-fetch
   (enumerate pattern keys after vector build, submit all fetches in parallel,
   single wait) — IO parallelism win for large sorts; not needed for
   correctness.
