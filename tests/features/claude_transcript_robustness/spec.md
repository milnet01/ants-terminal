# claude_transcript_robustness — feature spec

Two previously-silent failure modes in the Claude Code integration:

1. **Tail-window drops events > 32 KB.** `parseTranscriptForState` read
   the last 32 KB of the JSONL transcript and skipped the first line
   after the seek as "likely truncated". If the final event (usually a
   `tool_result` carrying inline file contents) exceeded 32 KB, the
   whole tail landed inside that one event, the firstLine-skip ate it,
   and the parser returned with zero events. State decisions silently
   fell back to "unchanged".

2. **`decodeProjectPath` mangled paths with embedded hyphens.** Claude
   Code encodes an absolute path by replacing every `/` with `-`. The
   encoding is lossy: `my-project` and `my/project` produce the same
   encoded string. The decoder previously replaced every `-` with `/`
   unconditionally, so `~/my-project/sub-dir` round-tripped as
   `~/my/project/sub/dir`.

A third fix — rendering `thinking` content blocks in
`ClaudeTranscriptDialog::formatEntry` — is a GUI-level presentation
change covered by code review rather than this spec (the dialog is a
`QDialog` and needs a `QApplication`; the fix is 10 lines of HTML
formatting with no branching logic worth automating).

## Invariants asserted here

**INV-1** — Transcript tail grows until it contains a line boundary.
Given a transcript whose final event is a single-line, 40 KB user
`tool_result`, `parseTranscriptForState` must still resolve to the
correct post-event state (the thinking/user event IS the state
determinant in this case).

**INV-2** — Tail-growth is bounded. Pathological transcripts (one
event of >4 MiB with no newlines anywhere) must return without
consuming unbounded memory. The 4 MiB cap matches the explicit
`kMaxWindow` constant in `claudeintegration.cpp`.

**INV-3** — Hyphens in leaf directory names survive decoding. Given a
real directory at `<tmp>/my-project`, `decodeProjectPath` must resolve
an encoding of that path back to the original, not to
`<tmp>/my/project`.

**INV-4** — Hyphens in intermediate segments survive decoding.
`<tmp>/my-project/sub` decoded from `-<tmp>-my-project-sub` must
return the original path when `<tmp>/my-project/sub` exists and
`<tmp>/my/project/sub` does not.

**INV-5** — Absence of filesystem hints falls back to legacy behavior.
When neither `withSep` nor `withHyphen` exists on disk, the decoder
defaults to `/` — matching the pre-0.7.14 behavior for absolute paths
without embedded hyphens, so well-formed cases don't regress.

**INV-6** — Legacy case unchanged. `-mnt-Storage-Scripts-Linux-Ants`
where `/mnt/Storage/Scripts/Linux/Ants` exists still decodes to
`/mnt/Storage/Scripts/Linux/Ants`.

## Out of scope

- Rendering/presentation of `thinking` blocks in the transcript
  dialog (manual verification; format-only change).
- Ambiguity resolution when BOTH `/a/b` and `/a-b` exist as directories
  — the greedy preference (separator wins) is documented in
  `decodeProjectPath`'s comment and accepted as best-effort. Users who
  need exact round-trips should rely on `extractCwdFromTranscript`.
