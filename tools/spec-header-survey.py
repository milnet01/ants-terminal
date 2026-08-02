#!/usr/bin/env python3
"""Survey the spec corpus's `**Field:**` header blocks (ANTS-3785 / ANTS-3672).

Produces every corpus figure docs/specs/ANTS-3785-header-field-extent.md
quotes, so the spec's numbers are an output rather than a transcription
(docs/standards/documentation.md; precedent: tools/roadmap-corpus-survey.py).

The extent rule implemented here is the spec's § 2.1, and this script is the
parity oracle for src/specparse.cpp's headerField():

    a field runs from its `**Field:**` line through every following line up
    to, but excluding, the first blank line, the first further field marker,
    the first ATX heading, or EOF.

List bullets deliberately do NOT terminate a field -- see WRAPPED_PROSE_BULLET
below for the corpus case that settles it.

A second mode, --scope=docs-index (ANTS-3786 INV-9), surveys what docs_index
actually indexes -- <root>/*.md non-recursive plus <root>/docs/**/*.md
recursive -- and classifies each document by how this change moves its status.
It SIMULATES BOTH CODE PATHS rather than counting `**Status:**` lines, because
scanDoc does not look at every line: it skips any line over kMaxLineBytes and
every line inside a fenced block before its matcher runs, and its old matcher
required a non-empty same-line tail where headerField accepts an empty one.

Usage:  python3 tools/spec-header-survey.py [specs_dir]
        python3 tools/spec-header-survey.py --scope=docs-index [root]
Exit:   0 always (a survey, not a gate).
"""
import re
import sys
from pathlib import Path

FIELD = re.compile(r"^\*\*([^*:]+):\*\*\s*(.*)$")
TERMINATOR = re.compile(r"^\s*$|^\*\*[^*:]+:\*\*|^#{1,6}\s")
FENCE = re.compile(r"^ {0,3}(```+(?!.*`)|~~~+)")

# The corpus case that rejects `^[-*+]\s` as a terminator: this continuation
# line is prose ("...paginationengine.{h,cpp}` + `cmdRoadmapQuery`..."), not a
# list item. Treating a bullet as a terminator truncates it.
WRAPPED_PROSE_BULLET = "ANTS-1436.md"


def fence_mask(lines):
    """True where a line sits inside a fenced block."""
    mask, opener = [], None
    for line in lines:
        m = FENCE.match(line)
        if opener is None and m:
            opener, inside = m.group(1)[0], True
        elif opener is not None and m and m.group(1)[0] == opener:
            opener, inside = None, True
        else:
            inside = opener is not None
        mask.append(inside)
    return mask


def header_block_end(lines):
    """Index one past the header block -- the first `## ` heading, else EOF.

    The scan is BOUNDED because an unbounded one runs to EOF on a document
    with no such field and can then match a `**Status:**` quoted inside a
    later fenced example (spec § 2.2 / INV-7).
    """
    for i, line in enumerate(lines):
        if re.match(r"^##\s", line):
            return i
    return len(lines)


def field_extent(lines, i):
    """The field starting at `lines[i]`, as (line_count, joined_value).

    Mirrors SpecParse::headerField: line_count is 1 + continuation lines,
    and the value is the opener's trailing text plus each continuation,
    each stripped and joined with a single space.
    """
    head = FIELD.match(lines[i])
    parts = [head.group(2).strip()] if head else []
    j = i + 1
    while j < len(lines) and not TERMINATOR.match(lines[j]):
        parts.append(lines[j].strip())
        j += 1
    return j - i, " ".join(p for p in parts if p)


# A field NAME is short and title-shaped. A prose sentence that happens to wrap
# so a bold colon-run lands at column 0 would be read as a field by the extent
# rule (spec § 2.1's sharp edge) and shows up here as a prose-shaped "name".
# The rule cannot tell the two apart, so this census is the only evidence that
# no corpus spec is currently mis-split; every hit needs a human look.
def prose_shaped(name):
    return len(name.split()) > 3 or name[:1].islower()


def survey(specs_dir):
    total = {"Status": 0, "Kind": 0}
    wrapped = {"Status": [], "Kind": []}
    in_fence = []
    orphans = []
    field_names = {}
    fenced_header_block = []

    for path in sorted(specs_dir.glob("*.md")):
        lines = path.read_text(encoding="utf-8").split("\n")
        mask = fence_mask(lines)
        limit = header_block_end(lines)
        if any(mask[:limit]):
            fenced_header_block.append(path.name)
        seen = set()
        for i, line in enumerate(lines[:limit]):
            m = FIELD.match(line)
            if not m:
                continue
            name = m.group(1)
            field_names.setdefault(name, []).append(f"{path.name}:{i + 1}")
            if name not in total or name in seen:
                continue
            seen.add(name)
            total[name] += 1
            if mask[i]:
                in_fence.append(f"{path.name}:{i + 1} ({name})")
            count, value = field_extent(lines, i)
            if count > 1:
                wrapped[name].append((path.name, i + 1, count, value))
                # Orphan signature: a short sentence-final opener followed by
                # a lowercase continuation -- what a first-line-only rewrite
                # leaves behind (spec § 5).
                opener = m.group(2).strip()
                nxt = lines[i + 1].strip()
                if (opener and re.search(r"[.)]$", opener)
                        and len(opener.split()) <= 4
                        and nxt and nxt[0].islower()):
                    orphans.append(f"{path.name}:{i + 1} ({name})")
    return total, wrapped, in_fence, orphans, field_names, fenced_header_block


# -- ANTS-3786: the docs_index scope -----------------------------------------
#
# Everything below simulates src/docsindex.cpp::scanDoc, whose reader differs
# from the spec-corpus survey above in three ways that all change the answer.

