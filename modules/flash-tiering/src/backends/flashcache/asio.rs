//! FlashCache ASIO layer — 1 IO thread, async reads via callbacks, cron ticks.
//!
//! Reads are truly async: `flashcacheGetItem` returns immediately and the
//! completion callback fires later during `flashcacheRunCronTasks()`.
//! Writes are synchronous (staging buffer).

use std::ffi::c_void;
use std::os::raw::c_char;
use std::ptr;
use std::sync::atomic::{AtomicBool, AtomicU32, AtomicU64, Ordering};
use std::sync::Arc;
use std::thread::{self, JoinHandle};
use std::time::{Duration, Instant};

use crossbeam_queue::{ArrayQueue, SegQueue};
use once_cell::sync::Lazy;

use crate::common::backend_trait::DataTieringBackend;
use crate::common::serialization::SerializationCallbacks;
use crate::common::types::{
    BackendError, BackendResult, StorageRequest, StorageResponse, SubmitResult,
};
use super::backend::FlashCacheBackend;
use super::ffi;

// ---------------------------------------------------------------------------
// Metrics
// ---------------------------------------------------------------------------

static ASIO_WRITES_SUBMITTED: AtomicU64 = AtomicU64::new(0);
static ASIO_READS_SUBMITTED: AtomicU64 = AtomicU64::new(0);
static ASIO_WRITES_PROCESSED: AtomicU64 = AtomicU64::new(0);
static ASIO_READS_PROCESSED: AtomicU64 = AtomicU64::new(0);
static ASIO_READS_THROTTLED: AtomicU64 = AtomicU64::new(0);
static ASIO_COMPLETIONS_POLLED: AtomicU64 = AtomicU64::new(0);
static ASIO_EVICTIONS_RECEIVED: AtomicU64 = AtomicU64::new(0);
static ASIO_CONTROL_MSG_RECEIVED: AtomicU64 = AtomicU64::new(0);

// ---------------------------------------------------------------------------
// Global eviction queue and control msg flag (accessed from C callbacks)
// ---------------------------------------------------------------------------

/// Eviction event pushed by FlashCache GC callback.
struct EvictionEvent {
    db_id: u32,
    key: Vec<u8>,
}

static EVICTION_QUEUE: Lazy<SegQueue<EvictionEvent>> =
    Lazy::new(|| SegQueue::new());

static ASIO_CONTROL_MSG_FLAG: AtomicBool = AtomicBool::new(false);

/// Called from C (fc_shim.c) on the IO thread when FlashCache evicts a key.
#[no_mangle]
pub extern "C" fn fc_eviction_callback_rust(dbid: u32, key: *const c_char, key_len: usize) {
    if key.is_null() || key_len == 0 {
        return;
    }
    let key_bytes = unsafe { std::slice::from_raw_parts(key as *const u8, key_len) };
    EVICTION_QUEUE.push(EvictionEvent {
        db_id: dbid,
        key: key_bytes.to_vec(),
    });
    ASIO_EVICTIONS_RECEIVED.fetch_add(1, Ordering::Relaxed);
}

/// Called from C (fc_shim.c) on the IO thread when FlashCache has completions ready.
#[no_mangle]
pub extern "C" fn fc_asio_control_msg_rust() {
    ASIO_CONTROL_MSG_FLAG.store(true, Ordering::Release);
    ASIO_CONTROL_MSG_RECEIVED.fetch_add(1, Ordering::Relaxed);
}

// ---------------------------------------------------------------------------
// Async read context
// ---------------------------------------------------------------------------

struct FcAsyncReadContext {
    db_id: u32,
    key_robj: *mut c_void,
    request_context: u64,
    completion_queue: Arc<ArrayQueue<StorageResponse>>,
    callbacks: SerializationCallbacks,
    current_in_flight_reads: Arc<AtomicU32>,
}

unsafe impl Send for FcAsyncReadContext {}

