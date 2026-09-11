# window_close_confirm — ANTS-5120

Source-grep wiring test. Confirms that closing a whole window asks
before ending the programs running in its tabs, the way closing a
single tab already does.

## Invariants

INV labels are qualified by the roadmap item that added them.

| ANTS-5120 | Statement |
|----|-----------|
| 1  | `MainWindow::closeEvent` checks `m_config.confirmCloseWithProcesses()` and calls `firstNonShellDescendant`. On a hit it calls `event->ignore()` before it calls `saveAllSessions()` and before it calls `anotherWindowStaysOpen()`. |
| 2  | The running-program check in `closeEvent` covers every terminal in the window — it calls `liveTerminals()`, not just the active pane of the current tab. |
| 3  | `MainWindow::showCloseWindowConfirmDialog` is non-modal: its body calls no `exec(` and no `setModal(true)`, and it calls `show()`. |
| 4  | `showCloseWindowConfirmDialog`'s proceed path calls `close()` on the window (not only `dlg->close()`) and sets a `m_`-prefixed member flag to `true`; `closeEvent` reads that same flag. |
| 5  | `showCloseWindowConfirmDialog`'s "don't ask again" path calls `m_config.setConfirmCloseWithProcesses(false)` — the same setting `closeTab` uses. |

## Rationale

`MainWindow::closeTab` asks before tearing down a tab whose shell has
a running program (ANTS-1102, `confirmCloseWithProcesses`). Closing
the whole window ends every tab's programs too, but `closeEvent` never
asked. Found 2026-09-11 while fixing ANTS-5118. A user closing a window
that has `vim` or a long build running in one of its panes loses that
work with no warning, where closing just that one tab would have
warned them.

The fix reuses `firstNonShellDescendant` and the
`confirmCloseWithProcesses` setting `closeTab` already has. The
running-program probe covers every pane in the window
(`liveTerminals()`), not only the active pane of each tab — a
background split running `vim` must still trigger the question.

The dialog must stay non-modal, matching `showCloseTabConfirmDialog`'s
pattern. So `closeEvent` cannot simply block on it:
`event->ignore()` first, then the dialog; "Close anyway" sets a flag
`closeEvent` checks on the second call, so it does not ask again, and
calls `close()` to re-drive the shutdown.

## Scope

In scope: that `closeEvent` asks before ending running programs, that
the check covers every pane, that the dialog is non-modal, and that
"Close anyway" / "don't ask again" behave like the tab-close dialog's.

Out of scope: `ANTS-5118`'s own deferred-close and
`anotherWindowStaysOpen()` behaviour (covered by
`multi_window_session`'s `Ants5118Inv1`/`Ants5118Inv2`), and the
session-manager / KDE-logout interaction the roadmap item's
"Consequence to accept" names — nothing in this project's source talks
to a session manager, so there is nothing to grep for.

`showCloseWindowConfirmDialog` is this spec's own name for the new
helper — mainwindow.cpp defines no window-close dialog today. The fix
must define a function whose signature matches
`void MainWindow::showCloseWindowConfirmDialog(` for INV-3, INV-4 and
INV-5 to find it; a differently-named helper would need this spec and
test renamed together with it, not a change to what they check.

## Why source-grep, not behavioural

No test in this project constructs a `MainWindow` — its constructor
starts a shell and sets up the remote-control server, exactly the
problem `multi_window_session/spec.md`'s "Why source-grep" section
describes for the sibling `ANTS-5032`/`ANTS-5118` wiring facts. The
same reasoning applies here: these are wiring facts (a check happens
before a save, a dialog is non-modal, a flag is read where it is set),
not values a running instance would need to produce.
