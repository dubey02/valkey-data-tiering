# Wiki Log

Append-only. One line per ingest/query/lint. Newest at bottom.

## [2026-06-03] scaffold | Created data tiering wiki skeleton
- Schema [WIKI](WIKI.md), [index](index.md), this log.
- 3 seed pages written (draft): [00-overview](00-overview.md), [01-architecture](01-architecture.md), [state-machine](components/state-machine.md).
- 22 stub pages created across components/, interfaces/, flows/, decisions/.
- 6 Graphviz diagrams rendered to PNG. 5 mermaid sequence sources written; PNG render
  blocked on this aarch64 host (puppeteer ships x86-64 Chrome only, no sudo). `mmdc`
  installed; `make seq` works once an arm64 Chrome is provided via PUPPETEER_EXECUTABLE_PATH.
- Next: ingest source files to promote stubs draft→active (verify file:line citations).

## [2026-06-03] keg | Ported KiRoom Entity Graph tooling + typed edges
- Ported from ElastiCacheServerlessPEBrain/tools/keg: `keg/keg_lint.py`, `query.py`,
  `extract_relations.py`, `migrate_all.py`, `RETRIEVAL.md`, `viewer/` (Cytoscape).
  Adaptations: `--root` default `wiki`; added `calls` + `configures` edge kinds.
- Converted 110 `[[wikilinks]]` → standard markdown links (tooling requirement).
- `migrate_all.py` seeded 51 `refers_to` edges; hand-authored strong edges
  (contains/depends_on/calls/implements/configures + symmetric contradicts) and
  added type/tier/claim_count to all 25 pages.
- Lint clean: 0 errors, 0 warnings, 0 orphans, 68 edges. `edges.json` generated.
- View graph: `python3 -m http.server 8000 --bind 127.0.0.1 -d wiki/keg/viewer`.

## [2026-06-03] move | Wiki relocated to .agent/wiki
- New home: `.agent/wiki/` (copied from `wiki/`).
- KEG tools now resolve their root from `Path(__file__).parent.parent`, so `--root`
  defaults to the wiki they live in — no path edits needed for future moves.
- Updated command/help paths in WIKI.md, index.md, keg/RETRIEVAL.md, and the viewer.
- Viewer served from the new root: `python3 -m http.server 8000 --bind 127.0.0.1 -d .agent/wiki`
  → open `http://localhost:8000/keg/viewer/index.html`.
- Old `wiki/` copy can be removed by the owner once confirmed.

## [2026-06-04] ingest | headers → interfaces tranche
- Sources: src/storage.h, src/ext_storage.h, src/ext_storage_bridge.h,
  src/ext_storage_throttle.h, src/ext_storage.c:996-1072, src/config.c:3312-3427.
- Updated (stub→active): interfaces/storagetype-vtable, ext-storage-api, bridge-api,
  throttle-api, info-metrics, config-and-module-args. All citations file:line-verified
  against headers read in full.
- Notes: dispatch rule confirmed — async path iff put_async!=NULL else shared middleware
  (storage.h:7-15). INFO field names captured verbatim. ext-storage-* directives are
  IMMUTABLE except items-spillover-batch-size (MODIFIABLE). 0 lint errors.

## [2026-06-04] fix | scrub cross-project bleed from interface pages
- info-metrics: removed "Use verbatim — no aliasing/renaming" (a benchmark-CSV tooling
  lesson, not a code fact).
- ext-storage-api: rewrote TieringState section to drop "tieringStateEntry hashtable was
  removed" changelog narration. Verified the current mechanism against this project's code:
  state is robj.tiering_state 3-bit bitfield (server.h:829), read/written via
  extStorageGet/Set/RemoveState over db->keys (ext_storage.c:97-124); no side table.
  Added src/server.h:829 + src/ext_storage.c:97-124 to sources. 0 lint errors.

## [2026-06-04] fix | review-driven accuracy fixes (interface tranche)
- 6 fresh-agent adversarial reviews + new Layer-1 verifier (keg/verify_citations.py) run
  on the 6 active interface pages.
- throttle-api: signature param `us` -> `duration_us` (match ext_storage_throttle.h:28).
- info-metrics: relabeled the `:999-1004` section "lifetime totals" -> "Item counters &
  gauges"; added a Kind column marking num_items_spilling/fetching/on_flash as gauges
  (only the three total_num_items_* are monotonic). All 31 field names + lines verified.
- Other 4 pages verified clean. Every reviewer "off-by-one" line finding was a false
  positive (LLM miscount) adjudicated against grep -n; no citations changed for those.

## [2026-06-04] ingest | engine-integration + core flows (spill/fetch/delete)
- Sources read in full: src/ext_storage.c:204-995 (gate, drain, spillItemAsync, two spill
  pumps, eviction override) + call sites server.c:1911/2036/4017/4718-4735, evict.c:540-555,
  networking.c:4309. All citations from grep -n.
- Updated (stub→draft): components/engine-integration; flows/spill, fetch, delete.
- Key facts: 4 event-loop hooks (throttle gate, preCommandExec block gate + post-gate TIERED
  recheck, beforeSleep drain+weak pump, performEvictions aggressive pump+eviction override);
  drain is uncapped; spill is zero-copy borrowed-robj, serialization on the IO thread (not
  main); DEL/evict on flash use async delete with NO fetch; PENDING_EVICT resolves
  evict-during-spill/fetch races.
- Corrected diagrams: spill/fetch mermaid + .mmd sources moved serialize/deserialize to the
  IO thread (was shown on main thread).
- Status draft (not active): pending Layer-1 verify_citations + Layer-2 fresh-agent review.

## [2026-06-04] lint | Layer-2 fresh-agent review → engine-integration + flows active
- 4 batched fresh-agent reviews (max_turns=60) on engine-integration, spill, fetch, delete:
  all VERDICT PASS. 0 fabricated symbols, 0 misframings, 0 sequencing errors. IO-thread
  serialize/deserialize claims independently confirmed (extStorageSerializeValue→createDumpPayload
  in fc_io_worker; READ completion receives a ready robj).
- Applied citation-precision tweaks only: engine serialization :751-757(comment)→:167-172(code);
  aggressive pump :964-995→:964-990; spill zfree :552→:554; fetch reject :493-497, PENDING_EVICT
  :614-629, value-present :636-662; delete state-read :677, COPYING_TO_FLASH :679-685.
- Promoted stub→draft→active (verified vs code): engine-integration, spill, fetch, delete.
- Process note: per-claim grep reviews hit turn_limit:30 (engine, fetch, spill originals);
  re-ran batched at max_turns=60. Reviewer line-number findings adjudicated against grep -n.

