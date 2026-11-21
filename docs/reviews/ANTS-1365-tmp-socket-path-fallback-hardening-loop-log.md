# ANTS-1365 — `/tmp` socket-path fallback hardening (per-user 0700 subdir) — review loop log

Record for [`../specs/ANTS-1365-tmp-socket-path-fallback-hardening.md`](../specs/ANTS-1365-tmp-socket-path-fallback-hardening.md) § 11. Cold-eyes review log.

## Cold-eyes loop log

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
