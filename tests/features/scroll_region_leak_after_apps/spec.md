# Scroll-region leak after apps exit (ANTS-1187)

## Why this test exists

User report 2026-05-07: a long-running server tab renders new output only
into the upper half of the terminal; the bottom half stays blank.
Symptom signature is a stale `DECSTBM` (CSI top;bottom r) scroll region
left behind by an earlier process — claude-code's status footer, an
ncurses progress bar, or an alt-screen TUI that exited without
restoring main-screen state.

This test pins the contract for the four standard ways DECSTBM is
expected to be reset, plus the alt-screen save/restore semantics that
should NOT leak the alt's scroll region back onto main. If any of
these regress, server-style tabs will silently lose their bottom rows.

## Invariants

### INV-1 — DECSTBM persists until explicitly reset

`CSI 5;15 r` on a 20-row grid sets the scroll region to rows 5..15.
After that, plain printing 30 lines must scroll **inside** [5, 15] —
text never appears at row 16 or below, never at row 4 or above. This
is the BY-DESIGN xterm contract; the test exists to make sure we
don't accidentally make the region transient.

### INV-2 — `CSI r` (no params) resets DECSTBM to full screen

After INV-1's bug-state (region = [5, 15]), sending plain `\x1b[r`
must restore the region to `[0, m_rows - 1]`. A subsequent line of
plain text printed after enough newlines must reach the bottom row.

### INV-3 — RIS (`ESC c`) resets DECSTBM to full screen

Same as INV-2 but the trigger is `\x1bc` (RIS — Reset to Initial
State). Already covered indirectly by RIS's full grid reset; this
invariant nails it down so the next refactor of the RIS path can't
silently lose it.

### INV-4 — Alt-screen DECSTBM does NOT leak back to main on exit

Starting from main scroll region = full screen (`[0, m_rows - 1]`):
1. Enter alt-screen (`CSI ?1049h`).
2. Set DECSTBM in alt-screen to a smaller region (`CSI 1;5 r`).
3. Exit alt-screen (`CSI ?1049l`).
4. Inspect main's `m_scrollTop` / `m_scrollBottom`.

The main-screen scroll region must equal what it was BEFORE alt-entry
(full screen here), NOT the alt-screen's `[0, 4]`. xterm reference
behaviour: alt-screen has its own DECSTBM that doesn't follow you out.

### INV-5 — Pre-alt-entry main DECSTBM survives an alt session

Starting from main scroll region = `[3, 12]` (set BEFORE alt-entry):
1. Enter alt-screen.
2. Set DECSTBM in alt-screen to `[0, 4]`.
3. Exit alt-screen.

Main's scroll region must be `[3, 12]` again — the user's pre-entry
state, not full-screen and not the alt's `[0, 4]`. Distinct from
INV-4: that one starts from full-screen, this one starts from a
non-default region. Catches a regression that resets to full instead
of restoring saved.

### INV-6 — Print after `CSI r` reaches the bottom row of the grid

Concrete user-facing assertion of INV-2 / INV-3: after the reset
sequence, sending exactly `kRows` lines of plain text places visible
content on the bottom row (`m_rows - 1`). This pins the symptom that
the user reports — text NOT reaching the bottom — at the level of
"new output landed where the user expects it."

## Test harness

Standalone C++ test linking only `terminalgrid.cpp` + `vtparser.cpp`
(GUI-free), modelled on `tests/features/scroll_region_rotate/`. The
test feeds escape sequences via `VtParser` → `TerminalGrid` and
inspects `m_scrollTop`, `m_scrollBottom`, and `cellAt(row, col)`.
Exit 0 = invariants hold, non-zero = regression.

Note: `scrollTop()` / `scrollBottom()` accessors do not exist today;
the test reads them indirectly via `cellAt` content inspection plus
a behavioural probe (print N lines, observe where text lands).
