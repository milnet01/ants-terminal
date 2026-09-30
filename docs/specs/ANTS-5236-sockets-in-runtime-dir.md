# ANTS-5236 — Bind the Claude hook and MCP sockets in the private runtime directory

**Status:** spec draft (2026-10-01).
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
| Claude hooks | `<antsRuntimeDir>/claude-hooks-<pid>` | `/tmp/ants-claude-hooks-<pid>` |
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
private, so it is an acceptable home; it is not one of the shared names.

The terminal never binds a legacy path again. Legacy paths are only READ, by
the clients in § 2.3 and § 2.4, so a terminal started before this change stays
reachable until it is relaunched.

### 2.2 The terminal side

- `ClaudeIntegration::defaultHookSocketPath()` returns
  `privateSocketPath("claude-hooks-<pid>")`.
- `startHookServer` and `startMcpServer` return false for an empty path,
  before touching `LocalSocketHub`.
- In `MainWindow::setupClaudeMcpProviders`, `mcpSocket` becomes
  `privateSocketPath("mcp-<pid>")`, and the `startMcpServer(mcpSocket)` call and
  the `qputenv("ANTS_MCP_SOCKET", …)` are skipped when it is empty. Each stays
  the single occurrence inside the `claudeMcpEnabled()` branch, so ANTS-5144
  INV-8, ANTS-1901 INV-2 and ANTS-1897 INV-14 hold.
- The stale-socket sweep moves out of that function into
  `mcpd::reapStaleTerminalSockets(pid_t self)` (`src/mcpdsocket.cpp`, which
  `ants_mcpcore_lib` builds and the terminal links), so a test can call it. It
  runs over both directories, `mcp-*` in `antsRuntimeDir()` and
  `ants-terminal-mcp-*` in `tempPath()`. Its rules are unchanged: skip `self`,
  skip a live pid, remove only through `safeToUnlinkLocalSocket`.

### 2.3 The hook forwarder script

The script text moves into one function that both writers use:

```cpp
// src/claudesetup.h
QString statusHookScript(const QString &runtimeDir);
Outcome refreshStatusHookScript();   // rewrite an installed forwarder when stale
```

- The script walks up to the nearest `ants-terminal` as today. For that pid it
  tries `<runtimeDir>/claude-hooks-$pid`, then the legacy
  `/tmp/ants-claude-hooks-$pid`, and sends to the first that is a socket.
  `runtimeDir` is the value of `antsRuntimeDir()` when the script is written.
- The `SO_PEERCRED` uid check stays, for both candidates.
- The socket path reaches Python as `argv[1]`, not spliced into the Python
  source, so a path containing a quote cannot change the program.
- `refreshStatusHookScript()` runs at start-up, before `startHookServer()`. It
  writes the script only where the forwarder file already exists and its bytes
  differ from `statusHookScript(antsRuntimeDir())`. It never creates the file
  and never touches `~/.claude/settings.json`, so a user who never installed
  the hooks is not opted in.

### 2.4 The MCP clients

- `mcpd::pickTerminalSocket` keeps its `ANTS_MCP_SOCKET` override unchanged.
  Without it, it ranks candidates from both directories in one list: `mcp-*` in
  `antsRuntimeDir()` and `ants-terminal-mcp-*` in `tempPath()`. The ranking and
  the per-candidate checks do not change: `lstat` socket owned by the uid, live
  pid first, then newest mtime. The `whyNot` text names both directories.
- `tools/mcp-bridge.py::pick_socket` does the same, with
  `$XDG_RUNTIME_DIR/ants-terminal/mcp-*` checked when `XDG_RUNTIME_DIR` is set.
  The bridge is kept through the next release only (ANTS-5308), so it gets no
  equivalent of Qt's fallback when `XDG_RUNTIME_DIR` is unset.

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
  `antsRuntimeDir()` + `/claude-hooks-<pid>` and `/mcp-<pid>`. Neither is under
  `QDir::tempPath()` when `XDG_RUNTIME_DIR` is set. Broken by leaving either
  path built from `tempPath()`. *Test:*
  `tests/features/claude_socket_runtime_dir/` case `Inv1PathsUseRuntimeDir`:
  it calls `defaultHookSocketPath()`, and reads `mainwindow.cpp` to check that
  `mcpSocket` is built with `privateSocketPath`.
