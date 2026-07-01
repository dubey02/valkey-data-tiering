//! RocksDB ASIO — multi-threaded IO worker pool.
//!
//! Spawns N worker threads that process put/get/del requests synchronously
//! against RocksDB. Requests arrive via crossbeam SegQueue, completions are
//! returned via a separate SegQueue polled from the main thread.

use std::collections::HashMap;
use std::ffi::c_void;
use std::os::raw::c_char;
use std::ptr;
use std::sync::atomic::{AtomicBool, AtomicU32, AtomicU64, Ordering};
use std::sync::Arc;
use std::thread::{self, JoinHandle};
use std::time::Duration;

use crossbeam_queue::SegQueue;
use rocksdb::{BlockBasedOptions, ColumnFamilyDescriptor, Options, WriteBatch, WriteOptions, DB};

use crate::SerializationCallbacks;

// ---------------------------------------------------------------------------
// Request / Response types
// ---------------------------------------------------------------------------

pub enum Request {
    Put { db_id: u32, key_robj: *mut c_void, value_robj: *mut c_void, ctx: u64 },
    Get { db_id: u32, key_robj: *mut c_void, ctx: u64 },
    Del { db_id: u32, key_robj: *mut c_void, ctx: u64 },
}

unsafe impl Send for Request {}

pub enum Response {
    Put { db_id: u32, value_robj: *mut c_void, ctx: u64, ok: bool },
    Get { db_id: u32, value_robj: *mut c_void, ctx: u64, ok: bool },
    Del { db_id: u32, ctx: u64, ok: bool },
}

unsafe impl Send for Response {}

// ---------------------------------------------------------------------------
// Metrics
// ---------------------------------------------------------------------------

static WRITES_SUBMITTED: AtomicU64 = AtomicU64::new(0);
static WRITES_PROCESSED: AtomicU64 = AtomicU64::new(0);
static READS_SUBMITTED: AtomicU64 = AtomicU64::new(0);
static READS_PROCESSED: AtomicU64 = AtomicU64::new(0);
static READS_THROTTLED: AtomicU64 = AtomicU64::new(0);

pub fn metrics_string() -> String {
    format!(
        "rocksdb_writes_submitted:{}\r\nrocksdb_writes_processed:{}\r\nrocksdb_reads_submitted:{}\r\nrocksdb_reads_processed:{}\r\nrocksdb_reads_throttled:{}\r\n",
        WRITES_SUBMITTED.load(Ordering::Relaxed),
        WRITES_PROCESSED.load(Ordering::Relaxed),
        READS_SUBMITTED.load(Ordering::Relaxed),
        READS_PROCESSED.load(Ordering::Relaxed),
        READS_THROTTLED.load(Ordering::Relaxed),
    )
}

// ---------------------------------------------------------------------------
// RocksDBAsio
// ---------------------------------------------------------------------------

pub struct RocksDBAsio {
    read_queue: Arc<SegQueue<Request>>,
    write_queue: Arc<SegQueue<Request>>,
    pub completion_queue: Arc<SegQueue<Response>>,
    workers: Vec<JoinHandle<()>>,
    shutdown: Arc<AtomicBool>,
    max_in_flight_reads: u32,
    in_flight_reads: Arc<AtomicU32>,
}

impl RocksDBAsio {
    pub fn new(
        db_path: &str,
        num_databases: u32,
        max_in_flight_reads: u32,
        num_io_threads: usize,
        opts: &HashMap<String, String>,
        callbacks: SerializationCallbacks,
    ) -> Result<Self, String> {
        let db = open_rocksdb(db_path, num_databases, opts)?;
        let db = Arc::new(db);

        let read_queue = Arc::new(SegQueue::new());
        let write_queue = Arc::new(SegQueue::new());
        let completion_queue = Arc::new(SegQueue::new());
        let shutdown = Arc::new(AtomicBool::new(false));
        let in_flight_reads = Arc::new(AtomicU32::new(0));

        let mut workers = Vec::with_capacity(num_io_threads);
        for i in 0..num_io_threads {
            let rq = Arc::clone(&read_queue);
            let wq = Arc::clone(&write_queue);
            let cq = Arc::clone(&completion_queue);
            let sf = Arc::clone(&shutdown);
            let ifr = Arc::clone(&in_flight_reads);
            let db_clone = Arc::clone(&db);

            let is_writer = i == 0; // Thread 0 = dedicated writer, rest = readers
            let handle = thread::Builder::new()
                .name(if is_writer { "rdb-io-wr".into() } else { format!("rdb-io-rd-{}", i - 1) })
                .spawn(move || {
                    if is_writer {
                        write_worker(db_clone, wq, cq, sf, callbacks);
                    } else {
                        read_worker(db_clone, rq, cq, sf, ifr, callbacks);
                    }
                })
                .map_err(|e| format!("spawn IO thread {i}: {e}"))?;
            workers.push(handle);
        }

        Ok(Self { read_queue, write_queue, completion_queue, workers, shutdown, max_in_flight_reads, in_flight_reads })
    }

