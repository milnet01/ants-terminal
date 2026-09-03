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
# ANTS-3770 - any leading bracketed token, recognised as an id or not. Mirrors
# the reader's rxLeadToken: what makes a bullet an ITEM is the bold headline
# after the token, and refusing to look past a MALFORMED token is what made
# this scan under-count.
ID_LEAD_TOKEN = re.compile(r"\[[^\]]{1,64}\]")

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


# A `Kind:` trailer is written inline — trailing a prose sentence rather than
# on its own line — often enough that an anchored matcher misses whole values.
# This pattern was `^\s+…$`, the same blind spot rxKind() has in
# src/roadmapparse.cpp (ANTS-4065 § 2.2), and it is why every earlier run of
# this survey reported no `bug` at all: all 29 write the field inline. The
# survey is the evidence base for roadmap-data-model.md § 7.4's mapping, so an
# undercount here becomes a missing mapping there.
#
# Un-anchored within the line, with three guards, each mirroring the contract:
#   - ANTS-3722's backtick lookbehind, so a bullet *quoting* the label does not
#     count as declaring it;
#   - case-SENSITIVE (no re.I), so prose — "…changed the kind: of work…" —
#     does not match. § 2.2 drops the tolerance ANTS-3407 added for exactly
#     this reason once the anchor goes;
#   - the caller requires an indented continuation line, which keeps a headline
#     *about* the field (ANTS-4062's, for one) from parsing as a declaration.
# Last match wins, per INV-11: a rendered bullet puts the body before the
# trailer, so the canonical value is the later occurrence.
# `+` is in the class because three corpus values are compounds joined by it
# (`process + tooling`, `design + implement`, `design + fix`); without it they
# do not match at all and the inventory loses them silently.
KIND_VALUE = re.compile(r"(?<!`)(?:\*\*)?Kind:(?:\*\*)?\s*([A-Za-z0-9 _/+-]+?)\s*(?:\.|$)")

# Un-anchoring admits one false positive the parser tolerates but a measurement
# must not: prose that capitalises the label mid-sentence ("…the value Kind:
# happens to sit at the line start…") parses as a declaration. § 2.2 accepts
# that residue for the parser, on the grounds that narrowing further would
# re-introduce the anchor. Here it is bounded by shape instead: every real
# value in the corpus is at most four words and 30 characters, the longest
# being `process + tooling`. Anything longer is counted as prose and REPORTED,
# never dropped in silence — a filtered inventory that under-reports would
# reproduce the very failure this survey exists to measure.
KIND_MAX_WORDS, KIND_MAX_CHARS = 4, 30
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


def roadmap_in(directory):
    """The project's roadmap file, matched case-INSENSITIVELY.

    RetroDB names its roadmap `roadmap.md`. An uppercase-only glob silently
    excluded a 4,800-line project from the first survey, which then reported
    a corpus figure and an "no project uses pass headings" claim that were
    both wrong. Filename case is not a reason to be invisible.
    """
    try:
        entries = os.listdir(directory)
    except OSError:
        return None
    for entry in sorted(entries):
        if entry.lower() == "roadmap.md" and \
                os.path.isfile(os.path.join(directory, entry)):
            return os.path.join(directory, entry)
    return None


def find_roadmaps(roots):
    found = []
    for root in roots:
        own = roadmap_in(root)
        if own:
            found.append((os.path.basename(os.path.abspath(root)), own))
            continue
        for entry in sorted(os.listdir(root)):
            path = os.path.join(root, entry)
            if os.path.isdir(path):
                hit = roadmap_in(path)
                if hit:
                    found.append((entry, hit))
    return found


def survey(path):
    """Return per-project counters for one ROADMAP.md."""
    with open(path, encoding="utf-8", errors="replace") as fh:
        lines = fh.read().split("\n")
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

        # roadmap-format.md § 3.10.5: a third shape, where the item is a
        # level-4 heading and the status is a free-text sub-bullet. It has no
        # bullet and no status emoji, so every counter below is blind to it.
        if re.match(r"^####\s+Pass\s+\d+\.\d+", line):
            c["pass_heading_items"] += 1
        st_line = re.match(r"^\s*-\s+\*\*Status\*\*\s*:\s*(.+?)\s*$", line)
        if st_line:
            c["pass_status_lines"] += 1
            word = st_line.group(1).lower().lstrip("*").split()[0].strip(".,—-")
            if word not in ("planned", "in-progress", "shipped", "considered",
                            "dropped"):
                c["pass_status_off_enum"] += 1

        if re.match(r"^\s*\|.*\|\s*$", line):
            if re.match(r"^\s*\|[\s:|-]+\|\s*$", line):
                c["table_separator_rows"] += 1
            else:
                c["table_data_rows"] += 1

        # Body continuation lines only (see KIND_VALUE), last match wins.
        kv = None
        if line[:1].isspace():
            for kv in KIND_VALUE.finditer(line):
                pass
        if kv:
            val = kv.group(1).strip().lower()
            if not val:
                # Prose quoting the label -- `the "Kind: ..." line` -- matches
                # with an empty capture, since the value class excludes `.` and
                # stops before the ellipsis. Not a declaration; not malformed.
                c["kind_rejected_as_prose"] += 1
            elif len(val) > KIND_MAX_CHARS or len(val.split()) > KIND_MAX_WORDS:
                c["kind_rejected_as_prose"] += 1
            else:
                kinds[val] += 1
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
        # ANTS-3770 — the headline test must see past ANY leading `[token]`,
        # not only a RECOGNISED id. The reader's own rxLeadToken strips the
        # bracketed token whatever is inside it, so a bullet carrying an
        # unrecognised token AND a bold headline is an item by
        # roadmap-data-model.md 7.2 — and this scan was failing it into
        # `status_no_id_no_headline`, i.e. counting it as detail belonging to
        # the item above.
        #
        # `tail` feeds ONLY the headline discriminator below. The id
        # classification still keys on `dashed` / `anyid`, so an unrecognised
        # token stays off-grammar rather than being promoted to an id.
        lead = (dashed or anyid) or ID_LEAD_TOKEN.match(after)
        tail = after[lead.end():].lstrip() if lead else after

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
    if total["pass_heading_items"]:
        print(f"\npass-headings roadmaps (roadmap-format § 3.10.5)")
        print(f"  `#### Pass N.M` items               {total['pass_heading_items']}")
        print(f"  `- **Status**:` lines               {total['pass_status_lines']}")
        print(f"  ...whose value is OUTSIDE the five-status enum  "
              f"{total['pass_status_off_enum']}")

    print(f"\n{len(roadmaps)} projects, {n} bullet-form + checkbox items")
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
        print(f"  {k or '(empty — value ran onto the next line)':22s} {v}")
    if total["kind_rejected_as_prose"]:
        print(f"  ...plus {total['kind_rejected_as_prose']} match(es) rejected "
              f"as prose (over {KIND_MAX_WORDS} words or {KIND_MAX_CHARS} chars)")

    print("\nfield keys in use (top 12)")
    for k, v in keys.most_common(12):
        print(f"  {k:22s} {v}")
    print(f"  ({len(keys)} distinct keys)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
