# claude_task_list_session_isolation — ANTS-1219

> **Source:** user 2026-05-10 — *"dialog shows entries from prior
> compacted sessions ('Phase 2 — Add test_vt bundle…',
> 'Phase 2 — Update tests/features/README.md',
> 'Identify chrome-bundle test candidates…') that are no longer
> active work."*
>
> **Reproduction state (2026-05-10).** The original framing was
> "stale tasks after `/compact`". Live testing in Claude Code
> v2.1.138 against session `94218f91-…` showed that `/compact`
> compacts **in place** — same `session_id`, same JSONL file,
> the compaction merely appends a single
> `"isCompactSummary":true` event and continues. The resolver
> stayed on the correct file; the tracker stayed correct. The
> remaining session-id-transition surfaces in current Claude
> Code are: launch (new `session_id` per process), re-launch via
> "Continue previous coding session", and `claude --resume <id>`.
> None directly reproduced as of this spec.
>
> **Spec disposition.** Lock in the existing wiring contract so
> that (a) future drift is caught by a regression test, and
> (b) the next live repro (whenever it arrives) localises to a
> specific INV violation rather than a vague symptom. Updates
> the ROADMAP entry's root-cause hypothesis: the tracker does
> NOT retain tasks across a `setTranscriptPath` swap — it
> replaces atomically (`claudetasklist.cpp:124`). The earlier
> hypothesis was wrong-shaped.

## 1. Contract

At every refresh tick (`m_statusTimer` cadence 2 s, set in
`mainwindow.cpp:698`), the Task List dialog and the Tasks chip
MUST display **exactly** the tasks that
`parseTranscript(activeSessionPath(focusedTabCwd))` would
return at that instant.

The resolver (`activeSessionPath`) is owned by
`claude_session_freshness` (ANTS-1163-INV-1..13). The parser
(`parseTranscript`) is owned by `claude_task_list`
(ANTS-1158-INV-1..6 + 1218/1221 follow-ons). This spec asserts
the **wiring** between them: the path returned by the resolver
becomes the tracker's source path within one tick, and the
tracker's emitted state strictly equals the parse of that path.

## 2. Invariants

INV labels qualified `ANTS-1219-INV-N`. All citations are
against current `src/` code. This spec asserts **wiring**;
the tracker's internal contract (idempotence on same path,
atomic replace, emit-iff-changed) is owned by `claude_task_list`
(ANTS-1158-INV-7/INV-8) and is referenced here, not duplicated.

| #  | Lane | Statement |
|----|------|-----------|
| 1  | wiring | `ClaudeStatusBarController::refreshTasksButton()` calls `m_integration->activeSessionPath(cwd)` (`claudestatuswidgets.cpp:679`), where `cwd` is the focused-tab terminal's `shellCwd()` (`:671`). It calls `m_tasks->setTranscriptPath(path)` iff `path != m_tasks->transcriptPath()` (`:680-682`). |
| 2  | wiring | The `m_statusTimer->setInterval(2000)` connection from `mainwindow.cpp:730-731` ensures `refreshTasksButton` fires at most 2 s after a resolver-result change. INV-1's path-change branch therefore propagates to the tracker within one tick of the resolver swapping paths. |
| 3  | wiring | When `activeSessionPath` returns `""` (no live Claude / freshness floor rejected all candidates per ANTS-1163-INV-7), INV-1 propagates an empty path to the tracker via `setTranscriptPath("")`; the parser-side ANTS-1158-INV-7 then clears `m_tasks`; the chip's hide branch (`total <= 0 || unfinished <= 0` at `claudestatuswidgets.cpp:724`) fires. |
| 4  | wiring | `ClaudeStatusBarController::resetForTabSwitch()` (`claudestatuswidgets.cpp:502-516`) calls `m_tasks->setTranscriptPath(QString())` synchronously on every tab switch — invoked from `MainWindow`'s tab-change handler at `mainwindow.cpp:3987`. The next `refreshTasksButton` tick re-binds the new focused tab's path via INV-1. |
| 5  | wiring | The `m_tasks` `tasksChanged` → `refreshTasksButton` connection (`claudestatuswidgets.cpp:117-118`) creates a feedback loop: a parser-driven content change (file appended on disk → `QFileSystemWatcher::fileChanged` → `rescan` → `tasksChanged` from ANTS-1158) re-runs `refreshTasksButton`, refreshing the chip text. Default `Qt::AutoConnection`, same thread → direct delivery, no event-loop gap. |
| 6  | poll-rescue | `refreshTasksButton` calls `m_tasks->poll()` unconditionally on every tick (`claudestatuswidgets.cpp:690`). `poll()` mtime-checks `m_transcriptPath` and rescans only on change. This is the rescue path for `QFileSystemWatcher` silently dropping its watch on Claude's atomic-rewrite (`tmpfile + rename`). Bg-tasks side has the structural twin in `ClaudeBgTaskTracker::sweepLiveness`. |

