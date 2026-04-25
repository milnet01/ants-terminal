# Feature: PTY child closes all inherited FDs (close_range, not fd<1024)

## Contract

When `Pty::start` calls `forkpty()` and lands in the child branch, the
child MUST close every inherited file descriptor with index ≥ 3 before
calling `execvp` / `execlp`. The implementation must not assume the
process's `RLIMIT_NOFILE` soft cap is 1024.

The previous implementation iterated `fd = 3; fd < 1024; ++fd` and
called `::close(fd)` on each. On systems where the parent's
`RLIMIT_NOFILE` soft cap is above 1024 (systemd ≥ 240 service-unit
default `1024:524288` after the soft-cap bump in v240/v246, container
runtime defaults, hardened-server profiles), any descriptor allocated
above index 1023 leaked into the user's shell — defeating the
[CWE-403](https://cwe.mitre.org/data/definitions/403.html) mitigation
that the loop is supposed to provide.

## Rationale

Qt opens a moderate fan-out of FDs at startup: the X11 / Wayland
display socket, the D-Bus session connection, GLib's eventfd, the QPA
input-device watch FDs, plus whatever plugins (Lua, AI HTTP, MCP IPC,
remote-control socket) have allocated by the time the user spawns a
new tab. None of these are async-signal-safe to inspect after fork,
and all of them leak full read/write access to the parent process if
not closed before exec. A leaked AI-HTTP socket is a credentials
exfiltration vector; a leaked remote-control socket is a UID-scope
RCE-by-proxy vector; a leaked D-Bus socket lets the shell impersonate
the user's desktop session.

Linux 5.9 ships `close_range(2)`, a single syscall that closes every
descriptor in a contiguous range. glibc 2.34 wrapped it. It is
async-signal-safe (resolves to a single `syscall(2)` invocation),
fast (one transition into the kernel for the whole range), and does
not depend on the soft-cap value at all.

The fallback path runs only when `close_range` is unavailable
(kernel < 5.9, ENOSYS at runtime) or the syscall number is missing at
build time. It must consult `getrlimit(RLIMIT_NOFILE)` rather than
hard-code an upper bound, with a sanity cap to avoid spending seconds
in the loop on systems where the soft cap is in the hundreds of
thousands.

## Invariants

**INV-1 — `close_range` is invoked at the post-fork close-inherited-FDs
site.** Source-grep against `src/ptyhandler.cpp`: a literal
`SYS_close_range` token must appear inside the child branch
(`if (m_childPid == 0)` body). The mechanism may be a raw
`::syscall(SYS_close_range, …)` invocation or a glibc wrapper call
gated on `SYS_close_range` being defined.

**INV-2 — Hard-coded `fd < 1024` upper bound is gone.** Source-grep:
the regex `for\s*\(\s*int\s+fd\s*=\s*3\s*;\s*fd\s*<\s*1024\s*;` must
not match anywhere in `src/ptyhandler.cpp`. The previous loop is the
exact pattern this fix replaces; resurrecting it would silently
re-introduce the FD-leak window.

**INV-3 — Fallback consults `RLIMIT_NOFILE`, not a hardcoded cap.**
Source-grep: `getrlimit` (or `RLIMIT_NOFILE`) must appear inside the
child branch alongside the FD-close logic. If `close_range` returns
non-zero (failed for any reason), the fallback loop must be bounded by
the runtime soft cap, not a compile-time constant.

**INV-4 — Required headers included.** The file must include
`<sys/resource.h>` (for `getrlimit` / `RLIMIT_NOFILE`) and
`<sys/syscall.h>` (for `SYS_close_range`). Without these, the change
either fails to compile or silently degrades to the fallback.

**INV-5 — The fallback bound is capped to a sane maximum.** A naive
`for (fd = 3; fd < rl.rlim_cur; ++fd)` loop on a system with
`rlim_cur == 1048576` would issue ~1M `close()` syscalls per shell
spawn. Source-grep: a numeric cap (any of `65536`, `0x10000`, `1 <<
16`, etc.) must appear in close proximity to the `RLIMIT_NOFILE`
reference. The exact value is implementation choice; the requirement
is that *some* cap is applied.

## Scope

### In scope
- Source-grep regression test pinning the close_range usage, the
  removal of the `fd<1024` cap, and the RLIMIT_NOFILE fallback shape.

### Out of scope
- Behavioral test that opens an FD, spawns a real shell via
  `forkpty`, and inspects `/proc/<pid>/fd` to confirm the FD was
  closed. Would require linking against the full `Pty` class and a
  live PTY plus a child process to inspect — disproportionate for the
  invariant we want to lock. The structural test is sufficient because
  the bug class (forgetting to close FDs above some hardcoded
  threshold) is exactly what source-grep catches.
- macOS / non-Linux fallback paths. macOS has its own `closefrom(3)`
  in libsystem; cross-platform is out of scope until the H8 macOS port
  lands (ROADMAP).
- The `m_masterFd` `FD_CLOEXEC` set on the parent side at
  `ptyhandler.cpp:167` — that is a separate, already-shipped guard
  that prevents leaks the OTHER direction (master FD into child
  processes the parent later spawns). Distinct concern; out of scope
  here.

## Regression history

- **Original implementation:** `for (int fd = 3; fd < 1024; ++fd)
  ::close(fd)`. Correct on traditional `RLIMIT_NOFILE = 1024:4096`
  Linux distros, silently leaks FDs above 1024 on systemd / container
  defaults.
- **0.7.12 ROADMAP entry (Tier 2 hardening):** flagged as a CWE-403
  recurrence on systemd / container default RLIMIT_NOFILE. Tracked
  alongside `closefrom(3)` glibc wrapper availability (glibc 2.34+).
- **0.7.27 (this fix):** replaced with `close_range(2)` syscall +
  RLIMIT_NOFILE-bounded fallback. Locked by this spec.
