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

Usage:  python3 tools/spec-header-survey.py [specs_dir]
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


def main():
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
