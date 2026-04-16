# Feature: Review Changes button opens a visible dialog

## Contract

When the user clicks the "Review Changes" button on the status bar and
the git working tree differs from HEAD, a dialog showing the diff MUST:

1. **Become the active window.** After `showDiffViewer()` returns, the
   dialog must be visible AND raised on top of the main window AND be
   the input-focus target. This is enforced by explicit
   `dialog->raise()` + `dialog->activateWindow()` calls after
   `dialog->show()`.

2. **Not be stolen by a queued status-bar refocus.** The global
   `focusChanged` handler in `MainWindow`'s constructor queues a
   `QTimer::singleShot(0, terminal->setFocus())` whenever focus lands
   on status-bar chrome (so subsequent keystrokes reach the terminal).
   That queue was populated when the Review button took focus on mouse
   press. If the queued refocus fires *after* the dialog opens, it
   would steal focus from the dialog and — on KWin with a frameless
   parent (Qt::FramelessWindowHint, see mainwindow.cpp:74) — re-raise
   the main window over the dialog. The fire-time re-evaluation guard
   MUST skip the refocus if a QDialog other than the main window is
   the active window.

### Regression context

User report: *"The Review Changes button does nothing when I click it.
It has a mouseover event that indicates that it is active as it has
detected changes between local and git but the window does not come up
showing the differences when I click the button."*

Root cause: `showDiffViewer()` called `dialog->show()` only — no
`raise()` or `activateWindow()`. Sibling dialogs opened from menu
actions (AI, Claude Transcript) already called `raise()`; the Review
Changes button was the outlier. On a frameless QMainWindow parent,
KWin's window-stacking heuristics do not guarantee a newly-shown
transient dialog lands on top. Compounded by the focus-redirect
lambda at mainwindow.cpp:~411 which queued `terminal->setFocus()` at
mouse-press time; when that fired after dialog show, it pulled focus
from the dialog and re-activated the main window, obscuring the
dialog entirely.

See `0.6.29` `showDiffViewer`-silent-return fix in the regression
history (`.claude/agents/ants-status-bar.md`) — that addressed the
"no feedback on failure" half of the problem. This spec locks the
"visible dialog on success" half.

## Scope

### In scope
- Dialog raise/activate invariant after `dialog->show()`.
- Focus-redirect lambda's fire-time re-evaluation (skip refocus when
  a dialog is active).

### Out of scope
- Diff rendering correctness (colorization, +/- line detection).
- `git diff` command formulation (covered by the enablement probe
  agreeing with the viewer probe — see `showDiffViewer` comment
  block).
- Pre-click gating (`refreshReviewButton`) — separately tested.

## Rationale

A button whose click produces no visible effect is indistinguishable
from a disabled button. The pre-0.6.29 showDiffViewer had three
silent returns (no terminal / no cwd / non-zero git exit); 0.6.29
replaced those with `showStatusMessage` so the user sees *why* on
failure. But the *success* path — diff found, dialog created — still
failed silently when the dialog was created behind the main window.
Both halves are needed for the button to feel trustworthy.
