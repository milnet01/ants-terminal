# Feature: roadmap_log writers emit no trailing whitespace (ANTS-3417)

`roadmap_log` op:append / op:flip wrote continuation lines that could end
in trailing whitespace, so the very next `git commit` was rejected by the
ubiquitous `trim trailing whitespace` pre-commit hook — forcing a
re-stage + re-commit after every roadmap write.

Root cause: `formatRoadmapBullet`'s body loop emitted `"  " + ln + "\n"`
for **every** line, so a blank body line rendered as `"  \n"` (two
trailing spaces). `appendBodyNote` collapsed empty note lines but left
space-only / trailing-whitespace lines dangling. Both now route every
emitted continuation line through `rcRightStrip` (trailing space/tab
removal); a whitespace-only line collapses to an empty line.

## Invariants

- **INV-1** — append with a body containing a blank line writes no line
  ending in a space/tab; the blank line is a truly empty line (not the
  `"  "` hang indent).
- **INV-2** — an appended note (op:annotate / op:flip-with-note) containing
  a blank line writes no line ending in a space/tab (the shared
  `appendBodyNote` path).

## Pre-fix check

Against pre-fix code INV-1 FAILS (the blank body line renders as `"  "`,
a trailing-whitespace line). Verified before the fix.

Label: `features`.
