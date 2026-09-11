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

**ANTS-2191 refinement.** The mtime fallback is same-UID-spoofable
(any process running as the user can `touch` a transcript; ADR-0004
treats this as an integrity smell, not a privilege crossing). So the
fallback is gated on filter (a) being inactive: when a live PID anchor
exists (`minLastEventMs > 0`) a candidate with no content timestamp is
**rejected** rather than adopted via mtime — a tampered transcript's
mtime must not pass the freshness filter and bind the wrong session
(a narrowed re-open of this spec's own wrong-session bind). With no
PID anchor (`minLastEventMs == 0`) the mtime fallback is retained — it
is the only freshness signal available, and the liveness floor (b)
still bounds staleness.

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
| 11 | fallback | (ANTS-2191) A metadata-only JSONL (no `timestamp` field) is **rejected** when a live PID anchor exists (`minLastEventMs > 0`) — no spoofable-mtime fallback. |
| 11b | fallback | (ANTS-2191) With no PID anchor (`minLastEventMs == 0`) a metadata-only JSONL still uses the mtime fallback (only freshness signal available; liveness floor (b) bounds staleness). |
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

## 7. ANTS-5048 — every status tick opens every transcript

### Rationale

**Source:** roadmap item ANTS-5048, found independently by two
review lanes. `ClaudeStatusBarController` calls `activeSessionPath`
several times per 2-second tick, whether or not Claude is running
for the focused shell. `sessionPathForCwd` already lists a
project's transcripts newest-mtime-first (`QDir::Time`) but never
uses that order: it opens and JSON-parses every `*.jsonl` in the
directory on every call. A project with over a hundred top-level
transcripts pays for all of them, every tick, on the GUI thread.

### Scope

In scope: the observable return value of `sessionPathForCwd` under
fixtures designed to prove a candidate transcript was never
*opened* — the only way to observe that from outside the function.
Out of scope: the actual per-call cost (not measured here), the
`activeSessionPath` call-count reduction and the per-cwd/pid
memoisation named in the roadmap item's fix — those are un-shipped
follow-up work, not locked by this spec.

### Regression history

Not yet fixed as of this writing. The defect is in the loop body
of `sessionPathForCwd` in `src/claudeintegration.cpp`, which the
prior sections of this document (§§ 1-6) already describe and test
for filtering *correctness* — this section locks a *cost* contract
on top of that: a candidate ruled out by mtime alone must never be
opened, whatever its content claims.

### Invariants

The numbering continues §4's INV map — ANTS-1192-INV-16 was the
last id in use. A transcript's content timestamp can never
legitimately be newer than its own mtime (an event is written when
the file is appended to), so a candidate already ruled out by mtime
cannot legitimately win once opened either. Each invariant below
forges a transcript whose content timestamp lies about being newer
than its own mtime — impossible for a real transcript, but the only
way to observe, from the returned path alone, whether a candidate
was ever opened.

- **INV-17** — `sessionPathForCwd(cwd, 0, now)`: a transcript whose
  mtime already fails the liveness floor is never opened, even when
  its content claims a last event newer than a fresh transcript's.
  *Test:* `ClaudeSessionFreshness.MtimeShortCircuit`.
- **INV-18** — `sessionPathForCwd(cwd, minLastEventMs, 0)`: a
  transcript whose mtime is older than `minLastEventMs` minus the
  clock-skew leeway is never opened, even with a forged-fresh
  content timestamp. *Test:* `ClaudeSessionFreshness.MtimeShortCircuit`.
- **INV-19** — among mtime-sorted candidates, one whose mtime
  cannot beat the best effective timestamp already found is never
  opened, even with a forged-newer content timestamp. *Test:*
  `ClaudeSessionFreshness.MtimeShortCircuit`.
- **INV-20** (guard) — the freshest valid transcript is still
  returned among several valid candidates. *Test:*
  `ClaudeSessionFreshness.MtimeShortCircuit`.
- **INV-21** (guard) — a transcript with no content timestamp
  still falls back to mtime when no PID anchor is known. *Test:*
  `ClaudeSessionFreshness.MtimeShortCircuit`.
- **INV-22** (guard) — a directory holding only stale transcripts
  still returns empty. *Test:*
  `ClaudeSessionFreshness.MtimeShortCircuit`.

INV-17 through INV-19 are expected to fail against the current
implementation — the loop opens every candidate regardless of mtime
order, so the forged content timestamp wins. INV-20 through INV-22
are guards: they carry no forged fixture and must pass both before
and after a fix.