    /// Submit a PUT request. Returns true if accepted.
    pub fn submit_put(&self, db_id: u32, key: *mut c_void, value: *mut c_void, ctx: u64) -> bool {
        WRITES_SUBMITTED.fetch_add(1, Ordering::Relaxed);
        self.write_queue.push(Request::Put { db_id, key_robj: key, value_robj: value, ctx });
        true
    }

    /// Submit a GET request. Returns false if throttled.
    pub fn submit_get(&self, db_id: u32, key: *mut c_void, ctx: u64) -> bool {
        let current = self.in_flight_reads.load(Ordering::Acquire);
        if current >= self.max_in_flight_reads {
            READS_THROTTLED.fetch_add(1, Ordering::Relaxed);
            return false;
        }
        self.in_flight_reads.fetch_add(1, Ordering::AcqRel);
        READS_SUBMITTED.fetch_add(1, Ordering::Relaxed);
        self.read_queue.push(Request::Get { db_id, key_robj: key, ctx });
        true
    }

    /// Submit a DEL request.
    pub fn submit_del(&self, db_id: u32, key: *mut c_void, ctx: u64) -> bool {
        self.write_queue.push(Request::Del { db_id, key_robj: key, ctx });
        true
    }

    /// Poll one completion. Called from main thread.
    pub fn poll(&self) -> Option<Response> {
        self.completion_queue.pop()
    }

    pub fn shutdown(&mut self) -> Result<(), String> {
        self.shutdown.store(true, Ordering::Release);
        for h in self.workers.drain(..) {
            h.join().map_err(|_| "IO thread panicked".to_string())?;
        }
        Ok(())
    }
}

// ---------------------------------------------------------------------------
// Dedicated write worker (single thread — preserves write ordering)
// ---------------------------------------------------------------------------

fn write_worker(
    db: Arc<DB>,
    wq: Arc<SegQueue<Request>>,
    cq: Arc<SegQueue<Response>>,
    shutdown: Arc<AtomicBool>,
    cb: SerializationCallbacks,
) {
    let dummy = Arc::new(AtomicU32::new(0));
    while !shutdown.load(Ordering::Relaxed) {
        if let Some(req) = wq.pop() {
            let resp = unsafe { process_request(&db, req, &cb, &dummy) };
            cq.push(resp);
        } else {
            thread::sleep(Duration::from_micros(50));
        }
    }
    // Drain remaining writes
    while let Some(req) = wq.pop() {
        let resp = unsafe { process_request(&db, req, &cb, &dummy) };
        cq.push(resp);
    }
}

// ---------------------------------------------------------------------------
// Read workers (N-1 threads — parallelized, order doesn't matter)
// ---------------------------------------------------------------------------

fn read_worker(
    db: Arc<DB>,
    rq: Arc<SegQueue<Request>>,
    cq: Arc<SegQueue<Response>>,
    shutdown: Arc<AtomicBool>,
    in_flight_reads: Arc<AtomicU32>,
    cb: SerializationCallbacks,
) {
    while !shutdown.load(Ordering::Relaxed) {
        if let Some(req) = rq.pop() {
            let resp = unsafe { process_request(&db, req, &cb, &in_flight_reads) };
            cq.push(resp);
        } else {
            thread::sleep(Duration::from_micros(50));
        }
    }
    // Drain remaining reads
    while let Some(req) = rq.pop() {
        let resp = unsafe { process_request(&db, req, &cb, &in_flight_reads) };
        cq.push(resp);
    }
}

