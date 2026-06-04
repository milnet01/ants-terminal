# scrollback_hyperlink_restore_sync — OSC 8 span / line lockstep (ANTS-1999)

## Problem

`TerminalGrid` stores OSC 8 hyperlink spans in `m_scrollbackHyperlinks`,
indexed in parallel with `m_scrollback` (span for line `idx` lives at
`m_scrollbackHyperlinks[idx]`). Two sites broke that lockstep:

- `pushScrollbackLine` (the session-restore path) pushed to `m_scrollback`
  but **never** pushed a matching `m_scrollbackHyperlinks` entry. After a
  restore the two deques had different lengths, so every later OSC 8 span
  mapped to the wrong row (or was lost).
- `setMaxScrollback` trimmed the two deques in **independent** loops, which
  cannot re-align them once they have diverged.

## Fix

- `pushScrollbackLine` pushes an empty span vector alongside each restored
  line (restore carries no serialized span data) and pops both fronts
  together when over the cap.
- `setMaxScrollback` pops both deques in one lockstep loop.

The normal scroll path already pushes both in parallel
(`m_screenHyperlinks` is always sized to `m_rows`, so its `hlInRange` guard
is always true), so 1:1 lockstep is the real invariant.

## Invariants

- **INV-1** — after a restore (N `pushScrollbackLine` calls) followed by a
  real OSC 8 line scrolled into scrollback, the span is retrievable at the
  *same* scrollback index whose cells hold the link text.
- **INV-2** — after `setMaxScrollback` evicts front lines, a surviving OSC 8
  line's span still maps to its (shifted) row.
