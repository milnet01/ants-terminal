# Feature: Default scroll region grows / shrinks with the grid on resize

## Problem

User report 2026-05-08 (ANTS-1187 → ANTS-1193 umbrella → ANTS-1194
root cause): a long-running web-server tab rendered new output only
into the **upper portion** of the terminal window. Bottom rows stayed
empty until the user manually reset the scroll region, scrolled, or
restarted the tab.

Discriminator tests yesterday ruled out:

- Prior tab state (fresh tab repros — Flask/MAME-Curator both).
- Flask emitting DECSTBM (`script -q` byte capture showed zero
  scroll-region escapes in the stream).
- Alt-screen leak (no `\e[?1049` in the stream).
- Manual `Tools → Reset Scroll Region` — had no observable effect on
  fresh content (it correctly reset state, but new output kept piling
  in the upper rows).

Today's bisection (after seeing the "stops short of bottom" detail
clearly in a new screenshot) pinned it to `TerminalGrid::resize()`
in `src/terminalgrid.cpp`. The ANTS-1130 fix preserves DECSTBM
state across resize via `std::clamp` — correct for shrink, **wrong
for grow when no DECSTBM was set**.

## Reproducer

```
1. Open a tab at the default 24×80.
   → m_scrollTop = 0, m_scrollBottom = 23.
2. Maximize the window so the grid grows to (e.g.) 60 rows.
   → resize(60, 80).
3. Pre-fix: m_scrollBottom = clamp(23, 0, 59) = 23.
4. Run any program that streams output (Flask, MAME Curator,
   `yes`, `find /`).
5. Output piles into rows 0–23. Rows 24–59 stay empty.
   Cursor parks at row 23 instead of row 59.
```

## Why ANTS-1130's clamp was wrong for grow

ANTS-1130 (commit history; pre-1194) replaced an unconditional
`m_scrollBottom = m_rows - 1; m_scrollTop = 0;` with a clamp:

```cpp
m_scrollTop = std::clamp(m_scrollTop, 0, m_rows - 1);
m_scrollBottom = std::clamp(m_scrollBottom, m_scrollTop, m_rows - 1);
```

The motivation was correct: TUI apps with explicit DECSTBM (tmux
splits, less with status line, mc) must not lose their scroll region
on every window resize. The clamp preserves explicit DECSTBM.

But `std::clamp` can only *narrow* a range — never widen it. So when
the user grows the window without an explicit DECSTBM having ever
been issued, the implicit "full-screen scroll region" (`top=0,
bottom=oldRows-1`) stays frozen at `oldRows-1` instead of tracking
the new `m_rows-1`.

xterm's behaviour is the right reference: when the scroll region is
at "full screen" (default state), it tracks the screen on resize.
When the user has explicitly carved out a partial region, that
region is preserved as much as possible.

## Fix

Capture a "was full-screen" snapshot at the top of `resize()` —
before any mutation. In the post-mutation block, branch:

```cpp
const bool primaryWasFullScreen =
    (m_scrollTop == 0 && m_scrollBottom == m_rows - 1);
const bool altWasFullScreen =
    (m_altScrollTop == 0 && m_altScrollBottom == m_rows - 1);

// ... resize/reflow body, m_rows = rows; m_cols = cols ...

if (primaryWasFullScreen) {
    m_scrollTop = 0;
    m_scrollBottom = m_rows - 1;
} else {
    m_scrollTop = std::clamp(m_scrollTop, 0, m_rows - 1);
    m_scrollBottom = std::clamp(m_scrollBottom, m_scrollTop, m_rows - 1);
}
// same shape for alt scroll-region
```

The snapshot must be captured BEFORE `m_rows` is updated (line ~2430)
because the test "was at full screen" compares against the OLD
`m_rows - 1`. After the update, that comparison would always fail.

Alt scroll-region gets the same treatment for symmetry. In practice
the alt members are dormant outside alt-mode (overwritten on alt
entry from the saved primary), but the symmetric fix avoids a class
of "I left alt mode at the wrong moment, and now alt's scroll region
is stale" bugs that would otherwise need their own analysis.

