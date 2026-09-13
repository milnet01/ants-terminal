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
| INV-6 | `setTranscriptPath` resets the cursor and the accumulator with the existing `m_lastRescan*` reset, then takes the state kept for the new path, if any (INV-9). State kept for one path is never used for another, so a path change can never resume against another file's offset. |
| INV-7 | For any transcript, walking it cold and walking it in arbitrary append-sized steps yield the **same tracker result** — the parse is resumable, not merely cheaper. Holds for files under the cap; over the cap a stepped walk legitimately retains history a cold walk drops, which INV-5 permits. |
| INV-8 | `parseTranscript(path)`'s signature and result are unchanged. |
| INV-9 | After each parse a tracker keeps its cursor and accumulator under the transcript path, in a `ClaudeTranscript::WalkCache` shared by every tracker of its type. A tracker binding a path with kept state resumes from it: a tab switch back, or a second pane on the same transcript. `rescan`'s `canResume` check still walks cold when the kept cursor no longer holds. |
| INV-10 | The cache keeps at most `WalkCache::kMaxPaths` paths, most recent first, and never keeps an unprimed cursor. Each entry is a cursor and one tracker's accumulator. |

## What checks this

| Invariant | Test |
|---|---|
| INV-1 | `PartialTrailingLineIsNotConsumed` |
| INV-2 | `IncrementalVisitsOnlyAppendedEvents` |
| INV-3 | `CursorTracksConsumedNotSampledSize` |
| INV-4 | `TruncationForcesFullRewalk`, `ShrinkForcesFullRewalk`, `RewriteInPlaceForcesFullRewalk` |
| INV-5 | `ResumedWalkIgnoresTailCap` |
| INV-6 | `PathChangeResetsCursor`, `PathChangeNeverTakesAnotherPathsState` |
| INV-7 | `SteppedParseEqualsColdParse` (both trackers) |
| INV-8 | `FullParseContractUnchanged` |
| INV-9 | `RebindResumesTaskListFromKeptState`, `RebindResumesBgTasksFromKeptState`, `SiblingTrackerResumesFromKeptState` |
| INV-10 | `WalkCacheKeepsOnlyTheMostRecentPaths` |

## Scope

Covers the incremental parse and the per-path cache of its state (INV-9,
INV-10). The user chose that cache on 2026-09-12 over sharing one tracker per
transcript path. It removes the cold walk a tab switch back or a second pane
on the same transcript used to pay, and leaves tracker ownership unchanged.

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
