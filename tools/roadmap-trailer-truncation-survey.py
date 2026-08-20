#!/usr/bin/env python3
"""Measure the trailer-column residue ANTS-4585 has to repair.

Two migration-era defects live in the store's trailer columns, and telling them
apart is the whole difficulty:

  * TRUNCATED   a `layman` / `source` value that was cut short at import. The
                full text still survives in the item's own body, because the
                legacy inline run was never stripped, so it is repairable.
  * DUPLICATED  the item states its metadata twice — once as that inline run,
                once as the rendered trailer. Stripping the run removes the
                duplication and would destroy the only copy of the truncated
                text, which is why ANTS-4585 fixes them in that order.

A value is truncated iff it appears verbatim after its own key in the inline
run AND the next character is neither its sentence stop nor the following
trailer key. Bracket imbalance is NOT the detector: measured 2026-08-20 it
found 88 values, missing most real truncations while flagging intact ones.

Both forms of the run count — `Kind: x` and `**Layman:** x`. Matching only the
bare form under-counts the duplication population by an order of magnitude
(123 against a true 3461).

READ-ONLY. Opens the store with mode=ro and writes nothing.

Usage:
    tools/roadmap-trailer-truncation-survey.py [--store PATH] [--samples N]
"""

import argparse
import collections
import re
import sqlite3
import sys

DEFAULT_STORE = "~/.local/share/ants-terminal/roadmap.sqlite"
KEYS = ("Kind", "Source", "Layman", "Lanes", "Evidence")
NEXT_KEY = re.compile(r"^(?:%s):\s" % "|".join(KEYS))
ANY_KEY = re.compile(r"(?:^|\s)(?:%s):\s" % "|".join(KEYS))


def norm(s):
    """Collapse whitespace and drop bold markers.

    The wrap-truncation class only shows up once the body's newlines are
    flattened, and the bold form is half the corpus.
    """
    return re.sub(r"\s+", " ", (s or "").replace("**", "")).strip()


def survey(store, sample_cap):
    con = sqlite3.connect("file:%s?mode=ro" % store, uri=True)
    rows = con.execute(
        "SELECT p.root, i.id, i.layman, i.source, i.body "
        "FROM item i JOIN project p USING(project_id)"
    ).fetchall()

    stats = collections.defaultdict(collections.Counter)
    samples = collections.defaultdict(list)

    for root, iid, layman, source, body in rows:
        nb = norm(body)
        if ANY_KEY.search(nb):
            stats[root]["dup_inline_run"] += 1
        for key, val in (("Layman", layman), ("Source", source)):
            if not val:
                continue
            needle = "%s: %s" % (key, norm(val))
            idx = nb.find(needle)
            if idx < 0:
                # No verbatim copy: either no run for this key, or the value
                # was mangled beyond a prefix match. Not repairable from prose.
                if ANY_KEY.search(nb):
                    stats[root]["no_verbatim_match"] += 1
                continue
            tail = nb[idx + len(needle):]
            if tail == "" or tail[0] == "." or NEXT_KEY.match(tail.lstrip()):
                stats[root]["intact"] += 1
                continue
            stats[root]["truncated"] += 1
            stats[root]["chars_lost"] += len(tail.split(". ")[0])
            if len(samples[root]) < sample_cap:
                samples[root].append(
                    "%s %s=%r + %r" % (iid, key, norm(val)[-46:], tail[:52]))

    return rows, stats, samples


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--store", default=DEFAULT_STORE)
    ap.add_argument("--samples", type=int, default=2)
    args = ap.parse_args()

    import os
    store = os.path.expanduser(args.store)
    if not os.path.exists(store):
        sys.exit("no store at %s" % store)

    rows, stats, samples = survey(store, args.samples)

    head = ("project", "dupRun", "TRUNC", "lost~", "intact")
    print("%-46s %7s %7s %7s %7s" % head)
    print("-" * 78)
    total = collections.Counter()
    for root in sorted(stats, key=lambda r: -stats[r]["truncated"]):
        c = stats[root]
        total.update(c)
        print("%-46s %7d %7d %7d %7d" % (root[-46:], c["dup_inline_run"],
              c["truncated"], c["chars_lost"], c["intact"]))
    print("-" * 78)
    print("%-46s %7d %7d %7d %7d" % ("TOTAL of %d items" % len(rows),
          total["dup_inline_run"], total["truncated"], total["chars_lost"],
          total["intact"]))
    print("\nvalues with an inline run but no verbatim match: %d"
          % total["no_verbatim_match"])
    for root in sorted(samples):
        print("--- %s" % root)
        for s in samples[root]:
            print("    " + s)


if __name__ == "__main__":
    main()
