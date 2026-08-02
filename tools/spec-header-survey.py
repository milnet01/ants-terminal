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


def field_extent(lines, i):
    """Continuation-line count for the field starting at `lines[i]`."""
    n, j = 0, i + 1
    while j < len(lines) and not TERMINATOR.match(lines[j]):
        n, j = n + 1, j + 1
    return n


def survey(specs_dir):
    total = {"Status": 0, "Kind": 0}
    wrapped = {"Status": [], "Kind": []}
    in_fence = []

    for path in sorted(specs_dir.glob("*.md")):
        lines = path.read_text(encoding="utf-8").split("\n")
        mask = fence_mask(lines)
        seen = set()
        for i, line in enumerate(lines):
            m = FIELD.match(line)
            if not m:
                continue
            name = m.group(1)
            if name not in total or name in seen:
                continue
            seen.add(name)
            total[name] += 1
            if mask[i]:
                in_fence.append(f"{path.name}:{i + 1} ({name})")
            cont = field_extent(lines, i)
            if cont:
                wrapped[name].append((path.name, i + 1, cont))
    return total, wrapped, in_fence


def main():
    specs_dir = Path(sys.argv[1] if len(sys.argv) > 1 else "docs/specs")
    if not specs_dir.is_dir():
        print(f"no such directory: {specs_dir}", file=sys.stderr)
        return 0

    total, wrapped, in_fence = survey(specs_dir)

    for name in ("Status", "Kind"):
        print(f"wrapped {name}: {len(wrapped[name])} of {total[name]}")
    print(f"first field inside a fence: {len(in_fence)}")
    for entry in in_fence:
        print(f"  {entry}")

    bullet = [w for w in wrapped["Status"] if w[0] == WRAPPED_PROSE_BULLET]
    print(f"prose-bullet continuation ({WRAPPED_PROSE_BULLET}): "
          f"{'present' if bullet else 'ABSENT -- spec § 2.1 evidence moved'}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