## 3. Negative invariants (what MUST NOT happen)

These are the user-visible failure modes this spec rules out. Each
one decomposes into INV violations elsewhere in the contract.

| #   | Statement | Decomposes to |
|-----|-----------|---------------|
| N1  | A JSONL at a path other than `m_tasks->transcriptPath()` MUST NOT contribute tasks to the visible list. | `rescan()` calls `parseTranscript(m_transcriptPath)` as its sole parse entry (`claudetasklist.cpp:97-100`); no other site reads task content. |
| N2  | Across a resolver-result swap (old path → new path), the chip / dialog MUST NOT briefly show old tasks under the new path. | ANTS-1219-INV-1 (push happens on path change) + `setTranscriptPath` calls `rescan()` synchronously inside the same function (`claudetasklist.cpp:54`) — no event-loop gap between setting `m_transcriptPath` and replacing `m_tasks`. |
| N3  | The chip MUST NOT remain visible when the focused tab has no live Claude session. | ANTS-1219-INV-3 + the existing `total <= 0 \|\| unfinished <= 0` hide branch at `claudestatuswidgets.cpp:705`. |

## 4. Out of scope

- **Resolver correctness.** Whether `activeSessionPath` returns the *right* path is owned by `claude_session_freshness` (ANTS-1163). This spec asserts the tracker faithfully follows whatever the resolver returns, not that the resolver chose well.
- **Parser correctness.** Whether `parseTranscript` extracts the right task entries from a given JSONL is owned by `claude_task_list` (ANTS-1158). This spec does not exercise the parser; it asserts only the wiring around it.
- **Background-tasks chip** (ANTS-1053). The same wiring shape applies with `ClaudeBgTaskTracker` substituted (`claudestatuswidgets.cpp:573-645`). A sibling spec — `claude_bg_tasks_session_isolation` — would mirror this one. Bundling deferred until this spec lands.
- **`/resume <id>` re-opening the same historical JSONL.** No transition occurs (resolver returns the same path it would resolve cold). This spec's swap invariants are vacuously satisfied.
- **`isCompactSummary` checkpoint semantics.** Once the ANTS-1224 fix lands, `parseTranscript` will treat `isCompactSummary:true` events as state-reset checkpoints so post-`/compact` + relaunch (via `claude --resume` or "Continue previous coding session") doesn't carry pre-compact tasks into the visible state. Owned by `claude_task_list` (`ANTS-1224-INV-1..3`, recorded in `tests/features/claude_task_list/spec.md` under that spec's "Follow-on INVs" table — the `1224-` prefix scopes them to this fix bundle while keeping them in the parent spec's contract surface). The original draft of *this* spec wrongly listed `isCompactSummary` as "filtered out" — that was a misread of `ANTS-1158-INV-5`/`INV-6`, which only filter sidechain and subagent-dispatch events. The checkpoint contract is downstream of the wiring asserted here; the wiring tests do not re-instantiate it.
- **Per-tab Claude PID resolution under multi-tab Claude.** `ClaudeIntegration::m_claudePid` is singleton-scoped; if two tabs each run Claude, `processStartTimeMs(m_claudePid)` inside `activeSessionPath` uses whichever PID was last detected. That's an ANTS-1161/1168 boundary concern, not this spec's surface.

## 5. Acceptance

`ctest -L features -R claude_task_list_session_isolation` exits zero.

This spec's invariants are wiring-shape claims (where signals connect,
which call sites push paths to which trackers, what the timer cadence
is). They're locked in via **source-grep** — strings the build will
fail to find if a refactor breaks the wiring. The parser-side
behaviour exercised by ANTS-1158-INV-7/INV-8 is already covered by
`claude_task_list/`, so this spec deliberately does not re-instantiate
`ClaudeTaskListTracker` against fixture JSONLs.

Test shape (source-grep, no GUI, no QTemporaryDir):

