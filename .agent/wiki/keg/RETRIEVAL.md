# KEG Retrieval Cheatsheet

Common question shapes mapped to `query.py` calls. Run from the package
root (the dir containing `wiki/`). Default `--root` is `wiki`.

## "What does X depend on / call?" — direct technical edges

```bash
python3 .agent/wiki/keg/query.py outbound components/engine-integration.md --kinds depends_on
python3 .agent/wiki/keg/query.py outbound components/engine-integration.md --kinds calls
```

## "What depends on / calls X?" — reverse (blast radius)

```bash
python3 .agent/wiki/keg/query.py inbound interfaces/storagetype-vtable.md --kinds depends_on,implements
```

## "What configures X?"

```bash
python3 .agent/wiki/keg/query.py inbound components/backends.md --kinds configures
```

## "What contradicts decision/page D?"

```bash
python3 .agent/wiki/keg/query.py contradicts decisions/known-limitations.md
```

Symmetric `contradicts` partners only (one-way edges are schema violations).

## "What connects A to B?" — find the bridge

```bash
python3 .agent/wiki/keg/query.py path components/state-machine.md components/backends.md
```

## "What's the most central page?"

```bash
python3 .agent/wiki/keg/query.py hubs --by inbound --n 10
python3 .agent/wiki/keg/query.py hubs --by degree --n 10
```

## "Everything connected to X within 2 hops"

```bash
python3 .agent/wiki/keg/query.py related components/engine-integration.md --max-hops 2 --limit 15
python3 .agent/wiki/keg/query.py related components/engine-integration.md \
    --kinds depends_on,implements,contains,calls   # strong-only, drop refers_to noise
```

## "Triage queue" — edges flagged by the extractor

```bash
python3 .agent/wiki/keg/extract_relations.py            # dry-run proposals
python3 .agent/wiki/keg/extract_relations.py --apply    # write source=llm_relation, needs_review=true
python3 .agent/wiki/keg/query.py review                 # see what needs review
```

## JSON for follow-up processing

```bash
python3 .agent/wiki/keg/query.py related components/state-machine.md --format json | jq .
```

## When the graph isn't enough

1. **Page not migrated** — check `keg_lint.py --stats`; no `edges:` block ⇒ no edges.
2. **Question is about contents, not relationships** — use grep / read the page.
3. **Relationship is in prose but not typed** — promote the body link to a typed edge
   in the same session (migration policy in `WIKI.md`).

## Schema reference (edge kinds and weights)

| Kind | Weight | Meaning |
|---|---|---|
| `cites` | 1.0 | Page backed by a source |
| `depends_on` | 1.0 | A's behavior requires B |
| `calls` | 1.0 | A invokes/dispatches to B (callgraph) |
| `implements` | 1.0 | Interface realised by component |
| `supersedes` | 0.9 | Replaces; old should be `status: superseded` |
| `contradicts` | 0.9 | Symmetric; both pages MUST link back |
| `contains` | 0.9 | Composition / part-of |
| `blocks` | 0.9 | Issue blocking a goal |
| `resolves` | 0.9 | Answer closes a question |
| `mitigates` | 0.9 | Security / risk relationship |
| `configures` | 0.7 | Config/arg drives a component |
| `owns` | 0.7 | Team/person owns something |
| `contributed_to` | 0.7 | Contributed without owning |
| `refers_to` | 0.3 | Soft "see also"; default fallback |

`related` picks the strongest single edge along the shortest path and divides by hop
count: 1-hop `depends_on` (1.0) outranks 2-hop `cites` (0.5) outranks 1-hop `refers_to` (0.3).

## See also

- `WIKI.md` § "Edges & Graph (KEG)" — schema reference.
- `.agent/wiki/keg/keg_lint.py --report` — lint output.
- `.agent/wiki/keg/viewer/index.html` — interactive graph (serve on 127.0.0.1).
