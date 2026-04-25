# Feature: `secureio.h` is perms-only; `configbackup.h` owns rotation + lock

## Problem

`src/secureio.h` previously straddled two concerns:

1. **Owner-only perms** (`setOwnerOnlyPerms`) — the original purpose,
   covering 12 call sites that all need 0600 on persistence files.
2. **Corrupt-file rotation** (`rotateCorruptFileAside`) — added in
   0.7.12 when /indie-review flagged the silent-data-loss pattern
   in Config + ClaudeAllowlist + SessionManager. Same shape, same
   helper, but a fundamentally different concern (parse-failure
   recovery, not file-mode setting).

Adding a third helper for cooperative inter-process locks
(`ConfigWriteLock`, 0.7.31) would push the file past the
"single-purpose header" line. Splitting before the third helper
lands keeps each header readable and lets call sites pull only what
they need.

## Layout (post-split)

| File | Contents | Audience |
|---|---|---|
| `src/secureio.h` | `setOwnerOnlyPerms(QFileDevice&)`, `setOwnerOnlyPerms(const QString&)` | Every persistence site (~12 callers). |
| `src/configbackup.h` | `rotateCorruptFileAside(const QString&)`, `class ConfigWriteLock` | The four sites that read-modify-write a shared config file (Config, ClaudeAllowlist, SettingsDialog, future `claudeintegration` plan-mode prefs). |

## Contract

### Invariant 1 — `secureio.h` is perms-only

`src/secureio.h` MUST NOT define `rotateCorruptFileAside` or
`ConfigWriteLock`. The file's responsibility is the 0600 bitmask
helpers. A regression that re-merges the helpers (e.g. someone
moves rotation back into secureio.h to "shorten the include list")
violates this.

### Invariant 2 — `configbackup.h` is the new home for rotation + lock

`src/configbackup.h` MUST define `rotateCorruptFileAside` (inline
free function) and `class ConfigWriteLock`. The class MUST be
non-copyable (`= delete`'d copy ctor + assignment) and MUST own
the `flock(2)` fd lifecycle in its destructor.

### Invariant 3 — every caller of rotation includes configbackup.h

The four call sites that use `rotateCorruptFileAside` —
`config.cpp`, `claudeallowlist.cpp`, `settingsdialog.cpp`,
`auditdialog.cpp` (if it adopts the helper later) — must include
`configbackup.h`. They MAY also include `secureio.h` for the perms
helper, but MUST not rely on the rotation helper transitively
through `secureio.h`.

### Invariant 4 — `setOwnerOnlyPerms` callers continue to use `secureio.h`

The 12 call sites currently including `secureio.h` for
`setOwnerOnlyPerms` keep that include intact. Removing the include
from a site that still calls `setOwnerOnlyPerms` would compile-
fail; the test asserts the inclusions stay.

## How this test anchors to reality

Source-grep:

1. `src/secureio.h` — must contain `setOwnerOnlyPerms` and must
   NOT contain `rotateCorruptFileAside` or `class ConfigWriteLock`.
2. `src/configbackup.h` — must contain `rotateCorruptFileAside`
   and `class ConfigWriteLock` and must declare both inline /
   header-only.
3. `src/config.cpp`, `src/claudeallowlist.cpp`,
   `src/settingsdialog.cpp` — each must `#include
   "configbackup.h"`.
4. The non-copyable invariant on ConfigWriteLock — the file must
   contain `ConfigWriteLock(const ConfigWriteLock &) = delete`
   and the assignment-operator delete.

## Regression history

- **Introduced:** 0.7.12 (rotateCorruptFileAside landed in
  secureio.h).
- **Split:** 0.7.31 — Persistence integrity (cross-file) bundle.
  The trigger was adding `ConfigWriteLock`, which is even less
  about "perms" than `rotateCorruptFileAside`.