unsafe extern "C" fn fc_async_read_callback(
    request_context: *mut c_void,
    value: *mut c_char,
    value_len: usize,
    _add_item_to_rdb: std::os::raw::c_int,
) {
    let ctx = Box::from_raw(request_context as *mut FcAsyncReadContext);

    let (value_robj, result) = if !value.is_null() && value_len > 0 {
        match ctx
            .callbacks
            .deserialize_value_from_buf(value as *const c_char, value_len)
        {
            Ok(robj) => {
                ffi::fc_record_get_completion(true, false);
                (robj, Ok(()))
            }
            Err(e) => {
                ffi::fc_record_get_completion(true, false);
                (ptr::null_mut(), Err(e))
            }
        }
    } else {
        ffi::fc_record_get_completion(false, false);
        (ptr::null_mut(), Ok(()))
    };

    ctx.current_in_flight_reads.fetch_sub(1, Ordering::AcqRel);
    ASIO_READS_PROCESSED.fetch_add(1, Ordering::Relaxed);

    let _ = ctx.completion_queue.push(StorageResponse::ReadValue {
        db_id: ctx.db_id,
        key_robj: ctx.key_robj,
        value_robj,
        request_context: ctx.request_context,
        result,
    });
}

// ---------------------------------------------------------------------------
// FlashCacheAsio
// ---------------------------------------------------------------------------

pub struct FlashCacheAsio {
    read_queue: Arc<ArrayQueue<StorageRequest>>,
    write_queue: Arc<ArrayQueue<StorageRequest>>,
    completion_queue: Arc<ArrayQueue<StorageResponse>>,
    worker_handle: Option<JoinHandle<()>>,
    shutdown_flag: Arc<AtomicBool>,
    max_in_flight_reads: u32,
    current_in_flight_reads: Arc<AtomicU32>,
    fatal_error: Arc<AtomicBool>,
}

impl FlashCacheAsio {
    pub fn new(
        backend: FlashCacheBackend,
        max_in_flight_reads: u32,
        callbacks: SerializationCallbacks,
    ) -> BackendResult<Self> {
        let read_queue = Arc::new(ArrayQueue::new(1024));
        let write_queue = Arc::new(ArrayQueue::new(8192));
        let completion_queue = Arc::new(ArrayQueue::new(8192));
        let shutdown_flag = Arc::new(AtomicBool::new(false));
        let current_in_flight_reads = Arc::new(AtomicU32::new(0));
        let fatal_error = Arc::new(AtomicBool::new(false));

        let worker_handle = {
            let rq = Arc::clone(&read_queue);
            let wq = Arc::clone(&write_queue);
            let cq = Arc::clone(&completion_queue);
            let sf = Arc::clone(&shutdown_flag);
            let cir = Arc::clone(&current_in_flight_reads);

            thread::Builder::new()
                .name("ks-fc-io".into())
                .spawn(move || {
                    fc_io_worker_loop(backend, rq, wq, cq, sf, cir, callbacks);
                })
                .map_err(|e| {
                    BackendError::FatalError(format!(
                        "Failed to spawn FlashCache IO worker: {e}"
                    ))
                })?
        };

        Ok(Self {
            read_queue,
            write_queue,
            completion_queue,
            worker_handle: Some(worker_handle),
            shutdown_flag,
            max_in_flight_reads,
            current_in_flight_reads,
            fatal_error,
        })
    }

    pub fn submit_request(&self, request: StorageRequest) -> SubmitResult {
        match &request {
            StorageRequest::ReadValue { .. } => {
                self.current_in_flight_reads.fetch_add(1, Ordering::AcqRel);
                ASIO_READS_SUBMITTED.fetch_add(1, Ordering::Relaxed);
                let _ = self.read_queue.push(request);
            }
            StorageRequest::WriteValue { db_id, key_robj, value_robj, request_context, .. } => {
                ASIO_WRITES_SUBMITTED.fetch_add(1, Ordering::Relaxed);
                let _ = self.write_queue.push(request);
            }
            _ => {
                let _ = self.write_queue.push(request);
            }
        }
        SubmitResult::Ok
    }

    pub fn poll_single_completion(&self) -> Option<StorageResponse> {
        // First, drain any eviction events from the FC GC callback
        while let Some(evt) = EVICTION_QUEUE.pop() {
            let _ = self.completion_queue.push(StorageResponse::EvictKey {
                db_id: evt.db_id,
                key_bytes: evt.key,
            });
        }

        let resp = self.completion_queue.pop();
        if resp.is_some() {
            ASIO_COMPLETIONS_POLLED.fetch_add(1, Ordering::Relaxed);
        }
        resp
    }

    /// Returns true if FlashCache signaled that completions are ready.
    /// Clears the flag after reading.
    pub fn has_pending_control_msg(&self) -> bool {
        ASIO_CONTROL_MSG_FLAG.compare_exchange(true, false, Ordering::AcqRel, Ordering::Acquire)
            .is_ok()
    }