MAX_LINE_BYTES = 1024                              # DocsIndex::kMaxLineBytes
STATUS_OLD = re.compile(r"^\*\*Status:\*\*\s*(.+)$")   # scanDoc's old matcher
CLASSES = ("truncated_now_whole", "body_prose_now_empty",
           "unchanged_value", "both_empty", "other")


def considered(lines):
    """The lines scanDoc actually looks at.

    Both skips are `continue`s, so surviving lines close up: an excluded line
    between a field and the prose after it makes that prose adjacent to the
    field, and therefore a continuation of it.
    """
    out, opener = [], None
    for line in lines:
        if len(line.encode("utf-8")) + 1 > MAX_LINE_BYTES:
            continue                               # over-long line (INV-3)
        m = FENCE.match(line)
        if opener is None and m:
            opener = m.group(1)[0]
            continue
        if opener is not None:
            if m and m.group(1)[0] == opener:
                opener = None
            continue                               # fenced block (ANTS-3604)
        out.append(line)
    return out


def status_today(seen):
    """(value, extent) scanDoc returns today: first non-empty-tail Status,
    anywhere in the file, truncated to its own physical line."""
    for i, line in enumerate(seen):
        m = STATUS_OLD.match(line)
        if m:
            return m.group(1).strip(), field_extent(seen, i)[0]
    return "", 0


def status_after(seen):
    """The value headerField returns over the buffered header block."""
    limit = header_block_end(seen)
    for i in range(limit):
        m = FIELD.match(seen[i])
        if m and m.group(1) == "Status":
            return field_extent(seen, i)[1]
    return ""


def survey_docs_index(root):
    """Walk walkDocs's scope: <root>/*.md then <root>/docs/**/*.md."""
    docs = sorted(root.glob("*.md")) + sorted(root.glob("docs/**/*.md"))
    counts = {k: 0 for k in CLASSES}
    wrapped = {k: 0 for k in CLASSES}
    no_h2 = 0
    worst_block, worst_field = (0, 0, ""), (0, "")

    for path in docs:
        lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
        seen = considered(lines)
        limit = header_block_end(lines)
        if limit == len(lines):
            no_h2 += 1
        block_bytes = sum(len(line) + 1 for line in lines[:limit])
        if limit > worst_block[0]:
            worst_block = (limit, block_bytes, str(path.relative_to(root)))

        today, extent = status_today(seen)
        after = status_after(seen)
        if extent > worst_field[0]:
            worst_field = (extent, str(path.relative_to(root)))

        if today == after == "":
            k = "both_empty"
        elif today == after:
            k = "unchanged_value"
        elif today and after.startswith(today):
            k = "truncated_now_whole"
        elif today and not after:
            k = "body_prose_now_empty"
        else:
            k = "other"
        counts[k] += 1
        if extent > 1:
            wrapped[k] += 1

    return len(docs), counts, wrapped, no_h2, worst_block, worst_field


def main_docs_index(argv):
    root = Path(next((a for a in argv[1:] if not a.startswith("--")), "."))
    if not root.is_dir():
        print(f"no such directory: {root}", file=sys.stderr)
        return 0

    total, counts, wrapped, no_h2, block, field = survey_docs_index(root)
    print(f"docs={total}")
    for k in CLASSES:
        print(f"  {k}={counts[k]} (of which wrapped: {wrapped[k]})")
    # `other` is the bucket for a document this change moves in a way the
    # design did not predict. An empty one is what makes the survey evidence
    # rather than a tally; a non-zero one is a design finding.
    print(f"docs_with_no_h2={no_h2}")
    print(f"largest_header_block={block[0]} lines, {block[1]} bytes ({block[2]})")
    print(f"longest_status_extent={field[0]} lines ({field[1]})")
    return 0


def main():
    if "--scope=docs-index" in sys.argv:
        return main_docs_index(sys.argv)

    specs_dir = Path(sys.argv[1] if len(sys.argv) > 1 else "docs/specs")
    if not specs_dir.is_dir():
        print(f"no such directory: {specs_dir}", file=sys.stderr)
        return 0

    total, wrapped, in_fence, orphans, field_names, fenced_hdr = survey(specs_dir)

    for name in ("Status", "Kind"):
        print(f"wrapped {name}: {len(wrapped[name])} of {total[name]}")
    print(f"first field inside a fence: {len(in_fence)}")
    for entry in in_fence:
        print(f"  {entry}")
    print(f"fence opened inside a header block: {len(fenced_hdr)}")
    for entry in fenced_hdr:
        print(f"  {entry}")
    print(f"orphaned-continuation signature: {len(orphans)}")
    for entry in orphans:
        print(f"  {entry}")

    suspect = sorted(n for n in field_names if prose_shaped(n))
    print(f"distinct header-field names: {len(field_names)} "
          f"({len(suspect)} prose-shaped, each needing a human look)")
    for name in suspect:
        print(f"  {name!r} -- {', '.join(field_names[name][:3])}")

    bullet = [w for w in wrapped["Status"] if w[0] == WRAPPED_PROSE_BULLET]
    print(f"prose-bullet continuation ({WRAPPED_PROSE_BULLET}): "
          f"{'present' if bullet else 'ABSENT -- spec § 2.1 evidence moved'}")

    if "--values" in sys.argv:
        print("\n-- joined values (headerField parity: line_count, value) --")
        for name in ("Status", "Kind"):
            for fname, line, count, value in wrapped[name]:
                print(f"  {fname}:{line} ({name}) line_count={count}")
                print(f"    {value}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
