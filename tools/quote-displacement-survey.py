#!/usr/bin/env python3
"""Measure how far a review lane's quotation sits from the line it cites.

ANTS-5732 sets quotation_check's default `tolerance` from this survey
(docs/specs/ANTS-5502-quotation-check.md § 2.3). Each lane finding of the form
[Qn] SEVERITY `path:line` — `quotation` is matched against single lines of the
file at BASE, the commit the review read. The figure is the distance from the
cited line to the nearest line holding the quotation.

Single lines only, with no normalisation, so a quotation that wraps or carries
markup is counted as not found rather than guessed at.

Usage:
    tools/quote-displacement-survey.py BASE [GLOB]

GLOB defaults to the lane reports of every review under .indie-review/.
Run from the project root.
"""
import collections
import glob
import re
import subprocess
import sys

FINDING = re.compile(
    r"\[Q\d\] (?:CRITICAL|HIGH|MEDIUM|LOW) `([^`:]+):(\d+)[^`]*` — `([^`]{12,})`")


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    base = sys.argv[1]
    pattern = sys.argv[2] if len(sys.argv) > 2 else ".indie-review/*/*/lanes/*.result"
    files = {}

    def show(path):
        if path not in files:
            r = subprocess.run(["git", "show", f"{base}:{path}"], capture_output=True)
            files[path] = r.stdout.decode("utf-8", "replace") if r.returncode == 0 else None
        return files[path]

    distances, not_found, no_file = [], 0, 0
    for report in glob.glob(pattern):
        with open(report, encoding="utf-8") as f:
            text = f.read()
        for m in FINDING.finditer(text):
            path, cited = m.group(1), int(m.group(2))
            quote = m.group(3).split("…")[0].split("...")[0].strip()
            if len(quote) < 12:
                continue
            source = show(path)
            if source is None:
                no_file += 1
                continue
            hits = [i + 1 for i, line in enumerate(source.split("\n")) if quote in line]
            if not hits:
                not_found += 1
                continue
            distances.append(min(abs(h - cited) for h in hits))

    n = len(distances)
    print(f"found {n}, not found {not_found}, no file {no_file}")
    if not n:
        return
    for t in (0, 1, 2, 3, 5, 10, 20, 50):
        print(f"within {t}: {sum(d <= t for d in distances) / n:.1%}")
    print("largest:", sorted(distances)[-5:])


if __name__ == "__main__":
    main()
