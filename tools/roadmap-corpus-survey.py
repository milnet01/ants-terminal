#!/usr/bin/env python3
"""Measure the roadmap corpus that docs/standards/roadmap-data-model.md rests on.

Every figure that standard quotes is produced here, so a reader can re-derive
it instead of trusting a hand count. Hand counts were wrong: the first survey
for ANTS-3753 under-counted the corpus by ~14% and invented five `Kind:` values
that appear nowhere, because its regex could not express undashed or
digit-initial ID prefixes.

Usage:
    tools/roadmap-corpus-survey.py [ROOT ...]

ROOT is a directory holding projects (each with a ROADMAP.md), or a project
directory itself. Default: the parent of this repo, i.e. the sibling projects
this machine's corpus is made of.
"""

import argparse
import collections
import os
import re
import sys

STATUS = "✅🚧📋💭"

# roadmap-format.md § 3.5.1 / § 3.10.4: a letter-containing prefix, then a
# dash, then digits. This is the grammar the tooling actually enforces.
ID_DASHED = re.compile(r"\[(?=[A-Za-z0-9_-]*[A-Za-z])[A-Za-z0-9][A-Za-z0-9_-]*-\d+\]")
# The same, with the dash optional — catches `[Cl9]` / `[CE18]`, which read as
# IDs to a human but do NOT match the grammar above. Counted separately so the
# gap stays visible rather than being silently absorbed.
ID_ANY = re.compile(r"\[(?=[A-Za-z0-9_-]*[A-Za-z])[A-Za-z0-9][A-Za-z0-9_-]*?-?\d+\]")

# roadmap-format.md § 3.5.3's 21-value enum.
CANONICAL_KINDS = {
    "implement", "fix", "audit-fix", "review-fix", "doc", "doc-fix", "refactor",
    "test", "chore", "release", "perf", "security", "feature", "enhancement",
    "investigate", "research", "accessibility", "optimize", "package",
    "marketing", "ux",
}

# Fields are written both bare (`Kind: fix.`) and bold (`**Kind:** fix.`).
# Matching only the bare form loses every bullet written by the dialog.
def field_re(name):
    return re.compile(r"^\s+(?:\*\*)?" + name + r":(?:\*\*)?\s", re.I)


KIND_VALUE = re.compile(
    r"^\s+(?:\*\*)?Kind:(?:\*\*)?\s*([A-Za-z0-9 _/-]+?)\.?\s*$", re.I
)
FIELD_KEY = re.compile(r"^\s+(?:\*\*)?([A-Z][A-Za-z ]{1,20}):(?:\*\*)?\s")


def fenced(lines):
    """Mark lines inside (or opening/closing) a fenced code block."""
    mask, inside = [False] * len(lines), False
    for i, line in enumerate(lines):
        if re.match(r"^\s*```", line):
            mask[i], inside = True, not inside
        else:
            mask[i] = inside
    return mask


def find_roadmaps(roots):
    found = []
    for root in roots:
        if os.path.isfile(os.path.join(root, "ROADMAP.md")):
            found.append((os.path.basename(os.path.abspath(root)),
                          os.path.join(root, "ROADMAP.md")))
            continue
        for entry in sorted(os.listdir(root)):
            path = os.path.join(root, entry, "ROADMAP.md")
            if os.path.isfile(path):
                found.append((entry, path))
    return found