    pub fn shutdown(&mut self) -> BackendResult<()> {
        self.shutdown_flag.store(true, Ordering::Release);
        if let Some(handle) = self.worker_handle.take() {
            handle.join().map_err(|_| {
                BackendError::FatalError("FlashCache IO worker panicked during shutdown".into())
            })?;
        }
        Ok(())
    }

    pub fn is_fatal_error(&self) -> bool {
        self.fatal_error.load(Ordering::Acquire)
    }

    pub fn key_may_exist(&self, db_id: u32, key: &[u8]) -> bool {
        ffi::fc_key_exists(db_id, key)
    }

    pub fn get_metrics_string(&self) -> String {
        let ws = ASIO_WRITES_SUBMITTED.load(Ordering::Relaxed);
        let wp = ASIO_WRITES_PROCESSED.load(Ordering::Relaxed);
        let rs = ASIO_READS_SUBMITTED.load(Ordering::Relaxed);
        let rp = ASIO_READS_PROCESSED.load(Ordering::Relaxed);
        let rt = ASIO_READS_THROTTLED.load(Ordering::Relaxed);
        let cp = ASIO_COMPLETIONS_POLLED.load(Ordering::Relaxed);
        format!(
            "asio_write_queue_depth:{}\r\nasio_read_queue_depth:{}\r\nasio_writes_submitted:{}\r\nasio_writes_processed:{}\r\nasio_reads_submitted:{}\r\nasio_reads_processed:{}\r\nasio_reads_throttled:{}\r\nasio_completions_polled:{}\r\n",
            ws.saturating_sub(wp), rs.saturating_sub(rp),
            ws, wp, rs, rp, rt, cp,
        )
    }
}

// ---------------------------------------------------------------------------
// IO worker loop — 1 thread, async reads, cron ticks
// ---------------------------------------------------------------------------

fn fc_io_worker_loop(
    backend: FlashCacheBackend,
    read_queue: Arc<ArrayQueue<StorageRequest>>,
    write_queue: Arc<ArrayQueue<StorageRequest>>,
    completion_queue: Arc<ArrayQueue<StorageResponse>>,
    shutdown_flag: Arc<AtomicBool>,
    current_in_flight_reads: Arc<AtomicU32>,
    callbacks: SerializationCallbacks,
) {
    while !shutdown_flag.load(Ordering::Relaxed) {
        let mut did_work = false;

        // 1. Process all pending async reads (batch)
        while let Some(req) = read_queue.pop() {
            unsafe {
                submit_fc_async_read(req, &callbacks, &completion_queue, &current_in_flight_reads);
            }
            did_work = true;
        }

        // 2. Process all pending writes (synchronous, batch)
        while let Some(req) = write_queue.pop() {
            let resp = unsafe {
                process_write_request(&backend, req, &callbacks)
            };
            let _ = completion_queue.push(resp);
            did_work = true;
        }

        // 3. Run cron tasks only when idle (not between write batches)
        //    This matches native behavior where cron runs from the main thread timer,
        //    not in the IO thread hot path.
        if !did_work {
            backend.tick();
        }

        // 4. Sleep only when idle
        if !did_work && !ffi::fc_should_run_cron_immediately() {
            thread::sleep(Duration::from_micros(50));
        }
    }

    // Drain remaining
    while let Some(req) = write_queue.pop() {
        let resp = unsafe { process_write_request(&backend, req, &callbacks) };
        let _ = completion_queue.push(resp);
    }
    while let Some(req) = read_queue.pop() {
        unsafe {
            submit_fc_async_read(req, &callbacks, &completion_queue, &current_in_flight_reads);
        }
    }
    // Final ticks to flush pending AIO
    for _ in 0..100 {
        backend.tick();
        thread::sleep(Duration::from_millis(1));
    }
}

