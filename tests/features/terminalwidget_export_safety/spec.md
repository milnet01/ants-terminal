# Feature spec: the terminal's exports report failure (ANTS-5078)

The terminal's context menu writes three kinds of file to a path the user
picks: the scrollback as text, the scrollback as HTML, and one command block
as an asciicast (Share Block).

All three opened the target with `QFile` and `Truncate` and ignored every
write. A full disk or a failed write left a truncated file behind, and the
user was told nothing. `MainWindow`'s own Export Scrollback handler was fixed
the same way under ANTS-5079 (`mainwindow_command_safety` INV-2).

All three now run through `ScrollbackExporter`
([`docs/specs/ANTS-5078-export-streaming.md`](../../../docs/specs/ANTS-5078-export-streaming.md)).
That test, `scrollback_export_streaming`, checks that each action calls
`TerminalWidget::startExport` and that a failed export emits `captureFailed`.

## Invariants

- **INV-1 — the exporter replaces the file only on success.**
  `ScrollbackExporter` writes through `QSaveFile`, never a truncating `QFile`,
  and calls `commit()` only after every write has returned its full length.
- **INV-2 — a short write fails the export.** Each write compares the bytes
  written with the bytes given, and a mismatch fails the export.
- **INV-3 — Share Block reports a missing block.** The "Share Block as
  .cast..." handler emits `captureFailed` when the block is gone.

`captureFailed` is the signal `MainWindow::connectTerminal` already shows in
the status bar (ANTS-5151). Its meaning widens from "a capture file could not
be made private" to "a file the user asked for could not be written".

## Out of scope

- File permissions. A path the user picks keeps its default mode, as
  `session_capture_perms` decided.

## Test scope

Source scrapes of `ScrollbackExporter` and `contextMenuEvent`. A write failure
needs a full disk, which the unit harness cannot arrange.
