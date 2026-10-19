# ANTS-5773 — "Claude: idle" means nothing is pending

**Status:** spec draft (2026-10-19).
**Kind:** feature.
**Source:** ROADMAP.md ANTS-5773 (user-request-2026-10-19: a screenshot of "Claude: idle" while Claude Code's own footer read "1 shell · 1 agent").
**Amends:** ANTS-1873 (resolver), and the feature contracts `claude_state_resolver`, `claude_state_dot_palette`, `claude_bg_tasks_button` (§ 2.6 names each clause).

**Layman:** The status bar and the tab dot stop saying Claude is idle while a background shell, helper agent or monitor it started is still running; they say "waiting" in pink instead.

## 1. Problem

The status bar and the tab dot both read one resolver: `claudestate::display()` in `src/claudestateresolver.cpp`, fed by `ClaudeTabTracker::ShellState`. It returns `Display::Idle` whenever the transcript's last assistant event ended its turn (`ClaudeIntegration::parseTranscriptTail`). Nothing it reads describes background work. So a turn that ends while a background shell, agent or monitor still runs shows "Claude: idle" and a grey dot.

The terminal does count background work, for the Background Tasks button (`ClaudeBgTaskTracker`, `src/claudebgtasks.cpp`), but that count cannot stand in for it:

1. **It misses today's records.** `applyBgEvent` starts a task only on a `tool_use` carrying `input.run_in_background`. It ends one only on `KillShell`, or on a `tool_result` whose `toolUseResult.status` is completed, killed or failed. Claude Code (probed 2026-10-19, this session's transcript) also:
   - moves a foreground shell to the background on timeout, with no flag on its `tool_use`;
   - starts an agent with no flag, recording `toolUseResult.status: "async_launched"` and `agentId`;
   - starts a monitor recording `toolUseResult.taskId`;
   - ends all three with a `<task-notification>` carrying the task id and a `<status>`.
   No `<task-notification>` is read anywhere.