def survey(path):
    """Return per-project counters for one ROADMAP.md."""
    lines = open(path, encoding="utf-8", errors="replace").read().split("\n")
    mask = fenced(lines)
    c = collections.Counter()
    kinds = collections.Counter()
    keys = collections.Counter()
    prefixes = collections.Counter()
    off_grammar = collections.Counter()
    item = None

    def close():
        nonlocal item
        if item is not None:
            c["items"] += 1
            for f in ("Kind", "Source", "Layman"):
                if item.get(f):
                    c["has_" + f] += 1
        item = None

    for i, line in enumerate(lines):
        if re.match(r"^\s*```", line):
            c["fence_lines"] += 1
        if mask[i]:
            continue

        if re.match(r"^\s*\|.*\|\s*$", line):
            if re.match(r"^\s*\|[\s:|-]+\|\s*$", line):
                c["table_separator_rows"] += 1
            else:
                c["table_data_rows"] += 1

        kv = KIND_VALUE.match(line)
        if kv:
            kinds[kv.group(1).strip().lower()] += 1
            if item is not None:
                item["Kind"] = True
        fk = FIELD_KEY.match(line)
        if fk:
            keys[fk.group(1)] += 1
        for f in ("Source", "Layman"):
            if item is not None and field_re(f).match(line):
                item[f] = True

        m = re.match(r"^(\s*)-\s+(.*)$", line)
        if not m:
            continue
        indent, rest = m.group(1), m.group(2)
        status = rest[0] if rest and rest[0] in STATUS else None
        gfm = re.match(r"^\[([ xX])\]\s", rest)
        if status is None and not gfm:
            if indent:
                c["sub_bullets"] += 1
            continue

        after = (rest[1:] if status else rest[3:]).lstrip()
        dashed, anyid = ID_DASHED.match(after), ID_ANY.match(after)
        tail = after[(dashed or anyid).end():].lstrip() if (dashed or anyid) else after

        # A status bullet with neither an ID nor a bold headline is not an
        # item: it is either detail belonging to the item above it, or a
        # status-legend line. roadmap-format.md § 3.5 makes the bold headline
        # the discriminator.
        if status and not (dashed or anyid) and not tail.startswith("**"):
            c["status_no_id_no_headline"] += 1
            if re.match(r"^(Done|In progress|Planned|Considered)\b", after, re.I) \
                    and len(after) < 160:
                c["status_legend_lines"] += 1
            continue

        close()
        item = {}
        if dashed:
            c["id_dashed"] += 1
            prefixes[dashed.group(0)[1:-1].rsplit("-", 1)[0]] += 1
        elif anyid:
            c["id_off_grammar"] += 1
            off_grammar[re.sub(r"\d+$", "", anyid.group(0)[1:-1]).rstrip("-")] += 1
        else:
            c["id_none"] += 1
            closed = status == "✅" or (gfm and gfm.group(1).lower() == "x")
            c["id_none_closed" if closed else "id_none_open"] += 1
    close()
    return c, kinds, keys, prefixes, off_grammar


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("roots", nargs="*", help="project dirs, or dirs holding projects")
    args = ap.parse_args()
    roots = args.roots or [os.path.dirname(
        os.path.dirname(os.path.dirname(os.path.abspath(__file__))))]

    roadmaps = find_roadmaps(roots)
    if not roadmaps:
        print("no ROADMAP.md found under: " + ", ".join(roots), file=sys.stderr)
        return 1

    total = collections.Counter()
    kinds = collections.Counter()
    keys = collections.Counter()
    prefixes = collections.Counter()
    off_grammar = collections.Counter()

    print(f"{'project':22s}{'items':>8s}{'id':>8s}{'no-id':>8s}{'sub':>8s}")
    for name, path in roadmaps:
        c, k, f, p, og = survey(path)
        total.update(c); kinds.update(k); keys.update(f)
        prefixes.update(p); off_grammar.update(og)
        print(f"{name:22s}{c['items']:>8d}{c['id_dashed']:>8d}"
              f"{c['id_none']:>8d}{c['sub_bullets']:>8d}")

    n = total["items"]
    print(f"\n{len(roadmaps)} projects, {n} items")
    print(f"  with an ID matching roadmap-format § 3.5.1   {total['id_dashed']}")
    print(f"  with an ID that does NOT match that grammar  {total['id_off_grammar']}"
          f"   {dict(off_grammar)}")
    print(f"  with no ID                                   {total['id_none']}"
          f"   (closed {total['id_none_closed']} / open {total['id_none_open']})")

    print("\nfield absence, per item")
    for f in ("Kind", "Source", "Layman"):
        missing = n - total["has_" + f]
        print(f"  no {f:7s} {missing:6d}  ({missing * 100.0 / n:.0f}%)")

    print("\nstructures the model must survive")
    detail = total["status_no_id_no_headline"] - total["status_legend_lines"]
    print(f"  sub-bullets                        {total['sub_bullets']}")
    print(f"  status-marked detail lines         {detail}")
    print(f"  status-legend lines                {total['status_legend_lines']}")
    print(f"  table data rows                    {total['table_data_rows']}"
          f"   (+{total['table_separator_rows']} separator rows, not rows)")
    print(f"  fenced code blocks                 {total['fence_lines'] // 2}"
          f"   ({total['fence_lines']} fence lines)")

    non_canonical = {k: v for k, v in kinds.items() if k not in CANONICAL_KINDS}
    print(f"\nKind: values — {len(kinds)} distinct "
          f"({len(set(kinds) & CANONICAL_KINDS)} canonical + {len(non_canonical)} not)")
    for k, v in sorted(non_canonical.items(), key=lambda x: -x[1]):
        print(f"  {k:22s} {v}")

    print("\nfield keys in use (top 12)")
    for k, v in keys.most_common(12):
        print(f"  {k:22s} {v}")
    print(f"  ({len(keys)} distinct keys)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
