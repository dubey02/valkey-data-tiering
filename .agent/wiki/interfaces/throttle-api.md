---
title: Throttle API
status: active
sources:
  - src/ext_storage_throttle.h:1-39
updated: 2026-06-04
type: interface
tier: working
claim_count: 5
edges:
  - to: components/throttle-equilibrium.md
    kind: refers_to
    source: human
    created: 2026-06-03
  - to: interfaces/info-metrics.md
    kind: refers_to
    source: llm_relation
    created: 2026-06-04
---

# Throttle API

> `src/ext_storage_throttle.h` — a token-bucket client throttler. Under memory pressure
> clients are **queued instead of processed**; a timer periodically refills tokens and
> releases queued clients, keeping the event loop free to drain completions and spill.
> Control-loop behaviour: [throttle-equilibrium](../components/throttle-equilibrium.md).

## Functions

| Fn (`ext_storage_throttle.h`) | Line | Role |
|------|------|------|
| `extStorageThrottle_init(void)` | 16 | init; called from `extStorage_init()` |
| `extStorageThrottle_shouldThrottle(client *c)` | 20 | returns 1 if client was queued (defer command), 0 to proceed |
| `extStorageThrottle_adjustRate(void)` | 24 | recompute rate from current memory pressure (periodic) |
| `extStorageThrottle_recordCommandLatency(long long duration_us)` | 28 | feed command duration; tracks min latency over rolling window for TPS |
| `extStorageThrottle_removeClient(client *c)` | 31 | drop a client from the queue (e.g. on disconnect) |

## Metrics getters (`:34-37`)

`getThrottledCount()` and `getQueuedClients()` (`long long`), `getCurrentRate()` and
`getAllowedTps()` (`double`) — surfaced as the `throttle_*` fields in
[info-metrics](info-metrics.md).

See also: [throttle-equilibrium](../components/throttle-equilibrium.md), [info-metrics](info-metrics.md).
