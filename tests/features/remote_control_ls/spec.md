# Remote-control `ls` — first slice of Kitty-style rc_protocol

## Contract

Ants Terminal exposes a Kitty-style remote-control protocol over a Unix
domain socket. Each connection handles one JSON request / one JSON
response, LF-terminated. The socket path resolves in order:

1. `$ANTS_REMOTE_SOCKET` — explicit override
2. `$XDG_RUNTIME_DIR/ants-terminal.sock` — XDG-standard user-scoped runtime dir
3. `/tmp/ants-terminal-<uid>.sock` — fallback when `$XDG_RUNTIME_DIR` is unset

Request shape: `{"cmd": "<name>", ...args}`
Response shape on success: `{"ok": true, ...result-fields}`
Response shape on error: `{"ok": false, "error": "<message>"}`

This first slice implements only `ls` — the socket + envelope
infrastructure is in place for the remaining commands (`send-text`,
`set-title`, `select-window`, `get-text`, `new-tab`, `launch`) to land
one-by-one in subsequent commits.

The client is the same binary invoked with `--remote <cmd>` — no
separate client binary. Exit code: 0 success, 1 connect/parse error,
2 server-side error response.

## Invariants (source-grep)

Locked against `src/remotecontrol.{h,cpp}`, `src/mainwindow.{h,cpp}`,
`src/main.cpp`, `CMakeLists.txt`.

- **INV-1** `remotecontrol.h` declares a class `RemoteControl : public QObject`.
- **INV-2** `RemoteControl::defaultSocketPath()` consults
  `ANTS_REMOTE_SOCKET` via `qgetenv` — the env-var override must win
  unconditionally so multi-instance setups work.
- **INV-3** `RemoteControl::dispatch()` handles at minimum the `ls`
  command name, and returns the `unknown command:` error shape for any
  other string — future commands extend the switch, not replace it.
- **INV-4** The response envelope for a successful `ls` is
  `{"ok": true, "tabs": [...]}`; `ok` and `tabs` field names are the
  stable contract rc_protocol clients depend on.
- **INV-5** `MainWindow::tabListForRemote()` is exposed publicly so
  `RemoteControl` can read it without `friend` access. The method
  emits each tab as a JSON object with `index`, `title`, `cwd`,
  `active` keys — those names are the stable contract exposed
  to rc_protocol callers.
- **INV-6** `main.cpp` registers a `--remote` `QCommandLineOption` and
  delegates to `RemoteControl::runClient` when set; the branch runs
  *before* `MainWindow` construction so the client never boots a
  second GUI on top of the running instance.
- **INV-7** `CMakeLists.txt` lists `src/remotecontrol.cpp` in the
  `ants-terminal` executable sources — drift here would link-fail at
  `RemoteControl::runClient` but this invariant catches it at
  source-grep time for cleaner diagnostics.
- **INV-8 (negative)** `remotecontrol.cpp` must not import any Qt
  `Widgets` / `Gui` headers beyond what's transitively required for
  `MainWindow` — the remote-control layer has no UI surface and
  should not grow one. Specifically: no `QMessageBox`, `QMenu`,
  `QDialog`.
