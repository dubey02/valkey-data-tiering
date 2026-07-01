//! RocksDB Tiering Module
//!
//! Self-contained Valkey module that provides data tiering to RocksDB.
//! Registers a `storageType` backend via `ValkeyModule_RegisterStorageBackend`.
//!
//! Usage:
//!   --loadmodule .../librocksdb_tiering_module.so db_path=/mnt/nvme/tiering
//!
//! Optional args: db_size_bytes, num_databases, max_in_flight_reads, num_io_threads,
//!   compression_type (none|snappy|lz4|zstd), block_cache_size, disable_wal (0|1),
//!   direct_io (0|1), bloom_filter_bits_per_key

mod asio;
mod bridge;

use std::collections::HashMap;
use std::ffi::c_void;
use std::os::raw::{c_char, c_int};
use std::sync::Mutex;

use valkey_module::{valkey_module, Context, Status, ValkeyString};
use valkey_module::alloc::ValkeyAlloc;

use crate::asio::RocksDBAsio;

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

const DEFAULT_DB_SIZE_BYTES: u64 = 1_073_741_824;
const DEFAULT_NUM_DATABASES: u32 = 16;
const DEFAULT_MAX_IN_FLIGHT_READS: u32 = 128;
const DEFAULT_NUM_IO_THREADS: usize = 4;

// ---------------------------------------------------------------------------
// Serialization callbacks (engine-provided, called on IO thread)
// ---------------------------------------------------------------------------

extern "C" {
    fn extStorageSerializeKey(key: *mut c_void, serialized_key: *mut *mut c_char) -> c_int;
    fn extStorageSerializeValue(value: *mut c_void, serialized_value: *mut *mut c_char) -> c_int;
    fn extStorageDeserializeValue(value: *mut c_char, length: c_int) -> *mut c_void;
}

/// Callbacks passed to IO threads for robj* <-> bytes conversion.
#[derive(Clone, Copy)]
pub struct SerializationCallbacks {
    pub serialize_key:
        unsafe extern "C" fn(key: *mut c_void, out: *mut *mut c_char) -> c_int,
    pub serialize_value:
        unsafe extern "C" fn(value: *mut c_void, out: *mut *mut c_char) -> c_int,
    pub deserialize_value:
        unsafe extern "C" fn(value: *mut c_char, length: c_int) -> *mut c_void,
    pub free_key: unsafe extern "C" fn(key: *mut c_void),
    pub free_value: unsafe extern "C" fn(value: *mut c_void),
}

unsafe impl Send for SerializationCallbacks {}
unsafe impl Sync for SerializationCallbacks {}

unsafe extern "C" fn noop_free(_: *mut c_void) {}

fn engine_callbacks() -> SerializationCallbacks {
    SerializationCallbacks {
        serialize_key: extStorageSerializeKey,
        serialize_value: extStorageSerializeValue,
        deserialize_value: extStorageDeserializeValue,
        free_key: noop_free,
        free_value: noop_free,
    }
}

// ---------------------------------------------------------------------------
// Module state
// ---------------------------------------------------------------------------

pub static STATE: once_cell::sync::Lazy<Mutex<Option<RocksDBAsio>>> =
    once_cell::sync::Lazy::new(|| Mutex::new(None));

// ---------------------------------------------------------------------------
// Config parsing
// ---------------------------------------------------------------------------

fn parse_args(args: &[String]) -> Result<(String, HashMap<String, String>), String> {
    let mut map = HashMap::new();
    for arg in args {
        if let Some((k, v)) = arg.split_once('=') {
            map.insert(k.to_string(), v.to_string());
        } else {
            return Err(format!("Invalid arg '{}': expected key=value", arg));
        }
    }
    let db_path = map.remove("db_path")
        .ok_or_else(|| "Missing required: db_path=/path/to/storage".to_string())?;
    Ok((db_path, map))
}

// ---------------------------------------------------------------------------
// RegisterStorageBackend FFI
// ---------------------------------------------------------------------------

unsafe fn register_storage_backend(
    ctx: *mut valkey_module::raw::RedisModuleCtx,
) -> Result<(), String> {
    type GetApiFn = unsafe extern "C" fn(
        name: *const c_char, func: *mut *mut c_void,
    ) -> c_int;
    type RegisterFn = unsafe extern "C" fn(
        ctx: *mut valkey_module::raw::RedisModuleCtx,
        storage_type: *mut c_void,
    ) -> c_int;

    let get_api: GetApiFn = std::mem::transmute(
        valkey_module::raw::RedisModule_GetApi
            .ok_or_else(|| "GetApi unavailable".to_string())?
    );

    let mut register_ptr: *mut c_void = std::ptr::null_mut();
    let name = b"ValkeyModule_RegisterStorageBackend\0";
    let rc = get_api(name.as_ptr() as *const c_char, &mut register_ptr);
    if rc != 0 || register_ptr.is_null() {
        return Err("RegisterStorageBackend API not available".to_string());
    }

    let register: RegisterFn = std::mem::transmute(register_ptr);
    let st_ptr = bridge::get_storage_type_ptr();
    if register(ctx, st_ptr) != 0 {
        return Err("RegisterStorageBackend returned error".to_string());
    }
    Ok(())
}

// ---------------------------------------------------------------------------
// Module init/deinit
// ---------------------------------------------------------------------------

fn init(ctx: &Context, args: &[ValkeyString]) -> Status {
    let string_args: Vec<String> = args.iter().map(|a| a.to_string_lossy()).collect();

    let (db_path, opts) = match parse_args(&string_args) {
        Ok(v) => v,
        Err(e) => { eprintln!("rocksdb-tiering: {e}"); return Status::Err; }
    };

    // Register storageType
    unsafe {
        if let Err(e) = register_storage_backend(ctx.ctx) {
            eprintln!("rocksdb-tiering: {e}");
            return Status::Err;
        }
    }

    let num_databases: u32 = opts.get("num_databases")
        .and_then(|v| v.parse().ok()).unwrap_or(DEFAULT_NUM_DATABASES);
    let max_in_flight: u32 = opts.get("max_in_flight_reads")
        .and_then(|v| v.parse().ok()).unwrap_or(DEFAULT_MAX_IN_FLIGHT_READS);
    let num_io_threads: usize = opts.get("num_io_threads")
        .and_then(|v| v.parse().ok()).unwrap_or(DEFAULT_NUM_IO_THREADS);

    let callbacks = engine_callbacks();

    let asio = match RocksDBAsio::new(&db_path, num_databases, max_in_flight, num_io_threads, &opts, callbacks) {
        Ok(a) => a,
        Err(e) => { eprintln!("rocksdb-tiering: init failed: {e}"); return Status::Err; }
    };

    let mut guard = match STATE.lock() {
        Ok(g) => g,
        Err(e) => { eprintln!("rocksdb-tiering: lock: {e}"); return Status::Err; }
    };
    *guard = Some(asio);
    Status::Ok
}

fn deinit(_ctx: &Context) -> Status {
    let mut guard = match STATE.lock() {
        Ok(g) => g,
        Err(_) => return Status::Err,
    };
    if let Some(mut asio) = guard.take() {
        let _ = asio.shutdown();
    }
    Status::Ok
}

valkey_module! {
    name: "rocksdb-tiering",
    version: 1,
    allocator: (ValkeyAlloc, ValkeyAlloc),
    data_types: [],
    init: init,
    deinit: deinit,
    commands: [],
}
