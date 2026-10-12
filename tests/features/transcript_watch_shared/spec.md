# transcript_watch_shared — ANTS-1285

The task-list and background-task trackers watch the focused tab's
transcript through one shared `ClaudeTranscriptWatch`, not a watcher
each. Behavioural: links `ants_claude_lib` and drives real trackers
against a temporary file.

## Invariants

- **INV-1** (one watch, counted): a `ClaudeTaskListTracker` and a
  `ClaudeBgTaskTracker` bound to the same path give that path two
  subscribers and one watch. Re-pointing one tracker leaves one
  subscriber and the watch. Destroying the last tracker removes both.
  Breaks if a tracker keeps a watcher of its own, or forgets to
  release on re-point or destruction.
- **INV-2** (appends reach the hub): appending to a subscribed
  transcript emits `ClaudeTranscriptWatch::changed` with that path.
  Breaks if the hub's watcher is not wired to its signal.
- **INV-3** (re-watch after replacement): when the transcript is
  replaced by an atomic rename, a tracker's `poll()` leaves the path
  watched again. Breaks if `poll()` stops re-adding a dropped watch.

## Tests

`test_transcript_watch_shared.cpp`, suite `TranscriptWatchShared`:
`Inv1OneWatchCounted`, `Inv2AppendReachesHub`,
`Inv3RewatchAfterReplace`.
