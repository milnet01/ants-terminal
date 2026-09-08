# Feature: session log and session recording are owner-only

## Problem

Two capture files are created automatically in the application's own
data directory, with a path the user never chooses:

- `TerminalWidget::setSessionLogging` opens
  `$XDG_DATA_HOME/ants-terminal/logs/session_<stamp>.log` and writes
  every raw byte received from the PTY.
- `TerminalWidget::startRecording` opens
  `$XDG_DATA_HOME/ants-terminal/recordings/recording_<stamp>.cast`
  and writes the same stream as asciicast v2 events. `MainWindow`'s
  **Record Session** action is its only caller and composes that path
  itself.

Both are the terminal's full output stream. That includes anything a
command printed — API keys echoed by `env`, tokens in a curl trace, a
private key `cat`ed by mistake — and anything pasted into the shell.

Neither called for owner-only permissions. `QFile::open` creates at the
process umask, typically 0022 on a desktop, so both files landed 0644
and were readable by any other local user whose path traversal reached
them. Both containing directories were created with `QDir::mkpath`,
which is also umask-derived, so traversal was permitted.

This is the same defect `debuglog_perms` fixed for `debug.log`, in the
same data directory, on files whose contents are strictly larger.

## External anchors

- [CWE-732 — Incorrect Permission Assignment for Critical Resource](https://cwe.mitre.org/data/definitions/732.html):
  exact shape — intended owner-only access, actual file at umask.
- `src/secureio.h` is the project's answer to this class:
  `ensurePrivateDir` creates a directory at 0700 with no
  create-then-chmod window (ANTS-1821), and `setOwnerOnlyPerms` applies
  0600 to a file. Its own header comment names session blobs and
  scrollback as the data it was written for.

## Contract

Both capture paths MUST be private to the invoking user:

1. The containing directory is created through `ensurePrivateDir`, so
   it is born 0700 rather than created at umask and tightened after.
2. A failure to secure the directory aborts the capture. `secureio.h`
   says a caller must surface a false return rather than swallow it,
   and writing the stream into a directory that could not be secured
   is the outcome this feature exists to prevent.
3. The opened file is narrowed with `setOwnerOnlyPerms`, so the
   capture is owner-only even if the directory is later loosened by
   hand.

## Invariants

**INV-1 — the session-log directory is created privately.**
Source-grep `TerminalWidget::setSessionLogging`: it must call
`ensurePrivateDir` and must NOT call `QDir().mkpath`.

**INV-2 — a session-log directory that cannot be secured stops the
log.** The `ensurePrivateDir` result in `setSessionLogging` is tested,
not discarded.

**INV-3 — the session log file is owner-only.**
`setSessionLogging` calls `setOwnerOnlyPerms` on the opened file.

**INV-4 — the recording file is owner-only.**
`TerminalWidget::startRecording` calls `setOwnerOnlyPerms` on the
opened file. It is checked on the widget rather than on the caller so
that any future caller inherits it.

**INV-5 — the recordings directory is created privately.**
Source-grep `src/mainwindow.cpp`: the Record Session handler composes
the recordings path and must create it through `ensurePrivateDir`, with
the result tested.

## Scope

### In scope
- Source-grep regression test over `src/terminalwidget.cpp` and
  `src/mainwindow.cpp`.

### Out of scope
- A runtime `stat` check of the kind `debuglog_perms` runs. That test
  can call a free function on a static class; these two paths need a
  constructed `TerminalWidget` — which brings up a PTY and a grid —
  and, for INV-5, a `MainWindow`. The unit harness has neither. This is
  the same reason `paste_dialog_custom` and
  `scratchpad_submit_ordering` grep this file.
- The three export actions that write through
  `QFileDialog::getSaveFileName` — export scrollback as text, as HTML,
  and `exportBlockAsCast`. The user picks those paths deliberately and
  may well intend to share the file. Forcing 0600 on a path the user
  chose is a different decision from securing one they never saw, and
  is not made here.
- Retrofitting permissions onto capture files already on disk from an
  earlier build. They keep whatever mode they were created with.

## Regression history

- **`debuglog_perms` (ANTS-1190 era):** fixed the same defect for
  `debug.log` and recorded that it was "the last outlier" among
  `$XDG_DATA_HOME/ants-terminal/` files. That was not true: the
  session log and the session recording were never enumerated. The
  claim is corrected in that spec by the same change that adds this
  one.
- **ANTS-4456 (cold sweep 2026-08-18, verified 2026-09-08):** reported
  as "session log and asciicast created without owner-only
  permissions". Verified against source and fixed. Locked by this spec.
