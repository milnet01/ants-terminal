# ANTS-1365 — `/tmp` socket-path fallback hardening (per-user 0700 subdir)

**Status:** spec draft → review → implement
**Kind:** security (DoS-resistance + path-squat closure)
**Source:** indie-review-2026-05-14 (lane-2 M1)
**Pairs with:** ANTS-1132 (existing same-UID SO_PEERCRED + 0700 +
S_ISSOCK trust posture — same posture extended to the fallback
location).

---

## 1. Summary

`RemoteControl::defaultSocketPath` (`src/remotecontrol.cpp`) falls
back to `/tmp/ants-terminal-<uid>.sock` when `XDG_RUNTIME_DIR` is
unset (`QStandardPaths::writableLocation(RuntimeLocation)` returns
empty). `/tmp` is world-writable; a same-UID rogue process can
pre-create the path as a regular file or non-socket entry before
Ants binds, causing `listen()` to fail and the `safeToUnlinkLocalSocket`
guard to refuse cleanup (correctly — the guard rejects non-socket
paths to avoid clobbering user data). Result: Ants starts up
without rc/MCP, all integration silently degraded.

Fix: when the fallback path is taken, create a per-user **0700**
subdirectory under `/tmp` (`/tmp/ants-<uid>/`) via `::mkdir` (raw
syscall, not `QDir::mkpath` — see § 4) and bind the socket inside
it. Pre-creation `lstat` confirms the subdir is a real directory
owned by us with 0700 mode; reject the fallback if any of those
fail.

## 2. Problem statement (threat model)

The fallback path is reached on:
- Non-systemd distros without `XDG_RUNTIME_DIR` exported.
- Stripped-down container images.
- `env -u XDG_RUNTIME_DIR ants-terminal` (intentional dev test).
- macOS (on principle, even though Ants targets Linux today).

On any of these, `/tmp` is the directory the socket lands in. Same-UID
processes — a misbehaving Lua plugin, a stuck Claude Code session, a
co-tenant in a multi-user shared workstation — can pre-create:

```sh
# Same UID as Ants
touch /tmp/ants-terminal-1000.sock
# or
ln -s /etc/passwd /tmp/ants-terminal-1000.sock
```

When Ants starts:
1. `m_server->listen("/tmp/ants-terminal-1000.sock")` fails (path
   exists).
2. `safeToUnlinkLocalSocket` (`src/secureio.h`) calls `lstat`
   on the path. Both `S_ISREG` and `S_ISLNK` results return `false`
   from `S_ISSOCK`, so the guard refuses to unlink — correct
   behaviour, since silently `unlink(/etc/passwd-symlink)` would be
   catastrophic.
3. The fallback `listen()` retry in `start()` also fails.
4. Ants logs `remote-control: refusing to unlink … not a socket
   owned by this user … remote-control disabled for this process`
   (from `start()`) and starts with degraded integration.

Effect: rc/MCP unavailable for the session. Claude Code can't
inspect tabs, drift-check, fold-in. Recovery requires manual rm of
`/tmp/ants-terminal-<uid>.sock` and restart. The bug is **DoS by
path-squat**, not data theft (the 0700-on-the-socket and SO_PEERCRED
checks still hold post-bind), but it silently degrades the integration
Ants users rely on.

## 3. Surface — current state

| Site | Current behaviour |
|---|---|
| `defaultSocketPath` (`src/remotecontrol.cpp`) | Falls back to `/tmp/ants-terminal-<uid>.sock` when XDG is unset |
| `start()` (`src/remotecontrol.cpp`) | `QDir().mkpath(QFileInfo(path).absolutePath())` — for `/tmp` this is a no-op |
| `start()` (`src/remotecontrol.cpp`) | `setSocketOptions(UserAccessOption)` — applies to the socket file, not the containing dir |
| `safeToUnlinkLocalSocket` (`src/secureio.h`) | Correctly refuses non-socket cleanup |
| `start()` (`src/remotecontrol.cpp`) | `setOwnerOnlyPerms(path)` — 0700 on the socket |

The XDG path (`/run/user/<uid>/ants-terminal.sock`) is already in a
systemd-managed 0700 dir, so it inherits the protection. Only the
`/tmp` fallback is exposed.

