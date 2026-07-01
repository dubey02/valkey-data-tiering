# Data Flow: Non-Key-Spilling FlashCache Implementation

## Architecture Overview

```
┌─────────────────────────────────────────────────────────────────────┐
│ Event Loop (ae.c)                                                   │
│                                                                     │
│  ┌─────────────┐    ┌──────────────┐    ┌───────────────────────┐  │
│  │ Read from   │───>│ processCmd() │───>│ preCommandExec()      │  │
│  │ client sock │    │              │    │ (key on flash? block) │  │
│  └─────────────┘    └──────────────┘    └───────────────────────┘  │
│        │                                          │                 │
│        │ throttle gate                            │ async read req  │
│        ▼                                          ▼                 │
│  ┌─────────────┐                        ┌──────────────────────┐   │
│  │ Token Bucket│                        │ FlashCache ASIO      │   │
│  │ (1ms timer) │                        │ (background thread)  │   │
│  └─────────────┘                        └──────────────────────┘   │
│                                                   │                 │
│  ┌─────────────────────────────────────┐          │ completion     │
│  │ beforeSleep()                       │<─────────┘ callback       │
│  │  1. processCompletedStorageRequests │                            │
│  │  2. spillOldItems (if over maxmem)  │                            │
│  │  3. unblock clients                 │                            │
│  └─────────────────────────────────────┘                            │
└─────────────────────────────────────────────────────────────────────┘
```

---

## WRITE/SPILL FLOW (Memory → Flash)

### Trigger
- `beforeSleep()` calls `processCompletedStorageRequestsAndSpillOldItems()`
- Also called from 1ms timer (`extStorageTimerCallback`)
- Condition: `zmalloc_used_memory() > server.maxmemory`

### Step-by-Step

```
1. processCompletedStorageRequestsAndSpillOldItems()
   │
   ├── processCompletedStorageRequests()  ← handle any completed I/O first
   │
   └── if (used_memory > maxmemory):
       │
       ├── for batch_size iterations (default 10):
       │   │
       │   ├── findBestEvictionCandidate(spillPoolLRU)
       │   │   └── picks LRU victim from eviction sample pool
       │   │
       │   └── spillItemAsync(key, db_id)
       │       │
       │       ├── GUARDS:
       │       │   - key exists in DB (dbFind)
       │       │   - not embedded encoding (listpack, intset, ziplist)
       │       │   - not already tiered (OBJ_ENCODING_TIERED)
       │       │   - refcount == 1 (no shared references)
       │       │   - state == ONLY_MEMORY
       │       │   - concurrent spills < max_num_concurrent_items_spilled
       │       │
       │       ├── SERIALIZE: createStringObject(raw_value, raw_len)
       │       │   └── copies the value into a new robj for the ASIO thread
       │       │
       │       ├── CREATE MESSAGE: {type=WRITE, db_id, key, value, expireMs}
       │       │
       │       ├── moduleFireExternalStorageEvent(msg)
       │       │   └── submits to FlashCache ASIO thread
       │       │   └── internally calls flashcachePutItem(dbid, key, klen, val, vlen)
       │       │
       │       └── STATE TRANSITION: ONLY_MEMORY → COPYING_TO_FLASH
       │
       └── extStorageUpdateSpillConcurrency(throttle_rate)
```

### Write Completion (ASIO callback → main thread)

```
FlashCache ASIO thread completes write
   │
   └── queues completion message (thread-safe queue)
       │
       └── processCompletedStorageRequests() [in beforeSleep]
           │
           ├── msg->type == WRITE && msg->status == OK:
           │   ├── dbFind(db, key) — locate entry
           │   ├── Replace value with empty sds placeholder
           │   ├── Set encoding = OBJ_ENCODING_TIERED
           │   ├── Free the original in-memory value
           │   ├── STATE: COPYING_TO_FLASH → ONLY_FLASH
           │   └── Decrement total_items_spilling counter
           │
           └── msg->type == WRITE && msg->status == FAIL:
               ├── STATE: COPYING_TO_FLASH → ONLY_MEMORY
               └── Value stays in memory (no data loss)
```

### Key Insight: Value Lifecycle During Spill
- Value is **copied** before submission (not moved)
- Original stays in memory during COPYING_TO_FLASH (still serveable)
- Only after completion does the original get freed and replaced with placeholder
- If spill fails, nothing changes — value stays in RAM

---

## READ/FETCH FLOW (Flash → Memory)

### Trigger
- Client issues any command (GET, SET, MGET, etc.) on a key with `OBJ_ENCODING_TIERED`

### Step-by-Step

