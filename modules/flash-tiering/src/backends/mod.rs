//! Pluggable storage backends for the key spilling module.
//!
//! Each backend is self-contained with its own ASIO implementation.

#[cfg(feature = "backend-rocksdb")]
pub mod rocksdb;

#[cfg(feature = "backend-flashcache")]
pub mod flashcache;
