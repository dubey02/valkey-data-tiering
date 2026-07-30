#!/usr/bin/env python3
"""
verify_citations.py - Layer-1 citation + symbol-grounding check for the data tiering wiki.

Mechanical (not semantic) verification. For every wiki page that has a
`sources:` frontmatter block it checks:

  1. CITATIONS  - every `file:line` / `file:start-end` reference (in `sources:`
                  and inline `` `path:line` ``) resolves to a real file and the
                  line range is in-bounds.  Out-of-range or missing => ERROR.
  2. STALENESS  - WARN if a cited source file's last git commit date is newer than the page's
                  `updated:` date (code may have moved past the page).
  3. SYMBOLS    - every distinctive identifier / snake_case / CamelCase /
                  hyphenated-directive written in inline-code on the page must
                  appear verbatim somewhere in the repo's C/Rust sources.
                  Not found anywhere      => ERROR (likely fabricated or
                                              imported from another codebase).
                  Found in repo but not in
                  THIS page's cited files  => WARN (citation may be incomplete).

This does NOT judge whether the prose *means* what the code means - that needs a
human or a fresh-agent review. It is a fast first filter for rot and fabrication.

Exit status 1 if any ERROR is found (CI gate). Stdlib only.

Usage:
  python3 .agent/wiki/keg/verify_citations.py                # all sourced pages
  python3 .agent/wiki/keg/verify_citations.py interfaces/*.md # specific pages
"""
import os
import re
import subprocess
import sys
from pathlib import Path
from datetime import datetime

WIKI_ROOT = Path(__file__).resolve().parent.parent     # .agent/wiki
REPO_ROOT = WIKI_ROOT.parent.parent                     # repo root (has src/)
CODE_DIRS = ["src", "modules"]
CODE_EXT = {".c", ".h", ".rs"}

# `path.ext:line` or `path.ext:start-end`
CITE_RE = re.compile(r'`([A-Za-z0-9_./-]+\.(?:c|h|rs|md)):(\d+)(?:-(\d+))?`')
# bare `:line` / `:start-end` -> resolved against the page's primary code source
BARE_RE = re.compile(r'`:(\d+)(?:-(\d+))?`')
# any inline-code span
SPAN_RE = re.compile(r'`([^`\n]+)`')
# identifier / snake / hyphenated-directive token inside a span
TOKEN_RE = re.compile(r'[A-Za-z_][A-Za-z0-9_]*(?:-[A-Za-z0-9_]+)*')
# a span that is itself a citation (skip for symbol grounding)
CITE_SPAN_RE = re.compile(r'^[A-Za-z0-9_./-]+\.(?:c|h|rs|md):\d|^:\d')
# a span that is a path to a non-code asset (skip for symbol grounding)
ASSET_SPAN_RE = re.compile(r'\.(?:png|svg|jpe?g|gif|webp|dot|mmd|json|html|md)\b')


def load_repo_corpus():
    """filename -> text for every C/Rust source under CODE_DIRS."""
    corpus = {}
    for d in CODE_DIRS:
        base = REPO_ROOT / d
        if not base.is_dir():
            continue
        for p in base.rglob("*"):
            if p.is_file() and p.suffix in CODE_EXT:
                try:
                    corpus[p] = p.read_text(errors="replace")
                except OSError:
                    pass
    return corpus


def split_frontmatter(text):
    if not text.startswith("---"):
        return "", text
    parts = text.split("---", 2)
    if len(parts) < 3:
        return "", text
    return parts[1], parts[2]


def parse_sources(fm):
    src, in_src = [], False
    for line in fm.splitlines():
        if re.match(r'^sources:\s*$', line):
            in_src = True
            continue
        if in_src:
            m = re.match(r'^\s*-\s*(\S+)', line)
            if m:
                src.append(m.group(1))
            elif re.match(r'^\S', line):   # next top-level key
                in_src = False
    return src


def parse_updated(fm):
    m = re.search(r'^updated:\s*(\d{4}-\d{2}-\d{2})', fm, re.M)
    if not m:
        return None
    return datetime.strptime(m.group(1), "%Y-%m-%d").date()


def resolve_path(name, sources):
    """Resolve a (possibly src/-less) cited path to a file under the repo."""
    cands = [name, f"src/{name}", f"modules/{name}"]
    base = os.path.basename(name)
    for s in sources:
        sp = s.split(":")[0]
        if os.path.basename(sp) == base:
            cands.append(sp)
    for c in cands:
        fp = REPO_ROOT / c
        if fp.is_file():
            return fp
    return None


_linecount_cache = {}


def line_count(fp):
    if fp not in _linecount_cache:
        try:
            with open(fp, "rb") as f:
                _linecount_cache[fp] = sum(1 for _ in f)
        except OSError:
            _linecount_cache[fp] = 0
    return _linecount_cache[fp]


_changedate_cache = {}