## [2026-06-04] fix | rebase onto live src/storage/storage.h; deleted dead src/storage.h
- Discovery: src/storage.h (richer vtable: drain/iterator/flush/snapshot/config/exists/sync,
  CacheLib, STORAGE_ERR_CORRUPT, REJECTED=-4) was NOT compiled — nothing includes the bare
  header; the build compiles storage/*.o which include src/storage/storage.h (the live, smaller
  interface: completion_privdata, storageInit, FlashCacheReal/RocksDBAsync getters, REJECTED=-3,
  no iterator/snapshot/flush/config/exists/sync). Verified via Makefile:512-518/773-774 and
  grep of #include "storage/storage.h" (bridge, module) vs zero bare includes.
- Owner-directed: deleted src/storage.h (git-tracked, reversible; build references = 0).
- Rebased on src/storage/storage.h: rewrote interfaces/storagetype-vtable (active) and
  components/pluggable-storage-api (draft, + storage_dispatch.c/storage_middleware.c).
- Scrubbed src/storage.h refs: 01-architecture (sources + component row), index summary,
  backends/serialization/persistence-replication sources, WIKI.md raw-source list, and the
  config-and-module-args backend_opts claim (no such field in the live storageConfig).

## [2026-06-04] lint | Layer-2 (inline) on rebased storage pages
- Subagent runner down this session (interpreter path .../meshclaw/2.6.0/python3.10 missing,
  2/2 spawns failed) → did Layer-2 inline against cat -n/grep -n.
- pluggable-storage-api: all dispatch/middleware citations verified → promoted draft→active.
- storagetype-vtable: fixed 3 citation errors introduced in the rebase — dispatch-rule
  :50-53→:3+:93 (was pointing at storageCompletion struct), identity name/version :78-79→:76-77,
  sync KV :84-92→:84-91. Stays active.

## [2026-06-04] ingest | bridge-layer + eviction-integration (inline; subagents down)
- bridge-layer (stub→draft): ext_storage_bridge.c:1-275 — backend select/init + mock fallback,
  bridgeRequestCtx key_owned borrow-vs-dup, submitPut borrows key / submitGet+Del sdsdup,
  completion ring (IO thread writes via bridge_on_completion), poll+translate to
  ValkeyModuleExternalStorageMsg incl req_ctx==NULL FlashCache-GC synthetic DELETE.
- eviction-integration (stub→draft): evict.c (performEvictions short-circuits to tiering path
  + goto update_metrics, never the destructive loop; findBestEvictionCandidate shared between
  EvictionPoolLRU and spillPoolLRU; evictionPoolPopulate spillable-only gate evict.c:120) +
  ext_storage.c (extStoragePerformEvictions OK/OOM table, extStorageEvictFlashKey, dynamic
  concurrency). Flagged contradiction: code SPILL_CONCURRENT_BASE=50 vs stale comment "2".
- Layer-2 pending (subagent runner down); citations grep -n verified inline.

## [2026-06-04] ingest | throttle-equilibrium + completion-drain + evict-during-fetch (inline)
- throttle-equilibrium (stub→draft): ext_storage_throttle.c:1-298 — token bucket, gate at
  readQueryFromClient (networking.c:4309), adjustRate (rate from memory pressure 0→1 over
  [maxmemory, 1.1x], allowed_tps = max_tps*(1-rate*0.99)), 1ms release timer, and the coupling
  extStorageUpdateSpillConcurrency(rate) that makes rate drive BOTH ingress-down and spill-up.
- completion-drain (stub→draft): processCompletedStorageRequests:527-722 uncapped poll loop +
  per-completion transitions + unblock; called from beforeSleep/timer.
- evict-during-fetch (stub→draft): PENDING_EVICT entry (extStorageEvictFlashKey :814-816;
  delete-during-spill :679-685) and resolution (READ :614-629 discard+dbDelete; WRITE :556-567).
- Citations grep -n verified inline (subagent runner still down). 0 lint errors.

## [2026-06-04] lint | Layer-2 (inline) → 5 draft pages active
- Subagent runner still down (interpreter path); did Layer-2 inline by re-confirming each
  page's load-bearing claims against the cited source lines (cat -n/grep -n).
- All PASS, 0 issues: bridge-layer (backend-select order, req_ctx==NULL synthetic DELETE),
  eviction-integration (override OK/OOM + goto update_metrics short-circuit), throttle-equilibrium
  (rate/allowed_tps formula + spill-concurrency coupling), completion-drain (uncapped poll loop),
  evict-during-fetch (PENDING_EVICT entry/resolution). No citation fixes needed (bare-:NN issues
  were already fixed at ingest).
- Promoted stub→draft→active: bridge-layer, eviction-integration, throttle-equilibrium,
  completion-drain, evict-during-fetch. A fresh-agent re-pass is recommended once the runner is
  restored, for independent (non-inline) assurance.

## [2026-06-04] ingest | serialization + memory-accounting (inline)
- serialization (stub→draft): ext_storage.c:155-198 — value via RDB DUMP (createDumpPayload/
  rdbLoadObject, all object types), zero-copy key, IO-thread execution (flashcache_real:132/167-180).
  Resolved contradiction: DUMP path is not string-only (supersedes the historical string-only note).
- memory-accounting (stub→draft): tiered marker OBJ_ENCODING_TIERED=15 (server.h:779) + empty-SDS
  placeholder; objectComputeSize tiered branch (object.c:1202-1212) = robj shell + placeholder;
  decrRefCount tiered free (object.c:632); spill tombstone ext_storage.c:594-596; per-key RAM
  floor → key-heavy OOM. Flagged contradiction: server.h:838 says val_ptr NULL but code uses a
  non-NULL empty SDS (objectComputeSize + decrRefCount confirm).
- Layer-1 clean (0 errors). Layer-2 pending (runner down).

## [2026-06-05] doc | handoff — documented Verification (3 layers) in WIKI.md
- WIKI.md now documents the 3-layer verification (Layer-1 verify_citations.py gate, Layer-2
  fresh-agent/inline adversarial review, Layer-3 human) + the operational rules (adjudicate
  line findings vs grep -n; max_turns>=60 + batched greps; explicit file:line on multi-source
  pages; author from grep -n). Added verify_citations.py to Start-here + tooling. Fixed the
  stale "most pages are stub" line.
- index.md gained a "Remaining work (handoff)" section: 16 active / 5 draft / 4 stub, with the
  exact draft (Layer-2-pending vs seed) and stub (sources to ingest) breakdown, the subagent
  runner restart caveat, and the diagram render block.
- Corrected an earlier mis-count (had said 18 active; actual 16). A future agent can resume from
  AGENTS.md -> WIKI.md Start-here -> index.md statuses + Remaining work + log tail.

## [2026-06-05] lint | Layer-2 (fresh-agent) → serialization + memory-accounting active
- Subagent runner restored (was down in prior handoff). Ran fresh-agent adversarial Layer-2 via
  two parallel `valkey-dt-reviewer` subagents (one per page, ONLY repo code, batched grep,
  max_turns 70). NOTE: spawn `cwd` outside allowed roots (~/workspace,~/workplace) → HTTP 400;
  pass absolute repo paths in the prompt instead.
- serialization: REVIEW PASS, all 8 load-bearing claims verified. 1 NIT — `extStorageDeserializeValue`
  cited `:180-189` but fn closes at `:190` (return @189, `}` @190); fixed → `:180-190`. DUMP-all-types
  (createDumpPayload→rdbSaveObject), zero-copy key, IO-thread exec all confirmed.
- memory-accounting: REVIEW PASS, all claims verified. 1 NIT — `num_items_on_flash++` is at
  ext_storage.c:597 not :596 (596 = `total_items_spilled_to_ext_storage++`); fixed body cite
  594-596 → 594-597. Also tightened decrRefCount source 626-640 → 629-640 (626 was inside prior
  fn). val_ptr-NULL-vs-empty-SDS contradiction (server.h:838) confirmed real + correctly flagged.
- Promoted both draft→active. Layer-1 re-run clean (0 errors). State now 18 active / 3 draft / 4 stub.
- Next: seed pages (00-overview, 01-architecture, state-machine) need full Layer-1+2; then stubs
  (backends, persistence-replication, adr-index, known-limitations) via Ingest.

## [2026-06-05] ingest | verify 3 seed pages → active (00-overview, 01-architecture, state-machine)
- Inline Layer-2 against code. state-machine: accurate as-is (robj.tiering_state, transitions,
  blocking matrix all match keyBlocksClient) → active.
- 00-overview: CORRECTED the headline "Key semantics" table. It claimed EXISTS/TYPE/TTL/EXPIRE
  answer from RAM with no fetch (sourced from DATA-TIERING.md design doc), but verified the gate
  is value-agnostic: preCommandExec (server.c:4718, no cmd-type guard) → keyBlocksClient ONLY_FLASH
  blocks+fetches ANY named key (ext_storage.c:288-526). Only SCAN/DBSIZE (no named key) and
  DEL/UNLINK (async delete) avoid the fetch. Flagged as ⚠️ CONTRADICTION (design intent vs POC);
  added src/ext_storage.c:288-526 to sources. → active.
- 01-architecture: fixed threading model — serialize/deserialize runs on the IO thread (RDB DUMP),
  not the main thread as previously stated. → active.
- New active count: 21. Remaining: 4 stub (backends, persistence-replication, adr-index,
  known-limitations). The metadata-fetch contradiction is a candidate for known-limitations.

## [2026-06-05] ingest | backends (src/storage/* + modules/* storage backends) → draft
- New/written: components/backends.md (stub→draft). Cataloged all 8 `storageType`s: native C
  getters `flashcache`/`flashcache-real`/`rocksdb`(sync)/`rocksdb-async` (storage.h:127-130), plus
  module backends `module-example`, `module-flashcache-real`, Rust `non-key-spilling-rust`,
  `rocksdb-tiering`. Documented the two registration paths (native getters selected by the bridge
  name-switch ext_storage_bridge.c:68-74 vs ValkeyModule_RegisterStorageBackend module.c:820-832,
  module-takes-precedence ext_storage_bridge.c:65-66), the sync/async dispatch fork
  (storage_dispatch.c:26 → shared middleware storage_middleware.c:103), and real-backend robj
  serialization on the IO thread (storage_flashcache_real.c:170-192; Rust FFI lib.rs:32-36).
- Citations authored from grep -n; multi-source page uses explicit file.c:line throughout.
- 2 NEW contradictions flagged inline (→ known-limitations tranche):
  1. storageGetRocksDBType (sync, "uses shared middleware" per storage.h:129) is declared but never
     referenced in src/ or modules/ — bridge's "rocksdb" arm selects the ASYNC type
     (ext_storage_bridge.c:74). The whole storage_middleware.c sync path is unreachable/dead.
  2. Rust backends implement key_may_exist (rocksdb/backend.rs:218, flashcache/backend.rs:100) but
     the value-spill module leaves it unregistered (lib.rs:533-536: false positives "block clients forever").
  + doc-vs-code: README.md uses stale `key-spilling` module name/paths; live crate is `non-key-spilling`.
- Updated: 01-architecture.md backends-row cell ("rocksdb (sync via middleware)" → async; sync
  variant unwired) to match the bridge (grep-verified). index.md catalog + Remaining work + counts.
- Layer-1 gate: verify_citations + keg_lint --stats both re-run (see lint log line below).
- Next: Layer-2 fresh-agent adversarial review of backends.md (valkey-dt-reviewer, max_turns>=60,
  batched greps), adjudicate line findings vs grep -n, then promote draft→active.

## [2026-06-05] lint | Layer-2 (fresh-agent) → backends active
- Ran fresh-agent adversarial Layer-2 on components/backends.md via `valkey-dt-reviewer` subagent
  (ONLY repo code, batched grep, max_turns 70). Verdict: REVIEW PASS — all 16 load-bearing claims
  supported, 0 fabricated symbols, both flagged contradictions confirmed real (storageGetRocksDBType
  truly dead: only its def in storage_rocksdb.c:115 + decl storage.h:129, no callers anywhere;
  key_may_exist implemented but unregistered, lib.rs:533-536).
- 2 line-number nits, both ADJUDICATED against grep -n and CONFIRMED (reviewer right this time):
  1. `MODULE_NAME` is at lib.rs:39 (page said :42 = DEFAULT_DB_SIZE_BYTES). Fixed.
  2. flashcache-real serialize calls: extStorageSerializeKey@167, extStorageSerializeValue@169;
     page range :170-176 missed both. Fixed → :167-176.
- Layer-1 re-run clean (backends.md [OK], keg_lint 0 errors/0 warnings/0 orphans). Promoted
  draft→active. State now **22 active / 0 draft / 3 stub**. edges.json regenerated.
- Next stub tranches: persistence-replication (rdb/aof/defrag skips); then adr-index +
  known-limitations (collect all flagged contradictions, incl. the 2 new backends ones).

## [2026-06-05] ingest | persistence-replication (rdb.c/aof.c/defrag.c/replication.c tiering skips) → active
- New/written: components/persistence-replication.md (stub→active). Thesis: all persistence/replication
  paths key off `objectIsTiered` (server.h:839). RDB: rdbSaveKeyValuePair returns 0 (rdb.c:1195),
  omitting the WHOLE pair; caller loop rdb.c:1447 treats res<0 as the only error. rdbSaveRio (rdb.c:1483)
  is the shared funnel: RDB file (rdbSave rdb.c:1636), AOF preamble (aof.c:2530-2532), full-sync
  (rdbSaveRioWithEOFMark rdb.c:1536-1545, RDBFLAGS_REPLICATION; driven from replication.c:1018-1021).
  AOF tail = feedAppendOnlyFile (aof.c:1446) logs commands not values. defragKey (defrag.c:704) skips
  tiered at :712 before activeDefragStringOb/defragLater. replication.c has ZERO tiering code.
- 2 NEW contradictions flagged inline (→ known-limitations tranche):
  1. RDB/full-sync drops tiered keys entirely (cold data lost on restart/sync unless module restores;
     rdb.c:1191-1194) — headline durability limitation.
  2. Plain-AOF asymmetry: rewriteObjectRio (aof.c:2356-2409) has no objectIsTiered guard, so
     aof_use_rdb_preamble=no emits a tiered key from its empty-SDS placeholder (object.c:1205-1211)
     instead of skipping. Default preamble mode avoids it.
- Citations authored from grep -n; unique basenames so basename:line resolves. Edges: refers_to →
  state-machine, serialization, memory-accounting, engine-integration, backends, bridge-layer,
  known-limitations.
- Layer-2: fresh-agent valkey-dt-reviewer (max_turns 70, batched grep) = REVIEW PASS, all 7 claims
  SUPPORTED, 0 fabricated symbols, contradiction correctly identified, NO fixes required. Adjudicated
  the one reviewer nit (rewriteObjectRio ends at 2409 not 2404) via awk → tightened range to :2356-2409.
- Layer-1 clean (page [OK], keg_lint 0 errors/0 warnings/0 orphans). Promoted draft→active.
  State now **23 active / 0 draft / 2 stub**. edges.json regenerated.
- Next: adr-index + known-limitations (collect ALL flagged contradictions, incl. 2 backends + 2 here).

## [2026-06-05] ingest | adr-index + known-limitations (decisions) → active | WIKI COMPLETE
- New/written: decisions/adr-index.md + decisions/known-limitations.md (stub→active). Final tranche.
- adr-index: 9 code-grounded ADRs (001 values-only server.h:779/839; 002 robj tiering_state
  bitfield server.h:829; 003 one vtable two paths storage_dispatch.c:26; 004 robj-at-boundary
  serialize ext_storage.c:169; 005 module-precedence + mock fallback ext_storage_bridge.c:65-104;
  006 DUMP all-types ext_storage.c:169/749; 007 persistence skips tiered rdb.c:1195; 008 memory-gated
  throttle 1.0-1.1x/1.2x ext_storage.c:877-884; 009 dynamic spill concurrency ext_storage.c:840-846).
- known-limitations: collected ALL contradictions, each cited to compare/ code (NOT ab/ redesign —
  deliberately ignored the ab/ cap-less controller + image, which is a different uncommitted branch):
  C1 val_ptr NULL (server.h:838) vs empty-SDS (object.c:632-634, 1206-1211); C2 SPILL_CONCURRENT_BASE
  comment "only 2" (ext_storage.c:836) vs #define 50 (:39, floor :842); C3 metadata no-fetch intent
  vs value-agnostic preCommandExec (server.c:4718) + keyBlocksClient ONLY_FLASH (ext_storage.c:275-526);
  C4 dead sync rocksdb (storage_rocksdb.c:115); C5 unregistered key_may_exist (lib.rs:533-536); C6
  plain-AOF placeholder (aof.c:2356-2409). Functional: L1 durability drop (rdb.c:1191-1195), L2 two
  OOM paths (hard cap ext_storage.c:877-884; key-floor :894-908), L3 single IO thread
  (ext_storage_bridge.c:89). Resolved: R1 string-only→DUMP (ext_storage.c:169/749). Symmetric
  contradicts edge with serialization preserved.
- Layer-2: fresh-agent valkey-dt-reviewer (max_turns 70, batched grep) = REVIEW PASS both pages, 0
  unsupported claims, 0 fabricated symbols, contradictions accurately stated. 3 nits: hard-cap range
  :880-886→:877-884 (GENUINE, fixed in both pages + frontmatter); key-floor C_ERR "909" and C3 "4722"
  REJECTED as reviewer miscounts (awk confirms C_ERR@908, comment@4721 — page was right).
- Also de-backticked command-name list in C3 (EXISTS/TYPE/TTL) to avoid benign symbol-grounding WARN.
- Layer-1 clean (both [OK], keg_lint 0 errors/0 warnings/0 orphans, 180 edges). Promoted both
  draft→active. **WIKI COMPLETE: 25 active / 0 draft / 0 stub / 0 stale.** edges.json regenerated.
- Remaining is maintenance only: Layer-3 human review of synthesis pages; mermaid PNG render block
  (arm64 Chrome); re-ingest on code change.

## [2026-06-05] fix | mermaid sequence diagrams rendered + embedded in flow pages
- Unblocked mermaid PNG rendering on this aarch64 + glibc-2.26 host. Root cause of the prior
  block: Chrome-for-Testing (what Puppeteer fetches) ships no linux-arm64 build — the cached
  "linux_arm" Chrome was actually an x86-64 ELF (`file` confirmed) and won't exec; no sudo to
  install chromium; no qemu. Fix: render through a container. New `diagrams/render.Dockerfile`
  (node:22-slim + Debian arm64 `chromium` + `@mermaid-js/mermaid-cli@11.4.2`); `Makefile` now
  drives mermaid via `docker run … mermaid-render:local` (targets `seq-image`, `seq`), with
  `SCALE?=3` + `BG?=white` for hi-DPI output. Native override documented (`MERMAID=…`).
- Rendered all 5 sequences at 3× (was 784px wide → now ~2352px): spill 2352x1581, fetch
  2352x1560, delete 2352x1134, evict-during-fetch 2226x1503, completion-drain 2352x1503.
- Updated: 5 flow pages (`flows/{spill,fetch,delete,evict-during-fetch,completion-drain}.md`)
  — replaced the `PNG (pending arm64 Chrome)` placeholder line with a `![…](../diagrams/X.png)`
  embed and wrapped the in-page mermaid source in a collapsible `<details>` (regenerate hint).
  Balance verified: each page = 1 img + 1 `<details>` + 1 `</details>`.
- Updated `index.md`: Diagrams table 5 mermaid rows `mmdc ⏳`→`mmdc ✅`; regenerate command fixed
  (`.agent/wiki/diagrams`, container note); legend rewritten; Remaining-work diagram bullet +
  maintenance sentence updated (render no longer pending).
- No `sources:` or content claims changed — diagram/tooling only. Layer-1 gate re-run below.

## [2026-06-08] ingest | Smith-predictor commit e4ed4ff3a (Tranche A — spill/eviction core)
- Source: git commit `e4ed4ff3a` "Smith predictor for spilling" (src/ext_storage.c +244/-94,
  ext_storage_throttle.c, ext_storage_bridge.c, storage/storage.h, storage_flashcache_real.c,
  ext_storage.h). Landed 2026-06-06, after the 2026-06-05 verify; shifted all ext_storage.c
  citations by ~74 lines and deleted `SPILL_CONCURRENT_LIMIT` (the one standing Layer-1 ERROR).
- Updated: `components/eviction-integration.md`, `components/engine-integration.md`,
  `flows/spill.md`, `components/memory-accounting.md`; re-rendered `diagrams/spill-sequence.{mmd,png}`.
- Substance: spill loop is now **cap-less + projected-gated** (`spillFillToProjected`,
  `ext_storage.c:997-1033`); eviction over-budget decision reads `extStorageProjectedMemory()`
  (two-stage Smith predictor `used − inflight_spill_ram_bytes − submit_depth·mean_spill_ram`,
  `ext_storage.c:79-88`); WRITE completion debits `inflight_spill_ram_bytes` by `msg->ram_bytes`
  (`:613`); `extStorageOnSpillSubmit()` hook in `spillItemAsync` (`:849`); footprint computed by
  `objectComputeSize` on the IO thread at serialize (`storage_flashcache_real.c:197`).
  `SPILL_CONCURRENT_BASE`/LIMIT, `max_num_concurrent_items_spilled`, and
  `extStorageUpdateSpillConcurrency` removed; spill/throttle now decoupled (implicit via memory).
- Contradictions found (folding into known-limitations in Tranche D): (1) `items_spillover_batch_size`
  var (`ext_storage.c:95`) + `ext-storage-items-spillover-batch-size` config (`config.c:3426`) still
  exist but are now **dead** (cap-less loop never reads them); (2) prior C2 (`SPILL_CONCURRENT_BASE`
  comment "2" vs `#define` 50) is retired — the `#define` no longer exists.
- Edges: +4 typed `refers_to` (engine→memory-accounting, engine→known-limitations,
  eviction→memory-accounting, spill→memory-accounting). Graph 180→184 edges.
- Layer-1: `verify_citations.py` 0 errors (1 pre-existing `req_ctx` WARN on eviction-integration);
  `keg_lint.py --stats` 0 errors/0 warnings/0 orphans. **Layer-2 pending** for these 4 pages.
- Remaining: Tranches B (throttle), C (accounting plumbing + interfaces), D (decisions) — see index.md.

## [2026-06-08] fix | Layer-2 adversarial review of Tranche A (Smith predictor)
- Reviewer: fresh `valkey-dt-reviewer` subagent (batched grep strategy, this repo's code only).
- Result: **REVIEW PASS** on all 4 pages — eviction-integration, engine-integration, flows/spill,
  memory-accounting. All key claims verified against code: cap-less `spillFillToProjected`
  (`while projected > maxmemory`, `ext_storage.c:1012`); eviction override gates on
  `extStorageProjectedMemory()` (`:933`), hard cap on raw `used_memory` (`:940`); predictor
  formula + EMA α=1/16 (`:79-88`, `:68`); inflight debit (`:613`); IO-thread `objectComputeSize`
  (`storage_flashcache_real.c:197`); removed cap symbols (comment `:100-104`); dead
  `items_spillover_batch_size` (`:95` + `config.c:3426`); throttle band 1.1×–1.2×
  (`ext_storage_throttle.c:221-222`).
- 1 adjudicated citation fix: eviction-integration "nothing left to spill" `C_ERR` `:970-972`
  → `:969-970` (confirmed via `grep -n`: `*result = C_ERR` at 969, `return 1` at 970). Reviewer's
  finding was correct here (not a miscount).
- Layer-1 re-run: `verify_citations.py` 0 errors (pre-existing `req_ctx` WARN); `keg_lint.py --stats`
  0/0/0. Tranche A pages now Layer-1 + Layer-2 clean.

## [2026-06-08] ingest | Smith-predictor commit e4ed4ff3a (Tranche B — throttle decoupling)
- Source: `src/ext_storage_throttle.c` (commit `e4ed4ff3a` changed lines ~211-256; file now 319 lines).
- Updated: `components/throttle-equilibrium.md`; re-rendered `diagrams/throttle-feedback.{dot,png}`
  (native `dot`). `interfaces/throttle-api.md` left unchanged — header `ext_storage_throttle.h` was
  not touched by the commit.
- Substance: throttle reads **raw `used_memory`** (not projected); band moved from `[1.0×, 1.1×]`
  to `[1.1×, 1.2×]` — `throttle_start = maxmemory + maxmemory/10` (1.1×, `:221`),
  `throttle_max = maxmemory + maxmemory/5` (1.2×, == OOM hard cap, `:222`), linear between (`:230`).
  The `extStorageUpdateSpillConcurrency(rate)` spill coupling was **removed** (`:252-254`): throttle
  no longer drives spill concurrency. Spill owns `[1.0×, 1.1×]` on projected memory; throttle owns
  `[1.1×, 1.2×]` on raw. Coupling is implicit via memory (`used_memory − projected` = disk-sat backlog).
  Re-pinned shifted citations: shouldThrottle `:250-293`→`:257-302` (adjustRate `:262`, pending skip
  `:268-270`, throttle block `:289-297`); adjustRate `:188-247`→`:188-255`; allowed_tps `:239-243`→`:246-249`.
- Diagram: throttle-feedback.dot redrawn — removed the `extStorageUpdateSpillConcurrency` "speed up
  spills" edge; now shows spill controller (reads projected, drains to disk) and throttle controller
  (reads raw used_memory, slows clients) as two decoupled loops + an implicit-coupling dotted edge.
- Edges: +1 typed `refers_to` (throttle-equilibrium→memory-accounting). Graph 184→185.
- Layer-1: `verify_citations.py` throttle-equilibrium 0 errors; throttle-api 2 pre-existing partial-token
  WARNs (unchanged page); `keg_lint.py --stats` 0/0/0. **Layer-2 pending** for throttle-equilibrium.

## [2026-06-08] fix | Layer-2 adversarial review of Tranche B (throttle decoupling)
- Reviewer: fresh `valkey-dt-reviewer` subagent (batched grep strategy, this repo's code only).
- Page: `components/throttle-equilibrium.md`. Result: **REVIEW PASS**, no findings, no edits.
- Independently verified: throttle reads RAW `used_memory` (`ext_storage_throttle.c:220`); band
  `throttle_start` 1.1× (`:221`) / `throttle_max` 1.2× (`:222`) / linear (`:230`);
  `extStorageUpdateSpillConcurrency` has NO active call anywhere (only comment hits at throttle.c:254,
  ext_storage.c:903); all shouldThrottle (`:257-302`), adjustRate (`:188-255`), token-bucket, and
  timer sub-citations match exactly; `recordCommandLatency` `:169` fed by `server.c:4017`;
  `networking.c:4309` hook confirmed.
- Layer-1 unchanged (no edits): `keg_lint.py --stats` 0/0/0. Tranche B now Layer-1 + Layer-2 clean.

## [2026-06-08] ingest | Smith-predictor commit e4ed4ff3a (Tranche C — accounting plumbing + interfaces)
- Updated: `components/serialization.md`, `flows/completion-drain.md`, `interfaces/storagetype-vtable.md`,
  `interfaces/ext-storage-api.md`, `interfaces/info-metrics.md`, `components/bridge-layer.md`;
  re-rendered `diagrams/completion-drain-sequence.{mmd,png}`. `interfaces/bridge-api.md` unchanged
  (`ext_storage_bridge.h` not touched by the commit).
- Substance:
  - serialization: IO-thread serialize also computes `comp.ram_bytes = objectComputeSize(NULL, value, 5, db_id)`
    (`storage_flashcache_real.c:197`) → `extStorageInflightAddRam` (`:198`) + `extStorageOnSpillSerialize`
    (`:201`). New contradiction: NULL-key `objectComputeSize` would deref on an OBJ_MODULE spill candidate
    (spillItemAsync doesn't exclude OBJ_MODULE) — folded to known-limitations (Tranche D). All serialize-fn
    citations re-pinned (`ext_storage.c:218-261`); flashcache real ser/deser `:172/:183/:135`.
  - completion-drain: drain loop `:590-782`; WRITE `:610-676` debits `inflight_spill_ram_bytes -= msg->ram_bytes`
    (`:613`); READ `:677-741`; DELETE `:742-779`; unblock `:780`; pump → `spillFillToProjected` (`:1043-1049`).
  - storagetype-vtable: `storageCompletion.ram_bytes` added (`storage.h:52`); all post-line-52 citations +1
    (storageType `:76-110`, dispatch `:115-125`, getters `:128-131`, serialize `:134`).
  - ext-storage-api: `extStorageUpdateSpillConcurrency` removed from header; documented
    `extStorageOnSpillSubmit` (`:66`)/`OnSpillSerialize` (`:67`)/`InflightAddRam` (`:78`)/`ProjectedMemory`
    (`:79`); `extStorageGetState`… re-pinned to `ext_storage.c:160-189`; `items_spillover_batch_size` noted vestigial.
  - info-metrics: INFO fn `:1066-1145`; all field-group ranges re-pinned; NEW fields
    `inflight_spill_ram_bytes` + `projected_memory` (`:1135-1136`) documented.
  - bridge-layer: `msg->ram_bytes = c->ram_bytes` (`ext_storage_bridge.c:241`), GC branch sets 0 (`:215`);
    poll fn `:193-272`, key-free `:264`; file now 277 lines.
- Edges: +6 typed `refers_to` to memory-accounting/known-limitations (serialization, storagetype-vtable,
  ext-storage-api ×2, info-metrics, bridge-layer). Graph 185→191.
- Layer-1: `verify_citations.py` 0 errors (pre-existing partial-token WARNs only); `keg_lint.py --stats`
  0/0/0. **Layer-2 pending** for the 6 touched pages.

## [2026-06-08] fix | Layer-2 adversarial review of Tranche C (accounting plumbing + interfaces)
- Reviewer: fresh `valkey-dt-reviewer` subagent (batched grep strategy, this repo's code only).
- Result: REVIEW PASS on 5/6 — serialization, completion-drain, storagetype-vtable, ext-storage-api,
  bridge-layer. Independently verified the substantive claims: `objectComputeSize(NULL,…)` at
  `storage_flashcache_real.c:197`; WRITE-completion inflight debit `ext_storage.c:613`;
  `storageCompletion.ram_bytes` at `storage.h:52` and the +1 shift of all members;
  `extStorageUpdateSpillConcurrency` confirmed ABSENT from `ext_storage.h`; the new predictor hooks
  at `ext_storage.h:66-67,78-79`; `bridge.c:241` ram_bytes copy.
- 2 adjudicated citation fixes on `info-metrics.md` (both confirmed via `sed -n`): genExternalStorageInfoString
  span `:1066-1145` → `:1066-1148` (closing brace at 1148); module-metrics block `:1141-1144` → `:1141-1145`
  (`sdsfree` at 1145). Reviewer was correct on both (not miscounts).
- Layer-1 re-run: `verify_citations.py` info-metrics 0 errors/0 warnings; `keg_lint.py --stats` 0/0/0.
  Tranches A, B, C now all Layer-1 + Layer-2 clean.

## [2026-06-08] ingest | Smith-predictor commit e4ed4ff3a (Tranche D — decisions)
- Updated: `decisions/adr-index.md`, `decisions/known-limitations.md`.
- adr-index: ADR-008 reworded to throttle band `[1.1×,1.2×]` on raw used_memory (anchor
  `ext_storage_throttle.c:221-222` + hard cap `ext_storage.c:940-944`); **ADR-009 marked superseded**
  (dynamic concurrency removed); **new ADR-010** = cap-less projected-gated spill controller
  (`ext_storage.c:997-1033`, predictor `:79-88`); ADR-004/006 anchors `:169`→`:232`, `:749`→`:815`.
  Control note rewritten for decoupled controllers + Smith predictor.
- known-limitations: **C2 retired → R2** (SPILL_CONCURRENT macros + extStorageUpdateSpillConcurrency
  deleted, removal comment `:100-104`); **new C7** = dead `items_spillover_batch_size` config
  (`ext_storage.c:95`, `config.c:3426`); **new L4** = OBJ_MODULE `objectComputeSize(NULL,…)` deref
  risk (`storage_flashcache_real.c:191-201`); **new L5** = Smith-predictor cold-start/stale-mean
  under-braking (`ext_storage.c:1003-1011`). Re-pinned C3 `:275-526`→`:351-581` (cases `:389`),
  C4 `storage.h:129`→`:130`, L2 `:868`→`:926`/`:877-884`→`:940-944`/`:894-908`→`:955-964`, R1 `:169`→`:232`/`:749`→`:815`.
- Edges: +2 typed `refers_to` on adr-index (eviction-integration, memory-accounting). Graph 191→193.
- Layer-1: `verify_citations.py` both pages 0 errors/0 warnings; `keg_lint.py --stats` 0/0/0.
  **Layer-2 pending** for adr-index + known-limitations (synthesis pages — also want Layer-3 human).
- Smith-predictor re-ingest (Tranches A–D) now COMPLETE; A/B/C also Layer-2 clean.

## [2026-06-08] fix | Layer-2 adversarial review of Tranche D (decisions)
- Reviewer: fresh `valkey-dt-reviewer` subagent (batched grep strategy, this repo's code only).
- adr-index.md → REVIEW PASS (all ADR anchors verified, incl. ADR-008 band `throttle.c:221-222`,
  ADR-010 `spillFillToProjected` `:997-1033`/`:1012`, ADR-009 superseded — `extStorageUpdateSpillConcurrency`
  has no active def/call, only comments at `throttle.c:254`/`ext_storage.c:903`).
- known-limitations.md → 1 REAL ERROR fixed: L4 body cited `ext_storage.c:191-201` for the OBJ_MODULE
  `TODO(safety)`, but that note is in `storage_flashcache_real.c:191-201` (`ext_storage.c:191` is
  keyBlocksClient metrics). Corrected (frontmatter source was already right). All other entries
  (C1/C3/C4/C5/C6/C7, L1/L2/L3/L5, R1/R2) confirmed accurate. Reviewer's C5 `lib.rs:533-536` range nit
  judged trivial (cited lines do contain the key_may_exist ref + reason) — left as-is.
- Layer-1 re-run: `verify_citations.py` known-limitations 0 errors/0 warnings; `keg_lint.py --stats` 0/0/0.
- Smith-predictor re-ingest (commit e4ed4ff3a) COMPLETE and Layer-1+Layer-2 clean across Tranches A–D.
  Remaining: Layer-3 (human) on synthesis pages; optional controller_model_detailed diagram source.

## [2026-06-08] ingest | controller-model diagram (Smith predictor + two controllers)
- New: `diagrams/controller-model.dot` (+ rendered `.png`, native `dot`). Authored from this repo's
  code (NOT the user's pre-rendered PNG, which had no source): two decoupled controllers around the
  memory zone (spill owns 1.0–1.1× on projected; throttle owns 1.1–1.2× on raw; 1.2× OOM reject) plus
  the two-stage Smith predictor `projected = used_memory − P1 − P2` (P1 = submit_depth × mean_spill_ram,
  modeled; P2 = inflight_spill_ram_bytes, exact). Every label cites code (ext_storage.c:79-88, :997-1033,
  :613, :849; ext_storage_throttle.c:221-230; storage_flashcache_real.c:197).
- Embedded in `components/memory-accounting.md` (Smith-predictor section); registered in `index.md`
  diagrams table.
- Render note: initial `dot` run hit a pango markup error on a literal `<` in an HTML-table label
  (`&lt;` decoded then re-parsed as markup) — replaced with "under 1.0x"; re-rendered clean.
- Layer-1: `verify_citations.py` memory-accounting 0 errors/0 warnings; `keg_lint.py --stats` 0/0/0.

## [2026-06-10] fix | rebase conflict resolution + line-drift re-ground sweep
- Trigger: user rebased off upstream; a `git stash pop` left an unresolved conflict (`UU`) in
  `decisions/known-limitations.md` (no source files conflicted). The rebased code also shifted
  `ext_storage.c` line numbers non-uniformly and added INFO fields.
- Conflict resolution (union, per user direction): kept upstream's evidence-backed **L4**
  (embedded-values-unspillable + memory-gated throttle collapse; `fullrun-20260608` W2 benchmark)
  and **L5** (`spillFillToProjected` livelock; gdb backtrace) AND the session's OBJ_MODULE
  NULL-deref → renumbered **L6**, cold-start under-braking → **L7** (cross-linked to L5). Unioned the
  `sources:` list; re-grounded all citations. C2 stays retired→R2; C7 (dead config) retained.
  Empirical benchmark/gdb claims preserved as authored, flagged as not code-derivable.
- Line-drift re-ground sweep (7 pages): known-limitations, engine-integration, eviction-integration,
  flows/spill, flows/completion-drain, serialization, memory-accounting, info-metrics. Key new anchors
  (`grep -n`): createDumpPayload `:240`, extStorageSerializeKey `:226`, keyBlocksClient `:359-434`,
  preCommandExec `:444-593`, processCompletedStorageRequests `:602-794` (WRITE `:622-688`, debit `:625`,
  READ `:689-753`, DELETE `:754-791`, unblock `:792`), spillItemAsync `:808-868` (submit hook `:861`),
  extStorageEvictFlashKey `:880-912`, extStoragePerformEvictions `:938-983` (gate `:945`, hard cap `:952`,
  key floor `:973`), spillFillToProjected `:1009-1046` (while `:1024`), pumps `:1055-1061`/`:1066-1071`,
  1ms timer `:311`, INFO fn `:1078-1168`. info-metrics also gained 3 new predictor fields
  (`spill_submitted_count`/`spill_serialized_count`/`mean_spill_ram`, `:1149-1151`).
- No change needed: `throttle-equilibrium`, `bridge-layer`, `ext-storage-api`, `storagetype-vtable`
  (ext_storage_throttle.c / ext_storage_bridge.c / ext_storage.h / storage.h untouched by the rebase).
- Layer-1: `verify_citations.py` 0 errors; `keg_lint.py --stats` 0/0/0 (193 edges). **Layer-2 pending.**

## [2026-06-10] fix | Layer-2 adversarial review of rebase re-grounded pages
- Two parallel fresh `valkey-dt-reviewer` subagents (4 pages each, batched grep). All 8 pages
  **REVIEW PASS**: known-limitations, engine-integration, eviction-integration, spill (group 1);
  completion-drain, serialization, memory-accounting, info-metrics (group 2). Post-rebase line
  numbers verified accurate across the board.
- Adjudicated fixes (confirmed via `grep -n`):
  1. **info-metrics** — real omission: `dram_value_hits` INFO field (`ext_storage.c:1109`, backing
     var `:208`, counted `:385`/`:415`, ratio comment `:207`) was undocumented (new upstream metric
     between the spill and throttle groups). Added a "DRAM hit counter" section + sources.
  2. **completion-drain** — tightened 3 case ranges to the actual case-body braces: WRITE
     `:622-688`→`:622-686`, READ `:689-753`→`:689-751`, DELETE `:754-791`→`:754-785` (the old DELETE
     range had swallowed the `default:` case).
  3. **spill** — PENDING_EVICT `:635-646`→`:635-647` (include the `break;`).
- Cosmetic notes left as-is: memory-accounting `object.c:1202-1212` (reviewer agreed reasonable).
- Layer-1 re-run: `verify_citations.py` 0 errors (1 pre-existing `blockedBeforeSleep` WARN);
  `keg_lint.py --stats` 0/0/0 (193 edges). Rebase re-ground is now Layer-1 + Layer-2 clean.

## [2026-06-17] ingest | Tiering integration tests → new components/testing.md
- New: [testing](components/testing.md) (draft) — maps tiering behavior to the Tcl tests in
  `tests/unit/`: ext-storage-data-types.tcl (42), ext-storage-persistence.tcl (2),
  ext-storage.tcl (12), and the introspection.tcl skip_configs change.
- Coverage matrices: spill/fetch round-trip per data type × encoding (string/hash/list/set/
  zset/stream; listpack/hashtable/quicklist/intset/skiplist) and key-lifecycle (TTL, DEL,
  UNLINK, RENAME, DUMP/RESTORE, TYPE, SCAN, MGET, SUNIONSTORE, OBJECT ENCODING, overwrite,
  pressure-driven spill, stream consumer groups). All cited by test line number.
- Cited the staged crash-prevention guards: rdb.c:1195 (skip tiered in rdbSaveKeyValuePair),
  rdb.c:1454 (dismiss only non-tiered), aof.c:2441/2492 (serverAssert on tiered, TODO),
  lazyfree.c:141 (early-return on tiered). storage_flashcache.c:106-184 (mock ser/deser).
- Updated: [known-limitations](decisions/known-limitations.md) — L1 now links the failing
  AOF-reload regression marker; added inbound refers_to edge → testing.md. index.md registry.
- Contradictions found (NOT yet folded into known-limitations — pending direction):
  (1) ext-storage.tcl loads a non-existent `modules/key-spilling/` backend so it always SKIPs;
  (2) "Tiered keys persist across AOF reload" asserts behavior the code (rdb.c:1195) drops.
- Layer-1: keg_lint --stats and verify_citations re-run (see report). Layer-2 pending.

## [2026-06-25] fix | re-ground testing.md against updated ext-storage tcl suite

- Reconciled [testing](components/testing.md) with the tcl suite after the 2026-06-23 upstream
  port grew ext-storage-data-types.tcl from 42 → 53 tests (file 661 → 795 lines) and helper/
  env-backend additions shifted ext-storage-persistence.tcl (91 → 107 lines).
- Test count: 42 → 53 in the Test-files table; sources ranges bumped
  (data-types :1-661→:1-795, persistence :1-91→:1-107; ext-storage.tcl :1-153 unchanged).
- Added a new "scripting, transactions & extra commands" coverage matrix documenting the 11
  appended tests (all line numbers authored from `grep -n`):
  Lua EVAL 665/672/686/697, Lua INCRBY 710; MULTI/EXEC 731/741, WATCH 752;
  COPY 771, APPEND 780, PERSIST 788.
- Corrected stale citations: persistence BGREWRITEAOF 42→58, AOF-reload 66→82.
- Corrected false claim: line 483 is no longer "the only" pressure-driven (non-DEBUG-SPILL)
  spill test — the Lua INCRBY test (710) also spills via natural LRU pressure.
- Fixed ext-storage.tcl contradiction note's in-tree module list: only modules/rocksdb-tiering/
  and modules/valkeymodule-rs/ exist; flashcache/rocksdb backends are compiled into src/storage/
  (storage_flashcache.c, storage_flashcache_real.c, storage_rocksdb.c, storage_rocksdb_async.c),
  not loadable modules. SKIP behavior (modules/key-spilling/ absent) re-verified, still holds.
- Metadata: updated 2026-06-17→2026-06-25, claim_count 56→67. index.md description enriched.
- Note: verify_citations symbol grounding only scans cited C sources, not .tcl — dropped
  backticks on INCRBY/WATCH (grounded at data-types.tcl:721/:752) to avoid heuristic false
  positives.
- Layer-1: verify_citations [OK] components/testing.md (0 err/0 warn); keg_lint --stats
  pages=26 errors=0 warnings=0 orphans=0. Layer-2 (adversarial review) pending direction.

## [2026-06-25] fix | Layer-2 review + re-ground testing.md (53 → 60 tests)

- Ran Layer-2 adversarial review (valkey-dt-reviewer) on components/testing.md. Reviewer
  flagged one ERROR: data-types test count stated 53 but `grep -c` reported 60.
- Adjudicated against `grep -n`/`wc -l`: CONFIRMED — the file changed since the 19:00 pass
  (795→880 lines). 7 tests were appended after line 788 under a "MISSING FROM UPSTREAM BASH
  MIGRATION" banner: MSET 800, RANDOMKEY 811, DBSIZE 816, Lua conditional 821, Lua loop 836,
  Lua pcall 855, MULTI/EXEC APPEND 870 (all line numbers from `grep -n`).
- Re-grounded: count 53→60; sources data-types :1-795→:1-880; added a "command-surface & Lua
  edge cases (bash-migration backfill)" coverage matrix for the 7 tests; claim_count 67→74.
- Reviewer WARN (introspection 1274–1284 imprecise): adjudicated — the 4 ext-storage configs
  are exactly at lines 1277–1280 (`grep -n`). Tightened both the sources range and prose to
  :1277-1280.
- Reviewer re-verified (all OK) the C-file citations (rdb.c:1195/1454, aof.c:2441/2492,
  lazyfree.c:141, storage_flashcache.c:106-184), the two contradiction notes, and the prose
  behavior claims (WATCH abort, PERSIST TTL=-1, COPY source-survives).
- Layer-1: verify_citations [OK] (0 err/0 warn); keg_lint pages=26 errors=0 warnings=0 orphans=0.

## [2026-06-26] fix | Layer-2 confirm on the 7 backfill tests

- Focused Layer-2 (valkey-dt-reviewer) on the bash-migration backfill section of
  components/testing.md. Verified state stable first: data-types.tcl still 880 lines / 60 tests,
  backfill blocks at 800/811/816/821/836/855/870 (`grep -n`).
- Reviewer verdict on the 7 tests: all line numbers correct, all "What it asserts" prose matches
  the test bodies (MSET 3-key verbatim, RANDOMKEY non-empty, DBSIZE>0, Lua if/else branch,
  Lua loop sum=60, Lua pcall caught-error, MULTI/EXEC APPEND result), and the "MISSING FROM
  UPSTREAM BASH MIGRATION" banner exists at source line 797.
- One valid WARN: "All spill via DEBUG SPILL" was overstated — RANDOMKEY (811) and DBSIZE (816)
  call no debug_spill; they are keyspace-introspection checks riding on the preceding MSET test's
  spilled keys. Adjudicated against the code and corrected the framing sentence.
- Layer-1: verify_citations [OK] (0 err/0 warn); keg_lint pages=26 errors=0 warnings=0 orphans=0.

## [2026-06-26] fix | promote testing.md draft → active

- Promoted components/testing.md from draft to active after it cleared Layer-1 and three
  Layer-2 adversarial passes (full citation re-ground, 53→60 count correction, and the focused
  7-backfill-test confirm). Updated status in the page frontmatter and the index.md registry row.
- Layer-1 re-verified post-promotion: verify_citations [OK] (0 err/0 warn); keg_lint pages=26
  errors=0 warnings=0 orphans=0.

## [2026-07-30] tooling | reading view under docs/ + legacy moniker dropped

- Added a reading view at `docs/`: hierarchical table of contents over 6 parts / 31
  chapters / 189 sections, a page per part, and a chapter page with an in-page section
  ToC, numbered sections with anchor handles, breadcrumbs and Prev/Up/Next (also bound to
  left/right/u). The KEG viewer keeps the node view; the two cross-link.
- It is three static files (`docs/index.html`, `docs/app.js`, `docs/style.css`) that
  assemble the view from this wiki's markdown when the page is opened: hierarchy,
  summaries and status from the curated tables in `index.md`; titles/status/tier from each
  page's front matter; section ToCs, numbering and anchors from each page's H2/H3
  headings; prose via the `marked.js` vendored under `keg/viewer/vendor/`. `index.md` is
  the manifest -- adding a row to one of its tables is all it takes for a page to appear.
  The only structure held outside the wiki content is `PARTS` at the top of `app.js`.
- Routes are hash-based (`docs/#/state-machine`, `docs/#/<page>/<section>`) because GitHub
  Pages cannot rewrite paths. Cross-page `.md` links and `#fragments` in the rendered
  prose are rewritten to routes; diagrams and non-page assets resolve to their real paths.
- Dropped the legacy three-letter moniker from every title, heading, front-matter
  `title:`, blurb, table summary, diagram comment, tool docstring and both viewers. Scope
  is now stated as v1 -- values spill, keys stay in the dict, key spilling a possible
  future config -- in `00-overview.md`, `WIKI.md` § Scope and `AGENTS.md`; ADR-001 is
  titled "Values-only spill (v1)". Code identifiers are untouched
  (`modules/non-key-spilling/...` paths, the crate/module name, the `storageType` name,
  and the verbatim `server.h:779` comment quote) -- rewriting them would falsify the
  citations.
- Verified across the corpus: every chapter file present and summarised, 189 ToC sections
  against 212 rendered H2/H3 headings with 0 unreachable anchors, 0 duplicate route slugs,
  0 dead `#fragments`, 308/312 intra-wiki `.md` links resolving (the 4 misses are the
  illustrative `rel.md` placeholders in `WIKI.md`/`AGENTS.md`). Deep-link scrolling
  confirmed in-browser. `.md` files are served raw as `text/markdown` on Pages thanks to
  the repo-root `.nojekyll`. `state-machine.dot`'s digraph identifier was renamed and its
  render confirmed byte-identical, so the committed PNG stays valid. keg_lint: pages=26,
  0 errors / 0 warnings / 0 orphans.

## [2026-07-30] fix | re-grounded citations on the current tree — 42 errors → 0

- `verify_citations` had been red for a while: 42 errors, all in four pages, all one root
  cause. The pages were grounded on an older layout in which the storage backends were C
  modules under `modules/storage_example/` and `modules/storage_flashcache_module/`, the
  Rust module lived at `modules/non-key-spilling/`, and the tcl suite sat directly in
  `tests/unit/`. None of those paths exist any more. The architecture the pages describe —
  `storageType` vtable, bridge, middleware, mock and real-flashcache backends — is intact;
  only the file layout moved.
- Established mapping (each confirmed by reading the code, not assumed): the Rust module
  is now `modules/flash-tiering/` with the same internal shape (`src/lib.rs`,
  `src/dispatcher.rs`, `src/callbacks.rs`, `src/backends/{rocksdb,flashcache}/backend.rs`);
  the C backends moved into the engine as `src/storage/storage_mock.c` and
  `src/storage/storage_flashcache_real.c`; the tcl suite is now `tests/unit/data-tiering/`.
  Every citation was re-pointed **and its line numbers re-read from the current file** —
  no old range was carried across to a new path.
- Corrections to claims, not just paths:
  - `interfaces/storagetype-vtable.md` listed a getter `storageGetRocksDBType` ("RocksDB,
    sync via middleware") that exists nowhere in the tree. There is no engine-native
    RocksDB backend at all: `storageGetRocksDBAsyncType` is an **alias returning the same
    in-memory mock struct** (`src/storage/storage_mock.c:418`), whose own comment says the
    `"rocksdb"` config value reuses the mock "to avoid duplicating 300 lines of identical
    hashtable code". Real RocksDB is a module, not an engine backend.
  - `components/backends.md`: the claimed mock **fallback** does not exist — a failed
    backend does not silently degrade to the mock.
  - C5 in `decisions/known-limitations.md` still holds: `key_may_exist` is implemented by
    both flash-tiering backends but its registration is commented out
    (`modules/flash-tiering/src/lib.rs:552-559`) with the reason intact in the code —
    "Registering it causes false positives that block clients forever" — and
    `src/module.c:945` conservatively returns "may exist" when no callback is registered.
  - `components/testing.md`: the suite grew from the 3 cited files to **19 files / 225 test
    blocks**; every per-file count in the coverage table was re-derived from the tcl
    sources. `ext-storage.tcl` (12 tests) is documented as **never running** in practice
    because it needs a module path that is absent.
- **Citation hazard worth knowing:** two headers named `storage.h` exist —
  `src/storage/storage.h` (the compiled one) and an older, larger, **uncompiled**
  `src/storage.h` that nothing includes. A bare `storage.h:NN` or `:NN` citation resolves
  to the wrong file, so this interface must always be cited by full path.
- Flagged, not fixed: neither Rust module mirrors four of the vtable's fields; and the two
  `objectIsTiered` skip sites in `src/aof.c` carry each other's log wording.
- Result: `verify_citations` pages=26 **errors=0**, warnings 121 → 73. keg_lint unchanged
  at 0 errors / 0 warnings / 0 orphans.

## [2026-07-30] fix | re-grounded the persistence story — it had inverted under us

- The citation pass exposed a worse problem than stale paths: the engine gained fork-based
  snapshot persistence, and three pages still asserted the opposite. `verify_citations` was
  **green throughout** — every citation resolved to a real line; the prose simply said the
  reverse of what those lines do. A clean checker run proves nothing about accuracy.
- Corrected story, all re-read against the tree: `rdbSaveKeyValuePair` materializes a tiered
  value via `extStorageMaterializeTiered` and emits a **standard RDB entry** — "loadable by any
  node, tiered or not" — so RDB files, the AOF-preamble base (default) and disk-target replica
  full sync all carry tiered data. `return 0` survives only as the skip for a logically-gone
  key (`PENDING_DELETION` or GC-evicted). The fork-snapshot protocol (settle to no `COPYING_*`
  key → park the backend IO thread → pause GC → fork → resume → done) is what makes it
  consistent, and it is wired into foreground SAVE, BGSAVE and background AOF rewrite.
- `known-limitations` restructured rather than trimmed: **L1** narrowed to what is still true
  (non-preamble AOF, slot migration, diskless fork, tiering placement), the resolved bulk moved
  to **R3** with a proof table, former C6 moved to **R4**, and two new entries added — **L8**
  (with a non-snapshotting backend, tiered data disables persistence *entirely*: the save paths
  fail closed rather than write a lossy snapshot) and **C8** (see below). Nothing was deleted.
- **Engine finding, flagged not demonstrated:** `rdbSaveToReplicasSockets` (`rdb.c:3845`) — the
  **default** replication path, `repl-diskless-sync` defaults to yes — forks and materializes
  tiered values in the child but is the only fork/save entry point that never calls
  `extStorageSnapshotPrepare()`, and it carries no fail-closed guard either. So it can fork with
  keys in `COPYING_*`, the IO thread unparked and GC running: exactly the three conditions the
  protocol exists to exclude. Likely symptom is silently skipped keys (`forkRead` returns -1 on
  a miss rather than faulting) rather than a crash — but that is inference. No test covers a
  replica full sync with tiered values. Needs a maintainer decision on whether it is deliberate.
- **C8, a real source bug found by reading past the log text:** the two tiered-skip warnings in
  `src/aof.c` name each other's path. Line 2446 sits in `rewriteSlotToAppendOnlyFileRio` (slot
  migration) but logs "AOF rewrite (non-preamble)"; line 2504 sits in `rewriteAppendOnlyFileRio`
  but logs "Slot snapshot". Anything that trusts the message — including an earlier note in this
  log — attributes both skips to the wrong path. Assign these sites by enclosing function.
- `index.md`: the persistence registry row and the known-limitations counts (5 contradictions /
  8 limits / 4 resolved) were corrected. Dated session notes further down are left as record.
- Result: `verify_citations` pages=26 errors=0, warnings 73 → 67. keg_lint 0/0/0.

## [2026-07-30] tooling+triage | staleness check now uses git dates; the 52 STALE warnings are real

- `verify_citations` computed staleness from filesystem **mtime**, which in a fresh clone or
  after a branch checkout is the checkout time for every file — so the reported dates were
  meaningless (everything read "mtime 2026-07-30"). It now asks git for each cited file's last
  commit date (`source_change_date()`, falling back to mtime for untracked files or when git is
  unavailable).
- Worth recording honestly: I expected this to eliminate most of the 52 STALE warnings as clone
  artifacts. **It did not.** The identical 52 warnings fire, now with true dates — the cited
  sources really did change (2026-07-01, 07-22, 07-23) after these pages were last verified
  (2026-06-03 … 06-10). The fix corrected the *dates*, not the count; its value is accuracy plus
  removing a latent false-positive source.
- What the 52 actually point at: three feature commits landed on this branch after the pages were
  verified, and only one has been reconciled.
  - `bb442bcff` fork-based RDB snapshotting (07-23) — **reconciled** today, see the persistence
    entry above.
  - `6976634d2` module correctness / DEL semantics / mid-execution key access (07-22) — **not
    reconciled**.
  - `b36f2d1a1` SWAPDB support, pending-DEL guard, runtime policy guard (07-23) — **not
    reconciled**.
- Three concrete defects already confirmed from the unreconciled pair, each traced to code:
  1. `components/state-machine.md` says "each key is in one of **5** states" and documents five.
     The enum has **six**: `TIERING_STATE_PENDING_DELETION = 5` (`src/ext_storage.h`) is absent
     from the page's state table, transition table and blocking matrix.
  2. The SWAPDB physical-db-id indirection (`extStoragePhysicalDbId`, used in `src/db.c`,
     `src/expire.c`) appears in **no** component page — only as a test name in `testing.md`.
  3. Mid-execution synchronous fetch (`extStorageSyncFetch`, `src/db.c`) is likewise
     undocumented outside a test name.
- The remaining 15 warnings are the softer "symbol not in cited sources" class, and most are
  benign: command names used as prose (`DBSIZE`, `EXISTS`, `EXPIRE`), module/file basenames
  (`ext_storage_bridge`, `ext_storage_throttle`), and engine symbols a page mentions without
  citing (`processCommand`, `blockedBeforeSleep`, `storageConfig`, `req_ctx`, `capacity_bytes`).
  Padding `sources:` to silence them would game the check; they should be fixed only where the
  page genuinely relies on the symbol.
- Deliberately **not** done: bumping any page's `updated:` date. That would silence a real
  freshness signal while verifying nothing.

## [2026-07-30] ingest | reconciled the two outstanding feature commits (7 pages)

- Reconciled `6976634d2` (module correctness / DEL semantics / mid-execution key access, 07-22)
  and `b36f2d1a1` (SWAPDB, pending-DEL guard, runtime policy guard, 07-23) across
  state-machine, delete, fetch, engine-integration, ext-storage-api, eviction-integration and
  config-and-module-args. Warnings 67 → 49; errors stay 0; graph edges 203 → 215 from the new
  cross-references.
- **Sixth state documented.** `TIERING_STATE_PENDING_DELETION = 5` was missing from the state
  table, transitions and blocking matrix; the page claimed "one of 5 states". PENDING_DELETION
  is now distinguished from PENDING_EVICT (4) explicitly: the flash copy is *already* deleted
  for a client `DEL`/`UNLINK`, and the TIERED placeholder is deliberately retained so the
  re-executed command performs the keyspace removal with full command-layer side effects.
- **SWAPDB logical/physical db-id indirection** documented for the first time. The backend is
  keyed by a physical id that `SWAPDB` remaps (`extStorageSwapDbIds`, `ext_storage.c:352-360`,
  from `db.c:1931`), so after a swap logical db N is *not* backend namespace N. Getting the
  direction wrong does not fault — it silently reads or writes another database's keyspace.
  Both helpers are identity while the maps are unallocated (`ext_storage.c:343`, `:348`), so
  calls are safe pre-init. The complete translating-site inventory is on the page, and it
  includes the one deliberate **non**-translating call: the `READ_RETRY` resubmit at
  `ext_storage.c:934` passes `msg->db_id` straight through because inside
  `processOneCompletion` it is already physical — translating there would be the bug. Verified
  independently against every `extStorageBridge_submit*` / `forkRead` / `flushDB` call site.
- **Mid-execution synchronous fetch** (`extStorageSyncFetch`, `ext_storage.c:1078`, called from
  `db.c:95`) documented as a second fetch mode alongside block-and-retry, with its drain
  ordering rule (sync-fetch-deferred completions run first, `ext_storage.c:1040`) and its stall
  warning (`:1143`). Motivating cases: SORT BY/GET patterns and Lua undeclared keys.
- **Runtime `maxmemory-policy` guard** documented: `updateMaxmemoryPolicy`
  (`config.c:2620-2635`) restricts the policy to `allkeys-lru`, `allkeys-lfu` or `noeviction`
  while tiering is active, closing a runtime `CONFIG SET` path that the comment says
  "previously bypassed the check silently"; the same rule at init disables tiering instead.
- **Two stale source comments flagged, not fixed** (the wiki does not edit `src/`):
  `ext_storage.h:21-22` still says "one of 5 states" and still refers to the
  `keys_tiering_state` hashtable, which was removed in favour of the `robj->tiering_state`
  bitfield (`server.h:913`, `server.c:2942` both say so).
- Remaining 49 warnings: 13 of the soft "symbol not in cited sources" class, and 36 STALE on
  pages this pass did not touch — most concentrated in `adr-index` (9), `memory-accounting` (4)
  and `01-architecture` (4). No page's `updated:` was bumped except where an agent personally
  re-verified the content.
