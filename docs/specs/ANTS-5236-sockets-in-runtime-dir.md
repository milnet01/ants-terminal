# ANTS-5236 — Bind the Claude hook and MCP sockets in the private runtime directory

**Status:** accepted (2026-10-01); amended 2026-10-02 — the legacy `/tmp`
readers were removed one release later (ANTS-5587).
**Kind:** security.
**Source:** ROADMAP.md ANTS-5236 (code-quality-review-2026-09-11 perf pass,
lane claude-integration-a, via ANTS-5089).
**Composes with:** ANTS-5144 (one listener per path), ANTS-4932 (ants-mcpd
finds the terminal socket), ANTS-1897 (INV-14, the `ANTS_MCP_SOCKET`
export), ANTS-1365 (`ensureSocketDir`).

**Layman:** The two connection points Claude Code uses to talk to the
terminal move from the shared temporary folder into a folder only you can
open.

## 1. Problem

Two per-process sockets are bound in `QDir::tempPath()`, which is `/tmp` on
every supported system and writable by every local user:

- the Claude hook socket, `ClaudeIntegration::defaultHookSocketPath()`,
  `/tmp/ants-claude-hooks-<pid>`;
- the MCP socket, built in `MainWindow::setupClaudeMcpProviders`,
  `/tmp/ants-terminal-mcp-<pid>`.

The names are guessable from the terminal's pid. Consequences:

1. Another local user can create a file at the name first. Since ANTS-5144
   the terminal then refuses to bind (`LocalSocketHub::acquire`), so the
   hook feed or the MCP stops working for that launch.
2. Every client must defend itself against a foreign socket at the name. The
   installed hook forwarder checks `SO_PEERCRED`
   (`claude_setup::installStatusHooks`), and `mcpd::pickTerminalSocket` and
   `tools/mcp-bridge.py::pick_socket` check the owner uid. Each is a check a
   future client can forget.

The remote-control socket already avoids both: `RemoteControl::defaultSocketPath`
uses `QStandardPaths::RuntimeLocation`, which is `$XDG_RUNTIME_DIR`, a
directory systemd creates at mode 0700 for the user.

The move is not a one-line change. The hook forwarder script is written into
`~/.config/ants-terminal/hooks/` by `installStatusHooks`, which runs only when
the user clicks install (`WelcomeDialog`, `SettingsDialog`). An installed copy
hardcodes the `/tmp` name. And a terminal started before the change keeps its
`/tmp` sockets until it is relaunched, while a rebuilt `ants-mcpd` may be
looking for it.

## 2. Surface

### 2.1 One private directory

```cpp
// src/configpaths.h
inline QString antsRuntimeDir();   // "<RuntimeLocation>/ants-terminal", or "" when
                                   // QStandardPaths returns no RuntimeLocation

// src/secureio.h
// antsRuntimeDir() + "/" + name, after ensureSocketDir(antsRuntimeDir())
// accepts or creates the directory; "" when it does not, or the dir is "".
inline QString privateSocketPath(const QString &name);
```

Both sockets live directly in it:

| Socket | New path | Legacy path (read only) |
|---|---|---|
| Claude hooks | `<antsRuntimeDir>/claude-hooks-<pid>` | `<tempPath>/ants-claude-hooks-<pid>` |
| MCP | `<antsRuntimeDir>/mcp-<pid>` | `<tempPath>/ants-terminal-mcp-<pid>` |

Both socket paths come from `privateSocketPath`. `ensureSocketDir`
(`src/secureio.h`) accepts only a real directory owned by the user at mode
0700, and creates it that way when absent. When `privateSocketPath` returns
"", that server does not start and the terminal logs why. **There is no
fallback to the shared `/tmp` names.** A fallback would bring back the problem
this item removes.

Qt's own fallback is kept. Where `XDG_RUNTIME_DIR` is unset, or names a
directory that is not the user's at 0700, `RuntimeLocation` is
`<tempPath>/runtime-<user>`, which Qt creates at 0700. That directory is
private, so it is an acceptable home. Its name is guessable, though: where
another user holds it, both servers stay off, as a squat stops them today.
Sessions with a working `XDG_RUNTIME_DIR` do not reach that case.

The terminal never binds a legacy path again. Since ANTS-5587 no client reads
one either.

### 2.2 The terminal side

- `ClaudeIntegration::defaultHookSocketPath()` returns
  `privateSocketPath("claude-hooks-<pid>")`.
- `startHookServer` and `startMcpServer` return false for an empty path,
  before touching `LocalSocketHub`.
- Where `startHookServer()` succeeds, the terminal exports the path it bound as
  `ANTS_CLAUDE_HOOK_SOCKET`, with `qputenv`, before the first tab spawns a PTY.
  This is `ANTS_MCP_SOCKET`'s pattern (ANTS-1897 INV-14). It lets each
  terminal's own tabs name its own socket, whatever its environment made of
  the runtime directory. Where it fails, the terminal clears the variable, so
  a terminal started from an Ants tab never inherits its parent's socket.
