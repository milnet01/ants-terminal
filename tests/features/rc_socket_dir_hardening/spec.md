# Feature: rc-socket fallback dir hardening

Canonical design: `docs/specs/ANTS-1365.md`. This file is the
test-side restatement.

## Problem

`RemoteControl::defaultSocketPath` (`remotecontrol.cpp:60-73`) falls
back to `/tmp/ants-terminal-<uid>.sock` when `XDG_RUNTIME_DIR` is
unset. `/tmp` is world-writable. A same-UID rogue process can
pre-create the path as a regular file or symlink, blocking Ants
from binding the socket on startup — rc/MCP silently disables for
the session. DoS by path-squat.

Fix: wrap the fallback in a per-user 0700 subdir
(`/tmp/ants-<uid>/`) created via atomic `::mkdir(0700)` with
post-create `::lstat` verification (S_ISDIR + st_uid + mode bits).
Composes with the existing file-level `safeToUnlinkLocalSocket`
guard for defense-in-depth.

## External anchors

- `safeToUnlinkLocalSocket` at `secureio.h:72-82` — sibling helper
  using the same `::lstat → stat-mode-checks → reject` pattern.
- ANTS-1132 — origin of the 0700 + SO_PEERCRED + S_ISSOCK trust
  posture; this spec extends the same posture to the *containing
  directory*.

## Invariants (full list in `docs/specs/ANTS-1365.md`)

- **INV-2** Subdir is created with 0700 atomically (`::mkdir(0700)`,
  not two-step `mkpath` + `chmod`).
- **INV-3** Pre-bind `::lstat` verification rejects symlink, wrong
  owner, wrong mode.
- **INV-4** Idempotent — preexisting valid dir is accepted without
  recreation.
- **INV-5** Inherited symlink rejected even if it points to a valid
  0700 dir (S_ISLNK is false for symlinks under lstat).
- **INV-10** Same-UID rogue cannot pre-create or symlink-swap the
  socket's containing directory undetected.

## What the C++ test pins

SD-1..SD-4 exercise `ensureSocketDir` directly (no Qt event loop,
no MainWindow). WI-1..WI-3 grep the source for wiring.
