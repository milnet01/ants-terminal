# Feature test — secrets hidden from the terminal-reading MCP verbs (ANTS-5169)

Four MCP verbs hand terminal text to Claude: `get_scrollback`, `get_text`,
`recent_errors` and `last_selection`. A key or password shown in the
terminal used to reach Claude's context as is. These verbs now pass the
text through `SecretRedact::scrub` (the scrubber the AI request path
already uses; its patterns are `tests/features/ai_context_redaction/spec.md`'s
contract, not this one's) before it leaves the process.

Ants also warns in the status bar when Claude reads a file whose name
marks it as a secrets file.

## Invariants

- **INV-1** — `RemoteControl::redactForClaude(text, enabled)` returns the
  text with every secret `SecretRedact::scrub` matches replaced by its
  `[REDACTED:<kind>]` marker, and the number replaced. With `enabled`
  false it returns the text unchanged and zero.
- **INV-2** — The switch is the config key `claude.mcp_redact_secrets`,
  default **on**. Each verb reads it per call through a fresh `Config()`,
  so editing `config.json` takes effect on the next call with no
  relaunch.
- **INV-3** — Each of the four verbs calls `redactForClaude` with that
  switch. `get_text` and `get_scrollback` redact **before**
  `trimScrollbackForGetText`, so the byte cap still bounds what is sent
  and a secret is never cut in half by the trim.
- **INV-4** — A reply that hid anything says so: `get_text`,
  `last_selection`, `recent_errors` and `get_scrollback`'s `since_cursor`
  envelope carry `redacted: <n>` when n > 0. `get_scrollback`'s plain-text
  reply starts with a `<redacted N secrets>` line instead.
- **INV-5** — `ClaudeIntegration::isSecretFileName(path)` is true when the
  file name starts with `.env`, ends with `.pem`, or starts with `id_`;
  false otherwise. It looks at the name only, never the directory.
- **INV-6** — On every `Read` of such a file, `updateChangedFiles` emits
  `sensitiveFileRead(path)`, ahead of the once-per-path de-duplication
  that `fileChanged` uses, so a second read warns again.
  `claudestatuswidgets.cpp` turns it into a status-bar message naming the
  file.

## What this does not cover

- A secret shape `SecretRedact::scrub` does not know is not hidden.
- A secrets file read through `Bash` (`cat .env`) raises no warning: the
  hook cannot reliably pull a path out of a shell command. Its output is
  still redacted if Claude later reads it back through one of the four
  verbs.
