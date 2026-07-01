//! RocksDB storage backend implementation.
//!
//! Each Valkey logical database maps to a separate RocksDB column family.
//! Reads are destructive — `get_item` performs an atomic get+delete via
//! `WriteBatch` to enforce the single-location invariant.

use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Arc;

use rocksdb::{BlockBasedOptions, ColumnFamilyDescriptor, Options, WriteBatch, WriteOptions, DB};

use crate::common::backend_trait::DataTieringBackend;
use crate::common::types::{BackendConfig, BackendError, BackendResult, GetItemResult};

// ---------------------------------------------------------------------------
// RocksDB-specific configuration
// ---------------------------------------------------------------------------

#[derive(Debug, Clone)]
struct RocksDBConfig {
    write_buffer_size: usize,
    max_write_buffer_number: i32,
    target_file_size_base: u64,
    max_bytes_for_level_base: u64,
    compression_type: rocksdb::DBCompressionType,
    block_cache_size: usize,
    max_background_jobs: i32,
    level0_file_num_compaction_trigger: i32,
    bloom_filter_bits_per_key: i32,
    disable_wal: bool,
    use_direct_io: bool,
}

impl RocksDBConfig {
    fn from_options(options: &std::collections::HashMap<String, String>) -> Self {
        let compression = match options.get("compression_type").map(|s| s.as_str()) {
            Some("snappy") => rocksdb::DBCompressionType::Snappy,
            Some("lz4") => rocksdb::DBCompressionType::Lz4,
            Some("zstd") => rocksdb::DBCompressionType::Zstd,
            Some("none") | None => rocksdb::DBCompressionType::None,
            Some(_) => rocksdb::DBCompressionType::None,
        };

        Self {
            write_buffer_size: options.get("write_buffer_size")
                .and_then(|v| v.parse().ok()).unwrap_or(64 * 1024 * 1024), // 64 MB default
            max_write_buffer_number: options.get("max_write_buffer_number")
                .and_then(|v| v.parse().ok()).unwrap_or(3),
            target_file_size_base: options.get("target_file_size_base")
                .and_then(|v| v.parse().ok()).unwrap_or(64 * 1024 * 1024),
            max_bytes_for_level_base: options.get("max_bytes_for_level_base")
                .and_then(|v| v.parse().ok()).unwrap_or(256 * 1024 * 1024),
            compression_type: compression,
            block_cache_size: options.get("block_cache_size")
                .and_then(|v| v.parse().ok()).unwrap_or(8 * 1024 * 1024), // 8 MB default
            max_background_jobs: options.get("max_background_jobs")
                .and_then(|v| v.parse().ok()).unwrap_or(2),
            level0_file_num_compaction_trigger: options.get("level0_file_num_compaction_trigger")
                .and_then(|v| v.parse().ok()).unwrap_or(4),
            bloom_filter_bits_per_key: options.get("bloom_filter_bits_per_key")
                .and_then(|v| v.parse().ok()).unwrap_or(10),
            disable_wal: options.get("disable_wal")
                .map(|v| v == "1" || v == "true").unwrap_or(false), // WAL enabled by default
            use_direct_io: options.get("direct_io")
                .map(|v| v == "1" || v == "true").unwrap_or(false), // Direct IO off by default
        }
    }

    fn to_db_options(&self) -> Options {
        let mut opts = Options::default();
        opts.create_if_missing(true);
        opts.create_missing_column_families(true);
        opts.set_write_buffer_size(self.write_buffer_size);
        opts.set_max_write_buffer_number(self.max_write_buffer_number);
        opts.set_target_file_size_base(self.target_file_size_base);
        opts.set_max_bytes_for_level_base(self.max_bytes_for_level_base);
        opts.set_compression_type(self.compression_type);
        opts.set_max_background_jobs(self.max_background_jobs);
        opts.set_level_zero_file_num_compaction_trigger(self.level0_file_num_compaction_trigger);
        if self.use_direct_io {
            opts.set_use_direct_reads(true);
            opts.set_use_direct_io_for_flush_and_compaction(true);
        }
        opts
    }

    fn to_cf_options(&self) -> Options {
        let mut cf_opts = Options::default();
        let mut block_opts = BlockBasedOptions::default();
        block_opts.set_bloom_filter(self.bloom_filter_bits_per_key as f64, false);
        if self.block_cache_size > 0 {
            let cache = rocksdb::Cache::new_lru_cache(self.block_cache_size);
            block_opts.set_block_cache(&cache);
        }
        cf_opts.set_block_based_table_factory(&block_opts);
        cf_opts
    }
}

/// Column family name for a given database ID.
pub fn cf_name(db_id: u32) -> String {
    format!("db_{}", db_id)
}

// ---------------------------------------------------------------------------
// RocksDBBackend
// ---------------------------------------------------------------------------

pub struct RocksDBBackend {
    db: Arc<DB>,
    num_databases: u32,
    initialized: AtomicBool,
    write_opts: WriteOptions,
}

impl RocksDBBackend {
    /// Return a clone of the `Arc<DB>` handle for synchronous access.
    pub fn db_handle(&self) -> Arc<DB> {
        Arc::clone(&self.db)
    }

    /// Return the number of databases configured.
    pub fn num_databases(&self) -> u32 {
        self.num_databases
    }
}

