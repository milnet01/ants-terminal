# Feature: transcript resolution retries until it succeeds

## Problem

`ClaudeIntegration::pollClaudeProcess` runs on a two-second timer. When
it finds a Claude process under the focused tab's shell it enters a
branch gated on `m_claudePid != foundPid` — true on first detection,
since `m_claudePid` starts at zero — and does two things in order:
commits `m_claudePid = foundPid`, then resolves the session transcript
through `sessionPathForCwd` and stores it in `m_transcriptPath`.

The commit happens whether or not the resolution succeeds. So if
`sessionPathForCwd` returns empty, the next poll sees `m_claudePid ==
foundPid`, does not re-enter the branch, and never tries again. The
backstop re-parse at the end of the same function cannot rescue it: it
is itself gated on `m_transcriptPath` being non-empty.

Returning empty on the first attempt is ordinary, not exotic.
`sessionPathForCwd` returns empty when `/proc/<shellPid>/cwd` cannot be
read, when the project directory under `~/.claude/projects` does not
exist yet — which is the case the first time Claude runs in a project —
and when no transcript passes the process-anchored freshness filter,
which is what happens while Claude has started but not yet written its
first event. Detection happens as soon as the process is a child of the
shell; the first transcript write is not synchronised with that.

The consequence lasts for the life of the process. With
`m_transcriptPath` empty, `processHookEvent` treats every hook as a
cold start, and `isFocusedTabSession` cannot attribute a session, so
hook events are dropped. Only switching tabs recovers it, because
`setShellPid` zeroes `m_claudePid` and forces a fresh detection.

## What the original report got wrong

ANTS-4457 filed this as "transcript lookup sits only inside the
PID-changed branch, so on the ordinary launch path `m_transcriptPath`
stays empty for the life of the process". The conclusion is right and
the mechanism is not: that branch *is* entered on the ordinary launch
path, because initial detection is a PID change from zero. The defect
is that the attempt is made exactly once and the gate that would allow
another has already been satisfied.

The distinction decides the fix. Nothing needs moving out of the
branch; the branch needs a second reason to run.

## Contract

While a Claude process is running under the focused tab's shell and no
transcript has been resolved for it, each poll MUST re-attempt the
resolution. The attempt stops as soon as it succeeds.

Retrying costs one `QFileInfo` on `/proc`, one process-start read, and
one scoped directory listing per poll — the same order as the `/proc`
walk the poll already performs, and only while a transcript is
outstanding.

The state transition to `Idle` stays gated on the PID change alone.
It is about a newly detected process, not about the transcript, and
re-emitting it per poll is the flapping the existing guard was added to
prevent.

## Invariants

**INV-1 — the resolution block is not gated on the PID change alone.**
Source-grep `ClaudeIntegration::pollClaudeProcess`: the condition
guarding the `sessionPathForCwd` call also tests `m_transcriptPath`
emptiness.

**INV-2 — the backstop re-parse still requires a resolved path.** The
tail of the function keeps its `!m_transcriptPath.isEmpty()` guard;
re-parsing an empty path is not the retry mechanism and must not become
one.

**INV-3 — the Idle transition stays on the PID change.** The
`ClaudeState::Idle` assignment is not reached by the
transcript-emptiness condition.

## Scope

### In scope
- Source-grep over `src/claudeintegration.cpp`.

### Out of scope
- A runtime test. `pollClaudeProcess` reads `/proc` for a live Claude
  process under a live shell; the unit bundle has neither. The header
  exposes `setTranscriptPathForTest`, which sets the field the poll is
  supposed to populate, so it cannot stand in for this.
- Bounding the number of retries. The attempt is cheap and stops on
  success; a cap would reintroduce the stranding this fixes.
- The cold-start drop in `processHookEvent`, which is correct
  behaviour given an empty path and is what made this defect
  invisible rather than being the defect.

## Regression history

- **ANTS-1225:** widened the gate from `m_claudePid == 0` to
  `m_claudePid != foundPid` so a replaced PID rebinds. The failed
  resolution case was not considered.
- **ANTS-4456's sibling ANTS-4457 (cold sweep 2026-08-18, verified
  2026-09-08):** verified against source with the mechanism corrected,
  and fixed. Locked by this spec.
