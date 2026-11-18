#!/usr/bin/env python3
"""Report where a project's roadmap does not conform to roadmap-format.md.

Written for the roadmap -> store migration (ANTS-4065). Each project's roadmap
has to be conformant BEFORE it is imported, because once a project is migrated
the store renders the markdown and the file stops being able to correct itself.
This is the checklist that says whether a roadmap is ready.

It reports; it never edits. Every check here was derived from, and validated
against, this project's own 36k-line roadmap -- the largest in the corpus -- so
a smaller project should have strictly less to do.

Usage:
    tools/roadmap-conformance.py                 # this project
    tools/roadmap-conformance.py ../OtherProject # another project
    tools/roadmap-conformance.py --all ..        # every project under a dir
    tools/roadmap-conformance.py --verbose       # list every offending line

Exit codes:
    0  conformant (or nothing this tool governs)
    1  one or more findings
    2  setup error (path missing, no roadmap)
"""

import argparse
import pathlib
import re
import sys

from roadmap_kinds import CANONICAL_KINDS, KIND_VALUE, is_kind_shaped
from roadmap_paths import root_roadmap

STATUS = "📋🚧✅💭"
OPEN = "📋🚧💭"          # not-shipped; the render gate applies to these
BULLET = re.compile(r"^- ([" + STATUS + r"]) (?:\[([A-Za-z0-9_-]+-?\d+)\] )?\*\*(.*)$")
# A headline terminator. A period inside closing quotes still terminates.
TERM = (".", '."', ".”", ".'", ".)", "?", "!", '?"', '!"')

LAYMAN = re.compile(r"^\s*(?:\*\*)?Layman:(?:\*\*)?\s*\S", re.I)

# roadmap_log's input schema caps a headline at 200 (claudeintegration.cpp).
# Not a storage limit -- the column is TEXT with no constraint, and neither the
# import nor the render checks length -- so this is "the tool cannot rewrite
# this entry", not "this entry will not import".
HEADLINE_MAX = 200


def bold_headline(line):
    """The bullet's headline, honouring code spans.

    A naive first-`**`-to-next-`**` match truncates any headline that quotes a
    bold marker inside backticks -- including one quoting the C signature
    `char **argv`, where the asterisks are pointer syntax. Code spans are
    masked before matching, as the parser does.
    """
    masked = list(line)
    for m in re.finditer(r"`[^`]*`", line):
        for k in range(m.start(), m.end()):
            masked[k] = "\x00"
    m = re.search(r"\*\*(.+?)\*\*", "".join(masked), re.S)
    return line[m.start(1):m.end(1)] if m else None


def detect_dialect(text):
    """ants-v1 / gfm / pass-headings, per roadmap-format.md § 3.10.

    Only ants-v1 is governed here. The others are reported and skipped: the
    pass-headings status vocabulary is explicitly out of scope for the import
    contract, and a GFM task list is a different shape entirely.
    """
    if re.search(r"^####\s+Pass\s+\d+\.\d+", text, re.M):
        return "pass-headings"
    if re.search(r"^- [" + STATUS + r"] ", text, re.M):
        return "ants-v1"
    if re.search(r"^- \[[ xX]\] ", text, re.M):
        return "gfm"
    return "unknown"


def roadmap_files(root):
    """The project's roadmap plus any rotated archives."""
    live = root_roadmap(root)
    found = [pathlib.Path(live)] if live else []
    found += sorted((root / "docs" / "roadmap").glob("*.md"))
    return found


