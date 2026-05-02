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
| 10 | wiring | Widget label format is `<unfinished>/<total>`. |
| 11 | wiring | Dialog renders one row per task in parser-emitted order. |
| 12 | wiring | Dialog rebuilds on `tasksChanged()`. |
| 13 | wiring | Dialog source has neither `setModal(true)` nor `QDialogButtonBox`. |

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
