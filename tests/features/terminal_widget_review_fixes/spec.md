# Terminal widget: fixes from the 2026-11-06 reviews

ANTS-6654. Small fixes to `TerminalWidget` (and one in `MainWindow`), each
too small for a feature folder of its own. INV-1 to INV-3 build a real
widget; the rest read the source, because the code they lock needs a live
PTY, a launched process or a modal dialog.

## Invariants

- **INV-1 — every failed command is reported.** Two commands that each end
  with OSC 133 `D;1`, in two output batches, emit `commandFailed` twice.
  It fires from the grid's command-finished callback, under the same
  budget as `commandFinished`.
- **INV-2 — a bookmark moves with its line.** With scrollback at its cap, a
  bookmarked line pushed further up keeps its bookmark: the stored global
  line is the one that holds the bookmarked text.
- **INV-3 — a rich copy keeps combining marks.** The HTML half of
  `copySelectionRich` holds `e` followed by U+0301 when the cells do.
- **INV-4 — the send preview wraps long lines.** `showSendConfirmation`
  shows the payload in a read-only `QPlainTextEdit` that wraps, and no
  longer cuts a line at 117 characters.
- **INV-5 — an editor launch suits xdg-open and reports failure.**
  `openFileAtPath` drops `--` for `xdg-open` and emits `captureFailed` when
  `QProcess::startDetached` returns false.
- **INV-6 — a recording with no header is not made.** `startRecording`
  checks the header write and the flush; on failure it removes the file and
  emits `captureFailed` before the recording starts.
- **INV-7 — click-to-move counts characters.** `clickToMoveCursor` skips
  wide-character continuation cells when it counts arrows.
- **INV-8 — a plain-text file:// link asks first.** `openHyperlink` asks
  for confirmation on a non-OSC 8 `isLocalFile()` URL before
  `QDesktopServices::openUrl`.
- **INV-9 — reloading settings keeps per-style fonts.**
  `MainWindow::reapplyConfig` does not call `setFontFamily` after
  `applyConfigToTerminal`, which sets the bold and italic families.

## Regression history

- **ANTS-6654:** each invariant above was a finding of the 2026-11-06
  reviews (`docs/reviews/close-findings-2026-11-06.md`).
