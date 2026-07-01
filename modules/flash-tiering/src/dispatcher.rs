//! Backend dispatcher — routes storage requests to the configured backend.
//!
//! The backend is selected at module init time and remains fixed for the
//! module's lifetime.

use crate::common::types::{BackendResult, StorageRequest, StorageResponse, SubmitResult};

#[cfg(feature = "backend-rocksdb")]
use crate::backends::rocksdb::RocksDBAsio;

#[cfg(feature = "backend-flashcache")]
use crate::backends::flashcache::FlashCacheAsio;

/// Runtime dispatch enum that routes requests to the configured storage backend.
pub enum BackendDispatcher {
    #[cfg(feature = "backend-rocksdb")]
    RocksDB(RocksDBAsio),

    #[cfg(feature = "backend-flashcache")]
    FlashCache(FlashCacheAsio),
}

impl BackendDispatcher {
    pub fn submit_request(&self, request: StorageRequest) -> SubmitResult {
        match self {
            #[cfg(feature = "backend-rocksdb")]
            BackendDispatcher::RocksDB(ref asio) => asio.submit_request(request),

            #[cfg(feature = "backend-flashcache")]
            BackendDispatcher::FlashCache(ref asio) => asio.submit_request(request),
        }
    }

    pub fn poll_single_completion(&self) -> Option<StorageResponse> {
        match self {
            #[cfg(feature = "backend-rocksdb")]
            BackendDispatcher::RocksDB(ref asio) => asio.poll_single_completion(),

            #[cfg(feature = "backend-flashcache")]
            BackendDispatcher::FlashCache(ref asio) => asio.poll_single_completion(),
        }
    }

    pub fn shutdown(&mut self) -> BackendResult<()> {
        match self {
            #[cfg(feature = "backend-rocksdb")]
            BackendDispatcher::RocksDB(ref mut asio) => asio.shutdown(),

            #[cfg(feature = "backend-flashcache")]
            BackendDispatcher::FlashCache(ref mut asio) => asio.shutdown(),
        }
    }

    pub fn is_fatal_error(&self) -> bool {
        match self {
            #[cfg(feature = "backend-rocksdb")]
            BackendDispatcher::RocksDB(ref asio) => asio.is_fatal_error(),

            #[cfg(feature = "backend-flashcache")]
            BackendDispatcher::FlashCache(ref asio) => asio.is_fatal_error(),
        }
    }

    /// Synchronous key existence check using backend bloom filters.
    pub fn key_may_exist(&self, db_id: u32, key: &[u8]) -> bool {
        match self {
            #[cfg(feature = "backend-rocksdb")]
            BackendDispatcher::RocksDB(ref asio) => asio.key_may_exist(db_id, key),

            #[cfg(feature = "backend-flashcache")]
            BackendDispatcher::FlashCache(ref asio) => asio.key_may_exist(db_id, key),
        }
    }

    /// Get metrics string for INFO output.
    pub fn get_metrics_string(&self) -> String {
        match self {
            #[cfg(feature = "backend-rocksdb")]
            BackendDispatcher::RocksDB(ref asio) => asio.get_metrics_string(),

            #[cfg(feature = "backend-flashcache")]
            BackendDispatcher::FlashCache(ref asio) => asio.get_metrics_string(),
        }
    }
}
