# Module Integration (non-key-spilling)

## Status: BROKEN (spill regression)
The module path stopped spilling after commits ff980f826 / 5cf5a9e12 (FlashCache moved to deps/).
Native path is primary and working. Module investigation pending.

## How to Run (when working)
```bash
# Build (must enable flashcache feature)
cd modules/non-key-spilling && cargo build --release --features backend-flashcache

# Pre-create flash file (8GB for benchmarks)
fallocate -l 8G /mnt/nvme/flashcache.db

# Start server with module
src/valkey-server --ext-storage-enabled yes --maxmemory 1gb \
  --maxmemory-policy allkeys-lru --enable-debug-command yes \
  --loadmodule modules/non-key-spilling/target/release/libnon_key_spilling_module.so \
  backend=flashcache db_path=/mnt/nvme/flashcache.db
```

## Init Sequence
1. `initServer()` runs (ext_storage NOT initialized here)
2. `moduleLoadFromQueue()` → module's `OnLoad` → `RegisterStorageBackend` + `FlashCacheBackend::init`
3. `extStorage_init()` → `extStorageBridge_init()` detects registered module backend
4. Module IO thread begins accepting requests

## Native Path (primary, working)
```bash
src/valkey-server --ext-storage-enabled yes --ext-storage-backend flashcache \
  --ext-storage-path /mnt/nvme/flashcache.db --ext-storage-capacity-mb 8192 \
  --maxmemory 1gb --maxmemory-policy allkeys-lru
```
