#!/usr/bin/env python3
"""
KEG graph-aware retrieval — the wiki's "fourth retrieval channel".

Given a starting page, follow typed edges to surface related pages ranked
by edge-kind relevance.

Module-level functions (pure, testable, importable):
  - load_wiki(root)               -> (pages, edges)
  - neighbors(page, ...)          -> 1-hop in + out, deduped
  - outbound(page, ...)           -> 1-hop outgoing
  - inbound(page, ...)            -> 1-hop incoming
  - related(page, max_hops, ...)  -> N-hop neighborhood, ranked
  - path(src, dst, max_hops, ...) -> shortest typed path, or None
  - hubs(pages, edges, by, n)     -> top pages by inbound/outbound/degree
  - contradicts(page, ...)        -> symmetric `contradicts` partners
  - cites_transitive(page, ...)   -> all `cites` targets (transitive)
  - review(pages, edges, ...)     -> edges with needs_review=True

CLI:
  python3 wiki/keg/query.py <command> [args] [--format text|json]

Ported from ElastiCacheServerlessPEBrain/tools/keg. Adaptations: root
default `wiki`; `calls` and `configures` weights added.
"""
from __future__ import annotations

import argparse
import json
import re
import sys
from collections import deque
from dataclasses import dataclass, field, asdict
from pathlib import Path
from typing import Iterable

try:
    import yaml
except ImportError:
    sys.exit("PyYAML required: pip3 install pyyaml")


# Edge-kind weights for relevance ranking.
EDGE_WEIGHTS: dict[str, float] = {
    "cites": 1.0,
    "depends_on": 1.0,
    "calls": 1.0,
    "implements": 1.0,
    "supersedes": 0.9,
    "contradicts": 0.9,
    "contains": 0.9,
    "blocks": 0.9,
    "resolves": 0.9,
    "mitigates": 0.9,
    "configures": 0.7,
    "owns": 0.7,
    "contributed_to": 0.7,
    "refers_to": 0.3,
}
DEFAULT_WEIGHT = 0.3

VALID_KINDS = set(EDGE_WEIGHTS.keys())

FRONTMATTER_RE = re.compile(r"\A---\n(.*?)\n---\n", re.S)


@dataclass(frozen=True)
class Edge:
    from_page: str
    to_page: str
    kind: str
    source: str = "human"
    needs_review: bool = False
    note: str | None = None
    created: str | None = None
    invalidated: str | None = None


@dataclass
class Neighbor:
    page: str
    distance: int
    edges: list[Edge] = field(default_factory=list)
    score: float = 0.0

    def to_dict(self) -> dict:
        return {
            "page": self.page,
            "distance": self.distance,
            "score": round(self.score, 4),
            "edges": [
                {
                    "from": e.from_page,
                    "to": e.to_page,
                    "kind": e.kind,
                    "source": e.source,
                    "needs_review": e.needs_review,
                    "note": e.note,
                }
                for e in self.edges
            ],
        }


@dataclass
class PathStep:
    from_page: str
    to_page: str
    kind: str
    direction: str
    note: str | None = None

    def to_dict(self) -> dict:
        return asdict(self)


# ---------- Loading ----------

def load_wiki(root: Path) -> tuple[dict[str, dict], list[Edge]]:
    if not root.is_dir():
        raise FileNotFoundError(f"wiki root not found: {root}")

    pages: dict[str, dict] = {}
    for md in sorted(root.rglob("*.md")):
        rel = md.relative_to(root).as_posix()
        if rel in {"WIKI.md", "AGENTS.md", "log.md", "index.md"}:
            continue
        if rel.startswith("templates/") or rel.startswith("keg/"):
            continue
        text = md.read_text(encoding="utf-8")
        m = FRONTMATTER_RE.match(text)
        if not m:
            continue
        try:
            fm = yaml.safe_load(m.group(1)) or {}
        except yaml.YAMLError:
            continue
        if isinstance(fm, dict):
            pages[rel] = fm

    edges: list[Edge] = []
    for rel, fm in pages.items():
        for e in fm.get("edges", []) or []:
            if not isinstance(e, dict):
                continue
            to = e.get("to")
            kind = e.get("kind")
            if not to or not kind:
                continue
            target = _resolve(rel, to, pages)
            if target not in pages:
                continue
            edges.append(Edge(
                from_page=rel,
                to_page=target,
                kind=kind,
                source=e.get("source", "human"),
                needs_review=bool(e.get("needs_review", False)),
                note=e.get("note"),
                created=str(e["created"]) if e.get("created") else None,
                invalidated=str(e["invalidated"]) if e.get("invalidated") else None,
            ))
    return pages, edges


