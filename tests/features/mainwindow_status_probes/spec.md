# Feature: status probes and session commands stay tied to their tab

## Invariants

**INV-1 — the resume command quotes the session id.** The
`ClaudeProjectsDialog::resumeSession` handler passes `shellQuote(sessionId)`.

**INV-2 — the socket reaper checks it is a socket.** The stale MCP socket
reaper removes a path only after `safeToUnlinkLocalSocket(full)`.

**INV-3 — the review probe is keyed to its cwd.** `refreshReviewButton` records
`m_reviewProbeCwd`, hides the button when a probe for another cwd is still
running, and applies a finished probe's result only while the active tab still
has that cwd.

**INV-4 — the review probe has a timeout.** `refreshReviewButton` kills a probe
still running after a fixed delay, so the in-flight flag cannot stay set.

## Rationale

The resume command put the session id in a shell line unquoted. The reaper's
comment promised an `S_ISSOCK` check that the code did not make. One in-flight
flag was shared by every tab, so a tab switch during a probe kept the button
and then showed the previous tab's git state; a probe that never exited left
the flag set for the session.

## Test surface

`test_mainwindow_status_probes.cpp` reads `src/mainwindow.cpp`
(`SRC_MAINWINDOW_CPP_PATH`) and checks the handler text. No `MainWindow` is
constructed.

## Regression history

- **ANTS-5080:** the four defects above. Locked by this spec.
