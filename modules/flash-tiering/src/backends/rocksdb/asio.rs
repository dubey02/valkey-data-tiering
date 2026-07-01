//! RocksDB ASIO layer — multi-threaded IO pool, all synchronous operations.
//!
//! Unlike FlashCache, RocksDB reads are blocking so the worker loop is
//! straightforward: dequeue request, execute synchronously, enqueue response.

use std::ffi::c_void;
use std::os::raw::c_char;
use std::ptr;
use std::sync::atomic::{AtomicBool, AtomicU32, AtomicU64, Ordering};
use std::sync::Arc;
use std::thread::{self, JoinHandle};
use std::time::{Duration, Instant};

use crossbeam_queue::SegQueue;
use rocksdb::DB;

use crate::common::serialization::SerializationCallbacks;
use crate::common::types::{
    BackendError, BackendResult, StorageRequest, StorageResponse, SubmitResult,
};
use super::backend::{cf_name, RocksDBBackend};
use crate::common::backend_trait::DataTieringBackend;

// ---------------------------------------------------------------------------
// Metrics
// ---------------------------------------------------------------------------

static ASIO_WRITES_SUBMITTED: AtomicU64 = AtomicU64::new(0);
static ASIO_READS_SUBMITTED: AtomicU64 = AtomicU64::new(0);
static ASIO_WRITES_PROCESSED: AtomicU64 = AtomicU64::new(0);
static ASIO_READS_PROCESSED: AtomicU64 = AtomicU64::new(0);
static ASIO_READS_THROTTLED: AtomicU64 = AtomicU64::new(0);
static ASIO_COMPLETIONS_POLLED: AtomicU64 = AtomicU64::new(0);

pub fn asio_write_queue_depth() -> u64 {
    ASIO_WRITES_SUBMITTED.load(Ordering::Relaxed)
        .saturating_sub(ASIO_WRITES_PROCESSED.load(Ordering::Relaxed))
}

pub fn asio_read_queue_depth() -> u64 {
    ASIO_READS_SUBMITTED.load(Ordering::Relaxed)
        .saturating_sub(ASIO_READS_PROCESSED.load(Ordering::Relaxed))
}

pub struct AsioMetrics {
    pub writes_submitted: u64,
    pub writes_processed: u64,
    pub reads_submitted: u64,
    pub reads_processed: u64,
    pub reads_throttled: u64,
    pub completions_polled: u64,
}

pub fn asio_get_metrics() -> AsioMetrics {
    AsioMetrics {
        writes_submitted: ASIO_WRITES_SUBMITTED.load(Ordering::Relaxed),
        writes_processed: ASIO_WRITES_PROCESSED.load(Ordering::Relaxed),
        reads_submitted: ASIO_READS_SUBMITTED.load(Ordering::Relaxed),
        reads_processed: ASIO_READS_PROCESSED.load(Ordering::Relaxed),
        reads_throttled: ASIO_READS_THROTTLED.load(Ordering::Relaxed),
        completions_polled: ASIO_COMPLETIONS_POLLED.load(Ordering::Relaxed),
    }
}

// ---------------------------------------------------------------------------
// RocksDBAsio
// ---------------------------------------------------------------------------

/// Self-contained ASIO layer for the RocksDB backend.
///
/// Spawns IO worker threads that process requests synchronously.
pub struct RocksDBAsio {
    read_queue: Arc<SegQueue<StorageRequest>>,
    write_queue: Arc<SegQueue<StorageRequest>>,
    completion_queue: Arc<SegQueue<StorageResponse>>,
    worker_handles: Vec<JoinHandle<()>>,
    shutdown_flag: Arc<AtomicBool>,
    max_in_flight_reads: u32,
    current_in_flight_reads: Arc<AtomicU32>,
    fatal_error: Arc<AtomicBool>,
    /// Shared DB handle for synchronous key_may_exist from main thread.
    pub db: Arc<DB>,
    pub num_databases: u32,
}

