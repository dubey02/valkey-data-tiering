# BUG: Silent lost writes under promotion=never / 2hit-50k (transient revert clobbers SETs)

**Status**: Open — discovered 2026-07-14 during the public/private policy-benchmark parity
investigation. Out of scope for that fix set; needs its own investigation.

**Affects**: BOTH repos identically —
`dubey02/valkey-data-tiering` branch `policies` AND the private
`policies/private-valkey` branch `policies` (verified by single-key repro on both binaries,
2026-07-14). Applies to `--ext-storage-promotion-policy never` and the transient
(1st-hit) path of `2hit-50k`.

## Symptom

A `SET` on a flash-resident key returns `+OK` but the written value is silently
discarded. Subsequent `GET`s return the OLD value from flash. Silent data loss,
acknowledged as success.

## Single-key reproduction

```
valkey-server --maxmemory 100mb --enable-debug-command yes \
  --ext-storage-enabled yes --ext-storage-backend flashcache \
  --ext-storage-path <preallocated file >= capacity> --ext-storage-capacity-mb 2048 \
  --ext-storage-admission-policy dram --ext-storage-promotion-policy never

SET key1 <300B value "vvv...">     -> OK
DEBUG SPILL key1                   -> OK   (key1 now ONLY_FLASH)
GET key1                           -> "vvv..."  (transient serve, correct)
SET key1 <300B value "www...">     -> OK   (!!)
GET key1                           -> "vvv..."  (OLD value — the www write is lost)
```

Reproduced on 2026-07-14 against both the public binary (port 6412) and the private
binary (port 6413) with real FlashCache.

## Mechanism

The transient lifecycle (Phase 2B) is:

1. Key K is `ONLY_FLASH`. An incoming command (GET **or SET** — `keyBlocksClient`
   blocks both) blocks the client and submits a PEEK fetch.
2. Read completion (transient path): the fetched OLD value is installed into the
   dict entry, tiering state -> `ONLY_MEMORY`, and K is recorded in
   `transient_values[]` with `tv->orig_val` = the TIERED tombstone placeholder.
3. `processUnblockedClients()`: the blocked SET re-executes against the now-present
   entry — it **overwrites the entry's value with the NEW value** and replies `+OK`.
4. `extStorageFreeTransientValues()` (beforeSleep, after unblock): for each
   `transient_values[]` entry the guard is only

   ```c
   if (entry != NULL && !objectIsTiered(entry)) {
       /* free current value, restore tv->orig_val tombstone, state -> ONLY_FLASH */
   ```

   It cannot distinguish "entry still holds the transient value we installed" from
   "entry now holds a NEW value written by a client during the transient window".
   It unconditionally frees the entry's current value (the NEW value) and reinstalls
   the tombstone. The flash copy (OLD value) remains authoritative.

Source: `src/ext_storage.c`, `extStorageFreeTransientValues()` (revert loop guard at
`entry != NULL && !objectIsTiered(entry)`), identical in both repos.

## Why benchmarks never caught it

`valkey-benchmark` never reads back what it writes with verification, and the
policy-matrix workload treats SET replies (`+OK`) as success. Throughput and memory
metrics are unaffected — only the *data* is wrong.

## Impact scope

- Any write command on an `ONLY_FLASH` key while promotion is transient
  (never / 2hit-50k first hit): SET, APPEND, INCR, SETEX, etc.
- DEL is likely safe: the revert loop's `else` branch handles deleted entries,
  and delete commands take the `is_delete_cmd` path (submitDel), not fetch.
  Unverified — check `DEL` and `EXPIRE` during the transient window.

## Candidate fix directions (unvalidated)

1. **Dirty check in the revert**: record the installed transient value pointer
   (`tv->transient_val = objectGetVal(entry)` at install time); in
   `extStorageFreeTransientValues()` only revert when
   `objectGetVal(entry) == tv->transient_val`. If the value pointer changed, a
   client wrote during the window: keep the new value, free `tv->orig_val`,
   leave state `ONLY_MEMORY`, and submit a DELETE for the stale flash copy.
2. **Write-triggered cancellation**: in the write path (`setKey`/`dbReplace` hook),
   if the key is in `transient_values[]`, remove it from the array (promote-on-write
   semantics) and delete the flash copy.
3. Note fix (1) must also handle same-pointer-reuse (value freed and reallocated at
   the same address) — comparing a stored dirty FLAG set by the write path is more
   robust than pointer identity.

## Related but distinct

The 2026-07-14 parity fixes (duplicated completion epilogue, evict.c early-out,
mid-zone C_ERR, PEEK-preserving READ_RETRY) restored benchmark parity but do NOT
address this bug — it predates them and exists in the "good" private repo too.
