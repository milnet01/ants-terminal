# Feature spec: structural bounds for source-scrape tests (ANTS-3681)

## Problem

Source-scrape tests bounded their search region with a byte count —
`src.substr(pos, 4800)`. A literal drifts out of the window whenever an
unrelated neighbour grows, and the test then fails against code that is
entirely correct. That trains the reader to widen the number and move
on, which two call sites record having done.

Worse than the false red is the quiet one. A window is trimmed at the
END, so a row asserting an ABSENCE passes once its target falls out of
range — it stopped looking. Measured when this landed: every converted
window was reading a fraction of its target, `cmdSpecLint` 17% and
`recordDispatch` 20%, and one already-widened window was shorter than
the body it claimed to cover.

## Contract

Two helpers, so no test needs a byte count.

`slurpFunctionBody(src, signatureAnchor)` — already present — returns a
brace-matched function body.

`regionBetween(text, startAnchor, endAnchor)` returns from `startAnchor`
up to but excluding the next `endAnchor` after it, for a block that is
not a function: a registration entry, a schema block, one arm of a
dispatch chain.

Either helper returns the empty string on failure. It must NOT return a
tail running to end-of-file: a region that silently became the whole
file makes an absence assertion pass for the wrong reason, which is the
defect being removed rather than a new one.

## Invariants

- **INV-1 bounded.** `regionBetween` returns from the start anchor up to
  the next end anchor, excluding it.
- **INV-2 nearest end.** The end anchor is the first one AFTER the start
  anchor, so a block is not extended by a later occurrence.
- **INV-3 start absent.** A start anchor that does not occur returns "".
- **INV-4 end absent.** An end anchor that does not occur after the start
  returns "" — never the tail. This is the invariant the helper exists
  for; a tail would restore the failure it replaces.
- **INV-5 end anchor may repeat the start.** Bounding a repeated block by
  its own opening token works, since the search begins past the start
  anchor.