def _resolve(from_rel: str, to_rel: str, pages: dict) -> str:
    import posixpath
    if to_rel.startswith("../") or to_rel.startswith("./"):
        base = posixpath.dirname(from_rel)
        return posixpath.normpath(posixpath.join(base, to_rel))
    if to_rel in pages:
        return to_rel
    base = posixpath.dirname(from_rel)
    candidate = posixpath.normpath(posixpath.join(base, to_rel))
    return candidate if candidate in pages else to_rel


# ---------- Adjacency helpers ----------

def _build_adj(edges: list[Edge]) -> tuple[dict[str, list[Edge]], dict[str, list[Edge]]]:
    out_adj: dict[str, list[Edge]] = {}
    in_adj: dict[str, list[Edge]] = {}
    for e in edges:
        out_adj.setdefault(e.from_page, []).append(e)
        in_adj.setdefault(e.to_page, []).append(e)
    return out_adj, in_adj


def _filter_kinds(edges: Iterable[Edge], kinds: set[str] | None) -> list[Edge]:
    if not kinds:
        return list(edges)
    return [e for e in edges if e.kind in kinds]


def _weight(kind: str) -> float:
    return EDGE_WEIGHTS.get(kind, DEFAULT_WEIGHT)


# ---------- Public query API ----------

def outbound(page: str, pages: dict, edges: list[Edge],
             kinds: set[str] | None = None) -> list[Edge]:
    if page not in pages:
        raise KeyError(f"page not in wiki: {page}")
    out_adj, _ = _build_adj(edges)
    return _filter_kinds(out_adj.get(page, []), kinds)


def inbound(page: str, pages: dict, edges: list[Edge],
            kinds: set[str] | None = None) -> list[Edge]:
    if page not in pages:
        raise KeyError(f"page not in wiki: {page}")
    _, in_adj = _build_adj(edges)
    return _filter_kinds(in_adj.get(page, []), kinds)


def neighbors(page: str, pages: dict, edges: list[Edge],
              kinds: set[str] | None = None) -> list[Neighbor]:
    if page not in pages:
        raise KeyError(f"page not in wiki: {page}")
    out_adj, in_adj = _build_adj(edges)
    by_other: dict[str, list[Edge]] = {}
    for e in _filter_kinds(out_adj.get(page, []), kinds):
        by_other.setdefault(e.to_page, []).append(e)
    for e in _filter_kinds(in_adj.get(page, []), kinds):
        by_other.setdefault(e.from_page, []).append(e)
    results = []
    for other, es in by_other.items():
        best = max((_weight(e.kind) for e in es), default=0.0)
        results.append(Neighbor(page=other, distance=1, edges=es, score=best))
    results.sort(key=lambda n: (-n.score, n.page))
    return results