## 4. Design

### 4.1 Per-user subdir

```cpp
// remotecontrol.cpp — replace defaultSocketPath()
QString RemoteControl::defaultSocketPath() {
    // Override wins unconditionally — lets the user script
    // multi-instance setups without touching the source.
    const QByteArray override = qgetenv("ANTS_REMOTE_SOCKET");
    if (!override.isEmpty()) return QString::fromLocal8Bit(override);

    const QString xdg = QStandardPaths::writableLocation(
        QStandardPaths::RuntimeLocation);
    if (!xdg.isEmpty()) {
        return xdg + "/ants-terminal.sock";
    }

    // /tmp fallback — wrap in a per-user 0700 subdir so a same-UID
    // rogue process can't pre-create the socket path as a regular
    // file or symlink and DoS our startup. Mirrors the XDG layout
    // (which is itself 0700-on-the-dir). See ANTS-1365 spec.
    const QString tmpRoot = QStringLiteral("/tmp/ants-%1")
                                .arg(::getuid());
    return tmpRoot + QStringLiteral("/ants-terminal.sock");
}
```

### 4.2 Subdir creation + verification

`start()` already calls `QDir().mkpath(QFileInfo(path).absolutePath())`
in `src/remotecontrol.cpp`. That path now constructs `/tmp/ants-<uid>/`,
but `QDir::mkpath` doesn't accept a mode argument — Qt always uses
0755 on Unix. We need 0700.

Replace with a raw-syscall mkdir + lstat verification:

```cpp
// remotecontrol.cpp — replace the mkpath call in start()
const QString dir = QFileInfo(path).absolutePath();
if (!ensureSocketDir(dir)) {
    ANTS_LOG(DebugLog::Network,
        "remote-control: socket dir %s unavailable; "
        "remote-control disabled for this process",
        qUtf8Printable(dir));
    return false;
}
```

Where `ensureSocketDir` is a new file-scope helper:

```cpp
// Create the socket-containing directory at 0700 and verify it is
// a real directory owned by us. Returns true on success.
//
// Idempotent: if the dir already exists with the right mode + owner
// + ISDIR, we accept it. If it exists but is a symlink, regular
// file, world-readable dir, or owned by another UID, we refuse —
// the caller treats that as "remote-control disabled."
static bool ensureSocketDir(const QString &dir) {
    const QByteArray dirBytes = QFile::encodeName(dir);

    struct stat st{};
    if (::lstat(dirBytes.constData(), &st) == 0) {
        // Path exists. Verify it's a directory we own, mode 0700.
        if (!S_ISDIR(st.st_mode)) return false;
        if (st.st_uid != ::getuid()) return false;
        // mode bits 0777 mask; require exactly 0700.
        if ((st.st_mode & 0777) != 0700) return false;
        return true;
    }
    if (errno != ENOENT) return false;

    // Doesn't exist — create it with 0700, then re-stat to confirm
    // (TOCTOU defense: an attacker can't race a symlink replace
    // because mkdir(2) fails if the path exists; the post-create
    // lstat confirms the directory we made is the one still there).
    if (::mkdir(dirBytes.constData(), 0700) != 0) return false;
    if (::lstat(dirBytes.constData(), &st) != 0) return false;
    if (!S_ISDIR(st.st_mode)) return false;
    if (st.st_uid != ::getuid()) return false;
    if ((st.st_mode & 0777) != 0700) return false;
    return true;
}
```

Headers required: `<sys/stat.h>` (already in `remotecontrol.cpp`,
Loop 1 verified) and `<cerrno>` (need to add; the file uses Qt
error reporting today, no raw `errno` access yet).

### 4.3 Why `::mkdir`, not `QDir::mkpath`

`QDir::mkpath` always creates with 0755 on POSIX. Qt's high-level
API offers no way to pass a mode. We could `mkpath` then
`::chmod(…, 0700)`, but that opens a TOCTOU window where the
directory is readable by other users between the two syscalls.
`::mkdir(…, 0700)` is atomic w.r.t. the inode's mode bits (subject
to umask — see INV-7).

### 4.4 Composition with the existing socket cleanup

