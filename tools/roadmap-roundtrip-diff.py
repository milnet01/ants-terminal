#!/usr/bin/env python3
"""ANTS-4065 Phase D3's instrument: does a render -> re-import round trip change
the store? (INV-6.)

`roadmap_migrate` reports `items_updated` but not WHICH columns moved, and the
column is the whole diagnosis — D3 found `headline`/`lanes` clean and `body`
carrying all 363 diffs, the opposite of what ANTS-4065 § 2.6 predicted (cited
as "ANTS-3765 § 4" until 2026-08-15; that section is RAM / build cost).
Reports; never writes.

    # 1. before the render
    tools/roadmap-roundtrip-diff.py snap /tmp/before.json
    # 2. drive a real render (any roadmap_log write op), then roadmap_migrate
    # 3. after
    tools/roadmap-roundtrip-diff.py snap /tmp/after.json
    tools/roadmap-roundtrip-diff.py diff /tmp/before.json /tmp/after.json

Exit 0 when nothing moved, 1 when something did.

**Run this against a COMMITTED, clean tree.** Step 2 rewrites ROADMAP.md and
every rotated archive from the store, so `git checkout` is how you get back —
and a bullet the store has not imported is DELETED rather than reformatted
(ANTS-4141). Re-run `roadmap_migrate` after restoring, or the store keeps the
rendered values.

Cycle 2 matters as much as cycle 1: a single pass cannot tell "stable" from
"drifting slowly". D3 measured 363 then 2, which is what convergence looks
like.
"""
import argparse, collections, json, pathlib, sqlite3, sys

DEFAULT_STORE = pathlib.Path.home() / ".local/share/ants-terminal/roadmap.sqlite"
# ANTS-4065 § 2.6 governs nine columns; this list is NOT them, and the
# difference is deliberate rather than a transcription slip. It omits `id`
# (the match key) and `evidence` (not read here), and adds `section_id` and
# `id_origin`, which a re-import may write and which a drift hunt wants.
# Corrected 2026-08-15: this cited "ANTS-3765 § 2.4", which is that spec's
# list of nineteen store methods and defines no column set at all.
COLS = ["status", "headline", "layman", "kind", "source", "lanes", "body",
        "section_id", "id_origin"]


def connect(store, slug):
    db = sqlite3.connect(f"file:{store}?mode=ro", uri=True)
    row = db.execute("SELECT project_id FROM project WHERE export_slug=?",
                     (slug,)).fetchone()
    if not row:
        have = [r[0] for r in db.execute("SELECT export_slug FROM project")]
        sys.exit(f"no project with export_slug={slug!r}; store has: {have}")
    return db, row[0]


def snap(out, store, slug):
    db, pid = connect(store, slug)
    present = {r[1] for r in db.execute("PRAGMA table_info(item)")}
    use = [c for c in COLS if c in present]
    rows = {r[0]: r[1:] for r in
            db.execute(f"SELECT id,{','.join(use)} FROM item WHERE project_id=?", (pid,))}
    pathlib.Path(out).write_text(json.dumps({"slug": slug, "cols": use, "rows": rows}))
    print(f"snapshotted {len(rows)} items -> {out}")
    print(f"columns: {', '.join(use)}")


def diff(a, b):
    A = json.loads(pathlib.Path(a).read_text())
    B = json.loads(pathlib.Path(b).read_text())
    if A["cols"] != B["cols"]:
        sys.exit("snapshots have different columns — retake both")
    cols, ra, rb = A["cols"], A["rows"], B["rows"]
    gone, new = sorted(set(ra) - set(rb)), sorted(set(rb) - set(ra))

    per = collections.Counter()
    ex = collections.defaultdict(list)
    moved = set()
    for i in set(ra) & set(rb):
        for n, c in enumerate(cols):
            if ra[i][n] != rb[i][n]:
                per[c] += 1
                moved.add(i)
                if len(ex[c]) < 3:
                    ex[c].append((i, str(ra[i][n])[:120], str(rb[i][n])[:120]))

    print(f"items: before {len(ra)}  after {len(rb)}")
    print(f"  gone : {len(gone)}" + (f" {gone[:10]}" if gone else ""))
    print(f"  new  : {len(new)}" + (f" {new[:10]}" if new else ""))
    print(f"\nitems with any governed change: {len(moved)}")
    for c in cols:
        print(f"  {c:11s}: {per[c]}")
    for c in cols:
        for i, x, y in ex[c]:
            print(f"\n--- {c} / {i} ---\n  before: {x!r}\n  after : {y!r}")
    return 1 if (moved or gone or new) else 0


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("snap"); s.add_argument("out")
    s.add_argument("--slug", default="ants-terminal")
    s.add_argument("--store", default=str(DEFAULT_STORE))
    d = sub.add_parser("diff"); d.add_argument("before"); d.add_argument("after")
    a = ap.parse_args()
    if a.cmd == "snap":
        snap(a.out, pathlib.Path(a.store), a.slug)
        return 0
    return diff(a.before, a.after)


if __name__ == "__main__":
    sys.exit(main())
