# Feature: RIS returns to the configured initial state, not the compiled-in one

## Problem

`TerminalGrid::handleEsc` implements RIS — Reset to Initial State,
`ESC c` — by reconstructing the object: `*this = TerminalGrid(m_rows,
m_cols)`. It then restores the integration callbacks and the OSC 133
key by hand, because a `tput reset` from the running shell must not
silently kill desktop notifications or the forgery alarm.

Everything else returns to the values written in the class definition.
Two of those are not defaults at all — they are configuration the
application pushes onto the grid after construction:

- `setDefaultFg` / `setDefaultBg` carry the active theme's foreground
  and background. `TerminalWidget::applyThemeColors` sets them.
- `setMaxScrollback` carries the user's configured scrollback depth.
  `MainWindow` sets it per terminal, and again whenever the setting
  changes.

So `reset(1)` in a shell reverted the terminal to the colours compiled
into the header, whatever theme the user had chosen, and shrank the
scrollback cap to the compiled-in figure, however the user had set it.
The mismatch is visible immediately: the cursor and selection colours
live on `TerminalWidget`, are not part of the grid, and survive — so
the terminal is left showing one theme's text on another theme's
furniture.

RIS is meant to return the terminal to the state it started in. xterm
re-reads its resources when it does so; it does not fall back to values
compiled into the binary. The configured theme and scrollback depth are
this terminal's equivalent of those resources.

## Contract

RIS MUST clear terminal *content and state* — screen, cursor,
attributes, modes, scrollback contents, tab stops — and MUST preserve
*configuration* pushed onto the grid from outside it:

1. The default foreground colour.
2. The default background colour.
3. The scrollback capacity.

Preservation goes through the public setters rather than by assigning
the members back. Those setters also re-point cells that carry the
constructor's default colour, which a bare member assignment would
leave holding the compiled-in value.

The existing callback and OSC 133 key preservation is unchanged; this
adds to that list rather than replacing it.

## Invariants

**INV-1 — the configured scrollback capacity survives RIS.**
`setMaxScrollback(n)`, then RIS, leaves `maxScrollback() == n`.

**INV-2 — the theme foreground survives RIS.**
`setDefaultFg(c)`, then RIS, leaves `defaultFg() == c`.

**INV-3 — the theme background survives RIS.**
`setDefaultBg(c)`, then RIS, leaves `defaultBg() == c`.

**INV-4 — RIS still clears scrollback contents.** Preserving the
*capacity* must not preserve the *lines*. After content has been pushed
into scrollback, RIS leaves `scrollbackSize() == 0`. This invariant
exists to catch a fix that preserves too much.

**INV-5 — RIS still resets the cursor.** Same reason as INV-4, on the
other half of what RIS is for: after moving the cursor away from the
origin, RIS returns it there.

## Scope

### In scope
- Runtime test driving `processAction` with an `EscDispatch` action
  whose final character is `c`.

### Out of scope
- The callbacks and the OSC 133 key. Already preserved, already
  documented in the source, and not what this feature changes.
- DECSTR (soft reset, `CSI ! p`), which is a different sequence with a
  narrower remit and does not reconstruct the object.
- Cursor and selection colours. They live on `TerminalWidget`, are not
  reachable by any escape sequence, and already survive.
- Whether RIS should also clear the window title. Unchanged here.

## Regression history

- **2026-04-23 Tier 2 re-review:** established that integration
  callbacks must survive RIS, and added the save/restore block. The
  configuration members were not considered.
- **ANTS-4456 (cold sweep 2026-08-18, verified 2026-09-08):** reported
  as "RIS discards theme colours + configured scrollback". Verified
  against source and fixed. Locked by this spec.