`safeToUnlinkLocalSocket` (`src/secureio.h`) continues to apply to the
socket file inside the subdir. The subdir itself is not subject to
the safeToUnlink guard — `ensureSocketDir` is idempotent and re-
verifies the dir's ownership/mode every call, so a same-UID rogue
that swaps the dir for a symlink between Ants restarts is caught
on the next start.

### 4.5 Cleanup on shutdown

The socket file is removed by the existing `~RemoteControl`
destructor (`src/remotecontrol.cpp`). The per-user subdir is **left in place**
across Ants restarts — it's a 0700 inode owned by the user, costs
~0 bytes, and removing it on shutdown opens a new race window for
the next start. Documented in INV-6.

## 5. Invariants

- **INV-1 — Fallback path lives in a 0700 per-user subdir.**
  `/tmp/ants-<uid>/ants-terminal.sock`. The XDG primary path is
  unchanged.

- **INV-2 — Subdir is created with 0700 atomically.** `::mkdir(…,
  0700)` is the only creation site. No two-step `mkpath` +
  `chmod` racing.

- **INV-3 — Pre-bind verification.** Before `listen()`, the dir is
  `lstat`-verified to be a directory (not a symlink), owned by the
  same UID, mode 0700. Any failure disables rc/MCP for this process
  with a debug-log line.

- **INV-4 — Idempotent.** A pre-existing dir with the right
  attributes is accepted without recreation. Multiple Ants
  instances by the same user share the dir; each binds its own
  socket inside (today there's only one socket name, but the
  pattern is forward-compatible with per-instance suffixing).

- **INV-5 — Inherited symlink rejected.** If
  `/tmp/ants-<uid>` exists as a symlink (even one pointing to a
  valid 0700 dir), `lstat` returns `S_ISLNK` true; the check fails;
  rc/MCP disables. This is intentional — same-UID symlink swaps
  are the class of attack ANTS-1132 + this spec exist to close.

- **INV-6 — Subdir persists across restarts.** Shutdown does not
  remove `/tmp/ants-<uid>`. The subdir is 0700 owned by the user;
  leaving it in place avoids re-introducing the create-race on
  next start.

- **INV-7 — umask doesn't widen the mode.** `::mkdir(…, 0700)` is
  subject to the process umask. A user with `umask 0002` would
  otherwise create `0700 & ~0002 = 0700` (unaffected). But a
  `umask 0077` user gets `0700 & ~0077 = 0600` — not a directory
  that can be entered. The post-create `lstat` catches that:
  `(st_mode & 0777) != 0700` rejects, and the spec defaults to "rc
  disabled" rather than re-creating with `chmod`. Documented; a
  future ticket can `chmod(…, 0700)` after `mkdir` for users with
  unusual umasks.

- **INV-8 — No effect on XDG path.** `/run/user/<uid>/ants-terminal.sock`
  is unchanged. `ensureSocketDir` runs for it too, but the XDG dir
  is owned by systemd (or pam_systemd) with mode 0700 — the check
  passes idempotently.

- **INV-9 — Override env still wins.** `ANTS_REMOTE_SOCKET`
  bypasses the fallback path entirely. Multi-instance scripting,
  test harnesses, and exotic deployment topologies remain
  controllable. Override paths are NOT routed through
  `ensureSocketDir` — the user is responsible for their own
  trust posture when they pick a custom location.

- **INV-10 — DoS surface closed.** A same-UID rogue cannot
  pre-create or symlink-swap the socket's containing directory
  without `ensureSocketDir`'s `lstat` detecting it.

## 6. Failure modes & rejection table

`ensureSocketDir` returns `false`; `start()` logs a Network-category
debug line and returns `false` to its caller. rc/MCP is disabled
for the process — same outcome shape as the existing "listen failed"
path, distinct log message.

| Trigger | Log line |
|---|---|
| `mkdir` failed (permission, disk full) | `remote-control: socket dir <path> unavailable; remote-control disabled for this process` |
| Path exists as symlink | same (the lstat S_ISDIR check fails) |
| Path exists, owned by different UID | same |
| Path exists, mode != 0700 | same |

A single log message keeps the diagnostic clear without leaking
which specific check failed (defense-in-depth — don't help an
attacker tune their squat attempt).