unsafe fn submit_fc_async_read(
    request: StorageRequest,
    callbacks: &SerializationCallbacks,
    completion_queue: &Arc<ArrayQueue<StorageResponse>>,
    current_in_flight_reads: &Arc<AtomicU32>,
) {
    let (db_id, key_robj, request_context) = match request {
        StorageRequest::ReadValue {
            db_id,
            key_robj,
            request_context,
        } => (db_id, key_robj, request_context),
        _ => return,
    };

    let (key_buf, key_len) = match callbacks.serialize_key_to_buf(key_robj) {
        Ok(v) => v,
        Err(e) => {
            current_in_flight_reads.fetch_sub(1, Ordering::AcqRel);
            let _ = completion_queue.push(StorageResponse::ReadValue {
                db_id,
                key_robj,
                value_robj: ptr::null_mut(),
                request_context,
                result: Err(e),
            });
            return;
        }
    };

    let key_bytes = std::slice::from_raw_parts(key_buf as *const u8, key_len);

    let ctx = Box::new(FcAsyncReadContext {
        db_id,
        key_robj,
        request_context,
        completion_queue: Arc::clone(completion_queue),
        callbacks: *callbacks,
        current_in_flight_reads: Arc::clone(current_in_flight_reads),
    });
    let ctx_ptr = Box::into_raw(ctx) as *mut c_void;

    {
        static GET_LOG_COUNT: std::sync::atomic::AtomicU64 = std::sync::atomic::AtomicU64::new(0);
        if GET_LOG_COUNT.fetch_add(1, Ordering::Relaxed) < 5 {
            eprintln!("FC_GET: db={} key_len={} key={:?}", db_id, key_bytes.len(), String::from_utf8_lossy(key_bytes));
        }
    }
    let result = ffi::fc_submit_get(
        db_id,
        key_bytes,
        ffi::FlashcacheReadTypes::FcRead,
        ctx_ptr,
        fc_async_read_callback,
    );

    callbacks.free_serialized_key_buf(key_buf);

    if let Err(e) = result {
        let ctx = Box::from_raw(ctx_ptr as *mut FcAsyncReadContext);
        ctx.current_in_flight_reads.fetch_sub(1, Ordering::AcqRel);
        let err = if e.contains("throttled") {
            BackendError::Throttled
        } else {
            BackendError::OperationFailed(e)
        };
        let _ = completion_queue.push(StorageResponse::ReadValue {
            db_id,
            key_robj,
            value_robj: ptr::null_mut(),
            request_context,
            result: Err(err),
        });
    }
}

unsafe fn process_write_request(
    backend: &FlashCacheBackend,
    request: StorageRequest,
    callbacks: &SerializationCallbacks,
) -> StorageResponse {
    match request {
        StorageRequest::WriteValue {
            db_id,
            key_robj,
            value_robj, request_context, .. } => {
            let (key_buf, key_len) = match callbacks.serialize_key_to_buf(key_robj) {
                Ok(v) => v,
                Err(e) => {
                    return StorageResponse::WriteValue {
                        db_id,
                        key_robj,
                        value_robj,
                        request_context,
                        result: Err(e),
                        ram_bytes: 0,
                    }
                }
            };
            let (value_buf, value_len) = match callbacks.serialize_value_to_buf(value_robj) {
                Ok(v) => v,
                Err(e) => {
                    callbacks.free_serialized_key_buf(key_buf);
                    return StorageResponse::WriteValue {
                        db_id,
                        key_robj,
                        value_robj,
                        request_context,
                        result: Err(e),
                        ram_bytes: 0,
                    };
                }
            };
            let key_bytes = std::slice::from_raw_parts(key_buf as *const u8, key_len);
            let value_bytes = std::slice::from_raw_parts(value_buf as *const u8, value_len);

            // Smith predictor: compute RAM footprint, advance window-1→window-2
            let ram_bytes = crate::objectComputeSize(
                std::ptr::null(), value_robj, 5, db_id as std::ffi::c_int);
            crate::extStorageInflightAddRam(ram_bytes);
            crate::extStorageOnSpillSerialize(ram_bytes);
            if ASIO_WRITES_PROCESSED.load(Ordering::Relaxed) < 5 {
                eprintln!("FC_PUT: db={} key_len={} val_len={} key={:?}", db_id, key_bytes.len(), value_bytes.len(), String::from_utf8_lossy(key_bytes));
            }
            let result = backend.put_item(db_id, key_bytes, value_bytes);
            callbacks.free_serialized_key_buf(key_buf);
            callbacks.free_serialized_value_buf(value_buf);
            ASIO_WRITES_PROCESSED.fetch_add(1, Ordering::Relaxed);
            StorageResponse::WriteValue {
                db_id,
                key_robj,
                value_robj,
                request_context,
                result,
                ram_bytes,
            }
        }
        StorageRequest::DeleteKey { db_id, key_robj: _ } => {
            StorageResponse::DeleteKey {
                db_id,
                result: Ok(()),
            }
        }
        StorageRequest::FlushDB(db_id) => StorageResponse::FlushDB(backend.flush_db(db_id)),
        StorageRequest::FlushAll => StorageResponse::FlushAll(backend.flush_all()),
        StorageRequest::Shutdown => StorageResponse::Shutdown(Ok(())),
        _ => StorageResponse::Shutdown(Ok(())),
    }
}
