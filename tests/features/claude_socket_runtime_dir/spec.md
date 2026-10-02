# claude_socket_runtime_dir — the Claude hook and MCP sockets bind in the private runtime directory

Contract: [`docs/specs/ANTS-5236-sockets-in-runtime-dir.md`](../../../docs/specs/ANTS-5236-sockets-in-runtime-dir.md)
§ 3. The invariants below are that document's, under the same numbers.

## Invariants

- **INV-1** — the hook and MCP socket paths are in `antsRuntimeDir()`, not `tempPath()`. Case `Inv1PathsUseRuntimeDir`.
- **INV-2** — a runtime directory at a mode other than 0700 binds nothing and creates nothing in `tempPath()`. Case `Inv2BadDirBindsNothing`.
- **INV-3** — `pickTerminalSocket` scans the runtime directory, never picks a socket under the legacy `tempPath()` name (ANTS-5587), and skips a non-socket. Case `Inv3PickerIgnoresLegacyName`.
- **INV-4** — checked elsewhere: a case in `../mcp_bridge_client/test_mcp_bridge_client.py`; the bridge's runtime pattern is the module variable `RUNTIME_SOCK_GLOB`.
- **INV-5** — the forwarder script delivers to `$ANTS_CLAUDE_HOOK_SOCKET`, and with it unset delivers nothing, even to a listening legacy socket of the `ants-terminal` ancestor (ANTS-5587). Case `Inv5ScriptUsesExportedSocketOnly`.
- **INV-6** — `refreshStatusHookScript` rewrites a stale forwarder, leaves a current one alone, creates nothing. Case `Inv6RefreshOnlyWhatExists`.
- **INV-7** — `reapStaleTerminalSockets` sweeps the runtime directory, keeps live and own sockets, and leaves the legacy `tempPath()` name alone (ANTS-5587), a regular file there included (ANTS-5080). Case `Inv7SweepCoversRuntimeDirOnly`.
- **INV-8** — checked elsewhere: the existing `McpMasterToggle.INV2_StartupGate` and `McpOrientation_Inv14.MainWindowExportsSocket`, unmodified.
- **INV-9** — `mainwindow.cpp` exports `ANTS_CLAUDE_HOOK_SOCKET` once, after `startHookServer(`. Case `Inv9HookSocketExported`.

## How it runs

Each case points `XDG_RUNTIME_DIR` (a 0700 directory) and `TMPDIR` at two
separate scratch directories, restores the environment afterwards, and clears
`ANTS_MCP_SOCKET` and `ANTS_CLAUDE_HOOK_SOCKET`. Nothing touches the real
ones. INV-5 runs the script under `bash` against `QLocalServer` sockets; its
legacy route uses a copy of `bash` named `ants-terminal` as the fake
ancestor. It skips where `python3` or `bash` is absent. INV-1 and INV-9 also
scrape `MainWindow::setupClaudeMcpProviders`, because they are claims about
where code sits.
