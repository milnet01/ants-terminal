# Flatpak host-shell wiring — feature spec

## Contract

When Ants Terminal runs inside a Flatpak sandbox, the user's shell
must execute on the **host**, not inside the sandboxed runtime. The
sandbox's minimal KDE Platform doesn't carry the user's shell, tools,
or `$PATH`; running commands under the sandbox's namespaces would
also sever the user's filesystem and tooling. The standard pattern
(used by Ghostty's Flathub build) is to detect the sandbox at fork
time and exec the shell via `flatpak-spawn --host`.

## Invariants

**INV-1 — Flatpak detection probes both signals.**
`src/ptyhandler.cpp` must test **both** `getenv("FLATPAK_ID")` and
`access("/.flatpak-info", F_OK)` before branching. `FLATPAK_ID` alone
misses sandboxes started outside the usual `flatpak run` path;
`/.flatpak-info` alone misses portal-detached re-execs. The condition
is an OR so either signal triggers the host-shell branch.

**INV-2 — Host branch invokes `flatpak-spawn --host`.**
The in-sandbox exec must go through `execvpe("flatpak-spawn", …, childEnvp)`
(ANTS-1994 — the pre-fork sanitised envp, matching the direct-exec path; was
`execvp` pre-1994) with
`--host` as the first positional argument. The `--` separator must
appear before the shell path so flatpak-spawn cleanly separates its
own options from the command to run.

**INV-3 — TERM family crosses the sandbox via `--env=`.**
`flatpak-spawn --host` does not inherit the caller's environment. The
five variables the direct-exec path sets (`TERM`, `COLORTERM`,
`TERM_PROGRAM`, `TERM_PROGRAM_VERSION`, `COLORFGBG`) must each appear
as a `--env=KEY=VALUE` token in the argv so the host shell sees the
same capability string as the non-Flatpak path. `TERM_PROGRAM_VERSION`
must pull from the `ANTS_VERSION` compile definition (not a hardcoded
literal) so a version bump propagates without editing ptyhandler.

**INV-4 — Working directory crosses via `--directory=`.**
The sandbox `chdir()` does not cross into the host namespace. When a
non-empty `workDir` is passed into `Pty::start`, the host branch must
include a `--directory=<workDir>` argv token before `--` so
flatpak-spawn sets the host-side cwd correctly.

**INV-5 — Direct-exec fallback preserved.**
Outside the sandbox (no `FLATPAK_ID`, no `/.flatpak-info`), the fork
child must still reach the existing `execlp(shellCStr, argv0, nullptr)`
path with the login-shell `-<shellName>` argv[0] convention. The
Flatpak branch must be additive — removing it must return to the
pre-0.7.2 behavior byte-for-byte.

**INV-6 — Failure mode matches direct exec.**
If `flatpak-spawn` is missing (extremely unusual inside a Flatpak, but
possible on corrupted installs), the child must fall through to
`_exit(127)` the same way a missing shell does on the direct-exec
path. No crash, no undefined state.

**INV-7 — `ANTS_MCP_SOCKET` crosses the sandbox via `--env=`.**
`flatpak-spawn --host` does not inherit the caller's environment (same
reason as INV-3). When Ants runs directly, the MCP socket path reaches
Claude sessions through the non-flatpak `environ`-copy loop
(ANTS-1897 INV-14); inside a flatpak the sandbox boundary swallows it.
The host branch must therefore forward `--env=ANTS_MCP_SOCKET=<path>`,
reading the value from `getenv("ANTS_MCP_SOCKET")` (the path
`src/mainwindow.cpp` exported via `qputenv` after `startMcpServer`), so
the SessionStart MCP-orientation hook — and every socket-gated MCP
affordance — fires the same inside a flatpak install as outside. The
token is gated on a non-empty value (mirrors INV-4's `--directory`
gating) so an MCP-disabled install omits the arg rather than passing an
empty `--env=ANTS_MCP_SOCKET=`. The buffer is filled pre-fork
(ANTS-1046 no-heap-after-fork discipline). (ANTS-1900, follow-up to
ANTS-1897 § 4.)

## Why these invariants matter

- **INV-1/INV-2:** getting the detection or the --host flag wrong
  silently runs the shell inside the sandbox, which looks like "my
  PATH is broken" to the user — silent-wrong is the worst failure
  mode.
- **INV-3:** missing TERM propagation makes the host shell fall back
  to `dumb`, breaking colors, cursor positioning, and Kitty keyboard
  mode. Silent degradation.
- **INV-4:** missing --directory means the shell starts in the user's
  home instead of the Ants-requested working dir, breaking
  "open-new-tab-in-this-project" UX.
- **INV-5:** a change to the Flatpak branch that breaks the
  non-Flatpak branch would hit every user on every install.
- **INV-7:** without the socket forward, a flatpak-installed Ants runs
  MCP fine but Claude never learns it's there — the cheat-sheet never
  prints and the assistant falls back to the token-heavy built-ins.
  Silent degradation, exactly the ANTS-1897 regression this closes for
  the flatpak path.

## Test strategy

Source-grep only — we cannot fork a real Flatpak runtime from a unit
test, and the forkpty child's code path is never reached on the host
CI runners (which don't run inside Flatpak). A source-grep test pins
the shape of the branch so regressions show up on commit.

Test binary links no Qt / no Ants sources; it just reads
`src/ptyhandler.cpp` as text and asserts the invariants.
