//! Key Spilling Storage Module
//!
//! Bridges Valkey's external storage module API with pluggable storage
//! backends (RocksDB, FlashCache) via self-contained ASIO layers.
//!
//! # Backend Selection
//!
//! - `backend-rocksdb` (default): Multi-threaded IO pool, synchronous ops.
//! - `backend-flashcache` (optional): 1 IO thread, async reads via callbacks.
//!
//! Backends are selected at compile time via Cargo features and at runtime
//! via the `backend=rocksdb|flashcache` module load argument.
#![warn(rust_2018_idioms)]
#![allow(unused)]

pub mod common;
pub mod backends;
pub mod dispatcher;
pub mod callbacks;
pub mod storage_type_bridge;

use std::collections::HashMap;
use std::ffi::c_void;
use std::os::raw::{c_char, c_int, c_longlong};
use std::sync::Mutex;

use valkey_module::{valkey_module, Context, Status, ValkeyString};
use valkey_module::alloc::ValkeyAlloc;

use crate::common::pending::{PendingReadsMap, PendingWritesMap};
use crate::common::serialization::SerializationCallbacks;
use crate::common::types::BackendConfig;
use crate::dispatcher::BackendDispatcher;

// ---------------------------------------------------------------------------
// Module constants
// ---------------------------------------------------------------------------

pub const MODULE_NAME: &str = "flash-tiering";
pub const MODULE_VERSION: u32 = 1;

const DEFAULT_DB_SIZE_BYTES: u64 = 1_073_741_824;
const DEFAULT_NUM_DATABASES: u32 = 16;
const DEFAULT_MAX_IN_FLIGHT_READS: u32 = 128;
const DEFAULT_NUM_IO_THREADS: usize = 4;

// ---------------------------------------------------------------------------
// Global module state
// ---------------------------------------------------------------------------

pub static MODULE_STATE: once_cell::sync::Lazy<Mutex<Option<ModuleState>>> =
    once_cell::sync::Lazy::new(|| Mutex::new(None));

/// Lock-free pointer to ModuleState for the hot path (put/get/del/poll).
/// Set once at init, cleared at shutdown. The Mutex above is only for init/deinit.
static MODULE_STATE_PTR: std::sync::atomic::AtomicPtr<ModuleState> =
    std::sync::atomic::AtomicPtr::new(std::ptr::null_mut());

/// Get a reference to ModuleState without locking. Returns None only before init or after shutdown.
#[inline(always)]
pub fn get_module_state() -> Option<&'static ModuleState> {
    let ptr = MODULE_STATE_PTR.load(std::sync::atomic::Ordering::Acquire);
    if ptr.is_null() { None } else { Some(unsafe { &*ptr }) }
}

pub struct ModuleState {
    pub dispatcher: BackendDispatcher,
    pub pending_reads: PendingReadsMap,
    pub pending_writes: PendingWritesMap,
    pub serialization_callbacks: SerializationCallbacks,
}

// ---------------------------------------------------------------------------
// Placeholder serialization callbacks
// ---------------------------------------------------------------------------

// Serialization callbacks — call the engine's extStorageSerializeKey/Value directly.
// These are the same functions the native path uses.

extern "C" {
    fn extStorageSerializeKey(key: *mut c_void, serialized_key: *mut *mut c_char) -> c_int;
    fn extStorageSerializeValue(value: *mut c_void, serialized_value: *mut *mut c_char) -> c_int;
    fn extStorageDeserializeValue(value: *mut c_char, length: c_int) -> *mut c_void;
    fn extStorageFreeSerializedValue(value: *mut c_void);
    // Smith predictor: call after serialization on IO thread
    fn extStorageOnSpillSerialize(exact_bytes: usize);
    fn extStorageInflightAddRam(bytes: usize);
    fn objectComputeSize(key: *const c_void, o: *mut c_void, sample_size: usize, dbid: c_int) -> usize;
}

unsafe extern "C" fn placeholder_serialize_key(
    key: *mut c_void, serialized_key: *mut *mut c_char,
) -> c_int {
    extStorageSerializeKey(key, serialized_key)
}

