# Feature: allowlist `settings.local.json` perms survive commit

## Problem

`ClaudeAllowlistDialog::saveSettings()` writes
`~/.claude/settings.local.json` via `QSaveFile`, which:

1. Opens a sibling temp file `settings.local.json.XXXXXX`.
2. Writes the JSON payload to the temp fd.
3. Atomically renames it over the final path on `commit()`.

The pre-existing fix called `setOwnerOnlyPerms(file)` on the **temp
fd** before write. On most local filesystems, fd permissions carry
across rename and the final file is 0600 as intended. But rename is
not guaranteed to preserve perms everywhere:

- **FAT/exFAT** on removable media — no POSIX perm bits at all;
  the fd's perms are lost on rename and the final file lands with
  the mount's default (typically 0755).
- **SMB/NFS mounts** — server-side rename semantics vary; some
  servers apply umask to the destination filename regardless of
  the source fd's mode.
- **Qt fallback path** — on filesystems where `rename(2)` refuses
  (cross-device, exotic mounts), Qt's `QSaveFile::commit` falls
  back to copy + unlink, and copy creates the destination with the
  process umask, not the source fd's mode.

The file can hold Claude Code bearer tokens in the merged
`env`/`model`/custom-hooks keys. A 0644 final file on any of the
above filesystems leaks the token to every UID on the host.

Belt-and-suspenders: call `setOwnerOnlyPerms` on the **final path**
after `commit()` returns true, so the final file is 0600 regardless
of how `commit` got it there.

## External anchors

- [CWE-732](https://cwe.mitre.org/data/definitions/732.html) —
  Incorrect Permission Assignment for Critical Resource. Textbook
  shape: "intends 0600 but lands 0644 on rename fallback" is exactly
  the permission-assignment-drift pattern CWE-732 enumerates.
- [Qt docs — QSaveFile::commit](https://doc.qt.io/qt-6/qsavefile.html#commit)
  documents the copy+unlink fallback but does not specify what
  happens to file mode when the fallback fires. The safe
  assumption is "umask applies" — hence the post-commit chmod.
- [Anthropic Claude Code settings.json reference](https://docs.claude.com/en/docs/claude-code/settings)
  names the file we're hardening; the same file holds API keys in
  some configurations.

## Contract

### Invariant 1 — post-commit chmod applied to final path

After `saveSettings()` returns true, `QFile::permissions(path) ==
QFileDevice::ReadOwner | QFileDevice::WriteOwner` (0600). No
`ReadGroup`, `WriteGroup`, `ReadOther`, `WriteOther`, or any `Exec`
bit is set.

### Invariant 2 — pre-commit chmod retained

The fd-level `setOwnerOnlyPerms(file)` call BEFORE `file.commit()`
is also retained. This covers the window between `open` and
`commit` on filesystems that do preserve fd perms across rename —
the two calls are both needed (neither subsumes the other).

### Invariant 3 — commit failure does not chmod

If `file.commit()` returns false, `saveSettings()` must NOT call
`setOwnerOnlyPerms(m_settingsPath)`. The final file may not exist
at all, or may be a prior intact copy we mustn't clobber with a
chmod that hides a separate bug.

### Invariant 4 — first-run also lands 0600

On a fresh user account where `settings.local.json` does not exist
yet, saveSettings creates it. That first-run file must also land at
0600, not at the process umask default. This is the single most
common path for this code (every user's first allowlist edit), so
it has to be the default-safe case, not a retrofit.

## How this test anchors to reality

The test:

1. Constructs a `ClaudeAllowlistDialog`, redirects it via
   `setSettingsPath()` to an isolated path under
   `QStandardPaths::TempLocation`.
2. Pre-sets the process umask to 0022 (the POSIX default) so that
   if perms are not explicitly set, the file lands 0644. A test
   run under umask 0077 would pass by accident.
3. Calls `saveSettings()` (I1 + I4 path).
4. Asserts the final file exists and has exactly 0600 perms.
5. Runs saveSettings again to exercise the parse-happy-path
   (file exists, parses) and re-asserts 0600 (I1 repeat).
6. Source-greps `src/claudeallowlist.cpp` for both calls (pre-fd
   and post-path) to lock I2.

If any invariant drifts — someone removes the post-commit call,
someone replaces it with a widened perm mask, someone refactors
saveSettings without re-threading the chmod — this test fires.

## Regression history

- **Introduced:** 0.7.3 when `ClaudeAllowlistDialog::saveSettings`
  landed with the pre-commit perm set only. Latent on ext4/xfs/btrfs
  (fd perms survive rename) but exposed on FAT-mounted USB drives
  and some SMB home directories.
- **Flagged:** 2026-04-23 /indie-review — Config/IPC reviewer
  noted the pre-commit-only pattern as "works on ext4, breaks on
  anything else."
- **Fixed:** 0.7.17 — post-commit `setOwnerOnlyPerms` added on
  successful commit. The pre-commit call is retained for the
  open-before-commit window.
