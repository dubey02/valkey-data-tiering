# Module Mode Design Decision

## Final Decision: Module registers a storageType struct

### Current Problems (moduleFireExternalStorageEvent)
1. **Wrong abstraction** — Module API designed for client→module, not engine→module
2. **Per-call overhead** — moduleCreateContext + temp client alloc/free per operation (~6.7μs)
3. **No standard** — custom one-off ValkeyModuleExternalStorageMsg, not reusable
4. **Serialization coupling** — module must understand robj internals
5. **Single subscriber** — only one storage module can register

### New Approach
```c
// Module registers at load time:
int ValkeyModule_OnLoad(ValkeyModuleCtx *ctx, ...) {
    storageType *t = zmalloc(sizeof(*t));
    t->name = "my_backend";
    t->put = my_sync_put;       // OR put_async for own IO
    t->get = my_sync_get;
    // ...
    ValkeyModule_RegisterStorageBackend(ctx, t);
}
```

### Benefits
- Zero per-call overhead after registration (direct fn ptr, same as native)
- Same interface as native backends (storageType struct)
- Module receives opaque bytes (serialized by middleware or by itself)
- Multiple backends can register (select via config)
- Standard API — any module author can write a storage backend

### How It Fits with Hybrid Middleware

| Module Type | Implements | IO Handling |
|-------------|-----------|-------------|
| Simple module | sync put/get/del only | Engine's shared middleware provides async |
| Advanced module | async put_async/get_async + poll_completions | Module owns its own IO threads |

### Precedents
- MySQL: INSTALL PLUGIN + handler class (register struct, direct calls after)
- PostgreSQL: CREATE ACCESS METHOD + TableAmRoutine struct
- Linux: module_init() registers file_operations struct
- Nginx: module registers handler struct at load
- SQLite: sqlite3_vfs_register() at init

All follow same pattern: register function pointer struct once, direct calls thereafter.
