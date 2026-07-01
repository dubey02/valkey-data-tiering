#!/usr/bin/env python3
"""
KEG relation extractor — propose typed-edge upgrades for `refers_to` edges.

For every edge currently typed `refers_to`, this tool examines (1) the
edge's `note:` field and (2) body prose around the link, then proposes a
stronger kind based on regex pattern rules. Proposals are written with
`source: llm_relation, needs_review: true` so they show up in
`query.py review`.

v1 is rule-based — deterministic, idempotent, no LLM/AWS. Precision is
moderate; the operator triages each proposal. Here it's used mainly as a
validation cross-check on hand-authored edges.

Ported from ElastiCacheServerlessPEBrain/tools/keg. Adaptations: root
default `wiki`; `calls` and `configures` rules added.

CLI:
  python3 wiki/keg/extract_relations.py [--root wiki]
      [--dry-run] [--apply] [--kind KIND]
      [--min-confidence FLOAT] [--limit N] [--format text|json]
"""
from __future__ import annotations

import argparse
import json
import re
import sys
from dataclasses import dataclass, asdict
from pathlib import Path

try:
    import yaml
except ImportError:
    sys.exit("PyYAML required: pip3 install pyyaml")


# Each rule: (regex, proposed_kind, confidence, source_type_constraint, target_type_constraint)
RULES: list[tuple[re.Pattern, str, float, str | None, str | None]] = [
    (re.compile(r"\b(this\s+(rollup\s+)?supersedes?|"
                r"this\s+is\s+the\s+canonical\s+replacement\s+for|"
                r"now\s+the\s+canonical\b)", re.I),
     "supersedes", 0.95, None, None),

    (re.compile(r"\b(depends?\s+(on|upon)|"
                r"requires?\s+[\w-]+\s+to\s+function|"
                r"can'?t\s+function\s+without|"
                r"cannot\s+(start|run|operate)\s+without|"
                r"runtime\s+dependenc(y|ies))\b", re.I),
     "depends_on", 0.85, None, None),

    # calls — runtime call/dispatch phrasing (code-architecture addition)
    (re.compile(r"\b(calls?\s+(into\s+)?[\w./-]+|"
                r"invokes?\s+[\w./-]+|"
                r"dispatches?\s+to|"
                r"submits?\s+(to|via)\s+[\w./-]+|"
                r"hands?\s+off\s+to)\b", re.I),
     "calls", 0.80, None, None),

    # configures — config/arg drives a component (code-architecture addition)
    (re.compile(r"\b(configures?\s+[\w./-]+|"
                r"is\s+configured\s+by|"
                r"config(uration)?\s+(directive|arg(ument)?|option)\s+for|"
                r"tunes?\s+[\w./-]+)\b", re.I),
     "configures", 0.75, None, None),

    (re.compile(r"\b(is\s+(the\s+)?implementation\s+of|"
                r"is\s+implemented\s+by|"
                r"is\s+(the\s+)?reali[sz]ation\s+of|"
                r"reali[sz]es?\s+(the\s+)?[\w/-]+(\s+[\w/-]+)?\s+(concept|abstraction|pattern)|"
                r"implements?\s+(the\s+)?[\w/-]+(\s+[\w/-]+)?\s+(concept|abstraction|pattern))\b",
                re.I),
     "implements", 0.85, "concept", "entity"),

    (re.compile(r"\b(mitigates?\s+(the\s+|this\s+)?[\w/-]+\s+(threat|risk|attack|"
                r"vulnerability|class|surface)|"
                r"mitigation\s+(of|against|for)\s+[\w/-]+|"
                r"this\s+(control\s+)?protects?\s+against)\b", re.I),
     "mitigates", 0.80, None, None),

    (re.compile(r"\b(blocks?\s+(the\s+|this\s+)?(question|decision|launch|"
                r"GA|milestone|delivery)|"
                r"is\s+blocking\s+(the\s+|this\s+)?(question|decision|launch|"
                r"GA|milestone))\b", re.I),
     "blocks", 0.80, None, None),

    (re.compile(r"\b(owns?\s+(the\s+|this\s+|a\s+)?[\w-]+|"
                r"is\s+the\s+owner\s+(of|for)|"
                r"directly\s+responsible\s+for\s+[\w-]+|"
                r"team\s+lead\s+for\s+[\w-]+)\b", re.I),
     "owns", 0.85, "entity", None),

    (re.compile(r"\b(consists?\s+of|"
                r"is\s+composed\s+of|"
                r"is\s+(made\s+up|comprised)\s+of|"
                r"includes?\s+the\s+following\s+(component|sub-?part|element)|"
                r"\bcontains\s+(the\s+following|these)\b)", re.I),
     "contains", 0.70, None, None),

    (re.compile(r"\b(resolves?|answers?|closes?)\s+(the\s+|this\s+)?"
                r"(open\s+)?question\b", re.I),
     "resolves", 0.85, None, "question"),
]


