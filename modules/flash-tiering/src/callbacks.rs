//! FFI callback functions registered with Valkey core.
//!
//! `request_callback_ffi` is called by Valkey core to submit storage requests.
//! `response_callback_ffi` is polled by Valkey core to retrieve completed responses.

use std::ffi::c_void;
use std::os::raw::{c_char, c_int, c_longlong};
use std::ptr;
use std::sync::atomic::{AtomicU64, Ordering};

use crate::MODULE_STATE;

// ---------------------------------------------------------------------------
// Metrics
// ---------------------------------------------------------------------------

static CB_REQUEST_WRITE_COUNT: AtomicU64 = AtomicU64::new(0);
static CB_REQUEST_READ_COUNT: AtomicU64 = AtomicU64::new(0);
static CB_REQUEST_DELETE_COUNT: AtomicU64 = AtomicU64::new(0);
static CB_REQUEST_OTHER_COUNT: AtomicU64 = AtomicU64::new(0);
static CB_REQUEST_ERR_COUNT: AtomicU64 = AtomicU64::new(0);
static CB_RESPONSE_WRITE_COUNT: AtomicU64 = AtomicU64::new(0);
static CB_RESPONSE_READ_COUNT: AtomicU64 = AtomicU64::new(0);
static CB_RESPONSE_READ_HIT_COUNT: AtomicU64 = AtomicU64::new(0);
static CB_RESPONSE_READ_MISS_COUNT: AtomicU64 = AtomicU64::new(0);
static CB_RESPONSE_NULL_COUNT: AtomicU64 = AtomicU64::new(0);
static CB_KEY_MAY_EXIST_COUNT: AtomicU64 = AtomicU64::new(0);
static CB_KEY_MAY_EXIST_TRUE_COUNT: AtomicU64 = AtomicU64::new(0);
static CB_KEY_MAY_EXIST_FALSE_COUNT: AtomicU64 = AtomicU64::new(0);

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

const VALKEYMODULE_OK: c_int = 0;
const VALKEYMODULE_ERR: c_int = 1;

const MSG_TYPE_WRITE: c_int = 0;
const MSG_TYPE_READ: c_int = 1;
const MSG_TYPE_DELETE: c_int = 2;
const MSG_TYPE_FLUSH_DB: c_int = 3;
const MSG_TYPE_FLUSH_ALL: c_int = 4;
const MSG_TYPE_SHUTDOWN: c_int = 8;

// ---------------------------------------------------------------------------
// ValkeyModuleExternalStorageMsg layout
// ---------------------------------------------------------------------------

#[repr(C)]
pub struct ValkeyModuleExternalStorageMsg {
    pub msg_type: c_int,
    pub status: c_int,
    pub ttl: c_longlong,
    pub db_id: c_int,
    pub key: *mut c_void,
    pub value: *mut c_void,
}

// ---------------------------------------------------------------------------
// Callback function pointer types
// ---------------------------------------------------------------------------

pub type RequestCallbackFn = Option<
    unsafe extern "C" fn(
        ctx: *mut valkey_module::raw::RedisModuleCtx,
        msg_type: c_int,
        db_id: c_int,
        key: *mut valkey_module::raw::RedisModuleString,
        ttl: c_longlong,
        data: *mut c_void,
    ) -> c_int,
>;

pub type ResponseCallbackFn = Option<
    unsafe extern "C" fn(
        ctx: *mut valkey_module::raw::RedisModuleCtx,
    ) -> *mut ValkeyModuleExternalStorageMsg,
>;

pub type KeyMayExistCallbackFn = Option<
    unsafe extern "C" fn(db_id: c_int, key: *const c_char, key_len: usize) -> c_int,
>;

pub type GetMetricsCallbackFn = Option<unsafe extern "C" fn() -> *mut c_char>;

// ---------------------------------------------------------------------------
// Request callback
// ---------------------------------------------------------------------------

pub unsafe extern "C" fn request_callback_ffi(
    _ctx: *mut valkey_module::raw::RedisModuleCtx,
    msg_type: c_int,
    db_id: c_int,
    key: *mut valkey_module::raw::RedisModuleString,
    ttl: c_longlong,
    data: *mut c_void,
) -> c_int {
    request_callback_inner(msg_type, db_id, key as *mut c_void, ttl, data)
}

