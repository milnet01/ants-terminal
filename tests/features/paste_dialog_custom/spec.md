# Paste-confirmation dialog — async pattern invariants

## Problem

Under a frameless + translucent MainWindow on KWin/Wayland, every
*blocking* shape of the paste-confirmation dialog swallowed mouse
clicks on the button box — only the `&Paste` Alt-mnemonic key path
worked. Three attempts on the 0.7.4 release day all regressed to the
same user-reported symptom:

1. **`QMessageBox::exec()`** (original). Sets
   `Qt::WA_ShowModal` / `Qt::ApplicationModal` internally; every
   mouse click on the button box was dropped.
2. **Custom `QDialog` + `QDialog::exec()`.** Same root cause —
   `QDialog::exec()` sets the same modal attributes as
   `QMessageBox::exec()`.
3. **Custom `QDialog` + `QDialog::show()` + local `QEventLoop`.**
   Sidesteps `WA_ShowModal` but still blocks the caller on a nested
   event loop from the same call stack that dispatched the paste
   gesture; reproduced the identical click-swallow.

Pattern: every shape where the pasteToTerminal() call does not
*return* before the user decides hits the regression. The only shape
that works is the one `mainwindow.cpp::showDiffViewer` arrived at in
0.6.29 for the same reason — fully async.

## Fix (0.7.4 third attempt, final)

Replicate the Review-Changes dialog pattern exactly:

- Heap-allocated `QDialog` with `Qt::WA_DeleteOnClose` (same-day
  lifetime; self-destructs on close).
- Two explicit `QPushButton`s in an `QHBoxLayout` — Cancel is
  `setDefault(true)`, Paste is `setAutoDefault(false)`.
- Cancel's `clicked()` wired to `QDialog::close`.
- **Paste's `clicked()` wired to a lambda** that calls
  `performPaste(payload)` and closes the dialog. The lambda captures
  the byte array by value and carries a `QPointer<TerminalWidget>`
  guard so closing the tab between show and accept is a no-op, not
  a use-after-free.
- `pasteToTerminal()` returns synchronously once the dialog is
  shown — the paste happens later in the accept callback, or never
  if the user cancels.
- `show() + raise() + activateWindow() + setFocus(cancelBtn)`
  activation dance identical to `showDiffViewer`.

## Invariants

Source-grep — reproducing the click-swallow needs a live KWin +
Wayland + translucent-parent stack, which CI doesn't have. These
pin the implementation contract.

- **INV-1:** `pasteToTerminal()` constructs `new QDialog(this)` on
  the heap (not a stack `QDialog`).
- **INV-2:** `Qt::WA_DeleteOnClose` is set on the dialog.
- **INV-3:** Cancel has `setDefault(true)`, Paste has
  `setAutoDefault(false)` so Enter cannot dangerous-accept.
- **INV-4:** The Paste button's `clicked()` signal is wired to a
  lambda that calls `performPaste()`.
- **INV-5:** `dlg->show() + dlg->raise() + dlg->activateWindow()`
  appear in that order.
- **INV-6:** Explicit `cancelBtn->setFocus(...)` after activation.
- **INV-7 (neg):** `dlg->exec()` does not appear, and `QEventLoop`
  is not instantiated anywhere in the `pasteToTerminal()` body. Both
  are regression attractors.
- **INV-8:** A `QPointer<TerminalWidget>` guard is captured into
  the Paste-clicked lambda.
- **INV-9:** `TerminalWidget::performPaste(const QByteArray&)`
  exists as a separate helper handling the actual PTY write and
  bracketed-paste wrapping.
- **INV-10 (neg):** The old synchronous
  `bool TerminalWidget::confirmDangerousPaste(...)` definition is
  removed from the `.cpp` so it can't be accidentally revived.