## Edge case — explicit DECSTBM matching full-screen

If a TUI sets DECSTBM `1;m_rows` (top=1, bottom=screen-height-in-1-
indexed terms), it's indistinguishable from "no DECSTBM ever set"
under our snapshot test. That's not a bug: xterm treats `\e[1;Hr`
the same way as "no scroll region" — both are "scroll region is the
whole screen." Resizing such a region to track the new screen is
the user's expected behaviour.

## Edge case — DECSTBM, then resize, then RIS

If a TUI sets DECSTBM `5;15`, the user resizes from 24 to 60 rows,
the snapshot captures `primaryWasFullScreen = false` (5 ≠ 0 or 15
≠ 23), and the clamp branch runs: `top=5, bottom=clamp(15, 5, 59)=15`.
Region preserved. Subsequent RIS (`ESC c`) resets to full screen
correctly via the existing `m_scrollTop = 0; m_scrollBottom = m_rows-1`
path. No regression.

## Contract

### Invariant 1 — implicit (full-screen) primary region grows on resize

Construct grid at 24 rows. No DECSTBM. Resize to 60 rows.
`scrollBottom() == 59` (was 23 pre-fix).

### Invariant 2 — implicit (full-screen) primary region shrinks on resize

Construct grid at 60 rows. No DECSTBM. Resize to 24 rows.
`scrollBottom() == 23`.

### Invariant 3 — explicit DECSTBM preserved on grow

Construct grid at 24 rows. DECSTBM `5;15` (top=4, bottom=14 in 0-
indexed). Resize to 60 rows. `scrollTop() == 4 && scrollBottom() == 14`
(NOT auto-grown).

### Invariant 4 — explicit DECSTBM clamped on shrink below bottom

Construct grid at 60 rows. DECSTBM `5;55` (top=4, bottom=54).
Resize to 30 rows. `scrollTop() == 4 && scrollBottom() == 29`
(clamped to new max). Top preserved, bottom clamped — matches
ANTS-1130 contract.

### Invariant 5 — alt scroll-region also grows on resize when full-screen

Enter alt screen (DECSET 1049). Default alt region is full-screen
(top=0, bottom=rows-1). Resize from 24 to 60. Inside alt-mode, the
"current" scroll region is `m_scrollTop/m_scrollBottom` — assert
those grow to 0 / 59.

### Invariant 6 — alt explicit DECSTBM preserved on grow

Enter alt screen. Set DECSTBM `5;15`. Resize from 24 to 60.
`scrollTop() == 4 && scrollBottom() == 14` (preserved).

### Invariant 7 — sequential grow then shrink stays consistent

Construct grid at 24 rows. No DECSTBM. Resize to 60.
`scrollBottom() == 59`. Resize to 30. `scrollBottom() == 29`.
Resize back to 24. `scrollBottom() == 23`. Default region tracks
the grid through arbitrary sequences.

## How this test anchors to reality

Direct assertions via `TerminalGrid::scrollTop()` / `scrollBottom()`
(public accessors added in ANTS-1187 to support the prior scroll-
region test). No GUI, no parser interaction needed beyond the
DECSTBM-set tests; uses `TerminalGrid::resize()` directly. Sub-
millisecond execution.

## Regression history

- **Pre-ANTS-1130:** unconditional `m_scrollBottom = m_rows - 1`
  on every resize. Destroyed TUI scroll regions on window resize.
- **ANTS-1130 (0.7.x):** replaced with `std::clamp`. Preserved
  explicit DECSTBM but silently broke window-grow when no DECSTBM
  was ever set (the implicit full-screen case).
- **ANTS-1194 (0.7.45, 2026-05-08):** snapshot "was full-screen"
  at function entry; conditionally widen on grow, clamp on shrink.
  Fixes ANTS-1187 (primary symptom: output piles in upper rows)
  and unblocks ANTS-1193 (umbrella was incorrectly looking at
  paint pipeline; root cause was scroll region not tracking
  resize).