```
1. Client sends command (e.g., GET mykey)
   │
   ├── readQueryFromClient()
   │   └── extStorageThrottle_shouldThrottle(c)
   │       └── if throttled: stop reading, queue client, return
   │
   ├── processCommand()
   │   │
   │   └── preCommandExec(c)
   │       │
   │       ├── getKeysFromCommand(c->cmd, c->argv, c->argc)
   │       │
   │       ├── for each key:
   │       │   └── keyBlocksClient(db, key, is_write_cmd)
   │       │       │
   │       │       ├── Check state via extStorageGetState(db, key)
   │       │       │
   │       │       ├── ONLY_FLASH:
   │       │       │   ├── Determine msg_type (READ or DELETE for DEL/UNLINK)
   │       │       │   ├── createStorageMessage(READ, db_id, key, NULL, 0)
   │       │       │   ├── moduleFireExternalStorageEvent(msg)
   │       │       │   │   └── calls flashcacheGetItem(dbid, key, klen, READ_TYPE,
   │       │       │   │         request_ctx, completion_callback)
   │       │       │   ├── STATE: ONLY_FLASH → COPYING_TO_MEMORY
   │       │       │   └── return 1 (blocks client)
   │       │       │
   │       │       ├── COPYING_TO_FLASH:
   │       │       │   └── return 1 (blocks client, wait for spill to finish)
   │       │       │
   │       │       ├── COPYING_TO_MEMORY:
   │       │       │   └── return 1 (blocks client, already being fetched)
   │       │       │
   │       │       └── ONLY_MEMORY:
   │       │           └── return 0 (proceed normally)
   │       │
   │       ├── if any key blocks:
   │       │   ├── blockClientInUseOnKeys(c, num_keys, blocking_keys)
   │       │   ├── c->flag.pending_command = 1
   │       │   └── return CMD_FILTER_REJECT
   │       │
   │       └── return CMD_FILTER_ACCEPT
   │
   └── if ACCEPT: call() executes the command normally

2. FlashCache ASIO thread reads from NVMe
   │
   └── completion_callback fires (queues result)

3. processCompletedStorageRequests() [in beforeSleep]
   │
   ├── msg->type == READ && msg->status == OK:
   │   ├── new_value = msg->value (deserialized robj)
   │   ├── dbFind(db, key) — locate the placeholder entry
   │   ├── objectSetVal(entry, objectGetVal(new_value))
   │   ├── Set encoding back from OBJ_ENCODING_TIERED to original
   │   ├── STATE: COPYING_TO_MEMORY → ONLY_MEMORY
   │   ├── signalKeyAsReady(db, key) — unblocks waiting clients
   │   └── Clients re-execute their commands (pending_command flag)
   │
   └── msg->type == DELETE:
       ├── dbDelete(db, key) — remove from DB entirely
       ├── STATE: remove from state hashtable
       └── signalKeyAsReady → unblock clients
```

### Key Insight: Client Blocking Mechanism
- Client is blocked using Valkey's native `blockClient` infrastructure
- `pending_command = 1` means: when unblocked, re-run the command from scratch
- Multiple clients can block on the same key — only one fetch is issued
- The second client sees state=COPYING_TO_MEMORY and just blocks (no duplicate fetch)

---

## CONCURRENCY PROTECTION (Key Blocking)

### Problem
While a value is in-flight (being spilled or fetched), the engine must NOT modify it.

### Solution: State Machine + preCommandExec Gate

```
State Machine:
  ONLY_MEMORY ──spill──> COPYING_TO_FLASH ──done──> ONLY_FLASH
       ▲                                                │
       │                                                │ client access
       │                                                ▼
  ONLY_MEMORY <──done── COPYING_TO_MEMORY <──fetch── ONLY_FLASH

Any state except ONLY_MEMORY → block the client
```

**preCommandExec** runs BEFORE every command:
- If key is in ANY non-ONLY_MEMORY state → block client, reject command
- Command will re-execute after state returns to ONLY_MEMORY

**Race condition safety** (server.c line 4645-4662):
- After preCommandExec returns ACCEPT, server re-checks all keys for OBJ_ENCODING_TIERED
- If a key was spilled between preCommandExec and call(), re-runs preCommandExec
- This handles the window where spill completes during command dispatch

**PENDING_EVICT state:**
- If a key is being fetched (COPYING_TO_MEMORY) and memory pressure triggers eviction of that key
- State transitions to PENDING_EVICT
- When fetch completes: instead of restoring to memory, immediately re-spills it
- Prevents thrashing: don't fetch just to immediately evict

---

## SERIALIZATION / DESERIALIZATION

### Spill (serialize for flash):
```c
// In spillItemAsync():
raw_value = objectGetVal(item);           // get the raw sds/data
value_copy = createStringObject(raw_value, raw_len);  // copy into robj

// The module layer then serializes:
extStorageSerializeKey(key, &serialized_key);     // sds → char*
extStorageSerializeValue(value, &serialized_value); // robj → char*
```