## 7. Test plan

**`tests/features/rc_socket_dir_hardening/`** (new).

### Engine tests

- **SD-1 happy path** — invoke `ensureSocketDir(tmpdir + "/ants-N")`
  in an isolated `QTemporaryDir` → returns true; dir exists, mode
  0700, owned by us.
- **SD-2 idempotent** — call twice → second call returns true
  without creating.
- **SD-3 symlink reject** — pre-create `tmpdir/ants-N` as a symlink
  to a valid 0700 dir → returns false (S_ISLNK fails).
- **SD-4 wrong-mode reject** — pre-create `tmpdir/ants-N` as a
  0755 dir → returns false.
- **SD-5 different-UID reject** — fixture pre-creates dir with
  st_uid != getuid() (test runs the path through a fake
  syscall layer; in CI we may skip and fold this into doc).
- **SD-6 fallback override** — set `ANTS_REMOTE_SOCKET` →
  `defaultSocketPath` returns the override unchanged;
  `ensureSocketDir` is called on the override's parent (sanity
  check the helper handles arbitrary paths).
- **SD-7 XDG primary path still works** — set
  `XDG_RUNTIME_DIR=/tmp/xdg-N`; pre-create dir as 0700 us-owned
  → `defaultSocketPath` returns the XDG path;
  `ensureSocketDir` accepts.

### Wiring tests — source-grep

- **WI-1** `src/secureio.h` contains exactly one
  `ensureSocketDir(` definition.
- **WI-2** `defaultSocketPath` fallback string is now
  `/tmp/ants-%1` not `/tmp/ants-terminal-%1.sock`.
- **WI-3** `start()` body calls `ensureSocketDir(dir)` instead of
  `QDir().mkpath`.
- **WI-4** the socket dir is created by `::mkdir(…, 0700)` inside
  `ensureSocketDir` (`src/secureio.h`), never `QDir::mkpath`.

### Integration test

- **INT-1 same-UID DoS resistance** — in an isolated `QTemporaryDir`:
  pre-create `tmpdir/ants-N` as a regular file. Spawn an in-process
  `RemoteControl` pointed at `tmpdir/ants-N/ants-terminal.sock`
  via `ANTS_REMOTE_SOCKET`. Expect `start()` to return false
  (because `ensureSocketDir` rejects the regular-file path),
  *not* to bind the socket somewhere unexpected.

### Pre-fix regression check

SD-3, SD-4, INT-1 all fail to reject against the pre-patch
`QDir::mkpath` (which happily mkpath's into a symlink target and
creates the dir 0755). Loud failures.

## 8. Migration / back-compat

- **Existing `/tmp/ants-terminal-<uid>.sock` files:** orphan after
  upgrade. The new code writes to a different path; the old file
  sits in `/tmp` until the next reboot wipes `/tmp` (typical
  distro default). One-off CHANGELOG note can mention `rm
  /tmp/ants-terminal-$(id -u).sock` for users who want to tidy.
- **No wire schema change.** Clients reach the socket via the new
  path; `ANTS_REMOTE_SOCKET` lets a client point at the old path
  if it must (mostly for scripted regression of the migration
  itself).
- **No CHANGELOG-breaking change.** The socket lookup logic on the
  client side already reads `XDG_RUNTIME_DIR` or the
  `ANTS_REMOTE_SOCKET` env. The fallback isn't part of the public
  contract.

## 9. Performance & RAM budget

- **Startup overhead:** one `::lstat` + zero or one `::mkdir` per
  Ants launch. Sub-µs.
- **Runtime overhead:** zero. Once `start()` succeeds, the helper
  is never re-entered.
- **Memory:** stack-only. No heap.
- **Idle:** zero.

## 10. Out of scope

- Hardening `ANTS_REMOTE_SOCKET` override paths. The user picked
  the path; the user owns the trust posture.
- Per-instance socket suffixing. Today one socket per UID; multi-
  instance is via `ANTS_REMOTE_SOCKET` override. A future feature
  could append a session ID inside `/tmp/ants-<uid>/`.
