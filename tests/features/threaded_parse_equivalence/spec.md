# Threaded parse equivalence

**Status:** locked in 0.7.0.

## Invariant

For any byte sequence fed to `VtParser`, the sequence of `VtAction`s
emitted — concatenated in order — must be identical whether the parser
ran on the GUI thread (legacy path) or on the VtStream worker thread
(0.7.0 path).

In other words: moving parse work off-thread is allowed to change
*when* actions arrive, but must never change *what* actions arrive or
*in what order*.

This test pins the contract at the `VtParser` level. It does not spin up
an actual QThread — the equivalence is proven at the algorithmic layer
(one parser fed byte-by-byte; a second parser fed the same bytes in
worker-batch-sized chunks; emitted actions compared token-by-token).
Running the actual thread transport is covered separately by the Qt
queued-connection machinery, which is provider-tested — this spec pins
the application-level invariant those mechanics must preserve.

### Worker batches

The worker (`VtStream`) keeps the parser's coalescing: a printable-ASCII
run travels in its `VtBatch` as one Print action whose `printRun` points
into the batch's own `rawBytes`. `VtBatchBuilder` records the run as an
offset while the read buffer is alive and resolves the pointer in
`takeInto`, once the bytes sit in the heap batch that ships, so the
pointer survives the queued hop to the GUI thread. The GUI applies the
run with one `TerminalGrid::processAction` call (`handleAsciiPrintRun`).

So the contract is pinned twice more, through `VtBatchBuilder` itself.
Each read is a fresh buffer freed right after it is fed, and the batches
are moved before they are read, as in `VtStream`:

- **Action stream.** The batches' actions, concatenated, with every run
  expanded to one Print per byte, equal the byte-by-byte parse (the
  comparison below).
- **Terminal state.** Applying the batches to a `TerminalGrid` leaves it
  identical to a grid fed the direct parse with every run split into
  scalar Prints (`handlePrint`, the pre-run threaded path): every screen
  and scrollback cell (codepoint, all attributes, wide lead and
  continuation flags), combining marks, soft-wrap flags and the cursor.
  The grid is small (6×17) so runs wrap and scroll.
- **Coalescing kept.** 50 000 bytes of printable ASCII, read 16 KB at a
  time, ship as at most 64 actions, and each run's bytes lie inside its
  batch's `rawBytes`.

## Corpus

Fixture inputs must cover, at minimum, one of each:

1. Plain ASCII, no escape sequences.
2. Mixed ANSI: SGR, cursor-position, scroll region, tabs, wraps.
3. DCS payload (Sixel).
4. APC payload (Kitty graphics).
5. OSC long-string (OSC 52 clipboard set with a non-trivial base64 blob).
6. UTF-8 multi-byte across a buffer boundary. The boundary must fall
   inside a codepoint's continuation bytes to exercise the UTF-8
   decoder's resumption state.
7. Synchronous-output DEC mode 2026 begin/end pair.
8. OSC 133 A/B/C/D shell-integration markers.
9. CSI with many (≥32) numeric parameters.
10. Input split into 1-byte chunks.
11. Input split into arbitrary chunk sizes (a pseudo-random pattern
    derived from a fixed seed for reproducibility).
12. Print runs at grid edges: a run wrapping past the right column, a run
    over a wide pair, a combining mark after a run, runs after charset
    designations (`ESC ( 0`, `ESC ( B`) and SO/SI, in insert mode, and
    around SGR and an OSC 8 link.

Worker batches are cut three ways: 16 KB reads flushed at the worker's
thresholds (4096 actions or 16 KB), 7-byte reads flushed after each read,
and pseudo-random reads (seed 42) flushed after each read.

## Pass criteria

For every fixture:

- The number of emitted `VtAction`s is identical between the reference
  single-shot parse and every chunked parse.
- For each index `i`, `actions[i]` comparison covers `type`, `codepoint`,
  `controlChar`, `finalChar`, `params`, `colonSep`, `intermediate`, and
  `payload`. All must match byte-for-byte.

- For every batch shape, the expanded batch stream passes the same
  comparison against the byte-by-byte parse, and the two grids match.

On any mismatch the test prints the fixture name, the diverging action
index, and a diagnostic dump of both action tokens so the failure is
self-diagnosing.
