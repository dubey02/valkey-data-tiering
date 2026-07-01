//! Safe FFI wrappers for the FlashCache C API.
//!
//! Only compiled when feature `backend-flashcache` is enabled.

use std::ffi::{c_void, CString};
use std::os::raw::{c_char, c_int};
use std::sync::atomic::{AtomicU64, Ordering};
use std::time::Instant;

// ---------------------------------------------------------------------------
// C type aliases
// ---------------------------------------------------------------------------

#[repr(C)]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum FlashcacheReturnCode {
    FcOk = 0,
    FcErrSetupDbFile = 1,
    FcErrThrottled = 2,
    FcErrCatchAll = 3,
}

#[repr(C)]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum FlashcacheReadTypes {
    FcRead = 0,
    FcDelete = 1,
}

#[repr(C)]
pub struct FlashcacheEvictionDetails {
    pub context: *mut c_void,
    pub callback: unsafe extern "C" fn(*mut c_void, u32, *mut c_char, usize),
}

#[repr(C)]
pub struct FlashcacheAsioControlMsgCallbackDetails {
    pub context: *mut c_void,
    pub callback: unsafe extern "C" fn(*mut c_void),
}

pub type FlashcacheGetItemCallback = unsafe extern "C" fn(
    request_context: *mut c_void,
    value: *mut c_char,
    value_len: usize,
    add_item_to_rdb: c_int,
);

// ---------------------------------------------------------------------------
// Extern "C" declarations
// ---------------------------------------------------------------------------

extern "C" {
    fn fc_shim_init(
        db_filename: *const c_char, db_size_bytes: usize,
        initial_index_size_per_db: usize, num_databases: u32,
        max_allocated_db_size_percent: u32, max_num_in_flight_read_requests: u32,
        min_garbage_collection_rate: u32, evict_under_max_logsize_time_limit: u32,
        optimized_delete_enabled: u8, eviction_enabled: c_int,
        eviction_details: *mut FlashcacheEvictionDetails,
        asio_control_msg_callback_details: *mut FlashcacheAsioControlMsgCallbackDetails,
    ) -> FlashcacheReturnCode;

    pub fn flashcachePutItem(
        dbid: u32, key: *const c_char, key_len: usize,
        value: *const c_char, value_len: usize,
    ) -> FlashcacheReturnCode;

    pub fn flashcacheGetItem(
        dbid: u32, key: *const c_char, key_len: usize,
        read_type: FlashcacheReadTypes,
        request_context: *mut c_void,
        completion_callback: FlashcacheGetItemCallback,
    ) -> FlashcacheReturnCode;

    pub fn flashcacheRunCronTasks() -> FlashcacheReturnCode;
    pub fn flashcacheShouldRunCronTasksImmediately() -> c_int;
    fn flashcacheFlushDB(dbid: u32) -> FlashcacheReturnCode;
    fn flashcacheFlushAllDBs() -> FlashcacheReturnCode;
    fn flashcacheTearDown() -> FlashcacheReturnCode;
    fn flashcacheKeyExists(dbid: u32, key: *const c_char, key_len: usize) -> c_int;

    fn fc_shim_noop_eviction(context: *mut c_void, dbid: u32, key: *mut c_char, key_len: usize);
    fn fc_shim_noop_asio_control_msg(context: *mut c_void);
    fn fc_shim_print_metrics();
    fn fc_shim_run_cron_tasks();
    fn fc_shim_set_config(key: c_int, value: i64);
    pub fn fc_shim_get_metrics_string() -> *mut c_char;
    pub fn fc_shim_concat_metrics(base: *mut c_char, append: *const c_char) -> *mut c_char;
}

// ---------------------------------------------------------------------------
// Metrics
// ---------------------------------------------------------------------------

static FFI_PUT_COUNT: AtomicU64 = AtomicU64::new(0);
static FFI_GET_SUBMIT_COUNT: AtomicU64 = AtomicU64::new(0);
static FFI_GET_COMPLETE_COUNT: AtomicU64 = AtomicU64::new(0);
static FFI_GET_HIT_COUNT: AtomicU64 = AtomicU64::new(0);
static FFI_GET_MISS_COUNT: AtomicU64 = AtomicU64::new(0);
static FFI_CRON_TASKS_COUNT: AtomicU64 = AtomicU64::new(0);

// ---------------------------------------------------------------------------
// Safe wrappers
// ---------------------------------------------------------------------------

