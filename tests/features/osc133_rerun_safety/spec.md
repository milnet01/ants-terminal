# Feature spec: re-running a command is safe (ANTS-5029)

Re-run Last Command types the newest finished OSC 133 command block into the
shell and presses Enter. Two defects made that unsafe.

1. Program output can print its own OSC 133 markers. The HMAC verifier is off
   unless `$ANTS_OSC133_KEY` is set, so a crafted file could plant a forged
   block, and re-run would type its text unconfirmed.
2. Prompt regions store global line numbers (scrollback index plus screen
   row). Once the scrollback is full, each new line evicts the oldest, and the
   stored numbers were never shifted. A re-run could then read the wrong line.

The user decided on 2026-09-11: re-run asks first, showing the exact
command, only when the markers are unsigned.

## Invariants

- **INV-1 — regions follow eviction.** After the scrollback reaches its cap
  and evicts lines, a prompt region's `endLine` still names the line holding
  its command text.
- **INV-2 — an evicted region is dropped.** A region whose prompt line
  (`startLine`) has been evicted is removed from `promptRegions()`.
- **INV-3 — an unsigned re-run confirms.** `TerminalWidget::rerunCommandAt`
  writes the command straight to the PTY only when
  `osc133HmacEnforced()` is true. Otherwise it shows the command in a
  confirmation dialog and writes it only when the user accepts.
- **INV-4 — a region keeps its id (ANTS-5078).** Every region gets a unique
  non-zero `id`. `TerminalGrid::promptRegionIndexById` finds a region by that
  id after the cap or eviction drops regions in front of it, and returns -1
  once the region itself is dropped.
- **INV-5 — the context menu names a block by id (ANTS-5078).**
  `menu.exec()` and the Share dialog run an event loop, and output arriving
  there can shift or drop regions. So the block actions in
  `TerminalWidget::contextMenuEvent` capture the region's `id` and look it up
  when they run, never an index taken before the menu opened.

## Test scope

INV-1, INV-2 and INV-4 drive `TerminalGrid` through `VtParser`, headless.
INV-3 is a source scrape of `rerunCommandAt`, and INV-5 of
`contextMenuEvent`, because both need a live `TerminalWidget` and a modal
dialog.
