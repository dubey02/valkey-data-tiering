# Native Lock-Free IO Thread Architecture

## Design (storage_flashcache_real.c)

### Queue Structure
- **Request ring**: SPSC (main thread → IO thread), 4096 slots, C11 atomics
  - `atomic_store_explicit(&head, next, memory_order_release)` on push
  - `atomic_load_explicit(&head, memory_order_acquire)` on pop
- **Completion ring**: SPSC (IO thread → main thread), 8192 slots
  - Same memory ordering pattern
- **Wakeup**: pipe fd, main thread writes 1 byte after push, IO thread reads non-blocking

### IO Worker Loop
```
while (!shutdown) {
    read(wake_pipe)           // drain pipe (non-blocking)
    while (fc_req_pop(&req))  // batch dequeue all requests
        process(req)
    flashcacheRunCronTasks()  // fires async GET callbacks
    if (!did_work) nanosleep(50μs)
}
```

### Key Design Decisions
- **Batch dequeue**: Process ALL pending requests before calling cron (maximizes throughput)
- **50μs sleep**: Only when idle, matches module's crossbeam behavior
- **No mutex on hot path**: Pure atomic operations for push/pop
- **Pipe wakeup**: Ensures IO thread wakes immediately on new request

### GET Async Flow
1. Main thread: push {op=GET, key_robj, request_ctx} to req_ring, write pipe
2. IO thread: pop, serialize key, call flashcacheGetItem(async), decrRefCount(key_robj)
3. IO thread (later): flashcacheRunCronTasks() fires fc_get_callback
4. fc_get_callback: deserialize value → createStringObject → push to comp_ring
5. Main thread: fc_real_poll_completions → pop from comp_ring → bridge_on_completion

### Performance
- SET: 47K/s (under memory pressure), 52K/s (pre-pressure)
- GET: 30K/s (under pressure)
- 5-min stress test: 10M ops, 0 crashes, 0 hangs

### Bugs Fixed
- Initial "crash" was config bug: backend name typo + missing flash file
- NOT a memory ordering issue — the SPSC ring with acquire/release is correct
