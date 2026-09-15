# Feature spec: the terminal's exports report failure (ANTS-5078)

The terminal's context menu writes three kinds of file to a path the user
picks: the scrollback as text, the scrollback as HTML, and one command block
as an asciicast (Share Block, through `TerminalWidget::exportBlockAsCast`).

All three opened the target with `QFile` and `Truncate` and ignored every
write. A full disk or a failed write left a truncated file behind, and the
user was told nothing. `MainWindow`'s own Export Scrollback handler was fixed
the same way under ANTS-5079 (`mainwindow_command_safety` INV-2).

## Invariants

- **INV-1 — text export reports failure.** The context menu's "Export
  Scrollback as Text..." handler writes through `QSaveFile`, checks
  `commit()`, and emits `captureFailed` with a message when opening, writing
  or committing fails.
- **INV-2 — HTML export reports failure.** The same, for "Export Scrollback
  as HTML...".
- **INV-3 — a block cast checks its writes.** `exportBlockAsCast` writes
  through `QSaveFile`, never a truncating `QFile`, and returns false unless
  every write and the `commit()` succeed.
- **INV-4 — Share Block reports failure.** The "Share Block as .cast..."
  handler emits `captureFailed` when `exportBlockAsCast` returns false.

`captureFailed` is the signal `MainWindow::connectTerminal` already shows in
the status bar (ANTS-5151). Its meaning widens from "a capture file could not
be made private" to "a file the user asked for could not be written".

## Out of scope

- File permissions. A path the user picks keeps its default mode, as
  `session_capture_perms` decided.
- Moving the exports off the GUI thread (ANTS-5078's separate medium).

## Test scope

Source scrapes of `contextMenuEvent` and `exportBlockAsCast`: the handlers run
from a menu and a file dialog on a live `TerminalWidget`.
