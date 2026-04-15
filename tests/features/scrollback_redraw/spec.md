# Feature: main-screen scrollback preservation under TUI repaint

## Contract

When a program running on the **main screen** emits `CSI 2J` (erase-
display mode 2) followed by a re-paint of content that was previously
visible, the terminal's scrollback must **not grow by more than
`rows + small_slack` lines** across the repaint. The repaint overwrites
the previous view; it does not *accumulate* alongside it.

## Rationale

Several popular TUIs paint on the main screen rather than the alt-screen
buffer, and refresh their view by clearing-and-reprinting rather than
via differential cursor-positioned updates:

- **Claude Code v2.1+** — repaints the entire transcript view on every
  internal state update (tool completion, stream chunk, compaction).
  Frequent enough that RAM can visibly double within minutes.
- **gum**, **charm.sh** TUI widgets — same pattern.
- **`fzf --height=full --no-tmux`** — full-screen mode on main screen.

Without a suppression mechanism, each repaint of N lines pushes N
lines into scrollback (the "top of screen" is scrolled off with every
newline during the repaint burst). After K repaints the user's
scrollback contains K duplicates of the same content, user scrollback
reading is broken, and memory usage grows unboundedly.

## Invariant

Given a main-screen terminal of R rows and C cols, if the program:

1. Prints L1 lines of content (L1 > R, so the screen fills and some
   lines push to scrollback).
2. Emits `CSI 2J` `CSI H` (erase display + cursor home).
3. Prints L2 lines of content (regardless of whether the content is
   identical to phase 1, similar, or entirely new).

Then:

    scrollback_size_after_phase_3 - scrollback_size_after_phase_1 ≤ R + 10

The `R + 10` slack accounts for partial boundary lines and any
legitimate overflow at the moment the window closes. The invariant's
substance is: **the scrollback must not grow by a full `L2` lines'
worth.**

## Scope

### In scope
- Main-screen programs doing clear-and-reprint.
- Both identical-content repaints (stable TUI frame) and diverged-
  content repaints (TUI with new data).

### Out of scope
- Alt-screen apps (vim, htop, less). Alt-screen already bypasses
  scrollback entirely; no suppression needed.
- `CSI 3J` (erase display mode 3). Mode 3 is an explicit
  `clear-scrollback` request — the user *asked* for scrollback to be
  wiped. Suppression would be wrong.
- `ESC c` (RIS full reset). Treated as intentional full reset; no
  suppression.
- Programs that use differential cursor-positioned updates without
  `CSI 2J`. The terminal has no way to know "this line overwrites
  that line," and the user scrolled-up pause (0.6.21, `m_scrollOffset
  > 0`) is the appropriate defense there.

## Implementation-detail reference (informative)

The 0.6.22 implementation opens a 250 ms sliding window on main-screen
`eraseInDisplay(2)`. Each `scrollUp()` during the window extends it.
The window closes when no `scrollUp()` fires for ≥ 250 ms, after which
normal scrollback behavior resumes. Alt-screen entry defensively
clears the window flag.

This file is the contract; `test_redraw.cpp` asserts the invariant;
the implementation may change as long as the invariant holds.

## Regression history

- **0.6.20 and earlier:** no mitigation. Scrollback grew by `L2` per
  repaint, doubling / tripling / etc. RAM with each Claude Code tool
  turn.
- **0.6.21:** partial mitigation — suppression only while
  `m_scrollOffset > 0` (user actively scrolled up reading history).
  This fixed the "duplicates in the user's reading position" symptom
  but left scrollback growth unbounded when the user was at the bottom
  — which is the common case. **This test would have caught the gap
  at commit time** because it runs at the bottom.
- **0.6.22:** full mitigation. Sliding window triggered by main-screen
  `CSI 2J` composes with the 0.6.21 offset pause; either trigger
  suppresses. This test passes.
