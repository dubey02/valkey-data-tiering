#!/usr/bin/env python3
"""
KEG (KiRoom Entity Graph) lint + export tool.

Walks `wiki/**/*.md`, parses YAML frontmatter, validates the optional
`edges:` block per the schema in `WIKI.md` ("Edges & Graph (KEG)"), and
emits one of:

  - `--report` (default): human-readable markdown lint report
  - `--triage`: only the actionable issues (subset of `--report`)
  - `--json`:   edges.json for the graph viewer
  - `--stats`:  one-line summary

Run from the package root or pass `--root path/to/wiki`.

Ported from ElastiCacheServerlessPEBrain/tools/keg. Adaptations: root
default `wiki`; edge kinds `calls` and `configures` added.

Exit codes:
  0 = lint clean (no errors, warnings allowed)
  1 = errors present (broken edges, invalid kinds, asymmetric contradicts)
"""
from __future__ import annotations

import argparse
import json
import re
import sys
from collections import Counter, defaultdict
from pathlib import Path

try:
    import yaml
except ImportError:
    sys.exit("PyYAML required: pip install pyyaml")


# Edge-kind vocabulary (must match WIKI.md § Edges & Graph).
EDGE_KINDS = {
    "cites",
    "depends_on",
    "calls",
    "configures",
    "owns",
    "contributed_to",
    "contains",
    "implements",
    "supersedes",
    "contradicts",
    "blocks",
    "resolves",
    "mitigates",
    "refers_to",
}
EDGE_SOURCES = {"human", "structural", "llm_relation"}
TIERS = {"fact", "working", "wisdom"}
PAGE_TYPES = {
    "entity", "concept", "source", "decision",
    "question", "comparison", "overview", "playbook", "template",
    "component", "interface", "flow",
}

FRONTMATTER_RE = re.compile(r"\A---\n(.*?)\n---\n", re.S)
# Body markdown link to a `*.md` (skip absolute URLs and anchor-only)
BODY_LINK_RE = re.compile(r"\[[^\]]+\]\(([^)#\s]+\.md)(?:#[^)]*)?\)")


def parse_page(path: Path):
    """Return (frontmatter_dict_or_None, body_str)."""
    text = path.read_text(encoding="utf-8")
    m = FRONTMATTER_RE.match(text)
    if not m:
        return None, text
    try:
        fm = yaml.safe_load(m.group(1)) or {}
    except yaml.YAMLError as exc:
        return ("__yaml_error__", str(exc)), text[m.end():]
    return fm, text[m.end():]


def collect(wiki_root: Path):
    """Walk wiki_root and build {relpath: {fm, body}} for all `*.md`."""
    pages = {}
    for md in sorted(wiki_root.rglob("*.md")):
        rel = md.relative_to(wiki_root).as_posix()
        # Skip docs that aren't graph nodes:
        # - WIKI.md is the schema spec
        # - log.md is chronological history
        # - index.md is the content catalog
        # - keg/ holds tooling, not wiki pages
        if rel in {"WIKI.md", "AGENTS.md", "log.md", "index.md"}:
            continue
        if rel.startswith("templates/") or rel.startswith("keg/"):
            continue
        fm, body = parse_page(md)
        pages[rel] = {"fm": fm, "body": body, "path": md}
    return pages


def resolve_edge_target(from_rel: str, to_rel: str, pages: dict | None = None) -> str:
    """
    Edges and body links use three path conventions:

      1. Page-relative with explicit `../` or `./` prefix.
      2. Wiki-root-relative — e.g. `components/foo.md`.
      3. Same-directory bare filename.

    Try (2) first, fall back to (3). If `pages` is None we assume (2).
    """
    import posixpath
    if to_rel.startswith("../") or to_rel.startswith("./"):
        base = posixpath.dirname(from_rel)
        return posixpath.normpath(posixpath.join(base, to_rel))
    if pages is not None and to_rel in pages:
        return to_rel
    base = posixpath.dirname(from_rel)
    candidate = posixpath.normpath(posixpath.join(base, to_rel))
    if pages is None or candidate in pages:
        return candidate
    return to_rel


