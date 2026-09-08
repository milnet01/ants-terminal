# Feature: a wide character contributes two cell widths to a background run

## Problem

`TerminalWidget::paintEvent` coalesces adjacent same-colour cell
backgrounds into one `fillRect` (ANTS-1180). It walks every column,
computes `cellDrawWidth` — double for a cell whose `isWideChar` is set —
and either extends the active run by that width or flushes and starts a
new one.

A double-width character occupies two cells. The lead cell carries
`isWideChar = true`; the continuation cell carries `isWideCont = true`
and `isWideChar = false`. Both reach the background-run code, because
the `continue` that skips continuation cells sits below it, where the
glyph drawing starts.

So the lead adds two cell widths and the continuation adds one more:
three cell widths of fill for a two-column character. The error is per
wide character and accumulates — a run containing N of them is N
columns too wide, and the fill spills past the text it belongs to. A
selection over CJK or emoji is where this is visible.

## Contract

A background run MUST cover exactly the columns its cells occupy. A
wide character's continuation cell adds nothing to a run its lead cell
is already part of.

The continuation cell may still OPEN a run of its own, in the branch
that flushes and starts a new one. That happens when its background
colour differs from the lead's — a selection boundary falling between
the two halves of one character — and painting the right half there is
the intended result, not a bug to suppress.

Placing the guard on the extend branch alone is therefore deliberate,
and is what this feature requires.

## Invariants

**INV-1 — the extend branch does not add width for a continuation
cell.** Source-grep `TerminalWidget::paintEvent`: the branch that
extends an active background run tests `isWideCont` before adding.

**INV-2 — the guard is on the extend branch only.** The branch that
starts a new run must not be gated on `isWideCont`, or a split
selection stops painting its right half.

**INV-3 — the lead cell still contributes double.** The
`isWideChar ? m_cellWidth * 2 : m_cellWidth` computation is intact; the
fix is about the continuation cell, not about narrowing wide glyphs.

## Scope

### In scope
- Source-grep over `src/terminalwidget.cpp`.

### Out of scope
- A pixel test. `paintEvent` needs a real paint device and a font with
  true double-width metrics on the host; that belongs to the E2E
  harness, which can `grab-image`.
- The underline width computation in the same function, which reads
  `isWideChar` the same way. It is per-run and not accumulated, so it
  does not have this defect.
- Whether a selection should be able to split a wide character at all.
  Unchanged here.

## Regression history

- **ANTS-1180:** introduced background-run coalescing. The
  continuation cell's contribution was not considered.
- **ANTS-4456 (cold sweep 2026-08-18, verified 2026-09-08):** reported
  as "wide char adds three cell widths to a background run (CJK/emoji
  selection highlights one column too far)". Verified against source —
  and the overshoot accumulates per wide character rather than being a
  single column. Fixed. Locked by this spec.