def related(page: str, pages: dict, edges: list[Edge],
            max_hops: int = 2,
            kinds: set[str] | None = None,
            limit: int | None = None) -> list[Neighbor]:
    if page not in pages:
        raise KeyError(f"page not in wiki: {page}")
    if max_hops < 1:
        return []

    out_adj, in_adj = _build_adj(edges)
    best: dict[str, tuple[int, float, list[Edge]]] = {}
    queue: deque[tuple[str, int, list[Edge]]] = deque([(page, 0, [])])
    while queue:
        cur, dist, path_edges = queue.popleft()
        if dist >= max_hops:
            continue
        adj_edges: list[Edge] = []
        for e in out_adj.get(cur, []):
            if kinds is None or e.kind in kinds:
                adj_edges.append(e)
        for e in in_adj.get(cur, []):
            if kinds is None or e.kind in kinds:
                adj_edges.append(e)
        for e in adj_edges:
            other = e.to_page if e.from_page == cur else e.from_page
            if other == page:
                continue
            new_dist = dist + 1
            new_path = path_edges + [e]
            cur_best = best.get(other)
            new_best_weight = max(_weight(x.kind) for x in new_path)
            if cur_best is None or new_dist < cur_best[0] or (
                new_dist == cur_best[0] and new_best_weight > cur_best[1]
            ):
                best[other] = (new_dist, new_best_weight, new_path)
                queue.append((other, new_dist, new_path))

    results = []
    for other, (dist, weight, path_edges) in best.items():
        results.append(Neighbor(
            page=other,
            distance=dist,
            edges=path_edges,
            score=weight / dist,
        ))
    results.sort(key=lambda n: (n.distance, -n.score, n.page))
    if limit is not None:
        results = results[:limit]
    return results


def path(src: str, dst: str, pages: dict, edges: list[Edge],
         max_hops: int = 4,
         kinds: set[str] | None = None) -> list[PathStep] | None:
    if src not in pages:
        raise KeyError(f"page not in wiki: {src}")
    if dst not in pages:
        raise KeyError(f"page not in wiki: {dst}")
    if src == dst:
        return []

    out_adj, in_adj = _build_adj(edges)
    parent: dict[str, tuple[str, Edge, str] | None] = {src: None}
    queue: deque[tuple[str, int]] = deque([(src, 0)])
    while queue:
        cur, dist = queue.popleft()
        if dist >= max_hops:
            continue
        for e in out_adj.get(cur, []):
            if kinds is not None and e.kind not in kinds:
                continue
            if e.to_page in parent:
                continue
            parent[e.to_page] = (cur, e, "out")
            if e.to_page == dst:
                return _reconstruct_path(parent, dst)
            queue.append((e.to_page, dist + 1))
        for e in in_adj.get(cur, []):
            if kinds is not None and e.kind not in kinds:
                continue
            if e.from_page in parent:
                continue
            parent[e.from_page] = (cur, e, "in")
            if e.from_page == dst:
                return _reconstruct_path(parent, dst)
            queue.append((e.from_page, dist + 1))
    return None


def _reconstruct_path(parent: dict[str, tuple[str, Edge, str] | None],
                      dst: str) -> list[PathStep]:
    steps: list[PathStep] = []
    cur = dst
    while parent.get(cur) is not None:
        prev, edge, direction = parent[cur]  # type: ignore[misc]
        steps.append(PathStep(
            from_page=prev,
            to_page=cur,
            kind=edge.kind,
            direction=direction,
            note=edge.note,
        ))
        cur = prev
    steps.reverse()
    return steps


def hubs(pages: dict, edges: list[Edge],
         by: str = "degree",
         n: int = 10) -> list[tuple[str, int, dict[str, int]]]:
    if by not in {"inbound", "outbound", "degree"}:
        raise ValueError(f"by must be inbound/outbound/degree, got {by!r}")
    in_count: dict[str, int] = {p: 0 for p in pages}
    out_count: dict[str, int] = {p: 0 for p in pages}
    by_kind_in: dict[str, dict[str, int]] = {p: {} for p in pages}
    by_kind_out: dict[str, dict[str, int]] = {p: {} for p in pages}
    for e in edges:
        out_count[e.from_page] = out_count.get(e.from_page, 0) + 1
        in_count[e.to_page] = in_count.get(e.to_page, 0) + 1
        by_kind_out[e.from_page][e.kind] = by_kind_out[e.from_page].get(e.kind, 0) + 1
        by_kind_in[e.to_page][e.kind] = by_kind_in[e.to_page].get(e.kind, 0) + 1

    if by == "inbound":
        scored = [(p, in_count.get(p, 0), by_kind_in.get(p, {})) for p in pages]
    elif by == "outbound":
        scored = [(p, out_count.get(p, 0), by_kind_out.get(p, {})) for p in pages]
    else:
        merged_kinds = {}
        for p in pages:
            kinds_p = {}
            for k, v in by_kind_in.get(p, {}).items():
                kinds_p[k] = kinds_p.get(k, 0) + v
            for k, v in by_kind_out.get(p, {}).items():
                kinds_p[k] = kinds_p.get(k, 0) + v
            merged_kinds[p] = kinds_p
        scored = [(p, in_count.get(p, 0) + out_count.get(p, 0), merged_kinds[p])
                  for p in pages]

    scored.sort(key=lambda t: (-t[1], t[0]))
    return scored[:n]


