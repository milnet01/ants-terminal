# Feature: ANTS-1920 — output-driven model-switch confirm

The model-switch actuator confirms Claude Code's "Switch model?" dialog by
*watching the terminal output*, not by a blind timer. Replaces the original
ANTS-1924 sequence (`ESC@250ms` + `\r@400ms` + continuation`@1500ms`) whose
blind `\r@400ms` fired whether or not the dialog had rendered — when CC was slow
to draw it, the CR landed in the wrong place and left `/model <tier>` stranded
in the composer (user report 2026-05-29: model stayed on Sonnet).

## Detection signature

Verified against a live CC session (2026-06-01); the exact text is not in CC's
docs nor previously captured in this repo:

```
Switch model?
Your next response will be slower and use more tokens

This conversation is cached for the current model. Switching to Sonnet 4.6
means the full history gets re-read on your next message.

❯ 1. Yes, switch to Sonnet 4.6
  2. No, go back
```

The pre-highlighted `❯ 1.` row means a plain ENTER (`\r`) confirms — matching
the handshake's existing terminator. There is **no** "Esc to cancel" footer.

## Invariants

- **INV-1 — title+option detection.** `ModelAutoSwitch::switchConfirmVisible`
  returns true for output containing the title `"Switch model?"` AND at least
  one option line (`"Yes, switch to"` or `"No, go back"`). Verified against both
  the Sonnet and a generic variant of the real dialog.
- **INV-2 — no false positive on prose.** Returns false for normal terminal
  output (code, a shell prompt, unrelated text) and for empty input.
- **INV-3 — title alone is not enough.** Output that mentions `"Switch model?"`
  in prose but has no option line returns false (so a transcript quoting the
  phrase cannot trip a confirm).
- **INV-4 — case-insensitive.** Detection matches regardless of case.
- **INV-5 — actuator polls, never blind-confirms.** In
  `ClaudeStatusBarController`, the confirm `\r` is sent only from inside the
  `switchConfirmVisible(...)`-guarded branch of `pollModelSwitchConfirm`; the
  budget-exhaustion branch sends `ESC` (`\x1b`), never a bare `\r`. The
  handshake no longer contains the blind `QTimer::singleShot(400, ...)` →
  `sendToPty("\r")` pattern.
- **INV-6 — bounded poll.** The poll re-arms at most `kSwitchConfirmMaxPolls`
  times so a dialog that never renders cannot loop forever.

## Tests

- `test_switch_confirm_visible.cpp` (pure, on the core bundle) — INV-1..INV-4.
- `test_switch_confirm_actuator.cpp` (source-grep on
  `src/claudestatuswidgets.cpp` + `src/modelautoswitch.h`) — INV-5, INV-6.

To confirm the source-grep test would catch a real regression: reverting
`performModelSwitchHandshake` to the blind `\r@400` sequence reintroduces the
`singleShot(400` + unguarded `sendToPty("\r")` pattern INV-5 forbids, and drops
the `switchConfirmVisible` call the test requires — so the grep flips red.
