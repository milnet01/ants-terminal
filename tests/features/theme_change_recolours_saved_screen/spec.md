# Feature: a theme change recolours the saved screen, scrollback and saved SGR state

## Problem

`TerminalGrid::setDefaultFg` / `setDefaultBg` swap the old default colour
for the new one. They did so only in the visible screen cells and the
current SGR state. Four other places keep copies of cells or SGR state:

- the main screen saved while the alternate screen is active, which
  `ESC [ ? 1049 l` moves back;
- the scrollback lines;
- the SGR state saved at 1049 entry and restored at exit;
- the SGR state saved by DECSC and restored by DECRC.

Claude Code runs in the alternate screen. Change theme while it runs,
exit it, and the restored main screen, the restored pen and the
scrollback still carry the old theme's colours. The text area stays the
wrong colour until `clear`.

## Invariants

- **INV-1** After `setDefaultFg` / `setDefaultBg` while the alternate
  screen is active, the main screen restored by `ESC [ ? 1049 l` holds the
  new defaults in every cell that held the old defaults.
- **INV-2** After `setDefaultFg` / `setDefaultBg`, scrollback cells that
  held the old defaults hold the new defaults.
- **INV-3** After `setDefaultFg` / `setDefaultBg` while the alternate
  screen is active, the SGR state restored by `ESC [ ? 1049 l` draws new
  text in the new defaults.
- **INV-4** After `setDefaultFg` / `setDefaultBg` between DECSC (`ESC 7`)
  and DECRC (`ESC 8`), the restored SGR state draws new text in the new
  defaults.
- **INV-5** Cells holding an explicit non-default colour, in the saved
  main screen and in scrollback, keep that colour across the change.

## Scope

In scope: grid level, driven by escape sequences through `VtParser`.
Out of scope: widget repaint, theme loading, the Kitty and Sixel images.

## Rationale

User report: after a theme change, leaving Claude Code leaves the working
area in the wrong colour until `clear`.

## Regression history

Present since the alternate-screen buffer, DECSC and scrollback were added
without being added to the default-colour swap. Fixed in the commit that
adds this contract.
