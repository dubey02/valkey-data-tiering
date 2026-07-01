//! Shared types for the key spilling storage module.
//!
//! Defines request/response enums, error types, configuration structs,
//! and result types used across the ASIO layer, callbacks, and backends.

use std::collections::HashMap;
use std::ffi::c_void;

// ---------------------------------------------------------------------------
// Error types
// ---------------------------------------------------------------------------

/// Unified error type for all backend operations.
#[derive(Debug, Clone, thiserror::Error)]
pub enum BackendError {
    #[error("Initialization failed: {0}")]
    InitFailed(String),

    #[error("Operation failed: {0}")]
    OperationFailed(String),

    #[error("Request throttled")]
    Throttled,

    #[error("Serialization error: {0}")]
    SerializationError(String),

    #[error("Fatal error: {0}")]
    FatalError(String),
}

/// Convenience alias used throughout the crate.
pub type BackendResult<T> = Result<T, BackendError>;

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

/// Configuration passed during backend initialization.
#[derive(Debug, Clone)]
pub struct BackendConfig {
    /// Absolute path to the storage file or directory.
    pub db_path: String,
    /// Maximum size of the storage in bytes.
    pub db_size_bytes: u64,
    /// Number of Valkey logical databases.
    pub num_databases: u32,
    /// Max concurrent read requests before throttling.
    pub max_in_flight_reads: u32,
    /// Whether FlashCache GC can evict data (false for noeviction policy).
    pub eviction_enabled: bool,
    /// Backend-specific options stored as key-value pairs.
    pub options: HashMap<String, String>,
}

// ---------------------------------------------------------------------------
// Get result
// ---------------------------------------------------------------------------

/// Value returned from a get operation.
#[derive(Debug, Clone)]
pub struct GetItemResult {
    /// The retrieved value, or `None` if the key was not found.
    pub value: Option<Vec<u8>>,
}

// ---------------------------------------------------------------------------
// Submit result (throttling)
// ---------------------------------------------------------------------------

/// Result of submitting a request to the ASIO layer.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum SubmitResult {
    Ok,
    Throttled,
}

// ---------------------------------------------------------------------------
// Storage requests
// ---------------------------------------------------------------------------

/// Requests submitted from the Valkey main thread to the IO worker.
///
/// Read and Write variants carry raw `*mut c_void` pointers to Valkey robj
/// structures. These are opaque to the module — serialization happens on the
/// IO worker thread via SerializationCallbacks.
///
/// The `*Bytes` variants carry pre-serialized data from the new storageType
/// interface — no serialization needed, store/retrieve as-is.
pub enum StorageRequest {
    ReadValue {
        db_id: u32,
        key_robj: *mut c_void,
        request_context: u64,
    },
    WriteValue {
        db_id: u32,
        key_robj: *mut c_void,
        value_robj: *mut c_void,
        request_context: u64,
    },
    DeleteKey {
        db_id: u32,
        key_robj: *mut c_void,
    },
    // New: byte-based variants for storageType interface
    ReadBytes {
        db_id: u32,
        key: Vec<u8>,
        request_context: u64,
    },
    WriteBytes {
        db_id: u32,
        key: Vec<u8>,
        value: Vec<u8>,
        request_context: u64,
    },
    DeleteBytes {
        db_id: u32,
        key: Vec<u8>,
        request_context: u64,
    },
    FlushDB(u32),
    FlushAll,
    Shutdown,
}

// SAFETY: robj* pointers are reference-counted and their refcount is
// incremented before crossing thread boundaries. The IO worker thread
// is the sole consumer of these pointers until completion.
unsafe impl Send for StorageRequest {}
unsafe impl Sync for StorageRequest {}

// ---------------------------------------------------------------------------
// Storage responses
// ---------------------------------------------------------------------------

/// Responses returned from the IO worker to the Valkey main thread.
pub enum StorageResponse {
    ReadValue {
        db_id: u32,
        key_robj: *mut c_void,
        /// Deserialized value robj*, or null if key not found.
        value_robj: *mut c_void,
        request_context: u64,
        result: BackendResult<()>,
    },
    WriteValue {
        db_id: u32,
        key_robj: *mut c_void,
        /// The value robj pointer — returned so the C side can call decrRefCount.
        value_robj: *mut c_void,
        request_context: u64,
        result: BackendResult<()>,
        ram_bytes: usize,
    },
    DeleteKey {
        db_id: u32,
        result: BackendResult<()>,
    },
    // New: byte-based responses for storageType interface
    ReadBytes {
        db_id: u32,
        value: Option<Vec<u8>>,
        request_context: u64,
    },
    WriteBytes {
        db_id: u32,
        request_context: u64,
        result: BackendResult<()>,
    },
    DeleteBytes {
        db_id: u32,
        request_context: u64,
        result: BackendResult<()>,
    },
    /// FlashCache GC evicted a key — Valkey must remove it from the dict.
    EvictKey {
        db_id: u32,
        key_bytes: Vec<u8>,
    },
    FlushDB(BackendResult<()>),
    FlushAll(BackendResult<()>),
    Shutdown(BackendResult<()>),
}

// SAFETY: Same justification as StorageRequest — robj* pointers have
// incremented refcounts and are consumed exactly once on the main thread.
unsafe impl Send for StorageResponse {}
unsafe impl Sync for StorageResponse {}