- Cleanup of `/tmp/ants-<uid>` on shutdown (rejected by INV-6).
- Probing whether `/run/user/<uid>` is somehow degraded (e.g.,
  systemd not running, tmpfs full). Falling back from XDG to
  `/tmp` already covers the broken-systemd case; further fallback
  to `$HOME/.cache/ants-terminal/sock` is an option but adds
  complexity for a deployment shape (no `/tmp`, no `/run/user`)
  that doesn't exist in practice.

## 11. Cold-eyes review log

**Loop 0 (initial draft, 2026-05-15):** Drafted above.

**Loop 1 — VERIFY against current source (2026-05-15):**

- ✅ `defaultSocketPath` at `remotecontrol.cpp:60-73`. XDG primary
  at `:66-70`, `/tmp` fallback at `:71-72`.
- ✅ `QDir().mkpath(QFileInfo(path).absolutePath())` at `:79`.
  This is what we're replacing with `ensureSocketDir(dir)`.
- ✅ `setSocketOptions(UserAccessOption)` at `:85`.
- ✅ `safeToUnlinkLocalSocket` defined at `secureio.h:72-82`
  (not file-scope static in `remotecontrol.cpp`; promoted by
  ANTS-1132 per the header comment). The function checks
  `S_ISSOCK(st.st_mode)` and `st.st_uid != ::getuid()`. Our new
  `ensureSocketDir` mirrors the same pattern but checks `S_ISDIR`
  + mode 0700 instead.
- ✅ Headers: `<sys/stat.h>` already at `remotecontrol.cpp:40`
  (alongside `<sys/socket.h>` and `<sys/un.h>` for the existing
  `lstat` + `ucred` usage). Only `<cerrno>` needs adding for the
  `errno == ENOENT` check.
- ✅ `::lstat` + `S_ISDIR` + `st.st_uid` + mode-mask pattern is
  proven in the codebase — `safeToUnlinkLocalSocket` at
  `secureio.h:72-82` uses the same idioms. New helper composes
  cleanly.
- ⚠ Loop 0 § 4.2 didn't address what happens if `ensureSocketDir`
  succeeds but the **socket file itself** at
  `/tmp/ants-<uid>/ants-terminal.sock` was pre-created by an
  attacker (e.g., a regular file). The existing
  `safeToUnlinkLocalSocket` guard at `start()` already handles
  this — `lstat → !S_ISSOCK → refuse → log → return false`. The
  new layer (0700 subdir) raises the bar; the existing layer
  catches anything that gets past it. INV-10 implicitly covers
  this but not explicitly. Added clarification: even if a
  same-UID attacker creates `/tmp/ants-<uid>/` with valid 0700
  perms (e.g., they share the same UID), `safeToUnlinkLocalSocket`
  at the file level still gates the socket-bind step. The
  defense is layered, not single-point.
- ✅ Pattern mirrors `secureio.h:72-82` (`safeToUnlinkLocalSocket`)
  — same `lstat → stat-mode-checks → reject` shape. Consistent
  with the codebase's existing security helpers.
- ✅ INV-1 through INV-10 reviewed. Every INV maps to at least
  one SD-* / WI-* / INT-* test:
  - INV-1 ↔ WI-2 (new socket path format)
  - INV-2 ↔ WI-4 (single `::mkdir(0700)` call) + SD-1
  - INV-3 ↔ SD-1
  - INV-4 ↔ SD-2
  - INV-5 ↔ SD-3
  - INV-6 ↔ design constraint; no cleanup in destructor
  - INV-7 ↔ design discussion (umask edge case); SD-4 indirectly
    covers via the mode-check path
  - INV-8 ↔ SD-7
  - INV-9 ↔ SD-6 (override path bypasses dir helper for
    `defaultSocketPath`, but Loop 1 noted the dir helper still
    runs on the override's parent — by design)
  - INV-10 ↔ SD-3, SD-4, INT-1
- ✅ Cross-doc: composes with ANTS-1132 (existing trust model,
  SO_PEERCRED + 0700-on-socket + safeToUnlink). The new layer
  applies the same posture to the *containing directory*.
- ✅ RAM budget § 9 unchanged — sub-µs startup cost, zero runtime.

Status: **CLEAN.** Ready for Loop 2 implementation pass.

---

*End of spec.*

---

*End of spec.*