def source_change_date(fp):
    """Date a cited source last actually changed.

    Filesystem mtime is useless here: a fresh clone or a branch checkout stamps
    every file with the checkout time, which made every page look stale at once.
    Ask git for the file's last commit date instead, and fall back to mtime only
    for files git does not track (or when git is unavailable).
    """
    if fp in _changedate_cache:
        return _changedate_cache[fp]
    result = None
    try:
        out = subprocess.run(
            ["git", "-C", str(REPO_ROOT), "log", "-1", "--format=%cI", "--", str(fp)],
            capture_output=True, text=True, timeout=15,
        )
        stamp = out.stdout.strip()
        if out.returncode == 0 and stamp:
            result = datetime.fromisoformat(stamp).date()
    except (OSError, ValueError, subprocess.SubprocessError):
        result = None
    if result is None:
        try:
            result = datetime.fromtimestamp(fp.stat().st_mtime).date()
        except OSError:
            result = None
    _changedate_cache[fp] = result
    return result


def is_checkable(tok):
    """Is this token distinctive enough to demand grounding (low false-positive)?"""
    if len(tok) < 5:
        return False
    if "-" in tok or "_" in tok:
        return True
    if any(c.isupper() for c in tok[1:]):    # camelCase / PascalCase
        return True
    return False


def check_page(page, repo_corpus):
    text = page.read_text(errors="replace")
    fm, body = split_frontmatter(text)
    sources = parse_sources(fm)
    if not sources:
        return None  # not a sourced page; skip silently
    updated = parse_updated(fm)
    errors, warns = [], []

    code_sources = [s for s in sources
                    if os.path.splitext(s.split(":")[0])[1] in CODE_EXT]
    primary = code_sources[0].split(":")[0] if code_sources else None

    # ---- 1. citations (sources frontmatter + inline) ----
    cites = []  # (raw, path_name, start, end)
    for s in sources:
        if ":" in s:
            pth, _, rng = s.partition(":")
            m = re.match(r'(\d+)(?:-(\d+))?$', rng)
            if m:
                cites.append((s, pth, int(m.group(1)),
                              int(m.group(2) or m.group(1))))
    for m in CITE_RE.finditer(body):
        cites.append((m.group(0), m.group(1), int(m.group(2)),
                      int(m.group(3) or m.group(2))))
    for m in BARE_RE.finditer(body):
        if primary:
            cites.append((m.group(0), primary, int(m.group(1)),
                          int(m.group(2) or m.group(1))))
        else:
            warns.append(f"bare line ref {m.group(0)} but no code source to resolve against")

    for raw, name, start, end in cites:
        fp = resolve_path(name, sources)
        if fp is None:
            errors.append(f"citation {raw}: file not found ({name})")
            continue
        n = line_count(fp)
        if end > n:
            errors.append(f"citation {raw}: line {end} out of range "
                          f"({fp.relative_to(REPO_ROOT)} has {n} lines)")

    # ---- 2. staleness ----
    if updated:
        seen = set()
        for s in sources:
            pth = s.split(":")[0]
            fp = resolve_path(pth, sources)
            if not fp or fp in seen:
                continue
            seen.add(fp)
            mdate = source_change_date(fp)
            if mdate is None:
                continue
            if mdate > updated:
                warns.append(f"STALE? {fp.relative_to(REPO_ROOT)} changed {mdate} "
                             f"> page updated {updated} (re-verify citations)")

    # ---- 3. symbol grounding ----
    cited_paths = {resolve_path(s.split(":")[0], sources) for s in code_sources}
    cited_paths.discard(None)
    cited_text = "\n".join(repo_corpus.get(fp, "") for fp in cited_paths)

    checked = set()
    for span_m in SPAN_RE.finditer(body):
        span = span_m.group(1)
        if CITE_SPAN_RE.match(span) or ASSET_SPAN_RE.search(span):
            continue
        for tm in TOKEN_RE.finditer(span):
            tok = tm.group(0)
            if tok in checked or not is_checkable(tok):
                continue
            checked.add(tok)
            if tok in cited_text:
                continue
            in_repo = any(tok in t for t in repo_corpus.values())
            if in_repo:
                warns.append(f"symbol `{tok}`: not in cited sources "
                             f"(found elsewhere in repo - citation may be incomplete)")
            else:
                errors.append(f"symbol `{tok}`: NOT FOUND anywhere in src/ or modules/ "
                              f"(fabricated or from another codebase?)")

    return errors, warns


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("-")]
    if args:
        pages = []
        for a in args:
            p = Path(a)
            if not p.is_absolute():
                p = (Path.cwd() / a)
                if not p.exists():
                    p = WIKI_ROOT / a
            pages.append(p)
    else:
        pages = sorted(p for p in WIKI_ROOT.rglob("*.md")
                       if "keg" not in p.parts)

    repo_corpus = load_repo_corpus()
    total_err = total_warn = checked_pages = 0

    for page in pages:
        if not page.is_file():
            print(f"!! {page}: not found")
            continue
        res = check_page(page, repo_corpus)
        if res is None:
            continue
        errors, warns = res
        checked_pages += 1
        rel = page.resolve().relative_to(WIKI_ROOT) if str(page.resolve()).startswith(str(WIKI_ROOT)) else page
        status = "OK" if not errors and not warns else ("ERROR" if errors else "WARN")
        print(f"[{status}] {rel}")
        for e in errors:
            print(f"    ERROR  {e}")
        for w in warns:
            print(f"    warn   {w}")
        total_err += len(errors)
        total_warn += len(warns)

    print(f"\npages={checked_pages} errors={total_err} warnings={total_warn} "
          f"corpus_files={len(repo_corpus)}")
    sys.exit(1 if total_err else 0)


if __name__ == "__main__":
    main()
