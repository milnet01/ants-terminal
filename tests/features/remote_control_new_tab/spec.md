# Remote-control `new-tab` — open a tab, optional cwd + command

## Contract

Third rc_protocol command on top of the scaffold pinned by
`remote_control_ls`. Opens a fresh tab in the running GUI and returns
its index so the caller can target it in follow-up commands.

Request shape:

    {"cmd":"new-tab","cwd":"<path optional>","command":"<string optional>"}

- `cwd` — starting working directory for the new shell. Empty/absent
  → inherit from the focused / current terminal (same default as
  the menu-driven `newTab()` slot).
- `command` — when non-empty, written to the new shell after a 200 ms
  settle via `TerminalWidget::writeCommand`. Timing matches
  `onSshConnect` (the existing in-tree pattern for "shell just
  started, wait for it before we write"). The caller is responsible
  for the trailing newline, matching `send-text` semantics — one
  consistent rule across every rc_protocol write-path.

Response shape on success:

    {"ok":true,"index":<int>}

`index` is the 0-based position of the new tab in the tab strip.
rc_protocol clients chain subsequent commands against it:

    idx=$(ants-terminal --remote new-tab --remote-cwd /tmp | jq .index)
    ants-terminal --remote send-text --remote-tab "$idx" \
        --remote-text $'ls -la\n'

Client CLI:

    ants-terminal --remote new-tab
    ants-terminal --remote new-tab --remote-cwd /tmp
    ants-terminal --remote new-tab --remote-cwd /tmp --remote-command 'pwd'

## Invariants (source-grep)

- **INV-1** `RemoteControl::dispatch` routes `"new-tab"` to
  `cmdNewTab`.
- **INV-2** `RemoteControl::cmdNewTab` reads `cwd` and `command`
  fields via `QJsonValue::toString()` (both optional; missing =
  empty string).
- **INV-3** `cmdNewTab` delegates to `MainWindow::newTabForRemote`
  and returns the `index` field from it — the response contract
  depends on the index surviving unchanged.
- **INV-4** `MainWindow::newTabForRemote(const QString &, const QString &) const`?
  The method is non-const (creates a tab) and returns `int`. Pinned so
  a refactor that tries to return `void` or make the method a slot
  without a return value breaks here instead of at the dispatch site.
- **INV-5** `newTabForRemote` uses `QTimer::singleShot(200, ...)`
  when a command is present — matches the SSH-manager timing for
  "wait for the shell to finish initial setup before writing." A
  smaller delay races the shell's init; skipping the delay entirely
  drops the first keystrokes on some shells (tested 2026-04-16 in
  the onSshConnect path).
- **INV-6** `newTabForRemote` uses `QPointer<TerminalWidget>` to
  guard the deferred write — the tab can be closed between
  `singleShot` enqueue and fire (e.g. Ctrl+Shift+W), and without
  the guard the lambda UB's on a freed widget.
- **INV-7** Client CLI: `main.cpp` adds `--remote-cwd` and
  `--remote-command` options and forwards them as `cwd` / `command`
  fields in the JSON envelope when the command is `new-tab`.