pub fn fc_init(
    db_filename: &str, db_size_bytes: usize,
    initial_index_size_per_db: usize, num_databases: u32,
    max_in_flight_reads: u32, eviction_enabled: bool,
) -> Result<(), String> {
    let c_filename = CString::new(db_filename)
        .map_err(|e| format!("Invalid db_filename: {e}"))?;

    static EVICTION_SENTINEL: u8 = 0;
    let mut eviction_details = FlashcacheEvictionDetails {
        context: &EVICTION_SENTINEL as *const u8 as *mut c_void,
        callback: fc_shim_noop_eviction,
    };
    static ASIO_SENTINEL: u8 = 0;
    let mut asio_callback_details = FlashcacheAsioControlMsgCallbackDetails {
        context: &ASIO_SENTINEL as *const u8 as *mut c_void,
        callback: fc_shim_noop_asio_control_msg,
    };

    let rc = unsafe {
        fc_shim_init(
            c_filename.as_ptr(), db_size_bytes, initial_index_size_per_db,
            num_databases, 90, max_in_flight_reads, 1024 * 1024, 5, 0,
            if eviction_enabled { 1 } else { 0 },
            &mut eviction_details, &mut asio_callback_details,
        )
    };
    match rc {
        FlashcacheReturnCode::FcOk => Ok(()),
        FlashcacheReturnCode::FcErrSetupDbFile =>
            Err(format!("FlashCache init failed: could not set up DB file '{db_filename}'")),
        other => Err(format!("FlashCache init failed with code: {other:?}")),
    }
}

pub fn fc_put(db_id: u32, key: &[u8], value: &[u8]) -> Result<(), String> {
    FFI_PUT_COUNT.fetch_add(1, Ordering::Relaxed);
    let rc = unsafe {
        flashcachePutItem(db_id, key.as_ptr() as *const c_char, key.len(),
                          value.as_ptr() as *const c_char, value.len())
    };
    match rc {
        FlashcacheReturnCode::FcOk => Ok(()),
        FlashcacheReturnCode::FcErrThrottled => Err("FlashCache put throttled".into()),
        other => Err(format!("FlashCache put failed: {other:?}")),
    }
}

pub fn fc_submit_get(
    db_id: u32, key: &[u8], read_type: FlashcacheReadTypes,
    request_context: *mut c_void, completion_callback: FlashcacheGetItemCallback,
) -> Result<(), String> {
    FFI_GET_SUBMIT_COUNT.fetch_add(1, Ordering::Relaxed);
    let rc = unsafe {
        flashcacheGetItem(
            db_id, key.as_ptr() as *const c_char, key.len(),
            read_type, request_context, completion_callback,
        )
    };
    match rc {
        FlashcacheReturnCode::FcOk => Ok(()),
        FlashcacheReturnCode::FcErrThrottled => Err("FlashCache get throttled".into()),
        other => Err(format!("FlashCache get failed: {other:?}")),
    }
}

pub fn fc_record_get_completion(hit: bool, _immediate: bool) {
    FFI_GET_COMPLETE_COUNT.fetch_add(1, Ordering::Relaxed);
    if hit { FFI_GET_HIT_COUNT.fetch_add(1, Ordering::Relaxed); }
    else { FFI_GET_MISS_COUNT.fetch_add(1, Ordering::Relaxed); }
}

pub fn fc_flush_db(db_id: u32) -> Result<(), String> {
    let rc = unsafe { flashcacheFlushDB(db_id) };
    match rc {
        FlashcacheReturnCode::FcOk => Ok(()),
        other => Err(format!("FlashCache flushDB failed: {other:?}")),
    }
}

pub fn fc_flush_all() -> Result<(), String> {
    let rc = unsafe { flashcacheFlushAllDBs() };
    match rc {
        FlashcacheReturnCode::FcOk => Ok(()),
        other => Err(format!("FlashCache flushAll failed: {other:?}")),
    }
}

pub fn fc_teardown() -> Result<(), String> {
    let rc = unsafe { flashcacheTearDown() };
    match rc {
        FlashcacheReturnCode::FcOk => Ok(()),
        other => Err(format!("FlashCache teardown failed: {other:?}")),
    }
}

pub fn fc_key_exists(db_id: u32, key: &[u8]) -> bool {
    unsafe { flashcacheKeyExists(db_id, key.as_ptr() as *const c_char, key.len()) != 0 }
}

pub fn fc_run_cron_tasks() {
    unsafe { fc_shim_run_cron_tasks(); }
    FFI_CRON_TASKS_COUNT.fetch_add(1, Ordering::Relaxed);
}

pub fn fc_should_run_cron_immediately() -> bool {
    unsafe { flashcacheShouldRunCronTasksImmediately() != 0 }
}

pub const FC_CONFIG_MAX_BUFFERED_WRITE_SIZE_BYTES: c_int = 2;
pub const FC_CONFIG_BUFFERED_WRITE_FLUSH_THRESHOLD_BYTES: c_int = 3;

pub fn fc_set_config(key: c_int, value: i64) {
    unsafe { fc_shim_set_config(key, value); }
}
