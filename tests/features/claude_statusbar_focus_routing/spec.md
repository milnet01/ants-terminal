# claude status-bar — prompt routing + model-chip focus (ANTS-1835/1840/1849)

Three GUI-lambda fixes in `src/claudestatuswidgets.cpp`. The wiring lives
in signal/slot lambdas that need a live `QStatusBar`, real `TerminalWidget`s
(each spawns a PTY) and a focused window to drive end-to-end, so these
invariants are locked by source-scrape. The behaviourally-testable arms of
ANTS-1840 (background-task `sweepLiveness` un-latch, task-id prose
tolerance) are covered by `claude_bg_tasks_button` / `claude_task_list`.

## Background

The hook server is a single UDS shared by every Claude across every tab.
`PermissionRequest` is deliberately left ungated at the integration layer
(`claude_status_bar_per_tab` I3) — the slot must route it per-tab. Before
ANTS-1835 the slot routed only the tab-bar glyph; it painted the bottom-bar
message + Allow/Deny buttons on the focused tab unconditionally, so a
background tab's prompt leaked onto whichever tab you were looking at.

The model-recommender chip sends `/model <tier>` to the focused terminal on
click. Before the fixes it left keyboard focus stranded on the button and
kept showing the (now-acted-on) recommendation until the next assistant
turn re-scored the transcript.

## Invariants

- **INV-1 (ANTS-1835)** — the `permissionRequested` slot resolves the
  owning shell from the hook `session_id` and computes a `belongsToFocused`
  predicate; the bottom-bar message (`emit statusMessageRequested("Claude
  permission: …")`) is emitted only inside the `if (belongsToFocused)`
  branch.
- **INV-2 (ANTS-1835)** — the tab-glyph marking
  (`markShellAwaitingInput(awaitingPid, true)`) is unconditional: it runs
  *before* the `belongsToFocused` gate, so a background tab's dot still
  lights even though its message/buttons are suppressed.
- **INV-3 (ANTS-1840)** — the model-chip click handler hides the chip
  (`m_modelBtn->hide()`) after dispatching `/model`, so the recommendation
  stops lingering once the user has acted on it.
- **INV-4 (ANTS-1849)** — the model-chip click handler returns focus to
  the terminal (`focused->setFocus()`) after `sendToPty`, so typing
  resumes in the terminal rather than staying on the button.

## Acceptance

`ctest -L features -R claude_statusbar_focus_routing` exits zero. Reverting
any of the four fixes (un-gating the message, gating the glyph, dropping
`hide()`, dropping `setFocus()`) fails the matching invariant.
