# Claude Code background-tasks status-bar button

## Problem

Claude Code can spawn long-running tasks via `Bash` or `Task` with
`run_in_background: true`. The TUI shows a sidebar listing them with
their tail output; users dropping into a different tab to keep working
have no way to see those tasks from Ants Terminal — they're forced to
flip back to the TUI and parse the sidebar by hand.

User request 2026-04-25:
> "a button on the status bar when there are background tasks being
> run. We then click the button to view what Claude Code shows for
> the background tasks. The button opens a dialog showing the live
> update info on the background tasks."

## Fix (0.7.38)

Three pieces:

1. **Tracker** — `ClaudeBgTaskTracker` (`src/claudebgtasks.{h,cpp}`).
   Owns a `QFileSystemWatcher` on the active transcript JSONL,
   re-parses on every change, and exposes
   `tasks() → QList<ClaudeBackgroundTask>` plus a `runningCount()`
   convenience. The static `parseTranscript(path)` helper is the pure
   parser — no signals, no watcher — for direct test use. It walks
   the JSONL, recognizes:
   - `assistant` events with a `tool_use` block whose
     `input.run_in_background == true` → start a partial entry keyed
     by `tool_use.id`.
   - `user` events with a `tool_result` whose
     `toolUseResult.backgroundTaskId` is set → finalize the entry by
     stamping its public id and parsing the output path out of the
     "Output is being written to: …" body.
   - `user` events with `toolUseResult.status` of `completed`,
     `killed`, or `failed` → mark the matching id finished. Match by
     `shellId`/`bash_id` when present, otherwise scan the result text
     for tracked ids.
   - `assistant` events whose `tool_use.name == "KillShell"` →
     finished by `shell_id`.

2. **Status-bar button** — `m_bgTasksBtn` on the
   `ClaudeStatusBarController` (post-ANTS-1146; pre-1146 it was
   `m_claudeBgTasksBtn` on MainWindow), sibling to the Review
   Changes button, hidden by default. Shown only while
   `m_bgTasks->runningCount() > 0` for the active tab's session.
   Label: "Background Tasks (N)" where N is the
   running count. Tooltip discloses running + total. Re-targeted on
   tab switch via `refreshBgTasksButton()` (called from
   `refreshStatusBarForActiveTab`).

3. **Live-tail dialog** — `ClaudeBgTasksDialog`
   (`src/claudebgtasksdialog.{h,cpp}`). Mirrors the Review Changes
   live-update model:
   - `QFileSystemWatcher` on each task's `.output` file plus the
     transcript path (so new starts and completions land
     automatically).
   - 200 ms debounce timer collapses bursts into a single render.
   - Skip-identical-HTML guard via a per-dialog `m_lastHtml`
     (`std::shared_ptr<QString>`).
   - Capture-vbar/hbar before `setHtml`, restore after with a
     `qMin(value, maximum())` clamp — same scroll-preservation
     pattern that fixed Review Changes in 0.7.37.
   - "Was at bottom" detection: when the user is tailing the bottom
     of the output, keep them pinned to the bottom so live appends
     stay visible without manual scrolling. Otherwise restore their
     absolute position.

## Invariants

Source-grep harness, no Qt link. Each invariant pins one piece of
the wire-up that, if reverted, breaks the user-visible behavior.

- **INV-1** (parser detects `run_in_background`):
  `claudebgtasks.cpp` references the `run_in_background` JSON key
  inside `parseTranscript`. Without it the parser never sees any
  starts and the tracker is permanently empty.

- **INV-2** (parser correlates by `backgroundTaskId`):
  `claudebgtasks.cpp` references `backgroundTaskId` to finalize
  entries. Without it the tracker has no way to map a launch to its
  output path or to subsequent completion events.

- **INV-3** (tracker emits `tasksChanged`):
  `ClaudeBgTaskTracker` declares a `tasksChanged()` signal and
  emits it from `rescan()` only when the task-list shape changes.
  Without the signal the dialog and button never live-update.

- **INV-4** (tracker uses `QFileSystemWatcher`):
  `ClaudeBgTaskTracker::setTranscriptPath` adds the path to a
  `QFileSystemWatcher` member. Polling-only would not pick up
  per-character appends from the in-flight transcript writer.

- **INV-5** (status-bar button hidden when no tasks running):
  `ClaudeStatusBarController::refreshBgTasksButton` calls `hide()`
  when `runningCount() <= 0`. (Post-ANTS-1146 — pre-1146 this was
  `MainWindow::refreshBgTasksButton`.) Without this the button
  stays visible after every task finishes, which is the inverse
  of the user's ask.

