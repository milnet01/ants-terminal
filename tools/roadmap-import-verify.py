#!/usr/bin/env python3
"""ANTS-4065 Phase D2's acceptance check: does the store hold the kind each
bullet DECLARES?

This is the check that would have caught the original 123 silent `kind`
rewrites, and it is the one a Phase E rollout runs against each project after
its `roadmap_migrate`. Reports; never writes.

    tools/roadmap-import-verify.py [PROJECT_ROOT] [--slug SLUG] [--store PATH]

Exit 0 when every declaring bullet matches, 1 on any mismatch, 2 on a setup
error (no store, no project row, no roadmap).

**The taxonomy and the alias map below MIRROR `src/roadmapparse.cpp`**
(`canonicalKinds()` / `mappedKind()`), and the resolver rule they implement is
ANTS-4065 § 2.2: a `Kind:` capture is a candidate only if its lowercased value
is a taxonomy value or an alias key, and the LAST surviving candidate wins.
That guard is why a later `roadmap_log op:annotate` note mentioning a kind can
no longer displace the real declaration. If either list changes in the C++,
change it here — an out-of-date copy makes this tool report false mismatches,
which is the loud failure, not a silent pass.
"""
import argparse, collections, pathlib, re, sqlite3, sys

TAXONOMY = {"implement", "fix", "audit-fix", "review-fix", "doc", "doc-fix",
            "refactor", "test", "chore", "release", "perf", "security",
            "feature", "enhancement", "investigate", "research",
            "accessibility", "optimize", "package", "marketing", "ux"}

ALIASES = {"improve": "enhancement", "docs": "doc", "bugfix": "fix",
           "testing": "test", "spike": "research", "feat": "feature",
           "enhance": "enhancement", "perf / fix": "perf",
           "perf / optimize": "perf", "tooling": "chore",
           "behaviour-change": "enhancement", "bug": "fix",
           "performance": "perf", "process + tooling": "chore",
           "audit": "audit-fix"}

BULLET = re.compile(r"^- (?:📋|🚧|✅|💭) \[([A-Za-z][A-Za-z0-9_-]*-\d+)\]")
KIND = re.compile(r"Kind:\s*\**\s*([^\n*]+)")


def declared_kinds(root):
    """id -> canonical kind, over ROADMAP.md and any rotated archives."""
    srcs = [root / "ROADMAP.md"]
    srcs += sorted((root / "docs/roadmap").glob("*.md"))
    out, seen_any = {}, False
    for src in srcs:
        if not src.exists():
            continue
        seen_any = True
        cur, block = None, []

        def flush():
            if cur is None:
                return
            vals = []
            for line in block:
                for raw in KIND.findall(line):
                    v = raw.strip().lower().rstrip(".,;")
                    if v in TAXONOMY:
                        vals.append(v)
                    elif v in ALIASES:
                        vals.append(ALIASES[v])
            if vals:
                out[cur] = vals[-1]          # § 2.2: last surviving candidate

        for line in src.read_text(encoding="utf-8").splitlines():
            m = BULLET.match(line)
            if m:
                flush()
                cur, block = m.group(1), [line]
            elif line.startswith("- ") or line.startswith("#"):
                flush()
                cur, block = None, []
            elif cur is not None:
                block.append(line)
        flush()
    return out, seen_any


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("root", nargs="?", default=".")
    ap.add_argument("--slug", help="project.export_slug (default: slugified leaf dir)")
    ap.add_argument("--store", default=str(pathlib.Path.home()
                    / ".local/share/ants-terminal/roadmap.sqlite"))
    a = ap.parse_args()

    root = pathlib.Path(a.root).resolve()
    slug = a.slug or root.name.lower().replace("_", "-")
    store = pathlib.Path(a.store)
    if not store.exists():
        print(f"no store at {store}", file=sys.stderr)
        return 2

    declared, found_src = declared_kinds(root)
    if not found_src:
        print(f"no ROADMAP.md under {root}", file=sys.stderr)
        return 2

    db = sqlite3.connect(f"file:{store}?mode=ro", uri=True)
    row = db.execute("SELECT project_id FROM project WHERE export_slug=?",
                     (slug,)).fetchone()
    if not row:
        have = [r[0] for r in db.execute("SELECT export_slug FROM project")]
        print(f"no project with export_slug={slug!r}; store has: {have}",
              file=sys.stderr)
        return 2
    pid = row[0]
    stored = dict(db.execute("SELECT id, kind FROM item WHERE project_id=?", (pid,)))

    absent = sorted(i for i in declared if i not in stored)
    bad = sorted((i, declared[i], stored[i]) for i in declared
                 if i in stored and stored[i] != declared[i])

    print(f"project            : {slug} (project_id {pid})")
    print(f"items in store     : {len(stored)}")
    print(f"bullets declaring  : {len(declared)}")
    print(f"declared, not in db: {len(absent)}" + (f" {absent[:10]}" if absent else ""))
    print(f"MISMATCHES         : {len(bad)}")
    for i, d, s in bad[:40]:
        print(f"  {i}: declares {d!r} -> stored {s!r}")
    if len(bad) > 40:
        print(f"  ... and {len(bad) - 40} more")

    hist = collections.Counter(stored.values())
    print("stored kinds       : " + ", ".join(f"{k}={n}" for k, n in hist.most_common(6)))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