@dataclass
class Proposal:
    from_page: str
    to_page: str
    current_kind: str
    proposed_kind: str
    confidence: float
    reason: str
    note: str | None = None
    raw_to: str | None = None

    def to_dict(self) -> dict:
        return asdict(self)


FRONTMATTER_RE = re.compile(r"\A---\n(.*?)\n---\n", re.S)


def load_pages_with_bodies(root: Path) -> dict[str, tuple[dict, str]]:
    if not root.is_dir():
        raise FileNotFoundError(f"wiki root not found: {root}")
    out: dict[str, tuple[dict, str]] = {}
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
        if not isinstance(fm, dict):
            continue
        body = text[m.end():]
        out[rel] = (fm, body)
    return out


def get_page_type(fm: dict) -> str | None:
    t = fm.get("type")
    if isinstance(t, str):
        return t.lower()
    return None


def extract_link_context(body: str, target_basename: str,
                         window_chars: int = 200) -> str:
    pat = re.compile(
        r"\[[^\]]+\]\([^)]*\b" + re.escape(target_basename) + r"\.md\)"
    )
    m = pat.search(body)
    if not m:
        return ""
    start = max(0, m.start() - window_chars)
    end = min(len(body), m.end() + window_chars)
    return body[start:end]


def propose_for_edge(from_page: str, to_page: str, current_kind: str,
                    note: str | None,
                    body: str,
                    source_type: str | None,
                    target_type: str | None,
                    target_status: str | None,
                    raw_to: str | None = None) -> Proposal | None:
    if current_kind != "refers_to":
        return None

    target_basename = Path(to_page).stem
    body_window = extract_link_context(body, target_basename)

    best: Proposal | None = None
    for rule_re, kind, conf, req_src_type, req_tgt_type in RULES:
        if req_src_type is not None and source_type != req_src_type:
            continue
        if req_tgt_type is not None and target_type != req_tgt_type:
            continue

        match_in = None
        if note and rule_re.search(note):
            match_in = ("note", rule_re.search(note))
        elif body_window and rule_re.search(body_window):
            match_in = ("body", rule_re.search(body_window))
            conf = max(0.0, conf - 0.1)
        if not match_in:
            continue

        if kind == "supersedes" and target_status != "superseded":
            continue

        location, m = match_in
        proposal = Proposal(
            from_page=from_page,
            to_page=to_page,
            current_kind=current_kind,
            proposed_kind=kind,
            confidence=conf,
            reason=f"matched '{m.group(0).strip()}' in {location}",
            note=note,
            raw_to=raw_to if raw_to is not None else to_page,
        )
        if best is None or proposal.confidence > best.confidence:
            best = proposal
    return best


def extract_proposals(pages: dict[str, tuple[dict, str]]) -> list[Proposal]:
    proposals: list[Proposal] = []
    fm_by_path = {p: fm for p, (fm, _) in pages.items()}

    import posixpath
    for rel, (fm, body) in pages.items():
        for e in fm.get("edges", []) or []:
            if not isinstance(e, dict):
                continue
            kind = e.get("kind")
            to = e.get("to")
            if not kind or not to:
                continue
            if kind != "refers_to":
                continue

            target = to
            if not target.startswith(("../", "./")) and target not in fm_by_path:
                base = posixpath.dirname(rel)
                cand = posixpath.normpath(posixpath.join(base, target))
                if cand in fm_by_path:
                    target = cand
            if target not in fm_by_path:
                continue

            target_fm = fm_by_path[target]
            target_type = get_page_type(target_fm)
            target_status = target_fm.get("status") if isinstance(
                target_fm.get("status"), str) else None

            note = e.get("note")
            source_type = get_page_type(fm)
            proposal = propose_for_edge(
                from_page=rel, to_page=target, current_kind=kind,
                note=note, body=body,
                source_type=source_type,
                target_type=target_type, target_status=target_status,
                raw_to=to,
            )
            if proposal:
                proposals.append(proposal)
    proposals.sort(key=lambda p: (-p.confidence, p.from_page, p.to_page))
    return proposals


