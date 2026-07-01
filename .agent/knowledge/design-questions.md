# Design Questions & Answers (Resolved)

All questions below are RESOLVED and implemented. Kept as historical record.

## Q1: Module vs Native calls? ✅ RESOLVED
**Answer**: Direct function pointer call via storageType struct. Module API overhead eliminated.
Implemented in `src/storage.h` (storageType vtable) and `src/storage/storage_dispatch.c`.

## Q2: Can we avoid the value copy during spill? ✅ RESOLVED
**Answer**: YES for RAW-encoded sds values. Implemented in storage_flashcache_real.c — IO thread
receives robj* reference, serializes on IO thread (not main thread). Embedded values still
require copy (sds lives inside dbEntry struct, rehash would invalidate pointer).

## Q3: Background thread access to tiered values? ✅ RESOLVED
**Answer**: Guards implemented in all paths:
- rdb.c: skips tiered keys in RDB save
- defrag.c: skips in-flight keys
- networking.c: error if tiered value reaches addReply
- expire.c: defers deletion of in-flight keys (returns KEY_EXPIRED but doesn't delete)

## Q4: How does eviction interact with tiering? ✅ RESOLVED
**Answer**: Engine NEVER evicts keys. `performEvictions()` delegates to `extStoragePerformEvictions()`
which spills cold values to flash instead of deleting. If flash is full, OOM rejects writes at 1.2x.
findBestEvictionCandidate() extracted and shared between eviction and spill candidate selection.

## Q5: How to handle FLUSHDB/FLUSHALL with in-flight IO? ✅ RESOLVED
**Answer**: Call extStorageBridge_flushDB() BEFORE deleting in-memory keys. This drains in-flight
IO and wipes the flash index, preventing crashes from deleting keys mid-spill.
