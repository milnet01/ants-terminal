# Origin Mode (DECOM) — CUP/HVP/VPA translation + DECSC save/restore

## Background

`DECOM` (CSI ?6h / ?6l) toggles "origin mode" on a VT100/xterm. When
it's set:

1. CUP (`CSI Pn;Pn H`), HVP (`CSI Pn;Pn f`), and VPA (`CSI Pn d`) row
   coordinates are **relative to the scroll-region top**, not absolute
   row 1. So with scroll region rows 5..15 and origin mode on, `CSI 1;1H`
   places the cursor at row 5 col 0, and `CSI 11;1H` is the bottom of
   the scroll region.
2. The cursor cannot leave the scroll region. `CSI 99;1H` clamps to
   `scrollBottom`, not `m_rows - 1`.
3. DECSTBM (set scroll region) homes the cursor to `(scrollTop, 0)`,
   not `(0, 0)`.

Per VT420 spec, `DECSC` (`ESC 7` / `CSI s`) also saves the origin-mode
flag and the auto-wrap (DECAWM) flag alongside cursor position + SGR;
`DECRC` (`ESC 8` / `CSI u`) restores all of them. Without that, a TUI
that flips DECOM, scrolls, and DECRCs ends up in the wrong coordinate
space — which is exactly the breakage tmux/screen save-restore
round-trips hit when nesting their own DECSC/DECRC inside Ants.

Pre-fix, our `case 'H'`/`'f'` and `'d'` ignored `m_originMode`
entirely, treated CUP rows as absolute, and let the cursor escape the
scroll region. `saveCursor()` saved row/col/attrs but not DECOM /
DECAWM, so DECRC clobbered both flags' current state in undefined ways.

## Invariants

Every numbered invariant below has one corresponding test case in
`test_origin_mode.cpp`. Each test feeds VT bytes through `VtParser` →
`TerminalGrid` and reads `cursorRow()` / `cursorCol()` afterward.
Pre-fix, every invariant fails; post-fix, every one passes.

### CUP/HVP origin-mode translation (`'H'`, `'f'`)

- **I1** With DECOM **off**, `CSI 5;3H` after setting scroll region
  rows 4..10 puts cursor at absolute (4, 2). (Sanity baseline; was
  already correct.)
- **I2** With DECOM **on** and scroll region 4..10, `CSI 1;1H` places
  cursor at (4, 0). Pre-fix: (0, 0).
- **I3** With DECOM **on** and scroll region 4..10, `CSI 3;5H` places
  cursor at (6, 4). Pre-fix: (2, 4).
- **I4** With DECOM **on** and scroll region 4..10, `CSI 99;1H` clamps
  to (10, 0) — the scroll-region bottom. Pre-fix: clamps to bottom of
  the entire screen.
- **I5** Same as I4 but for `CSI 99;1f` (HVP). Equivalent to CUP and
  must have the same translation.

### VPA origin-mode translation (`'d'`)

- **I6** With DECOM **on** and scroll region 4..10, `CSI 3 d` (VPA row
  3, column unchanged) places cursor at row 6.
- **I7** With DECOM **on**, `CSI 99 d` clamps row to scrollBottom (10).

### DECSTBM home respects DECOM

- **I8** With DECOM **off**, `CSI 4;10r` (set scroll region 4..10)
  homes cursor to (0, 0).
- **I9** With DECOM **on**, `CSI 4;10r` homes cursor to (4, 0) — the
  scroll-region top.

### DECSC saves / DECRC restores DECOM + DECAWM

- **I10** `CSI ?6h` (DECOM on) → `CSI s` (DECSC) → `CSI ?6l` (DECOM off)
  → `CSI u` (DECRC) ⇒ DECOM is back on. Verified by issuing `CSI 1;1H`
  and observing cursor lands at scrollTop (DECOM-relative), not (0,0).
- **I11** `ESC 7` / `ESC 8` form: same restore semantics as `CSI s`/`u`.
- **I12** `CSI ?7l` (DECAWM off) → `CSI s` → `CSI ?7h` → `CSI u` ⇒
  auto-wrap is back off. Verified by writing past the right margin
  with cursor at the last column and confirming the cursor did not
  advance to the next line.

## Source-grep check

A separate test in this directory greps `src/terminalgrid.cpp` for the
specific code that implements the fix, locking it in as the only valid
shape:

- `setCursorPos` calls in CUP/HVP path are wrapped in an
  `if (m_originMode)` translation block that adds `m_scrollTop` and
  clamps to `[m_scrollTop, m_scrollBottom]`.
- `saveCursor()` reads `m_originMode` and `m_autoWrap` into
  `m_savedOriginMode` and `m_savedAutoWrap`.
- `restoreCursor()` writes them back.
- `setScrollRegion` homes cursor to `m_originMode ? m_scrollTop : 0`.

## Why behavioural over source-grep alone

Behavioural tests catch a regression that types correctly but loses
the semantic (e.g. someone "simplifies" the clamp to use `m_rows`
again). Source-grep alone catches refactors that drop the if-block
entirely. We do both — source-grep for cheap structural lock, run-time
tests for actual VT behaviour. Pre-fix run shows every behavioural
invariant FAIL; post-fix shows pass.