def contradicts(page: str, pages: dict, edges: list[Edge]) -> list[str]:
    if page not in pages:
        raise KeyError(f"page not in wiki: {page}")
    forward = {e.to_page for e in edges
               if e.from_page == page and e.kind == "contradicts"}
    backward = {e.from_page for e in edges
                if e.to_page == page and e.kind == "contradicts"}
    return sorted(forward & backward)


def cites_transitive(page: str, pages: dict, edges: list[Edge],
                     max_hops: int = 3) -> list[str]:
    if page not in pages:
        raise KeyError(f"page not in wiki: {page}")
    out_adj, _ = _build_adj(edges)
    seen: set[str] = {page}
    queue: deque[tuple[str, int]] = deque([(page, 0)])
    cited: list[str] = []
    while queue:
        cur, dist = queue.popleft()
        if dist >= max_hops:
            continue
        for e in out_adj.get(cur, []):
            if e.kind != "cites":
                continue
            if e.to_page in seen:
                continue
            seen.add(e.to_page)
            cited.append(e.to_page)
            queue.append((e.to_page, dist + 1))
    return sorted(cited)


def review(pages: dict, edges: list[Edge],
           kind: str | None = None,
           source: str | None = None) -> list[Edge]:
    out = [e for e in edges if e.needs_review]
    if kind:
        out = [e for e in out if e.kind == kind]
    if source:
        out = [e for e in out if e.source == source]
    out.sort(key=lambda e: (e.from_page, e.to_page, e.kind))
    return out


# ---------- CLI rendering ----------

def _render_neighbors_text(page: str, items: list[Neighbor], header: str = "") -> str:
    out = []
    if header:
        out.append(header)
    for n in items:
        edge_descs = []
        for e in n.edges:
            arrow = "→" if e.from_page == page else "←"
            note = f" ({e.note})" if e.note else ""
            edge_descs.append(f"{arrow} {e.kind}{note}")
        out.append(f"  {n.page}  [{', '.join(edge_descs)}]  d={n.distance} score={n.score:.2f}")
    if not items:
        out.append("  (none)")
    return "\n".join(out)


def _render_path_text(steps: list[PathStep]) -> str:
    if not steps:
        return "(empty path — same page)"
    lines = []
    for i, s in enumerate(steps):
        arrow = "→" if s.direction == "out" else "←"
        note = f" ({s.note})" if s.note else ""
        lines.append(f"  {i + 1}. {s.from_page}  {arrow} {s.kind}{note}  {s.to_page}")
    return "\n".join(lines)


def _render_hubs_text(items: list[tuple[str, int, dict]]) -> str:
    out = []
    for p, total, by_kind in items:
        kind_str = ", ".join(f"{k}:{v}" for k, v in sorted(by_kind.items(),
                                                           key=lambda kv: -kv[1]))
        out.append(f"  {p}  total={total}  ({kind_str})")
    return "\n".join(out) if out else "  (no hubs)"


def _render_paths_json(steps: list[PathStep]) -> str:
    return json.dumps([s.to_dict() for s in steps], indent=2)