def check_file(path):
    text = path.read_text(encoding="utf-8", errors="replace")
    dialect = detect_dialect(text)
    findings = {k: [] for k in ("no_period", "multiline", "too_long",
                                "off_taxonomy", "no_layman")}
    if dialect != "ants-v1":
        return dialect, findings, 0

    lines = text.splitlines()
    bullets = 0
    for i, line in enumerate(lines):
        m = BULLET.match(line)
        if not m:
            continue
        bullets += 1
        status, iid = m.group(1), m.group(2) or "(no id)"
        where = f"{path}:{i + 1}"
        label = f"{where}  {iid}"

        head = bold_headline(line)
        if head is None:
            # Closing `**` is on a later line: a wrapped headline.
            findings["multiline"].append(label)
            continue
        # Headline truncation at a code span is tested by
        # tests/features/roadmap_headline_code_span/, not here.
        if status in OPEN and head.strip() and not head.rstrip().endswith(TERM):
            findings["no_period"].append(label)
        if len(head) > HEADLINE_MAX:
            findings["too_long"].append(f"{label}  ({len(head)} chars)")

        # Body: this bullet's continuation lines, up to the next bullet.
        end = next((k for k in range(i + 1, len(lines))
                    if lines[k].startswith("- ")), len(lines))
        body = lines[i:end]

        # Indented continuation lines only. Scanning the bullet line too would
        # read a headline that MENTIONS the label -- "...bullets whose Kind: is
        # outside the taxonomy." -- as declaring one, and the length guard below
        # is no help because "is outside the taxonomy" is short enough to pass.
        kind = None
        for bl in body:
            if not bl[:1].isspace():
                continue
            for last in KIND_VALUE.finditer(bl):
                kind = last.group(1).strip().lower()
        # An empty capture is prose quoting the label, never a malformed Kind;
        # reporting it would send someone to "fix" a correct sentence.
        if is_kind_shaped(kind):
            if kind not in CANONICAL_KINDS:
                findings["off_taxonomy"].append(f"{label}  Kind: {kind}")

        if status in OPEN and not any(LAYMAN.match(bl) for bl in body):
            findings["no_layman"].append(label)

    return dialect, findings, bullets


ORDER = [
    ("no_layman",
     "open items with no `Layman:` line",
     "BLOCKS EVERY WRITE once migrated (the render gate) -- fix this first"),
    ("off_taxonomy",
     "bullets whose `Kind:` is outside the 21-value taxonomy",
     "map each to a § 3.5.3 value; prefer fixing the text over teaching a synonym"),
    ("multiline",
     "headlines wrapped across more than one line",
     "§ 3.5 wants a one-line summary; join them, changing no words"),
    ("no_period",
     "open-item headlines with no terminating period",
     "check first whether the period is merely OUTSIDE the `**`, or the bullet "
     "uses a `**Headline**: body` label form -- appending one blindly is wrong "
     "for both"),
    ("too_long",
     f"headlines over {HEADLINE_MAX} characters",
     "cosmetic for import (no length limit exists in the store, importer or "
     "render) -- it only stops roadmap_log rewriting that entry. Split at a "
     "clause boundary, moving the remainder into the body so no text is lost"),
]


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("paths", nargs="*", default=["."],
                    help="project dir(s); default the current one")
    ap.add_argument("--all", action="store_true",
                    help="treat each path as a directory OF projects")
    ap.add_argument("--verbose", "-v", action="store_true",
                    help="list every offending line, not just counts")
    args = ap.parse_args()

    roots = []
    for p in args.paths:
        d = pathlib.Path(p).resolve()
        if not d.is_dir():
            print(f"roadmap-conformance: not a directory: {d}", file=sys.stderr)
            return 2
        roots += sorted(x for x in d.iterdir() if x.is_dir()) if args.all else [d]

    total = {k: 0 for k, _, _ in ORDER}
    checked = skipped = 0
    for root in roots:
        files = roadmap_files(root)
        if not files:
            if not args.all:
                print(f"roadmap-conformance: no roadmap under {root}", file=sys.stderr)
                return 2
            continue
        agg = {k: [] for k, _, _ in ORDER}
        dialects = set()
        bullets = 0
        for f in files:
            dialect, found, n = check_file(f)
            dialects.add(dialect)
            bullets += n
            for k in agg:
                agg[k] += found[k]
        governed = "ants-v1" in dialects
        n = sum(len(v) for v in agg.values())
        if not governed:
            skipped += 1
            print(f"\n{root.name}: SKIPPED — dialect {'/'.join(sorted(dialects))}, "
                  f"not governed by this contract")
            continue
        checked += 1
        print(f"\n{root.name}: {bullets} bullets, {n} finding(s)")
        for key, title, advice in ORDER:
            hits = agg[key]
            total[key] += len(hits)
            if not hits:
                continue
            print(f"  {len(hits):5d}  {title}")
            print(f"         → {advice}")
            if args.verbose:
                for h in hits:
                    print(f"           {h}")

    if len(roots) > 1:
        print(f"\n=== {checked} project(s) checked, {skipped} skipped ===")
        for key, title, _ in ORDER:
            if total[key]:
                print(f"  {total[key]:5d}  {title}")
    return 1 if any(total.values()) else 0


if __name__ == "__main__":
    sys.exit(main())