def apply_proposal(root: Path, proposal: Proposal) -> bool:
    src_path = root / proposal.from_page
    text = src_path.read_text(encoding="utf-8")
    fm_match = re.match(r"\A(---\n)(.*?)(\n---\n)", text, re.S)
    if not fm_match:
        return False
    fm = fm_match.group(2)

    yaml_to = proposal.raw_to if proposal.raw_to is not None else proposal.to_page

    edge_block_re = re.compile(
        r"(\n  - to:\s*" + re.escape(yaml_to) + r"\n"
        r"(?:    [^\n]+\n)*?)"
        r"(    kind:\s*)" + re.escape(proposal.current_kind) + r"(\n)",
        re.M
    )
    m = edge_block_re.search(fm)
    if not m:
        return False
    new_kind_line = m.group(2) + proposal.proposed_kind + m.group(3)
    new_fm = fm[:m.start()] + m.group(1) + new_kind_line + fm[m.end():]

    edge_entry_re = re.compile(
        r"(\n  - to:\s*" + re.escape(yaml_to) + r"\n"
        r"(?:    [^\n]+\n)*)",
        re.M
    )
    m2 = edge_entry_re.search(new_fm)
    if not m2:
        return False
    block = m2.group(1)
    block = _set_yaml_field(block, "source", "llm_relation")
    block = _set_yaml_field(block, "needs_review", "true")
    new_fm = new_fm[:m2.start()] + block + new_fm[m2.end():]

    new_text = fm_match.group(1) + new_fm + fm_match.group(3) + text[fm_match.end():]
    src_path.write_text(new_text, encoding="utf-8")
    return True


def _set_yaml_field(edge_block: str, field_name: str, value: str) -> str:
    pat = re.compile(r"(\n    " + re.escape(field_name) + r":\s*)\S+")
    if pat.search(edge_block):
        return pat.sub(r"\g<1>" + value, edge_block)
    if edge_block.endswith("\n"):
        return edge_block[:-1] + f"\n    {field_name}: {value}\n"
    return edge_block + f"\n    {field_name}: {value}\n"


def render_text(proposals: list[Proposal]) -> str:
    if not proposals:
        return "(no proposals)"
    out = []
    by_kind: dict[str, list[Proposal]] = {}
    for p in proposals:
        by_kind.setdefault(p.proposed_kind, []).append(p)
    for kind in sorted(by_kind, key=lambda k: -len(by_kind[k])):
        out.append(f"\n=== {kind} ({len(by_kind[kind])}) ===")
        for p in by_kind[kind]:
            out.append(f"  {p.from_page}  →  {p.to_page}  (conf={p.confidence:.2f})")
            out.append(f"    reason: {p.reason}")
            if p.note:
                out.append(f"    note:   {p.note[:80]!r}")
    return "\n".join(out)


def render_json(proposals: list[Proposal]) -> str:
    return json.dumps([p.to_dict() for p in proposals], indent=2)


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter
    )
    p.add_argument("--root", type=Path, default=Path(__file__).resolve().parent.parent)
    p.add_argument("--dry-run", action="store_true", default=True,
                   help="default: print proposals without writing")
    p.add_argument("--apply", action="store_true",
                   help="rewrite YAML in place; sets source=llm_relation, "
                        "needs_review=true on each promoted edge")
    p.add_argument("--kind", help="filter to one proposed kind")
    p.add_argument("--min-confidence", type=float, default=0.6,
                   help="discard proposals below this confidence (default 0.6)")
    p.add_argument("--limit", type=int, help="cap number of results")
    p.add_argument("--format", choices=["text", "json"], default="text")
    args = p.parse_args(argv)

    pages = load_pages_with_bodies(args.root)
    proposals = extract_proposals(pages)

    proposals = [pr for pr in proposals if pr.confidence >= args.min_confidence]
    if args.kind:
        proposals = [pr for pr in proposals if pr.proposed_kind == args.kind]
    if args.limit:
        proposals = proposals[:args.limit]

    if args.format == "json":
        print(render_json(proposals))
    else:
        print(render_text(proposals))
        print(f"\nTotal proposals: {len(proposals)}")
        if not args.apply:
            print("(dry-run; pass --apply to rewrite YAML)")

    if args.apply:
        applied = 0
        failed = []
        for proposal in proposals:
            if apply_proposal(args.root, proposal):
                applied += 1
            else:
                failed.append(proposal)
        print(f"\nApplied: {applied}/{len(proposals)}")
        if failed:
            print(f"Failed: {len(failed)}")
            for f in failed[:5]:
                print(f"  {f.from_page} → {f.to_page}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
