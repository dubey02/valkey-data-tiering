# Performance Analysis — Convergence Package

## Final Benchmark Results (2026-05-25)

### Test Parameters
- 200 clients, 4 threads, 50K keys, 1000B values, 50MB maxmemory, allkeys-lru
- Flash: 1GB file on local NVMe (truncate -s 1G)

### Side-by-Side (200K SET + 200K GET)
| Metric | Native (C lock-free) | Module (Rust crossbeam) |
|--------|---------------------|------------------------|
| SET/s | 46,981 | 49,937 |
| GET/s | 29,577 | 34,716 |
| Spills | 45,048 | 50,672 |
| Fetches | 39,446 | 43,448 |
| Read misses | 19,847 | 11,348 |
| Hangs | 0 | 0 |

### 5-Minute Stress Test (5M SET + 5M GET = 10M ops)
| Metric | Native | Module |
|--------|--------|--------|
| SET/s sustained | 19,300 | 22,500 |
| SET/s pre-pressure | 52,000 | 52,000 |
| Crashes | 0 | 0 |
| Hangs | 0 | 0 |

### Why Module is ~17% Faster Under Pressure
- crossbeam SegQueue is unbounded → no ring overflow risk, better backpressure handling
- Native SPSC ring (8192 slots) can create brief stalls when full
- Both are identical pre-pressure (52K SET/s) — zero dispatch overhead confirmed

### Read Misses Explanation
- NOT a bug — expected under extreme memory pressure
- FlashCache GC evicts keys faster than engine can fetch them
- Engine handles correctly: returns nil, marks key absent
- Module has fewer misses because crossbeam processes completions faster

### Historical Comparison
| Version | SET/s (pressure) | Issue |
|---------|-----------------|-------|
| pthread mutex (old) | 19,000 | Mutex contention |
| Native lock-free | 47,000 (short) / 19,300 (5-min) | Production stable |
| Module crossbeam | 50,000 (short) / 22,500 (5-min) | Production stable |

### Bottleneck Under Pressure
- FlashCache io_uring becomes the bottleneck (not the queue)
- Main thread CPU not saturated under pressure (blocked clients reduce load)
- Disk IOPS moderate — io_uring completion processing is the limit
