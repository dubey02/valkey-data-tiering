//! Pending reads and writes tracking for the key spilling module.

use std::collections::HashMap;
use std::ffi::c_void;
use std::sync::atomic::{AtomicU64, Ordering};

// ---------------------------------------------------------------------------
// Pending reads
// ---------------------------------------------------------------------------

pub struct PendingRead {
    pub key_robj: *mut c_void,
    pub ttl: i64,
    pub db_id: u32,
}

unsafe impl Send for PendingRead {}
unsafe impl Sync for PendingRead {}

pub struct PendingReadsMap {
    map: HashMap<u64, PendingRead>,
    next_id: AtomicU64,
}

impl PendingReadsMap {
    pub fn new() -> Self {
        Self {
            map: HashMap::new(),
            next_id: AtomicU64::new(1),
        }
    }

    pub fn insert(&mut self, key_robj: *mut c_void, ttl: i64, db_id: u32) -> u64 {
        let id = self.next_id.fetch_add(1, Ordering::Relaxed);
        self.map.insert(id, PendingRead { key_robj, ttl, db_id });
        id
    }

    pub fn remove(&mut self, request_context: u64) -> Option<PendingRead> {
        self.map.remove(&request_context)
    }

    pub fn len(&self) -> usize {
        self.map.len()
    }

    pub fn is_empty(&self) -> bool {
        self.map.is_empty()
    }
}

impl Default for PendingReadsMap {
    fn default() -> Self {
        Self::new()
    }
}

// ---------------------------------------------------------------------------
// Pending writes
// ---------------------------------------------------------------------------

pub struct PendingWrite {
    pub key_robj: *mut c_void,
    pub db_id: u32,
}

unsafe impl Send for PendingWrite {}
unsafe impl Sync for PendingWrite {}

pub struct PendingWritesMap {
    map: HashMap<u64, PendingWrite>,
    next_id: AtomicU64,
}

impl PendingWritesMap {
    pub fn new() -> Self {
        Self {
            map: HashMap::new(),
            next_id: AtomicU64::new(1),
        }
    }

    pub fn insert(&mut self, key_robj: *mut c_void, db_id: u32) -> u64 {
        let id = self.next_id.fetch_add(1, Ordering::Relaxed);
        self.map.insert(id, PendingWrite { key_robj, db_id });
        id
    }

    pub fn remove(&mut self, id: u64) -> Option<PendingWrite> {
        self.map.remove(&id)
    }

    pub fn len(&self) -> usize {
        self.map.len()
    }

    pub fn is_empty(&self) -> bool {
        self.map.is_empty()
    }
}

impl Default for PendingWritesMap {
    fn default() -> Self {
        Self::new()
    }
}
