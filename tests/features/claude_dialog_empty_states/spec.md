# Feature: Claude dialogs explain an empty view

## Invariants

**INV-1 — an unreadable allowlist says so.** `ClaudeAllowlistDialog`
given a settings file that is not a JSON object shows a message naming the
file in its validation label, and a later `setSettingsPath` does not clear
that message.

**INV-2 — a tool-result-only user entry is not an empty User line.**
`ClaudeTranscriptDialog::formatEntry` renders a user entry whose content array
holds no text block as `[tool result]`, not as `User:` with nothing after it.

## Rationale

The ANTS-5092 performance pass found both: a corrupt `settings.json` showed
empty Allow, Deny and Ask lists with no reason, and the transcript view
printed an empty User line for every tool result Claude received.

## Test surface

`test_claude_dialog_empty_states.cpp`: INV-1 builds the dialog over a corrupt
file in a temporary directory; INV-2 reads `src/claudetranscript.cpp`
(located from the test's own path), since `formatEntry` is private.

## Regression history

- **ANTS-5092:** the two defects above. Locked by this spec.