- **INV-6** (status-bar button click opens the dialog): the
  controller's `m_bgTasksBtn` clicked-signal is relayed via
  `ClaudeStatusBarController::bgTasksClicked`, which MainWindow
  connects to its `showBgTasksDialog` slot. `showBgTasksDialog`
  constructs a `ClaudeBgTasksDialog` and calls `show()`.
  (Post-ANTS-1146 — pre-1146 the click was a direct
  `connect(m_claudeBgTasksBtn, …, MainWindow::showBgTasksDialog)`.)

- **INV-7** (dialog reuses scroll-preservation pattern):
  `ClaudeBgTasksDialog::rebuild()` declares a `*m_lastHtml == html`
  early-return guard, captures `verticalScrollBar()` value before
  `setHtml`, and restores after with a `qMin(... , maximum())`
  clamp. Same shape as `MainWindow::showDiffViewer` — diverging
  here would re-open the 0.7.37 scroll-reset bug for the new
  dialog.

- **INV-8** (dialog watches output files):
  `ClaudeBgTasksDialog` adds each task's `outputPath` to its
  `m_watcher` via `rewatch()`. Without this the live-tail pane
  shows a stale snapshot until the user hits Refresh manually.

- **INV-9** (dialog debounces with 200 ms timer):
  `ClaudeBgTasksDialog::m_debounce` is a `QTimer` configured with a
  ≤500 ms interval and `setSingleShot(true)`. A noisy `make -j` can
  emit dozens of fileChanged signals per second; without the
  debounce we'd render that many times.

- **INV-10** (CMake builds the new sources):
  `CMakeLists.txt` lists both `src/claudebgtasks.cpp` and
  `src/claudebgtasksdialog.cpp` in the `ants-terminal` target.

- **INV-11** (project-scoped lookup, added 0.7.44 — user feedback
  2026-04-27 "ensure that it only references the background tasks
  for that project, not all projects"):
  `ClaudeIntegration::activeSessionPath` accepts a
  `const QString &projectCwd` argument. Its body walks up
  `projectCwd` via `cdUp`, encoding each ancestor with
  `encodeProjectPath` to Claude Code's `<dashed-cwd>` directory
  name, and returns the newest `*.jsonl` from the deepest matching
  `~/.claude/projects/<encoded>/`. `ClaudeStatusBarController::refreshBgTasksButton`
  (moved from MainWindow by ANTS-1146) reads the active tab's
  cwd via the `m_focusedTerminalProvider` callback (which
  MainWindow installs as `[]{ return focusedTerminal(); }`) and
  passes it through. Without all three pieces, the dialog
  shows the system-wide newest `.jsonl` regardless of the active
  tab's project — visible to the user as "background tasks from
  another window's project leaking into this window's button". The
  source-grep checks the header signature, implementation walk-up,
  and the call-site wiring.

- **INV-12** (liveness sweep, added 0.7.49 — user feedback
  2026-04-27 "Background Tasks are showing 12 tasks but there
  shouldn't be any"):
  Transcript-only completion detection misses tasks whose
  completion events Claude Code never recorded (the assistant
  moved on without polling `BashOutput`). Those linger as
  `finished == false` indefinitely, producing a phantom
  running-count chip on the status bar. The fix adds a liveness
  sweep at the end of `parseTranscript`: for each unfinished task
  with a non-empty `outputPath`, mark it finished if (a) the
  output file no longer exists OR (b) its mtime is older than a
  60 s staleness window. Pieces:
  1. `claudebgtasks.cpp::parseTranscript` references
     `lastModified()` AND `QFileInfo::exists()` (or `fi.exists()`).
  2. `ClaudeBgTaskTracker::rescan` is exposed in `public slots:`
     so the periodic-tick path can call it directly.
     `setTranscriptPath` short-circuits when the path is
     unchanged, so it can't be the entry point for the periodic
     sweep on a quiet transcript.
  3. `ClaudeStatusBarController::refreshBgTasksButton` (moved
     from MainWindow by ANTS-1146) calls `m_bgTasks->sweepLiveness()`
     whenever the resolved transcript path is the same as the
     previous one (forcing the staleness check to re-run). The 2 s
     status timer drives `refreshBgTasksButton` directly on the
     controller (post-1146 connect target), so the sweep keeps
     firing while the transcript itself is silent.

  Without the sweep, the user sees stale running-counts that
  only clear on app restart or transcript-rotate; with it, the
  chip falls to 0 within ~60 s of the last task going idle.

## How to verify pre-fix code fails

```bash
git checkout fc1201c -- src/ CMakeLists.txt
cmake --build build --target test_claude_bg_tasks_button
ctest --test-dir build -R claude_bg_tasks_button
# Expect: every invariant fails — pre-0.7.38 source has no
# claudebgtasks.{cpp,h}, no button, no dialog.
```
