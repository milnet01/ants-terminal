# Background Color Erase (BCE) — scroll + erase paths

## Contract

When a terminal operation exposes or clears cells while the current SGR
bg is non-default, the newly-visible cells must adopt that bg, not the
terminal's default bg. This is the xterm BCE convention that vim, less,
tmux, mc, and htop rely on to paint full-screen colored backgrounds.

Concretely, after `CSI 44m` (blue bg) plus any of the operations below,
the cells that were *exposed* (not the cells that were *preserved*)
must have `bg == blue`:

1. `CSI Ps J` (ED) — erase in display, modes 0 / 1 / 2.
2. `CSI Ps K` (EL) — erase in line, modes 0 / 1 / 2.
3. `CSI Ps L` (IL) — insert lines. The N new blank lines inserted at
   the cursor inherit current bg; the rest shift down.
4. `CSI Ps M` (DL) — delete lines. The N new blank lines appended at
   `m_scrollBottom` inherit current bg.
5. `CSI Ps S` (SU) — scroll up. New blank row at scroll-bottom.
6. `CSI Ps T` (SD) — scroll down. New blank row at scroll-top.
7. `CSI Ps P` (DCH) — delete chars. Newly-exposed right-edge cells
   inherit current bg.
8. `CSI Ps @` (ICH) — insert blanks. The inserted cells inherit
   current bg.
9. LF past scroll-bottom — implicit scroll-up via newline. New bottom
   row inherits current bg.

Operations that do NOT apply BCE:
- Resize (geometry, not erase) — new rows keep default bg.
- Alt-screen enter — xterm clears with default attrs.
- RIS (ESC c) — full reset drops SGR before erasing.

## Invariants tested

- **IL/DL**: `CSI 44m CSI 3L` → 3 inserted rows all have bg=blue.
- **SU/SD**: `CSI 44m CSI 1S` → bottom row has bg=blue.
- **DCH**: at col 0 in a row prefilled with 'x', `CSI 44m CSI 5P` →
  rightmost 5 cells have bg=blue.
- **ICH**: `CSI 44m CSI 3@` → 3 inserted cells have bg=blue.
- **ED 2**: `CSI 44m CSI 2J` → every cell has bg=blue.
- **LF scroll**: put cursor at bottom row, `CSI 44m` + LF → new
  bottom row has bg=blue.
- **SGR reset**: `CSI 44m CSI 0m CSI 2J` → every cell has bg=default
  (BCE honors the *current* SGR, which is back to default).

## Why this test

The BCE path previously routed through `takeBlankedCellsRow()` and
`deleteChars` / `insertBlanks`, all of which used either `m_defaultBg`
or `m_currentAttrs.bg` without an `.isValid()` fallback. Apps with
non-default backgrounds (vim `:colorscheme`, less on a painted prompt,
tmux status bar) saw visible gaps on scroll.

Fix consolidates the policy in a single `eraseBg()` helper so every
erase/scroll callsite reads from one place.
