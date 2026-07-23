# Config Compatibility with Data Tiering (Non-Key-Spilling POC)

Status 2026-07-22, unstable @ 6976634d2 + this audit. Methodology mirrors the
command audit: classify configs by tiering-interaction surface, live-test the
risky ones (flashcache-mock).

## Summary answer: "does tiering work with all configs?"

Mostly yes — the config surface splits into: (a) configs tiering explicitly
integrates with (validated at init), (b) configs that are orthogonal, and
(c) a small risk set, live-tested below. One real bug found (volatile-*
runtime bypass) and one by-design surprise (noeviction makes tiering inert).

## maxmemory-policy (the core interaction)

| Policy | Status | Evidence |
|---|---|---|
| allkeys-lru | ✅ | primary test config, all 17 suites |
| allkeys-lfu | ✅ | live pressure test: 1238 spills, clean |
| noeviction | ⚠️ by design INERT | policy accepted, but ALL spill paths early-return (ext_storage.c:1319+): zero spilling, writes OOM exactly like vanilla. Tiering adds nothing under noeviction — document to users |
| volatile-lru/lfu/random/ttl, allkeys-random | 🚫 rejected (init + runtime) | init guard disables tiering with a warning (ext_storage.c:331); runtime CONFIG SET now rejected by updateMaxmemoryPolicy apply-guard (FIXED 2026-07-22, was a silent bypass). Tests: ext-storage-swapdb.tcl POLICY GUARD section, incl. tiering-disabled passthrough |

## Live-tested risk set

| Config | Status | Evidence |
|---|---|---|
| io-threads 4 | ✅ | full pressure: 13.2K spills, 7.3K fetches, DEL-of-flash-key, Lua sync-fetch — all correct, zero crash/BUG markers |
| maxmemory 0 (unlimited) | ✅ | explicit DEBUG SPILL + fetch round-trip works; natural spilling never triggers (no pressure) — expected |
| databases N / SELECT | ✅ | per-db flash namespaces isolated (db2 spill/fetch clean, db0 unaffected). SWAPDB remains gated |
| runtime CONFIG SET maxmemory (shrink) | ✅ | exercised throughout suites (pressure tests shrink/grow constantly) |
| lazyfree-* (all 5) | ✅ | tiered-aware free-effort guard (lazyfree.c:141) + crash-audit config ran all-yes |
| hash-max-listpack-* thresholds | ⚠️ partially probed | large values force hashtable pre-spill in probe; data-types suite spills small (listpack) collections. No issue observed; dedicated threshold-crossing-around-spill test would close it |
| activedefrag | ✅ | dedicated suite (ext-storage-defrag, 7 tests) |
| notify-keyspace-events | ✅ | del-semantics suite |
| appendonly / save / repl-* / cluster-enabled | 🚫 documented unsupported | persistence gates + KNOWN LIMITATION test; see command-compatibility.md |

## ext-storage-* own configs

Covered by dedicated suites: ext-storage-fc-configs (21 tests: immutable vs
modifiable enforcement, defaults, GC rates, buffered-write sizes, max-spill),
ext-storage-module-configs (16), ext-storage-max-spill-size (4).

## Open items from this audit

1. ~~volatile-* runtime bypass~~ FIXED 2026-07-22 (updateMaxmemoryPolicy
   apply-guard; value rolls back on rejection).
2. noeviction inertness: document user-facing ("tiering requires an eviction
   policy to spill"); consider warning at init when combined.
3. Encoding threshold-crossing test (small).

## SWAPDB: SUPPORTED (2026-07-22)

Implemented via option (a) — logical→physical db-id indirection, and no
storage-layer changes were needed (the indirection lives entirely in
ext_storage.c): identity-mapped arrays at init; SWAPDB swaps the two
mapping entries; all submit sites translate logical→physical; completion
processing reverse-routes physical→logical. Correct for IO in flight across
a swap because dbSwapDatabases moves entries (and their tiering-state bits)
together with the keyspace, and the completion follows the physical id to
whichever logical db now owns it. Tests: ext-storage-swapdb.tcl (5 SWAPDB +
4 policy-guard tests).