impl RocksDBAsio {
    /// Create a new RocksDB ASIO layer.
    ///
    /// `num_io_threads` controls how many worker threads process requests.
    /// Default: 4 threads for parallel IO.
    pub fn new(
        backend: RocksDBBackend,
        max_in_flight_reads: u32,
        callbacks: SerializationCallbacks,
        num_io_threads: usize,
    ) -> BackendResult<Self> {
        let read_queue = Arc::new(SegQueue::new());
        let write_queue = Arc::new(SegQueue::new());
        let completion_queue = Arc::new(SegQueue::new());
        let shutdown_flag = Arc::new(AtomicBool::new(false));
        let current_in_flight_reads = Arc::new(AtomicU32::new(0));
        let fatal_error = Arc::new(AtomicBool::new(false));

        let db = backend.db_handle();
        let num_databases = backend.num_databases();

        let num_threads = if num_io_threads == 0 { 4 } else { num_io_threads };

        let mut worker_handles = Vec::with_capacity(num_threads);
        for i in 0..num_threads {
            let rq = Arc::clone(&read_queue);
            let wq = Arc::clone(&write_queue);
            let cq = Arc::clone(&completion_queue);
            let sf = Arc::clone(&shutdown_flag);
            let cir = Arc::clone(&current_in_flight_reads);
            let db_clone = Arc::clone(&db);

            let handle = thread::Builder::new()
                .name(format!("ks-rocksdb-io-{}", i))
                .spawn(move || {
                    rocksdb_io_worker(db_clone, num_databases, rq, wq, cq, sf, cir, callbacks);
                })
                .map_err(|e| BackendError::FatalError(
                    format!("Failed to spawn RocksDB IO worker thread {i}: {e}"),
                ))?;
            worker_handles.push(handle);
        }

        Ok(Self {
            read_queue,
            write_queue,
            completion_queue,
            worker_handles,
            shutdown_flag,
            max_in_flight_reads,
            current_in_flight_reads,
            fatal_error,
            db,
            num_databases,
        })
    }

    pub fn submit_request(&self, request: StorageRequest) -> SubmitResult {
        match &request {
            StorageRequest::ReadValue { .. } => {
                let current = self.current_in_flight_reads.load(Ordering::Acquire);
                if current >= self.max_in_flight_reads {
                    ASIO_READS_THROTTLED.fetch_add(1, Ordering::Relaxed);
                    return SubmitResult::Throttled;
                }
                self.current_in_flight_reads.fetch_add(1, Ordering::AcqRel);
                ASIO_READS_SUBMITTED.fetch_add(1, Ordering::Relaxed);
                self.read_queue.push(request);
            }
            StorageRequest::WriteValue { db_id, key_robj, value_robj, request_context } => {
                ASIO_WRITES_SUBMITTED.fetch_add(1, Ordering::Relaxed);
                self.write_queue.push(request);
            }
            _ => {
                self.write_queue.push(request);
            }
        }
        SubmitResult::Ok
    }

    pub fn poll_single_completion(&self) -> Option<StorageResponse> {
        let resp = self.completion_queue.pop();
        if resp.is_some() {
            ASIO_COMPLETIONS_POLLED.fetch_add(1, Ordering::Relaxed);
        }
        resp
    }

    pub fn shutdown(&mut self) -> BackendResult<()> {
        self.shutdown_flag.store(true, Ordering::Release);
        for handle in self.worker_handles.drain(..) {
            handle.join().map_err(|_| BackendError::FatalError(
                "RocksDB IO worker thread panicked during shutdown".into(),
            ))?;
        }
        Ok(())
    }

    pub fn is_fatal_error(&self) -> bool {
        self.fatal_error.load(Ordering::Acquire)
    }

    /// Synchronous key existence check using RocksDB bloom filters.
    pub fn key_may_exist(&self, db_id: u32, key: &[u8]) -> bool {
        let cf = match self.db.cf_handle(&cf_name(db_id)) {
            Some(cf) => cf,
            None => return false,
        };
        self.db.key_may_exist_cf(&cf, key)
    }

