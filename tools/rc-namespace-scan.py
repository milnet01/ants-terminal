#!/usr/bin/env python3
"""ANTS-3833 — brace-aware namespace scanner for a C++ source.

Reports every namespace block's extent, and (with --seams) whether a set of
proposed translation-unit boundaries falls in open code.

WHICH NAMESPACES COUNT IS A FLAG, AND IT HAS TO BE. This scanner was written
before ANTS-3833's commit 1, when the hazard was the file's 24 ANONYMOUS
namespaces. That commit renamed all 24 to `namespace rcdetail`, so a scan for
anonymous blocks now finds none and every seam is reported as open code —
green, and blind to the thing it was shipped to see. The hazard never changed:
a seam inside a `rcdetail { ... }` block splits that block across two TUs and
the tree does not compile. Pass `--ns` to name the namespaces that matter;
`anon` is the special token for the unnamed ones.

WHY THIS IS A TOOL AND NOT A GREP. The obvious check —
`grep -n '^namespace {' file.cpp` paired with `^}  // namespace` — cannot
work, and its failure is silent rather than loud:

  * it counts openings it can see and closings that happen to be commented,
    and cannot pair them;
  * it cannot see an indented or nested opening at all;
  * the closing comment can be WRONG. src/remotecontrol.cpp:16959 is
    annotated "anonymous from line 1320"; that block actually opens at
    16875 (ANTS-3840).

Measured 2026-08-06: two of three independent reviewers, reasoning from that
grep and that comment, concluded seven of ten proposed seams sat inside an
anonymous namespace spanning 1320-16959 — which would have made the eleven-way
split uncompilable. This scanner says zero of ten. Re-deriving the check by
hand is how that mistake gets made again, which is why ANTS-3833 § 2.2 makes
running THIS the precondition on the cut.

Tracks nesting depth while skipping string literals, character literals, line
comments and block comments. Deliberately conservative in the same way
tests/_support/srcgrep.h is: no raw-string literals, no trigraphs, no
preprocessor games. Erring toward over-reporting a namespace is the safe
direction — it can only make a seam look unsafe when it is fine, never the
reverse.

Usage:
    tools/rc-namespace-scan.py src/remotecontrol.cpp
    tools/rc-namespace-scan.py src/remotecontrol.cpp --seams 2262,3666,6556
    tools/rc-namespace-scan.py src/remotecontrol.cpp --ns anon,rcdetail --seams ...

Exit status: 0 if every seam is in open code (or no seams given), 1 otherwise.
"""

from __future__ import annotations

import argparse
import re
import sys

_QUALIFIED = re.compile(r"[A-Za-z_][A-Za-z0-9_]*(?:\s*::\s*[A-Za-z_][A-Za-z0-9_]*)*")


def _skip_trivia(src: str, i: int) -> int:
    """Index of the next significant character at or after `i`."""
    n = len(src)
    while i < n:
        if src[i] in " \t\r\n":
            i += 1
        elif src.startswith("//", i):
            while i < n and src[i] != "\n":
                i += 1
        elif src.startswith("/*", i):
            i += 2
            while i + 1 < n and not (src[i] == "*" and src[i + 1] == "/"):
                i += 1
            i += 2
        else:
            break
    return i


def namespace_blocks(src: str) -> list[tuple[int, int, str]]:
    """Return [(open_line, close_line, name)] per namespace block, 1-based.

    `name` is the declared identifier, or "anon" for an unnamed namespace.
    """
    found: list[tuple[int, int, str]] = []
    stack: list[tuple[str, int]] = []
    pending: tuple[str, int] | None = None
    line = 1
    i, n = 0, len(src)

    while i < n:
        c = src[i]

        if c == "\n":
            line += 1
            i += 1
            continue

        if c == '"':
            i += 1
            while i < n and src[i] != '"':
                if src[i] == "\\":
                    i += 1
                if i < n and src[i] == "\n":
                    line += 1
                i += 1
            i += 1
            continue

        if c == "'":
            i += 1
            while i < n and src[i] != "'":
                if src[i] == "\\":
                    i += 1
                i += 1
            i += 1
            continue

        if c == "/" and i + 1 < n and src[i + 1] == "/":
            while i < n and src[i] != "\n":
                i += 1
            continue

        if c == "/" and i + 1 < n and src[i + 1] == "*":
            i += 2
            while i + 1 < n and not (src[i] == "*" and src[i + 1] == "/"):
                if src[i] == "\n":
                    line += 1
                i += 1
            i += 2
            continue

        if src.startswith("namespace", i) and (
            i == 0 or not (src[i - 1].isalnum() or src[i - 1] == "_")
        ):
            j = i + len("namespace")
            k = _skip_trivia(src, j)
            # A name after `namespace` makes it named; otherwise anonymous.
            m = _QUALIFIED.match(src, k)
            name = m.group(0) if m else "anon"
            k = _skip_trivia(src, m.end() if m else k)
            # Only a `{` here opens a block. `using namespace rcdetail;` and
            # `namespace x = y::z;` reach this point too, and leaving `pending`
            # set for them would tag the NEXT unrelated brace — a function body
            # — as a namespace. Commit 2 puts a `using namespace rcdetail;` in
            # ten of the eleven TU preambles, so this is the common case.
            pending = (name, line) if k < n and src[k] == "{" else None
            i = j
            continue

        if c == "{":
            stack.append(pending if pending else ("", line))
            pending = None
            i += 1
            continue

        if c == "}":
            if stack:
                name, open_line = stack.pop()
                if name:
                    found.append((open_line, line, name))
            i += 1
            continue

        i += 1

    found.sort()
    return found


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("path")
    ap.add_argument(
        "--seams",
        help="comma-separated 1-based line numbers to check for open-code containment",
    )
    ap.add_argument(
        "--ns",
        default="anon",
        help="comma-separated namespace names a seam must not fall inside; "
        "'anon' means the unnamed ones (default: anon)",
    )
    args = ap.parse_args()

    with open(args.path, encoding="utf-8", errors="replace") as fh:
        src = fh.read()

    wanted = [s.strip() for s in args.ns.split(",") if s.strip()]
    blocks = [b for b in namespace_blocks(src) if b[2] in wanted]
    label = " / ".join(wanted)
    print(f"{args.path}: {len(blocks)} {label} namespaces")
    for a, b, name in blocks:
        print(f"  {a:6d} .. {b:6d}   ({b - a + 1} lines)  {name}")

    if not args.seams:
        return 0

    seams = [int(s) for s in args.seams.split(",") if s.strip()]
    print("\nseam containment:")
    bad = 0
    for s in seams:
        inside = [(a, b, nm) for a, b, nm in blocks if a < s <= b]
        if inside:
            bad += 1
            print(f"  {s:6d}  INSIDE {inside}")
        else:
            nearest = max((b for _, b, _ in blocks if b < s), default=None)
            gap = f", clears the previous block by {s - nearest} lines" if nearest else ""
            print(f"  {s:6d}  open code{gap}")
    print(f"\nseams inside a {label} namespace: {bad} of {len(seams)}")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
