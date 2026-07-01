//! FlashCache backend — 1 IO thread, async reads via callbacks, cron ticks.
//!
//! Only compiled when feature `backend-flashcache` is enabled.

pub mod ffi;
pub mod backend;
pub mod asio;

pub use backend::FlashCacheBackend;
pub use asio::FlashCacheAsio;
