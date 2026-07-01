//! Bridge between the new `storageType` C interface and the Rust BackendDispatcher.
//!
//! Exposes C-compatible function pointers for `put_async`, `get_async`, `del_async`,
//! and `poll_completions` that delegate to the Rust ASIO layer.
//!
//! This replaces the old `SubscribeToExternalStorage` request/response callback pattern
//! with the new zero-overhead `RegisterStorageBackend` API.

use std::ffi::c_void;
use std::os::raw::{c_char, c_int};
use std::ptr;

use crate::get_module_state;
use crate::common::types::{StorageRequest, StorageResponse, SubmitResult};

// ---------------------------------------------------------------------------
// storageType C struct layout (must match src/storage/storage.h)
// ---------------------------------------------------------------------------

pub const VALKEY_STORAGE_VERSION: c_int = 1;

pub const STORAGE_OK: c_int = 0;
pub const STORAGE_NOT_FOUND: c_int = 1;
pub const STORAGE_WOULDBLOCK: c_int = 2;
pub const STORAGE_ERR_REJECTED: c_int = -3;

pub const STORAGE_OP_PUT: c_int = 0;
pub const STORAGE_OP_GET: c_int = 1;
pub const STORAGE_OP_DEL: c_int = 2;

#[repr(C)]
pub struct StorageCompletion {
    pub request_ctx: *mut c_void,
    pub op_type: c_int,
    pub status: c_int,
    pub db_id: u32,
    pub key: *mut c_void,
    pub klen: usize,
    pub value: *mut c_void,
    pub vlen: usize,
    pub expire_ms: i64,
    pub ram_bytes: usize,
}

pub type StorageCompletionFn = unsafe extern "C" fn(*mut StorageCompletion, *mut c_void);

#[repr(C)]
pub struct StorageConfig {
    pub path: *const c_char,
    pub capacity_bytes: usize,
    pub num_databases: u32,
    pub io_threads: c_int,
    pub eviction_enabled: c_int,
    pub completion_fn: Option<StorageCompletionFn>,
    pub completion_privdata: *mut c_void,
}

#[repr(C)]
pub struct StorageType {
    pub name: *const c_char,
    pub version: c_int,
    pub open: Option<unsafe extern "C" fn(*mut StorageConfig) -> *mut c_void>,
    pub close: Option<unsafe extern "C" fn(*mut c_void)>,
    pub put: Option<unsafe extern "C" fn(*mut c_void, u32, *const c_void, usize, *const c_void, usize, i64) -> c_int>,
    pub get: Option<unsafe extern "C" fn(*mut c_void, u32, *const c_void, usize, *mut *mut c_void, *mut usize, *mut i64) -> c_int>,
    pub del: Option<unsafe extern "C" fn(*mut c_void, u32, *const c_void, usize) -> c_int>,
    pub put_async: Option<unsafe extern "C" fn(*mut c_void, u32, *const c_void, usize, *const c_void, usize, i64, *mut c_void) -> c_int>,
    pub get_async: Option<unsafe extern "C" fn(*mut c_void, u32, *const c_void, usize, *mut c_void) -> c_int>,
    pub del_async: Option<unsafe extern "C" fn(*mut c_void, u32, *const c_void, usize, *mut c_void) -> c_int>,
    pub poll_completions: Option<unsafe extern "C" fn(*mut c_void, c_int) -> c_int>,
    pub cron: Option<unsafe extern "C" fn(*mut c_void) -> c_int>,
    pub get_stats: Option<unsafe extern "C" fn(*mut c_void, *mut c_void)>,
}

// ---------------------------------------------------------------------------
// Static storage type name
// ---------------------------------------------------------------------------

static STORAGE_NAME: &[u8] = b"flash-tiering-rust\0";

// ---------------------------------------------------------------------------
// Completion delivery state
// ---------------------------------------------------------------------------

static mut COMPLETION_FN: Option<StorageCompletionFn> = None;
static mut COMPLETION_PRIVDATA: *mut c_void = ptr::null_mut();

// ---------------------------------------------------------------------------
// storageType function implementations
// ---------------------------------------------------------------------------

unsafe extern "C" fn nks_open(cfg: *mut StorageConfig) -> *mut c_void {
    if !cfg.is_null() {
        let c = &*cfg;
        COMPLETION_FN = c.completion_fn;
        COMPLETION_PRIVDATA = c.completion_privdata;
    }
    // Module state is already initialized in init() — return a non-null sentinel
    1usize as *mut c_void
}

unsafe extern "C" fn nks_close(_ctx: *mut c_void) {
    // Shutdown handled by module deinit
}