def lint(wiki_root: Path):
    """Walk every page, validate edges, return pages, edges, errors, warnings, orphans."""
    pages = collect(wiki_root)
    edges = []
    errors: list[str] = []
    warnings: list[str] = []

    for rel, info in pages.items():
        fm = info["fm"]
        if fm is None:
            warnings.append(f"{rel}: no YAML frontmatter (skipping graph)")
            continue
        if isinstance(fm, tuple) and fm[0] == "__yaml_error__":
            errors.append(f"{rel}: YAML parse error: {fm[1]}")
            continue

        tier = fm.get("tier")
        if tier and tier not in TIERS:
            errors.append(f"{rel}: invalid `tier` '{tier}' (allowed: {sorted(TIERS)})")
        ptype = fm.get("type")
        if ptype and ptype not in PAGE_TYPES:
            warnings.append(f"{rel}: unknown `type` '{ptype}'")

        for i, e in enumerate(fm.get("edges", []) or []):
            if not isinstance(e, dict):
                errors.append(f"{rel}: edges[{i}] is not a mapping")
                continue
            to = e.get("to")
            kind = e.get("kind")
            src = e.get("source", "human")
            needs_review = bool(e.get("needs_review", False))

            if not to or not kind:
                errors.append(f"{rel}: edges[{i}] missing required `to` or `kind`")
                continue
            if kind not in EDGE_KINDS:
                errors.append(f"{rel}: edges[{i}] invalid kind '{kind}' (allowed: {sorted(EDGE_KINDS)})")
            if src not in EDGE_SOURCES:
                errors.append(f"{rel}: edges[{i}] invalid source '{src}' (allowed: {sorted(EDGE_SOURCES)})")

            target = resolve_edge_target(rel, to, pages)
            if target not in pages:
                errors.append(f"{rel}: edges[{i}] target missing: '{to}' (resolved: '{target}')")
                continue

            edges.append({
                "from": rel,
                "to": target,
                "kind": kind,
                "source": src,
                "needs_review": needs_review,
                "created": e.get("created"),
                "invalidated": e.get("invalidated"),
                "note": e.get("note"),
            })

    # `contradicts` MUST be symmetric
    contradict_pairs = {(e["from"], e["to"]) for e in edges if e["kind"] == "contradicts"}
    for a, b in contradict_pairs:
        if (b, a) not in contradict_pairs:
            errors.append(f"asymmetric contradicts: {a} → {b} but no edge back")

    # `supersedes` target should have status: superseded
    for e in edges:
        if e["kind"] != "supersedes":
            continue
        target_fm = pages[e["to"]]["fm"]
        if isinstance(target_fm, dict) and target_fm.get("status") not in ("superseded", "stale"):
            warnings.append(
                f"{e['from']} supersedes {e['to']} but target status is "
                f"'{target_fm.get('status', 'unset')}' (expected 'superseded')"
            )

    # Untyped body link: body link not represented in `edges:`.
    edges_by_from = defaultdict(set)
    for e in edges:
        edges_by_from[e["from"]].add(e["to"])

    for rel, info in pages.items():
        if not isinstance(info["fm"], dict):
            continue
        if not info["fm"].get("edges"):
            continue
        body_targets = set()
        for m in BODY_LINK_RE.finditer(info["body"]):
            target = resolve_edge_target(rel, m.group(1), pages)
            if target in pages and target != rel:
                body_targets.add(target)
        missing = body_targets - edges_by_from[rel]
        if missing:
            for t in sorted(missing):
                warnings.append(f"{rel}: body link to '{t}' not in edges: (suggest kind=refers_to)")

    # Orphans: migrated pages with no inbound edges
    inbound = Counter(e["to"] for e in edges)
    orphans = []
    for rel, info in pages.items():
        if not isinstance(info["fm"], dict):
            continue
        if not info["fm"].get("edges"):
            continue
        if inbound[rel] == 0:
            orphans.append(rel)

    return pages, edges, errors, warnings, orphans


