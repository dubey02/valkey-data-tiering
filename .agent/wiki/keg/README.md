# KEG — KiRoom Entity Graph (tooling)

Typed-edge graph tooling for this wiki. The schema is markdown-frontmatter only
(see [`../WIKI.md`](../WIKI.md) § Edges & Graph); these tools are pure consumers —
they never mutate page prose. Pure stdlib + PyYAML. Run from the package root.

`--root` defaults to the wiki this tool lives in (resolved from the script path), so
the tools work from any CWD and survive relocation.

## Files

| Path | What it does |
|---|---|
| `keg_lint.py` | Walk `.agent/wiki/**/*.md`, validate the `edges:` block, emit a report / triage / one-line `--stats` / `--json` (`edges.json`). Exit 1 on errors. |
| `migrate_all.py` | Bootstrap migrator — add `refers_to` edges from body links (and `cites` from any `sources:` that resolve to a wiki page) to pages lacking `edges:`. Idempotent. |
| `query.py` | Graph-aware retrieval: `neighbors / outbound / inbound / related / path / hubs / contradicts / sources / review`. Importable. Cheatsheet: [`RETRIEVAL.md`](RETRIEVAL.md). |
| `extract_relations.py` | Relation extractor — proposes `refers_to → strong-kind` upgrades via regex rules; writes `source: llm_relation, needs_review: true`. Used here as a validation cross-check on hand-authored edges. |
| `RETRIEVAL.md` | Query cheatsheet: question shapes → commands. |
| `viewer/` | Self-contained Cytoscape graph + markdown page reader (`index.html`, vendored `cytoscape.min.js` + `marked.min.js`, generated `edges.json`). |

## Usage

```bash
python3 .agent/wiki/keg/keg_lint.py --stats         # pages/edges/errors/warnings/orphans
python3 .agent/wiki/keg/keg_lint.py --report        # full lint report
python3 .agent/wiki/keg/keg_lint.py --json --out .agent/wiki/keg/viewer/edges.json
python3 .agent/wiki/keg/migrate_all.py              # seed refers_to from body links
python3 .agent/wiki/keg/query.py related <page> --max-hops 2
python3 .agent/wiki/keg/extract_relations.py        # dry-run; --apply to write proposals
```

Exit 0 = clean (warnings allowed); 1 = errors (broken edge targets, invalid kinds,
asymmetric `contradicts`). Use `--stats` as a manual gate.

## Viewing the graph

```bash
python3 -m http.server 8000 --bind 127.0.0.1 -d .agent/wiki
# open http://localhost:8000/keg/viewer/index.html
```

Graph mode: node shape = page type, colour = edge kind, size ∝ `claim_count`,
dashed = `needs_review`/`contradicts`. Click a node to isolate its neighbourhood;
layout picker (incl. **Columns by type**); **📄 Open page** renders the markdown
in-panel with working diagram images and clickable in-panel `.md` links. Triage mode
lists draft / needs-review items.

## Not wired into the build

Unlike the upstream package, KEG lint is **not** hooked into `brazil-build` here — this
wiki lives inside Valkey and must not gate its build. Run `keg_lint.py --stats` manually
(or via an optional git pre-commit hook) after editing pages.

## Provenance

Ported from `ElastiCacheServerlessPEBrain/tools/keg`. Adaptations: `--root` defaults to
script location; edge kinds `calls` + `configures` added; page types `component` /
`interface` / `flow`; viewer gained the in-panel markdown reader. Research-only tools
(`sync_*`, `extract_decisions`, `patch_body_links`, `find_impact`, `test_*`) were not ported.
