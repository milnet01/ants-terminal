# Feature: `shell_command` config actually launches the user's shell

## Problem

Settings → General has a Shell picker (Default / bash / zsh / fish /
Custom…) that calls `Config::setShellCommand(…)` on Apply. The value
persists to `~/.config/ants-terminal/config.json` under
`"shell_command"` and round-trips through `Config::shellCommand()` on
reload. The Settings-dialog load path reads it and re-populates the
combobox correctly.

**Pre-fix bug:** nothing in the PTY-launch path consumed the value.
`TerminalWidget::startShell(const QString &workDir)` took only a
cwd; `VtStream::onStart` invoked `Pty::start(QString(), workDir, …)`
with a hardcoded empty shell arg, so `Pty::start` always fell back
to `$SHELL` / `getpwuid_r()->pw_shell` / `/bin/bash`. The user's
shell choice was silently dropped on every tab open.

Symptom: user picks "zsh" in Settings → Apply → restart or open a
new tab → bash still launches. No error, no log, no indication that
the setting is being ignored.

## External anchors

- [POSIX `execlp(3)`](https://pubs.opengroup.org/onlinepubs/9699919799/functions/execlp.html) —
  resolves a bare shell name (`zsh`) against `$PATH`. Both bare
  names and absolute paths work identically in `Pty::start`; the
  fix doesn't need to add path lookup logic.
- [Qt `QMetaObject::invokeMethod` with `Q_ARG`](https://doc.qt.io/qt-6/qmetaobject.html#invokeMethod-5) —
  the invocation path was already threading a `QString` shell
  argument to `VtStream::start`; only the caller was force-feeding
  it `QString()`. The fix changes the caller, not the plumbing.

## Contract

### Invariant 1 — `TerminalWidget::startShell` signature carries a `shell` parameter

`src/terminalwidget.h` declares
`bool startShell(const QString &workDir = QString(),
                 const QString &shell = QString());`.
The default is empty (matches pre-fix behaviour — `Pty::start`
falls back to `$SHELL` etc.) so callers that don't care about the
shell (e.g. future test helpers) don't have to thread the config.

### Invariant 2 — `TerminalWidget::startShell` forwards the `shell` arg to `VtStream::start`

The `QMetaObject::invokeMethod(m_vtStream, "start", …, Q_ARG(QString, shell), …)`
call passes the method's `shell` parameter, NOT `QString()`. A
refactor that drops the parameter back to empty re-opens the bug.

### Invariant 3 — All four production call sites in `MainWindow` pass `m_config.shellCommand()`

`MainWindow::newTab`, `MainWindow::newTabForRemote`,
`MainWindow::splitCurrentPane`, and the `restoredTabs` loop in
`MainWindow::restoreSessions` (or equivalent session-restore path)
all pass `m_config.shellCommand()` as the `shell` argument. The
consumer callsites are identified by source-grep for
`startShell(…m_config.shellCommand())`.

### Invariant 4 — `Config::shellCommand` round-trip is stable

`Config::setShellCommand("zsh")` followed by serialization and a
fresh load returns `"zsh"`. Guards against someone inadvertently
dropping the serialization case in config.cpp's load/save helpers.

## How this test anchors to reality

The test is source-grep + in-memory round-trip — no PTY fork
(would require a real shell binary in CI) and no widget harness
(would require QT_QPA_PLATFORM=offscreen + a parent window, not
worth the complexity for a four-call-site wiring test).

Steps:

1. **I1:** open `src/terminalwidget.h`, assert the `startShell`
   declaration has two `const QString &` parameters.
2. **I2:** open `src/terminalwidget.cpp`, find the
   `QMetaObject::invokeMethod(m_vtStream, "start"` block, assert
   the `Q_ARG(QString, …)` for the shell slot is `shell`, not
   `QString()`.
3. **I3:** open `src/mainwindow.cpp`, count occurrences of
   `startShell(…m_config.shellCommand(…)…)` — must be ≥ 4.
   Also require zero occurrences of `startShell(` that lack the
   `shellCommand` token, excluding the comment block (canonicalise
   by stripping `//` lines before the grep).
4. **I4:** construct a `Config` (sandboxed via `XDG_CONFIG_HOME`
   in a temp dir), call `setShellCommand("zsh")`, save, reload a
   fresh `Config`, read `shellCommand()`. Must equal `"zsh"`.

## Regression history

- **Introduced:** original implementation of the Shell picker
  (pre-0.5 era). The UI + config plumbing was built; the runtime
  consumer was never wired.
- **Discovered:** 2026-04-24 documentation-debt sweep. Search for
  "dead config keys" flagged `shellCommand()` as having zero call
  sites outside `settingsdialog.cpp::loadSettings` — which is the
  READ side of the round-trip, not a consumer.
- **Fixed:** 0.7.18 (unreleased at time of writing). Added
  `shell` parameter to `TerminalWidget::startShell`, threaded it
  through the `QMetaObject::invokeMethod(m_vtStream, "start", …)`
  call, and wired `m_config.shellCommand()` at all four call
  sites in `MainWindow`.