- **INV-2** — When `<RuntimeLocation>/ants-terminal` exists at a mode other
  than 0700, `privateSocketPath` returns "", `startHookServer` and
  `startMcpServer` return false for it, and nothing is created under
  `tempPath()`. Broken by a fallback to `/tmp`, or by skipping
  `ensureSocketDir`. *Test:*
  `tests/features/claude_socket_runtime_dir/` case `Inv2BadDirBindsNothing`.
- **INV-3** — `pickTerminalSocket` returns a live socket in the runtime
  directory, and still returns a live legacy socket in `tempPath()` when that is
  the only one. It skips a candidate in either directory that is not a socket
  owned by the uid. Broken by scanning one directory only. *Test:*
  `tests/features/claude_socket_runtime_dir/` case `Inv3PickerScansBothDirs`.
- **INV-4** — `mcp-bridge.py`'s `pick_socket` finds a socket in
  `$XDG_RUNTIME_DIR/ants-terminal/` and in the legacy glob. Broken by keeping
  the single `SOCK_GLOB`. *Test:*
  `tests/features/mcp_bridge_client/test_mcp_bridge_client.py`.
- **INV-5** — The script from `statusHookScript(dir)` delivers stdin to
  `<dir>/claude-hooks-<pid>` when that socket exists, and to the legacy path
  when only that exists. It sends nothing to a socket whose peer uid is not the
  user's. Broken by dropping either candidate or the peer check. *Test:*
  `tests/features/claude_socket_runtime_dir/` case `Inv5ScriptTriesBothPaths`,
  which runs the script against listening sockets with a fake process tree.
- **INV-6** — `refreshStatusHookScript()` rewrites an existing forwarder whose
  bytes differ, leaves an up-to-date one untouched, and creates nothing when
  the file is absent. Broken by creating the file, or by rewriting unchanged
  bytes. *Test:* `tests/features/claude_socket_runtime_dir/` case
  `Inv6RefreshOnlyWhatExists`.
- **INV-7** — `mcpd::reapStaleTerminalSockets` removes a dead-pid socket in
  each directory and keeps a live one and `self`'s. Broken by sweeping one directory only.
  *Test:* `tests/features/claude_socket_runtime_dir/` case
  `Inv7SweepCoversBothDirs`.
- **INV-8** — ANTS-5144 INV-8, ANTS-1901 INV-2 and ANTS-1897 INV-14 hold.
  *Test:* `McpMasterToggle.INV2_StartupGate` and
  `McpOrientation_Inv14.MainWindowExportsSocket` pass unmodified.

## 4. RAM / build cost

None worth stating: one more directory listing in the sweep and the picker,
and one small file compare at start-up. No new target or library. The new
feature test joins an existing bundle.

## 5. Out of scope

- Removing the legacy `/tmp` readers from the picker, the bridge and the
  script. They exist for terminals started before this change, so they can go
  one release after it ships. Needs a roadmap item (not yet filed).
- Flatpak. Inside the sandbox the runtime directory is the app's own, and
  whether host-side clients can reach it is ANTS-5527's question.
- The remote-control socket. It is already in the runtime directory
  (`RemoteControl::defaultSocketPath`), and moving it into `ants-terminal/`
  would break `--remote` clients for no security gain.

## 6. Tests

Feature test: `tests/features/claude_socket_runtime_dir/`, with a `spec.md`
mapping each case to its invariant. Covers INV-1, INV-2, INV-3, INV-5, INV-6
and INV-7. Each case points `XDG_RUNTIME_DIR` and `TMPDIR` at scratch
directories, so nothing touches the real ones. INV-4 is a case added to
`tests/features/mcp_bridge_client/test_mcp_bridge_client.py`. INV-8 is the
existing tests, unmodified.

Red run: INV-1, INV-3 and INV-4 fail against pre-change source. INV-2,
INV-5, INV-6 and INV-7 call API this item adds; stub it first, so they fail on
their assertions rather than on the build.

## 7. Cross-doc impact

- ANTS-4932 § 2.2 (the picker) and § 2.6 (the terminal keeps the `/tmp`
  path) are amended to name the runtime directory and point here.
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
| INV-8 | `tests/features/mcp_master_toggle/`, `tests/features/mcp_orientation_install/` |

## Cold-eyes loop log

The rows are in [`docs/reviews/ANTS-5236-sockets-in-runtime-dir-loop-log.md`](../reviews/ANTS-5236-sockets-in-runtime-dir-loop-log.md).