2. **Silence ends a task.** `finalizeBgTasks` and `sweepLiveness` mark a task finished when its output file is unwritten for 60 s (`outputLooksStale`). A shell waiting quietly on a long job (the screenshot's case: a push gate, silent for minutes) reads finished within a minute.
3. **Only the focused tab is counted.** `ClaudeStatusBarController::refreshBgTasksButton` is the only caller that gives a per-PID tracker its transcript path, so another tab's count is 0 or stale.

The silence rule had a reason (`claude_bg_tasks_button` spec, INV-12): tasks whose end was never recorded lingered as phantoms. Old sessions keep their task output files in `/tmp/claude-$UID/` after they exit (measured 2026-10-19: one ended session still held 20), so "the output file is gone" cannot retire them. This spec replaces silence with the Claude process's start time (§ 2.2).

## 2. Surface

### 2.1 Which records start and end a task

`applyBgEvent` keeps its current rules and gains these. Each task carries a **kind**, `ClaudeBackgroundTask::kind` of `enum class Kind { Shell, Agent, Monitor }`; § 2.3's counts are taken by it.

| Record | Effect |
|---|---|
| assistant `tool_use` named `Bash`, `Agent`, `Task` or `Monitor` | Remember it by `tool_use_id`: name, description, command or prompt (truncated as today). Forgotten when its `tool_result` arrives. |
| user `tool_result`, `toolUseResult.backgroundTaskId` set | A flagged launch's task for this `tool_use_id` takes the id, as today. With no such task, start a `shell` task with that id. |
| user `tool_result`, `toolUseResult.status == "async_launched"` and `agentId` set | Start an `agent` task, id = `agentId`, output path = `toolUseResult.outputFile`. |
| user `tool_result` for a remembered `Monitor` call, `toolUseResult.taskId` set | Start a `monitor` task, id = `taskId`. |
| `<task-notification>` block whose `<task-id>` names a task and which carries a non-empty `<status>` | End that task. Read from a `user` record's text and from a `queue-operation` record's `content`; the first one seen ends it. A block without `<status>` (a monitor's per-event notice) ends nothing. |
| assistant `tool_use` named `TaskStop` (`input.task_id`, or the deprecated `input.shell_id`) | End that task, as `KillShell` already does. |

The 60 s silence rule is removed from both `finalize` (§ 2.2) and `sweepLiveness`. A missing output file still ends a task.

### 2.2 The process-start rule

`ClaudeBgTaskTracker` gains a parameter on `parseIncremental`, a public `finalize` and a setter:

```cpp
// A task whose startedAt precedes processStartMs is not running: it
// belonged to an earlier Claude process. 0 = unknown; the rule is skipped.
static QList<ClaudeBackgroundTask> parseIncremental(
    const QString &path, ClaudeTranscript::Cursor &cursor,
    ClaudeBgTaskAccum &acc, qint64 processStartMs = 0);
// Today's finalizeBgTasks, made public; it applies the rule above.
static QList<ClaudeBackgroundTask> finalize(const ClaudeBgTaskAccum &acc,
                                            qint64 processStartMs = 0);
// The button's tracker passes this value to both.
void setProcessStartMs(qint64 ms);
```

The tab tracker calls the two static functions with its entry's `claudeStartMs`. `ClaudeTabTracker::detectClaudeChild` already computes `ClaudeIntegration::processStartTimeMs(found)` for the tab's Claude process; it keeps the value in the shell's entry.

### 2.3 Per-tab count

`ClaudeTabTracker::ShellState` gains:

```cpp
int backgroundShells = 0;   // pending tasks by kind, for this tab's
int backgroundAgents = 0;   // Claude process, per § 2.1 and § 2.2
int backgroundMonitors = 0;
qint64 claudeStartMs = 0;   // processStartTimeMs of the tab's Claude; 0 = unknown
```

Each shell entry holds its own `ClaudeTranscript::Cursor` and `ClaudeBgTaskAccum`. `reparseTranscript` runs `ClaudeBgTaskTracker::parseIncremental` on the entry's transcript alongside the tail parse. `pollAllShells` (every 2 s) re-runs `finalize`, so a deleted output file is noticed without a transcript write. A change in any count emits `shellStateChanged`. A new Claude PID resets cursor, accumulator and counts.

`ClaudeStatusBarController::refreshBgTasksButton` passes the focused tab's `claudeStartMs` to the button's tracker.

### 2.4 Resolver

```cpp
struct Resolved { /* existing fields */ int backgroundPending = 0; };
enum class Display { /* existing values */ Background };
```

`fromShell` sets `backgroundPending` to the sum of the three counts. In `display()`'s switch, `ClaudeState::Idle` returns `Display::Background` when `backgroundPending > 0`, else `Display::Idle`. Every branch before the switch (awaiting input, planning, auditing, not running) is unchanged and outranks it. `ClaudeState` gains no value.

### 2.5 What the user sees

- **Tab dot:** `ClaudeTabIndicator::Glyph::Background`, base colour `#F07AB4` (pink), adapted by `ClaudeTabIndicator::contrastColor` like every other dot.
- **Status bar:** `Claude: waiting (N bg)`, N = `backgroundPending`, in the same colour. Tooltip: `N background tasks running: S shells, A agents, M monitors`, omitting a zero kind.
- **`pinSlots`:** the state list gains `waiting (88 bg)`.

### 2.6 Clauses this changes in other specs

- **ANTS-1873 INV-1** — the lossless adapter also carries `backgroundPending` (the sum of § 2.3's counts).
- **`claude_state_resolver` spec** — gains the INV-1 below as its coverage of `Display::Background`.
- **`claude_state_dot_palette` spec** — the table gains a Background row (pink, `#F07AB4`). Its INV-2 and INV-3 lists and the "all eight" wording in invariant 8 become nine. Its precedence (invariant 6) inserts Background directly before Idle.
- **`claude_bg_tasks_button` spec** — INV-1 recognises § 2.1's start records rather than only `run_in_background`. INV-12's condition (b), the 60 s staleness window, is withdrawn. INV-13's un-latch then applies only to a task finished by a missing file.

## 3. Invariants

- **INV-1** — `display()` returns `Background` iff `base == Idle`, `backgroundPending > 0`, and none of awaiting input, planning or auditing is set. With `backgroundPending == 0` it returns `Idle` exactly as today. No other base changes with the count.
  *Test:* `tests/features/claude_state_resolver/test_claude_state_resolver.cpp`, a table over every base × {0, 2} × each overlay.
  *Breaks when:* the count is checked outside the `Idle` case (a thinking tab shows "waiting"), or before an overlay (a prompting tab shows pink).
- **INV-2** — A `tool_result` with `backgroundTaskId` starts a `shell` task even when its `tool_use` carried no `run_in_background`.
  *Test:* `tests/features/claude_bg_tasks_button/test_claude_bg_tasks_button.cpp`, a transcript holding a timed-out foreground Bash.
  *Breaks when:* a start still requires the flag. The fixture's only start record is the unflagged one, so the old rule alone yields zero tasks.
- **INV-3** — `async_launched` with `agentId` starts an `agent` task, and a `Monitor` result with `taskId` starts a `monitor` task. Each counts in its own field.
  *Test:* same file, one fixture per kind.
  *Breaks when:* either kind is not recognised, or is counted under `shell`.
- **INV-4** — A `<task-notification>` with a non-empty `<status>` ends the task it names, from a `user` record or a `queue-operation` record. One without `<status>` ends nothing.
  *Test:* same file. Fixtures: a monitor with two status-less events, then a status block; and a shell ended only by a `queue-operation`.
  *Breaks when:* a status-less event ends a monitor, or one record form is ignored.
- **INV-5** — `TaskStop` ends the task named by `task_id` or `shell_id`.
  *Test:* same file.
  *Breaks when:* only `KillShell` is read.
- **INV-6** — Silence does not end a task: one whose output file exists but is unwritten for longer than 60 s stays pending, in both `finalize` and `sweepLiveness`.
  *Test:* same file; the output file's mtime set 10 minutes back.
  *Breaks when:* `outputLooksStale`'s mtime test remains. The fixture holds no finish record, so the mtime is the only thing that can end it.
- **INV-7** — A missing output file still ends a task.
  *Test:* same file.
  *Breaks when:* INV-6's change also drops the existence check.
- **INV-8** — A task whose `startedAt` precedes the process start time (§ 2.2) is not pending. With 0 the rule is skipped.
  *Test:* same file; one task an hour before the start, one after.
  *Breaks when:* phantoms from an earlier Claude process count, or a 0 start retires every task.
- **INV-9** — Each tracked shell's counts come from its own transcript: a tab that was never focused shows `Background` when its transcript holds a running task.
  *Test:* `tests/features/claude_status_bar_per_tab/test_claude_status_bar_per_tab.cpp`, two tracked shells via `ClaudeTabTracker::forceRefreshForTest`, the second never focused.
  *Breaks when:* the count still comes only from `refreshBgTasksButton`'s focused-tab tracker.
- **INV-10** — `Glyph::Background`'s base colour is `#F07AB4`. After `contrastColor` it clears WCAG 3:1 on the Light and Catppuccin-Latte backgrounds with its hue kept, and it differs in hue from `Glyph::AwaitingInput` and `Glyph::Auditing`.
  *Test:* `tests/features/claude_state_dot_palette/test_claude_state_dot_palette.cpp` (INV-3's literal list and INV-9's loop gain the glyph).
  *Breaks when:* the glyph reuses another state's colour, or misses the floor.
- **INV-11** — The status label reads `Claude: waiting (N bg)` with the per-kind tooltip, and `pinSlots` includes `waiting (88 bg)`.
  *Test:* `tests/features/claude_status_bar/test_claude_status.cpp`.
  *Breaks when:* the label or tooltip drifts from § 2.5, or the slot list omits it (the pill would clip).
- **INV-12** — `ClaudeState` has no new value, so `tab_list`'s `claude_state` still reads `idle` for a waiting tab, and `ModelAutoSwitch` and `idleSinceMs` see Idle as before.
  *Test:* source-grep: `enum class ClaudeState` in `src/claudeintegration.h` still lists exactly NotRunning, Idle, Thinking, ToolUse, Compacting.
  *Breaks when:* a value is added for this state.

## 4. Failure modes

- **Claude Code renames a record.** Starts or ends go unseen. A missed start shows idle, as today. A missed end leaves a task pending until its output file goes or Claude restarts. That is the visible failure, so INV-2 to INV-5 pin today's shapes, and the fixtures are copied from a real transcript.
- **A status this spec does not know.** Any non-empty `<status>` ends a task. A future non-final status would end it early, and the tab shows idle: today's behaviour, never a stuck pink.
- **A task started before the transcript's walked window.** `ClaudeTranscript::walkFrom` reads at most the last 16 MiB on a cold start. Such a task is not counted, and the tab shows idle, as today.
- **`/proc` unreadable.** `processStartTimeMs` returns 0, INV-8's rule is skipped, and phantoms from an earlier Claude process may count until their output files go.
- **Subagents' own background tasks.** `walkFrom` skips `isSidechain` events, so they are not counted.

## 5. RAM / build cost

Per tracked shell: one cursor, one accumulator holding that session's background tasks, plus the in-flight `tool_use` map, which empties as results arrive. No new build target. The 2 s poll adds one `stat()` per pending task.

## 6. Out of scope

- Scheduled and recurring jobs (`CronCreate`) — not counted at all (user, 2026-10-19: they can run for days, and Claude is not waiting on them).
- A `background_tasks` field on `tab_list` — deferred; not yet queued.
- Tasks inside a subagent's own transcript — not counted at all, because `walkFrom` skips sidechain events.

## 7. Tests

Feature tests named per invariant in § 3, in their existing bundles. Label `features;fast`. The bundles are `build_target_for`'s answer for each file, rebuilt before the run. Fixture records are copied from a real 2026-10-19 transcript. Each new test is run against pre-change source first and must fail there, except the two that guard behaviour this spec keeps: a missing output file ends a task, and `ClaudeState` gains no value.

## 8. Reload

Code only the terminal serves: the tab tracker, the resolver, the tab bar and the status bar all live in the terminal process. It reaches a running terminal only through a relaunch, which closes the Claude Code sessions in its tabs (`~Pty`). It has no stop/start control because it holds no state a restart of its own could rebuild: every count is re-derived from the transcript on the next parse.

## 9. Decisions and alternatives

User decisions, 2026-10-19:

- **What counts:** background shells, agents and monitors; not scheduled jobs.
- **Look:** a new label and its own dot colour.
- **Colour:** pink `#F07AB4`. The prompt red and the idle grey are excluded. Orange was chosen first and withdrawn: on 2026-10-15 (ANTS-5718) the user found orange read as the ToolUse yellow. White was rejected because contrast adaptation darkens it to grey on light themes.
- **Label:** `Claude: waiting (N bg)`, the width of today's widest state, with the full wording in the tooltip. The full sentence would nearly double the pinned pill.

Rejected:

- **A new `ClaudeState` value.** `ModelAutoSwitch::decide`, `idleSinceMs` and `tab_list`'s published enum key on `ClaudeState::Idle`, so each would change meaning.
- **Reading Claude Code's footer ("1 shell · 1 agent") from the screen.** It is drawn only while visible, its wording is not a contract, and an unfocused or scrolled tab has none.
- **Keeping the 60 s silence rule with a longer window.** Any window misreads a long quiet job. The process-start rule removes the phantoms it existed for.

## 10. What checks this

| Rule | What catches a breach |
|------|----------------------|
| INV-1 | `test_claude_state_resolver.cpp` |
| INV-2 – INV-8 | `test_claude_bg_tasks_button.cpp` |
| INV-9 | `test_claude_status_bar_per_tab.cpp` |
| INV-10 | `test_claude_state_dot_palette.cpp` |
| INV-11 | `test_claude_status.cpp` |
| INV-12 | source-grep in `test_claude_state_resolver.cpp` |
| Claude Code's record shapes staying as § 2.1 states | **nothing** — fixtures freeze today's shapes, and a rename upstream shows only as a tab stuck pink or showing idle |

## 11. Cross-doc impact

- The three feature specs and ANTS-1873, per § 2.6.
- CHANGELOG `[Unreleased]`: one Added line.
- `docs/subsystems.md`: no change; no file moves.

## 12. Open questions

None.

## Cold-eyes loop log

Rows live in [`../reviews/ANTS-5773-idle-means-nothing-pending-loop-log.md`](../reviews/ANTS-5773-idle-means-nothing-pending-loop-log.md).