    /// Get metrics string for INFO output.
    pub fn get_metrics_string(&self) -> String {
        let m = asio_get_metrics();
        format!(
            "asio_write_queue_depth:{}\r\nasio_read_queue_depth:{}\r\nasio_writes_submitted:{}\r\nasio_writes_processed:{}\r\nasio_reads_submitted:{}\r\nasio_reads_processed:{}\r\nasio_reads_throttled:{}\r\nasio_completions_polled:{}\r\n",
            m.writes_submitted.saturating_sub(m.writes_processed),
            m.reads_submitted.saturating_sub(m.reads_processed),
            m.writes_submitted, m.writes_processed,
            m.reads_submitted, m.reads_processed,
            m.reads_throttled, m.completions_polled,
        )
    }
}

// ---------------------------------------------------------------------------
// IO worker loop — synchronous processing
// ---------------------------------------------------------------------------

fn rocksdb_io_worker(
    db: Arc<DB>,
    num_databases: u32,
    read_queue: Arc<SegQueue<StorageRequest>>,
    write_queue: Arc<SegQueue<StorageRequest>>,
    completion_queue: Arc<SegQueue<StorageResponse>>,
    shutdown_flag: Arc<AtomicBool>,
    current_in_flight_reads: Arc<AtomicU32>,
    callbacks: SerializationCallbacks,
) {
    while !shutdown_flag.load(Ordering::Relaxed) {
        let mut did_work = false;

        // Process one read
        if let Some(req) = read_queue.pop() {
            let resp = unsafe {
                process_sync_request(&db, num_databases, req, &callbacks, &current_in_flight_reads)
            };
            completion_queue.push(resp);
            did_work = true;
        }

        // Process one write
        if let Some(req) = write_queue.pop() {
            let resp = unsafe {
                process_sync_request(&db, num_databases, req, &callbacks, &current_in_flight_reads)
            };
            completion_queue.push(resp);
            did_work = true;
        }

        if !did_work {
            thread::sleep(Duration::from_micros(50));
        }
    }

    // Drain remaining requests
    while let Some(req) = write_queue.pop() {
        let resp = unsafe {
            process_sync_request(&db, num_databases, req, &callbacks, &current_in_flight_reads)
        };
        completion_queue.push(resp);
    }
    while let Some(req) = read_queue.pop() {
        let resp = unsafe {
            process_sync_request(&db, num_databases, req, &callbacks, &current_in_flight_reads)
        };
        completion_queue.push(resp);
    }
}

