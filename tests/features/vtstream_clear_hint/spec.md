# Feature: a screen clear clears the selection however the bytes arrive

## Invariants

**INV-1 — a clear split across two reads still sets the hint.** A child that
writes `ESC [ 2`, pauses, then writes `J` produces a `VtBatch` whose
`clearSelectionHint` is true.

**INV-2 — output with no clear sets no hint.** A child that writes plain text
produces no batch with `clearSelectionHint` set.

## Rationale

`VtStream::onPtyData` looked for `ESC[2J`, `ESC[3J` and form feed in each raw
read. A sequence split across two reads matched in neither, so the selection
survived a screen clear. The hint is now set from the parser's actions: a
`CsiDispatch` with final `J`, no intermediate and parameter 2 or 3, or an
`Execute` of form feed. The parser keeps its state across reads, so a split
sequence is seen whole.

## Test surface

`test_vtstream_clear_hint.cpp` starts a `VtStream` on a script shell in the
test thread, acknowledges each batch, and records `clearSelectionHint`.
Behavioural: the split depends on real reads from a real child.

## Regression history

- **ANTS-5075:** the selection-clear hint missed a clear sequence split across
  two PTY reads. Locked by this spec.
