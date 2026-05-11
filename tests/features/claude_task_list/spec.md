# claude_task_list — ANTS-1158

Hybrid feature test: links `claudetasklist.cpp` for parser
behaviour; source-greps `claudetasklistdialog.cpp`,
`claudestatuswidgets.{cpp,h}`, and `mainwindow.cpp` for wiring.

## INV map

INV labels qualified `ANTS-1158-INV-N`. Full statements in
`docs/specs/ANTS-1158.md` §9.

| #  | Lane | Statement |
|----|------|-----------|
| 1  | parser | TodoWrite at transcript tail produces N tasks matching `input.todos`. |
| 2  | parser | Most-recent TodoWrite wins. |
| 3  | parser | TaskCreate + paired tool_result → entry with `status="pending"`, ID extracted. |
| 4  | parser | TaskUpdate flips status on a known taskId; no-op on unknown. |
| 5  | parser | `isSidechain == true` events skipped. |
| 6  | parser | `Task` tool_use with `subagent_type` filtered out. |
| 7  | parser | `setTranscriptPath("")` clears state and emits `tasksChanged()` once. |
| 8  | parser | Re-set with same path is idempotent (no second emit). |
| 9  | wiring | Status-bar widget is hidden on empty list, shown when ≥ 1 task. |
| 10 | wiring | Widget label is a `%1/%2` format (post-ANTS-1218 the args are `total - unfinished` / `total`; INV is intentionally permissive — see ANTS-1218-INV-1 for the tight check). |
| 11 | wiring | Dialog renders one row per task in parser-emitted order. |
| 12 | wiring | Dialog rebuilds on `tasksChanged()`. |
| 13 | wiring | Dialog source has neither `setModal(true)` nor `QDialogButtonBox`. |

### Follow-on INVs (post-ANTS-1158 contract refinements)

**Status:** 1218-INV-1/2 ✅ shipped 0.7.80; 1221-INV-1/2 ✅ shipped 0.7.81; 1224-INV-1/2/3 ✅ shipped 0.7.82 (ANTS-1219+1224+1225 bundle, 2026-05-11).

| #            | Lane   | Statement |
|--------------|--------|-----------|
| 1221-INV-1   | parser | `unfinishedCount()` counts `pending` only — `in_progress` is excluded. (Pre-fix counted `pending + in_progress`; user report 2026-05-10.) |
| 1221-INV-2   | parser | A list of all-`in_progress` tasks yields `unfinishedCount() == 0` so the chip's existing `unfinished <= 0` hide branch fires. |
| 1218-INV-1   | wiring | `m_tasksBtn` setText numerator is `total - unfinished` (chip counts up; X/Y reads as completed/total like every other progress display in the app). |
| 1218-INV-2   | parser | Walking through pending → in_progress → completed transitions, the chip's displayed numerator (`total - unfinished`) is non-decreasing. Locks in monotone progress display. |
| 1224-INV-1   | parser | A top-level `isCompactSummary == true` event is a state-reset checkpoint — `parseTranscript` MUST clear `out`, `idxByToolUseId`, and `sawTodoWrite` upon encountering it, then `continue` to the next line. The checkpoint event itself does NOT contribute task entries (the reset-then-continue happens before the `type==assistant` / `type==user` dispatch, so its content is never inspected for `tool_use` / `tool_result` blocks). The state at end-of-file therefore reflects only TodoWrite/TaskCreate/TaskUpdate events strictly *after* the last such event. (User report 2026-05-10: post-`/compact` + `/exit` + `claude --resume`, the chip showed pre-compact tasks because the parser ignored the checkpoint.) |
| 1224-INV-2   | parser | The INV-1 reset is applied for *each* `isCompactSummary` event encountered during the file walk. If N checkpoints occur in one transcript, the final state reflects only the events after checkpoint N — multi-compaction sessions converge to "state after the final checkpoint" by induction over INV-1. |
| 1224-INV-3   | parser | Sidechain compact-summary events (theoretical: `isSidechain==true` AND `isCompactSummary==true`) are filtered by the existing INV-5 sidechain skip *before* the checkpoint reset would fire — i.e., the implementation MUST place the `isCompactSummary` check *after* the sidechain filter. Defensive ordering against a hypothetical future Claude Code version that emits such events; no observed instances in current Claude Code. |

**Memory note (1224 follow-on):** the three clears (`out.clear()`, `idxByToolUseId.clear()`, `sawTodoWrite = false`) are intra-call within `parseTranscript` — no persistent state added. The cleared containers are exactly the ones the parser already owns; the INV-9 envelope of the spec is unchanged.

INV-1 through INV-8 are link-based: the test instantiates a
`ClaudeTaskListTracker`, points it at a temp-file JSONL fixture
(written inline), and asserts on `tracker->tasks()`.

INV-9 through INV-13 are source-grep against the wiring sites.

## Why the split

Parser behaviour is the contract — it must work against real
JSONL byte sequences, not just have the right strings in the
source. Source-grep on parser code would let a future refactor
silently break a Mode-A / Mode-B branch and leave the test
green.

Wiring behaviour (button shown / dialog opened / signal
connected) is shape-driven — Qt itself enforces the runtime
behaviour once the connect lands. Source-grep is sufficient
and cheaper than instantiating a MainWindow.