unsafe extern "C" fn placeholder_serialize_value(
    value: *mut c_void, serialized_value: *mut *mut c_char,
) -> c_int {
    extStorageSerializeValue(value, serialized_value)
}

unsafe extern "C" fn placeholder_deserialize_key(
    key: *mut c_char, _length: c_int,
) -> *mut c_void {
    key as *mut c_void
}

unsafe extern "C" fn placeholder_deserialize_value(
    value: *mut c_char, length: c_int,
) -> *mut c_void {
    extStorageDeserializeValue(value, length)
}

unsafe extern "C" fn placeholder_free_serialized_key(_key: *mut c_void) {}
unsafe extern "C" fn placeholder_free_serialized_value(value: *mut c_void) {
    extStorageFreeSerializedValue(value);
}

fn placeholder_serialization_callbacks() -> SerializationCallbacks {
    SerializationCallbacks {
        serialize_key: placeholder_serialize_key,
        serialize_value: placeholder_serialize_value,
        deserialize_key: placeholder_deserialize_key,
        deserialize_value: placeholder_deserialize_value,
        free_serialized_key: placeholder_free_serialized_key,
        free_serialized_value: placeholder_free_serialized_value,
    }
}

// ---------------------------------------------------------------------------
// RegisterStorageBackend FFI (new zero-overhead API)
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
            .ok_or_else(|| "RedisModule_GetApi not available".to_string())?
    );

    let mut register_ptr: *mut c_void = std::ptr::null_mut();
    let api_name = b"ValkeyModule_RegisterStorageBackend\0";
    let rc = get_api(api_name.as_ptr() as *const c_char, &mut register_ptr);

    if rc != 0 || register_ptr.is_null() {
        return Err("RegisterStorageBackend API not available".to_string());
    }

    let register: RegisterFn = std::mem::transmute(register_ptr);
    let storage_type_ptr = storage_type_bridge::get_storage_type_ptr();

    if register(ctx, storage_type_ptr) != 0 {
        return Err("RegisterStorageBackend returned error".to_string());
    }
    Ok(())
}

// ---------------------------------------------------------------------------
// SubscribeToExternalStorage FFI (legacy, kept for reference)
// ---------------------------------------------------------------------------

unsafe fn subscribe_to_external_storage(
    ctx: *mut valkey_module::raw::RedisModuleCtx,
    req_callback: callbacks::RequestCallbackFn,
    res_callback: callbacks::ResponseCallbackFn,
) -> Result<(), String> {
    type GetApiFn = unsafe extern "C" fn(
        name: *const c_char, func: *mut *mut c_void,
    ) -> c_int;

    type SubscribeFn = unsafe extern "C" fn(
        ctx: *mut valkey_module::raw::RedisModuleCtx,
        req_callback: callbacks::RequestCallbackFn,
        res_callback: callbacks::ResponseCallbackFn,
    ) -> c_int;

    let get_api: GetApiFn = std::mem::transmute(
        valkey_module::raw::RedisModule_GetApi
            .ok_or_else(|| "RedisModule_GetApi not available".to_string())?
    );

    let mut subscribe_ptr: *mut c_void = std::ptr::null_mut();
    let api_name = b"ValkeyModule_SubscribeToExternalStorage\0";
    let rc = get_api(api_name.as_ptr() as *const c_char, &mut subscribe_ptr);

    if rc != 0 || subscribe_ptr.is_null() {
        let api_name_redis = b"RedisModule_SubscribeToExternalStorage\0";
        let rc2 = get_api(api_name_redis.as_ptr() as *const c_char, &mut subscribe_ptr);
        if rc2 != 0 || subscribe_ptr.is_null() {
            return Err("SubscribeToExternalStorage API not available".to_string());
        }
    }

    let subscribe: SubscribeFn = std::mem::transmute(subscribe_ptr);
    if subscribe(ctx, req_callback, res_callback) != 0 {
        return Err("SubscribeToExternalStorage returned error".to_string());
    }
    Ok(())
}

// ---------------------------------------------------------------------------
// GetExternalStorageSerializationCallbacks FFI
// ---------------------------------------------------------------------------

