# Feature: `QDataStream::status()` checks inside cell-decoding loops

## Contract

Inside `SessionManager::restore`, the cell-decoding helper
(`readCell`) MUST return false on stream-status failure, and every
caller MUST short-circuit the surrounding loop on that false. The
combining-character helper MUST check status after each codepoint
read.

## Rationale

Pre-fix, `readCell` was a void lambda:

```cpp
auto readCell = [&in](Cell &c) {
    QRgb fg, bg;
    uint8_t flags;
    in >> c.codepoint >> fg >> bg >> flags;
    c.attrs.fg = QColor::fromRgba(fg);     // populated even if read failed
    ...
};
```

A truncated stream would set `in.status()` to `ReadPastEnd`, but the
default-constructed `QRgb` and `uint8_t` values still flowed through
`QColor::fromRgba(fg)` etc. into the cell — silently writing
uninitialized colors and flags into the grid. Worse, the surrounding
loop didn't check the per-iteration status, so the grid kept
accepting cells from a stream that was already done.

The fix is to make `readCell` return bool and short-circuit at every
caller. The combining-character inner-codepoint loop gets the same
treatment.

## Invariants

**INV-1 — `readCell` returns bool.** Source-grep against
`src/sessionmanager.cpp`: the lambda definition for `readCell` MUST
declare a return type of `-> bool` and contain at least one
`return false` (status check) and a final `return true`.

**INV-2 — Every `readCell` call site checks the result.**
Source-grep: every `readCell(...)` invocation in `restore` MUST be
guarded by `if (!readCell(...))` returning false from the surrounding
function. Bare `readCell(x);` statements (return-value discarded) are
not allowed — they're the pre-fix shape that led to silent corruption.

**INV-3 — Combining-codepoint inner loop checks status.** Source-grep:
inside `readCombining`, the inner `for (int k = 0; k < cpCount; ++k)`
loop MUST contain an `in.status() != QDataStream::Ok` check after the
codepoint read. Without it, a stream truncated mid-codepoint pushes
default-constructed `0` codepoints into the combining map.

## Scope

### In scope
- `readCell` lambda return type + status check.
- All four `readCell` call sites (scrollback cells, screen cells in
  range, screen cells skipped on width shrink, screen cells skipped
  on height shrink).
- Inner codepoint read in `readCombining`.

### Out of scope
- Per-field status checks inside `readCell`. The aggregate
  post-`flags` check catches all four-field truncation cases; finer
  granularity adds noise without eliminating a class of bug.
- The outer `cellCount` / `wrapped` / `combCount` reads. Those
  already had status checks and continue to.

## Regression history

- **V1 – V3:** `readCell` was void; truncated streams silently
  populated cells with uninitialized fg/bg/flags. The grid would
  render those cells as if they were intentional state.
- **0.7.30 (this fix):** `readCell` returns bool, every caller
  short-circuits, `readCombining` inner loop checks status per
  codepoint.
