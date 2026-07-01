//! Counting Bloom filter for tracking keys currently on flash storage.
//!
//! Uses 4-bit counters to support both add and remove operations.

use std::hash::{Hash, Hasher};

const NUM_HASHES: usize = 7;
const COUNTER_BITS: u8 = 4;
const COUNTER_MAX: u8 = (1 << COUNTER_BITS) - 1;

/// A counting Bloom filter with 4-bit counters.
pub struct CountingBloomFilter {
    counters: Vec<u8>,
    num_counters: usize,
    seeds: [u64; NUM_HASHES],
}

impl CountingBloomFilter {
    pub fn new(expected_items: usize, bits_per_item: usize) -> Self {
        let num_counters = expected_items * bits_per_item;
        let num_bytes = (num_counters + 1) / 2;

        let seeds: [u64; NUM_HASHES] = [
            0x9e3779b97f4a7c15,
            0x6c62272e07bb0142,
            0xbf58476d1ce4e5b9,
            0x94d049bb133111eb,
            0xd6e8feb86659fd93,
            0x4a7330612e8a9531,
            0x1b69170e635a6891,
        ];

        CountingBloomFilter {
            counters: vec![0u8; num_bytes],
            num_counters,
            seeds,
        }
    }

    pub fn add(&mut self, key: &[u8]) {
        for i in 0..NUM_HASHES {
            let idx = self.hash_index(key, i);
            let val = self.get_counter(idx);
            if val < COUNTER_MAX {
                self.set_counter(idx, val + 1);
            }
        }
    }

    pub fn remove(&mut self, key: &[u8]) {
        for i in 0..NUM_HASHES {
            let idx = self.hash_index(key, i);
            let val = self.get_counter(idx);
            if val > 0 {
                self.set_counter(idx, val - 1);
            }
        }
    }

    pub fn may_contain(&self, key: &[u8]) -> bool {
        for i in 0..NUM_HASHES {
            let idx = self.hash_index(key, i);
            if self.get_counter(idx) == 0 {
                return false;
            }
        }
        true
    }

    #[inline]
    fn get_counter(&self, idx: usize) -> u8 {
        let byte_idx = idx / 2;
        if idx % 2 == 0 {
            self.counters[byte_idx] & 0x0F
        } else {
            (self.counters[byte_idx] >> 4) & 0x0F
        }
    }

    #[inline]
    fn set_counter(&mut self, idx: usize, val: u8) {
        let byte_idx = idx / 2;
        if idx % 2 == 0 {
            self.counters[byte_idx] = (self.counters[byte_idx] & 0xF0) | (val & 0x0F);
        } else {
            self.counters[byte_idx] = (self.counters[byte_idx] & 0x0F) | ((val & 0x0F) << 4);
        }
    }

    #[inline]
    fn hash_index(&self, key: &[u8], hash_idx: usize) -> usize {
        let h = self.compute_hash(key, self.seeds[hash_idx]);
        (h as usize) % self.num_counters
    }

    #[inline]
    fn compute_hash(&self, key: &[u8], seed: u64) -> u64 {
        let mut hasher = std::collections::hash_map::DefaultHasher::new();
        seed.hash(&mut hasher);
        key.hash(&mut hasher);
        hasher.finish()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_basic_add_query() {
        let mut bf = CountingBloomFilter::new(1000, 10);
        assert!(!bf.may_contain(b"hello"));
        bf.add(b"hello");
        assert!(bf.may_contain(b"hello"));
    }

    #[test]
    fn test_add_remove() {
        let mut bf = CountingBloomFilter::new(1000, 10);
        bf.add(b"key1");
        assert!(bf.may_contain(b"key1"));
        bf.remove(b"key1");
        assert!(!bf.may_contain(b"key1"));
    }

    #[test]
    fn test_multiple_adds() {
        let mut bf = CountingBloomFilter::new(1000, 10);
        bf.add(b"key1");
        bf.add(b"key1");
        bf.remove(b"key1");
        assert!(bf.may_contain(b"key1"));
        bf.remove(b"key1");
        assert!(!bf.may_contain(b"key1"));
    }
}