1. **INV-1 — refresh wiring**: grep `src/claudestatuswidgets.cpp` for `m_integration->activeSessionPath(cwd)` AND `if (path != prevPath)` AND `m_tasks->setTranscriptPath(path)` — all three within `refreshTasksButton`.
2. **INV-2 — timer cadence**: grep `src/mainwindow.cpp` for `m_statusTimer->setInterval(2000)` AND the `refreshTasksButton` connect line. Failing either string would mean a resolver-swap could go un-propagated for longer than the contract allows.
3. **INV-3 — empty-resolver propagation**: covered by INV-1's grep (the same `setTranscriptPath(path)` push handles empty `path` identically) plus the existing chip hide-branch grep at `claudestatuswidgets.cpp:724` for `unfinished <= 0`. ANTS-1158-INV-7 owns the parser-side empty handling.
4. **INV-4 — tab-switch reset**: grep `src/claudestatuswidgets.cpp` for `m_tasks->setTranscriptPath(QString())` inside `resetForTabSwitch`, plus grep `src/mainwindow.cpp` for the call site `m_claudeStatusBarController->resetForTabSwitch()` to confirm it is invoked from a tab-change handler.
5. **INV-5 — feedback connect**: grep `src/claudestatuswidgets.cpp` for `connect(m_tasks, &ClaudeTaskListTracker::tasksChanged` AND `&ClaudeStatusBarController::refreshTasksButton` on adjacent lines.
6. **INV-6 — poll-rescue call**: grep `src/claudestatuswidgets.cpp` for `m_tasks->poll()` inside `refreshTasksButton`.

INV labels embedded as `// ANTS-1219-INV-N` comments next to each asserted string in source so the grep is intentional, not coincidental — same pattern `claude_session_freshness` uses for INV-9/INV-10.

The parser-side checkpoint contract (`ANTS-1224-INV-1..3`) is NOT exercised here — that surface is owned by `tests/features/claude_task_list/` (link-based parser tests against fixture JSONLs). A pass through this spec's six steps is necessary but not sufficient for ANTS-1219 closure; the parser-side test must also pass.

Test slot: `tests/features/claude_task_list_session_isolation/test_claude_task_list_session_isolation.cpp`. Wired into the existing `test_claude` bundle per ANTS-1217 — no new standalone executable, no new CMake target visible at top-level.

## 6. Memory budget

No new fields, caches, or buffers. Existing memory accounting:

- `m_tasks` (the tracker's task list) is bounded by either a single `TodoWrite` snapshot OR the union of `TaskCreate` IDs in the current transcript. Both bounds owned by `claude_task_list` spec.
- The `QFileSystemWatcher` holds at most one path. Removed-and-re-added on every swap (no leak).
- Test is source-grep only (no fixtures, no `QTemporaryDir`, no instantiation) — zero runtime memory cost.
- Spec lifecycle is "lock current behaviour"; no growth.

## 7. How to verify pre-fix code fails

The pre-fix code IS the current code — this is a regression-locking spec, not a reproduce-and-fix item. To confirm each grep would catch a real regression:

- **INV-1:** delete `if (path != prevPath)` at `claudestatuswidgets.cpp:666`, or replace `m_tasks->setTranscriptPath(path)` with a no-op. Acceptance step 1 must turn red.
- **INV-2:** change `m_statusTimer->setInterval(2000)` at `mainwindow.cpp:698` to a larger value, or remove the `refreshTasksButton` connect at `:724-726`. Acceptance step 2 must turn red.
- **INV-4:** remove `m_tasks->setTranscriptPath(QString())` from `resetForTabSwitch` at `claudestatuswidgets.cpp:506`. Acceptance step 4 must turn red.
- **INV-5:** delete the `connect(m_tasks, ...tasksChanged, ...refreshTasksButton)` at `claudestatuswidgets.cpp:111-112`. Acceptance step 5 must turn red.
- **INV-6:** remove `m_tasks->poll()` at `claudestatuswidgets.cpp:674`. Acceptance step 6 must turn red.

Parser-side regressions (atomic-replace at `claudetasklist.cpp:124`, idempotence early-return at `:48`, empty-path clear) are caught by `claude_task_list/` (ANTS-1158-INV-7/INV-8), not here.

Restore each before committing.

## 8. Re-open conditions

If the user reports the symptom again after this spec lands, the failure mode is one of:

1. **Resolver pointing at the wrong file** → ANTS-1163 boundary breach. Add INV to `claude_session_freshness`, not here.
2. **Tracker not receiving the swap within one tick** → this spec's INV-1 / INV-2 violation. Capture with the existing `tasks/refresh:` debug log line (`claudestatuswidgets.cpp:687`); it already prints `prev-changed=yes/no` per tick.
3. **Parser pulling cross-session content from one JSONL** → ANTS-1158 boundary breach. Add INV there.
4. **A new transition surface** (e.g., a future Claude Code version that introduces in-place /resume, or a hot-reload mode) — extend this spec's §4 "Out of scope" to "In scope" and add the corresponding INV.

The existing diagnostic log lines in `refreshTasksButton` (`tasks/refresh:`) and `refreshBgTasksButton` (`bgtasks/refresh:`) already carry `prev-changed`, `path` basename, `total`, `unfinished`, and the hide-branch reason — sufficient to localise any of (1)-(3) without further instrumentation.
