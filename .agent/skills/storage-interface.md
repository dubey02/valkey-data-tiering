# Storage Interface — Quick Reference

## Config Options
```
--ext-storage-enabled yes          # Enable tiering
--ext-storage-backend flashcache   # Native backend (default: flashcache-mock)
--maxmemory 50mb                   # Trigger spilling
--maxmemory-policy allkeys-lru     # Required for eviction-based spilling
```

## Native Path
```bash
truncate -s 1G /tmp/valkey-flash.db
src/valkey-server --ext-storage-enabled yes --ext-storage-backend flashcache \
  --maxmemory 50mb --maxmemory-policy allkeys-lru --enable-debug-command yes
```

## Module Path
```bash
truncate -s 1G /tmp/valkey-flash.db
cd modules/non-key-spilling && ~/.cargo/bin/cargo build --release --features backend-flashcache
src/valkey-server --ext-storage-enabled yes --maxmemory 50mb --maxmemory-policy allkeys-lru \
  --enable-debug-command yes \
  --loadmodule modules/non-key-spilling/target/release/libnon_key_spilling_module.so \
  backend=flashcache db_path=/tmp/valkey-flash.db
```

## Key Files
| File | Role |
|------|------|
| `src/storage/storage.h` | storageType interface definition |
| `src/storage/storage_dispatch.c` | Dispatch layer (storageSubmitPut/Get/Del/Poll) |
| `src/storage/storage_flashcache_real.c` | Native lock-free FlashCache backend |
| `src/ext_storage.c` | Engine integration (state machine, spill/fetch logic) |
| `src/ext_storage_bridge.c` | Bridge between storageType and ext_storage.c |
| `src/config.c` | ext-storage-backend config registration |
| `modules/non-key-spilling/src/storage_type_bridge.rs` | Module storageType impl |
| `modules/non-key-spilling/src/backends/flashcache/asio.rs` | Module IO worker |

## Debug Commands
```
DEBUG SPILL <key>   # Manually spill a key to flash
SET key value       # Then DEBUG SPILL key, then GET key to test round-trip
```

## storageType Interface (C struct fn ptrs)
```c
typedef struct storageType {
    const char *name;
    void *(*open)(storageConfig *cfg);
    void (*close)(void *opaque);
    storageStatus (*put_async)(void *opaque, uint32_t db_id, ...);
    storageStatus (*get_async)(void *opaque, uint32_t db_id, ...);
    storageStatus (*del_async)(void *opaque, uint32_t db_id, ...);
    int (*poll_completions)(void *opaque, int max);
    int (*cron)(void *opaque);
} storageType;
```