fn request_callback_inner(
    msg_type: c_int,
    db_id: c_int,
    key: *mut c_void,
    ttl: c_longlong,
    value: *mut c_void,
) -> c_int {
    let mut guard = match MODULE_STATE.lock() {
        Ok(g) => g,
        Err(_) => {
            CB_REQUEST_ERR_COUNT.fetch_add(1, Ordering::Relaxed);
            return VALKEYMODULE_ERR;
        }
    };

    let state = match guard.as_mut() {
        Some(s) => s,
        None => {
            CB_REQUEST_ERR_COUNT.fetch_add(1, Ordering::Relaxed);
            return VALKEYMODULE_ERR;
        }
    };

    if state.dispatcher.is_fatal_error() {
        CB_REQUEST_ERR_COUNT.fetch_add(1, Ordering::Relaxed);
        return VALKEYMODULE_ERR;
    }

    let db = db_id as u32;

    match msg_type {
        MSG_TYPE_WRITE => {
            use crate::common::types::{StorageRequest, SubmitResult};
            CB_REQUEST_WRITE_COUNT.fetch_add(1, Ordering::Relaxed);
            let request = StorageRequest::WriteValue {
                db_id: db,
                key_robj: key,
                value_robj: value,
                request_context: 0,
            };
            match state.dispatcher.submit_request(request) {
                SubmitResult::Ok => {
                    state.pending_writes.insert(key, db);
                    VALKEYMODULE_OK
                }
                SubmitResult::Throttled => {
                    CB_REQUEST_ERR_COUNT.fetch_add(1, Ordering::Relaxed);
                    VALKEYMODULE_ERR
                }
            }
        }

        MSG_TYPE_READ => {
            use crate::common::types::{StorageRequest, SubmitResult};
            CB_REQUEST_READ_COUNT.fetch_add(1, Ordering::Relaxed);
            let request_context = state.pending_reads.insert(key, ttl, db);
            let request = StorageRequest::ReadValue {
                db_id: db,
                key_robj: key,
                request_context,
            };
            match state.dispatcher.submit_request(request) {
                SubmitResult::Ok => VALKEYMODULE_OK,
                SubmitResult::Throttled => {
                    state.pending_reads.remove(request_context);
                    CB_REQUEST_ERR_COUNT.fetch_add(1, Ordering::Relaxed);
                    VALKEYMODULE_ERR
                }
            }
        }

        MSG_TYPE_DELETE => {
            use crate::common::types::StorageRequest;
            CB_REQUEST_DELETE_COUNT.fetch_add(1, Ordering::Relaxed);
            state.dispatcher.submit_request(StorageRequest::DeleteKey {
                db_id: db,
                key_robj: key,
            });
            VALKEYMODULE_OK
        }

        MSG_TYPE_FLUSH_DB => {
            use crate::common::types::StorageRequest;
            CB_REQUEST_OTHER_COUNT.fetch_add(1, Ordering::Relaxed);
            state.dispatcher.submit_request(StorageRequest::FlushDB(db));
            VALKEYMODULE_OK
        }

        MSG_TYPE_FLUSH_ALL => {
            use crate::common::types::StorageRequest;
            CB_REQUEST_OTHER_COUNT.fetch_add(1, Ordering::Relaxed);
            state.dispatcher.submit_request(StorageRequest::FlushAll);
            VALKEYMODULE_OK
        }

        MSG_TYPE_SHUTDOWN => {
            use crate::common::types::StorageRequest;
            CB_REQUEST_OTHER_COUNT.fetch_add(1, Ordering::Relaxed);
            state.dispatcher.submit_request(StorageRequest::Shutdown);
            VALKEYMODULE_OK
        }

        _ => {
            CB_REQUEST_ERR_COUNT.fetch_add(1, Ordering::Relaxed);
            VALKEYMODULE_ERR
        }
    }
}

// ---------------------------------------------------------------------------
// Response callback
// ---------------------------------------------------------------------------

pub unsafe extern "C" fn response_callback_ffi(
    _ctx: *mut valkey_module::raw::RedisModuleCtx,
) -> *mut ValkeyModuleExternalStorageMsg {
    response_callback_inner()
}

