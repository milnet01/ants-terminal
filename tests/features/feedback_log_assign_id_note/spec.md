# Feature spec: `feedback_log op:assign_id` honours `note` (ANTS-3571)

## Problem

`feedback_log op:assign_id` (the v2 inline-triage write, ANTS-3447) accepts
a `note` param documented in the schema as "Optional prose under the
heading". The handler read `heading`, `ids`/`closure`, `heading_line` and
`dry_run` but **never read `note`** — so `assign_id{ids:[…], note:"…"}`
wrote only the `**Proposed ID:**` line and dropped the note silently
(3D_Engine feedback 2026-07-18). A maintainer's closure note (e.g. "fixed —
please relaunch") never reached the contributor session.

## Surface

- `RemoteControl::cmdFeedbackLog` assign_id branch (the remotecontrol TUs).
- `FeedbackFile::assignId` engine (`src/feedbackfile.cpp`) + `AssignTarget`
  / `AssignResult` (`src/feedbackfile.h`).
- `feedback_log` `note` schema description (`src/claudeintegration.cpp`).

## Invariants

- **INV-1 — note is written under the id line.** When a non-empty `note`
  is supplied, `assignId` renders it as a single `- **Note:** <note>`
  bullet directly under the finding's `**Proposed ID:**` line (before the
  next body bullet). `AssignResult.noteWritten` is set; the envelope
  echoes `note_written:true`.
- **INV-2 — newlines fold to spaces.** The wrapper folds any newline /
  control char in `note` to a space and trims, so the bullet stays one
  line (the finding block is one bullet per line). An empty-after-fold
  note leaves the block note-free.
- **INV-3 — note is replaced in place, never duplicated.** A re-assign
  with a different note overwrites the existing `- **Note:**` bullet in
  the same finding rather than appending a second one.
- **INV-4 — idempotency spans id AND note (INV-8 of ANTS-3447).**
  `AssignResult.changed` is the final byte comparison of the whole file,
  so a re-assign with the same id + note is a byte-identical no-op
  (`changed:false`, `bytes_delta:0`) and skips the write.
- **INV-5 — no note ⇒ prior behaviour.** `assign_id` without `note`
  behaves exactly as before: only the id line is touched, no `- **Note:**`
  bullet, `note_written` absent from the envelope.

## Test scope

Pure `FeedbackFile::assignId` over a synthetic v2 fixture for placement /
idempotency / replace-in-place, plus a live `RemoteControl::cmdFeedbackLog`
drive for the exact repro (ids + multi-line note → note on disk +
`note_written`) and the schema description grep.