def _render_neighbors_json(items: list[Neighbor]) -> str:
    return json.dumps([n.to_dict() for n in items], indent=2)


# ---------- CLI entry point ----------

def _parse_kinds(s: str | None) -> set[str] | None:
    if not s:
        return None
    out = {k.strip() for k in s.split(",") if k.strip()}
    invalid = out - VALID_KINDS
    if invalid:
        sys.exit(f"unknown edge kinds: {sorted(invalid)}\n  valid: {sorted(VALID_KINDS)}")
    return out


def _cmd_neighbors(args, pages, edges):
    items = neighbors(args.page, pages, edges, kinds=_parse_kinds(args.kinds))
    if args.format == "json":
        print(_render_neighbors_json(items))
    else:
        print(_render_neighbors_text(args.page, items, f"Neighbors of {args.page}:"))


def _cmd_outbound(args, pages, edges):
    es = outbound(args.page, pages, edges, kinds=_parse_kinds(args.kinds))
    if args.format == "json":
        items = [Neighbor(page=e.to_page, distance=1, edges=[e], score=_weight(e.kind))
                 for e in es]
        print(_render_neighbors_json(items))
    else:
        print(f"Outbound from {args.page}:")
        for e in sorted(es, key=lambda x: (-_weight(x.kind), x.to_page)):
            note = f" ({e.note})" if e.note else ""
            print(f"  → {e.kind}{note}  {e.to_page}")
        if not es:
            print("  (none)")


def _cmd_inbound(args, pages, edges):
    es = inbound(args.page, pages, edges, kinds=_parse_kinds(args.kinds))
    if args.format == "json":
        items = [Neighbor(page=e.from_page, distance=1, edges=[e], score=_weight(e.kind))
                 for e in es]
        print(_render_neighbors_json(items))
    else:
        print(f"Inbound to {args.page}:")
        for e in sorted(es, key=lambda x: (-_weight(x.kind), x.from_page)):
            note = f" ({e.note})" if e.note else ""
            print(f"  ← {e.kind}{note}  {e.from_page}")
        if not es:
            print("  (none)")


def _cmd_related(args, pages, edges):
    items = related(args.page, pages, edges,
                    max_hops=args.max_hops,
                    kinds=_parse_kinds(args.kinds),
                    limit=args.limit)
    if args.format == "json":
        print(_render_neighbors_json(items))
    else:
        print(_render_neighbors_text(
            args.page, items,
            f"Related to {args.page} (max-hops={args.max_hops}):"))


def _cmd_path(args, pages, edges):
    steps = path(args.from_page, args.to_page, pages, edges,
                 max_hops=args.max_hops, kinds=_parse_kinds(args.kinds))
    if steps is None:
        if args.format == "json":
            print("null")
        else:
            print(f"No path from {args.from_page} to {args.to_page} "
                  f"within {args.max_hops} hops.")
        sys.exit(1)
    if args.format == "json":
        print(_render_paths_json(steps))
    else:
        print(f"Path from {args.from_page} → {args.to_page} ({len(steps)} hops):")
        print(_render_path_text(steps))


def _cmd_hubs(args, pages, edges):
    items = hubs(pages, edges, by=args.by, n=args.n)
    if args.format == "json":
        print(json.dumps([{"page": p, "total": t, "by_kind": k}
                          for p, t, k in items], indent=2))
    else:
        print(f"Top {args.n} pages by {args.by}:")
        print(_render_hubs_text(items))


def _cmd_contradicts(args, pages, edges):
    parts = contradicts(args.page, pages, edges)
    if args.format == "json":
        print(json.dumps(parts, indent=2))
    else:
        print(f"Pages that contradict {args.page}:")
        if not parts:
            print("  (none)")
        for p in parts:
            print(f"  {p}")


def _cmd_sources(args, pages, edges):
    cited = cites_transitive(args.page, pages, edges, max_hops=args.max_hops)
    if args.format == "json":
        print(json.dumps(cited, indent=2))
    else:
        print(f"Sources cited (transitively) from {args.page}:")
        if not cited:
            print("  (none)")
        for p in cited:
            print(f"  {p}")


