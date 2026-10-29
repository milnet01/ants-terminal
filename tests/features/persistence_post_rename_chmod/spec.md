# Feature: persistence sites post-rename-chmod the final inode

## Problem

Three persistence sites in 0.7.31 share a temp-file-then-rename
shape:

- `Config::save()` — `~/.config/ants-terminal/config.json` (may hold
  `ai_api_key`).
- `SessionManager::saveSession` / `saveTabOrder` —
  `~/.local/share/ants-terminal/session_*.dat` + `tab_order.txt`
  (scrollback content can include passwords mistyped at the prompt,
  ssh history, paste buffers).
- `SettingsDialog::installClaudeHooks` /
  `installClaudeGitContextHook` — `~/.claude/settings.json` (may
  hold Claude Code bearer tokens, env keys with secrets).

All three set 0600 perms on the **temp fd** before write. On most
local filesystems (ext4/xfs/btrfs) the perms carry across rename
and the final file lands 0600 as intended. But:

- **FAT/exFAT** on removable media — no POSIX bits at all; the
  fd's mode is lost on rename, final mode is the mount default
  (typically 0755).
- **SMB/NFS mounts** — server-side rename semantics vary; some
  servers apply server umask to the destination filename
  regardless of the source fd's mode.
- **Qt copy+unlink fallback** — when `rename(2)` refuses
  (cross-device, exotic mounts), Qt falls back to copy+unlink, and
  copy creates the destination with the **process umask**, not
  the source fd's mode.

Belt-and-suspenders: call `setOwnerOnlyPerms(<final-path>)`
**after** the atomic rename / `commit()` succeeds. Idempotent on
ext4/xfs/btrfs (already 0600); essential elsewhere.

This is the same pattern previously locked for
`ClaudeAllowlistDialog::saveSettings()` by
`tests/features/allowlist_perms_postcommit/`. 0.7.31 extends
coverage to the four other persistence sites that were still
relying on fd-only chmod.

## External anchors

- [CWE-732](https://cwe.mitre.org/data/definitions/732.html) —
  Incorrect Permission Assignment for Critical Resource.
- [Qt docs — QSaveFile::commit](https://doc.qt.io/qt-6/qsavefile.html#commit)
  documents the copy+unlink fallback but not the resulting mode.
- POSIX `rename(2)` permits the implementation to copy+unlink for
  cross-device renames; ext4 + tmpfs + FAT mixes this in user
  homes.

## Contract

### Invariant 1 — the shared writer chmods the final path after rename succeeds

`Config::save()`, `SessionManager::saveSession` and `saveTabOrder`
write through `writeFileAtomicOwnerOnly` (`src/secureio.h`). It must
call `setOwnerOnlyPerms(path)` only on the success branch of the
`std::rename` return-code check. The chmod must NOT run when rename
failed (the failure branch removes the orphan tmp; chmod-ing the
destination would chmod whatever prior file is sitting there).

### Invariant 2 — the config and session writers use it

`src/config.cpp` calls `writeFileAtomicOwnerOnly` once;
`src/sessionmanager.cpp` calls it twice (`writeBlob`, `saveTabOrder`).

### Invariant 3 — the Claude hook installers chmod after commit

`src/claudesetup.cpp` `writeClaudeSettings` must call
`setOwnerOnlyPerms(settingsPath)` after `settingsOut.commit()`
returns true. Both hook installers write `~/.claude/settings.json`
through it.

### Invariant 4 — pre-write fd chmod retained at every site

The fd-level `setOwnerOnlyPerms(file)` (in the shared writer) and
`setOwnerOnlyPerms(settingsOut)` remain. The two calls cover different windows:

- Pre-write fd chmod = covers the open→close→rename window on
  filesystems that DO preserve fd perms across rename. Closes the
  brief world-readable hole between `open(O_WRONLY|O_CREAT, 0666)`
  and the explicit chmod.
- Post-rename path chmod = covers the rename→subsequent-read window
  on filesystems that DO NOT preserve fd perms across rename.

Neither subsumes the other. If a refactor drops one, the spec is
violated.

## How this test anchors to reality

Source-greps:

1. `src/secureio.h` — `writeFileAtomicOwnerOnly` contains
   `setOwnerOnlyPerms(file)`, then `std::rename`, then `if (rc != 0)`,
   then `setOwnerOnlyPerms(path)` (Invariants 1 and 4).
2. `src/config.cpp` and `src/sessionmanager.cpp` — the calls to it
   (Invariant 2).
3. `src/claudesetup.cpp` — `setOwnerOnlyPerms(settingsPath)` after
   `settingsOut.commit()`, plus the fd chmod (Invariants 3 and 4).

A simpler runtime test would also work for Config but the other
two require a real Qt event loop + mocked save-settings dialog;
source-grep is the lowest-cost anchor for what is mechanically a
"don't delete this line" invariant.

## Regression history

- **Introduced:** `ClaudeAllowlistDialog::saveSettings` shipped the
  pattern in 0.7.17 (post-commit chmod) — it was added to ONE
  callsite. The other four kept relying on fd-only chmod until
  the 0.7.31 sweep extended coverage uniformly.
- **Bundled in:** 0.7.31 — Persistence integrity (cross-file)
  bundle, alongside the secureio.h split and the concurrent-writer
  guard.