unsafe fn process_sync_request(
    db: &Arc<DB>,
    num_databases: u32,
    request: StorageRequest,
    callbacks: &SerializationCallbacks,
    current_in_flight_reads: &AtomicU32,
) -> StorageResponse {
    match request {
        StorageRequest::WriteValue { db_id, key_robj, value_robj, request_context } => {
            let (key_buf, key_len) = match callbacks.serialize_key_to_buf(key_robj) {
                Ok(v) => v,
                Err(e) => return StorageResponse::WriteValue { db_id, key_robj, value_robj, request_context, result: Err(e), ram_bytes: 0 },
            };
            let (value_buf, value_len) = match callbacks.serialize_value_to_buf(value_robj) {
                Ok(v) => v,
                Err(e) => {
                    callbacks.free_serialized_key_buf(key_buf);
                    return StorageResponse::WriteValue { db_id, key_robj, value_robj, request_context, result: Err(e), ram_bytes: 0 };
                }
            };
            let key_bytes = std::slice::from_raw_parts(key_buf as *const u8, key_len);
            let value_bytes = std::slice::from_raw_parts(value_buf as *const u8, value_len);

            let cf = db.cf_handle(&cf_name(db_id));
            let result = match cf {
                Some(cf) => db.put_cf(&cf, key_bytes, value_bytes)
                    .map_err(|e| BackendError::OperationFailed(format!("put: {e}"))),
                None => Err(BackendError::OperationFailed(format!("cf db_{db_id} not found"))),
            };

            callbacks.free_serialized_key_buf(key_buf);
            callbacks.free_serialized_value_buf(value_buf);
            ASIO_WRITES_PROCESSED.fetch_add(1, Ordering::Relaxed);
            StorageResponse::WriteValue { db_id, key_robj, value_robj, request_context, result, ram_bytes: 0 }
        }

        StorageRequest::ReadValue { db_id, key_robj, request_context } => {
            let (key_buf, key_len) = match callbacks.serialize_key_to_buf(key_robj) {
                Ok(v) => v,
                Err(e) => {
                    current_in_flight_reads.fetch_sub(1, Ordering::AcqRel);
                    return StorageResponse::ReadValue {
                        db_id, key_robj, value_robj: ptr::null_mut(), request_context, result: Err(e),
                    };
                }
            };
            let key_bytes = std::slice::from_raw_parts(key_buf as *const u8, key_len);

            let cf = db.cf_handle(&cf_name(db_id));
            let (value_robj, result) = match cf {
                Some(cf) => {
                    match db.get_cf(&cf, key_bytes) {
                        Ok(Some(value_bytes)) => {
                            // Destructive read: delete after get
                            let mut batch = rocksdb::WriteBatch::default();
                            batch.delete_cf(&cf, key_bytes);
                            let _ = db.write(batch);

                            match callbacks.deserialize_value_from_buf(
                                value_bytes.as_ptr() as *const c_char, value_bytes.len(),
                            ) {
                                Ok(robj) => (robj, Ok(())),
                                Err(e) => (ptr::null_mut(), Err(e)),
                            }
                        }
                        Ok(None) => (ptr::null_mut(), Ok(())),
                        Err(e) => (ptr::null_mut(), Err(BackendError::OperationFailed(format!("get: {e}")))),
                    }
                }
                None => (ptr::null_mut(), Err(BackendError::OperationFailed(format!("cf db_{db_id} not found")))),
            };

            callbacks.free_serialized_key_buf(key_buf);
            current_in_flight_reads.fetch_sub(1, Ordering::AcqRel);
            ASIO_READS_PROCESSED.fetch_add(1, Ordering::Relaxed);
            StorageResponse::ReadValue { db_id, key_robj, value_robj, request_context, result }
        }

        StorageRequest::DeleteKey { db_id, key_robj } => {
            let result = match callbacks.serialize_key_to_buf(key_robj) {
                Ok((key_buf, key_len)) => {
                    let key_bytes = std::slice::from_raw_parts(key_buf as *const u8, key_len);
                    let cf = db.cf_handle(&cf_name(db_id));
                    let r = match cf {
                        Some(cf) => db.delete_cf(&cf, key_bytes)
                            .map_err(|e| BackendError::OperationFailed(format!("delete: {e}"))),
                        None => Err(BackendError::OperationFailed(format!("cf db_{db_id} not found"))),
                    };
                    callbacks.free_serialized_key_buf(key_buf);
                    r
                }
                Err(e) => Err(e),
            };
            StorageResponse::DeleteKey { db_id, result }
        }

        StorageRequest::FlushDB(db_id) => {
            let name = cf_name(db_id);
            let result = db.drop_cf(&name)
                .and_then(|_| db.create_cf(&name, &rocksdb::Options::default()))
                .map_err(|e| BackendError::OperationFailed(format!("flush_db: {e}")));
            StorageResponse::FlushDB(result)
        }

        StorageRequest::FlushAll => {
            let mut result = Ok(());
            for i in 0..num_databases {
                let name = cf_name(i);
                if let Err(e) = db.drop_cf(&name)
                    .and_then(|_| db.create_cf(&name, &rocksdb::Options::default()))
                {
                    result = Err(BackendError::OperationFailed(format!("flush_all db_{i}: {e}")));
                    break;
                }
            }
            StorageResponse::FlushAll(result)
        }

        StorageRequest::Shutdown => StorageResponse::Shutdown(Ok(())),

        // Byte-based variants (new storageType interface) — TODO: implement properly
        StorageRequest::ReadBytes { db_id, key, request_context } => {
            StorageResponse::ReadBytes { db_id, value: None, request_context }
        }
        StorageRequest::WriteBytes { db_id, request_context, .. } => {
            StorageResponse::WriteBytes { db_id, request_context, result: Ok(()) }
        }
        StorageRequest::DeleteBytes { db_id, request_context, .. } => {
            StorageResponse::DeleteBytes { db_id, request_context, result: Ok(()) }
        }
    }
}
