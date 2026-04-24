# Feature: `debug.log` lands 0600 on first open

## Problem

`DebugLog::setActive()` opens
`$XDG_DATA_HOME/ants-terminal/debug.log` in append mode via
`QFile::open`. The file inherits the process umask — typically 0022
on desktops, so the log lands at 0644 by default.

The log can contain material that should never be world-readable:

- **PTY data** — when the `pty` or `input` categories are enabled,
  keystrokes and command output (including passwords typed under
  no-echo) flow through log lines.
- **Network responses** — the `network` category logs AI-dialog
  request bodies and response bodies. Response bodies routinely
  include bearer tokens echoed back by misbehaving servers.
- **OSC 133 forgery HMAC material** — the `vt` and `signals`
  categories include digest inputs around shell-integration events.
- **Claude transcript tails** — the `claude` category logs the
  transcript-tail parse state, which includes message content.

Any other user on the host can read a 0644 `debug.log` if the
`$XDG_DATA_HOME/ants-terminal/` directory permits traversal.

## External anchors

- [CWE-732 — Incorrect Permission Assignment for Critical Resource](https://cwe.mitre.org/data/definitions/732.html):
  exact shape — intended owner-only access, actual file lands at
  process umask.
- [XDG Base Directory Specification](https://specifications.freedesktop.org/basedir-spec/latest/):
  `$XDG_DATA_HOME` is documented as user-scope but does not mandate
  perms on individual files; callers must enforce.
- Project convention: every other
  `$XDG_DATA_HOME/ants-terminal/` file
  (`sessions/*.bin`, `config.json`, `settings.local.json`,
  `.remotecontrol.sock`) already calls `setOwnerOnlyPerms`. The
  debug log was the last outlier.

## Contract

### Invariant 1 — open leaves the file at 0600 under umask 0022

After the first `DebugLog::setActive(<any non-zero mask>)` call,
`stat(debug.log).st_mode & 0777 == 0600`, even when the process
umask is 0022 at the moment of open.

### Invariant 2 — re-open preserves 0600

If `DebugLog::clear()` is called (which closes + removes the file)
and `setActive` is called again, the new file also lands 0600.

### Invariant 3 — pre-existing wider perms are narrowed

If a pre-existing `debug.log` from an older build (pre-fix) sits at
0644, the first `setActive` call on the new build narrows it to
0600 rather than appending to the wider file. This is why we call
`setOwnerOnlyPerms` on both the open `QFileDevice` and the path
string — path-level covers the inode state when append reuses the
existing file.

### Invariant 4 — `secureio.h` is the chosen helper

The fix uses the project-wide `setOwnerOnlyPerms` helper from
`src/secureio.h`, not an ad-hoc `QFile::setPermissions` call with a
literal bitmask. This is the audit-rule-enforced pattern
(`setPermissions_pair_no_helper`). Asserted via source-grep.

## How this test anchors to reality

The test:

1. Sets umask to 0022 (POSIX default on most distros — the
   worst-case public-readable default).
2. Points `DebugLog::logFilePath` at an isolated per-test directory
   via an `XDG_DATA_HOME` override (TempLocation-based UUID) so
   parallel runs don't collide.
3. Calls `DebugLog::setActive(DebugLog::Events)`.
4. `stat()`s the file and asserts mode == 0600.
5. Calls `DebugLog::clear()` + `setActive()` again and re-asserts
   0600.
6. For I3, creates a pre-existing 0644 file in the target directory
   before `setActive`, then verifies it's 0600 after open.
7. Source-greps `src/debuglog.cpp` for `#include "secureio.h"` and
   the `setOwnerOnlyPerms` call name to lock I4.

## Regression history

- **Introduced:** pre-0.7.0, when DebugLog first landed. The file
  has always been opened with default umask.
- **Flagged:** 2026-04-23 /indie-review cross-cutting theme (Tier 2
  hardening sweep: `debug.log` 0600 perms).
- **Fixed:** 0.7.20 — `setOwnerOnlyPerms` called on both the
  opened QFile and the path string immediately after successful
  open, before the session header is written.