impl DataTieringBackend for RocksDBBackend {
    fn init(config: BackendConfig) -> BackendResult<Self> {
        let rocksdb_config = RocksDBConfig::from_options(&config.options);
        let db_opts = rocksdb_config.to_db_options();
        let cf_opts = rocksdb_config.to_cf_options();

        let mut cf_descriptors =
            vec![ColumnFamilyDescriptor::new("default", Options::default())];
        for i in 0..config.num_databases {
            cf_descriptors.push(ColumnFamilyDescriptor::new(cf_name(i), cf_opts.clone()));
        }

        let db = DB::open_cf_descriptors(&db_opts, &config.db_path, cf_descriptors)
            .map_err(|e| BackendError::InitFailed(format!(
                "Failed to open RocksDB at {}: {e}", config.db_path
            )))?;

        let mut write_opts = WriteOptions::default();
        if rocksdb_config.disable_wal {
            write_opts.disable_wal(true);
        }

        Ok(Self {
            db: Arc::new(db),
            num_databases: config.num_databases,
            initialized: AtomicBool::new(true),
            write_opts,
        })
    }

    fn put_item(&self, db_id: u32, key: &[u8], value: &[u8]) -> BackendResult<()> {
        let cf = self.db.cf_handle(&cf_name(db_id)).ok_or_else(|| {
            BackendError::OperationFailed(format!("put_item: cf for db_{db_id} not found"))
        })?;
        self.db.put_cf_opt(&cf, key, value, &self.write_opts).map_err(|e| {
            BackendError::OperationFailed(format!("put_item: {e}"))
        })
    }

    fn get_item(&self, db_id: u32, key: &[u8]) -> BackendResult<GetItemResult> {
        let cf = self.db.cf_handle(&cf_name(db_id)).ok_or_else(|| {
            BackendError::OperationFailed(format!("get_item: cf for db_{db_id} not found"))
        })?;

        let value = self.db.get_cf(&cf, key).map_err(|e| {
            BackendError::OperationFailed(format!("get_item: {e}"))
        })?;

        if value.is_some() {
            let mut batch = WriteBatch::default();
            batch.delete_cf(&cf, key);
            self.db.write_opt(batch, &self.write_opts).map_err(|e| {
                BackendError::OperationFailed(format!("get_item: delete after read failed: {e}"))
            })?;
        }

        Ok(GetItemResult { value })
    }

    fn delete_key(&self, db_id: u32, key: &[u8]) -> BackendResult<()> {
        let cf = self.db.cf_handle(&cf_name(db_id)).ok_or_else(|| {
            BackendError::OperationFailed(format!("delete_key: cf for db_{db_id} not found"))
        })?;
        self.db.delete_cf_opt(&cf, key, &self.write_opts).map_err(|e| {
            BackendError::OperationFailed(format!("delete_key: {e}"))
        })
    }

    fn flush_db(&self, db_id: u32) -> BackendResult<()> {
        let name = cf_name(db_id);
        self.db.drop_cf(&name).map_err(|e| {
            BackendError::OperationFailed(format!("flush_db: drop cf {name}: {e}"))
        })?;
        self.db.create_cf(&name, &Options::default()).map_err(|e| {
            BackendError::OperationFailed(format!("flush_db: recreate cf {name}: {e}"))
        })
    }

    fn flush_all(&self) -> BackendResult<()> {
        for i in 0..self.num_databases {
            self.flush_db(i)?;
        }
        Ok(())
    }

    fn shutdown(&mut self) -> BackendResult<()> {
        self.initialized.store(false, Ordering::Release);
        Ok(())
    }

    fn key_may_exist(&self, db_id: u32, key: &[u8]) -> bool {
        let cf = match self.db.cf_handle(&cf_name(db_id)) {
            Some(cf) => cf,
            None => return false,
        };
        self.db.key_may_exist_cf(&cf, key)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::collections::HashMap;

    fn test_backend(num_databases: u32) -> (RocksDBBackend, tempfile::TempDir) {
        let tmp = tempfile::tempdir().expect("failed to create temp dir");
        let config = BackendConfig {
            db_path: tmp.path().to_str().unwrap().to_string(),
            db_size_bytes: 100 * 1024 * 1024,
            num_databases,
            max_in_flight_reads: 64,
            eviction_enabled: true,
            options: HashMap::new(),
        };
        let backend = RocksDBBackend::init(config).expect("failed to init backend");
        (backend, tmp)
    }

    #[test]
    fn put_then_get_returns_value_and_deletes() {
        let (backend, _tmp) = test_backend(1);
        backend.put_item(0, b"hello", b"world").unwrap();
        let result = backend.get_item(0, b"hello").unwrap();
        assert_eq!(result.value.as_deref(), Some(b"world".as_slice()));
        let result = backend.get_item(0, b"hello").unwrap();
        assert!(result.value.is_none());
    }

    #[test]
    fn delete_key_removes_entry() {
        let (backend, _tmp) = test_backend(1);
        backend.put_item(0, b"k", b"v").unwrap();
        backend.delete_key(0, b"k").unwrap();
        let result = backend.get_item(0, b"k").unwrap();
        assert!(result.value.is_none());
    }

    #[test]
    fn flush_db_clears_single_database() {
        let (backend, _tmp) = test_backend(2);
        backend.put_item(0, b"k0", b"v0").unwrap();
        backend.put_item(1, b"k1", b"v1").unwrap();
        backend.flush_db(0).unwrap();
        assert!(backend.get_item(0, b"k0").unwrap().value.is_none());
        assert_eq!(backend.get_item(1, b"k1").unwrap().value.as_deref(), Some(b"v1".as_slice()));
    }

    #[test]
    fn flush_all_clears_all() {
        let (backend, _tmp) = test_backend(3);
        for db in 0..3 {
            backend.put_item(db, format!("k{db}").as_bytes(), b"val").unwrap();
        }
        backend.flush_all().unwrap();
        for db in 0..3 {
            assert!(backend.get_item(db, format!("k{db}").as_bytes()).unwrap().value.is_none());
        }
    }
}