unsafe fn get_serialization_callbacks(
    ctx: *mut valkey_module::raw::RedisModuleCtx,
) -> Option<SerializationCallbacks> {
    type GetApiFn = unsafe extern "C" fn(
        name: *const c_char, func: *mut *mut c_void,
    ) -> c_int;

    let get_api: GetApiFn = std::mem::transmute(valkey_module::raw::RedisModule_GetApi?);

    type GetCallbacksFn = unsafe extern "C" fn(
        ctx: *mut valkey_module::raw::RedisModuleCtx,
        sk: *mut Option<unsafe extern "C" fn(*mut c_void, *mut *mut c_char) -> c_int>,
        sv: *mut Option<unsafe extern "C" fn(*mut c_void, *mut *mut c_char) -> c_int>,
        dk: *mut Option<unsafe extern "C" fn(*mut c_char, c_int) -> *mut c_void>,
        dv: *mut Option<unsafe extern "C" fn(*mut c_char, c_int) -> *mut c_void>,
        fk: *mut Option<unsafe extern "C" fn(*mut c_void)>,
        fv: *mut Option<unsafe extern "C" fn(*mut c_void)>,
    ) -> c_int;

    let mut func_ptr: *mut c_void = std::ptr::null_mut();
    let name = b"ValkeyModule_GetExternalStorageSerializationCallbacks\0";
    let rc = get_api(name.as_ptr() as *const c_char, &mut func_ptr);
    if rc != 0 || func_ptr.is_null() {
        let name_redis = b"RedisModule_GetExternalStorageSerializationCallbacks\0";
        let rc2 = get_api(name_redis.as_ptr() as *const c_char, &mut func_ptr);
        if rc2 != 0 || func_ptr.is_null() {
            return None;
        }
    }

    let get_callbacks: GetCallbacksFn = std::mem::transmute(func_ptr);

    let mut sk: Option<unsafe extern "C" fn(*mut c_void, *mut *mut c_char) -> c_int> = None;
    let mut sv: Option<unsafe extern "C" fn(*mut c_void, *mut *mut c_char) -> c_int> = None;
    let mut dk: Option<unsafe extern "C" fn(*mut c_char, c_int) -> *mut c_void> = None;
    let mut dv: Option<unsafe extern "C" fn(*mut c_char, c_int) -> *mut c_void> = None;
    let mut fk: Option<unsafe extern "C" fn(*mut c_void)> = None;
    let mut fv: Option<unsafe extern "C" fn(*mut c_void)> = None;

    if get_callbacks(ctx, &mut sk, &mut sv, &mut dk, &mut dv, &mut fk, &mut fv) != 0 {
        return None;
    }

    Some(SerializationCallbacks {
        serialize_key: sk?,
        serialize_value: sv?,
        deserialize_key: dk?,
        deserialize_value: dv?,
        free_serialized_key: fk?,
        free_serialized_value: fv?,
    })
}

// ---------------------------------------------------------------------------
// SetExternalStorageKeyMayExistCallback FFI
// ---------------------------------------------------------------------------

unsafe fn set_key_may_exist_callback(
    ctx: *mut valkey_module::raw::RedisModuleCtx,
    callback: callbacks::KeyMayExistCallbackFn,
) -> Result<(), String> {
    type GetApiFn = unsafe extern "C" fn(
        name: *const c_char, func: *mut *mut c_void,
    ) -> c_int;

    let get_api: GetApiFn = std::mem::transmute(
        valkey_module::raw::RedisModule_GetApi
            .ok_or_else(|| "RedisModule_GetApi not available".to_string())?
    );

    type SetCallbackFn = unsafe extern "C" fn(
        ctx: *mut valkey_module::raw::RedisModuleCtx,
        callback: callbacks::KeyMayExistCallbackFn,
    ) -> c_int;

    let mut func_ptr: *mut c_void = std::ptr::null_mut();
    let name = b"ValkeyModule_SetExternalStorageKeyMayExistCallback\0";
    let rc = get_api(name.as_ptr() as *const c_char, &mut func_ptr);
    if rc != 0 || func_ptr.is_null() {
        let name_redis = b"RedisModule_SetExternalStorageKeyMayExistCallback\0";
        let rc2 = get_api(name_redis.as_ptr() as *const c_char, &mut func_ptr);
        if rc2 != 0 || func_ptr.is_null() {
            return Err("SetExternalStorageKeyMayExistCallback API not available".to_string());
        }
    }

    let set_callback: SetCallbackFn = std::mem::transmute(func_ptr);
    if set_callback(ctx, callback) != 0 {
        return Err("SetExternalStorageKeyMayExistCallback returned error".to_string());
    }
    Ok(())
}

