# Feature: AI `Insert Cmd` sanitization + confirmation

## Problem

The 0.7.12 `/indie-review` sweep flagged `AiDialog::insertCommand` as
OWASP LLM01+LLM02: the AI assistant writes its response verbatim to
the PTY when the user clicks "Insert Cmd". A prompt-injected response
— via poisoned terminal context the user pastes in, or a compromised
endpoint — can inject ESC sequences, NUL bytes, or multi-kilobyte
payloads into the user's shell with a single click.

## External anchors

- [OWASP LLM Top 10](https://genai.owasp.org/llmrisk/) — LLM01 Prompt
  Injection, LLM02 Insecure Output Handling.
- ECMA-48 C0 control classification (same anchor as remote_control_opt_in).
- Principle of least authority: LLM output is untrusted; user intent
  is the trust boundary.

## Contract

### Invariant 1 — extraction prefers fenced code blocks

When the last AI response contains a ```-fenced block, the command is
the contents of the LAST fenced block (trimming a language
identifier on the first line if < 10 chars, e.g. "bash\n"). Falls
back to the last non-empty line.

### Invariant 2 — dangerous controls are stripped

`{0x00..0x08, 0x0B..0x1F, 0x7F}` bytes are removed. HT / LF / CR
are preserved (a multi-line paste is legitimate). Multi-byte UTF-8
characters (U+0080+) pass through unchanged.

### Invariant 3 — length capped at 4 KiB

`kInsertCommandMaxBytes = 4096`. Excess bytes are truncated. The
stripped-byte counter includes both the truncation count and the
control-char count.

### Invariant 4 — `out_stripped` reports total filtered bytes

Zero when the input was already clean. Non-zero otherwise. The
dialog surfaces this count in the confirmation dialog's footer.

### Invariant 5 — user confirmation REQUIRED

The `insertCommand` signal MUST NOT fire until the user clicks Yes
in a confirmation dialog that displays the sanitized bytes verbatim.

This test exercises the pure extractAndSanitizeCommand helper — the
UI confirmation is a source-grep assertion that the `emit
insertCommand` call is preceded by a `QMessageBox::question` in the
same click handler.

## Regression history

- **Introduced:** when the AI Insert Cmd feature shipped. The design
  assumed the user reads and approves the AI's response text;
  cognitively, clicking Insert Cmd feels like approval, but the actual
  bytes can be different from what the user saw (hidden chars, ESC
  sequences invisible in the chat widget).
- **Discovered:** 2026-04-23 via `/indie-review`. AI subsystem reviewer
  flagged OWASP LLM01+LLM02.
- **Fixed:** 0.7.12 — `extractAndSanitizeCommand` helper + confirmation
  dialog. Context-redaction (LLM06) and rate-limit visibility (LLM10)
  deferred.
