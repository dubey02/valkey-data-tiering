# Bugs Fixed — Reference

## Lock-Free GET "Crash" (2026-05-25)
- **Symptom**: Server crashed on startup or during GET after DEBUG SPILL
- **Root cause**: NOT a queue bug. Two config issues:
  1. Backend name `"flashcache-mock-mock"` was invalid (no matching backend)
  2. Flash file `/tmp/valkey-flash.db` didn't exist (FlashCache asserts in `getFileSize`)
- **Fix**: Changed default to `"flashcache-mock"`, added `--ext-storage-backend` config option, document that file must be pre-created
- **Lesson**: Always check init/config before assuming data structure bugs

## Module Completion Hang (2026-05-25)
- **Symptom**: Last 72 of 500K requests hang forever (200 - 128 = 72)
- **Root cause**: `max_in_flight_reads = 128` in asio.rs. FlashCache io_uring couldn't drain all 128 pending reads in one `tick()` cycle. Remaining callbacks never fired.
- **Fix**: Removed in-flight throttle entirely. FlashCache handles own backpressure.
- **Secondary fix**: Changed IO worker from `if let` to `while let` (batch processing)
- **Lesson**: Don't add application-level throttles on top of io_uring — it has its own queue depth management

## Module Load Failure (2026-05-25)
- **Symptom**: `Module initialization failed. Module not loaded.`
- **Root cause**: `extStorage_init()` ran inside `initServer()` (line 3164) BEFORE `moduleLoadFromQueue()` (line 7728). Bridge initialized mock backend, then module tried to init FlashCache singleton → conflict.
- **Fix**: Moved `extStorage_init()` to after `moduleLoadFromQueue()` in main(). Bridge checks `moduleHasRegisteredStorageBackend()` first.
- **Lesson**: Module init ordering matters — defer engine subsystem init if modules might override

## decrRefCount Crash on IO Thread robj (2026-05-25 earlier)
- **Symptom**: SIGSEGV in processCompletedStorageRequests at zfree(new_value)
- **Root cause**: robj created on IO thread via createStringObject. decrRefCount on main thread tried to free internal sds that was already transferred via objectSetVal.
- **Fix**: Use `zfree(new_value)` after transferring sds ownership, skip freeStringObject entirely
- **Lesson**: Cross-thread robj lifecycle needs explicit ownership transfer, not refcount

## FlashCache Thread Safety (2026-05-25 earlier)
- **Symptom**: Random crashes during concurrent spill+fetch
- **Root cause**: flashcacheRunCronTasks() called from main thread while IO thread called put/get
- **Fix**: ALL FlashCache calls on single IO thread. Cron runs in IO worker loop.
- **Rule**: FlashCache is NOT thread-safe. Single-threaded access only.
