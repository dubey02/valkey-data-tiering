#!/usr/bin/env python3
"""
Bulk migrate every unmigrated wiki page to typed edges.

Strategy (per `WIKI.md` § "Edges & Graph"):
  - For every entry in `sources:` frontmatter that resolves to a wiki page
        → `cites` edge with `source: structural`
    (Our `sources:` mostly point to code paths outside wiki/, which do
    not resolve and are skipped — only refers_to from body links remain.)
  - For every unique body markdown link to another wiki page
        → `refers_to` edge with `source: human`
  - Pages that already have `edges:` are left alone (idempotent).
  - `WIKI.md`, `log.md`, `index.md`, `keg/`, `templates/` are skipped.

Preserves existing frontmatter as raw text; the new `edges:` block is
appended just before the closing `---`.

Ported from ElastiCacheServerlessPEBrain/tools/keg. Adaptations: WIKI=wiki.

Usage (from package root):
    python3 wiki/keg/migrate_all.py [--dry-run]
"""
from __future__ import annotations

import argparse
import re
import sys
from collections import OrderedDict
from pathlib import Path, PurePosixPath

try:
    import yaml
except ImportError:
    sys.exit("PyYAML required: pip3 install pyyaml")


WIKI = Path(__file__).resolve().parent.parent
TODAY = "2026-06-03"

FRONTMATTER_RE = re.compile(r"\A(---\n)(.*?)(\n---\n)", re.S)
BODY_LINK_RE = re.compile(r"\[[^\]]+\]\(([^)#\s]+\.md)(?:#[^)]*)?\)")


def collect_pages() -> dict[str, Path]:
    pages: dict[str, Path] = {}
    for md in sorted(WIKI.rglob("*.md")):
        rel = md.relative_to(WIKI).as_posix()
        if rel in {"WIKI.md", "AGENTS.md", "log.md", "index.md"}:
            continue
        if rel.startswith("templates/") or rel.startswith("keg/"):
            continue
        pages[rel] = md
    return pages


def resolve(from_rel: str, to_rel: str, pages: dict | None = None) -> str:
    import posixpath
    if to_rel.startswith("../") or to_rel.startswith("./"):
        base = str(PurePosixPath(from_rel).parent)
        return posixpath.normpath(posixpath.join(base, to_rel))
    if pages is not None and to_rel in pages:
        return to_rel
    base = str(PurePosixPath(from_rel).parent)
    return posixpath.normpath(posixpath.join(base, to_rel))


def build_edges(rel: str, fm: dict, body: str, pages: dict[str, Path]) -> list[dict]:
    edges: OrderedDict[str, dict] = OrderedDict()
    for src in fm.get("sources", []) or []:
        if not isinstance(src, str):
            continue
        target = resolve(rel, src, pages)
        if target in pages and target != rel:
            edges[target] = {"to": target, "kind": "cites",
                             "source": "structural", "created": TODAY}
    for match in BODY_LINK_RE.finditer(body):
        link = match.group(1)
        target = resolve(rel, link, pages)
        if target not in pages or target == rel or target in edges:
            continue
        edges[target] = {"to": target, "kind": "refers_to",
                         "source": "human", "created": TODAY}
    return list(edges.values())


def render_edges_block(edges: list[dict]) -> str:
    lines = ["edges:"]
    for e in edges:
        lines.append(f"  - to: {e['to']}")
        lines.append(f"    kind: {e['kind']}")
        lines.append(f"    source: {e['source']}")
        lines.append(f"    created: {e['created']}")
    return "\n".join(lines)


def migrate_page(rel: str, path: Path, pages: dict[str, Path], dry_run: bool) -> int:
    text = path.read_text(encoding="utf-8")
    m = FRONTMATTER_RE.match(text)
    if not m:
        return 0
    fm_text = m.group(2)
    try:
        fm = yaml.safe_load(fm_text) or {}
    except yaml.YAMLError:
        return 0
    if not isinstance(fm, dict):
        return 0
    if fm.get("edges"):
        return 0
    body = text[m.end():]
    edges = build_edges(rel, fm, body, pages)
    if not edges:
        return 0
    edges_block = render_edges_block(edges)
    new_text = f"---\n{fm_text}\n{edges_block}\n---\n{body}"
    if not dry_run:
        path.write_text(new_text, encoding="utf-8")
    return len(edges)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--dry-run", action="store_true", help="Don't write files")
    args = ap.parse_args()

    if not WIKI.is_dir():
        sys.exit(f"wiki root not found: {WIKI}")

    pages = collect_pages()
    print(f"Discovered {len(pages)} wiki pages")

    migrated = 0
    skipped = 0
    total_edges = 0
    for rel, path in pages.items():
        n = migrate_page(rel, path, pages, args.dry_run)
        if n > 0:
            print(f"  + {rel} ({n} edges)")
            migrated += 1
            total_edges += n
        else:
            skipped += 1

    verb = "would migrate" if args.dry_run else "migrated"
    print(f"\n{verb}: {migrated} pages ({total_edges} new edges total)")
    print(f"skipped (already migrated, no edges, or unparseable): {skipped}")


if __name__ == "__main__":
    main()
