# Feature: main-screen scrollback preservation under TUI repaint

## Contract

When a program running on the **main screen** emits any erase-display
sequence that produces the same post-state as `CSI 2J` (every visible
cell cleared) followed by a re-paint of content that was previously
visible, the terminal's scrollback must **not grow by more than
`rows + small_slack` lines** across the repaint. The repaint overwrites
the previous view; it does not *accumulate* alongside it.

The equivalent-effect shapes that the contract covers:

- `CSI 2J` (mode 2 erase display) — canonical full-screen clear. Cursor
  position irrelevant.
- `CSI H; CSI 0J` — move-home + erase-to-end. When cursor is at `(0,0)`,
  mode 0 clears every row.
- Any `CSI N;M H; CSI 1J` where `(N,M)` is the bottom-right corner —
  mode 1 clears everything above the cursor plus the cursor row up to
  the cursor, which at the corner is every cell. Rare in practice but
  included for correctness.

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
clears the window flag. The coverage was broadened post-0.6.23 to also
arm the window on the two equivalent-effect shapes listed in §Contract
(mode-0-from-home and mode-1-from-corner), because the underlying bug
isn't specific to the canonical sequence — it's specific to the
post-state (blank screen) plus subsequent overflow during redraw.

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
- **0.6.25:** viewport-stable invariant added (see §Viewport-stable
  below). The 0.6.21 pause and the 0.6.22/0.6.24 suppression window
  both skipped scrollback pushes — which froze
  `TerminalGrid::scrollbackPushed()`, which in turn kept
  `TerminalWidget`'s scroll anchor from advancing `m_scrollOffset`.
  The screen still scrolled on every `LF`, so the rows of a scrolled-
  up user's viewport that overlapped the screen got overwritten in
  place (user report: "line 251 becomes line 250"). Fix: when the
  user is scrolled up, ALWAYS push to scrollback; the widget anchor
  advances `m_scrollOffset` by the push count, keeping `viewStart`
  pinned. Doubling protection remains for the user-at-bottom path —
  the only path where the doubling symptom is observable.

---

## §Viewport-stable — companion contract

### Contract

Given a user who has scrolled up in history (`m_scrollOffset > 0`)
and is reading a specific logical line of scrollback, streaming new
output from the running program must not change the *content* at the
user's viewport rows.

Formally: let `snapshot(offset)` return the viewport's 2D grid of
codepoints at a given `m_scrollOffset`. For any `offset0 > 0` and any
amount of subsequent output `data`:

    snapshot_before = snapshot(offset0)
    feed(data)                                   # pushes may or may not happen
    offset1 = offset0 + pushes_during_feed       # anchor adjustment
    snapshot_after = snapshot(offset1)
    ⟹ snapshot_before == snapshot_after   (content invariant)

### Scope

- **Must hold** when the viewport is entirely in scrollback
  (`offset ≥ rows`). This is the user's real-world scenario — scroll
  up to read something that has passed off the screen.
- **Partial guarantee** when the viewport overlaps the screen
  (`offset < rows`). Rows that fall within scrollback are stable;
  rows that fall within the screen mutate as the program writes,
  which is inherent terminal semantics the library cannot override —
  no terminal can promise "program writes to the screen are invisible
  to the user while they're scrolled up slightly."

`test_viewport_stable.cpp` asserts the must-hold case across the
pause-only path (pure scrolled-up, no `CSI 2J`) and the CSI-clear
path (pause + suppression window both active).

### Why this complements §Contract above

The §Contract invariant ("no doubling") is about the **at-bottom**
user seeing scrollback grow with duplicate content. This invariant is
about the **scrolled-up** user seeing their reading position shift.
They're separate failure modes with separate test coverage; the
0.6.25 fix has to preserve both simultaneously.
