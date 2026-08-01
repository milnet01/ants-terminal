#!/usr/bin/env python3
"""Survey rotated roadmap archives — the source of every figure in
docs/specs/ANTS-3766-roadmap-migration-archives.md.

The spec's numbers are this script's output rather than a transcription, so a
reader can re-derive them instead of trusting them (drafting rule: "better than
citing the command: ship it").

Usage:
    tools/roadmap-archive-survey.py [PROJECT_ROOT] [CORPUS_PARENT]

    PROJECT_ROOT    the project to survey in detail   (default: repo root)
    CORPUS_PARENT   directory holding sibling project roots, for the
                    "how many projects have archives" figure
                    (default: PROJECT_ROOT's parent)

Sections A-D map to the spec: A -> § 1.1 archive inventory, B -> § 1.1 corpus
coverage, C -> § 1 item 2 and § 2.3 slug collisions, D -> § 2.3.1's counter
argument.
"""
from __future__ import annotations

import os
import re
import sys
from collections import Counter

# roadmap-format.md § 3.9 — case-sensitive, per-minor, no `v`, no zero-padding,
# no patch suffix. The standard's PROSE says all of that; the regex it prints
# (`^[0-9]+\.[0-9]+\.md$`) does not actually forbid zero-padding, so `00.07.md`
# would match it and collide with `0.7.md` on the same (0, 7) tuple. This is
# the tightened form ANTS-3766 § 2.2 adopts, which is what the prose describes.
ARCHIVE_RE = re.compile(r"^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.md$")
HEADING_RE = re.compile(r"^(#{2,3}) (.+)$")
BULLET_RE = re.compile(r"^- [\U0001F4CB\U0001F6A7✅\U0001F4AD] ")


def slugify(heading: str) -> str:
    """Mirror of RoadmapIndex::slugifyHeading (src/roadmapindex.cpp).

    Lowercases, keeps letters/numbers, collapses every other run to a single
    dash, strips trailing dashes. Emoji are not alphanumeric, so they vanish —
    which is why `### 🎨 Features` and a bare `### Features` collide.
    """
    out: list[str] = []
    prev_dash = True
    for ch in heading:
        low = ch.lower()
        if low.isalnum():
            out.append(low)
            prev_dash = False
        elif not prev_dash:
            out.append("-")
            prev_dash = True
    return "".join(out).rstrip("-")


def read_lines(path: str) -> list[str]:
    with open(path, encoding="utf-8") as fh:
        return fh.readlines()


def headings(path: str) -> list[tuple[int, str]]:
    res = []
    for line in read_lines(path):
        m = HEADING_RE.match(line)
        if m:
            res.append((len(m.group(1)), slugify(m.group(2).strip())))
    return res


def emoji_bullets(path: str) -> int:
    return sum(1 for line in read_lines(path) if BULLET_RE.match(line))


def live_roadmap(root: str) -> str | None:
    """ANTS-3757 § 2.2 — the file directly in the root case-folding to
    roadmap.md. Not recursive; a docs/ROADMAP.md is not a candidate."""
    try:
        for entry in sorted(os.listdir(root)):
            if entry.lower() == "roadmap.md" and os.path.isfile(os.path.join(root, entry)):
                return os.path.join(root, entry)
    except OSError:
        pass
    return None


def archives(root: str) -> list[str]:
    """Conforming archives, newest first — § 3.9's (major, minor) descending."""
    adir = os.path.join(root, "docs", "roadmap")
    if not os.path.isdir(adir):
        return []
    hits = [e for e in os.listdir(adir) if ARCHIVE_RE.match(e)]
    hits.sort(key=lambda e: tuple(int(p) for p in e[:-3].split(".")), reverse=True)
    return [os.path.join(adir, e) for e in hits]


def non_conforming(root: str) -> list[str]:
    adir = os.path.join(root, "docs", "roadmap")
    if not os.path.isdir(adir):
        return []
    return sorted(e for e in os.listdir(adir) if not ARCHIVE_RE.match(e))


def main() -> int:
    here = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    root = os.path.abspath(sys.argv[1]) if len(sys.argv) > 1 else here
    corpus = os.path.abspath(sys.argv[2]) if len(sys.argv) > 2 else os.path.dirname(root)

    live = live_roadmap(root)
    if live is None:
        print(f"no roadmap directly under {root}", file=sys.stderr)
        return 1

    arch = archives(root)
    print("=== A. archive inventory (spec § 1.1) ===")
    print(f"root: {root}")
    print(f"conforming archives ({ARCHIVE_RE.pattern}): {len(arch)}")
    for p in arch:
        print(f"  {os.path.relpath(p, root):24s} bytes={os.path.getsize(p):6d} "
              f"emoji_bullets={emoji_bullets(p):3d} sections={len(headings(p)):2d}")
    if arch:
        print(f"  total archive bytes: {sum(os.path.getsize(p) for p in arch)}")
    other = non_conforming(root)
    print(f"non-conforming entries in docs/roadmap/: {len(other)} {other}")

    print()
    print("=== B. corpus coverage (spec § 1.1) ===")
    roots_with_roadmap = 0
    roots_with_archives = 0
    roots_with_nonconforming = 0
    for name in sorted(os.listdir(corpus)):
        d = os.path.join(corpus, name)
        if not os.path.isdir(d) or live_roadmap(d) is None:
            continue
        roots_with_roadmap += 1
        n = len(archives(d))
        nc = len(non_conforming(d))
        has_dir = os.path.isdir(os.path.join(d, "docs", "roadmap"))
        if n:
            roots_with_archives += 1
        if nc:
            roots_with_nonconforming += 1
        print(f"  {name:28s} docs/roadmap={'yes' if has_dir else 'no ' :3s} "
              f"conforming={n} non_conforming={nc}")
    print(f"  roots with a live roadmap: {roots_with_roadmap}; "
          f"with >= 1 conforming archive: {roots_with_archives}; "
          f"with non-conforming entries: {roots_with_nonconforming}")

    print()
    print("=== C. slug collisions, archive vs live (spec § 1 item 2, § 2.3) ===")
    live_slugs = [s for _, s in headings(live)]
    live_set = set(live_slugs)
    print(f"live ## / ### headings: {len(live_slugs)}")
    total = 0
    for p in arch:
        a = [s for _, s in headings(p)]
        coll = [s for s in a if s in live_set]
        total += len(coll)
        print(f"  {os.path.relpath(p, root):24s} sections={len(a)} "
              f"colliding={len(coll)} {coll}")
    print(f"  TOTAL colliding archive sections: {total}")

    print()
    print("=== D. live repeats of each colliding slug (spec § 2.3.1) ===")
    counts = Counter(live_slugs)
    colliding = sorted({s for p in arch for s in (x for _, x in headings(p))
                        if s in live_set})
    for s in colliding:
        print(f"  {s:20s} live occurrences={counts[s]} "
              f"-> live pass emits {', '.join([s] + [f'{s}-{i}' for i in range(2, counts[s] + 1)])}")
    if not colliding:
        print("  (none)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