unsafe fn set_metrics_callback(
    ctx: *mut valkey_module::raw::RedisModuleCtx,
    callback: callbacks::GetMetricsCallbackFn,
) -> Result<(), String> {
    type GetApiFn = unsafe extern "C" fn(
        name: *const c_char, func: *mut *mut c_void,
    ) -> c_int;

    let get_api: GetApiFn = std::mem::transmute(
        valkey_module::raw::RedisModule_GetApi
            .ok_or_else(|| "RedisModule_GetApi not available".to_string())?
    );

    type SetCallbackFn = unsafe extern "C" fn(
        ctx: *mut valkey_module::raw::RedisModuleCtx,
        callback: callbacks::GetMetricsCallbackFn,
    ) -> c_int;

    let mut func_ptr: *mut c_void = std::ptr::null_mut();
    let name = b"ValkeyModule_SetExternalStorageMetricsCallback\0";
    let rc = get_api(name.as_ptr() as *const c_char, &mut func_ptr);
    if rc != 0 || func_ptr.is_null() {
        let name_redis = b"RedisModule_SetExternalStorageMetricsCallback\0";
        let rc2 = get_api(name_redis.as_ptr() as *const c_char, &mut func_ptr);
        if rc2 != 0 || func_ptr.is_null() {
            return Err("SetExternalStorageMetricsCallback API not available".to_string());
        }
    }

    let set_callback: SetCallbackFn = std::mem::transmute(func_ptr);
    if set_callback(ctx, callback) != 0 {
        return Err("SetExternalStorageMetricsCallback returned error".to_string());
    }
    Ok(())
}

// ---------------------------------------------------------------------------
// Config parsing
// ---------------------------------------------------------------------------

pub fn parse_config_from_args(args: &[String]) -> Result<(String, BackendConfig), String> {
    let mut map = HashMap::new();
    for arg in args {
        if let Some((k, v)) = arg.split_once('=') {
            map.insert(k.to_string(), v.to_string());
        } else {
            return Err(format!("Invalid argument '{}': expected key=value", arg));
        }
    }

    let backend = map
        .remove("backend")
        .ok_or_else(|| "Missing required argument: backend=rocksdb|flashcache".to_string())?;

    let db_path = map
        .remove("db_path")
        .ok_or_else(|| "Missing required argument: db_path=/path/to/storage".to_string())?;

    let db_size_bytes: u64 = map.remove("db_size_bytes")
        .map(|v| v.parse().map_err(|_| format!("Invalid db_size_bytes: '{v}'")))
        .transpose()?
        .unwrap_or(DEFAULT_DB_SIZE_BYTES);

    let num_databases: u32 = map.remove("num_databases")
        .map(|v| v.parse().map_err(|_| format!("Invalid num_databases: '{v}'")))
        .transpose()?
        .unwrap_or(DEFAULT_NUM_DATABASES);

    let max_in_flight_reads: u32 = map.remove("max_in_flight_reads")
        .map(|v| v.parse().map_err(|_| format!("Invalid max_in_flight_reads: '{v}'")))
        .transpose()?
        .unwrap_or(DEFAULT_MAX_IN_FLIGHT_READS);

    let config = BackendConfig {
        db_path,
        db_size_bytes,
        num_databases,
        max_in_flight_reads,
        eviction_enabled: true, // TODO: read from engine's maxmemory-policy via module API
        options: map,
    };

    Ok((backend, config))
}

// ---------------------------------------------------------------------------
// Backend initialization helpers
// ---------------------------------------------------------------------------

