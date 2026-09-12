# Claude transcript: incremental parse from a byte cursor

ANTS-5050. Both task trackers re-walked the transcript's 16 MiB tail and
JSON-parsed every line on every debounced append, on the GUI thread. This
makes the cost proportional to the bytes appended instead.

## Why

Measured 2026-09-12 with `tests/perf/bench_transcript_walk`, which links the
real trackers. One walk pair — both trackers, one transcript, one debounced
append — cost 257 ms against a 65 MiB real transcript (capped to its 16 MiB
tail) and 98 ms against a 5.6 MiB one. The frame budget is 16 ms, so a single
append stalled the window for a quarter of a second on a large session.

ANTS-1458 phase 2 closed the *latency* half of this (the read-window race) and
deferred the incremental parse. ANTS-5050 carries the debounce, which landed
earlier and cut a burst of appends to one walk per tick — it did not cut the
cost of that one walk, which is what this does.

## Shape, and where it comes from

This follows the cursor discipline ANTS-1500 already ships for
`get_scrollback`: a cursor the caller hands back, a staleness test, and a
fall back to the full read whenever the cursor cannot be trusted. Nothing here
invents a new mechanism — the one rule that matters is **if the cursor is not
trustworthy, do the full walk**, which is what makes the edge cases a single
branch rather than a design space.

`ClaudeTranscript::Cursor` carries the byte offset, the latest-event clock and
a `primed` flag. `canResume()` decides; `walkFrom()` walks and advances the
cursor. The per-event handler and the post-walk finalize step are factored out
of each tracker's `parseTranscript` so the full and incremental paths share one
implementation — the same reason ANTS-1261 factored the walk itself out.

`parseTranscript(path)` keeps its exact signature and result. It is a public
static used by the feature tests and is now a thin wrapper over the shared
handler, so the full-parse contract is unchanged by construction.

## Contract

| # | Invariant |
|---|---|
| INV-1 | `walkFrom` advances the cursor over **complete lines only**. A trailing line with no newline is not consumed and the offset stays at its first byte, so the next call parses it once it is complete. |
| INV-2 | An incremental walk visits exactly the events in `(cursor.offset, EOF]`, in document order, with the same gating `walk()` applies — sidechain skip, compact-summary skip, blank and non-object skip. |
| INV-3 | The cursor's offset is what the walk **actually consumed**, never the pre-read size sample. A write landing during the read leaves a strictly newer on-disk signal, which is what ANTS-1458 INV-4 relies on; recording the sample as the offset would skip those bytes permanently. |
| INV-4 | `canResume` is false when the cursor is not primed, when the file is shorter than the offset (truncated or replaced by something smaller), or when the byte before the offset is not a newline (rewritten in place). A false answer obliges the caller to clear its accumulator before walking. |
| INV-5 | The 16 MiB tail cap applies to a **cold** walk only. A resumed walk starts at the cursor and reads to EOF, so an append is never dropped for sitting below the cap. |
| INV-6 | `setTranscriptPath` resets the cursor and the accumulator with the existing `m_lastRescan*` reset, so a path change can never resume against another file's offset. |
| INV-7 | For any transcript, walking it cold and walking it in arbitrary append-sized steps yield the **same tracker result** — the parse is resumable, not merely cheaper. Holds for files under the cap; over the cap a stepped walk legitimately retains history a cold walk drops, which INV-5 permits. |
| INV-8 | `parseTranscript(path)`'s signature and result are unchanged. |

## What checks this

| Invariant | Test |
|---|---|
| INV-1 | `PartialTrailingLineIsNotConsumed` |
| INV-2 | `IncrementalVisitsOnlyAppendedEvents` |
| INV-3 | `CursorTracksConsumedNotSampledSize` |
| INV-4 | `TruncationForcesFullRewalk`, `ShrinkForcesFullRewalk`, `RewriteInPlaceForcesFullRewalk` |
| INV-5 | `ResumedWalkIgnoresTailCap` |
| INV-6 | `PathChangeResetsCursor` |
| INV-7 | `SteppedParseEqualsColdParse` (both trackers) |
| INV-8 | `FullParseContractUnchanged` |

## Scope

Covers the incremental parse only. The other half ANTS-5050 still names —
one tracker per transcript path, so N panes stop meaning N walks — is an
ownership change in `MainWindow` and is not touched here. Measurement says it
is the smaller win: it removes duplicate walks but does nothing at one pane,
where this fix takes the cost to the appended delta.

Not covered: moving the walk off the GUI thread. Considered and not taken —
it hides the cost rather than removing it, and this item's own Fix line names
the incremental parse.

## Regression history

- ANTS-5050 — this item. Cost measured 2026-09-12; the item's body had
  recorded it as unmeasured since the review that filed it.
- ANTS-1458 phase 2 — closed the read-window race and deferred this. Its
  INV-4 is why INV-3 above is stated as a prohibition.
- ANTS-1261 — factored the shared walk out of the two trackers after their
  copies drifted. This extends that walker rather than adding a second one.
- ANTS-1500 — the cursor-with-stale-fallback shape this follows.