### Fetch (deserialize from flash):
```c
// FlashCache returns raw bytes via callback
// Module layer deserializes:
key_obj = extStorageDeserializeKey(raw_key, key_len);     // char* → sds
val_obj = extStorageDeserializeValue(raw_val, val_len);   // char* → robj

// Then in processCompletedStorageRequests():
objectSetVal(entry, objectGetVal(val_obj));  // restore into DB entry
```

### What's serialized:
- Key: sds string (key name)
- Value: the raw string representation of the robj (already in sds form for string types)
- For complex types (hash, list, set, zset): they must be in a serializable encoding (not embedded/listpack)
- The `spillItemAsync` guard skips embedded encodings — only spills RAW/HT encoded objects

---

## ASYNC vs SYNC PATHS

### Async (ASIO thread):
| Operation | Function | Thread |
|-----------|----------|--------|
| Write to flash | `flashcachePutItem()` | ASIO background |
| Read from flash | `flashcacheGetItem()` | ASIO background |
| Delete from flash | `flashcacheGetItem(DELETE)` | ASIO background |
| GC / compaction | `flashcacheRunCronTasks()` | ASIO background |

### Sync (main thread):
| Operation | Function | Thread |
|-----------|----------|--------|
| preCommandExec (key check) | `preCommandExec()` | Main |
| State machine transitions | `extStorageSetState()` | Main |
| Process completions | `processCompletedStorageRequests()` | Main |
| Serialize/deserialize | `extStorageSerialize*()` | Main |
| Client block/unblock | `blockClientInUseOnKeys()` | Main |
| Eviction candidate selection | `findBestEvictionCandidate()` | Main |
| Throttle decisions | `extStorageThrottle_*()` | Main |

### Communication: Main ↔ ASIO
- Main → ASIO: `moduleFireExternalStorageEvent(msg)` (submits work)
- ASIO → Main: completion callback queues result in thread-safe queue
- Main polls queue in `processCompletedStorageRequests()` (called from beforeSleep)

---

## THROTTLE & EQUILIBRIUM

### Token Bucket (controls client ingestion rate):
```
measured_max_tps = 1,000,000 / min_cmd_latency_us
allowed_tps = measured_max_tps * (1 - throttle_rate)

throttle_rate:
  used_mem ≤ maxmemory        → 0.0 (no throttle)
  used_mem ≥ maxmemory * 1.1  → 1.0 (max throttle)
  between                     → linear interpolation
```

### Dynamic Spill Concurrency:
```
concurrent_spills = 50 + (200 - 50) * throttle_rate

Memory at 100%: 50 concurrent spills, no throttle
Memory at 105%: 125 concurrent spills, 50% throttle
Memory at 110%: 200 concurrent spills, full throttle
```

### Feedback Loop:
```
Memory rises → throttle_rate increases → clients slow down + spills speed up
Memory drops → throttle_rate decreases → clients speed up + spills slow down
System oscillates around maxmemory with 10% band as damping
```

---

## EVENT LOOP INTEGRATION SUMMARY

```
Each event loop iteration:
  1. ae polls for events (client reads, timer fires)
  2. readQueryFromClient() → throttle gate
  3. processCommand() → preCommandExec() → block or execute
  4. call() → records latency for throttler
  5. beforeSleep():
     a. processCompletedStorageRequests() ← unblock clients, restore values
     b. spillOldItems() ← if over maxmemory, spill LRU batch
     c. blockedBeforeSleep() ← re-execute pending commands
  6. 1ms timer: refill throttle tokens, release queued clients
```

---

## FLASHCACHE API CALLS (from ext_storage.c)

| Call Site | FlashCache Function | Purpose |
|-----------|-------------------|---------|
| spillItemAsync | flashcachePutItem(dbid, key, klen, val, vlen) | Write value to NVMe |
| preCommandExec (fetch) | flashcacheGetItem(dbid, key, klen, READ, ctx, cb) | Async read from NVMe |
| preCommandExec (delete) | flashcacheGetItem(dbid, key, klen, DELETE, ctx, cb) | Async delete from NVMe |
| beforeSleep/timer | flashcacheRunCronTasks() | GC, process completions |
| init | flashcacheInit(...) | Setup device, index, ASIO thread |
| shutdown | flashcacheTearDown() | Cleanup |
| snapshot | flashcacheStartFileBasedSave(...) | RDB-like persistence |
| replication | flashcacheStartStreamBasedSave(...) | Stream to replica |
| flushdb | flashcacheFlushDB(dbid) | Clear one DB |
| flushall | flashcacheFlushAllDBs() | Clear all DBs |
| config | flashcacheSetConfig/GetConfig | Runtime tuning |
