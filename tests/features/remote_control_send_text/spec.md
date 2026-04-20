# Remote-control `send-text` — write bytes to a tab's PTY

## Contract

Extends the rc_protocol scaffold documented in
`tests/features/remote_control_ls/spec.md`. Adds the `send-text` command
which writes a UTF-8 string, byte-for-byte, to a tab's PTY master.
Matches Kitty's rc_protocol semantics
(https://sw.kovidgoyal.net/kitty/rc_protocol/#send-text).

Request shape:

    {"cmd": "send-text", "tab": <int optional>, "text": "<string>"}

- `tab` — 0-based tab index. Omitted → target the active tab.
- `text` — required UTF-8 string. Included bytes are written verbatim,
  including control characters and escape sequences. Callers are
  responsible for line terminators — `send-text` does **not** append
  a newline automatically (matches Kitty; auto-newline would surprise
  callers writing binary streams or partial lines).

Response shape on success:

    {"ok": true, "bytes": <int>}

`bytes` is the UTF-8 byte length written — useful for progress
reporting on large streams. Empty `text` is a legitimate no-op and
returns `"ok": true, "bytes": 0`.

Errors (each `{"ok": false, "error": "<msg>"}`, client exit code 2):

- `send-text: missing or non-string "text" field` — text field absent
  or not a JSON string
- `send-text: no tab at index <i>` — explicit `tab` out of range
- `send-text: no active terminal` — fallback target missing (should
  not fire in practice — `MainWindow` always keeps ≥ 1 tab)

Client CLI surface:

    ants-terminal --remote send-text --remote-text "echo hi\n"
    ants-terminal --remote send-text --remote-tab 0 --remote-text ""
    echo -n 'ls\n' | ants-terminal --remote send-text

When `--remote-text` is absent, the client reads stdin until EOF —
enables shell piping without inline-arg quoting pain.

## Invariants (source-grep)

- **INV-1** `RemoteControl::dispatch` routes `"send-text"` to
  `cmdSendText`. Drift would silently fall through to the
  "unknown command" branch and remote automation would break.
- **INV-2** `RemoteControl::cmdSendText` reads the `text` field
  via `QJsonValue::isString()` — matches Kitty's "text is required
  and must be a string" contract.
- **INV-3** `cmdSendText` uses `isDouble()` (not `toInt() == 0`) to
  distinguish "tab omitted" from "tab 0". A `toInt()`-based check
  would silently target tab 0 when the caller intended the active
  tab, and `--remote-tab 0` would lose its meaning.
- **INV-4** `cmdSendText` calls `sendToPty` on the target terminal.
  The existing public TerminalWidget API — no new coupling.
- **INV-5** `MainWindow::terminalAtTab(int)` is declared public and
  guards against out-of-range indices (returns nullptr rather than
  asserting). Future commands (`get-text`, `select-window`, ...)
  share this accessor.
- **INV-6** `main.cpp` reads stdin when `send-text` is invoked without
  `--remote-text`. That's the documented ergonomic for shell piping;
  dropping it would break script authors who discover it and rely on
  it.
- **INV-7 (negative)** `cmdSendText` must NOT auto-append a newline.
  A caller writing `"text": "ls"` expects the remote terminal to see
  exactly 2 bytes; auto-newline would surprise binary-stream
  callers and diverge from Kitty semantics.
