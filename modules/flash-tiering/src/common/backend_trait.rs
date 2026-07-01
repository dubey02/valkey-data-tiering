//! Common storage backend trait for the key spilling module.
//!
//! Both FlashCache and RocksDB backends implement [`DataTieringBackend`],
//! allowing the ASIO layer to be generic over the storage engine.

use crate::common::types::{BackendConfig, BackendResult, GetItemResult};

/// Common interface that both FlashCache and RocksDB backends implement.
///
/// The trait is `Send + Sync + 'static` so that the backend can be moved
/// into the IO worker thread and shared safely.
pub trait DataTieringBackend: Send + Sync + 'static {
    /// Initialize the backend with the given configuration.
    fn init(config: BackendConfig) -> BackendResult<Self>
    where
        Self: Sized;

    /// Store a key-value pair for the given database.
    fn put_item(&self, db_id: u32, key: &[u8], value: &[u8]) -> BackendResult<()>;

    /// Retrieve a value by key (destructive for single-location invariant).
    fn get_item(&self, db_id: u32, key: &[u8]) -> BackendResult<GetItemResult>;

    /// Delete a key explicitly (DEL, expiry).
    fn delete_key(&self, db_id: u32, key: &[u8]) -> BackendResult<()>;

    /// Flush all keys for a specific database.
    fn flush_db(&self, db_id: u32) -> BackendResult<()>;

    /// Flush all keys across all databases.
    fn flush_all(&self) -> BackendResult<()>;

    /// Shut down the backend, releasing all resources.
    fn shutdown(&mut self) -> BackendResult<()>;

    /// Check if a key may exist in storage.
    fn key_may_exist(&self, _db_id: u32, _key: &[u8]) -> bool {
        true
    }

    /// Periodic tick called from the IO worker loop.
    fn tick(&self) {}

    /// Returns true if this backend uses async reads (FlashCache).
    fn is_async_read(&self) -> bool {
        false
    }
}