- In `MainWindow::setupClaudeMcpProviders`, `mcpSocket` becomes
  `privateSocketPath("mcp-<pid>")`, and the `startMcpServer(mcpSocket)` call and
  the `qputenv("ANTS_MCP_SOCKET", …)` are skipped when it is empty. Each stays
  the single occurrence inside the `claudeMcpEnabled()` branch, so ANTS-5144
  INV-8, ANTS-1901 INV-2 and ANTS-1897 INV-14 hold.
- The stale-socket sweep moves out of that function into
  `mcpd::reapStaleTerminalSockets(pid_t self)` (`src/mcpdsocket.cpp`, which
  `ants_mcpcore_lib` builds and the terminal links), so a test can call it. It
  runs over `mcp-*` in `antsRuntimeDir()`. Its rules are unchanged: skip `self`,
  skip a live pid, remove only through `safeToUnlinkLocalSocket`.

### 2.3 The hook forwarder script

The script text moves into one function that both writers use:

```cpp
// src/claudesetup.h
QString statusHookScript();
Outcome refreshStatusHookScript();   // rewrite an installed forwarder when stale
```

- The script sends to `$ANTS_CLAUDE_HOOK_SOCKET` when that names a socket, and
  otherwise sends nothing. No runtime directory is baked in, so terminals with
  different environments write the same bytes.
- The `SO_PEERCRED` uid check stays.
- The socket path reaches Python as `argv[1]`, not spliced into the Python
  source, so a path containing a quote cannot change the program.
- `refreshStatusHookScript()` runs at start-up, before `startHookServer()`. It
  writes the script only where the forwarder file already exists and its bytes
  differ from `statusHookScript()`. It never creates the
  file and never touches `~/.claude/settings.json`, so a user who never installed
  the hooks is not opted in.

### 2.4 The MCP clients

- `mcpd::pickTerminalSocket` keeps its `ANTS_MCP_SOCKET` override unchanged.
  Without it, it ranks the `mcp-*` candidates in `antsRuntimeDir()`. The ranking
  and the per-candidate checks do not change: `lstat` socket owned by the uid,
  live pid first, then newest mtime. The `whyNot` text names the directory.
- `tools/mcp-bridge.py::pick_socket` does the same, with
  `$XDG_RUNTIME_DIR/ants-terminal/mcp-*` checked when `XDG_RUNTIME_DIR` is set.
  That pattern is the module variable `RUNTIME_SOCK_GLOB`, so a test can point
  it elsewhere. The bridge is kept through the next release only (ANTS-5308), so  it gets no equivalent of Qt's fallback, and misses a terminal that took it.
- Outside an Ants tab, both pickers resolve the runtime directory from their
  own environment. A picker whose environment resolves it differently from the
  terminal's finds nothing. That case is accepted.

### 2.5 How it reaches a running terminal

It does not, by design: a socket is bound at start-up. Each terminal moves at
its next launch. Until then:

- a terminal started before the change keeps its `/tmp` sockets, and the
  refreshed script and the new pickers still find them;
- a Claude Code session inside a tab already has `ANTS_MCP_SOCKET`, which
  names the socket of the terminal it runs in;
- an `ants-mcpd` started before the rebuild, outside any Ants tab, finds a
  relaunched terminal only after `/mcp` reconnects it.

## 3. Invariants

- **INV-1** — `defaultHookSocketPath()` and the MCP socket path are
  `antsRuntimeDir()` + `/claude-hooks-<pid>` and `/mcp-<pid>`. Broken by leaving
  either path built from `tempPath()`. *Test:*
  `tests/features/claude_socket_runtime_dir/` case `Inv1PathsUseRuntimeDir`:
  it calls `defaultHookSocketPath()`, and reads `mainwindow.cpp` to check that
  `mcpSocket` is built with `privateSocketPath`. The case's `XDG_RUNTIME_DIR`
  is a 0700 directory outside its `TMPDIR`, so a path left in `tempPath()`
  cannot pass.
- **INV-2** — When `<RuntimeLocation>/ants-terminal` exists at a mode other
  than 0700, `privateSocketPath` returns "", `startHookServer` and
  `startMcpServer` return false for it, and nothing is created under
  `tempPath()`. Broken by a fallback to `/tmp`, or by skipping
  `ensureSocketDir`. *Test:*
  `tests/features/claude_socket_runtime_dir/` case `Inv2BadDirBindsNothing`.
- **INV-3** — `pickTerminalSocket` returns a live socket in the runtime
  directory, never a live socket under the legacy `tempPath()` name, and skips
  a candidate that is not a socket owned by the uid. Broken by reading the
  legacy name. *Test:* `tests/features/claude_socket_runtime_dir/` case
  `Inv3PickerIgnoresLegacyName`.
- **INV-4** — `mcp-bridge.py`'s `pick_socket` finds a socket in
  `$XDG_RUNTIME_DIR/ants-terminal/` and never one under the legacy
  `/tmp/ants-terminal-mcp-*` name. Broken by reading that glob. *Test:*
  `tests/features/mcp_bridge_client/test_mcp_bridge_client.py`.