unsafe fn process_request(
    db: &DB,
    req: Request,
    cb: &SerializationCallbacks,
    in_flight_reads: &AtomicU32,
) -> Response {
    match req {
        Request::Put { db_id, key_robj, value_robj, ctx } => {
            let ok = (|| -> bool {
                let mut key_buf: *mut c_char = ptr::null_mut();
                let klen = (cb.serialize_key)(key_robj, &mut key_buf);
                if klen < 0 || key_buf.is_null() { return false; }

                let mut val_buf: *mut c_char = ptr::null_mut();
                let vlen = (cb.serialize_value)(value_robj, &mut val_buf);
                if vlen < 0 || val_buf.is_null() {
                    (cb.free_key)(key_buf as *mut c_void);
                    return false;
                }

                let key_bytes = std::slice::from_raw_parts(key_buf as *const u8, klen as usize);
                let val_bytes = std::slice::from_raw_parts(val_buf as *const u8, vlen as usize);

                let cf_name = format!("db_{}", db_id);
                let result = match db.cf_handle(&cf_name) {
                    Some(cf) => db.put_cf(&cf, key_bytes, val_bytes).is_ok(),
                    None => false,
                };

                (cb.free_key)(key_buf as *mut c_void);
                (cb.free_value)(val_buf as *mut c_void);
                result
            })();

            WRITES_PROCESSED.fetch_add(1, Ordering::Relaxed);
            Response::Put { db_id, value_robj, ctx, ok }
        }

        Request::Get { db_id, key_robj, ctx } => {
            let value_robj = (|| -> *mut c_void {
                let mut key_buf: *mut c_char = ptr::null_mut();
                let klen = (cb.serialize_key)(key_robj, &mut key_buf);
                if klen < 0 || key_buf.is_null() { return ptr::null_mut(); }

                let key_bytes = std::slice::from_raw_parts(key_buf as *const u8, klen as usize);
                let cf_name = format!("db_{}", db_id);

                let result = match db.cf_handle(&cf_name) {
                    Some(cf) => {
                        match db.get_cf(&cf, key_bytes) {
                            Ok(Some(val_bytes)) => {
                                // Destructive read
                                let mut batch = WriteBatch::default();
                                batch.delete_cf(&cf, key_bytes);
                                let _ = db.write(batch);

                                (cb.deserialize_value)(
                                    val_bytes.as_ptr() as *mut c_char,
                                    val_bytes.len() as i32,
                                )
                            }
                            _ => ptr::null_mut(),
                        }
                    }
                    None => ptr::null_mut(),
                };

                (cb.free_key)(key_buf as *mut c_void);
                result
            })();

            in_flight_reads.fetch_sub(1, Ordering::AcqRel);
            READS_PROCESSED.fetch_add(1, Ordering::Relaxed);
            Response::Get { db_id, value_robj, ctx, ok: !value_robj.is_null() }
        }

        Request::Del { db_id, key_robj, ctx } => {
            let ok = (|| -> bool {
                let mut key_buf: *mut c_char = ptr::null_mut();
                let klen = (cb.serialize_key)(key_robj, &mut key_buf);
                eprintln!("[rdb-tiering] DEL: serialize_key returned klen={}", klen);
                if klen < 0 || key_buf.is_null() { return false; }

                let key_bytes = std::slice::from_raw_parts(key_buf as *const u8, klen as usize);
                let cf_name = format!("db_{}", db_id);

                let result = match db.cf_handle(&cf_name) {
                    Some(cf) => {
                        let r = db.delete_cf(&cf, key_bytes).is_ok();
                        eprintln!("[rdb-tiering] DEL: delete_cf result={}", r);
                        r
                    }
                    None => { eprintln!("[rdb-tiering] DEL: cf not found"); false }
                };

                (cb.free_key)(key_buf as *mut c_void);
                result
            })();

            eprintln!("[rdb-tiering] DEL: pushing response ok={}", ok);
            Response::Del { db_id, ctx, ok }
        }
    }
}

// ---------------------------------------------------------------------------
// RocksDB initialization
// ---------------------------------------------------------------------------

fn open_rocksdb(path: &str, num_databases: u32, opts: &HashMap<String, String>) -> Result<DB, String> {
    let compression = match opts.get("compression_type").map(|s| s.as_str()) {
        Some("snappy") => rocksdb::DBCompressionType::Snappy,
        Some("lz4") => rocksdb::DBCompressionType::Lz4,
        Some("zstd") => rocksdb::DBCompressionType::Zstd,
        _ => rocksdb::DBCompressionType::None,
    };

    let block_cache_size: usize = opts.get("block_cache_size")
        .and_then(|v| v.parse().ok()).unwrap_or(8 * 1024 * 1024);
    let bloom_bits: i32 = opts.get("bloom_filter_bits_per_key")
        .and_then(|v| v.parse().ok()).unwrap_or(10);
    let disable_wal = opts.get("disable_wal").map(|v| v == "1" || v == "true").unwrap_or(false);
    let direct_io = opts.get("direct_io").map(|v| v == "1" || v == "true").unwrap_or(false);

    let mut db_opts = Options::default();
    db_opts.create_if_missing(true);
    db_opts.create_missing_column_families(true);
    db_opts.set_write_buffer_size(64 * 1024 * 1024);
    db_opts.set_max_write_buffer_number(3);
    db_opts.set_max_background_jobs(2);
    db_opts.set_compression_type(compression);
    if direct_io {
        db_opts.set_use_direct_reads(true);
        db_opts.set_use_direct_io_for_flush_and_compaction(true);
    }

    let mut block_opts = BlockBasedOptions::default();
    block_opts.set_bloom_filter(bloom_bits as f64, false);
    if block_cache_size > 0 {
        let cache = rocksdb::Cache::new_lru_cache(block_cache_size);
        block_opts.set_block_cache(&cache);
    }

    let mut cf_opts = Options::default();
    cf_opts.set_block_based_table_factory(&block_opts);

    let mut cf_descriptors = vec![ColumnFamilyDescriptor::new("default", Options::default())];
    for i in 0..num_databases {
        cf_descriptors.push(ColumnFamilyDescriptor::new(format!("db_{}", i), cf_opts.clone()));
    }

    let db = DB::open_cf_descriptors(&db_opts, path, cf_descriptors)
        .map_err(|e| format!("RocksDB open at {path}: {e}"))?;

    // Apply WAL setting
    if disable_wal {
        // WriteOptions are per-operation, handled in process_request if needed
        // For now default WAL is on; disable_wal would need WriteOptions passed per-op
    }

    Ok(db)
}