fn create_dispatcher(
    backend_name: &str,
    config: BackendConfig,
    callbacks: SerializationCallbacks,
) -> Result<BackendDispatcher, String> {
    match backend_name {
        #[cfg(feature = "backend-rocksdb")]
        "rocksdb" => {
            use crate::backends::rocksdb::{RocksDBAsio, RocksDBBackend};
            use crate::common::backend_trait::DataTieringBackend;

            let num_io_threads: usize = config.options
                .get("num_io_threads")
                .and_then(|v| v.parse().ok())
                .unwrap_or(DEFAULT_NUM_IO_THREADS);
            let max_in_flight = config.max_in_flight_reads;

            let backend = RocksDBBackend::init(config)
                .map_err(|e| format!("RocksDB init failed: {e}"))?;
            let asio = RocksDBAsio::new(backend, max_in_flight, callbacks, num_io_threads)
                .map_err(|e| format!("RocksDB ASIO init failed: {e}"))?;
            Ok(BackendDispatcher::RocksDB(asio))
        }

        #[cfg(feature = "backend-flashcache")]
        "flashcache" => {
            use crate::backends::flashcache::{FlashCacheAsio, FlashCacheBackend};
            use crate::common::backend_trait::DataTieringBackend;

            let max_in_flight = config.max_in_flight_reads;
            let backend = FlashCacheBackend::init(config)
                .map_err(|e| format!("FlashCache init failed: {e}"))?;
            let asio = FlashCacheAsio::new(backend, max_in_flight, callbacks)
                .map_err(|e| format!("FlashCache ASIO init failed: {e}"))?;
            Ok(BackendDispatcher::FlashCache(asio))
        }

        other => Err(format!(
            "Unknown or disabled backend '{}'. Available: {}",
            other,
            available_backends(),
        )),
    }
}

fn available_backends() -> &'static str {
    match (
        cfg!(feature = "backend-rocksdb"),
        cfg!(feature = "backend-flashcache"),
    ) {
        (true, true) => "rocksdb, flashcache",
        (true, false) => "rocksdb",
        (false, true) => "flashcache",
        (false, false) => "(none)",
    }
}

// ---------------------------------------------------------------------------
// Module init / deinit
// ---------------------------------------------------------------------------

pub fn module_init(args: &[String]) -> Result<(), String> {
    {
        let guard = MODULE_STATE.lock()
            .map_err(|e| format!("Failed to lock MODULE_STATE: {e}"))?;
        if guard.is_some() {
            return Err("Module already initialized".to_string());
        }
    }

    let (backend_name, config) = parse_config_from_args(args)?;
    let callbacks = placeholder_serialization_callbacks();
    let dispatcher = create_dispatcher(&backend_name, config, callbacks)?;

    let state = ModuleState {
        dispatcher,
        pending_reads: PendingReadsMap::new(),
        pending_writes: PendingWritesMap::new(),
        serialization_callbacks: callbacks,
    };

    // Leak the state into a 'static reference for lock-free hot path access.
    // The Mutex is only used for the init check; the leaked Box lives for the process lifetime.
    let state_ptr = Box::into_raw(Box::new(state));
    MODULE_STATE_PTR.store(state_ptr, std::sync::atomic::Ordering::Release);

    let mut guard = MODULE_STATE.lock()
        .map_err(|e| format!("Failed to lock MODULE_STATE: {e}"))?;
    *guard = None; // Mutex no longer owns the state — just used as init sentinel
    Ok(())
}

pub fn module_deinit() -> Result<(), String> {
    // Clear hot-path pointer first (prevents new submits during shutdown)
    let ptr = MODULE_STATE_PTR.swap(std::ptr::null_mut(), std::sync::atomic::Ordering::AcqRel);
    if !ptr.is_null() {
        let mut state = unsafe { Box::from_raw(ptr) };
        state.dispatcher.shutdown()
            .map_err(|e| format!("Dispatcher shutdown failed: {e}"))?;
    }
    Ok(())
}

// ---------------------------------------------------------------------------
// valkey_module! init / deinit entry points
// ---------------------------------------------------------------------------

