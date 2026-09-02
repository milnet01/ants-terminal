# Feature spec: the closer-length rule reaches the hand-rolled fence loops (ANTS-4820)

## Problem

ANTS-3678 gave `MarkdownScan::fenceMask` CommonMark § 4.5: a closing
fence must be at least as long as its opener. Five files never called
`fenceMask`. Each kept its own open/close loop against
`fenceOpenerChar` and closed on the fence CHARACTER alone —
`feedbackfile.cpp`, `speclog.cpp`, `speclint.cpp`, `docsindex.cpp` and
`fileoutline.cpp`. `fileoutline.cpp`'s comment stated the rule without
its length half, so the code matched its own description and was still
wrong.

A document that QUOTES fence syntax opens a longer fence to do it. On
each of these surfaces its block ended at the first quoted short run,
and the sample text after it — headings included — was read as real.

## Contract

`MarkdownScan::fenceCloses(line, openChar, openRun)` answers whether a
line closes a fence: same character, run at least as long. Every site
that tracks a fence by hand asks it, rather than comparing characters.

The rule lives in one place because it was written wrong in every place
that had its own copy.

## Invariants

- **INV-1 predicate.** `fenceCloses` is false for a shorter run, true
  for an equal or longer one, and false for a different fence character.
- **INV-2 null opener.** A null `openChar` is false, so a caller may ask
  without first testing whether it is inside a fence.
- **INV-3 outline.** `file_outline` in md mode does not report a heading
  written inside a longer fence.
- **INV-4 spec log.** `SpecLog` does not resolve a section heading
  quoted inside a longer fence.
- **INV-5 feedback file.** `FeedbackFile::parse` does not treat a
  boundary heading inside a longer fence as a real boundary.