def _cmd_review(args, pages, edges):
    items = review(pages, edges, kind=args.kind, source=args.source)
    if args.format == "json":
        out = [
            {
                "from": e.from_page,
                "to": e.to_page,
                "kind": e.kind,
                "source": e.source,
                "needs_review": e.needs_review,
                "note": e.note,
            }
            for e in items
        ]
        print(json.dumps(out, indent=2))
    else:
        print(f"Edges needing review: {len(items)}")
        if not items:
            print("  (none)")
            return
        by_from: dict[str, list[Edge]] = {}
        for e in items:
            by_from.setdefault(e.from_page, []).append(e)
        for f in sorted(by_from):
            print(f"\n  {f}")
            for e in by_from[f]:
                note = f"  ({e.note})" if e.note else ""
                print(f"    → {e.kind}{note}  source={e.source}  {e.to_page}")


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--root", type=Path, default=Path(__file__).resolve().parent.parent,
                   help="wiki root (default: the wiki this tool lives in)")
    p.add_argument("--format", choices=["text", "json"], default="text",
                   help="output format (default: text)")

    def _shared(subparser):
        subparser.add_argument("--format", choices=["text", "json"],
                               default=argparse.SUPPRESS,
                               help="output format (overrides global)")
        return subparser

    sub = p.add_subparsers(dest="cmd", required=True)

    sn = _shared(sub.add_parser("neighbors", help="1-hop in + out neighbors"))
    sn.add_argument("page")
    sn.add_argument("--kinds", help="comma-separated edge kinds to include")
    sn.set_defaults(func=_cmd_neighbors)

    so = _shared(sub.add_parser("outbound", help="direct outgoing edges"))
    so.add_argument("page")
    so.add_argument("--kinds")
    so.set_defaults(func=_cmd_outbound)

    si = _shared(sub.add_parser("inbound", help="direct incoming edges"))
    si.add_argument("page")
    si.add_argument("--kinds")
    si.set_defaults(func=_cmd_inbound)

    sr = _shared(sub.add_parser("related", help="N-hop neighborhood, ranked"))
    sr.add_argument("page")
    sr.add_argument("--max-hops", type=int, default=2)
    sr.add_argument("--kinds")
    sr.add_argument("--limit", type=int)
    sr.set_defaults(func=_cmd_related)

    sp = _shared(sub.add_parser("path", help="shortest typed path between two pages"))
    sp.add_argument("from_page", metavar="from")
    sp.add_argument("to_page", metavar="to")
    sp.add_argument("--max-hops", type=int, default=4)
    sp.add_argument("--kinds")
    sp.set_defaults(func=_cmd_path)

    sh = _shared(sub.add_parser("hubs", help="top pages by edge degree"))
    sh.add_argument("--by", choices=["inbound", "outbound", "degree"], default="degree")
    sh.add_argument("--n", type=int, default=10)
    sh.set_defaults(func=_cmd_hubs)

    sc = _shared(sub.add_parser("contradicts", help="symmetric contradicts partners"))
    sc.add_argument("page")
    sc.set_defaults(func=_cmd_contradicts)

    ss = _shared(sub.add_parser("sources", help="all `cites` targets, transitive"))
    ss.add_argument("page")
    ss.add_argument("--max-hops", type=int, default=3)
    ss.set_defaults(func=_cmd_sources)

    sv = _shared(sub.add_parser("review",
                                help="list edges with needs_review=true (triage queue)"))
    sv.add_argument("--kind", help="filter to one edge kind")
    sv.add_argument("--source", help="filter to one source value (e.g. llm_relation)")
    sv.set_defaults(func=_cmd_review)

    args = p.parse_args(argv)
    pages, edges = load_wiki(args.root)
    args.func(args, pages, edges)
    return 0


if __name__ == "__main__":
    sys.exit(main())
