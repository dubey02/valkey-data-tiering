//! RocksDB backend — multi-threaded IO pool, all synchronous operations.
//!
//! No FlashCache dependencies. Self-contained ASIO implementation.

pub mod backend;
pub mod asio;

pub use backend::RocksDBBackend;
pub use asio::RocksDBAsio;
