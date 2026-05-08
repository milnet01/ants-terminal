# Feature: OSC 9 vs OSC 9;4 disambiguator

## Problem

OSC 9 has two competing meanings depending on payload prefix:

- **iTerm2 / Ghostty desktop notification:** `OSC 9 ; <body> ST` —
  short notification with no title.
- **ConEmu / Microsoft Terminal progress:** `OSC 9 ; 4 ; <state>
  [;<percent>] ST` — taskbar progress bar update.

Pre-fix code (`terminalgrid.cpp:1337-1339`) discriminated on:

```cpp
payload.size() >= semi + 3 &&
payload[semi + 1] == '4' && payload[semi + 2] == ';'
```

This **misclassifies the minimal legal progress payload `9;4`**
(ConEmu state-0 / "remove progress" — taskbar icon clears, no
percent given). With `payload="9;4"`, `semi=1`, length is 3,
`semi+3=4`; the `>=` check fails, branch skipped, falls through to
desktop notification at `:1365`, fires `m_notifyCallback` with body
`"4"`. A shell legitimately emitting `\e]9;4\a` to clear progress
instead pops a notification.

Found by `/indie-review` 2026-05-08, Lane B HIGH.

## External anchors

- ConEmu OSC 9: https://conemu.github.io/en/AnsiEscapeCodes.html#OSC_Operating_system_commands
  > `ESC ] 9 ; 4 ; st [; pr] ST` — `st=0` removes progress.
- Microsoft Terminal progress sequences:
  https://learn.microsoft.com/en-us/windows/terminal/tutorials/progress-bar-sequences
  > "State 0 — Hide. The progress bar is hidden."
- iTerm2 OSC 9: https://iterm2.com/documentation-escape-codes.html
  > `OSC 9 ; <body> ST` — single-arg notification.

## Fix

Discriminator must accept the minimal `9;4` form AND reject ambiguous
forms where the byte after `4` is not `;` and not end-of-payload:

```
9;4              → progress, state=0, percent=0 (clear)
9;4;0            → progress, state=0, percent=0
9;4;1;42         → progress, state=1 (default), percent=42
9;4;<n>          → progress, state=n, percent=0
9;42             → notification body "42" (NOT progress — `4` not followed by `;` or EOF)
9;4Hello         → notification body "4Hello" (same — `4` then non-`;`)
9;<anything-not-starting-4-then-(;|EOF)> → notification
```

The new predicate:

```cpp
payload.size() >= semi + 2 &&
payload[semi + 1] == '4' &&
(payload.size() == semi + 2 || payload[semi + 2] == ';')
```

## Contract

### Invariant 1 — `OSC 9 ; 4 ST` (no body) fires progress with state 0

Drive parser with `\e]9;4\e\\`. `m_progressState == ProgressState::Default`
(state 0). `m_progressValue == 0`. Notification callback NOT fired.

### Invariant 2 — `OSC 9 ; 4 ; 0 ST` matches state-0 explicit form

Drive `\e]9;4;0\e\\`. Same outcome as INV-1.

### Invariant 3 — `OSC 9 ; 4 ; 1 ; 42 ST` parses state and percent

Drive `\e]9;4;1;42\e\\`. State=1 (Default/Normal), percent=42.

### Invariant 4 — `OSC 9 ; 42 ST` does NOT fire progress; fires notification

Drive `\e]9;42\e\\`. Notification callback fires with body "42".
Progress callback NOT fired.

### Invariant 5 — `OSC 9 ; Hello ST` fires notification with body "Hello"

Drive `\e]9;Hello\e\\`. Notification body "Hello". Progress NOT
fired.

### Invariant 6 — `OSC 9 ; 4Hello ST` is a notification (4 not followed by `;`)

Drive `\e]9;4Hello\e\\`. Notification body "4Hello". Progress NOT
fired. (This is the disambiguator's "byte after 4 must be ; or EOF"
rule — protects against shells that legitimately want to send a
notification body starting with the digit 4.)

## Open question (for follow-up)

Notification body literally starting with `"4;"` (e.g.
`OSC 9;4;30 PM meeting`) would be misclassified as progress with
state=4 (Warning) and percent=30. This is fundamentally ambiguous in
the wire format — either the spec must constrain notification
bodies, or a stricter discriminator is needed (e.g. require state in
[0..4] AND optional percent in [0..100], else notification). NOT
addressed by this fix; logged in ROADMAP open-questions.

## Regression history

- **Pre-0.7.79:** `9;4` minimal-progress form misclassified as
  notification. ConEmu / Windows Terminal-style "clear progress"
  shell scripts pop a notification with body "4".
- **0.7.79 (ANTS-1197 from indie-review #3):** discriminator
  predicate widened to accept the minimal form.