fn response_callback_inner() -> *mut ValkeyModuleExternalStorageMsg {
    let mut guard = match MODULE_STATE.lock() {
        Ok(g) => g,
        Err(_) => return ptr::null_mut(),
    };

    let state = match guard.as_mut() {
        Some(s) => s,
        None => return ptr::null_mut(),
    };

    let response = match state.dispatcher.poll_single_completion() {
        Some(r) => r,
        None => {
            CB_RESPONSE_NULL_COUNT.fetch_add(1, Ordering::Relaxed);
            return ptr::null_mut();
        }
    };

    use crate::common::types::StorageResponse;

    let msg = match response {
        StorageResponse::WriteValue { db_id, key_robj, value_robj, result, ..
        } => {
            CB_RESPONSE_WRITE_COUNT.fetch_add(1, Ordering::Relaxed);
            // Non-key-spilling: no bloom filter update needed (key stays in dict)
            ValkeyModuleExternalStorageMsg {
                msg_type: MSG_TYPE_WRITE,
                status: if result.is_ok() { VALKEYMODULE_OK } else { VALKEYMODULE_ERR },
                ttl: -1,
                db_id: db_id as c_int,
                key: key_robj,
                value: value_robj,
            }
        }

        StorageResponse::ReadValue {
            db_id,
            key_robj,
            value_robj,
            request_context,
            result,
        } => {
            let ttl = state
                .pending_reads
                .remove(request_context)
                .map(|pr| pr.ttl)
                .unwrap_or(-1);

            CB_RESPONSE_READ_COUNT.fetch_add(1, Ordering::Relaxed);
            if !value_robj.is_null() {
                CB_RESPONSE_READ_HIT_COUNT.fetch_add(1, Ordering::Relaxed);
                // Non-key-spilling: no bloom filter update needed (key stays in dict)
            } else {
                CB_RESPONSE_READ_MISS_COUNT.fetch_add(1, Ordering::Relaxed);
            }

            ValkeyModuleExternalStorageMsg {
                msg_type: MSG_TYPE_READ,
                status: if result.is_ok() { VALKEYMODULE_OK } else { VALKEYMODULE_ERR },
                ttl,
                db_id: db_id as c_int,
                key: key_robj,
                value: value_robj,
            }
        }

        StorageResponse::DeleteKey { db_id, result } => ValkeyModuleExternalStorageMsg {
            msg_type: MSG_TYPE_DELETE,
            status: if result.is_ok() { VALKEYMODULE_OK } else { VALKEYMODULE_ERR },
            ttl: -1,
            db_id: db_id as c_int,
            key: ptr::null_mut(),
            value: ptr::null_mut(),
        },

        StorageResponse::EvictKey { db_id, key_bytes } => {
            // FlashCache GC evicted this key. Create a key robj from the raw bytes
            // and send as a DELETE completion so the C side removes it from the dict.
            let key_robj = unsafe {
                (state.serialization_callbacks.deserialize_key)(
                    key_bytes.as_ptr() as *mut c_char,
                    key_bytes.len() as c_int,
                )
            };
            ValkeyModuleExternalStorageMsg {
                msg_type: MSG_TYPE_DELETE,
                status: VALKEYMODULE_OK,
                ttl: -1,
                db_id: db_id as c_int,
                key: key_robj,
                value: ptr::null_mut(),
            }
        }

        StorageResponse::FlushDB(result) => ValkeyModuleExternalStorageMsg {
            msg_type: MSG_TYPE_FLUSH_DB,
            status: if result.is_ok() { VALKEYMODULE_OK } else { VALKEYMODULE_ERR },
            ttl: -1,
            db_id: 0,
            key: ptr::null_mut(),
            value: ptr::null_mut(),
        },

        StorageResponse::FlushAll(result) => ValkeyModuleExternalStorageMsg {
            msg_type: MSG_TYPE_FLUSH_ALL,
            status: if result.is_ok() { VALKEYMODULE_OK } else { VALKEYMODULE_ERR },
            ttl: -1,
            db_id: 0,
            key: ptr::null_mut(),
            value: ptr::null_mut(),
        },

        StorageResponse::Shutdown(result) => ValkeyModuleExternalStorageMsg {
            msg_type: MSG_TYPE_SHUTDOWN,
            status: if result.is_ok() { VALKEYMODULE_OK } else { VALKEYMODULE_ERR },
            ttl: -1,
            db_id: 0,
            key: ptr::null_mut(),
            value: ptr::null_mut(),
        },

        // Byte-based responses are handled by storage_type_bridge, not this callback
        StorageResponse::ReadBytes { .. } |
        StorageResponse::WriteBytes { .. } |
        StorageResponse::DeleteBytes { .. } => {
            return ptr::null_mut();
        }
    };

    Box::into_raw(Box::new(msg))
}

// ---------------------------------------------------------------------------
// Key existence check callback
// ---------------------------------------------------------------------------

pub unsafe extern "C" fn key_may_exist_callback(
    _db_id: c_int,
    _key: *const c_char,
    _key_len: usize,
) -> c_int {
    // Non-key-spilling mode: keys are always in the dict. If a key is not
    // in the dict, it was explicitly deleted and should NOT be fetched from
    // disk. Always return 0 (key does not exist on disk).
    CB_KEY_MAY_EXIST_COUNT.fetch_add(1, Ordering::Relaxed);
    CB_KEY_MAY_EXIST_FALSE_COUNT.fetch_add(1, Ordering::Relaxed);
    0
}

// ---------------------------------------------------------------------------
// Metrics callback — no FlashCache dependency
// ---------------------------------------------------------------------------

/// Metrics callback registered with Valkey core.
/// Returns a heap-allocated C string with metrics in "key:value\r\n" format.
pub unsafe extern "C" fn get_metrics_callback() -> *mut c_char {
    let guard = match MODULE_STATE.lock() {
        Ok(g) => g,
        Err(_) => return ptr::null_mut(),
    };
    let state = match guard.as_ref() {
        Some(s) => s,
        None => return ptr::null_mut(),
    };

    let metrics_str = state.dispatcher.get_metrics_string();

    // Allocate via libc malloc so the caller can free with free()
    let c_str = match std::ffi::CString::new(metrics_str) {
        Ok(s) => s,
        Err(_) => return ptr::null_mut(),
    };
    let len = c_str.as_bytes_with_nul().len();
    let buf = libc::malloc(len) as *mut c_char;
    if buf.is_null() {
        return ptr::null_mut();
    }
    ptr::copy_nonoverlapping(c_str.as_ptr(), buf, len);
    buf
}
