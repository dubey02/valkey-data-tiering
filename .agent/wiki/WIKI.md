# WIKI.md — Schema & Maintenance Guide

This is the **schema layer** of an LLM-maintained wiki for Valkey **data tiering**. It tells any LLM agent how this wiki is structured and how to
keep it current. Read this file first in every session before editing the wiki.

Pattern: [karpathy/llm-wiki](https://gist.github.com/karpathy/442a6bf555914893e9891c11519de94f).

## Start here (every session)

1. **Health check (Layer-1 gate):** `python3 .agent/wiki/keg/keg_lint.py --stats` (graph) **and**
   `python3 .agent/wiki/keg/verify_citations.py` (citations + symbol grounding) — both expect
   0 errors. See § Verification (3 layers).
2. **Current state:** read `index.md` for the page catalog + per-page `status`
   (`stub`/`draft`/`active`), and `tail .agent/wiki/log.md` for recent activity.
3. **What to do next:** most subsystems are now `active`; the outstanding `draft`/`stub`
   pages and known follow-ups are listed in `index.md` § Remaining work. Promote them via the
   **Ingest** workflow + **Verification (3 layers)** below (highest-value order:
   engine-integration + core flows → storage API/vtable → throttle/eviction → serialization →
   interfaces → backends → decisions).
4. **Explore by relationship, not just filename:**
   `python3 .agent/wiki/keg/query.py related <page> --max-hops 2` (see `keg/RETRIEVAL.md`),
   or browse the graph + read pages in the viewer:
   `python3 -m http.server 8000 --bind 127.0.0.1 -d .agent/wiki`
   → `http://localhost:8000/keg/viewer/index.html`.

## Three layers

1. **Raw sources (immutable, never edited by the wiki):**
   - Engine code: `src/ext_storage.c`, `src/ext_storage.h`, `src/ext_storage_bridge.{c,h}`,
     `src/ext_storage_throttle.{c,h}`, `src/storage/storage.h`, and tiering touch-points in
     `src/evict.c`, `src/object.c`, `src/rdb.c`, `src/defrag.c`, `src/blocked.c`,
     `src/db.c`, `src/server.{c,h}`.
   - Backends/modules: `modules/non-key-spilling/`, `modules/storage_flashcache_module/`,
     `modules/storage_example/`, `src/storage/`.
   - Legacy notes (predecessor prose, treated as source, may contradict code):
     `DATA-TIERING.md`, `context/*.md`, `.agent/knowledge/*.md`, `design-docs/data-tiering/`.
2. **The wiki (this directory, LLM-owned):** interlinked markdown + regenerable diagrams.
3. **The schema (this file):** conventions + workflows below.

## Scope

- **v1 scope: values only.** Keys always stay in the dict; only *values* spill to flash.
  Key spilling is a possible future config and is out of scope for these pages.
- **Ignore key-spilling entirely.** Do not document, compare against, or reference the
  key-spilling / DT branch. If a legacy source describes key-spilling, extract only the
  in-scope parts.

## Directory structure

```
.agent/wiki/
  WIKI.md            # this file — schema
  index.md           # catalog: every page, one-line summary, by category
  log.md             # append-only timeline: ## [date] ingest|query|lint | <what>
  00-overview.md     # L0 — what data tiering is, mental model
  01-architecture.md # L1 — system context, threads, event-loop integration
  components/        # L2 — one page per subsystem
  interfaces/        # L3 — precise API reference, 1:1 with headers
  flows/             # L3/L4 — step-by-step sequences
  decisions/         # L4 — ADRs, known limitations
  diagrams/          # .dot + .mmd sources, Makefile, rendered .png
```

## Page format

Every page starts with YAML frontmatter, then a body. Keep it terse and factual.

```markdown
---
title: <Page Title>
status: stub | draft | active | stale
sources:
  - src/ext_storage.c:880-960
  - context/state-machine-design.md
updated: 2026-06-03
---

# <Page Title>

> One-sentence scope.

<body — sections, tables, [wikilinks](wikilinks.md), embedded diagrams>
```

- **status:** `stub` = placeholder only; `draft` = written, unverified; `active` =
  written and verified against code; `stale` = code moved past it, needs re-ingest.
- **sources:** every non-trivial claim must trace to a raw source. Prefer `file:line`
  ranges for code. This is what makes the wiki verifiable.
- **Links:** use `[path](relative/path.md)` style, e.g. `[state-machine](components/state-machine.md)`. Always
  cross-link related pages. No orphans.
- **Diagrams:** embed rendered PNGs with `![alt](diagrams/<name>.png)`. Never hand-draw
  ASCII where a diagram source exists — edit the source and re-render.

## Edges & Graph (KEG)

The wiki is a typed graph. Plain markdown links (standard `[text](rel.md)`,
**not** `[[wikilinks]]`) are the soft fallback; real relationships are declared as
typed edges in frontmatter. Tooling lives in `keg/` (ported from
ElastiCacheServerlessPEBrain). Schema reference: `keg/RETRIEVAL.md`.

Node fields (optional) + edges block:

```yaml
type: overview | component | interface | flow | decision   # = wiki directory; drives viewer shape/colour
tier: fact | working | wisdom               # fact=immutable, working=active, wisdom=synthesis
claim_count: <int>                          # node radius in the viewer
edges:
  - to: components/pluggable-storage-api.md  # wiki-root-relative (or ../ explicit)
    kind: depends_on
    source: human            # human | structural | llm_relation
    needs_review: false
    created: 2026-06-03
    note: optional one-liner
```

### Edge kinds (weights drive `query.py related` ranking)

| Kind | Wt | Use |
|------|----|-----|
| `depends_on` | 1.0 | A's behaviour requires B |
| `calls` | 1.0 | A invokes/dispatches to B (callgraph) |
| `implements` | 1.0 | component realises an interface |
| `contains` | 0.9 | composition (architecture → component) |
| `supersedes` | 0.9 | new → old (`status: superseded`) |
| `contradicts` | 0.9 | **symmetric** — both pages MUST link back |
| `configures` | 0.7 | config/arg drives a component |
| `refers_to` | 0.3 | soft see-also; migration fallback |

`cites`, `owns`, `contributed_to`, `blocks`, `resolves`, `mitigates` exist in the
vocabulary (inherited) but are unused here. Code files are **not** graph nodes —
keep page→code links in `sources:` frontmatter, not `edges:`.

### Tooling (run from the package root)

```bash
python3 .agent/wiki/keg/keg_lint.py --stats                              # integrity check (CI gate: exit 1 on error)
python3 .agent/wiki/keg/keg_lint.py --json --out .agent/wiki/keg/viewer/edges.json   # regen graph data
python3 .agent/wiki/keg/verify_citations.py [pages...]                   # Layer-1: citation range + symbol grounding (exit 1 on error)
python3 .agent/wiki/keg/migrate_all.py                                  # bulk: body links → refers_to edges
python3 .agent/wiki/keg/query.py related <page> --max-hops 2            # graph-aware retrieval (see RETRIEVAL.md)
python3 .agent/wiki/keg/extract_relations.py                            # validation cross-check (proposes upgrades)
python3 -m http.server 8000 --bind 127.0.0.1 -d .agent/wiki  # interactive Cytoscape graph
```

### Migration policy & lint

- New pages SHOULD ship with `edges:`. Any page **touched** during ingest MUST be
  migrated to typed edges in the same pass; untouched pages fall back to `refers_to`
  via `migrate_all.py`.
- `keg_lint.py` errors on: broken edge targets, invalid kinds, asymmetric
  `contradicts`. Warns on: untyped body links, `supersedes` whose target isn't
  `superseded`, orphans (migrated page, no inbound).
- Use `extract_relations.py` (regex, ~50% precision — fine at this scale) as a second
  pair of eyes on your edge authoring; triage with `query.py review`.

## Diagrams — regeneration

All diagrams are generated from text sources in `diagrams/`, never drawn by hand.

- **Structural / state / flow graphs:** Graphviz `.dot` → `.png` (`dot -Tpng`).
- **Sequence diagrams:** Mermaid `.mmd` → `.png` (`mmdc -i x.mmd -o x.png`).
- **Whole-wiki relationship graph:** the typed-edge graph — interactive Cytoscape
  viewer (`keg/viewer/`), regenerated via `keg_lint.py --json`. See Edges & Graph (KEG).

Regenerate everything: `cd .agent/wiki/diagrams && make`. After editing any source, re-run
`make` and confirm the PNG changed. The `.png` files are committed so the wiki renders
without a toolchain present.

## Workflows

### Ingest (a source file → wiki)
0. **Discuss** 3–6 key takeaways with the user before writing, unless told "silent".
1. Read the raw source (code or legacy note) fully.
2. Identify every component / interface / flow page it affects (one source → several pages).
3. Update them **in one pass** — add `file:line` citations, bump `updated`/`status`, and add
   typed `edges:` for any new relationships (see Migration policy above).
4. If a source **contradicts** the wiki, flag it inline (`> ⚠️ CONTRADICTION: <note>`) and
   prefer the code. Known cases: [known-limitations](decisions/known-limitations.md).
5. Update `index.md`, then append a grep-parseable log entry:
   `## [YYYY-MM-DD] ingest | <source>` then `- New: <pages>  Updated: <pages>  Notes: <one line>`.

### Query (answer a question from the wiki)
1. **Pick the retrieval channel for the question shape:** filename / `index.md` (you know the
   page), full-text grep (a literal phrase or identifier), tag, or **graph** (`keg/query.py` —
   for relationship questions: depends-on, contradicts, path, hubs; see `keg/RETRIEVAL.md`).
2. Open the pages and answer with citations (`page` + `file:line`).
3. File durable answers back as a new page or section; update `index.md` + `log.md`.

### Lint (health check)
1. Run `keg/keg_lint.py --report`, and scan for: contradictions, stale claims vs current code,
   orphans, concepts mentioned ≥3× without a page, missing cross-references, broken diagram embeds.
2. **Present findings as a numbered list and ask which to address — do not auto-fix.**
3. After approved fixes, append a `## [YYYY-MM-DD] lint | …` entry to `log.md`.

## Verification (3 layers)

Before a page goes `active`, its claims must be verified against the code. Citations are
load-bearing, not decorative — tiering code changes often (state machine, throttle, spill loop),
so open every cited `file:line` range and confirm it still says what the page claims.

- **Layer 1 — mechanical (gate).** `keg/verify_citations.py [pages...]` checks every `file:line`
  citation resolves and is in-bounds, flags stale sources (source mtime > page `updated`), and
  grounds every distinctive inline-code symbol against `src/`+`modules/` — NOT FOUND anywhere =
  ERROR (fabricated / wrong-codebase), found but not in the page's cited sources = WARN. Exit 1
  on any ERROR. Run it with `keg_lint.py --stats` as the standing gate (both 0 errors).
- **Layer 2 — fresh-agent adversarial review.** Spawn a subagent per page (MeshClaw `spawn_run`)
  that uses ONLY this repo's code, no prior tiering knowledge, and reports claims unsupported by
  the cited lines, fabricated symbols, and silently-resolved source contradictions. Hard-won rules:
    - **Adjudicate every line-number finding against `grep -n` before editing.** LLM reviewers
      systematically miscount lines from raw content (uniform off-by-one); the page is usually
      right and the reviewer is the one miscounting.
    - Spawn with `max_turns >= 60` and a **batched** grep strategy (a few `grep -n`/`sed -n`
      passes, then verify from the output) — per-claim grepping exhausts the 30-turn default.
    - If the subagent runner is down, do Layer 2 **inline**: re-read each cited range with
      `cat -n`/`grep -n` and adjudicate yourself.
- **Layer 3 — human.** Domain expert reviews the synthesis/interpretive pages (overview,
  architecture, flows, decisions), focused by what Layer 2 flagged.

**Authoring rule:** write citations from `grep -n` output, never by counting. On a page with
**multiple** code sources, always use an explicit `file.c:line` — a bare `:NN` resolves against
the page's *primary* (first) source and silently mis-points (the most common defect found). Skip
asset/doc paths (`.png`/`.mmd`/`.md`) from symbol concerns. Re-run Layer 1 after any edit.
