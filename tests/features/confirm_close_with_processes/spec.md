# confirm_close_with_processes — feature spec

**Theme:** ANTS-1102. Closing a tab whose shell has any non-shell
descendant process (vim, top, claude, tail -f, …) shows a
confirmation dialog before tearing the PTY down. Default on; the
dialog has a "Don't ask again" checkbox that flips
`Config::confirmCloseWithProcesses` to false for subsequent closes.

The fix lives at three layers:

1. **`Config`** — new bool key `confirm_close_with_processes`,
   default `true`, with idempotent setter (per the storeIfChanged
   pattern from the 0.7.51 config-loop fix).

2. **MainWindow** — `closeTab(int)` probes the active terminal's
   shell for non-shell descendants; if any are found and the
   config is on, routes to `showCloseTabConfirmDialog(QWidget *,
   QString)` instead of tearing down. Both confirm and silent
   paths land in `performTabClose(int)`.

3. **Settings UI** — new checkbox
   `m_confirmCloseWithProcesses` mirrors the existing
   `m_confirmMultilinePaste` shape (defaults, applySettings,
   restore-defaults).

The descendant probe is a free helper in mainwindow.cpp's
top-of-file anonymous namespace
(`firstNonShellDescendant(pid_t shellPid)`) walking
`/proc/<pid>/task/<pid>/children` transitively, capped at 256
visited pids, comparing each `comm` against `safeShellNames()`
(bash/zsh/fish/sh/ksh/dash/ash/tcsh/csh/mksh/yash). All in-process,
no QProcess.

The confirm dialog uses the Wayland-correct non-modal pattern
established in 0.7.50 (`new QDialog(this)` + `WA_DeleteOnClose` +
plain `QPushButton`s with `clicked → close`/`clicked → action`,
no `setModal`, no `QDialogButtonBox`).

## Invariants

- **INV-1**: `Config::confirmCloseWithProcesses` and
  `setConfirmCloseWithProcesses` exist with declarations in
  `config.h`. Source: ANTS-1102.
- **INV-2**: `Config::confirmCloseWithProcesses` defaults to
  `true` (matching `confirmMultilinePaste` precedent for
  destructive-action confirmations). Source: ANTS-1102.
- **INV-3**: `Config::setConfirmCloseWithProcesses` uses the
  `storeIfChanged` idempotency helper to avoid the
  inotify-loop class of bugs (see 0.7.51 `m_inConfigReload`
  fix). Source: regression hardening from ANTS-1023.
- **INV-4**: A free helper `firstNonShellDescendant(pid_t)` is
  defined in mainwindow.cpp's anonymous namespace and walks
  `/proc/<pid>/task/<pid>/children` transitively. Source:
  ANTS-1102.
- **INV-5**: A free helper `safeShellNames()` returns a
  `QSet<QString>` containing at minimum `bash`, `zsh`, `fish`,
  `sh`, `ksh`, `dash`. Source: ANTS-1102.
- **INV-6**: `MainWindow::closeTab` calls
  `firstNonShellDescendant` (via the helper) when
  `m_config.confirmCloseWithProcesses()` returns true, gated
  on `term && term->shellPid() > 0`. Source: ANTS-1102.
- **INV-7**: `MainWindow::closeTab` routes to
  `showCloseTabConfirmDialog(...)` instead of tearing the tab
  down when a descendant is found, and **does not** push a
  `ClosedTabInfo` onto `m_closedTabs` in that branch (the
  dialog's accept handler does, via `performTabClose`).
  Source: ANTS-1102 + ANTS-1101 interaction.
- **INV-8**: `performTabClose(int)` is the single teardown site
  — both the silent path and the confirm-and-proceed path land
  there. The undo-close-tab info push lives in
  `performTabClose`, not `closeTab`. Source: ANTS-1102 (refactor
  invariant — the user-visible undo behaviour must continue to
  work for confirmed closes).
- **INV-9**: The confirm dialog uses the Wayland-correct
  pattern: `new QDialog(this)` (heap), `WA_DeleteOnClose`,
  no `setModal`, plain `QPushButton`s wired to `clicked →`
  lambda/close. No `QDialogButtonBox`. Source: 0.7.50
  QTBUG-79126 lessons.
- **INV-10**: The "Don't ask again" checkbox calls
  `m_config.setConfirmCloseWithProcesses(false)` when checked
  before the close fires. Source: ANTS-1102.
- **INV-11**: `SettingsDialog` exposes the toggle as
  `m_confirmCloseWithProcesses` (a `QCheckBox*` member),
  wired to:
  - `loadFromConfig` reads `m_config->confirmCloseWithProcesses()`
    and sets the checkbox.
  - `applySettings` writes back via
    `m_config->setConfirmCloseWithProcesses(...)`.
  - Restore-defaults sets the checkbox back to `true`.
  Source: ANTS-1102 + the cancel/rollback contract from 0.7.32
  (settings_profile_cancel_rollback).

## Out of scope

- Restoring the *PTY itself* across close → reopen (long-running
  processes are still terminated; v1 is just the prevention
  layer per ANTS-1101).
- Configurable safe-shell list (the eleven-entry default is
  exhaustive enough for v1).
- Confirm-on-quit when multiple tabs have descendants (separate
  feature; not in this item).
