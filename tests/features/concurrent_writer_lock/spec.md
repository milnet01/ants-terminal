# Feature: ConfigWriteLock serializes concurrent persistent-config writers

## Problem

Three save sites converge on two shared paths:

- `~/.config/ants-terminal/config.json` written by `Config::save`.
- `~/.claude/settings.json` written by both
  `ClaudeAllowlistDialog::saveSettings` and
  `SettingsDialog::installClaudeHooks` /
  `installClaudeGitContextHook`.

When two Ants processes (a Quake instance + a regular window, or
two file-manager double-clicks) save the same file at the same time:

1. Both open `<path>.tmp` for write — the second `open()` truncates
   the first writer's bytes.
2. Both write — bytes interleave on the same inode.
3. Both rename — `rename(2)` is atomic, so the last-rename-wins
   determines the final inode, and one of the writers' data is
   silently dropped.

For `~/.claude/settings.json` the conflict is sharper still: the
Allowlist dialog writes a `permissions` block and the
Install-hooks button writes a `hooks` block. Both perform a
read-modify-write, so the second writer's read sees stale bytes
(missing the first writer's update), and the merge silently
clobbers the first writer's keys.

`ConfigWriteLock` (header `src/configbackup.h`) is an RAII guard
around an advisory POSIX `flock(2)` on a sibling `<path>.lock`
file. Cooperating callers serialize on the lock; non-cooperating
callers (vim, jq, sed) bypass it by design (we don't want to
freeze the user's text editor).

## External anchors

- [POSIX flock(2)](https://man7.org/linux/man-pages/man2/flock.2.html)
  documents the advisory-lock semantics: cooperating processes
  serialize, non-cooperating ones bypass.
- [Linux Programming Interface §53.3.1](https://man7.org/tlpi/)
  walks through `flock` vs `fcntl` advisory locks; flock is
  whole-fd, fcntl is byte-range. The whole-file granularity is
  what we want here — we're locking the path, not a region.
- [SQLite WAL design notes](https://www.sqlite.org/wal.html#concurrency)
  describe the same RMW conflict shape (one writer at a time on a
  shared file). Our config files are too small to justify WAL;
  a coarse flock is the right cost.

## Contract

### Invariant 1 — first ConfigWriteLock acquires; second blocks then times out

A second `ConfigWriteLock` constructed on the same path while the
first is still in scope must NOT acquire within the 5-second
deadline. `acquired()` returns false. The first lock destructor
releases; a fresh ConfigWriteLock after the first goes out of
scope must succeed.

### Invariant 2 — single-process consecutive locks both acquire

Constructing one ConfigWriteLock, letting it destruct, then
constructing another, both must succeed. `flock(2)` releases
properly on close — a stale-lock regression would manifest here.

### Invariant 3 — Config::save acquires the lock before write

`src/config.cpp` `Config::save()` body must contain
`ConfigWriteLock writeLock(path)` and an `acquired()` check, with
an early return if the lock could not be acquired. The lock must
be in scope for the write+rename window — i.e. declared before the
`QFile file(tmpPath)` block, so the destructor releases AFTER the
rename completes.

### Invariant 4 — ClaudeAllowlistDialog::saveSettings acquires the lock

`src/claudeallowlist.cpp` `saveSettings` body must contain
`ConfigWriteLock writeLock(m_settingsPath)` and an `acquired()`
guard.

### Invariant 5 — SettingsDialog hook installers acquire the lock

`src/settingsdialog.cpp` `installClaudeHooks` and
`installClaudeGitContextHook` must each construct a
`ConfigWriteLock` on `settingsPath` before the QSaveFile commit.

## How this test anchors to reality

Runtime portion:

1. Build a path in `QStandardPaths::TempLocation`.
2. Construct a `ConfigWriteLock` A — assert `A.acquired() == true`.
3. In a nested scope, construct ConfigWriteLock B on the same path.
4. To make the test fast, fork a child process (instead of waiting
   the full 5-second timeout in-process). The child constructs a
   ConfigWriteLock and reports `acquired()`. Parent asserts the
   child reported false. (Threads share fds → flock semantics
   collapse; processes are the only honest test.)
5. Destruct A. Construct C in the same path — assert C.acquired().

Source-grep portion: I3, I4, I5 above.

## Regression history

- **Introduced:** Latent in every Ants version since
  `Config::save` shipped (0.5.x). Two simultaneously-running Ants
  instances were unusual until session persistence + Quake mode +
  the multi-tab restore landed; once those existed, two windows
  on the same `config.json` became a routine pattern.
- **Bundled in:** 0.7.31 — Persistence integrity (cross-file)
  bundle.
