# Feature: the sticky command header reads a bounded number of lines

## Problem

`TerminalWidget::paintEvent` rebuilds the pinned OSC 133 command-header text
on every paint by joining `lineText()` for every global line from a prompt
region's A marker to its B marker, and truncates to `maxChars` only *after*
the whole span has been read. `B` lands wherever the cursor happened to be
when the region closed, and the forgery check that would otherwise cap it is
off unless `ANTS_OSC133_KEY` is set — so any program that prints enough
output stretches the span across the whole scrollback. The cost is every
cell of every line in the span, read on the GUI thread, once per paint.
Filed as ANTS-5027.

`src/stickycommandtext.h`'s `stickyCommandText` is the pure function that
does the join-then-truncate. As authored (current tree) it has no bound: the
loop over `[startLine, endLine]` runs to completion regardless of
`maxChars`, calling `lineAt` once per line every time. The planned fix stops
the loop once the joined text is already past `maxChars` — a caching layer
and a B-forgery bound are separate, not covered here.

## Why behavioural

`stickyCommandText` is pure and takes `lineAt` as an injected callback, so
the read count the defect is about is directly observable: hand it a
counting `lineAt` and read the count back. No source scrape is needed or
used — every invariant below drives the real function through its real
signature.

## Invariants

- **ANTS-5027-INV-1 — the read is bounded by `maxChars`, not by the span
  length.** For a span of 100000 lines, each non-empty, and `maxChars = 80`,
  `lineAt` is called at most `maxChars + 2` times. For a span of 100000
  *blank* lines and the same `maxChars`, `lineAt` is also called at most
  `maxChars + 2` times, and the returned text is empty. *Test:*
  `test_sticky_header_bound.cpp` (paired test file).
- **ANTS-5027-INV-2 — output is unchanged for text that already fits.** The
  four lines `"git"`, `"commit"`, `"-m"`, `"msg"` joined by one space and
  trimmed is `"git commit -m msg"` (17 characters). With `maxChars = 80` the
  result is that string, untruncated. With `maxChars = 10` the result is the
  first 9 characters of that string followed by U+2026 (`…`), matching the
  current `left(maxChars - 1) + ellipsis` truncation rule exactly — the
  bound changes how many lines are *read*, never what a fitting or
  overflowing string *renders as*. *Test:* `test_sticky_header_bound.cpp`
  (paired test file).
- **ANTS-5027-INV-3 — trailing blank lines do not change the result, and
  reading them is still bounded.** `"ls"`, `"-la"`, then 5000 blank lines,
  `maxChars = 80`, produces `"ls -la"`, and `lineAt` is called at most
  `maxChars + 2` times. *Test:* `test_sticky_header_bound.cpp` (paired test
  file).
- **ANTS-5027-INV-4 — each line is trimmed before joining, and lines join
  with exactly one space.** `"  a  "`, `"  b  "` (leading and trailing
  spaces on each) produces `"a b"`. *Test:* `test_sticky_header_bound.cpp`
  (paired test file).
- **ANTS-5027-INV-5 — a non-positive `maxChars` never truncates.**
  `paintEvent` can pass a non-positive width on a tiny window. This is
  current, pre-fix behaviour and the bound must not change it: for a short
  span (`"hello"`, `"world"`) with `maxChars = 0`, the result is the full
  joined string `"hello world"`, untruncated; the same holds for a negative
  `maxChars` (`-5`). *Test:* `test_sticky_header_bound.cpp` (paired test
  file).

## Scope

In scope: `stickyCommandText`'s own contract — how many times it calls
`lineAt` for a given span and `maxChars`, and what string it returns.

Out of scope:
- `TerminalWidget::paintEvent`'s call site, the per-region cache the
  ANTS-5027 roadmap entry also asks for, and the B-forgery bound — none of
  those are reachable from this header-only, `QString`-only function, and
  none is asserted here.
- The `ANTS_OSC133_KEY` forgery check and how a prompt region's A/B markers
  are set — upstream of this function; it only ever sees the two line
  numbers it is handed.
- Any locale- or font-dependent rendering of the returned string — this
  function returns a `QString`, painting it is `paintEvent`'s job.

## Test

`test_sticky_header_bound.cpp`, suite `StickyHeaderBound`, in the
`test_core` bundle (`Qt6::Core` only — `stickycommandtext.h` includes only
`<QChar>`/`<QString>`/`<functional>`, and `src` is already a public include
directory on `ants_core_lib`, which `test_core` links). Each `TEST()` builds
a `QStringList` of lines and a `lineAt` lambda that increments a call
counter and reads from that list, calls `stickyCommandText`, and checks the
call count and/or the returned string via the shared `expect()` helper
(`tests/_support/expect.h`), each labelled with its `ANTS-5027-INV-N`. On
the current, unbounded tree, INV-1 and INV-3 are expected to fail: the
5-digit spans they construct make the current call count (one per line, no
early stop) far exceed `maxChars + 2`, which is exactly the defect this test
locks against. INV-2, INV-4 and INV-5 assert the current output shape and
are expected to pass on both the current tree and the bounded fix, proving
the fix does not change output for spans that already fit.

## Regression history

- **ANTS-5027 (2026-09-11, open at authoring time):** `stickyCommandText`'s
  loop has no bound; this test is written to fail against that on INV-1 and
  INV-3, and to keep passing on INV-2/4/5 once the fix lands, since the
  fix's own stated scope is "changes only how many lines are read".
