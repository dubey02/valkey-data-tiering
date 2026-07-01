//! FlashCache storage backend implementation.
//!
//! Reads are inherently destructive and asynchronous.
//! The ASIO layer uses `fc_submit_get` + callback instead of `get_item`.

use std::sync::atomic::{AtomicBool, Ordering};

use crate::common::backend_trait::DataTieringBackend;
use crate::common::types::{BackendConfig, BackendError, BackendResult, GetItemResult};
use super::ffi;

const DEFAULT_INITIAL_INDEX_SIZE: usize = 1048576;

pub struct FlashCacheBackend {
    initialized: AtomicBool,
    num_databases: u32,
}

unsafe impl Send for FlashCacheBackend {}
unsafe impl Sync for FlashCacheBackend {}

impl DataTieringBackend for FlashCacheBackend {
    fn init(config: BackendConfig) -> BackendResult<Self>
    where
        Self: Sized,
    {
        let initial_index_size: usize = config
            .options
            .get("initial_index_size")
            .and_then(|v| v.parse().ok())
            .unwrap_or(DEFAULT_INITIAL_INDEX_SIZE);

        let fc_max_in_flight = std::cmp::max(config.max_in_flight_reads, 126);

        ffi::fc_init(
            &config.db_path,
            config.db_size_bytes as usize,
            initial_index_size,
            config.num_databases,
            fc_max_in_flight,
            config.eviction_enabled,
        )
        .map_err(|e| BackendError::InitFailed(e))?;

        let max_buffer_bytes: i64 = config
            .options
            .get("max_buffered_write_bytes")
            .and_then(|v| v.parse().ok())
            .unwrap_or(4 * 1024 * 1024);
        ffi::fc_set_config(ffi::FC_CONFIG_MAX_BUFFERED_WRITE_SIZE_BYTES, max_buffer_bytes);
        ffi::fc_set_config(
            ffi::FC_CONFIG_BUFFERED_WRITE_FLUSH_THRESHOLD_BYTES,
            max_buffer_bytes,
        );

        Ok(FlashCacheBackend {
            initialized: AtomicBool::new(true),
            num_databases: config.num_databases,
        })
    }

    fn put_item(&self, db_id: u32, key: &[u8], value: &[u8]) -> BackendResult<()> {
        ffi::fc_put(db_id, key, value).map_err(|e| {
            if e.contains("throttled") {
                BackendError::Throttled
            } else {
                BackendError::OperationFailed(e)
            }
        })
    }

    fn get_item(&self, _db_id: u32, _key: &[u8]) -> BackendResult<GetItemResult> {
        Err(BackendError::OperationFailed(
            "FlashCache get_item should not be called directly; use async path".into(),
        ))
    }

    fn delete_key(&self, _db_id: u32, _key: &[u8]) -> BackendResult<()> {
        Ok(())
    }

    fn flush_db(&self, db_id: u32) -> BackendResult<()> {
        ffi::fc_flush_db(db_id).map_err(|e| BackendError::OperationFailed(e))
    }

    fn flush_all(&self) -> BackendResult<()> {
        ffi::fc_flush_all().map_err(|e| BackendError::OperationFailed(e))
    }

    fn shutdown(&mut self) -> BackendResult<()> {
        if self
            .initialized
            .compare_exchange(true, false, Ordering::AcqRel, Ordering::Acquire)
            .is_ok()
        {
            ffi::fc_teardown().map_err(|e| BackendError::OperationFailed(e))?;
        }
        Ok(())
    }

    fn key_may_exist(&self, db_id: u32, key: &[u8]) -> bool {
        ffi::fc_key_exists(db_id, key)
    }

    fn tick(&self) {
        ffi::fc_run_cron_tasks();
    }

    fn is_async_read(&self) -> bool {
        true
    }
}
