# Remote-control `get-text` — read scrollback + screen

## Contract

Sixth rc_protocol command. Returns trailing N lines of (scrollback +
screen) joined with `\n`. Used by scripts that need to capture
terminal output, dispatch on visible state, or grab the last command's
result.

Request shape:

    {"cmd":"get-text","tab":<int optional>,"lines":<int optional>}

- `tab`   — optional 0-based index. Omitted → active tab.
- `lines` — optional. Default 100. Negative or zero → falls back to
  default. Capped server-side at **10 000** so a script that
  inadvertently passes `--remote-lines 1000000` against a million-line
  scrollback doesn't return a 100 MB JSON envelope. Beyond 10 000
  the caller probably wants the file directly (Ctrl+Shift+P → Export
  Scrollback) rather than over the wire.

Response shape on success:

    {"ok":true,"text":"<string>","lines":<int>,"bytes":<int>}

`bytes` is `text.toUtf8().size()`; `lines` is the actual line count
in the returned text (counts trailing-newline-or-not consistently —
e.g. `"a\nb"` and `"a\nb\n"` both report 2 lines so a client doing
`if [ "$lines" = N ]` works either way).

Errors:

- `get-text: no tab at index <i>`
- `get-text: no active terminal`

Reuses the existing `TerminalWidget::recentOutput(int lines)` —
already used by the AI dialog for context capture, so the format
stays consistent across both consumers.

## Invariants (source-grep)

- **INV-1** `RemoteControl::dispatch` routes `"get-text"` to
  `cmdGetText`.
- **INV-2** `cmdGetText` uses `isDouble()` for the optional `tab`
  field (consistent with send-text / set-title / select-window).
- **INV-3** `cmdGetText` calls `TerminalWidget::recentOutput(...)`.
  No new text-extraction logic — the AI dialog and rc_protocol use
  the same accessor so format drift between them is impossible.
- **INV-4** `cmdGetText` caps `lines` at 10 000 to bound the
  response envelope.
- **INV-5** Response carries `text`, `lines`, and `bytes` fields —
  rc_protocol clients can check size / count without re-parsing the
  text.
- **INV-6** Client CLI: `--remote-lines <n>` parses to int, rejects
  non-numeric / negative values with an explicit error rather than
  silently sending the malformed value.