unsafe extern "C" fn nks_put_async(
    _ctx: *mut c_void, db_id: u32,
    key: *const c_void, klen: usize,
    value: *const c_void, vlen: usize,
    _expire_ms: i64, request_ctx: *mut c_void,
) -> c_int {
    let _ = (klen, vlen);

    let request = StorageRequest::WriteValue {
        db_id,
        key_robj: key as *mut c_void,
        value_robj: value as *mut c_void,
        request_context: request_ctx as u64,
    };

    let state = match get_module_state() {
        Some(s) => s,
        None => return STORAGE_ERR_REJECTED,
    };

    match state.dispatcher.submit_request(request) {
        SubmitResult::Ok => STORAGE_WOULDBLOCK,
        SubmitResult::Throttled => STORAGE_ERR_REJECTED,
    }
}

unsafe extern "C" fn nks_get_async(
    _ctx: *mut c_void, db_id: u32,
    key: *const c_void, klen: usize,
    request_ctx: *mut c_void,
) -> c_int {
    let _ = klen;

    let request = StorageRequest::ReadValue {
        db_id,
        key_robj: key as *mut c_void,
        request_context: request_ctx as u64,
    };

    let state = match get_module_state() {
        Some(s) => s,
        None => return STORAGE_ERR_REJECTED,
    };

    match state.dispatcher.submit_request(request) {
        SubmitResult::Ok => STORAGE_WOULDBLOCK,
        SubmitResult::Throttled => STORAGE_ERR_REJECTED,
    }
}

unsafe extern "C" fn nks_del_async(
    _ctx: *mut c_void, db_id: u32,
    key: *const c_void, klen: usize,
    request_ctx: *mut c_void,
) -> c_int {
    let _ = klen;

    let request = StorageRequest::DeleteKey {
        db_id,
        key_robj: key as *mut c_void,
    };

    let state = match get_module_state() {
        Some(s) => s,
        None => return STORAGE_ERR_REJECTED,
    };

    match state.dispatcher.submit_request(request) {
        SubmitResult::Ok => STORAGE_WOULDBLOCK,
        SubmitResult::Throttled => STORAGE_ERR_REJECTED,
    }
}

unsafe extern "C" fn nks_poll_completions(_ctx: *mut c_void, max: c_int) -> c_int {
    let state = match get_module_state() {
        Some(s) => s,
        None => return 0,
    };

    let completion_fn = match COMPLETION_FN {
        Some(f) => f,
        None => return 0,
    };

    let mut count = 0;
    while count < max {
        match state.dispatcher.poll_single_completion() {
            Some(response) => {
                let (op_type, status, value_ptr, key_ptr, req_ctx, db_id, resp_ram_bytes) = match &response {
                    StorageResponse::WriteValue { db_id, value_robj, request_context, result, ram_bytes, .. } =>
                        (STORAGE_OP_PUT,
                         if result.is_ok() { STORAGE_OK } else { STORAGE_ERR_REJECTED },
                         *value_robj, ptr::null_mut(), *request_context, *db_id, *ram_bytes),
                    StorageResponse::ReadValue { db_id, value_robj, request_context, result, .. } =>
                        (STORAGE_OP_GET,
                         if result.is_ok() { STORAGE_OK } else { STORAGE_NOT_FOUND },
                         *value_robj, ptr::null_mut(), *request_context, *db_id, 0usize),
                    StorageResponse::DeleteKey { db_id, result, .. } =>
                        (STORAGE_OP_DEL,
                         if result.is_ok() { STORAGE_OK } else { STORAGE_ERR_REJECTED },
                         ptr::null_mut(), ptr::null_mut(), 0u64, *db_id, 0usize),
                    _ => continue,
                };

                let mut comp = StorageCompletion {
                    request_ctx: req_ctx as *mut c_void,
                    op_type,
                    status,
                    db_id,
                    key: key_ptr,
                    klen: if key_ptr.is_null() { 0 } else { 1 },
                    value: value_ptr,
                    vlen: 0,
                    expire_ms: 0,
                    ram_bytes: resp_ram_bytes,
                };

                completion_fn(&mut comp, COMPLETION_PRIVDATA);
                count += 1;
            }
            None => break,
        }
    }
    count
}

// ---------------------------------------------------------------------------
// Public: get the static storageType struct
// ---------------------------------------------------------------------------

static mut NKS_STORAGE_TYPE: StorageType = StorageType {
    name: STORAGE_NAME.as_ptr() as *const c_char,
    version: VALKEY_STORAGE_VERSION,
    open: Some(nks_open),
    close: Some(nks_close),
    put: None,
    get: None,
    del: None,
    put_async: Some(nks_put_async),
    get_async: Some(nks_get_async),
    del_async: Some(nks_del_async),
    poll_completions: Some(nks_poll_completions),
    cron: None,
    get_stats: None,
};

/// Returns a pointer to the static storageType struct for registration.
pub unsafe fn get_storage_type_ptr() -> *mut c_void {
    &mut NKS_STORAGE_TYPE as *mut StorageType as *mut c_void
}