fn init(ctx: &Context, args: &[ValkeyString]) -> Status {
    let string_args: Vec<String> = args.iter().map(|a| a.to_string_lossy()).collect();

    let (backend_name, config) = match parse_config_from_args(&string_args) {
        Ok(v) => v,
        Err(e) => {
            eprintln!("flash-tiering: config error: {e}");
            return Status::Err;
        }
    };

    // Register storage backend via new zero-overhead API
    unsafe {
        if let Err(e) = register_storage_backend(ctx.ctx) {
            eprintln!("flash-tiering: RegisterStorageBackend failed: {e}");
            return Status::Err;
        }
    }

    // Fetch real serialization callbacks
    let ser_callbacks = unsafe {
        get_serialization_callbacks(ctx.ctx).unwrap_or_else(|| {
            eprintln!("flash-tiering: WARNING: using placeholder serialization callbacks");
            placeholder_serialization_callbacks()
        })
    };

    // Initialize backend
    let dispatcher = match create_dispatcher(&backend_name, config, ser_callbacks) {
        Ok(d) => d,
        Err(e) => {
            eprintln!("flash-tiering: backend init failed: {e}");
            return Status::Err;
        }
    };

    let state = ModuleState {
        dispatcher,
        pending_reads: PendingReadsMap::new(),
        pending_writes: PendingWritesMap::new(),
        serialization_callbacks: ser_callbacks,
    };

    // Leak the state for lock-free hot path access
    let state_ptr = Box::into_raw(Box::new(state));
    MODULE_STATE_PTR.store(state_ptr, std::sync::atomic::Ordering::Release);

    // key_may_exist callback is NOT needed with the new RegisterStorageBackend API.
    // The engine's state machine handles key existence tracking.
    // Registering it causes false positives that block clients forever.
    // unsafe {
    //     if let Err(e) = set_key_may_exist_callback(ctx.ctx, Some(callbacks::key_may_exist_callback)) {
    //         eprintln!("flash-tiering: WARNING: set_key_may_exist_callback failed: {e}");
    //     }
    // }

    // Register metrics callback
    unsafe {
        if let Err(e) = set_metrics_callback(ctx.ctx, Some(callbacks::get_metrics_callback)) {
            eprintln!("flash-tiering: WARNING: set_metrics_callback failed: {e}");
        }
    }

    Status::Ok
}

fn deinit(_ctx: &Context) -> Status {
    match module_deinit() {
        Ok(()) => Status::Ok,
        Err(e) => {
            eprintln!("flash-tiering: deinit failed: {e}");
            Status::Err
        }
    }
}

// ---------------------------------------------------------------------------
// valkey_module! macro
// ---------------------------------------------------------------------------

valkey_module! {
    name: "flash-tiering",
    version: 1,
    allocator: (ValkeyAlloc, ValkeyAlloc),
    data_types: [],
    init: init,
    deinit: deinit,
    commands: [],
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parse_config_valid_rocksdb() {
        let args = vec![
            "backend=rocksdb".to_string(),
            "db_path=/tmp/test-db".to_string(),
            "db_size_bytes=2147483648".to_string(),
        ];
        let (backend, config) = parse_config_from_args(&args).unwrap();
        assert_eq!(backend, "rocksdb");
        assert_eq!(config.db_path, "/tmp/test-db");
        assert_eq!(config.db_size_bytes, 2_147_483_648);
    }

    #[test]
    fn parse_config_defaults() {
        let args = vec![
            "backend=flashcache".to_string(),
            "db_path=/tmp/fc".to_string(),
        ];
        let (backend, config) = parse_config_from_args(&args).unwrap();
        assert_eq!(backend, "flashcache");
        assert_eq!(config.db_size_bytes, DEFAULT_DB_SIZE_BYTES);
        assert_eq!(config.num_databases, DEFAULT_NUM_DATABASES);
    }

    #[test]
    fn parse_config_missing_backend() {
        let args = vec!["db_path=/tmp/test".to_string()];
        assert!(parse_config_from_args(&args).unwrap_err().contains("backend"));
    }

    #[test]
    fn parse_config_missing_db_path() {
        let args = vec!["backend=rocksdb".to_string()];
        assert!(parse_config_from_args(&args).unwrap_err().contains("db_path"));
    }

    #[test]
    fn parse_config_invalid_format() {
        let args = vec![
            "backend=rocksdb".to_string(),
            "db_path=/tmp/test".to_string(),
            "not_key_value".to_string(),
        ];
        assert!(parse_config_from_args(&args).unwrap_err().contains("key=value"));
    }
}
