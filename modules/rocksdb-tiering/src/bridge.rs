//! storageType bridge — C-compatible function pointers that delegate to RocksDBAsio.

use std::ffi::c_void;
use std::os::raw::{c_char, c_int};
use std::ptr;

use crate::STATE;
use crate::asio::Response;

// ---------------------------------------------------------------------------
// storageType C struct layout (must match src/storage/storage.h)
// ---------------------------------------------------------------------------

const VALKEY_STORAGE_VERSION: c_int = 1;
const STORAGE_OK: c_int = 0;
const STORAGE_NOT_FOUND: c_int = 1;
const STORAGE_WOULDBLOCK: c_int = 2;
const STORAGE_ERR_REJECTED: c_int = -3;

const STORAGE_OP_PUT: c_int = 0;
const STORAGE_OP_GET: c_int = 1;
const STORAGE_OP_DEL: c_int = 2;

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
}

type StorageCompletionFn = unsafe extern "C" fn(*mut StorageCompletion, *mut c_void);

#[repr(C)]
pub struct StorageConfig {
    pub path: *const c_char,
    pub capacity_bytes: usize,
    pub num_databases: u32,
    pub io_threads: c_int,
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
// State
// ---------------------------------------------------------------------------

static STORAGE_NAME: &[u8] = b"rocksdb-tiering\0";
static mut COMPLETION_FN: Option<StorageCompletionFn> = None;
static mut COMPLETION_PRIVDATA: *mut c_void = ptr::null_mut();

// ---------------------------------------------------------------------------
// storageType callbacks
// ---------------------------------------------------------------------------

unsafe extern "C" fn rdb_open(cfg: *mut StorageConfig) -> *mut c_void {
    if !cfg.is_null() {
        let c = &*cfg;
        COMPLETION_FN = c.completion_fn;
        COMPLETION_PRIVDATA = c.completion_privdata;
    }
    1usize as *mut c_void // sentinel — state already in MODULE_STATE
}

unsafe extern "C" fn rdb_close(_: *mut c_void) {}

unsafe extern "C" fn rdb_put_async(
    _: *mut c_void, db_id: u32,
    key: *const c_void, _klen: usize,
    value: *const c_void, _vlen: usize,
    _expire_ms: i64, request_ctx: *mut c_void,
) -> c_int {
    let guard = match STATE.lock() {
        Ok(g) => g,
        Err(_) => return STORAGE_ERR_REJECTED,
    };
    let asio = match guard.as_ref() {
        Some(a) => a,
        None => return STORAGE_ERR_REJECTED,
    };
    if asio.submit_put(db_id, key as *mut c_void, value as *mut c_void, request_ctx as u64) {
        STORAGE_WOULDBLOCK
    } else {
        STORAGE_ERR_REJECTED
    }
}

unsafe extern "C" fn rdb_get_async(
    _: *mut c_void, db_id: u32,
    key: *const c_void, _klen: usize,
    request_ctx: *mut c_void,
) -> c_int {
    let guard = match STATE.lock() {
        Ok(g) => g,
        Err(_) => return STORAGE_ERR_REJECTED,
    };
    let asio = match guard.as_ref() {
        Some(a) => a,
        None => return STORAGE_ERR_REJECTED,
    };
    if asio.submit_get(db_id, key as *mut c_void, request_ctx as u64) {
        STORAGE_WOULDBLOCK
    } else {
        STORAGE_ERR_REJECTED
    }
}

unsafe extern "C" fn rdb_del_async(
    _: *mut c_void, db_id: u32,
    key: *const c_void, _klen: usize,
    request_ctx: *mut c_void,
) -> c_int {
    let guard = match STATE.lock() {
        Ok(g) => g,
        Err(_) => return STORAGE_ERR_REJECTED,
    };
    let asio = match guard.as_ref() {
        Some(a) => a,
        None => return STORAGE_ERR_REJECTED,
    };
    if asio.submit_del(db_id, key as *mut c_void, request_ctx as u64) {
        STORAGE_WOULDBLOCK
    } else {
        STORAGE_ERR_REJECTED
    }
}

unsafe extern "C" fn rdb_poll_completions(_: *mut c_void, max: c_int) -> c_int {
    let guard = match STATE.lock() {
        Ok(g) => g,
        Err(_) => return 0,
    };
    let asio = match guard.as_ref() {
        Some(a) => a,
        None => return 0,
    };
    let completion_fn = match COMPLETION_FN {
        Some(f) => f,
        None => return 0,
    };

    let mut count = 0;
    while count < max {
        match asio.poll() {
            Some(resp) => {
                let mut comp = StorageCompletion {
                    request_ctx: ptr::null_mut(),
                    op_type: 0,
                    status: STORAGE_OK,
                    db_id: 0,
                    key: ptr::null_mut(),
                    klen: 0,
                    value: ptr::null_mut(),
                    vlen: 0,
                    expire_ms: 0,
                };

                match resp {
                    Response::Put { db_id, value_robj, ctx, ok } => {
                        comp.op_type = STORAGE_OP_PUT;
                        comp.db_id = db_id;
                        comp.request_ctx = ctx as *mut c_void;
                        comp.value = value_robj;
                        comp.status = if ok { STORAGE_OK } else { STORAGE_ERR_REJECTED };
                    }
                    Response::Get { db_id, value_robj, ctx, ok } => {
                        comp.op_type = STORAGE_OP_GET;
                        comp.db_id = db_id;
                        comp.request_ctx = ctx as *mut c_void;
                        comp.value = value_robj;
                        comp.status = if ok { STORAGE_OK } else { STORAGE_NOT_FOUND };
                    }
                    Response::Del { db_id, ctx, ok } => {
                        eprintln!("[rdb-tiering] poll: DEL completion db_id={} ok={} ctx={:#x}", db_id, ok, ctx);
                        comp.op_type = STORAGE_OP_DEL;
                        comp.db_id = db_id;
                        comp.request_ctx = ctx as *mut c_void;
                        comp.status = if ok { STORAGE_OK } else { STORAGE_NOT_FOUND };
                    }
                }

                completion_fn(&mut comp, COMPLETION_PRIVDATA);
                count += 1;
            }
            None => break,
        }
    }
    count
}

// ---------------------------------------------------------------------------
// Static storageType struct
// ---------------------------------------------------------------------------

static mut RDB_STORAGE_TYPE: StorageType = StorageType {
    name: STORAGE_NAME.as_ptr() as *const c_char,
    version: VALKEY_STORAGE_VERSION,
    open: Some(rdb_open),
    close: Some(rdb_close),
    put: None,
    get: None,
    del: None,
    put_async: Some(rdb_put_async),
    get_async: Some(rdb_get_async),
    del_async: Some(rdb_del_async),
    poll_completions: Some(rdb_poll_completions),
    cron: None,
    get_stats: None,
};

pub unsafe fn get_storage_type_ptr() -> *mut c_void {
    &raw mut RDB_STORAGE_TYPE as *mut StorageType as *mut c_void
}