def render_report(pages, edges, errors, warnings, orphans, triage_only=False):
    out = []
    out.append("# KEG Lint Report\n")
    migrated = sum(1 for p in pages.values()
                   if isinstance(p["fm"], dict) and p["fm"].get("edges"))
    out.append(f"- **Pages**: {len(pages)} total, **{migrated} migrated** to typed edges ({migrated/max(len(pages),1)*100:.0f}%)")
    out.append(f"- **Edges**: {len(edges)}")
    out.append(f"- **Errors**: {len(errors)}")
    out.append(f"- **Warnings**: {len(warnings)}")
    out.append(f"- **Orphans (migrated, no inbound)**: {len(orphans)}")
    out.append("")

    if errors:
        out.append("## Errors (block the graph)\n")
        for er in errors:
            out.append(f"- ❌ {er}")
        out.append("")

    if not triage_only and warnings:
        out.append(f"## Warnings ({len(warnings)})\n")
        for w in warnings[:50]:
            out.append(f"- ⚠️  {w}")
        if len(warnings) > 50:
            out.append(f"- … and {len(warnings) - 50} more")
        out.append("")

    if orphans:
        out.append(f"## Orphans (no inbound edges)\n")
        for o in sorted(orphans):
            out.append(f"- 🏝️  {o}")
        out.append("")

    if not triage_only:
        kinds = Counter(e["kind"] for e in edges)
        out.append("## Edge-kind histogram\n")
        for k, n in kinds.most_common():
            out.append(f"- `{k}`: {n}")
        out.append("")

        types = Counter(p["fm"].get("type") for p in pages.values()
                        if isinstance(p["fm"], dict))
        out.append("## Page-type histogram\n")
        for t, n in types.most_common():
            out.append(f"- `{t}`: {n}")

    return "\n".join(out) + "\n"


def render_json(pages, edges):
    nodes = []
    for rel, info in pages.items():
        if not isinstance(info["fm"], dict):
            continue
        fm = info["fm"]
        nodes.append({
            "id": rel,
            "label": Path(rel).stem,
            "type": fm.get("type", "unknown"),
            "tier": fm.get("tier"),
            "claim_count": fm.get("claim_count"),
            "status": fm.get("status", "active"),
            "migrated": bool(fm.get("edges")),
        })
    return {"nodes": nodes, "edges": edges}


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--root", type=Path, default=Path(__file__).resolve().parent.parent,
                    help="wiki root directory (default: the wiki this tool lives in)")
    g = ap.add_mutually_exclusive_group()
    g.add_argument("--report", action="store_true", help="human-readable report (default)")
    g.add_argument("--triage", action="store_true", help="only actionable issues")
    g.add_argument("--json", action="store_true", help="dump edges.json (graph viewer)")
    g.add_argument("--stats", action="store_true", help="one-line summary")
    ap.add_argument("--out", type=Path, help="write output to file instead of stdout")
    args = ap.parse_args()

    if not args.root.is_dir():
        sys.exit(f"wiki root not found: {args.root}")

    pages, edges, errors, warnings, orphans = lint(args.root)

    if args.json:
        payload = render_json(pages, edges)
        text = json.dumps(payload, indent=2, sort_keys=True, default=str)
    elif args.stats:
        migrated = sum(1 for p in pages.values()
                       if isinstance(p["fm"], dict) and p["fm"].get("edges"))
        text = (f"pages={len(pages)} migrated={migrated} edges={len(edges)} "
                f"errors={len(errors)} warnings={len(warnings)} orphans={len(orphans)}")
    else:
        text = render_report(pages, edges, errors, warnings, orphans,
                             triage_only=args.triage)

    if args.out:
        args.out.parent.mkdir(parents=True, exist_ok=True)
        args.out.write_text(text + ("\n" if not text.endswith("\n") else ""), encoding="utf-8")
        print(f"wrote {args.out}", file=sys.stderr)
    else:
        print(text)

    sys.exit(1 if errors else 0)


if __name__ == "__main__":
    main()
