#!/usr/bin/env python3
"""ANTS-3833 — brace-aware anonymous-namespace scanner for a C++ source.

Reports every anonymous namespace's extent, and (with --seams) whether a set
of proposed translation-unit boundaries falls in open code.

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

Exit status: 0 if every seam is in open code (or no seams given), 1 otherwise.
"""

from __future__ import annotations

import argparse
import re
import sys

_IDENT = re.compile(r"[A-Za-z_][A-Za-z0-9_]*")


def anonymous_namespaces(src: str) -> list[tuple[int, int]]:
    """Return [(open_line, close_line)] for each anonymous namespace, 1-based."""
    found: list[tuple[int, int]] = []
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
            k = j
            while k < n and src[k] in " \t":
                k += 1
            # A name after `namespace` makes it named; otherwise anonymous.
            pending = ("named" if _IDENT.match(src, k) else "anon", line)
            i = j
            continue

        if c == "{":
            stack.append(pending if pending else ("brace", line))
            pending = None
            i += 1
            continue

        if c == "}":
            if stack:
                kind, open_line = stack.pop()
                if kind == "anon":
                    found.append((open_line, line))
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
    args = ap.parse_args()

    with open(args.path, encoding="utf-8", errors="replace") as fh:
        src = fh.read()

    blocks = anonymous_namespaces(src)
    print(f"{args.path}: {len(blocks)} anonymous namespaces")
    for a, b in blocks:
        print(f"  {a:6d} .. {b:6d}   ({b - a + 1} lines)")

    if not args.seams:
        return 0

    seams = [int(s) for s in args.seams.split(",") if s.strip()]
    print("\nseam containment:")
    bad = 0
    for s in seams:
        inside = [(a, b) for a, b in blocks if a < s <= b]
        if inside:
            bad += 1
            print(f"  {s:6d}  INSIDE {inside}")
        else:
            nearest = max((b for _, b in blocks if b < s), default=None)
            gap = f", clears the previous block by {s - nearest} lines" if nearest else ""
            print(f"  {s:6d}  open code{gap}")
    print(f"\nseams inside an anonymous namespace: {bad} of {len(seams)}")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
