# Trigger dispatch fires per completed line (ANTS-2119 terminalwidget M1)

## Problem

Non-instant trigger rules with a *dispatch* action (`notify` / `sound` /
`command` / `bell` / `inject` / `run_script`) were evaluated in
`checkTriggers`, which runs once per raw PTY batch with a single
`pattern.match()`:

1. A non-instant rule only fired when the batch happened to end in `\n`/`\r`
   (`chunkEndsLine`). Under the threaded parse path a batch can end mid-line,
   so a rule the user expects "on every completed line matching X" silently
   missed lines whose newline landed in the previous batch.
2. `match()` (not `globalMatch`, and not per-line) meant a batch spanning three
   matching lines fired the action **once**, not once per line.

The header contract (`TriggerRule`, terminalwidget.h) already documents
non-instant rules as "completed-line evaluation … matches iTerm2's Instant
flag" — so the per-batch behaviour was a defect, not the intended contract. The
grid-mutation triggers (`highlight_*` / `make_hyperlink`) already ran correctly
per completed line via `onGridLineCompleted`.

## Fix

Route non-instant dispatch triggers through `onGridLineCompleted` too — the
grid's per-newline line-completion callback — firing once per completed line on
the finalized line text. `checkTriggers` now handles only **instant** rules
(which must fire mid-line, e.g. a password-prompt watcher that never ends in a
newline), so its `chunkEndsLine` gate is gone.

## Invariants

- **INV-1** — a non-instant dispatch rule fires once per completed line: two
  matching lines emit `triggerFired` twice (not once for the whole batch).
- **INV-2** — the emitted signal carries the rule's pattern / action_type /
  action_value; a `run_script` rule emits `triggerRunScript` with the matched
  text instead.
- **INV-3** — a non-matching line emits nothing.

## Test

Construct a `TerminalWidget`, install a non-instant `notify` trigger, feed
completed lines through `grid()->processAction` (which fires the widget's
line-completion callback), and count `triggerFired` via `QSignalSpy`.
