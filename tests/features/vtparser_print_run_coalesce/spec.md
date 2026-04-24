# Feature: VT-parser Print-run coalescing preserves grid state

## Contract

Since 0.7.17 the VT parser's SIMD fast path emits printable-ASCII runs
as a single `VtAction` of type `Print` carrying a pointer+length into the
caller's feed buffer (`printRun` + `printRunLen`), instead of one Print
action per byte. `TerminalGrid::processAction` dispatches that form to a
new fast path `handleAsciiPrintRun(data, len)` that batches per-row
writes.

The contract is **grid-state equivalence**: for any input byte sequence
`S`, running `VtParser::feed(S, |S|)` through `TerminalGrid` (which
triggers the SIMD + batched-write path whenever `S` contains safe-ASCII
runs) produces the exact same post-feed state as feeding `S` one byte at
a time (which bypasses the SIMD path entirely and exercises
`handlePrint` for every byte).

"Exact same state" means:

- `rows()` and `cols()` are equal.
- `cursorRow()` and `cursorCol()` are equal.
- For every `(row, col)` in `[0..rows) × [0..cols)`, `cellAt(row, col)`
  agrees on `codepoint`, `isWideChar`, `isWideCont`, and the foreground/
  background/attribute bits of `attrs`.
- The delayed-wrap flag (`m_wrapNext`) must match too. We observe this
  indirectly by feeding one extra probe byte and checking that the
  resulting cursor position is identical on both feeds.

## Rationale

The SIMD fast path already bypasses the full VT state machine for runs
of printable ASCII. Before this feature, it emitted one `Print` action
per byte — preserving action-stream shape (validated by
`vtparser_simd_scan`) but paying a full `VtAction` construction +
`std::function` dispatch cost per byte. For a 10 KB printable-ASCII
PTY read, that's ~10 000 heap-touching VtAction zero-inits plus 10 000
indirect calls.

Coalescing collapses that to one action + one dispatch. The batched
`handleAsciiPrintRun` further skips per-byte `wcwidth()`, the combining-
character check, the wide-char branch, and the `std::clamp` inside
`cell()`. It splits the run into per-row spans so `markScreenDirty` and
`combining.erase` fire once per row instead of once per byte.

This optimization is only valid because:

1. `scanSafeAsciiRun` guarantees every byte in the run is in
   `[0x20..0x7E]`, so `wcwidth` would return 1, and the byte cannot be
   combining, cannot be wide, cannot be a UTF-8 leader.
2. The parser only enters the SIMD scan in Ground state with no
   pending UTF-8 continuation — the one scenario where the per-byte
   state machine would have emitted a Print with that literal byte as
   codepoint.

Any violation of (1) or (2) would silently corrupt grid state without
the existing `vtparser_simd_scan` test catching it, because that test
compares action streams after canonicalization — it can't observe a
divergence *inside* `handleAsciiPrintRun`. This feature test closes
that gap.

## Invariants

**INV-1 — 4 KiB ASCII run matches byte-by-byte feed.**
Feed a 4 KiB printable-ASCII buffer into one grid via `feed(buf, |buf|)`
and into a second grid via `|buf|` one-byte feeds. Post-feed grid state
is identical (cursor, all cells, wrapNext probe).

**INV-2 — Run containing a CSI sequence at every lane boundary.**
Plant `\x1b[31m` at offset `k` for `k ∈ {0, 1, …, 31}` inside a
printable-ASCII buffer and verify equivalence. Exercises the transition
between "coalesced run" and "scalar state machine".

**INV-3 — Wrap at the right edge is byte-identical.**
Feed enough safe-ASCII to wrap the cursor. Both feed strategies must
leave the cursor on the same row/col, with `m_wrapNext` in the same
state (observed by feeding one more probe byte and comparing final
cursor).

**INV-4 — Cursor starting mid-row.**
Seed the grid with `\x1b[5;37H` (cursor to row 5 col 37) before feeding
the safe-ASCII run. Verifies the per-row span accounting (`available =
cols - startCol`) matches the scalar `handlePrint` cursor advance.

**INV-5 — Mixed UTF-8, run, control chars, run.**
Feed `"ASCII " + "好" + "more" + "\n" + "tail"`. The UTF-8 multibyte
path goes through the scalar parser; the safe-ASCII runs around it
go through the SIMD+batched path. Final state is identical to the
byte-by-byte feed.

**INV-6 — Empty input is a no-op.**
`feed("", 0)` produces no state change. Degenerate but catches the
`len == 0` guard inside `handleAsciiPrintRun`.

**INV-7 — Combining characters on previously-written cells are cleared.**
If cell `(r, c)` has combining characters attached, writing a new base
char via the coalesced path must clear them — matching the
`combining.erase(cursorCol)` call in `handlePrint`. The test seeds a
combining mark, then overwrites with an ASCII run.

## Scope

### In scope
- Byte-identical grid state after any safe-ASCII input.
- Correct wrap behavior across the right-edge boundary (delayed-wrap
  semantics identical to the scalar path).
- Correct behavior when a run straddles two or more rows.

### Out of scope
- Throughput numbers. Bench harness `tests/perf/bench_vt_throughput.cpp`
  measures those; correctness is locked here.
- Alt-screen, scroll regions, DECSET modes. The coalesced path only
  touches the main screen's write semantics. Existing
  `handlePrint` tests cover the interaction with those modes.
- Architectures without SSE2/NEON. The scalar fallback inside
  `scanSafeAsciiRun` still returns the same run length, so the
  coalesced path still fires.

## Regression history

- **0.7.17:** Coalescing introduced. This test locks the grid-state
  contract so any future attempt to further optimize
  `handleAsciiPrintRun` (e.g. SIMD cell writes, `memcpy` into the
  codepoint field) has a failing reference if it drifts from
  `handlePrint`'s semantics.
