# claude_session_freshness — ANTS-1163

> **Source:** user 2026-05-07 — *"there is still this task list
> even though I rebooted and just started up a new Claude Code
> session."*

Cold-start regression. The Task List dialog (and the bg-tasks
chip behind it) surfaces tasks from the **previous** Claude Code
session whenever the previous session's `.jsonl` is the newest
file in `~/.claude/projects/<encoded-cwd>/` and the new session
hasn't yet appended any events of its own — which is the
default state of every fresh `claude` launch up to the first
user message.

## 1. Root cause

`ClaudeIntegration::sessionPathForCwd` (and therefore
`activeSessionPath`) picks the newest `.jsonl` by **mtime
alone**. After `reboot → claude`, the prior session's transcript
is still the newest file on disk; the tracker reads it and the
chip / dialog inherits its TodoWrite tasks.

## 2. Contract

`activeSessionPath(cwd)` must return a transcript that belongs
to the **currently-running Claude Code process** — not a
left-over file from a session that ended.

The signal we use to prove "currently-running" is two-layered:

  **(a) Process-anchored identity.** When a Claude Code child
  process exists for the focused tab (`m_claudePid > 0`),
  candidate transcripts are filtered against that process's
  start time. A transcript whose most-recent event predates the
  process by more than `LEEWAY_MS` is from a prior session and
  is dropped.

  **(b) Liveness floor.** Independently of (a), any transcript
  whose most-recent event is older than `STALE_MAX_MS` (24 h)
  is dropped. This is a defensive safety net for the case where
  `m_claudePid` is unknown (e.g. the integration's process poll
  hasn't fired yet) — it still rejects week-old transcripts.

"Most-recent event timestamp" is taken from the JSONL tail (the
last event with an ISO 8601 `timestamp` field). When no
timestamped event exists in the tail window — for transcripts
that contain only metadata events such as `last-prompt`,
`permission-mode`, `file-history-snapshot`, `ai-title` — the
file's mtime is used as a fallback, since the live process is
guaranteed to bump it on every write.

## 3. Why both fixes stack

Either filter alone leaves a hole.

- **(a) alone** would let a 90-day-old transcript through if
  `m_claudePid == 0` at the moment of the call (early startup
  race).
- **(b) alone** would adopt a prior session that ended 30 minutes
  ago, even after a fresh `claude` launch — exactly the user's
  reported repro.

Stacking gives identity-when-known plus a coarse liveness floor.

### Floor split (2026-05-08 follow-up)

Filter (b) is **two-tier**:

- **Wide (24 h)** when filter (a) is active (`m_claudePid > 0`,
  PID known) — defends against truly ancient transcripts but
  doesn't reject idle long-running sessions whose last event is
  hours old.
- **Tight (5 min)** when filter (a) is inactive (cold start,
  `m_claudePid == 0`, PID not yet detected by
  `pollClaudeProcess`) — the detection window is 1-3 s in
  practice; 5 min is generous leeway. Anything older than that
  with no PID known means "no live Claude process for this
  project," so prior-session tasks should NOT surface.

Without the split, the user's repro (2026-05-08) re-triggered:
relaunched Ants + Claude, opened Task List dialog before
`pollClaudeProcess` had detected the new Claude PID, and the
wide 24h floor let yesterday's 12h-old transcript through.

## 4. INV map

INV labels qualified `ANTS-1163-INV-N`.

| #  | Lane | Statement |
|----|------|-----------|
| 1  | helper | `processStartTimeMs(pid)` returns wall-clock epoch ms for a live PID, 0 for a dead PID. |
| 2  | helper | `lastEventTimestampMs(path)` returns the most recent ISO 8601 `timestamp` from the tail, 0 when none. |
| 3  | helper | `lastEventTimestampMs` falls through metadata-only events at the tail (no `timestamp` field) to find the last timestamped event. |
| 4  | filter | `sessionPathForCwd(cwd, minLastEventMs=T, nowMs=0)` drops candidates whose effective last-event ms < T - LEEWAY. |
| 5  | filter | `sessionPathForCwd(cwd, minLastEventMs=T, nowMs=NOW)` drops candidates whose effective last-event ms < NOW - 24h. |
| 6  | filter | Among survivors, the one with the most recent effective last-event ms wins. |
| 7  | filter | Empty result when no candidate survives both filters. |
| 8  | filter | `sessionPathForCwd(cwd)` (no boundary) preserves legacy newest-by-mtime behaviour. |
| 9  | wiring | `activeSessionPath(cwd)` calls `processStartTimeMs(m_claudePid)` and threads it as `minLastEventMs` when `m_claudePid > 0`. |
| 10 | wiring | `activeSessionPath(cwd)` always passes the current epoch as `nowMs` so the 24 h liveness floor is applied even when `m_claudePid == 0`. |
| 11 | fallback | A JSONL with only metadata events (no `timestamp` field anywhere) falls back to mtime as the effective last-event ms. |
| 12 | filter | Cold-start tight floor: `sessionPathForCwd(cwd, minLastEventMs=0, nowMs=NOW)` drops a 12 h-old transcript (within the 24 h wide floor, but outside the 5 min tight floor that applies when filter (a) is inactive). |
| 13 | filter | Wide floor preserved when PID known: `sessionPathForCwd(cwd, minLastEventMs=T, nowMs=NOW)` does NOT drop a 3 h-old transcript with `T = NOW - 4h` — idle long-running Claude session shouldn't get nuked. |

## 5. Test shape

Link-based — test instantiates a `QTemporaryDir`, sets
`HOME` to it, builds `~/.claude/projects/<encoded>/<sess>.jsonl`
fixtures with controlled `lastModified()` times via `utime()`,
and exercises `ClaudeIntegration::sessionPathForCwd` directly.

The static helpers (`processStartTimeMs`,
`lastEventTimestampMs`) are exercised against the test process's
own PID and against synthetic JSONL fixtures.

INV-9 / INV-10 are source-grep against
`src/claudeintegration.cpp` since the wiring is internal to a
single function whose body is grep-stable.

## 6. Why mtime isn't enough on its own

Initial design considered "drop candidates whose mtime <
process_start". It works for 99 % of cases but corrupts on the
edge where the user opens the prior session's `.jsonl` in an
editor (mtime bumped, content unchanged). Content-based
last-event timestamp is the principled signal; mtime is the
fallback. The two-layer approach handles both robustly.