- **INV-5** — The script from `statusHookScript()` delivers stdin to
  `$ANTS_CLAUDE_HOOK_SOCKET` when that socket exists, and with the variable
  unset delivers nothing, even to a listening legacy
  `<tempPath>/ants-claude-hooks-<pid>` of an `ants-terminal` ancestor. Broken
  by dropping the variable route or keeping the ancestor walk. *Test:*
  `tests/features/claude_socket_runtime_dir/` case
  `Inv5ScriptUsesExportedSocketOnly`, which runs the script against listening
  sockets with a fake process tree. The peer-uid check cannot be exercised by
  a single-user test, so the case also checks the script text: the uid compare
  guards the send.
- **INV-6** — `refreshStatusHookScript()` rewrites an existing forwarder whose
  bytes differ, leaves an up-to-date one untouched, and creates nothing when
  the file is absent. Broken by creating the file, or by rewriting unchanged
  bytes. *Test:* `tests/features/claude_socket_runtime_dir/` case
  `Inv6RefreshOnlyWhatExists`.
- **INV-7** — `mcpd::reapStaleTerminalSockets` removes a dead-pid socket in
  the runtime directory, keeps a live one and `self`'s, and leaves the legacy
  `tempPath()` name alone. Broken by skipping the runtime directory, or by
  sweeping the legacy name. *Test:* `tests/features/claude_socket_runtime_dir/`
  case `Inv7SweepCoversRuntimeDirOnly`.
- **INV-9** — `mainwindow.cpp` exports `ANTS_CLAUDE_HOOK_SOCKET` exactly once,
  after a successful `startHookServer()` and before the first `newTab()`.
  Broken by exporting before the bind, or not at all. *Test:*
  `tests/features/claude_socket_runtime_dir/` case `Inv9HookSocketExported`,
  a source check in the style of `McpOrientation_Inv14`.
- **INV-8** — ANTS-5144 INV-8, ANTS-1901 INV-2 and ANTS-1897 INV-14 hold.
  *Test:* `McpMasterToggle.INV2_StartupGate` and
  `McpOrientation_Inv14.MainWindowExportsSocket` pass unmodified.

## 4. RAM / build cost

None worth stating: one more directory listing in the sweep and the picker,
and one small file compare at start-up. No new target or library. The new
feature test joins an existing bundle.

## 5. Out of scope

- Flatpak. Inside the sandbox the runtime directory is the app's own, and
  whether host-side clients can reach it is ANTS-5527's question.
- The remote-control socket. It is already in the runtime directory
  (`RemoteControl::defaultSocketPath`), and moving it into `ants-terminal/`
  would break `--remote` clients for no security gain.

## 6. Tests

Feature test: `tests/features/claude_socket_runtime_dir/`, with a `spec.md`
mapping each case to its invariant. Covers INV-1, INV-2, INV-3, INV-5, INV-6,
INV-7 and INV-9. Each case points `XDG_RUNTIME_DIR` and `TMPDIR` at scratch
directories, so nothing touches the real ones. INV-4 is a case added to
`tests/features/mcp_bridge_client/test_mcp_bridge_client.py`, and every case in
that file clears `ANTS_MCP_SOCKET` and points both of the bridge's patterns at
scratch directories. The new cases clear `ANTS_MCP_SOCKET` and
`ANTS_CLAUDE_HOOK_SOCKET` too, since a test run inside an Ants tab inherits
both. INV-8 is the existing tests, unmodified.

Red run: INV-1, INV-3, INV-4 and INV-9 fail against pre-change source. INV-2,
INV-5, INV-6 and INV-7 call API this item adds; stub it first, so they fail on
their assertions rather than on the build.

## 7. Cross-doc impact

- ANTS-4932 § 2.1 (the picker) and § 2.6 (the terminal keeps the `/tmp`
  path) are amended to name the runtime directory and point here.
- ANTS-5144 INV-8 says the MCP start-up call site is "unchanged". It changes
  here, so its text is amended to what its tests check: one guarded
  `startMcpServer` and one `qputenv`.
- Comments naming the old paths: `src/mcpdsocket.h`, `src/claudesetup.cpp`,
  `src/secureio.h`, and `src/remotecontrolgate.h`, whose `control.sock` path
  was never right.
- CHANGELOG `### Security` bullet.

## What checks this

| Invariant | Checked by |
|---|---|
| INV-1 | `tests/features/claude_socket_runtime_dir/` |
| INV-2 | `tests/features/claude_socket_runtime_dir/` |
| INV-3 | `tests/features/claude_socket_runtime_dir/` |
| INV-4 | `tests/features/mcp_bridge_client/` |
| INV-5 | `tests/features/claude_socket_runtime_dir/` |
| INV-6 | `tests/features/claude_socket_runtime_dir/` |
| INV-7 | `tests/features/claude_socket_runtime_dir/` |
| INV-9 | `tests/features/claude_socket_runtime_dir/` |
| INV-8 | `tests/features/mcp_master_toggle/`, `tests/features/mcp_orientation_install/` |

## Cold-eyes loop log

The rows are in [`docs/reviews/ANTS-5236-sockets-in-runtime-dir-loop-log.md`](../reviews/ANTS-5236-sockets-in-runtime-dir-loop-log.md).
